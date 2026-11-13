// SPDX-License-Identifier: Apache-2.0

#include "kernel/ipc.h"

#include <errno.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Helpers

static permute_thread_t *thread_by_tid(permute_kernel_t *k, uint32_t pid,
                                       uint32_t tid)
{
    for (uint32_t i = 0; i < k->threads.capacity; i++) {
        if (!k->threads.in_use[i])
            continue;
        if (k->thread_pool[i].pid == pid && k->thread_pool[i].tid == tid)
            return &k->thread_pool[i];
    }
    return NULL;
}

static void result_error(permute_result_t *res, int err)
{
    res->action = PERMUTE_CONTINUE;
    res->ret = -1;
    res->err = err;
    res->rcvid = 0;
    res->bytes = 0;
}

// Copy a message into a receiver that is already blocked in MsgReceive. The
// receiver's buffer was captured at call time, so the copy happens here.
static void deliver_message(permute_thread_t *receiver, permute_message_t *m)
{
    uint32_t n = m->payload_len;
    if (n > receiver->recv_max)
        n = receiver->recv_max;
    if (receiver->recv_buf == NULL)
        n = 0;

    if (n > 0)
        memcpy(receiver->recv_buf, m->payload, n);

    receiver->recv_len = n;
    receiver->recv_rcvid = m->rcvid;
    receiver->state = PERMUTE_THREAD_READY;
    receiver->wobj = PERMUTE_HANDLE_NONE;

    receiver->pending.action = PERMUTE_CONTINUE;
    receiver->pending.ret = m->rcvid;
    receiver->pending.err = 0;
    receiver->pending.rcvid = m->rcvid;
    receiver->pending.bytes = n;

    m->receiver_tid = receiver->tid;
    m->in_receive_queue = 0;
}

static void deliver_pulse(permute_thread_t *receiver, const permute_pulse_t *p)
{
    receiver->recv_len = 0;
    receiver->recv_rcvid = 0;
    receiver->state = PERMUTE_THREAD_READY;
    receiver->wobj = PERMUTE_HANDLE_NONE;

    receiver->pending.action = PERMUTE_CONTINUE;
    receiver->pending.ret = 0;
    receiver->pending.err = 0;
    receiver->pending.rcvid = 0;
    receiver->pending.bytes = 0;
    receiver->pending.pulse.code = p->code;
    receiver->pending.pulse.value = p->value;
    receiver->pending.pulse.priority = p->priority;
}

// A queued message has just been received: the sender leaves SEND for REPLY.
static void sender_to_reply(permute_kernel_t *k, const permute_message_t *m)
{
    permute_thread_t *sender = thread_by_tid(k, m->src_pid, m->src_tid);
    if (sender != NULL && sender->state == PERMUTE_THREAD_SEND)
        sender->state = PERMUTE_THREAD_REPLY;
}

// ---------------------------------------------------------------------------
// Send

permute_action_t permute_ipc_send(permute_kernel_t *k, permute_handle_t thread,
                                  int coid, const void *smsg, size_t sbytes,
                                  void *rmsg, size_t rbytes,
                                  permute_result_t *res)
{
    memset(res, 0, sizeof *res);

    permute_thread_t *t = permute_thread_get(k, thread);
    if (t == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    permute_handle_t connh;
    if (permute_connection_lookup(k, t->process, coid, &connh) != 0) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    permute_connection_t *c = permute_connection_get(k, connh);
    permute_channel_t *ch = (c != NULL) ? permute_channel_get(k, c->channel)
                                        : NULL;
    if (ch == NULL) {
        // The connection outlived its channel; QNX reports ESRCH.
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    permute_handle_t mh;
    int rc = permute_message_create(k, t->process, t->tid, ch->owner_pid,
                                    t->eff_prio, smsg, (uint32_t)sbytes, &mh);
    if (rc != 0) {
        result_error(res, -rc);
        return PERMUTE_CONTINUE;
    }

    permute_message_t *m = permute_message_get(k, mh);

    // Reply context lives on the blocking thread: the reply buffer must still
    // be reachable when the server replies later.
    t->reply_buf = rmsg;
    t->reply_max = (uint32_t)rbytes;
    t->reply_len = 0;
    t->reply_err = 0;
    t->reply_status = 0;

    if (permute_queue_length(&ch->recv_waiters) > 0) {
        permute_queue_entry_t we;
        if (permute_queue_pop(&ch->recv_waiters, &we) == 0) {
            permute_thread_t *receiver = permute_thread_get(k, we.handle);
            if (receiver != NULL) {
                deliver_message(receiver, m);
                t->state = PERMUTE_THREAD_REPLY;
                t->wrcvid = m->rcvid;
                res->action = PERMUTE_BLOCKED;
                return PERMUTE_BLOCKED;
            }
        }
    }

    rc = permute_queue_insert(&ch->recvq, mh, t->eff_prio, m->sequence,
                              PERMUTE_Q_MESSAGE);
    if (rc != 0) {
        permute_message_destroy(k, mh);
        result_error(res, EAGAIN);
        return PERMUTE_CONTINUE;
    }

    m->in_receive_queue = 1;
    t->state = PERMUTE_THREAD_SEND;
    res->action = PERMUTE_BLOCKED;
    return PERMUTE_BLOCKED;
}

// ---------------------------------------------------------------------------
// Receive

permute_action_t permute_ipc_receive(permute_kernel_t *k, permute_handle_t thread,
                                     int chid, void *msg, size_t bytes,
                                     permute_msg_info_t *info,
                                     permute_result_t *res)
{
    memset(res, 0, sizeof *res);

    permute_thread_t *t = permute_thread_get(k, thread);
    if (t == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    permute_handle_t chh;
    if (permute_channel_lookup(k, t->process, chid, &chh) != 0) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    permute_channel_t *ch = permute_channel_get(k, chh);
    if (ch == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    permute_queue_entry_t e;
    if (permute_queue_pop(&ch->recvq, &e) == 0) {
        if (e.kind == PERMUTE_Q_PULSE) {
            permute_pulse_t *p = permute_pulse_get(k, e.handle);
            if (p != NULL) {
                res->action = PERMUTE_CONTINUE;
                res->ret = 0;
                res->rcvid = 0; // FR-3.3: a pulse receive returns 0
                res->bytes = 0;
                res->pulse.code = p->code;
                res->pulse.value = p->value;
                res->pulse.priority = p->priority;
                permute_pulse_destroy(k, e.handle);
                return PERMUTE_CONTINUE;
            }
            result_error(res, ESRCH);
            return PERMUTE_CONTINUE;
        }

        permute_message_t *m = permute_message_get(k, e.handle);
        if (m == NULL) {
            result_error(res, ESRCH);
            return PERMUTE_CONTINUE;
        }

        uint32_t n = m->payload_len;
        if (n > bytes)
            n = bytes;
        if (msg == NULL)
            n = 0;
        if (n > 0)
            memcpy(msg, m->payload, n);

        if (info != NULL) {
            info->src_pid = m->src_pid;
            info->src_tid = m->src_tid;
            info->dst_pid = m->dst_pid;
            info->dst_tid = m->dst_tid;
            info->msglen = m->payload_len;
            info->priority = m->send_prio;
        }

        m->in_receive_queue = 0;
        m->receiver_tid = t->tid;
        sender_to_reply(k, m);

        res->action = PERMUTE_CONTINUE;
        res->ret = m->rcvid;
        res->rcvid = m->rcvid;
        res->bytes = n;
        return PERMUTE_CONTINUE;
    }

    // Nothing pending: block, remembering the caller's buffer.
    t->state = PERMUTE_THREAD_RECEIVE;
    t->wobj = chh;
    t->recv_buf = msg;
    t->recv_max = (uint32_t)bytes;
    t->recv_len = 0;
    t->recv_rcvid = 0;

    int rc = permute_queue_insert(&ch->recv_waiters, thread, t->eff_prio,
                                  k->sequence++, PERMUTE_Q_THREAD);
    if (rc != 0) {
        t->state = PERMUTE_THREAD_RUNNING;
        t->wobj = PERMUTE_HANDLE_NONE;
        result_error(res, EAGAIN);
        return PERMUTE_CONTINUE;
    }

    res->action = PERMUTE_BLOCKED;
    return PERMUTE_BLOCKED;
}

// ---------------------------------------------------------------------------
// Reply and error

static permute_action_t finish_reply(permute_kernel_t *k, int32_t rcvid,
                                     long status, const void *msg, size_t bytes,
                                     int error, permute_result_t *res)
{
    memset(res, 0, sizeof *res);

    permute_handle_t mh;
    if (permute_message_lookup_rcvid(k, rcvid, &mh) != 0) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    permute_message_t *m = permute_message_get(k, mh);
    if (m == NULL || m->replied) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    permute_thread_t *client = thread_by_tid(k, m->src_pid, m->src_tid);
    if (client == NULL) {
        // The client died before the reply; the message is no longer useful.
        permute_message_destroy(k, mh);
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    uint32_t n = (uint32_t)bytes;
    if (n > client->reply_max)
        n = client->reply_max;
    if (client->reply_buf == NULL || msg == NULL)
        n = 0;
    if (n > 0)
        memcpy(client->reply_buf, msg, n);

    client->reply_len = n;
    client->reply_status = status;
    client->reply_err = error;
    client->state = PERMUTE_THREAD_READY;
    client->wrcvid = 0;

    client->pending.action = PERMUTE_CONTINUE;
    client->pending.ret = status;
    client->pending.err = error;
    client->pending.rcvid = 0;
    client->pending.bytes = n;

    m->replied = 1;
    permute_message_destroy(k, mh);

    res->action = PERMUTE_CONTINUE;
    res->ret = 0;
    return PERMUTE_CONTINUE;
}

permute_action_t permute_ipc_reply(permute_kernel_t *k, permute_handle_t thread,
                                   int32_t rcvid, long status, const void *msg,
                                   size_t bytes, permute_result_t *res)
{
    (void)thread; // the replying thread's identity is not needed yet
    return finish_reply(k, rcvid, status, msg, bytes, 0, res);
}

permute_action_t permute_ipc_error(permute_kernel_t *k, permute_handle_t thread,
                                   int32_t rcvid, int error,
                                   permute_result_t *res)
{
    (void)thread;
    return finish_reply(k, rcvid, -1, NULL, 0, error, res);
}

// ---------------------------------------------------------------------------
// Pulses

permute_action_t permute_ipc_send_pulse(permute_kernel_t *k,
                                        permute_handle_t thread, int coid,
                                        int priority, int32_t code,
                                        uint64_t value, permute_result_t *res)
{
    memset(res, 0, sizeof *res);

    permute_thread_t *t = permute_thread_get(k, thread);
    if (t == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    permute_handle_t connh;
    if (permute_connection_lookup(k, t->process, coid, &connh) != 0) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    permute_connection_t *c = permute_connection_get(k, connh);
    permute_channel_t *ch = (c != NULL) ? permute_channel_get(k, c->channel)
                                        : NULL;
    if (ch == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    int32_t prio;
    if (priority == -1)
        prio = t->eff_prio; // caller's priority
    else if (priority == 0)
        prio = 1;           // QNX coerces 0 to 1
    else
        prio = priority;

    permute_handle_t ph;
    int rc = permute_pulse_create(k, t->process, t->tid, code, value, prio, &ph);
    if (rc != 0) {
        result_error(res, -rc);
        return PERMUTE_CONTINUE;
    }

    if (permute_queue_length(&ch->recv_waiters) > 0) {
        permute_queue_entry_t we;
        if (permute_queue_pop(&ch->recv_waiters, &we) == 0) {
            permute_thread_t *receiver = permute_thread_get(k, we.handle);
            permute_pulse_t *p = permute_pulse_get(k, ph);
            if (receiver != NULL && p != NULL) {
                deliver_pulse(receiver, p);
                permute_pulse_destroy(k, ph);
                res->action = PERMUTE_CONTINUE;
                res->ret = 0;
                return PERMUTE_CONTINUE;
            }
        }
    }

    rc = permute_queue_insert(&ch->recvq, ph, prio, k->sequence++, PERMUTE_Q_PULSE);
    if (rc != 0) {
        permute_pulse_destroy(k, ph);
        result_error(res, EAGAIN);
        return PERMUTE_CONTINUE;
    }

    res->action = PERMUTE_CONTINUE;
    res->ret = 0;
    return PERMUTE_CONTINUE;
}
