// SPDX-License-Identifier: Apache-2.0
//
// P1-03 unit tests: pulses, local coids, short receive buffers, secure events,
// disconnect notification, and the teardown paths that keep a dead process
// from stranding a live one.

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "kernel/ipc.h"

typedef struct {
    permute_kernel_t k;

    permute_handle_t srv, cli, srv_t, cli_t;

    int srv_chid;
    int cli_chid;    // channel in the client, for pulses sent back to it
    int self_coid;   // client -> its own channel
    int srv_coid;    // client -> server channel
} fixture_t;

static fixture_t g_f;

static permute_sigevent_t pulse_event(int coid, int16_t code, uint64_t value,
                                      int16_t priority)
{
    permute_sigevent_t e;
    memset(&e, 0, sizeof e);
    e.notify = PERMUTE_SIGEV_PULSE;
    e.coid = coid;
    e.code = code;
    e.value = value;
    e.priority = priority;
    return e;
}

static void setup(fixture_t *f, uint32_t channel_flags)
{
    permute_handle_t ch, conn;

    permute_kernel_init(&f->k, 42);

    CHECK_EQ(permute_process_create(&f->k, "srv", &f->srv), 0);
    CHECK_EQ(permute_process_create(&f->k, "cli", &f->cli), 0);
    CHECK_EQ(permute_thread_create(&f->k, f->srv, 10, PERMUTE_SCHED_FIFO,
                                   &f->srv_t),
             0);
    CHECK_EQ(permute_thread_create(&f->k, f->cli, 10, PERMUTE_SCHED_FIFO,
                                   &f->cli_t),
             0);

    CHECK_EQ(permute_channel_create(&f->k, f->cli, 0, &f->cli_chid, &ch), 0);
    CHECK_EQ(permute_connect_attach(&f->k, f->cli, f->cli, f->cli_chid, 0,
                                    &f->self_coid, &conn),
             0);

    CHECK_EQ(permute_channel_create(&f->k, f->srv, channel_flags, &f->srv_chid,
                                    &ch),
             0);
    CHECK_EQ(permute_connect_attach(&f->k, f->cli, f->srv, f->srv_chid, 0,
                                    &f->srv_coid, &conn),
             0);
}

// ---------------------------------------------------------------------------
// Negative coid: MsgSendPulse(-chid) addresses the caller's own channel

static void test_local_coid(void)
{
    setup(&g_f, 0);

    permute_result_t r;

    // A second channel, because -0 is 0 and therefore not a negative coid.
    permute_handle_t ch2;
    int chid2 = -1;
    CHECK_EQ(permute_channel_create(&g_f.k, g_f.cli, 0, &chid2, &ch2), 0);
    CHECK_EQ(chid2, 1);

    // No receiver yet: the pulse queues on the client's own channel.
    CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, -chid2, -1, 0x11, 0x22,
                                    &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.ret, 0);
    CHECK_EQ(r.err, 0);

    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.cli_t, chid2, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.ret, 0);
    CHECK_EQ(r.pulse.code, 0x11);
    CHECK_EQ((uint64_t)r.pulse.value, 0x22ULL);
    CHECK_EQ(r.pulse.scoid, 0); // a local pulse has no connection

    // A negative coid that names no channel is just as dead as a bad coid.
    CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, -7, -1, 1, 2, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.err, ESRCH);
}

// The local receiver can be the same process, different thread, blocked first.
static void test_local_coid_wakes_sibling(void)
{
    setup(&g_f, 0);

    permute_handle_t ch2, rx_thread;
    int chid2 = -1;
    permute_result_t r;

    CHECK_EQ(permute_channel_create(&g_f.k, g_f.cli, 0, &chid2, &ch2), 0);
    CHECK_EQ(permute_thread_create(&g_f.k, g_f.cli, 10, PERMUTE_SCHED_FIFO,
                                   &rx_thread),
             0);

    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, rx_thread, chid2, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_BLOCKED);

    CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, -chid2, 7, 0x33, 4, &r),
             PERMUTE_CONTINUE);

    permute_thread_t *rx = permute_thread_get(&g_f.k, rx_thread);
    CHECK_EQ(rx->state, PERMUTE_THREAD_READY);
    CHECK_EQ(rx->pending.ret, 0);
    CHECK_EQ(rx->pending.pulse.code, 0x33);
    CHECK_EQ(rx->pending.pulse.priority, 7);
}

// ---------------------------------------------------------------------------
// MsgReceivePulse buffer rules

static void test_short_buffer_drops_queued_pulse(void)
{
    setup(&g_f, 0);

    permute_result_t r;
    CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, g_f.srv_coid, 5, 0x42, 9,
                                    &r),
             PERMUTE_CONTINUE);

    // One byte short of a struct _pulse: EFAULT, and the pulse is gone.
    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE - 1, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.ret, -1);
    CHECK_EQ(r.err, EFAULT);

    // Nothing left to receive.
    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_BLOCKED);
}

static void test_short_buffer_blocks_then_loses(void)
{
    setup(&g_f, 0);

    permute_result_t r;

    // The buffer can never hold a pulse, but the call still blocks - and the
    // pulse that wakes it is lost. This is the QNX footgun FR-3.4 pins down.
    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL, 4,
                                       &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_thread_get(&g_f.k, g_f.srv_t)->recv_short, 1);

    CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, g_f.srv_coid, 5, 0x55, 1,
                                    &r),
             PERMUTE_CONTINUE);

    permute_thread_t *srv = permute_thread_get(&g_f.k, g_f.srv_t);
    CHECK_EQ(srv->state, PERMUTE_THREAD_READY);
    CHECK_EQ(srv->pending.ret, -1);
    CHECK_EQ(srv->pending.err, EFAULT);
    CHECK_EQ(srv->recv_short, 0);

    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_BLOCKED);
}

static void test_receive_pulse_ignores_messages(void)
{
    setup(&g_f, 0);

    permute_result_t r;
    permute_msg_info_t info;

    CHECK_EQ(permute_ipc_send(&g_f.k, g_f.cli_t, g_f.srv_coid, "msg", 4, NULL, 0,
                              &r),
             PERMUTE_BLOCKED);

    // A message is queued, but MsgReceivePulse wants a pulse and must block.
    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_BLOCKED);

    // A pulse queued *behind* the message still reaches the pulse-only waiter.
    CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, g_f.srv_coid, 1, 0x77, 1,
                                    &r),
             PERMUTE_CONTINUE);

    permute_thread_t *srv = permute_thread_get(&g_f.k, g_f.srv_t);
    CHECK_EQ(srv->state, PERMUTE_THREAD_READY);
    CHECK_EQ(srv->pending.pulse.code, 0x77);

    // The message is still there for a normal receiver.
    uint8_t buf[8];
    CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.srv_chid, buf,
                                 sizeof buf, &info, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.bytes, 4);
    CHECK_EQ(memcmp(buf, "msg", 4), 0);
}

// ---------------------------------------------------------------------------
// Ordering and priority forms

static void test_pulse_priority_order(void)
{
    setup(&g_f, 0);

    permute_result_t r;
    int prios[3] = { 10, 30, 20 };

    for (int i = 0; i < 3; i++)
        CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, g_f.srv_coid,
                                        prios[i], (int32_t)i, 0, &r),
                 PERMUTE_CONTINUE);

    int expect[3] = { 30, 20, 10 };
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                           PERMUTE_PULSE_WIRE_SIZE, &r),
                 PERMUTE_CONTINUE);
        CHECK_EQ(r.pulse.priority, expect[i]);
    }
}

static void test_pulse_priority_forms(void)
{
    setup(&g_f, 0);

    permute_result_t r;

    // -1 is "the caller's priority"; 0 is the idle priority and is coerced.
    CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, g_f.srv_coid, -1, 1, 0,
                                    &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.pulse.priority, 10);

    CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, g_f.srv_coid, 0, 1, 0,
                                    &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.pulse.priority, 1);
}

static void test_pulse_code_is_one_byte(void)
{
    setup(&g_f, 0);

    permute_result_t r;

    // The wire field is int8_t. A code outside 0..127 is truncated to eight
    // bits and read back sign-extended, which is why the documented safe range
    // stops at _PULSE_CODE_MAXAVAIL.
    CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, g_f.srv_coid, 1, 0xFF, 0,
                                    &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.pulse.code, -1);

    CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, g_f.srv_coid, 1,
                                    PERMUTE_PULSE_CODE_DISCONNECT, 0, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.pulse.code, PERMUTE_PULSE_CODE_DISCONNECT);
}

// ---------------------------------------------------------------------------
// Disconnect notification

static void test_disconnect_on_last_detach(void)
{
    setup(&g_f, PERMUTE_CHF_DISCONNECT);

    permute_result_t r;
    permute_handle_t conn;
    int second_coid = -1;

    CHECK_EQ(permute_connect_attach(&g_f.k, g_f.cli, g_f.srv, g_f.srv_chid, 0,
                                    &second_coid, &conn),
             0);

    // One of two connections goes away: no notification yet.
    CHECK_EQ(permute_connect_detach(&g_f.k, g_f.cli, second_coid), 0);
    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_BLOCKED);

    // The last one goes away: the server is told, with the coid as the value.
    // The server is already blocked in MsgReceivePulse, so the pulse is
    // delivered into its pending result rather than queued. Queued delivery
    // is covered by test_disconnect_on_client_death.
    CHECK_EQ(permute_connect_detach(&g_f.k, g_f.cli, g_f.srv_coid), 0);

    permute_thread_t *srv = permute_thread_get(&g_f.k, g_f.srv_t);
    CHECK_EQ(srv->state, PERMUTE_THREAD_READY);
    CHECK_EQ(srv->pending.ret, 0);
    CHECK_EQ(srv->pending.pulse.code, PERMUTE_PULSE_CODE_DISCONNECT);
    CHECK_EQ((uint64_t)srv->pending.pulse.value, (uint64_t)g_f.srv_coid);
    CHECK_EQ(srv->pending.pulse.scoid, g_f.srv_coid);

    // And nothing was left behind in the queue.
    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_BLOCKED);
}

static void test_no_disconnect_without_flag(void)
{
    setup(&g_f, 0);

    permute_result_t r;
    CHECK_EQ(permute_connect_detach(&g_f.k, g_f.cli, g_f.srv_coid), 0);
    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_BLOCKED);
}

static void test_disconnect_on_client_death(void)
{
    setup(&g_f, PERMUTE_CHF_DISCONNECT);

    permute_result_t r;

    // The kernel detaches a dead process's connections for it; the server
    // must see exactly the same notification.
    CHECK_EQ(permute_process_destroy(&g_f.k, g_f.cli), 0);

    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.pulse.code, PERMUTE_PULSE_CODE_DISCONNECT);
}

// ---------------------------------------------------------------------------
// Channel destruction

static void test_channel_destroy_unblocks_sender(void)
{
    setup(&g_f, 0);

    permute_result_t r;

    CHECK_EQ(permute_ipc_send(&g_f.k, g_f.cli_t, g_f.srv_coid, "x", 1, NULL, 0,
                              &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_thread_get(&g_f.k, g_f.cli_t)->state, PERMUTE_THREAD_SEND);

    CHECK_EQ(permute_channel_destroy(&g_f.k, g_f.srv, g_f.srv_chid), 0);

    permute_thread_t *cli = permute_thread_get(&g_f.k, g_f.cli_t);
    CHECK_EQ(cli->state, PERMUTE_THREAD_READY);
    CHECK_EQ(cli->pending.ret, -1);
    CHECK_EQ(cli->pending.err, ESRCH);
    // No message leaked: the pool is empty again.
    CHECK_EQ(g_f.k.messages.live, 0);
}

static void test_channel_destroy_unblocks_receiver(void)
{
    setup(&g_f, 0);

    permute_result_t r;

    CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_channel_destroy(&g_f.k, g_f.srv, g_f.srv_chid), 0);

    permute_thread_t *srv = permute_thread_get(&g_f.k, g_f.srv_t);
    CHECK_EQ(srv->state, PERMUTE_THREAD_READY);
    CHECK_EQ(srv->pending.err, ESRCH);
}

static void test_dead_server_unblocks_client(void)
{
    setup(&g_f, 0);

    permute_result_t r;

    // The client sends, the server receives and then dies without replying.
    CHECK_EQ(permute_ipc_send(&g_f.k, g_f.cli_t, g_f.srv_coid, "x", 1, NULL, 0,
                              &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_CONTINUE);

    CHECK_EQ(permute_process_destroy(&g_f.k, g_f.srv), 0);

    permute_thread_t *cli = permute_thread_get(&g_f.k, g_f.cli_t);
    CHECK_EQ(cli->state, PERMUTE_THREAD_READY);
    CHECK_EQ(cli->pending.err, ESRCH);
    CHECK_EQ(g_f.k.messages.live, 0);
}

// ---------------------------------------------------------------------------
// Secure events

static void test_event_register_and_deliver(void)
{
    setup(&g_f, 0);

    permute_result_t r;
    permute_sigevent_t ev =
        pulse_event(g_f.self_coid, 0x21, 0xABCDEF, 5);

    CHECK_EQ(permute_ipc_register_event(&g_f.k, g_f.cli_t, &ev, g_f.srv_coid,
                                        &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.ret, 0);
    // The caller's event is replaced by an opaque handle.
    CHECK_EQ(ev.notify, PERMUTE_SIGEV_HANDLE);
    CHECK(ev.coid != 0);

    int32_t cli_prio_before =
        permute_thread_get(&g_f.k, g_f.cli_t)->eff_prio;

    CHECK_EQ(permute_ipc_send(&g_f.k, g_f.cli_t, g_f.srv_coid, &ev, sizeof ev,
                              NULL, 0, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_CONTINUE);
    CHECK(r.rcvid > 0);

    CHECK_EQ(permute_ipc_deliver_event(&g_f.k, g_f.srv_t, r.rcvid, &ev, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.ret, 0);

    // Delivery must not touch the client's priority (FR-3.5): sticky priority
    // is a message effect, not an event effect.
    CHECK_EQ(permute_thread_get(&g_f.k, g_f.cli_t)->eff_prio, cli_prio_before);
    CHECK_EQ(permute_thread_get(&g_f.k, g_f.cli_t)->sticky_prio, 0);

    CHECK_EQ(permute_ipc_receive_pulse(&g_f.k, g_f.cli_t, g_f.cli_chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.pulse.code, 0x21);
    CHECK_EQ((uint64_t)r.pulse.value, 0xABCDEFULL);
    CHECK_EQ(r.pulse.priority, 5);
}

static void test_event_requires_registration(void)
{
    setup(&g_f, 0);

    permute_result_t r;
    permute_sigevent_t ev = pulse_event(g_f.self_coid, 1, 2, 3);

    CHECK_EQ(permute_ipc_send(&g_f.k, g_f.cli_t, g_f.srv_coid, "x", 1, NULL, 0,
                              &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_CONTINUE);

    // An unregistered event is not a capability.
    CHECK_EQ(permute_ipc_deliver_event(&g_f.k, g_f.srv_t, r.rcvid, &ev, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.err, EACCES);
}

static void test_event_wrong_server(void)
{
    setup(&g_f, 0);

    permute_handle_t other, other_t;
    int other_chid = -1, other_coid = -1;
    permute_handle_t conn;
    permute_result_t r;

    CHECK_EQ(permute_process_create(&g_f.k, "other", &other), 0);
    CHECK_EQ(permute_thread_create(&g_f.k, other, 10, PERMUTE_SCHED_FIFO,
                                   &other_t),
             0);
    CHECK_EQ(permute_channel_create(&g_f.k, other, 0, &other_chid, &conn), 0);
    CHECK_EQ(permute_connect_attach(&g_f.k, g_f.cli, other, other_chid, 0,
                                    &other_coid, &conn),
             0);

    permute_sigevent_t ev = pulse_event(g_f.self_coid, 1, 2, 3);
    CHECK_EQ(permute_ipc_register_event(&g_f.k, g_f.cli_t, &ev, g_f.srv_coid,
                                        &r),
             PERMUTE_CONTINUE);

    // The client sends to the *other* server, which presents the handle.
    CHECK_EQ(permute_ipc_send(&g_f.k, g_f.cli_t, other_coid, "x", 1, NULL, 0,
                              &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_receive(&g_f.k, other_t, other_chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(permute_ipc_deliver_event(&g_f.k, other_t, r.rcvid, &ev, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.err, EACCES);
}

static void test_verify_event(void)
{
    setup(&g_f, 0);

    permute_result_t r;
    permute_sigevent_t ev = pulse_event(g_f.self_coid, 9, 9, 9);

    // Well-formedness check by the client itself.
    CHECK_EQ(permute_ipc_verify_event(&g_f.k, g_f.cli_t, 0, &ev, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.ret, 0);

    permute_sigevent_t bad = pulse_event(999, 9, 9, 9);
    CHECK_EQ(permute_ipc_verify_event(&g_f.k, g_f.cli_t, 0, &bad, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.err, ESRCH); // no such connection for the pulse

    CHECK_EQ(permute_ipc_register_event(&g_f.k, g_f.cli_t, &ev, g_f.srv_coid,
                                        &r),
             PERMUTE_CONTINUE);

    CHECK_EQ(permute_ipc_send(&g_f.k, g_f.cli_t, g_f.srv_coid, &ev, sizeof ev,
                              NULL, 0, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_CONTINUE);

    // The owning server can verify it...
    CHECK_EQ(permute_ipc_verify_event(&g_f.k, g_f.srv_t, r.rcvid, &ev, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.ret, 0);

    // ...a different thread in another process cannot.
    CHECK_EQ(permute_ipc_verify_event(&g_f.k, g_f.cli_t, r.rcvid, &ev, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.err, EINVAL);
}

static void test_unblock_event(void)
{
    setup(&g_f, 0);

    permute_result_t r;
    permute_sigevent_t ev;
    memset(&ev, 0, sizeof ev);
    ev.notify = PERMUTE_SIGEV_UNBLOCK;

    CHECK_EQ(permute_ipc_receive(&g_f.k, g_f.cli_t, g_f.cli_chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_BLOCKED);

    permute_thread_t *cli = permute_thread_get(&g_f.k, g_f.cli_t);
    int32_t cli_tid = (int32_t)cli->tid;

    // Negative rcvid addresses the thread directly; no registration needed.
    CHECK_EQ(permute_ipc_deliver_event(&g_f.k, g_f.srv_t, -cli_tid, &ev, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.ret, 0);
    CHECK_EQ(cli->state, PERMUTE_THREAD_READY);
    CHECK_EQ(cli->pending.ret, -1);
    CHECK_EQ(cli->pending.err, EINTR);

    // Not blocked any more: delivering again is a no-op, not an error.
    CHECK_EQ(permute_ipc_deliver_event(&g_f.k, g_f.srv_t, -cli_tid, &ev, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.ret, 0);
}

static void test_signal_event_rejected(void)
{
    setup(&g_f, 0);

    permute_result_t r;
    permute_sigevent_t ev;
    memset(&ev, 0, sizeof ev);
    ev.notify = PERMUTE_SIGEV_SIGNAL;

    CHECK_EQ(permute_ipc_register_event(&g_f.k, g_f.cli_t, &ev, g_f.srv_coid,
                                        &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.err, EINVAL);
}

static void test_registration_lifecycle(void)
{
    setup(&g_f, 0);

    permute_result_t r;
    int registered = 0;

    // Fill the registry through one connection.
    for (;;) {
        permute_sigevent_t ev = pulse_event(g_f.self_coid, 1, 1, 1);
        CHECK_EQ(permute_ipc_register_event(&g_f.k, g_f.cli_t, &ev,
                                            g_f.srv_coid, &r),
                 PERMUTE_CONTINUE);
        if (r.err != 0) {
            CHECK_EQ(r.err, EAGAIN);
            break;
        }
        registered++;
        if (registered > PERMUTE_MAX_REGISTRATIONS + 1)
            break; // guard against an unbounded loop
    }
    CHECK_EQ(registered, PERMUTE_MAX_REGISTRATIONS);
    CHECK_EQ(g_f.k.registrations.live, PERMUTE_MAX_REGISTRATIONS);

    // A dead process takes its registrations with it.
    CHECK_EQ(permute_process_destroy(&g_f.k, g_f.cli), 0);
    CHECK_EQ(g_f.k.registrations.live, 0);
}

// ---------------------------------------------------------------------------
// Volume

static void test_pulse_volume(void)
{
    setup(&g_f, 0);

    permute_result_t r;
    int received = 0;

    for (int i = 0; i < 10000; i++) {
        CHECK_EQ(permute_ipc_send_pulse(&g_f.k, g_f.cli_t, g_f.srv_coid, 1,
                                        (int32_t)(i & 0x7F), (uint64_t)i, &r),
                 PERMUTE_CONTINUE);
        if (permute_ipc_receive_pulse(&g_f.k, g_f.srv_t, g_f.srv_chid, NULL,
                                      PERMUTE_PULSE_WIRE_SIZE, &r) ==
            PERMUTE_CONTINUE) {
            CHECK_EQ((uint64_t)r.pulse.value, (uint64_t)i);
            received++;
        }
    }

    CHECK_EQ(received, 10000);
    CHECK_EQ(g_f.k.pulses.live, 0);
}

// ---------------------------------------------------------------------------

int main(void)
{
    test_local_coid();
    test_local_coid_wakes_sibling();
    test_short_buffer_drops_queued_pulse();
    test_short_buffer_blocks_then_loses();
    test_receive_pulse_ignores_messages();
    test_pulse_priority_order();
    test_pulse_priority_forms();
    test_pulse_code_is_one_byte();
    test_disconnect_on_last_detach();
    test_no_disconnect_without_flag();
    test_disconnect_on_client_death();
    test_channel_destroy_unblocks_sender();
    test_channel_destroy_unblocks_receiver();
    test_dead_server_unblocks_client();
    test_event_register_and_deliver();
    test_event_requires_registration();
    test_event_wrong_server();
    test_verify_event();
    test_unblock_event();
    test_signal_event_rejected();
    test_registration_lifecycle();
    test_pulse_volume();

    return TEST_RESULT("test_pulse");
}
