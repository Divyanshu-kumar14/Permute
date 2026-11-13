// SPDX-License-Identifier: Apache-2.0
//
// P1-02 unit tests: the message path and its state machine.
//
// The kernel never picks the next thread, so these tests drive the state
// machine directly: send, receive, reply, and check the states and buffers that
// the executor will later observe.

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "kernel/ipc.h"

typedef struct {
    permute_kernel_t k;
    permute_handle_t srv, cli, srv_t, cli_t;
    int chid;
    int coid;
} fixture_t;

static fixture_t g_f;

static void setup(fixture_t *f, uint64_t seed)
{
    permute_handle_t ch, conn;

    permute_kernel_init(&f->k, seed);

    CHECK_EQ(permute_process_create(&f->k, "srv", &f->srv), 0);
    CHECK_EQ(permute_process_create(&f->k, "cli", &f->cli), 0);
    CHECK_EQ(permute_thread_create(&f->k, f->srv, 10, PERMUTE_SCHED_FIFO,
                                   &f->srv_t),
             0);
    CHECK_EQ(permute_thread_create(&f->k, f->cli, 10, PERMUTE_SCHED_FIFO,
                                   &f->cli_t),
             0);
    CHECK_EQ(permute_channel_create(&f->k, f->srv, 0, &f->chid, &ch), 0);
    CHECK_EQ(permute_connect_attach(&f->k, f->cli, f->srv, f->chid, 0,
                                    &f->coid, &conn),
             0);
}

static void fill(uint8_t *buf, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)(seed * 31u + i);
}

// Adds an extra client at a given priority; returns its coid.
static int add_client(fixture_t *f, int prio, permute_handle_t *thread_out,
                      uint32_t *pid_out)
{
    permute_handle_t p, t, conn;
    int coid = -1;

    CHECK_EQ(permute_process_create(&f->k, "cli", &p), 0);
    CHECK_EQ(permute_thread_create(&f->k, p, prio, PERMUTE_SCHED_FIFO, &t), 0);
    CHECK_EQ(permute_connect_attach(&f->k, p, f->srv, f->chid, 0, &coid, &conn),
             0);

    *thread_out = t;
    *pid_out = permute_process_get(&f->k, p)->pid;
    return coid;
}

// ---------------------------------------------------------------------------

static void test_ping_pong(void)
{
    setup(&g_f, 42);

    uint8_t out[64], in[64], reply[64], rbuf[64];
    permute_result_t r;
    permute_msg_info_t info;

    for (unsigned i = 0; i < 10000; i++) {
        fill(out, sizeof out, i);
        fill(reply, sizeof reply, i + 7);

        CHECK_EQ(permute_ipc_send(&g_f.k, g_f.cli_t, g_f.coid, out, sizeof out,
                                  rbuf, sizeof rbuf, &r),
                 PERMUTE_BLOCKED);
        CHECK_EQ(permute_thread_get(&g_f.k, g_f.cli_t)->state,
                 PERMUTE_THREAD_SEND);

        CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.chid, in,
                                     sizeof in, &info, &r),
                 PERMUTE_CONTINUE);
        CHECK(r.rcvid > 0);
        CHECK_EQ(r.bytes, sizeof out);
        CHECK_EQ(memcmp(in, out, sizeof out), 0);
        CHECK_EQ(info.msglen, sizeof out);

        // The sender moves to REPLY the moment the server receives.
        CHECK_EQ(permute_thread_get(&g_f.k, g_f.cli_t)->state,
                 PERMUTE_THREAD_REPLY);

        CHECK_EQ(permute_ipc_reply(&g_f.k, g_f.srv_t, r.rcvid, 0, reply,
                                   sizeof reply, &r),
                 PERMUTE_CONTINUE);

        permute_thread_t *client = permute_thread_get(&g_f.k, g_f.cli_t);
        CHECK_EQ(client->state, PERMUTE_THREAD_READY);
        CHECK_EQ(client->reply_len, sizeof reply);
        CHECK_EQ(memcmp(rbuf, reply, sizeof reply), 0);
    }
}

static void test_receiver_blocks_first(void)
{
    setup(&g_f, 7);

    uint8_t in[32], out[32];
    permute_result_t r;
    permute_msg_info_t info;

    fill(out, sizeof out, 3);
    fill(in, sizeof in, 0);

    CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.chid, in, sizeof in,
                                 &info, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_thread_get(&g_f.k, g_f.srv_t)->state,
             PERMUTE_THREAD_RECEIVE);

    CHECK_EQ(permute_ipc_send(&g_f.k, g_f.cli_t, g_f.coid, out, sizeof out, NULL,
                              0, &r),
             PERMUTE_BLOCKED);

    permute_thread_t *server = permute_thread_get(&g_f.k, g_f.srv_t);
    CHECK_EQ(server->state, PERMUTE_THREAD_READY);
    CHECK(server->pending.rcvid > 0);
    CHECK_EQ(server->recv_len, sizeof out);
    CHECK_EQ(memcmp(in, out, sizeof out), 0);

    // The delivery carries the rcvid the server will reply with.
    CHECK_EQ(permute_ipc_reply(&g_f.k, g_f.srv_t, server->pending.rcvid, 0, "ok",
                               2, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(permute_thread_get(&g_f.k, g_f.cli_t)->state,
             PERMUTE_THREAD_READY);
    CHECK_EQ(permute_thread_get(&g_f.k, g_f.cli_t)->reply_len, 0); // no buffer
}

static void test_priority_order(void)
{
    setup(&g_f, 11);

    int coids[3];
    permute_handle_t threads[3];
    uint32_t pids[3];
    int prios[3] = { 10, 30, 20 };

    for (int i = 0; i < 3; i++)
        coids[i] = add_client(&g_f, prios[i], &threads[i], &pids[i]);

    permute_result_t r;
    uint8_t payload[8];

    for (int i = 0; i < 3; i++) {
        memset(payload, 0, sizeof payload);
        payload[0] = (uint8_t)i;
        CHECK_EQ(permute_ipc_send(&g_f.k, threads[i], coids[i], payload,
                                  sizeof payload, NULL, 0, &r),
                 PERMUTE_BLOCKED);
    }

    // Highest priority first: client 1 (prio 30), then 2 (20), then 0 (10).
    int expected[3] = { 1, 2, 0 };
    for (int k = 0; k < 3; k++) {
        uint8_t in[8];
        CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.chid, in,
                                     sizeof in, NULL, &r),
                 PERMUTE_CONTINUE);
        CHECK_EQ(in[0], expected[k]);
        permute_msg_info_t info;
        (void)info;
        CHECK_EQ(permute_ipc_reply(&g_f.k, g_f.srv_t, r.rcvid, 0, NULL, 0, &r),
                 PERMUTE_CONTINUE);
    }
}

static void test_equal_priority_fifo(void)
{
    setup(&g_f, 12);

    int coids[3];
    permute_handle_t threads[3];
    uint32_t pids[3];

    for (int i = 0; i < 3; i++)
        coids[i] = add_client(&g_f, 10, &threads[i], &pids[i]);

    permute_result_t r;
    uint8_t payload[4] = { 0, 0, 0, 0 };

    for (int i = 0; i < 3; i++) {
        payload[0] = (uint8_t)i;
        CHECK_EQ(permute_ipc_send(&g_f.k, threads[i], coids[i], payload,
                                  sizeof payload, NULL, 0, &r),
                 PERMUTE_BLOCKED);
    }

    for (int k = 0; k < 3; k++) {
        uint8_t in[4];
        CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.chid, in, sizeof in,
                                     NULL, &r),
                 PERMUTE_CONTINUE);
        CHECK_EQ(in[0], k); // insertion order preserved
        CHECK_EQ(permute_ipc_reply(&g_f.k, g_f.srv_t, r.rcvid, 0, NULL, 0, &r),
                 PERMUTE_CONTINUE);
    }
}

static void test_truncation(void)
{
    setup(&g_f, 5);

    uint8_t out[64], in[8], reply[32], rbuf[8];
    permute_result_t r;
    permute_msg_info_t info;

    fill(out, sizeof out, 1);
    fill(reply, sizeof reply, 2);
    memset(rbuf, 0xEE, sizeof rbuf);

    CHECK_EQ(permute_ipc_send(&g_f.k, g_f.cli_t, g_f.coid, out, sizeof out, rbuf,
                              sizeof rbuf, &r),
             PERMUTE_BLOCKED);

    CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.chid, in, sizeof in,
                                 &info, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.bytes, sizeof in);
    CHECK_EQ(info.msglen, sizeof out); // full length is still reported
    CHECK_EQ(memcmp(in, out, sizeof in), 0);

    CHECK_EQ(permute_ipc_reply(&g_f.k, g_f.srv_t, r.rcvid, 0, reply,
                               sizeof reply, &r),
             PERMUTE_CONTINUE);

    permute_thread_t *client = permute_thread_get(&g_f.k, g_f.cli_t);
    CHECK_EQ(client->reply_len, sizeof rbuf);
    CHECK_EQ(memcmp(rbuf, reply, sizeof rbuf), 0);
}

static void test_error_reply(void)
{
    setup(&g_f, 6);

    permute_result_t r;

    CHECK_EQ(permute_ipc_send(&g_f.k, g_f.cli_t, g_f.coid, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.chid, NULL, 0, NULL, &r),
             PERMUTE_CONTINUE);
    CHECK(r.rcvid > 0);

    CHECK_EQ(permute_ipc_error(&g_f.k, g_f.srv_t, r.rcvid, EPERM, &r),
             PERMUTE_CONTINUE);

    permute_thread_t *client = permute_thread_get(&g_f.k, g_f.cli_t);
    CHECK_EQ(client->state, PERMUTE_THREAD_READY);
    CHECK_EQ(client->pending.err, EPERM);
    CHECK_EQ(client->pending.ret, -1);
}

static void test_destroyed_channel(void)
{
    setup(&g_f, 8);

    permute_result_t r;

    CHECK_EQ(permute_channel_destroy(&g_f.k, g_f.srv, g_f.chid), 0);

    CHECK_EQ(permute_ipc_send(&g_f.k, g_f.cli_t, g_f.coid, "x", 1, NULL, 0, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.err, ESRCH);
    CHECK_EQ(r.ret, -1);
}

static void test_invalid_rcvid(void)
{
    setup(&g_f, 9);

    permute_result_t r;

    CHECK_EQ(permute_ipc_reply(&g_f.k, g_f.srv_t, 0, 0, NULL, 0, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.err, ESRCH);

    CHECK_EQ(permute_ipc_reply(&g_f.k, g_f.srv_t, -7, 0, NULL, 0, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.err, ESRCH);

    CHECK_EQ(permute_ipc_reply(&g_f.k, g_f.srv_t, 0x7FFFFFFF, 0, NULL, 0, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.err, ESRCH);
}

static void test_pulse_receive_returns_zero(void)
{
    setup(&g_f, 10);

    permute_result_t r;

    CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, g_f.coid, -1, 0x42,
                                    0xDEADBEEF, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.ret, 0);

    CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.chid, NULL, 0, NULL, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.rcvid, 0); // FR-3.3: pulse receive returns 0
    CHECK_EQ(r.pulse.code, 0x42);
    CHECK_EQ((uint64_t)r.pulse.value, 0xDEADBEEFULL);
}

static void test_pulse_wakes_receiver(void)
{
    setup(&g_f, 11);

    permute_result_t r;

    CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.chid, NULL, 0, NULL, &r),
             PERMUTE_BLOCKED);

    // 0x59 is inside _PULSE_CODE_MINAVAIL.._PULSE_CODE_MAXAVAIL (0..127).
    // The wire field is int8_t, so a code above 127 reads back sign-extended;
    // test_pulse covers that explicitly.
    CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, g_f.coid, 5, 0x59, 7, &r),
             PERMUTE_CONTINUE);

    permute_thread_t *server = permute_thread_get(&g_f.k, g_f.srv_t);
    CHECK_EQ(server->state, PERMUTE_THREAD_READY);
    CHECK_EQ(server->pending.rcvid, 0);
    CHECK_EQ(server->pending.pulse.code, 0x59);
    CHECK_EQ(server->pending.pulse.priority, 5);
}

// ---------------------------------------------------------------------------

int main(void)
{
    test_ping_pong();
    test_receiver_blocks_first();
    test_priority_order();
    test_equal_priority_fifo();
    test_truncation();
    test_error_reply();
    test_destroyed_channel();
    test_invalid_rcvid();
    test_pulse_receive_returns_zero();
    test_pulse_wakes_receiver();

    return TEST_RESULT("test_ipc");
}
