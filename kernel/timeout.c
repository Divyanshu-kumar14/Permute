// SPDX-License-Identifier: Apache-2.0

#include "kernel/timeout.h"

#include <errno.h>
#include <string.h>

#include "kernel/ipc_internal.h"
#include "kernel/objects.h"
#include "kernel/pi.h"
#include "kernel/readyq.h"
#include "kernel/timer.h"

uint32_t permute_timeout_state_bit(permute_thread_state_t s)
{
    switch (s) {
    case PERMUTE_THREAD_SEND:
        return PERMUTE_STATE_BIT_SEND;
    case PERMUTE_THREAD_REPLY:
        return PERMUTE_STATE_BIT_REPLY;
    case PERMUTE_THREAD_RECEIVE:
        return PERMUTE_STATE_BIT_RECEIVE;
    case PERMUTE_THREAD_NANOSLEEP:
        return PERMUTE_STATE_BIT_NANOSLEEP;
    case PERMUTE_THREAD_MUTEX:
        return PERMUTE_STATE_BIT_MUTEX;
    case PERMUTE_THREAD_COND:
        return PERMUTE_STATE_BIT_CONDVAR;
    case PERMUTE_THREAD_SEM:
        return PERMUTE_STATE_BIT_SEM;
    case PERMUTE_THREAD_JOIN:
        return PERMUTE_STATE_BIT_JOIN;
    case PERMUTE_THREAD_INTR:
        return PERMUTE_STATE_BIT_INTR;
    default:
        return 0;
    }
}

static void disarm(permute_thread_t *t)
{
    t->timeout_armed = 0;
    t->timeout_mask = 0;
    t->timeout_deadline_ns = 0;
    t->timeout_seq = 0;
}

permute_action_t permute_timeout_set(permute_kernel_t *k,
                                     permute_handle_t thread, uint32_t flags,
                                     const permute_sigevent_t *event,
                                     const uint64_t *ntime, uint64_t *otime,
                                     permute_result_t *res)
{
    memset(res, 0, sizeof *res);

    permute_thread_t *t = permute_thread_get(k, thread);
    if (t == NULL) {
        res->ret = -1;
        res->err = ESRCH;
        return PERMUTE_CONTINUE;
    }

    // TimerTimeout accepts only SIGEV_UNBLOCK; NULL means the same.
    if (event != NULL && event->notify != PERMUTE_SIGEV_UNBLOCK) {
        res->ret = -1;
        res->err = EINVAL;
        return PERMUTE_CONTINUE;
    }

    uint32_t previous_mask = t->timeout_mask;
    uint64_t previous_deadline = t->timeout_deadline_ns;
    if (otime != NULL)
        *otime = previous_deadline;

    // Disarm: flags == 0 (vendor) or ntime == NULL (PRD 3.23).
    if (flags == 0 || ntime == NULL) {
        disarm(t);
        res->action = PERMUTE_CONTINUE;
        res->ret = (long)previous_mask;
        return PERMUTE_CONTINUE;
    }

    uint32_t mask = flags & ~PERMUTE_TIMER_ABSTIME;
    if (mask == 0 || (mask & ~PERMUTE_TIMEOUT_MASK_ALL) != 0) {
        res->ret = -1;
        res->err = EINVAL;
        return PERMUTE_CONTINUE;
    }

    uint64_t when = *ntime;
    if ((flags & PERMUTE_TIMER_ABSTIME) == 0) {
        if (UINT64_MAX - k->now_ns < when) {
            res->ret = -1;
            res->err = EOVERFLOW;
            return PERMUTE_CONTINUE;
        }
        when += k->now_ns;
    }

    t->timeout_mask = mask;
    t->timeout_deadline_ns = when;
    t->timeout_armed = 1;
    t->timeout_seq = ++k->sequence;

    // If the thread is already blocked in a state the mask covers, the timer
    // activates now; otherwise it waits for the next matching block (FR-3.22).
    if (permute_timeout_state_bit(t->state) & mask) {
        int rc = permute_eventq_push(&k->eventq, when, t->timeout_seq, t->self,
                                     PERMUTE_TIMER_THREAD);
        if (rc != 0) {
            disarm(t);
            res->ret = -1;
            res->err = EAGAIN;
            return PERMUTE_CONTINUE;
        }
    }

    res->action = PERMUTE_CONTINUE;
    res->ret = (long)previous_mask;
    return PERMUTE_CONTINUE;
}

void permute_timeout_enter(permute_kernel_t *k, permute_thread_t *t)
{
    if (!t->timeout_armed)
        return;
    if ((permute_timeout_state_bit(t->state) & t->timeout_mask) == 0)
        return; // latch: this state is not covered, stay armed

    // Capacity is guaranteed: one entry per thread plus one per POSIX timer.
    (void)permute_eventq_push(&k->eventq, t->timeout_deadline_ns,
                              t->timeout_seq, t->self, PERMUTE_TIMER_THREAD);
}

// Remove a timed-out thread from whatever it was blocked on, so the rest of
// the system does not keep a reference to a call that has already failed.
static void unblock_timed_out(permute_kernel_t *k, permute_thread_t *t)
{
    switch (t->state) {
    case PERMUTE_THREAD_SEND: {
        permute_message_t *m = permute_message_get(k, t->wmsg);
        if (m != NULL) {
            if (m->in_receive_queue) {
                permute_channel_t *ch = permute_channel_get(k, m->channel);
                if (ch != NULL)
                    permute_queue_remove(&ch->recvq, t->wmsg);
            }
            permute_message_destroy(k, t->wmsg);
        }
        t->wmsg = PERMUTE_HANDLE_NONE;
        break;
    }
    case PERMUTE_THREAD_REPLY: {
        permute_message_t *m = permute_message_get(k, t->wmsg);
        if (m != NULL) {
            permute_channel_t *ch = permute_channel_get(k, m->channel);
            if (ch != NULL && (ch->flags & PERMUTE_CHF_UNBLOCK)) {
                // The server asked to hear about this before the client goes.
                // The client stays REPLY-blocked; the server decides when to
                // reply (the QNX unblock contract).
                permute_result_t ignored;
                permute_pulse_post(k, m->channel, PERMUTE_PULSE_CODE_UNBLOCK,
                                   (uint64_t)(int64_t)m->rcvid, 1, 0, 0, 0,
                                   &ignored);
                return; // timeout consumed, thread still blocked
            }
            // No unblock notification: the late reply must fail with ESRCH.
            permute_message_destroy(k, t->wmsg);
        }
        t->wmsg = PERMUTE_HANDLE_NONE;
        t->wrcvid = 0;
        break;
    }
    case PERMUTE_THREAD_RECEIVE: {
        permute_channel_t *ch = permute_channel_get(k, t->wobj);
        if (ch != NULL)
            permute_queue_remove(&ch->recv_waiters, t->self);
        t->recv_kind = 0;
        t->recv_short = 0;
        break;
    }
    case PERMUTE_THREAD_MUTEX: {
        permute_mutex_t *m = permute_mutex_get(k, t->wobj);
        if (m != NULL) {
            permute_queue_remove(&m->waiters, t->self);
            permute_pi_mutex_waiter_gone(k, m);
        }
        break;
    }
    default:
        break;
    }

    permute_thread_wake(k, t, -1, ETIMEDOUT, 0);
}

int permute_timeout_fire(permute_kernel_t *k, const permute_timer_entry_t *e)
{
    if (e->kind == PERMUTE_TIMER_POSIX)
        return permute_timer_fire(k, e->target, e->seq);

    permute_thread_t *t = permute_thread_get(k, e->target);
    if (t == NULL || !t->timeout_armed || t->timeout_seq != e->seq)
        return 0; // disarmed or replaced

    // The entry is consumed either way; the intent survives only if the
    // thread was not in a covered state when it fired.
    t->timeout_seq = 0;

    if ((permute_timeout_state_bit(t->state) & t->timeout_mask) == 0) {
        t->timeout_armed = 1; // keep waiting for a covered state
        return 0;
    }

    disarm(t);
    unblock_timed_out(k, t);
    return 1;
}

static int anything_runnable(const permute_kernel_t *k)
{
    return permute_readyq_count(&k->readyq) > 0 ||
           k->running != PERMUTE_HANDLE_NONE;
}

// A heap entry is live only while the thing it refers to still wants it.
// Superseded entries must be dropped *before* the clock moves to their
// deadline, or replacing a timer would still be observable as a time jump.
static int entry_is_live(permute_kernel_t *k, const permute_timer_entry_t *e)
{
    if (e->kind == PERMUTE_TIMER_POSIX) {
        permute_posix_timer_t *t = permute_posix_timer_get(k, e->target);
        return t != NULL && t->armed && t->seq == e->seq;
    }

    permute_thread_t *t = permute_thread_get(k, e->target);
    return t != NULL && t->timeout_armed && t->timeout_seq == e->seq;
}

permute_sim_outcome_t permute_sim_advance(permute_kernel_t *k)
{
    if (anything_runnable(k))
        return PERMUTE_SIM_RUNNABLE;

    permute_timer_entry_t e;
    for (;;) {
        if (permute_eventq_peek(&k->eventq, &e) != 0)
            return PERMUTE_SIM_DEADLOCK;
        if (entry_is_live(k, &e))
            break;
        (void)permute_eventq_pop(&k->eventq, &e); // stale: no clock movement
    }

    if (permute_vclock_advance(k, e.deadline_ns) != 0)
        return PERMUTE_SIM_IDLE;

    while (permute_eventq_peek(&k->eventq, &e) == 0 &&
           e.deadline_ns <= k->now_ns) {
        (void)permute_eventq_pop(&k->eventq, &e);
        (void)permute_timeout_fire(k, &e);
    }

    return anything_runnable(k) ? PERMUTE_SIM_RUNNABLE : PERMUTE_SIM_IDLE;
}

permute_sim_outcome_t permute_sim_run(permute_kernel_t *k)
{
    // Each iteration fires at least one entry, so the bound is the entry
    // count; it exists to make a bad heap terminate rather than hang.
    for (uint32_t i = 0; i <= PERMUTE_MAX_TIMERS; i++) {
        permute_sim_outcome_t o = permute_sim_advance(k);
        if (o != PERMUTE_SIM_IDLE)
            return o;
    }
    return PERMUTE_SIM_DEADLOCK;
}
