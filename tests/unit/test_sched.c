// SPDX-License-Identifier: Apache-2.0
//
// P1-04 unit tests: the 256-bucket ready queue, its bitmap invariant, FIFO
// within a priority, the three policies, the RR quantum, and SchedYield.
//
// The queue is tested against a shadow model as well as its own invariant:
// the invariant catches a corrupted bitmap, but only the model catches a pop
// that returns the wrong thread.

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "kernel/objects.h"
#include "kernel/readyq.h"
#include "kernel/sched.h"

static permute_kernel_t g_kernel;

// ---------------------------------------------------------------------------
// Ready queue

// Independent oracle for one queue entry. Head insertion needs an ordering
// key that sorts before every existing equal-priority entry, so the two
// counters move in opposite directions.
typedef struct {
    int64_t seq;
    uint16_t prio;
    uint8_t on;
} model_entry_t;

static int model_best(const model_entry_t *m)
{
    int best = -1;
    for (int i = 0; i < PERMUTE_MAX_THREADS; i++) {
        if (!m[i].on)
            continue;
        if (best < 0 || m[i].prio > m[best].prio ||
            (m[i].prio == m[best].prio && m[i].seq < m[best].seq))
            best = i;
    }
    return best;
}

static void test_readyq_against_model(void)
{
    static permute_readyq_t q;
    static model_entry_t model[PERMUTE_MAX_THREADS];

    permute_readyq_init(&q);
    memset(model, 0, sizeof model);

    uint64_t s = 0x9E3779B97F4A7C15ull;
    int64_t tail_seq = 0, head_seq = 0;
    uint32_t live = 0;
    int failures = 0;

    for (int i = 0; i < 1000000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t r = (uint32_t)(s >> 33);
        uint32_t slot = r % PERMUTE_MAX_THREADS;
        uint32_t prio = (r >> 8) % PERMUTE_PRIO_BUCKETS;

        switch (r & 3u) {
        case 0: { // pop: must be the model's max priority, earliest sequence
            uint32_t got = 0;
            int rc = permute_readyq_pop(&q, &got);
            int best = model_best(model);
            if (best < 0) {
                if (rc == 0)
                    failures++;
            } else if (rc != 0 || got != (uint32_t)best) {
                failures++;
            } else {
                model[best].on = 0;
                live--;
            }
            break;
        }
        case 1: { // push at tail
            int rc = permute_readyq_push(&q, slot, prio, 0);
            if (model[slot].on) {
                if (rc != -EEXIST)
                    failures++;
            } else if (rc != 0) {
                failures++;
            } else {
                model[slot].on = 1;
                model[slot].prio = (uint16_t)prio;
                model[slot].seq = ++tail_seq;
                live++;
            }
            break;
        }
        case 2: { // push at head
            int rc = permute_readyq_push(&q, slot, prio, 1);
            if (model[slot].on) {
                if (rc != -EEXIST)
                    failures++;
            } else if (rc != 0) {
                failures++;
            } else {
                model[slot].on = 1;
                model[slot].prio = (uint16_t)prio;
                model[slot].seq = --head_seq;
                live++;
            }
            break;
        }
        default: { // remove
            int rc = permute_readyq_remove(&q, slot);
            if (!model[slot].on) {
                if (rc != -ENOENT)
                    failures++;
            } else if (rc != 0) {
                failures++;
            } else {
                model[slot].on = 0;
                live--;
            }
            break;
        }
        }

        if (failures != 0)
            break;

        if ((i & 1023) == 0) {
            if (permute_readyq_check(&q) != 0) {
                failures++;
                break;
            }
            if (permute_readyq_count(&q) != live) {
                failures++;
                break;
            }
        }
    }

    CHECK_EQ(failures, 0);
    CHECK_EQ(permute_readyq_check(&q), 0);
    CHECK_EQ(permute_readyq_count(&q), live);
}

static void test_readyq_fifo_and_order(void)
{
    permute_readyq_t q;
    permute_readyq_init(&q);

    CHECK_EQ(permute_readyq_push(&q, 3, 10, 0), 0);
    CHECK_EQ(permute_readyq_push(&q, 7, 10, 0), 0);
    CHECK_EQ(permute_readyq_push(&q, 9, 10, 0), 0);

    uint32_t slot = 0;
    CHECK_EQ(permute_readyq_pop(&q, &slot), 0);
    CHECK_EQ(slot, 3u);
    CHECK_EQ(permute_readyq_pop(&q, &slot), 0);
    CHECK_EQ(slot, 7u);
    CHECK_EQ(permute_readyq_pop(&q, &slot), 0);
    CHECK_EQ(slot, 9u);
    CHECK_EQ(permute_readyq_pop(&q, &slot), -EAGAIN);

    // Priority wins over arrival order.
    CHECK_EQ(permute_readyq_push(&q, 1, 10, 0), 0);
    CHECK_EQ(permute_readyq_push(&q, 2, 30, 0), 0);
    CHECK_EQ(permute_readyq_push(&q, 3, 20, 0), 0);
    CHECK_EQ(permute_readyq_pop(&q, &slot), 0);
    CHECK_EQ(slot, 2u);
    CHECK_EQ(permute_readyq_pop(&q, &slot), 0);
    CHECK_EQ(slot, 3u);
    CHECK_EQ(permute_readyq_pop(&q, &slot), 0);
    CHECK_EQ(slot, 1u);

    // Head insertion jumps equal-priority peers.
    CHECK_EQ(permute_readyq_push(&q, 4, 10, 0), 0);
    CHECK_EQ(permute_readyq_push(&q, 5, 10, 1), 0);
    CHECK_EQ(permute_readyq_pop(&q, &slot), 0);
    CHECK_EQ(slot, 5u);
    CHECK_EQ(permute_readyq_pop(&q, &slot), 0);
    CHECK_EQ(slot, 4u);

    // Remove from the middle leaves the rest linked.
    CHECK_EQ(permute_readyq_push(&q, 6, 10, 0), 0);
    CHECK_EQ(permute_readyq_push(&q, 7, 10, 0), 0);
    CHECK_EQ(permute_readyq_push(&q, 8, 10, 0), 0);
    CHECK_EQ(permute_readyq_remove(&q, 7), 0);
    CHECK_EQ(permute_readyq_check(&q), 0);
    CHECK_EQ(permute_readyq_pop(&q, &slot), 0);
    CHECK_EQ(slot, 6u);
    CHECK_EQ(permute_readyq_pop(&q, &slot), 0);
    CHECK_EQ(slot, 8u);

    // Boundary conditions.
    CHECK_EQ(permute_readyq_push(&q, 0, PERMUTE_PRIO_BUCKETS, 0), -EINVAL);
    CHECK_EQ(permute_readyq_push(&q, PERMUTE_MAX_THREADS, 0, 0), -EINVAL);
    CHECK_EQ(permute_readyq_remove(&q, 0), -ENOENT);
    CHECK_EQ(permute_readyq_check(&q), 0);
}

// ---------------------------------------------------------------------------
// Scheduler

typedef struct {
    permute_handle_t proc;
    permute_handle_t lo, mid, hi;
} sched_fixture_t;

static sched_fixture_t g_s;

static void sched_setup(permute_policy_t policy)
{
    permute_kernel_init(&g_kernel, 7);
    CHECK_EQ(permute_process_create(&g_kernel, "p", &g_s.proc), 0);
    CHECK_EQ(permute_thread_create(&g_kernel, g_s.proc, 10, policy, &g_s.lo), 0);
    CHECK_EQ(permute_thread_create(&g_kernel, g_s.proc, 20, policy, &g_s.mid), 0);
    CHECK_EQ(permute_thread_create(&g_kernel, g_s.proc, 30, policy, &g_s.hi), 0);
}

static void test_sched_pick_order(void)
{
    sched_setup(PERMUTE_SCHED_FIFO);

    CHECK_EQ(permute_sched_check(&g_kernel), 0);
    CHECK_EQ(permute_readyq_count(&g_kernel.readyq), 3);

    CHECK_EQ(permute_sched_pick(&g_kernel), g_s.hi);
    CHECK_EQ(permute_thread_get(&g_kernel, g_s.hi)->state,
             PERMUTE_THREAD_RUNNING);
    CHECK_EQ(permute_sched_check(&g_kernel), 0); // running is not queued

    CHECK_EQ(permute_sched_pick(&g_kernel), g_s.mid);
    CHECK_EQ(permute_sched_pick(&g_kernel), g_s.lo);
    CHECK_EQ(permute_sched_pick(&g_kernel), PERMUTE_HANDLE_NONE);
    CHECK_EQ(permute_sched_running(&g_kernel), PERMUTE_HANDLE_NONE);
}

static void test_rr_alternation(void)
{
    permute_kernel_init(&g_kernel, 8);
    permute_handle_t proc, a, b;
    CHECK_EQ(permute_process_create(&g_kernel, "p", &proc), 0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_RR, &a),
             0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_RR, &b),
             0);

    CHECK_EQ(permute_sched_pick(&g_kernel), a);

    // Three ticks leave a quarter of the slice; the fourth expires it.
    CHECK_EQ(permute_sched_tick(&g_kernel, 3 * PERMUTE_TICK_NS), a);
    CHECK_EQ(permute_sched_tick(&g_kernel, PERMUTE_TICK_NS), b);
    CHECK_EQ(permute_sched_tick(&g_kernel, 4 * PERMUTE_TICK_NS), a);
    CHECK_EQ(permute_sched_tick(&g_kernel, 4 * PERMUTE_TICK_NS), b);
    CHECK_EQ(permute_sched_check(&g_kernel), 0);
}

static void test_rr_other_is_rr(void)
{
    permute_kernel_init(&g_kernel, 9);
    permute_handle_t proc, a, b;
    CHECK_EQ(permute_process_create(&g_kernel, "p", &proc), 0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_OTHER, &a),
             0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_OTHER, &b),
             0);

    CHECK_EQ(permute_sched_pick(&g_kernel), a);
    CHECK_EQ(permute_sched_tick(&g_kernel, 4 * PERMUTE_TICK_NS), b);
}

static void test_rr_alone_does_not_switch(void)
{
    permute_kernel_init(&g_kernel, 10);
    permute_handle_t proc, a;
    CHECK_EQ(permute_process_create(&g_kernel, "p", &proc), 0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_RR, &a),
             0);

    CHECK_EQ(permute_sched_pick(&g_kernel), a);
    for (int i = 0; i < 50; i++)
        CHECK_EQ(permute_sched_tick(&g_kernel, 4 * PERMUTE_TICK_NS), a);
    CHECK_EQ(permute_thread_get(&g_kernel, a)->cpu_ns,
             50ull * 4ull * PERMUTE_TICK_NS);
}

static void test_fifo_never_expires(void)
{
    permute_kernel_init(&g_kernel, 11);
    permute_handle_t proc, a, b;
    CHECK_EQ(permute_process_create(&g_kernel, "p", &proc), 0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 20, PERMUTE_SCHED_FIFO, &a),
             0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_RR, &b),
             0);

    CHECK_EQ(permute_sched_pick(&g_kernel), a);
    for (int i = 0; i < 100; i++)
        CHECK_EQ(permute_sched_tick(&g_kernel, 4 * PERMUTE_TICK_NS), a);
}

static void test_yield_lower_priority_does_not_switch(void)
{
    sched_setup(PERMUTE_SCHED_FIFO);

    // Highest priority runs; two lower-priority threads are ready.
    CHECK_EQ(permute_sched_pick(&g_kernel), g_s.hi);
    CHECK_EQ(permute_sched_yield(&g_kernel, g_s.hi), g_s.hi);

    // FR-3.19: no switch, and the running thread stayed runnable.
    permute_thread_t *t = permute_thread_get(&g_kernel, g_s.hi);
    CHECK_EQ(t->state, PERMUTE_THREAD_RUNNING);
    CHECK_EQ(permute_sched_running(&g_kernel), g_s.hi);
}

static void test_yield_same_priority_switches(void)
{
    permute_kernel_init(&g_kernel, 12);
    permute_handle_t proc, a, b;
    CHECK_EQ(permute_process_create(&g_kernel, "p", &proc), 0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_FIFO, &a),
             0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_FIFO, &b),
             0);

    CHECK_EQ(permute_sched_pick(&g_kernel), a);
    CHECK_EQ(permute_sched_yield(&g_kernel, a), b);
    CHECK_EQ(permute_thread_get(&g_kernel, a)->state, PERMUTE_THREAD_READY);
    CHECK_EQ(permute_thread_get(&g_kernel, b)->state, PERMUTE_THREAD_RUNNING);

    CHECK_EQ(permute_sched_yield(&g_kernel, b), a);
}

static void test_yield_alone_is_noop(void)
{
    permute_kernel_init(&g_kernel, 13);
    permute_handle_t proc, a;
    CHECK_EQ(permute_process_create(&g_kernel, "p", &proc), 0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_FIFO, &a),
             0);

    CHECK_EQ(permute_sched_pick(&g_kernel), a);
    CHECK_EQ(permute_sched_yield(&g_kernel, a), a);
    CHECK_EQ(permute_thread_get(&g_kernel, a)->state, PERMUTE_THREAD_RUNNING);
}

static void test_destroy_removes_from_queue(void)
{
    sched_setup(PERMUTE_SCHED_FIFO);

    CHECK_EQ(permute_readyq_count(&g_kernel.readyq), 3);
    CHECK_EQ(permute_thread_destroy(&g_kernel, g_s.mid), 0);
    CHECK_EQ(permute_readyq_count(&g_kernel.readyq), 2);
    CHECK_EQ(permute_sched_check(&g_kernel), 0);

    CHECK_EQ(permute_sched_pick(&g_kernel), g_s.hi);
    CHECK_EQ(permute_sched_pick(&g_kernel), g_s.lo);
    CHECK_EQ(permute_sched_pick(&g_kernel), PERMUTE_HANDLE_NONE);
}

static void test_set_prio_moves_bucket(void)
{
    permute_kernel_init(&g_kernel, 14);
    permute_handle_t proc, a, b;
    CHECK_EQ(permute_process_create(&g_kernel, "p", &proc), 0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_FIFO, &a),
             0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 20, PERMUTE_SCHED_FIFO, &b),
             0);

    CHECK_EQ(permute_thread_set_prio(&g_kernel, a, 30), 0);
    CHECK_EQ(permute_sched_check(&g_kernel), 0);
    CHECK_EQ(permute_sched_pick(&g_kernel), a);

    // At-head placement, on a clean queue: after an explicit priority change,
    // the thread is ahead of a peer it previously sat behind.
    permute_kernel_init(&g_kernel, 15);
    CHECK_EQ(permute_process_create(&g_kernel, "p", &proc), 0);

    permute_handle_t c, d;
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_FIFO, &c),
             0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_FIFO, &d),
             0);
    CHECK_EQ(permute_thread_set_prio(&g_kernel, d, 10), 0);
    CHECK_EQ(permute_sched_check(&g_kernel), 0);
    CHECK_EQ(permute_sched_pick(&g_kernel), d);
    CHECK_EQ(permute_sched_pick(&g_kernel), c);
}

static void test_destroy_running_thread_clears_running(void)
{
    permute_kernel_init(&g_kernel, 15);
    permute_handle_t proc, a;
    CHECK_EQ(permute_process_create(&g_kernel, "p", &proc), 0);
    CHECK_EQ(permute_thread_create(&g_kernel, proc, 10, PERMUTE_SCHED_FIFO, &a),
             0);

    CHECK_EQ(permute_sched_pick(&g_kernel), a);
    CHECK_EQ(permute_thread_destroy(&g_kernel, a), 0);
    CHECK_EQ(permute_sched_running(&g_kernel), PERMUTE_HANDLE_NONE);
    CHECK_EQ(permute_sched_check(&g_kernel), 0);
}

// ---------------------------------------------------------------------------

int main(void)
{
    test_readyq_against_model();
    test_readyq_fifo_and_order();
    test_sched_pick_order();
    test_rr_alternation();
    test_rr_other_is_rr();
    test_rr_alone_does_not_switch();
    test_fifo_never_expires();
    test_yield_lower_priority_does_not_switch();
    test_yield_same_priority_switches();
    test_yield_alone_is_noop();
    test_destroy_removes_from_queue();
    test_set_prio_moves_bucket();
    test_destroy_running_thread_clears_running();

    return TEST_RESULT("test_sched");
}
