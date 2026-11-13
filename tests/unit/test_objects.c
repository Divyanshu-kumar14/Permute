// SPDX-License-Identifier: Apache-2.0
//
// P1-01 unit tests: handle tables, object lifecycle, rcvid encoding, payload
// copy, effective priority, queue.

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "kernel/objects.h"

static permute_kernel_t g_kernel;

// ---------------------------------------------------------------------------
// Handle tables

static void test_table_generations(void)
{
    permute_table_t t;
    permute_table_init(&t, 4);

    uint32_t slot = 0, gen = 0;
    CHECK_EQ(permute_table_alloc(&t, &slot, &gen), 0);
    CHECK_EQ(slot, 0);
    CHECK_EQ(gen, 0);

    permute_handle_t h0 = PERMUTE_HANDLE_MAKE(slot, gen);
    CHECK(permute_table_valid(&t, h0));

    permute_table_free(&t, slot);
    CHECK(!permute_table_valid(&t, h0));

    uint32_t slot2 = 0, gen2 = 0;
    CHECK_EQ(permute_table_alloc(&t, &slot2, &gen2), 0);
    CHECK_EQ(slot2, 0);
    CHECK_EQ(gen2, 1);

    permute_handle_t h1 = PERMUTE_HANDLE_MAKE(slot2, gen2);
    CHECK(permute_table_valid(&t, h1));
    CHECK(!permute_table_valid(&t, h0)); // stale rejected
}

static void test_table_million_cycles(void)
{
    permute_table_t t;
    permute_table_init(&t, 1);

    uint32_t last_gen = 0;
    int bad = 0;

    for (int i = 0; i < 1000000; i++) {
        uint32_t slot = 0, gen = 0;
        if (permute_table_alloc(&t, &slot, &gen) != 0) {
            bad++;
            break;
        }
        if (slot != 0)
            bad++;
        if (i > 0 && gen != last_gen + 1u)
            bad++;

        permute_handle_t h = PERMUTE_HANDLE_MAKE(slot, gen);
        if (!permute_table_valid(&t, h))
            bad++;

        permute_table_free(&t, slot);

        if (permute_table_valid(&t, h))
            bad++;

        last_gen = gen;
    }

    CHECK_EQ(bad, 0);
    CHECK_EQ(t.live, 0);
    CHECK_EQ(last_gen, 999999u);
}

static void test_table_capacity(void)
{
    permute_table_t t;
    permute_table_init(&t, 2);

    uint32_t slot = 0, gen = 0;
    CHECK_EQ(permute_table_alloc(&t, &slot, &gen), 0);
    CHECK_EQ(permute_table_alloc(&t, &slot, &gen), 0);
    CHECK_EQ(permute_table_alloc(&t, &slot, &gen), -EMFILE);

    permute_table_free(&t, 1);
    CHECK_EQ(permute_table_alloc(&t, &slot, &gen), 0);
    CHECK_EQ(slot, 1);
    CHECK_EQ(gen, 1);
}

// ---------------------------------------------------------------------------
// rcvid encoding

static void test_rcvid_encoding(void)
{
    int32_t r = permute_rcvid_make(5, 7);
    CHECK(r > 0);
    CHECK_EQ(permute_rcvid_slot(r), 5);
    CHECK_EQ(permute_rcvid_gen(r), 7);
    CHECK(!permute_rcvid_is_valid(0));   // 0 means "pulse"
    CHECK(!permute_rcvid_is_valid(-1));

    // Sign bit must stay clear at the maximum representable generation.
    int32_t max = permute_rcvid_make(1023, 0x7FFF);
    CHECK(max > 0);
}

// ---------------------------------------------------------------------------
// Processes and threads

static void test_process_and_thread(void)
{
    permute_kernel_init(&g_kernel, 42);

    permute_handle_t p1, p2;
    CHECK_EQ(permute_process_create(&g_kernel, "srv", &p1), 0);
    CHECK_EQ(permute_process_create(&g_kernel, "cli", &p2), 0);

    permute_process_t *a = permute_process_get(&g_kernel, p1);
    permute_process_t *b = permute_process_get(&g_kernel, p2);
    CHECK(a != NULL && b != NULL);
    CHECK_EQ(a->pid, 1);
    CHECK_EQ(b->pid, 2);
    CHECK(strcmp(a->name, "srv") == 0);
    CHECK_EQ(a->prio_ceiling, PERMUTE_DEFAULT_PRIO_CEIL);

    permute_handle_t t1;
    CHECK_EQ(permute_thread_create(&g_kernel, p1, 20, PERMUTE_SCHED_FIFO, &t1),
             0);
    permute_thread_t *th = permute_thread_get(&g_kernel, t1);
    CHECK(th != NULL);
    CHECK_EQ(th->tid, 1);
    CHECK_EQ(th->pid, a->pid);
    CHECK_EQ(th->base_prio, 20);
    CHECK_EQ(th->eff_prio, 20);
    CHECK_EQ(th->state, PERMUTE_THREAD_READY);

    // Priority is clamped to the modelled maximum.
    permute_handle_t t2;
    CHECK_EQ(permute_thread_create(&g_kernel, p1, 99, PERMUTE_SCHED_RR, &t2), 0);
    CHECK_EQ(permute_thread_get(&g_kernel, t2)->base_prio, PERMUTE_PRIO_MAX);

    CHECK_EQ(permute_thread_destroy(&g_kernel, t1), 0);
    CHECK(permute_thread_get(&g_kernel, t1) == NULL);
    CHECK_EQ(permute_thread_destroy(&g_kernel, t1), -ESRCH);

    CHECK_EQ(permute_process_destroy(&g_kernel, p1), 0);
    CHECK(permute_process_get(&g_kernel, p1) == NULL);
    // Thread t2 belonged to p1 and must be gone.
    CHECK(permute_thread_get(&g_kernel, t2) == NULL);
}

// ---------------------------------------------------------------------------
// Channels and connections

static void test_channel_isolation(void)
{
    permute_kernel_init(&g_kernel, 7);

    permute_handle_t p1, p2;
    CHECK_EQ(permute_process_create(&g_kernel, "a", &p1), 0);
    CHECK_EQ(permute_process_create(&g_kernel, "b", &p2), 0);

    int chid1 = -1, chid2 = -1;
    permute_handle_t c1, c2;
    CHECK_EQ(permute_channel_create(&g_kernel, p1, 0, &chid1, &c1), 0);
    CHECK_EQ(permute_channel_create(&g_kernel, p2, 0, &chid2, &c2), 0);

    // The chid space is per process: both may own chid 0.
    CHECK_EQ(chid1, 0);
    CHECK_EQ(chid2, 0);
    CHECK(c1 != c2);

    permute_handle_t looked = PERMUTE_HANDLE_NONE;
    CHECK_EQ(permute_channel_lookup(&g_kernel, p1, 0, &looked), 0);
    CHECK_EQ(looked, c1);
    CHECK_EQ(permute_channel_lookup(&g_kernel, p2, 0, &looked), 0);
    CHECK_EQ(looked, c2);

    CHECK_EQ(permute_channel_lookup(&g_kernel, p1, 1, &looked), -ESRCH);
}

static void test_connection_refs(void)
{
    permute_kernel_init(&g_kernel, 7);

    permute_handle_t srv, cli;
    CHECK_EQ(permute_process_create(&g_kernel, "srv", &srv), 0);
    CHECK_EQ(permute_process_create(&g_kernel, "cli", &cli), 0);

    int chid = -1;
    permute_handle_t ch;
    CHECK_EQ(permute_channel_create(&g_kernel, srv, 0, &chid, &ch), 0);

    int coid = -1;
    permute_handle_t conn;
    CHECK_EQ(permute_connect_attach(&g_kernel, cli, srv, chid, 0, &coid, &conn),
             0);
    CHECK_EQ(coid, 0);
    CHECK_EQ(permute_channel_get(&g_kernel, ch)->conn_refs, 1);

    permute_handle_t looked = PERMUTE_HANDLE_NONE;
    CHECK_EQ(permute_connection_lookup(&g_kernel, cli, coid, &looked), 0);
    CHECK_EQ(looked, conn);

    permute_connection_t *c = permute_connection_get(&g_kernel, conn);
    CHECK(c != NULL);
    CHECK_EQ(c->client_pid, permute_process_get(&g_kernel, cli)->pid);
    CHECK_EQ(c->server_pid, permute_process_get(&g_kernel, srv)->pid);

    CHECK_EQ(permute_connect_detach(&g_kernel, cli, coid), 0);
    CHECK_EQ(permute_channel_get(&g_kernel, ch)->conn_refs, 0);
    CHECK_EQ(permute_connect_detach(&g_kernel, cli, coid), -ESRCH);
}

// ---------------------------------------------------------------------------
// Priority

static void test_effective_priority(void)
{
    permute_kernel_init(&g_kernel, 1);

    permute_handle_t p, t;
    CHECK_EQ(permute_process_create(&g_kernel, "p", &p), 0);
    CHECK_EQ(permute_thread_create(&g_kernel, p, 10, PERMUTE_SCHED_FIFO, &t), 0);

    permute_thread_t *th = permute_thread_get(&g_kernel, t);

    CHECK_EQ(permute_eff_prio(th), 10);

    th->sticky_prio = 25; // message-driven, does not revert
    permute_thread_recompute_prio(th);
    CHECK_EQ(th->eff_prio, 25);

    th->mutex_boost = 30; // classic PI
    permute_thread_recompute_prio(th);
    CHECK_EQ(th->eff_prio, 30);

    th->prot_ceil = 40; // PROTECT ceiling of a held mutex
    permute_thread_recompute_prio(th);
    CHECK_EQ(th->eff_prio, 40);

    // Explicit scheduling clears sticky, not held-lock contributions.
    CHECK_EQ(permute_thread_set_prio(&g_kernel, t, 5), 0);
    CHECK_EQ(th->base_prio, 5);
    CHECK_EQ(th->sticky_prio, 0);
    CHECK_EQ(th->eff_prio, 40);
}

// ---------------------------------------------------------------------------
// Payload copy and rcvid lifecycle

static void test_message_payload_is_copied(void)
{
    permute_kernel_init(&g_kernel, 3);

    permute_handle_t p;
    CHECK_EQ(permute_process_create(&g_kernel, "srv", &p), 0);

    uint8_t src[16];
    memset(src, 'A', sizeof src);

    permute_handle_t m;
    CHECK_EQ(permute_message_create(&g_kernel, p, 1, 2, 10, src, sizeof src,
                                    &m),
             0);

    // Poison the sender buffer: the kernel must hold its own copy (FR-3.7).
    memset(src, 'Z', sizeof src);

    permute_message_t *msg = permute_message_get(&g_kernel, m);
    CHECK(msg != NULL);
    CHECK_EQ(msg->payload_len, sizeof src);
    CHECK(msg->payload != NULL);
    CHECK_EQ(msg->payload[0], 'A');
    CHECK_EQ(msg->payload[15], 'A');
    CHECK(permute_blob_usable(msg->payload) >= sizeof src);
}

static void test_rcvid_lifecycle(void)
{
    permute_kernel_init(&g_kernel, 3);

    permute_handle_t p;
    CHECK_EQ(permute_process_create(&g_kernel, "srv", &p), 0);

    permute_handle_t m;
    CHECK_EQ(permute_message_create(&g_kernel, p, 1, 2, 10, "hi", 2, &m), 0);

    permute_message_t *msg = permute_message_get(&g_kernel, m);
    int32_t rcvid = msg->rcvid;
    CHECK(rcvid > 0);

    permute_handle_t looked = PERMUTE_HANDLE_NONE;
    CHECK_EQ(permute_message_lookup_rcvid(&g_kernel, rcvid, &looked), 0);
    CHECK_EQ(looked, m);

    CHECK_EQ(permute_message_destroy(&g_kernel, m), 0);
    CHECK_EQ(permute_message_lookup_rcvid(&g_kernel, rcvid, &looked), -ESRCH);
}

// ---------------------------------------------------------------------------
// Blob arena

static void test_blob_classes(void)
{
    static uint8_t storage[1 << 20];
    permute_blob_arena_t a;
    permute_blob_init(&a, storage, sizeof storage);

    size_t sizes[] = { 1, 32, 33, 100, 1024, 5000, 65536 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        void *p = permute_blob_alloc(&a, sizes[i]);
        CHECK(p != NULL);
        if (p == NULL)
            continue;
        CHECK(permute_blob_usable(p) >= sizes[i]);
        memset(p, 0xAB, sizes[i]);
        permute_blob_free(&a, p);
    }

    // 65537 exceeds the largest class.
    CHECK(permute_blob_alloc(&a, 65537) == NULL);
    // Zero is not an allocation.
    CHECK(permute_blob_alloc(&a, 0) == NULL);

    // Exhaustion is a hard NULL, not a silent short allocation.
    void *blocks[64];
    int n = 0;
    while (n < 64) {
        blocks[n] = permute_blob_alloc(&a, 65536);
        if (blocks[n] == NULL)
            break;
        n++;
    }
    CHECK(n > 0 && n < 64);
    permute_blob_free(&a, blocks[0]);
    CHECK(permute_blob_alloc(&a, 65536) != NULL);
}

// ---------------------------------------------------------------------------
// Queue

static void test_queue(void)
{
    permute_queue_t q;
    permute_queue_init(&q);

    CHECK_EQ(permute_queue_length(&q), 0);
    permute_queue_entry_t e;
    CHECK_EQ(permute_queue_pop(&q, &e), -EAGAIN);

    for (uint32_t i = 0; i < PERMUTE_QUEUE_CAP; i++)
        CHECK_EQ(permute_queue_insert(&q, PERMUTE_HANDLE_MAKE(i, 0), 10, i,
                                      PERMUTE_Q_MESSAGE),
                 0);

    CHECK_EQ(permute_queue_insert(&q, PERMUTE_HANDLE_MAKE(0, 0), 10, 999,
                                  PERMUTE_Q_MESSAGE),
             -EAGAIN);
    CHECK_EQ(permute_queue_length(&q), PERMUTE_QUEUE_CAP);

    // Equal priority pops in insertion order.
    for (uint32_t i = 0; i < PERMUTE_QUEUE_CAP; i++) {
        CHECK_EQ(permute_queue_pop(&q, &e), 0);
        CHECK_EQ(PERMUTE_HANDLE_SLOT(e.handle), i);
    }
    CHECK_EQ(permute_queue_pop(&q, &e), -EAGAIN);

    // Higher priority first, regardless of insertion order.
    permute_queue_init(&q);
    CHECK_EQ(permute_queue_insert(&q, PERMUTE_HANDLE_MAKE(1, 0), 10, 0,
                                  PERMUTE_Q_MESSAGE),
             0);
    CHECK_EQ(permute_queue_insert(&q, PERMUTE_HANDLE_MAKE(2, 0), 30, 1,
                                  PERMUTE_Q_MESSAGE),
             0);
    CHECK_EQ(permute_queue_insert(&q, PERMUTE_HANDLE_MAKE(3, 0), 20, 2,
                                  PERMUTE_Q_MESSAGE),
             0);

    CHECK_EQ(permute_queue_pop(&q, &e), 0);
    CHECK_EQ(PERMUTE_HANDLE_SLOT(e.handle), 2);
    CHECK_EQ(permute_queue_pop(&q, &e), 0);
    CHECK_EQ(PERMUTE_HANDLE_SLOT(e.handle), 3);
    CHECK_EQ(permute_queue_pop(&q, &e), 0);
    CHECK_EQ(PERMUTE_HANDLE_SLOT(e.handle), 1);

    // Removal by handle.
    permute_queue_init(&q);
    CHECK_EQ(permute_queue_insert(&q, PERMUTE_HANDLE_MAKE(4, 0), 5, 0,
                                  PERMUTE_Q_MESSAGE),
             0);
    CHECK_EQ(permute_queue_remove(&q, PERMUTE_HANDLE_MAKE(4, 0)), 0);
    CHECK_EQ(permute_queue_length(&q), 0);
    CHECK_EQ(permute_queue_remove(&q, PERMUTE_HANDLE_MAKE(4, 0)), -ESRCH);
}

// ---------------------------------------------------------------------------

int main(void)
{
    test_table_generations();
    test_table_million_cycles();
    test_table_capacity();
    test_rcvid_encoding();
    test_process_and_thread();
    test_channel_isolation();
    test_connection_refs();
    test_effective_priority();
    test_message_payload_is_copied();
    test_rcvid_lifecycle();
    test_blob_classes();
    test_queue();

    return TEST_RESULT("test_objects");
}
