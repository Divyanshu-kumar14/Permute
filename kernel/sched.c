// SPDX-License-Identifier: Apache-2.0

#include "kernel/sched.h"

#include <errno.h>

#include "kernel/objects.h"
#include "kernel/readyq.h"

static uint32_t bucket_of(const permute_thread_t *t)
{
    int32_t p = t->eff_prio;
    if (p < 0)
        p = 0;
    if (p > PERMUTE_PRIO_BUCKETS - 1)
        p = PERMUTE_PRIO_BUCKETS - 1;
    return (uint32_t)p;
}

void permute_sched_init(permute_kernel_t *k)
{
    permute_readyq_init(&k->readyq);
    k->running = PERMUTE_HANDLE_NONE;
}

int permute_sched_add(permute_kernel_t *k, permute_handle_t h, int at_head)
{
    permute_thread_t *t = permute_thread_get(k, h);
    if (t == NULL)
        return -ESRCH;
    return permute_readyq_push(&k->readyq, PERMUTE_HANDLE_SLOT(h),
                               bucket_of(t), at_head);
}

int permute_sched_remove(permute_kernel_t *k, permute_handle_t h)
{
    if (h == PERMUTE_HANDLE_NONE)
        return -EINVAL;
    return permute_readyq_remove(&k->readyq, PERMUTE_HANDLE_SLOT(h));
}

int permute_sched_move(permute_kernel_t *k, permute_handle_t h, int at_head)
{
    permute_thread_t *t = permute_thread_get(k, h);
    if (t == NULL)
        return -ESRCH;

    // Not queued: running or blocked. The caller's priority change still took
    // effect on eff_prio; there is simply nothing to reorder.
    if (permute_readyq_remove(&k->readyq, PERMUTE_HANDLE_SLOT(h)) != 0)
        return 0;

    return permute_sched_add(k, h, at_head);
}

permute_handle_t permute_sched_pick(permute_kernel_t *k)
{
    for (;;) {
        uint32_t slot;
        if (permute_readyq_pop(&k->readyq, &slot) != 0) {
            k->running = PERMUTE_HANDLE_NONE;
            return PERMUTE_HANDLE_NONE;
        }

        // A slot whose generation moved on cannot be the thread that was
        // queued. Skipping it is cheaper than trusting teardown to be perfect.
        permute_handle_t h = PERMUTE_HANDLE_MAKE(slot, k->threads.generation[slot]);
        permute_thread_t *t = permute_thread_get(k, h);
        if (t == NULL)
            continue;

        t->state = PERMUTE_THREAD_RUNNING;
        t->quantum_left_ns = t->quantum_ns;
        k->running = h;
        return h;
    }
}

permute_handle_t permute_sched_tick(permute_kernel_t *k, uint64_t dt_ns)
{
    permute_thread_t *t = permute_thread_get(k, k->running);
    if (t == NULL || t->state != PERMUTE_THREAD_RUNNING)
        return k->running;

    t->cpu_ns += dt_ns;
    t->quantum_left_ns = (dt_ns < t->quantum_left_ns)
                             ? t->quantum_left_ns - dt_ns
                             : 0;

    // FIFO never loses the CPU on time. SCHED_OTHER is an alias for RR
    // (FR-3.12), so only FIFO is exempt.
    if (t->policy == PERMUTE_SCHED_FIFO)
        return k->running;
    if (t->quantum_left_ns > 0)
        return k->running;

    permute_handle_t self = k->running;
    k->running = PERMUTE_HANDLE_NONE;
    t->state = PERMUTE_THREAD_READY;
    t->quantum_left_ns = t->quantum_ns;

    if (permute_sched_add(k, self, 0) != 0) {
        // Cannot happen while the thread exists, but a silent lost thread
        // would hang the run; keep it running instead.
        t->state = PERMUTE_THREAD_RUNNING;
        k->running = self;
        return self;
    }

    // If no equal-priority peer is waiting, pick returns this same thread and
    // the quantum simply restarts.
    return permute_sched_pick(k);
}

permute_handle_t permute_sched_yield(permute_kernel_t *k, permute_handle_t self)
{
    permute_thread_t *t = permute_thread_get(k, self);
    if (t == NULL || t->state != PERMUTE_THREAD_RUNNING || k->running != self)
        return k->running;

    k->running = PERMUTE_HANDLE_NONE;
    t->state = PERMUTE_THREAD_READY;

    if (permute_sched_add(k, self, 0) != 0) {
        t->state = PERMUTE_THREAD_RUNNING;
        k->running = self;
        return self;
    }

    // A higher-priority thread wins (strict priority), an equal-priority peer
    // wins because self is now at the tail, and a lower-priority thread can
    // never win because the pick starts at the highest non-empty bucket -
    // which is self's. That is FR-3.19 without a special case.
    return permute_sched_pick(k);
}

permute_handle_t permute_sched_running(const permute_kernel_t *k)
{
    return k->running;
}

int permute_sched_check(const permute_kernel_t *k)
{
    if (permute_readyq_check(&k->readyq) != 0)
        return -1;

    if (k->running != PERMUTE_HANDLE_NONE &&
        permute_readyq_contains(&k->readyq, PERMUTE_HANDLE_SLOT(k->running)))
        return -1; // running and queued at once

    return 0;
}
