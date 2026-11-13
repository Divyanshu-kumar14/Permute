// SPDX-License-Identifier: Apache-2.0

#include "kernel/pi.h"

#include <errno.h>
#include <string.h>

#include "kernel/objects.h"
#include "kernel/sched.h"

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

int32_t permute_pi_clamp(permute_kernel_t *k, const permute_thread_t *t,
                         int32_t prio)
{
    if (prio < 0)
        prio = 0;

    permute_process_t *p = permute_process_get(k, t->process);
    if (p == NULL)
        return prio;
    if (p->abilities & PERMUTE_AID_PRIORITY)
        return prio;
    if (prio > p->prio_ceiling)
        return p->prio_ceiling;
    return prio;
}

// ---------------------------------------------------------------------------
// Message-driven inheritance

void permute_pi_on_send(permute_kernel_t *k, permute_handle_t channel,
                        permute_thread_t *receiver, int32_t sender_prio)
{
    permute_channel_t *ch = permute_channel_get(k, channel);
    if (ch == NULL || (ch->flags & PERMUTE_CHF_FIXED_PRIORITY))
        return;

    int32_t p = permute_pi_clamp(k, receiver, sender_prio);
    if (p <= receiver->eff_prio)
        return;

    receiver->sticky_prio = p;
    permute_thread_recompute_prio(receiver);
    permute_sched_move(k, receiver->self, 1);
}

void permute_pi_on_receive(permute_kernel_t *k, permute_handle_t channel,
                           permute_thread_t *receiver, int32_t item_prio)
{
    permute_channel_t *ch = permute_channel_get(k, channel);
    if (ch == NULL || (ch->flags & PERMUTE_CHF_FIXED_PRIORITY))
        return;

    // FR-3.14: the maximum over everything still pending on the channel plus
    // the item just dequeued. The queue is priority-ordered, so the dequeued
    // item is normally the maximum; computing the maximum anyway keeps the
    // rule correct if ordering ever changes underneath it.
    int32_t max = item_prio;
    for (uint32_t i = 0; i < ch->recvq.length; i++) {
        const permute_queue_entry_t *e = &ch->recvq.entries[i];
        if ((e->kind & (PERMUTE_Q_MESSAGE | PERMUTE_Q_PULSE)) == 0)
            continue;
        if (e->priority > max)
            max = e->priority;
    }

    // Assignment, not a raise: a lower-priority sender lowers the inherited
    // value. The effective priority still cannot fall below base (PRD 9.1).
    receiver->sticky_prio = permute_pi_clamp(k, receiver, max);
    permute_thread_recompute_prio(receiver);
    permute_sched_move(k, receiver->self, 1);
}

// ---------------------------------------------------------------------------
// Mutex objects

permute_mutex_t *permute_mutex_get(permute_kernel_t *k, permute_handle_t h)
{
    if (!permute_table_valid(&k->mutexes, h))
        return NULL;
    return &k->mutex_pool[PERMUTE_HANDLE_SLOT(h)];
}

int permute_mutex_create(permute_kernel_t *k, permute_mutex_proto_t proto,
                         int32_t ceiling, int recursive,
                         permute_handle_t *out)
{
    if (proto != PERMUTE_MUTEX_NORMAL && proto != PERMUTE_MUTEX_INHERIT &&
        proto != PERMUTE_MUTEX_PROTECT)
        return -EINVAL;
    if (ceiling < 0 || ceiling > PERMUTE_PRIO_BUCKETS - 1)
        return -EINVAL;

    uint32_t slot, gen;
    int rc = permute_table_alloc(&k->mutexes, &slot, &gen);
    if (rc != 0)
        return rc;

    permute_mutex_t *m = &k->mutex_pool[slot];
    memset(m, 0, sizeof *m);
    m->owner = PERMUTE_HANDLE_NONE;
    m->proto = proto;
    m->ceiling = ceiling;
    m->recursive = recursive ? 1u : 0u;
    permute_queue_init(&m->waiters);

    *out = PERMUTE_HANDLE_MAKE(slot, gen);
    return 0;
}

int permute_mutex_destroy(permute_kernel_t *k, permute_handle_t h)
{
    permute_mutex_t *m = permute_mutex_get(k, h);
    if (m == NULL)
        return -ESRCH;
    if (m->owner != PERMUTE_HANDLE_NONE)
        return -EBUSY;

    permute_table_free(&k->mutexes, PERMUTE_HANDLE_SLOT(h));
    return 0;
}

// Raise one thread's mutex contribution, then follow the wait chain upwards:
// if the owner is itself blocked on a mutex, that mutex's owner inherits the
// same priority ("directly or indirectly" in the vendor documentation).
static void boost_apply(permute_kernel_t *k, permute_thread_t *t, int32_t prio)
{
    int32_t p = permute_pi_clamp(k, t, prio);
    if (p <= t->mutex_boost)
        return;

    t->mutex_boost = p;
    permute_thread_recompute_prio(t);
    permute_sched_move(k, t->self, 1);
}

static void boost_chain(permute_kernel_t *k, permute_thread_t *owner,
                        int32_t prio, uint32_t depth)
{
    if (owner == NULL || depth > PERMUTE_MAX_THREADS)
        return;

    boost_apply(k, owner, prio);

    if (owner->state != PERMUTE_THREAD_MUTEX)
        return;

    permute_mutex_t *m = permute_mutex_get(k, owner->wobj);
    if (m == NULL || m->owner == PERMUTE_HANDLE_NONE || m->owner == owner->self)
        return;

    permute_thread_t *next = permute_thread_get(k, m->owner);
    boost_chain(k, next, prio, depth + 1);
}

// Recompute the reverting contribution from the waiters this thread still
// blocks. Unlock may therefore lower the thread and requeue it at the tail
// (FR-3.18).
static void recompute_boost(permute_kernel_t *k, permute_thread_t *t)
{
    int32_t max = 0;

    for (uint32_t i = 0; i < k->mutexes.capacity; i++) {
        if (!k->mutexes.in_use[i])
            continue;
        const permute_mutex_t *m = &k->mutex_pool[i];
        if (m->owner != t->self || m->proto != PERMUTE_MUTEX_INHERIT)
            continue;

        for (uint32_t j = 0; j < m->waiters.length; j++) {
            const permute_thread_t *w =
                permute_thread_get(k, m->waiters.entries[j].handle);
            if (w != NULL && w->eff_prio > max)
                max = w->eff_prio;
        }
    }

    t->mutex_boost = permute_pi_clamp(k, t, max);
    permute_thread_recompute_prio(t);
    permute_sched_move(k, t->self, 0);
}

// PROTECT contribution: the highest ceiling among the protect mutexes this
// thread currently holds. Recomputed from the mutex table rather than kept as
// a per-thread set, so it cannot drift.
static void recompute_protect(permute_kernel_t *k, permute_thread_t *t)
{
    int32_t max = 0;

    for (uint32_t i = 0; i < k->mutexes.capacity; i++) {
        if (!k->mutexes.in_use[i])
            continue;
        const permute_mutex_t *m = &k->mutex_pool[i];
        if (m->owner == t->self && m->proto == PERMUTE_MUTEX_PROTECT &&
            m->ceiling > max)
            max = m->ceiling;
    }

    t->prot_ceil = max;
    permute_thread_recompute_prio(t);
    permute_sched_move(k, t->self, 0);
}

// ---------------------------------------------------------------------------
// Lock and unlock

permute_action_t permute_mutex_lock(permute_kernel_t *k, permute_handle_t thread,
                                    permute_handle_t mutex,
                                    permute_result_t *res)
{
    memset(res, 0, sizeof *res);

    permute_thread_t *t = permute_thread_get(k, thread);
    permute_mutex_t *m = permute_mutex_get(k, mutex);
    if (t == NULL || m == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }

    // A priority-protocol mutex refuses a locker that is already above its
    // ceiling; the vendor documentation says the lock fails rather than
    // silently capping the thread.
    if (m->proto == PERMUTE_MUTEX_PROTECT && t->base_prio > m->ceiling) {
        result_error(res, EINVAL);
        return PERMUTE_CONTINUE;
    }

    if (m->owner == PERMUTE_HANDLE_NONE) {
        m->owner = thread;
        m->owner_pid = t->pid;
        m->lock_count = 1;

        if (m->proto == PERMUTE_MUTEX_PROTECT) {
            if (m->ceiling > t->prot_ceil)
                t->prot_ceil = m->ceiling;
            permute_thread_recompute_prio(t);
        }

        res->action = PERMUTE_CONTINUE;
        res->ret = 0;
        return PERMUTE_CONTINUE;
    }

    if (m->owner == thread) {
        if (!m->recursive) {
            result_error(res, EDEADLK);
            return PERMUTE_CONTINUE;
        }
        m->lock_count++;
        res->action = PERMUTE_CONTINUE;
        res->ret = 0;
        return PERMUTE_CONTINUE;
    }

    t->state = PERMUTE_THREAD_MUTEX;
    t->wobj = mutex;

    int rc = permute_queue_insert(&m->waiters, thread, t->eff_prio,
                                  k->sequence++, PERMUTE_Q_THREAD);
    if (rc != 0) {
        t->state = PERMUTE_THREAD_RUNNING;
        t->wobj = PERMUTE_HANDLE_NONE;
        result_error(res, EAGAIN);
        return PERMUTE_CONTINUE;
    }

    if (m->proto == PERMUTE_MUTEX_INHERIT)
        boost_chain(k, permute_thread_get(k, m->owner), t->eff_prio, 0);

    res->action = PERMUTE_BLOCKED;
    return PERMUTE_BLOCKED;
}

permute_action_t permute_mutex_unlock(permute_kernel_t *k,
                                      permute_handle_t thread,
                                      permute_handle_t mutex,
                                      permute_result_t *res)
{
    memset(res, 0, sizeof *res);

    permute_thread_t *t = permute_thread_get(k, thread);
    permute_mutex_t *m = permute_mutex_get(k, mutex);
    if (t == NULL || m == NULL) {
        result_error(res, ESRCH);
        return PERMUTE_CONTINUE;
    }
    if (m->owner != thread || m->lock_count == 0) {
        result_error(res, EPERM);
        return PERMUTE_CONTINUE;
    }

    if (--m->lock_count > 0) {
        res->action = PERMUTE_CONTINUE;
        res->ret = 0;
        return PERMUTE_CONTINUE;
    }

    m->owner = PERMUTE_HANDLE_NONE;
    m->owner_pid = 0;

    // The unlocking thread may drop. Recompute both reverting contributions
    // before handing the lock over.
    if (m->proto == PERMUTE_MUTEX_PROTECT)
        recompute_protect(k, t);
    recompute_boost(k, t);

    // Direct handoff to the highest-priority waiter. A waiter whose thread
    // died is skipped rather than handed a lock it cannot use.
    permute_queue_entry_t e;
    while (permute_queue_pop_matching(&m->waiters, PERMUTE_Q_THREAD, &e) == 0) {
        permute_thread_t *w = permute_thread_get(k, e.handle);
        if (w == NULL)
            continue;

        m->owner = e.handle;
        m->owner_pid = w->pid;
        m->lock_count = 1;

        w->state = PERMUTE_THREAD_READY;
        w->wobj = PERMUTE_HANDLE_NONE;
        w->pending.action = PERMUTE_CONTINUE;
        w->pending.ret = 0;
        w->pending.err = 0;
        w->pending.rcvid = 0;
        w->pending.bytes = 0;

        if (m->proto == PERMUTE_MUTEX_PROTECT) {
            if (m->ceiling > w->prot_ceil)
                w->prot_ceil = m->ceiling;
            permute_thread_recompute_prio(w);
        }
        recompute_boost(k, w);
        break;
    }

    res->action = PERMUTE_CONTINUE;
    res->ret = 0;
    return PERMUTE_CONTINUE;
}
