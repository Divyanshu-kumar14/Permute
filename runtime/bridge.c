// SPDX-License-Identifier: Apache-2.0
//
// Host bridge: implements the kernel operation table for guest code.
//
// Every entry point here is the host side of a shim call. It resolves the
// calling simulated thread from thread-local state, forwards to the kernel,
// and parks the host thread when the kernel returns BLOCKED. There is no
// policy in this file: priorities, ordering, and timeouts all belong to the
// kernel.

#define _GNU_SOURCE

#include "runtime/runtime.h"

#include <errno.h>
#include <string.h>

#include "kernel/ipc.h"
#include "kernel/objects.h"
#include "kernel/sched.h"
#include "kernel/timeout.h"

static permute_kernel_t *g_k;
static permute_trace_t *g_trace;
static uint64_t g_seq;

_Static_assert(sizeof(struct sigevent) == sizeof(permute_sigevent_t),
               "guest and kernel sigevent must share the layout");
_Static_assert(sizeof(struct _pulse) == PERMUTE_PULSE_WIRE_SIZE,
               "guest and kernel pulse must share the wire size");

static permute_thread_t *current(void)
{
    return permute_thread_get(g_k, permute_runtime_current);
}

static int fail(int rc)
{
    errno = rc < 0 ? -rc : rc;
    return -1;
}

static void emit(uint8_t kind, uint16_t src, uint16_t dst, uint32_t obj,
                 uint32_t aux, const void *blob, uint16_t len)
{
    if (g_trace == NULL)
        return;
    g_seq++;
    (void)permute_trace_emit(g_trace, kind, g_seq, g_k->now_ns, src, dst, obj,
                             aux, blob, len, NULL);
}

// ---------------------------------------------------------------------------
// IPC

static int op_channel_create(unsigned flags)
{
    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    permute_handle_t handle;
    int chid = -1;
    int rc = permute_channel_create(g_k, t->process, flags, &chid, &handle);
    return rc == 0 ? chid : fail(rc);
}

static int op_channel_destroy(int chid)
{
    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);
    int rc = permute_channel_destroy(g_k, t->process, chid);
    return rc == 0 ? 0 : fail(rc);
}

static int op_connect_attach(uint32_t reserved, pid_t pid, int chid,
                             unsigned index, int flags)
{
    (void)reserved;
    (void)index;

    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    permute_handle_t server = t->process;
    if (pid != 0) {
        server = permute_process_handle_by_pid(g_k, (uint32_t)pid);
        if (server == PERMUTE_HANDLE_NONE)
            return fail(ESRCH);
    }

    int coid = -1;
    permute_handle_t handle;
    int rc = permute_connect_attach(g_k, t->process, server, chid, flags,
                                    &coid, &handle);
    return rc == 0 ? coid : fail(rc);
}

static int op_connect_detach(int coid)
{
    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);
    int rc = permute_connect_detach(g_k, t->process, coid);
    return rc == 0 ? 0 : fail(rc);
}

static long op_msg_send(int coid, const void *smsg, size_t sbytes, void *rmsg,
                        size_t rbytes)
{
    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    permute_result_t r;
    permute_action_t action = permute_ipc_send(g_k, t->self, coid, smsg,
                                               sbytes, rmsg, rbytes, &r);
    emit(PERMUTE_TR_SEND, (uint16_t)t->tid, 0, (uint32_t)coid,
         (uint32_t)sbytes, smsg, (uint16_t)sbytes);

    if (action == PERMUTE_BLOCKED) {
        permute_executor_park();
        r = t->pending;
    }

    if (r.err != 0)
        return fail(r.err);
    return r.ret;
}

static rcvid_t op_msg_receive(int chid, void *msg, size_t bytes,
                              struct _msg_info *info)
{
    permute_thread_t *t = current();
    if (t == NULL)
        return (rcvid_t)fail(ESRCH);

    permute_result_t r;
    permute_msg_info_t kinfo;
    memset(&kinfo, 0, sizeof kinfo);

    permute_action_t action =
        permute_ipc_receive(g_k, t->self, chid, msg, bytes, &kinfo, &r);
    if (action == PERMUTE_BLOCKED) {
        permute_executor_park();
        r = t->pending;
    }

    if (r.err != 0)
        return (rcvid_t)fail(r.err);

    if (r.rcvid == 0) {
        // A pulse is presented as struct _pulse in the caller's buffer.
        if (msg != NULL && bytes >= sizeof(struct _pulse)) {
            struct _pulse pulse;
            memset(&pulse, 0, sizeof pulse);
            pulse.code = (int8_t)r.pulse.code;
            pulse.value.sival_ptr = (void *)(uintptr_t)r.pulse.value;
            pulse.scoid = r.pulse.scoid;
            memcpy(msg, &pulse, sizeof pulse);
        }
        emit(PERMUTE_TR_PULSE_RECV, (uint16_t)t->tid, 0,
             (uint32_t)r.pulse.code, 0, NULL, 0);
        return 0;
    }

    if (info != NULL) {
        permute_handle_t mh;
        permute_message_t *m = NULL;
        if (permute_message_lookup_rcvid(g_k, r.rcvid, &mh) == 0)
            m = permute_message_get(g_k, mh);

        memset(info, 0, sizeof *info);
        if (m != NULL) {
            info->pid = (pid_t)m->src_pid;
            info->tid = (int32_t)m->src_tid;
            info->msglen = (int32_t)m->payload_len;
            info->priority = (uint16_t)m->send_prio;
        } else {
            info->pid = (pid_t)kinfo.src_pid;
            info->tid = (int32_t)kinfo.src_tid;
            info->msglen = (int32_t)kinfo.msglen;
            info->priority = (uint16_t)kinfo.priority;
        }
    }

    emit(PERMUTE_TR_RECV, (uint16_t)t->tid, 0, (uint32_t)r.rcvid,
         (uint32_t)r.bytes, msg, (uint16_t)r.bytes);
    return r.rcvid;
}

static int op_msg_reply(rcvid_t rcvid, long status, const void *msg,
                        size_t bytes)
{
    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    permute_result_t r;
    permute_ipc_reply(g_k, t->self, rcvid, status, msg, bytes, &r);
    emit(PERMUTE_TR_REPLY, (uint16_t)t->tid, 0, (uint32_t)rcvid, 0, NULL, 0);
    return r.err == 0 ? 0 : fail(r.err);
}

static int op_msg_error(rcvid_t rcvid, int error)
{
    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    permute_result_t r;
    permute_ipc_error(g_k, t->self, rcvid, error, &r);
    emit(PERMUTE_TR_ERROR, (uint16_t)t->tid, 0, (uint32_t)rcvid,
         (uint32_t)error, NULL, 0);
    return r.err == 0 ? 0 : fail(r.err);
}

static int op_msg_send_pulse(int coid, int priority, int code, long value)
{
    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    permute_result_t r;
    permute_ipc_send_pulse(g_k, t->self, coid, priority, code,
                           (uint64_t)value, &r);
    uint64_t payload = (uint64_t)value;
    emit(PERMUTE_TR_PULSE_SEND, (uint16_t)t->tid, 0, (uint32_t)coid,
         (uint32_t)code, &payload, sizeof payload);
    return r.err == 0 ? 0 : fail(r.err);
}

static int op_msg_receive_pulse(int chid, void *pulse, size_t bytes,
                                struct _msg_info *info)
{
    (void)info; // QNX does not fill it for pulses

    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    permute_result_t r;
    permute_action_t action =
        permute_ipc_receive_pulse(g_k, t->self, chid, pulse, bytes, &r);
    if (action == PERMUTE_BLOCKED) {
        permute_executor_park();
        r = t->pending;
    }

    if (r.err != 0)
        return fail(r.err);

    if (pulse != NULL && bytes >= sizeof(struct _pulse)) {
        struct _pulse p;
        memset(&p, 0, sizeof p);
        p.code = (int8_t)r.pulse.code;
        p.value.sival_ptr = (void *)(uintptr_t)r.pulse.value;
        p.scoid = r.pulse.scoid;
        memcpy(pulse, &p, sizeof p);
    }

    emit(PERMUTE_TR_PULSE_RECV, (uint16_t)t->tid, 0, (uint32_t)r.pulse.code,
         0, NULL, 0);
    return 0;
}

static int op_msg_register_event(struct sigevent *event, int coid)
{
    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    permute_result_t r;
    permute_ipc_register_event(g_k, t->self, (permute_sigevent_t *)event, coid,
                               &r);
    return r.err == 0 ? 0 : fail(r.err);
}

static int op_msg_deliver_event(rcvid_t rcvid, const struct sigevent *event)
{
    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    permute_result_t r;
    permute_ipc_deliver_event(g_k, t->self, rcvid,
                              (const permute_sigevent_t *)event, &r);
    emit(PERMUTE_TR_EVENT_DELIVER, (uint16_t)t->tid, 0, (uint32_t)rcvid, 0,
         NULL, 0);
    return r.err == 0 ? 0 : fail(r.err);
}

static int op_msg_verify_event(rcvid_t rcvid, const struct sigevent *event)
{
    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    permute_result_t r;
    permute_ipc_verify_event(g_k, t->self, rcvid,
                             (const permute_sigevent_t *)event, &r);
    return r.err == 0 ? 0 : fail(r.err);
}

// ---------------------------------------------------------------------------
// Time

static int op_clock_gettime(int clock_id, struct timespec *ts)
{
    (void)clock_id; // every modelled clock reads the virtual clock
    if (ts == NULL)
        return fail(EINVAL);
    ts->tv_sec = (time_t)(g_k->now_ns / 1000000000ull);
    ts->tv_nsec = (long)(g_k->now_ns % 1000000000ull);
    return 0;
}

static int op_nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)rem;
    if (req == NULL)
        return fail(EINVAL);

    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    uint64_t ns = (uint64_t)req->tv_sec * 1000000000ull +
                  (uint64_t)req->tv_nsec;
    permute_result_t r;
    permute_timeout_set(g_k, t->self, PERMUTE_STATE_BIT_NANOSLEEP, NULL, &ns,
                        NULL, &r);

    permute_thread_block(g_k, t, PERMUTE_THREAD_NANOSLEEP, PERMUTE_HANDLE_NONE);
    permute_timeout_enter(g_k, t);
    permute_executor_park();
    return 0;
}

static int op_timer_timeout(int clock_id, uint32_t flags,
                            const struct sigevent *event,
                            const uint64_t *ntime, uint64_t *otime)
{
    (void)clock_id;

    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    permute_result_t r;
    permute_timeout_set(g_k, t->self, flags,
                        (const permute_sigevent_t *)event, ntime, otime, &r);
    emit(PERMUTE_TR_TIMER_ARM, (uint16_t)t->tid, 0, 0, flags, NULL, 0);
    return r.err == 0 ? 0 : fail(r.err);
}

// ---------------------------------------------------------------------------
// Identity and scheduling

static pid_t op_getpid(void)
{
    permute_thread_t *t = current();
    permute_process_t *p = t ? permute_process_get(g_k, t->process) : NULL;
    return p != NULL ? (pid_t)p->pid : (pid_t)-1;
}

static pid_t op_gettid(void)
{
    permute_thread_t *t = current();
    return t != NULL ? (pid_t)t->tid : (pid_t)-1;
}

static uint64_t op_thread_token(void)
{
    return (uint64_t)permute_runtime_current;
}

static uint64_t op_rand_seed(void)
{
    permute_thread_t *t = current();
    return g_k->seed ^ (t != NULL ? t->pid : 0);
}

static int op_sched_set(pid_t pid, int tid, int policy,
                        const struct sched_param *param)
{
    (void)pid;
    (void)tid;
    (void)policy;
    if (param == NULL)
        return fail(EINVAL);

    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    int rc = permute_thread_set_prio(g_k, t->self, param->sched_priority);
    if (rc != 0)
        return fail(rc);

    // Let a raised priority take effect now rather than at the next block.
    permute_thread_yield(g_k, t);
    permute_executor_park();
    return 0;
}

static int op_sched_get(pid_t pid, int tid, struct sched_param *param)
{
    (void)pid;
    (void)tid;
    if (param == NULL)
        return fail(EINVAL);

    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    memset(param, 0, sizeof *param);
    // The host struct has no sched_curpriority, so the effective priority is
    // reported here. Documented as a simulator mapping.
    param->sched_priority = t->eff_prio;
    return 0;
}

static int op_sched_yield(void)
{
    permute_thread_t *t = current();
    if (t == NULL)
        return fail(ESRCH);

    permute_thread_yield(g_k, t);
    permute_executor_park();
    return 0;
}

// ---------------------------------------------------------------------------
// Thread lifecycle

static int op_pthread_create(uint64_t *token, const pthread_attr_t *attr,
                             void *(*start)(void *), void *arg)
{
    return permute_executor_thread_create(token, attr, start, arg);
}

static int op_pthread_join(uint64_t token, void **retval)
{
    return permute_executor_thread_join(token, retval);
}

static void op_pthread_exit(void *retval)
{
    permute_executor_thread_exit(retval);
}

static int op_pthread_cancel(uint64_t token)
{
    return permute_executor_thread_cancel(token);
}

// ---------------------------------------------------------------------------

static const permute_kernel_ops_t g_ops = {
    .channel_create = op_channel_create,
    .connect_attach = op_connect_attach,
    .msg_send = op_msg_send,
    .msg_receive = op_msg_receive,
    .msg_reply = op_msg_reply,
    .getpid = op_getpid,
    .gettid = op_gettid,
    .thread_token = op_thread_token,
    .clock_gettime = op_clock_gettime,
    .nanosleep = op_nanosleep,
    .rand_seed = op_rand_seed,
    .channel_destroy = op_channel_destroy,
    .connect_detach = op_connect_detach,
    .msg_error = op_msg_error,
    .msg_send_pulse = op_msg_send_pulse,
    .msg_receive_pulse = op_msg_receive_pulse,
    .msg_register_event = op_msg_register_event,
    .msg_deliver_event = op_msg_deliver_event,
    .msg_verify_event = op_msg_verify_event,
    .timer_timeout = op_timer_timeout,
    .sched_set = op_sched_set,
    .sched_get = op_sched_get,
    .sched_yield = op_sched_yield,
    .pthread_create = op_pthread_create,
    .pthread_join = op_pthread_join,
    .pthread_exit = op_pthread_exit,
    .pthread_cancel = op_pthread_cancel,
};

void permute_bridge_init(permute_kernel_t *k, permute_trace_t *trace)
{
    g_k = k;
    g_trace = trace;
    g_seq = 0;
}

const permute_kernel_ops_t *permute_bridge_ops(void)
{
    return &g_ops;
}
