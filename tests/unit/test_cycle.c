// SPDX-License-Identifier: Apache-2.0
//
// The live detector's only job: given blocked threads, find the reply cycle.
// The post-hoc oracle is tested separately, against record streams.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "kernel/objects.h"
#include "kernel/oracle.h"

static void two_thread_reply_cycle(permute_kernel_t *k,
                                   permute_handle_t *t1_out,
                                   permute_handle_t *t2_out)
{
    permute_handle_t process;
    CHECK_EQ(permute_process_create(k, "cycle", &process), 0);

    permute_handle_t t1, t2;
    CHECK_EQ(permute_thread_create(k, process, 10, PERMUTE_SCHED_FIFO, &t1), 0);
    CHECK_EQ(permute_thread_create(k, process, 10, PERMUTE_SCHED_FIFO, &t2), 0);

    permute_thread_t *a = permute_thread_get(k, t1);
    permute_thread_t *b = permute_thread_get(k, t2);
    CHECK(a != NULL && b != NULL);

    permute_handle_t m1, m2;
    CHECK_EQ(permute_message_create(k, process, a->tid, b->pid, 10, "a", 1,
                                    &m1), 0);
    CHECK_EQ(permute_message_create(k, process, b->tid, a->pid, 10, "b", 1,
                                    &m2), 0);

    permute_message_t *ma = permute_message_get(k, m1);
    permute_message_t *mb = permute_message_get(k, m2);
    CHECK(ma != NULL && mb != NULL);

    // a sent ma to b and is waiting for b's reply; b sent mb to a and is
    // waiting for a's reply.
    ma->receiver_tid = b->tid;
    mb->receiver_tid = a->tid;

    a->state = PERMUTE_THREAD_REPLY;
    a->wmsg = m1;
    a->wrcvid = ma->rcvid;

    b->state = PERMUTE_THREAD_REPLY;
    b->wmsg = m2;
    b->wrcvid = mb->rcvid;

    *t1_out = t1;
    *t2_out = t2;
}

static void test_reply_cycle(void)
{
    permute_kernel_t k;
    permute_kernel_init(&k, 7);

    permute_handle_t t1, t2;
    two_thread_reply_cycle(&k, &t1, &t2);

    permute_thread_t *a = permute_thread_get(&k, t1);
    permute_thread_t *b = permute_thread_get(&k, t2);
    CHECK(a != NULL && b != NULL);

    permute_oracle_cycle_t cycle;
    CHECK_EQ(permute_oracle_deadlock(&k, &cycle), 1);
    CHECK_EQ(cycle.len, 2u);
    CHECK_EQ(cycle.tid[0], a->tid);
    CHECK_EQ(cycle.tid[1], b->tid);
    CHECK_EQ(cycle.state[0], PERMUTE_THREAD_REPLY);
    CHECK_EQ(cycle.rcvid[0], a->wrcvid);
    CHECK_EQ(cycle.rcvid[1], b->wrcvid);

    char text[256];
    permute_oracle_format_cycle(&cycle, text, sizeof text);
    CHECK(strstr(text, "(REPLY ") != NULL);
    CHECK(strstr(text, " -> ") != NULL);

    // The rendering closes the loop with the first vertex.
    char first[32], last[32];
    snprintf(first, sizeof first, "T%u(", a->tid);
    snprintf(last, sizeof last, " -> T%u(", a->tid);
    CHECK(strncmp(text, first, strlen(first)) == 0);
    CHECK(strstr(text, last) != NULL);

    permute_kernel_destroy(&k);
}

static void test_no_cycle_when_one_thread_runs(void)
{
    permute_kernel_t k;
    permute_kernel_init(&k, 8);

    permute_handle_t t1, t2;
    two_thread_reply_cycle(&k, &t1, &t2);

    permute_thread_t *b = permute_thread_get(&k, t2);
    CHECK(b != NULL);
    b->state = PERMUTE_THREAD_READY;

    permute_oracle_cycle_t cycle;
    CHECK_EQ(permute_oracle_deadlock(&k, &cycle), 0);
    CHECK_EQ(cycle.len, 0u);

    permute_kernel_destroy(&k);
}

static void test_join_cycle(void)
{
    permute_kernel_t k;
    permute_kernel_init(&k, 9);

    permute_handle_t process;
    CHECK_EQ(permute_process_create(&k, "joiners", &process), 0);

    permute_handle_t t1, t2;
    CHECK_EQ(permute_thread_create(&k, process, 10, PERMUTE_SCHED_FIFO, &t1), 0);
    CHECK_EQ(permute_thread_create(&k, process, 10, PERMUTE_SCHED_FIFO, &t2), 0);

    permute_thread_t *a = permute_thread_get(&k, t1);
    permute_thread_t *b = permute_thread_get(&k, t2);
    CHECK(a != NULL && b != NULL);

    a->state = PERMUTE_THREAD_JOIN;
    a->wobj = t2;
    b->state = PERMUTE_THREAD_JOIN;
    b->wobj = t1;

    permute_oracle_cycle_t cycle;
    CHECK_EQ(permute_oracle_deadlock(&k, &cycle), 1);
    CHECK_EQ(cycle.len, 2u);
    CHECK_EQ(cycle.state[0], PERMUTE_THREAD_JOIN);

    char text[256];
    permute_oracle_format_cycle(&cycle, text, sizeof text);
    CHECK(strstr(text, "(JOIN)") != NULL);

    permute_kernel_destroy(&k);
}

int main(void)
{
    test_reply_cycle();
    test_no_cycle_when_one_thread_runs();
    test_join_cycle();

    return TEST_RESULT("test_cycle");
}
