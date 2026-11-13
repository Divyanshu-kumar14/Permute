// SPDX-License-Identifier: Apache-2.0
//
// Pulses and secure events.
//
// A pulse is a fixed-size, non-blocking message: one byte of code and eight
// bytes of value. It shares the channel queue with messages, ordered by
// priority then arrival, and it can wake either kind of receiver.
//
// Two rules here are easy to get wrong and are deliberately explicit:
//
//   1. MsgReceivePulse with a buffer smaller than struct _pulse does not
//      truncate. It returns EFAULT and the pulse is lost (FR-3.4). The check
//      therefore happens before blocking as well as at delivery.
//   2. Event delivery never raises the target's priority. Sticky priority is
//      a message-driven effect (P1-05); a pulse that wakes a thread must leave
//      its scheduling state untouched.

#include "kernel/ipc_internal.h"

#include <errno.h>
#include <string.h>

#include "kernel/pi.h"

// ---------------------------------------------------------------------------
// Helpers

static void result_error(permute_result_t *res, int err)
{
    res->action = PERMUTE_CONTINUE;
    res->ret = -1;
    res->err = err;
    res->rcvid = 0;
    res->bytes = 0;
}

static int32_t normalise_priority(int32_t priority, const permute_thread_t *ctx)
{
    if (priority < 0)
        return ctx->eff_prio; // -1 means "my priority"
    if (priority == 0)
        return 1;             // 0 is the idle priority and is coerced
    if (priority > PERMUTE_PRIO_MAX)
        return PERMUTE_PRIO_MAX;
    return priority;
}

// Give a pulse to a thread that is already blocked in a receive.
static void deliver_pulse(permute_thread_t *rx, int32_t code, uint64_t value,
                          int32_t priority, int32_t scoid)
{
    uint8_t short_buf = rx->recv_short;

    rx->state = PERMUTE_THREAD_READY;
    rx->wobj = PERMUTE_HANDLE_NONE;
    rx->recv_len = 0;
    rx->recv_rcvid = 0;
    rx->recv_kind = 0;
    rx->recv_short = 0;

    rx->pending.action = PERMUTE_CONTINUE;
    rx->pending.rcvid = 0;
    rx->pending.bytes = 0;

    if (short_buf) {
        // The buffer cannot hold a struct _pulse: the pulse is consumed and
        // lost, and the caller sees EFAULT.
        rx->pending.ret = -1;
        rx->pending.err = EFAULT;
        rx->pending.pulse.code = 0;
        rx->pending.pulse.value = 0;
        rx->pending.pulse.priority = 0;
        rx->pending.pulse.scoid = 0;
        return;
    }

    rx->pending.ret = 0;
    rx->pending.err = 0;
    rx->pending.pulse.code = code;
    rx->pending.pulse.value = value;
    rx->pending.pulse.priority = priority;
    rx->pending.pulse.scoid = scoid;
}

// ---------------------------------------------------------------------------
// Posting

permute_action_t permute_pulse_post(permute_kernel_t *k, permute_handle_t chan,
                                    int32_t code, uint64_t value,
                                    int32_t priority, int32_t scoid,
                                    uint32_t src_pid, uint32_t src_tid,
                                    permute_result_t *res)
{
    permute_channel_t *ch = permute_channel_get(k, chan);
    if (ch == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    code = (int32_t)(int8_t)code; // the wire field is one byte
    uint64_t seq = k->sequence++;

    // Both waiter kinds accept a pulse; take the highest-priority one. The
    // scan drops entries whose thread died, so a stale head cannot swallow a
    // pulse that a live waiter should have received.
    permute_queue_entry_t we;
    if (permute_queue_pop_matching(&ch->recv_waiters,
                                   PERMUTE_Q_THREAD | PERMUTE_Q_PULSE_RECV,
                                   &we) == 0) {
        permute_thread_t *rx = permute_thread_get(k, we.handle);
        if (rx != NULL && rx->state == PERMUTE_THREAD_RECEIVE) {
            permute_pi_on_send(k, chan, rx, priority);
            deliver_pulse(rx, code, value, priority, scoid);
            permute_pi_on_receive(k, chan, rx, priority);
            res->action = PERMUTE_CONTINUE;
            res->ret = 0;
            return PERMUTE_CONTINUE;
        }
    }

    permute_handle_t ph;
    int rc = permute_pulse_create(k, src_pid, src_tid, code, value, priority,
                                  &ph);
    if (rc != 0) {
        // QNX fails the pulse with EAGAIN when it cannot queue it.
        result_error(res, -rc == ENOMEM ? EAGAIN : -rc);
        return PERMUTE_CONTINUE;
    }

    permute_pulse_t *p = permute_pulse_get(k, ph);
    p->scoid = scoid;
    p->sequence = seq;

    rc = permute_queue_insert(&ch->recvq, ph, priority, seq, PERMUTE_Q_PULSE);
    if (rc != 0) {
        permute_pulse_destroy(k, ph);
        result_error(res, EAGAIN);
        return PERMUTE_CONTINUE;
    }

    res->action = PERMUTE_CONTINUE;
    res->ret = 0;
    return PERMUTE_CONTINUE;
}

// ---------------------------------------------------------------------------
// MsgSendPulse

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

    permute_handle_t chan;
    int32_t scoid;
    if (permute_coid_to_channel(k, t->process, coid, &chan, &scoid) != 0) {
        // A negative coid addresses the caller's own channel; a stale one
        // fails exactly like a bad connection.
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    int32_t prio = normalise_priority(priority, t);
    return permute_pulse_post(k, chan, code, value, prio, scoid, t->pid, t->tid,
                              res);
}

// ---------------------------------------------------------------------------
// MsgReceivePulse

permute_action_t permute_ipc_receive_pulse(permute_kernel_t *k,
                                           permute_handle_t thread, int chid,
                                           void *pulse, size_t bytes,
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

    // A buffer that can never hold struct _pulse is refused, and the pulse it
    // would have received is lost. That includes the case where no pulse has
    // arrived yet: the thread still blocks, and loses the next one.
    uint8_t short_buf = (bytes < PERMUTE_PULSE_WIRE_SIZE) ? 1u : 0u;

    permute_queue_entry_t e;
    if (permute_queue_pop_matching(&ch->recvq, PERMUTE_Q_PULSE, &e) == 0) {
        permute_pulse_t *p = permute_pulse_get(k, e.handle);
        if (short_buf) {
            if (p != NULL)
                permute_pulse_destroy(k, e.handle);
            result_error(res, EFAULT);
            return PERMUTE_CONTINUE;
        }
        if (p == NULL) {
            result_error(res, ESRCH);
            return PERMUTE_CONTINUE;
        }

        res->action = PERMUTE_CONTINUE;
        res->ret = 0;
        res->rcvid = 0;
        res->bytes = PERMUTE_PULSE_WIRE_SIZE;
        res->pulse.code = p->code;
        res->pulse.value = p->value;
        res->pulse.priority = p->priority;
        res->pulse.scoid = p->scoid;
        permute_pi_on_receive(k, chh, t, p->priority);
        permute_pulse_destroy(k, e.handle);
        return PERMUTE_CONTINUE;
    }

    // Nothing queued. Block as a pulse-only receiver; a message that arrives
    // meanwhile must be left for a MsgReceive waiter.
    t->state = PERMUTE_THREAD_RECEIVE;
    t->wobj = chh;
    t->recv_buf = pulse;
    t->recv_max = (uint32_t)bytes;
    t->recv_len = 0;
    t->recv_rcvid = 0;
    t->recv_kind = PERMUTE_Q_PULSE_RECV;
    t->recv_short = short_buf;

    int rc = permute_queue_insert(&ch->recv_waiters, thread, t->eff_prio,
                                  k->sequence++, PERMUTE_Q_PULSE_RECV);
    if (rc != 0) {
        t->state = PERMUTE_THREAD_RUNNING;
        t->wobj = PERMUTE_HANDLE_NONE;
        t->recv_kind = 0;
        t->recv_short = 0;
        result_error(res, EAGAIN);
        return PERMUTE_CONTINUE;
    }

    res->action = PERMUTE_BLOCKED;
    return PERMUTE_BLOCKED;
}

// ---------------------------------------------------------------------------
// Secure events

static permute_registration_t *registration_by_handle(permute_kernel_t *k,
                                                      uint32_t handle)
{
    if (handle == 0)
        return NULL;

    for (uint32_t i = 0; i < k->registrations.capacity; i++) {
        if (k->registrations.in_use[i] &&
            k->registration_pool[i].handle == handle)
            return &k->registration_pool[i];
    }
    return NULL;
}

// Copy an event into its canonical form. The reserved words are padding in
// the platform struct, so they must not take part in a comparison.
static permute_sigevent_t normalise_event(const permute_sigevent_t *e)
{
    permute_sigevent_t n = *e;
    n.reserved0 = 0;
    n.reserved1 = 0;
    n.reserved2 = 0;

    if (n.notify == PERMUTE_SIGEV_PULSE) {
        n.code = (int16_t)(int8_t)n.code;
        if (n.priority == 0)
            n.priority = 1;
        else if (n.priority > PERMUTE_PRIO_MAX)
            n.priority = (int16_t)PERMUTE_PRIO_MAX;
    }
    return n;
}

static permute_action_t register_event(permute_kernel_t *k,
                                       permute_thread_t *t,
                                       permute_sigevent_t *event, int coid,
                                       permute_result_t *res)
{
    if (event == NULL) {
        result_error(res, EINVAL);
        return PERMUTE_CONTINUE;
    }

    // Signals are out of scope (PRD section 17); the pulse and unblock forms
    // are the ones a transport actually uses.
    if (event->notify == PERMUTE_SIGEV_HANDLE) {
        result_error(res, EINVAL); // already a handle, not an event
        return PERMUTE_CONTINUE;
    }
    if (event->notify != PERMUTE_SIGEV_PULSE &&
        event->notify != PERMUTE_SIGEV_UNBLOCK) {
        result_error(res, EINVAL);
        return PERMUTE_CONTINUE;
    }

    // The coid names the one server allowed to fire this event. Special
    // registration targets (_NTO_REGEVENT_ALLOW_ANY_SERVER and friends) are
    // not modelled: their numeric values are not in the reference text, and
    // guessing them would be a fake ABI.
    permute_handle_t connh;
    if (coid < 0 || permute_connection_lookup(k, t->process, coid, &connh) != 0) {
        result_error(res, EINVAL);
        return PERMUTE_CONTINUE;
    }
    permute_connection_t *c = permute_connection_get(k, connh);
    if (c == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    uint32_t slot, gen;
    int rc = permute_table_alloc(&k->registrations, &slot, &gen);
    if (rc != 0) {
        result_error(res, -rc == EMFILE ? EAGAIN : -rc);
        return PERMUTE_CONTINUE;
    }

    permute_registration_t *r = &k->registration_pool[slot];
    memset(r, 0, sizeof *r);
    r->handle = k->next_registration++;
    if (r->handle == 0)
        r->handle = k->next_registration++; // never hand out 0
    r->owner_pid = t->pid;
    r->owner_tid = t->tid;
    r->allowed_pid = c->server_pid;
    r->allowed_coid = coid;
    r->event = normalise_event(event);

    // QNX replaces the caller's event with the handle; the caller then sends
    // that handle in its message and the kernel maps it back.
    memset(event, 0, sizeof *event);
    event->notify = PERMUTE_SIGEV_HANDLE;
    event->coid = (int32_t)r->handle;

    res->action = PERMUTE_CONTINUE;
    res->ret = 0;
    return PERMUTE_CONTINUE;
}

permute_action_t permute_ipc_register_event(permute_kernel_t *k,
                                            permute_handle_t thread,
                                            permute_sigevent_t *event, int coid,
                                            permute_result_t *res)
{
    memset(res, 0, sizeof *res);

    permute_thread_t *t = permute_thread_get(k, thread);
    if (t == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }
    return register_event(k, t, event, coid, res);
}

// Wake a thread that is blocked in a kernel call. SIGEV_UNBLOCK's meaning:
// the blocked call fails with EINTR.
static void unblock_thread(permute_thread_t *t, int err)
{
    switch (t->state) {
    case PERMUTE_THREAD_SEND:
    case PERMUTE_THREAD_REPLY:
    case PERMUTE_THREAD_RECEIVE:
    case PERMUTE_THREAD_NANOSLEEP:
    case PERMUTE_THREAD_MUTEX:
    case PERMUTE_THREAD_COND:
    case PERMUTE_THREAD_SEM:
    case PERMUTE_THREAD_JOIN:
        t->state = PERMUTE_THREAD_READY;
        t->wobj = PERMUTE_HANDLE_NONE;
        t->pending.action = PERMUTE_CONTINUE;
        t->pending.ret = -1;
        t->pending.err = err;
        t->pending.rcvid = 0;
        t->pending.bytes = 0;
        break;
    default:
        break; // not blocked: nothing to cancel
    }
}

static permute_action_t deliver_event(permute_kernel_t *k,
                                      permute_thread_t *caller,
                                      permute_handle_t target_process,
                                      permute_thread_t *target,
                                      const permute_sigevent_t *ev,
                                      permute_result_t *res)
{
    permute_sigevent_t e = normalise_event(ev);

    switch (e.notify) {
    case PERMUTE_SIGEV_PULSE: {
        permute_handle_t chan;
        int32_t scoid;
        if (permute_coid_to_channel(k, target_process, e.coid, &chan,
                                    &scoid) != 0) {
            result_error(res, ESRCH);
            return PERMUTE_CONTINUE;
        }
        int32_t prio = normalise_priority(e.priority, caller);
        return permute_pulse_post(k, chan, e.code, e.value, prio, scoid, 0, 0,
                                  res);
    }
    case PERMUTE_SIGEV_UNBLOCK:
        if (target != NULL)
            unblock_thread(target, EINTR);
        res->action = PERMUTE_CONTINUE;
        res->ret = 0;
        return PERMUTE_CONTINUE;
    default:
        result_error(res, EINVAL);
        return PERMUTE_CONTINUE;
    }
}

permute_action_t permute_ipc_deliver_event(permute_kernel_t *k,
                                           permute_handle_t thread, int32_t rcvid,
                                           const permute_sigevent_t *event,
                                           permute_result_t *res)
{
    memset(res, 0, sizeof *res);

    permute_thread_t *t = permute_thread_get(k, thread);
    if (t == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }
    if (event == NULL) {
        result_error(res, EINVAL);
        return PERMUTE_CONTINUE;
    }

    permute_handle_t target_process = PERMUTE_HANDLE_NONE;
    permute_thread_t *target = NULL;
    permute_sigevent_t registered;

    if (rcvid > 0) {
        // A message context: the event must be one the client registered for
        // this server. That registration is the capability.
        permute_handle_t mh;
        if (permute_message_lookup_rcvid(k, rcvid, &mh) != 0) {
            result_error(res, ESRCH);
            return PERMUTE_CONTINUE;
        }
        permute_message_t *m = permute_message_get(k, mh);
        if (m == NULL) {
            result_error(res, ESRCH);
            return PERMUTE_CONTINUE;
        }
        if (event->notify != PERMUTE_SIGEV_HANDLE) {
            result_error(res, EACCES);
            return PERMUTE_CONTINUE;
        }

        permute_registration_t *r =
            registration_by_handle(k, (uint32_t)event->coid);
        if (r == NULL || r->owner_pid != m->src_pid ||
            r->owner_tid != m->src_tid || r->allowed_pid != t->pid) {
            result_error(res, EACCES);
            return PERMUTE_CONTINUE;
        }

        target_process = permute_process_handle_by_pid(k, m->src_pid);
        target = permute_thread_by_tid(k, m->src_pid, m->src_tid);
        registered = r->event;
        return deliver_event(k, t, target_process, target, &registered, res);
    }

    if (rcvid == 0) {
        // Deliver to the calling thread. No registration is required.
        if (event->notify == PERMUTE_SIGEV_HANDLE) {
            result_error(res, EACCES);
            return PERMUTE_CONTINUE;
        }
        return deliver_event(k, t, t->process, t, event, res);
    }

    // Negative rcvid: the absolute value is a thread id.
    target = permute_thread_by_global_tid(k, (uint32_t)(-rcvid));
    if (target == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }
    if (event->notify == PERMUTE_SIGEV_HANDLE) {
        result_error(res, EACCES);
        return PERMUTE_CONTINUE;
    }

    target_process = permute_process_handle_by_pid(k, target->pid);
    return deliver_event(k, t, target_process, target, event, res);
}

permute_action_t permute_ipc_verify_event(permute_kernel_t *k,
                                          permute_handle_t thread, int32_t rcvid,
                                          const permute_sigevent_t *event,
                                          permute_result_t *res)
{
    memset(res, 0, sizeof *res);

    permute_thread_t *t = permute_thread_get(k, thread);
    if (t == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }
    if (event == NULL) {
        result_error(res, EINVAL);
        return PERMUTE_CONTINUE;
    }

    if (rcvid > 0) {
        permute_handle_t mh;
        if (permute_message_lookup_rcvid(k, rcvid, &mh) != 0) {
            result_error(res, ESRCH);
            return PERMUTE_CONTINUE;
        }
        permute_message_t *m = permute_message_get(k, mh);
        if (m == NULL) {
            result_error(res, ESRCH);
            return PERMUTE_CONTINUE;
        }
        if (event->notify != PERMUTE_SIGEV_HANDLE) {
            result_error(res, EINVAL);
            return PERMUTE_CONTINUE;
        }

        permute_registration_t *r =
            registration_by_handle(k, (uint32_t)event->coid);
        if (r == NULL || r->owner_pid != m->src_pid ||
            r->owner_tid != m->src_tid || r->allowed_pid != t->pid) {
            result_error(res, EINVAL);
            return PERMUTE_CONTINUE;
        }

        // The pulse target must still exist for the event to be deliverable.
        if (r->event.notify == PERMUTE_SIGEV_PULSE) {
            permute_handle_t chan;
            permute_handle_t owner = permute_process_handle_by_pid(k, m->src_pid);
            if (permute_coid_to_channel(k, owner, r->event.coid, &chan, NULL) !=
                0) {
                result_error(res, ESRCH);
                return PERMUTE_CONTINUE;
            }
        }
        res->action = PERMUTE_CONTINUE;
        res->ret = 0;
        return PERMUTE_CONTINUE;
    }

    // rcvid 0 or negative: well-formedness only.
    if (event->notify == PERMUTE_SIGEV_HANDLE) {
        result_error(res, EINVAL); // a handle means nothing without an owner
        return PERMUTE_CONTINUE;
    }
    if (event->notify != PERMUTE_SIGEV_PULSE &&
        event->notify != PERMUTE_SIGEV_UNBLOCK) {
        result_error(res, EINVAL);
        return PERMUTE_CONTINUE;
    }
    if (event->notify == PERMUTE_SIGEV_PULSE) {
        permute_handle_t chan;
        if (permute_coid_to_channel(k, t->process, event->coid, &chan, NULL) !=
            0) {
            result_error(res, ESRCH);
            return PERMUTE_CONTINUE;
        }
    }

    res->action = PERMUTE_CONTINUE;
    res->ret = 0;
    return PERMUTE_CONTINUE;
}

// ---------------------------------------------------------------------------
// Channel disconnect notification

void permute_pulse_on_detach(permute_kernel_t *k, permute_handle_t chan,
                             int32_t scoid, uint32_t client_pid)
{
    permute_channel_t *ch = permute_channel_get(k, chan);
    if (ch == NULL || (ch->flags & PERMUTE_CHF_DISCONNECT) == 0)
        return;

    // Only when the client has no connection left to this channel.
    for (uint32_t i = 0; i < k->connections.capacity; i++) {
        if (!k->connections.in_use[i])
            continue;
        const permute_connection_t *c = &k->connection_pool[i];
        if (c->channel == chan && c->client_pid == client_pid)
            return;
    }

    permute_result_t ignored;
    permute_pulse_post(k, chan, PERMUTE_PULSE_CODE_DISCONNECT,
                       (uint64_t)(int64_t)scoid, 1, scoid, 0, 0, &ignored);
}
