// SPDX-License-Identifier: Apache-2.0
//
// P1-06 unit tests: TimerTimeout and its state mask, the disarm/previous
// deadline contract, the latch, the unblock exception on REPLY, POSIX timers,
// the timer heap, and the deadlock outcome.

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "kernel/ipc.h"
#include "kernel/objects.h"
#include "kernel/pi.h"
#include "kernel/sched.h"
#include "kernel/timer.h"
#include "kernel/timeout.h"
#include "kernel/vclock.h"

static permute_kernel_t K;

typedef struct {
    permute_handle_t proc;
    permute_handle_t thread;
    int chid;
} ep_t;

static void make_ep(ep_t *e, const char *name, int32_t prio, uint32_t flags)
{
    permute_handle_t ch;
    CHECK_EQ(permute_process_create(&K, name, &e->proc), 0);
    CHECK_EQ(permute_thread_create(&K, e->proc, prio, PERMUTE_SCHED_FIFO,
                                   &e->thread),
             0);
    CHECK_EQ(permute_channel_create(&K, e->proc, flags, &e->chid, &ch), 0);
}

static int connect_to(permute_handle_t client, permute_handle_t server, int chid)
{
    permute_handle_t conn;
    int coid = -1;
    CHECK_EQ(permute_connect_attach(&K, client, server, chid, 0, &coid, &conn),
             0);
    return coid;
}

// Take a thread out of the runnable set so virtual time may advance. The unit
// tests drive the state machine directly; in a real executor these threads
// would be running or blocked.
static void park(permute_handle_t thread)
{
    permute_thread_t *t = permute_thread_get(&K, thread);
    permute_thread_block(&K, t, PERMUTE_THREAD_STOPPED, PERMUTE_HANDLE_NONE);
}

static void arm(permute_handle_t thread, uint32_t mask, uint64_t ns,
                uint64_t *otime)
{
    permute_result_t r;
    CHECK_EQ(permute_timeout_set(&K, thread, mask, NULL, &ns, otime, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.err, 0);
}

// ---------------------------------------------------------------------------
// TimerTimeout on a kernel blocking state

static void test_receive_timeout(void)
{
    permute_kernel_init(&K, 60);
    ep_t srv;
    make_ep(&srv, "srv", 10, 0);

    uint64_t t = 10 * PERMUTE_TICK_NS;
    arm(srv.thread, PERMUTE_STATE_BIT_RECEIVE, t, NULL);

    permute_result_t r;
    CHECK_EQ(permute_ipc_receive(&K, srv.thread, srv.chid, NULL, 0, NULL, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_eventq_count(&K.eventq), 1);

    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_RUNNABLE);
    CHECK_EQ(permute_vclock_now(&K), t);

    permute_thread_t *srv_t = permute_thread_get(&K, srv.thread);
    CHECK_EQ(srv_t->state, PERMUTE_THREAD_READY);
    CHECK_EQ(srv_t->pending.ret, -1);
    CHECK_EQ(srv_t->pending.err, ETIMEDOUT);
    CHECK_EQ(permute_eventq_count(&K.eventq), 0);
}

static void test_non_matching_state_stays_armed(void)
{
    permute_kernel_init(&K, 61);

    ep_t srv, drv;
    make_ep(&srv, "srv", 10, 0);
    make_ep(&drv, "drv", 10, 0);
    int to_drv = connect_to(srv.proc, drv.proc, drv.chid);

    uint64_t t = 10 * PERMUTE_TICK_NS;
    arm(srv.thread, PERMUTE_STATE_BIT_RECEIVE, t, NULL);

    // Block in SEND, which the mask does not cover. No timer may activate.
    permute_result_t r;
    CHECK_EQ(permute_ipc_send(&K, srv.thread, to_drv, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_thread_get(&K, srv.thread)->state, PERMUTE_THREAD_SEND);
    CHECK_EQ(permute_eventq_count(&K.eventq), 0);

    // Complete that call; the timeout must survive it (FR-3.22).
    CHECK_EQ(permute_ipc_receive(&K, drv.thread, drv.chid, NULL, 0, NULL, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(permute_ipc_reply(&K, drv.thread, r.rcvid, 0, NULL, 0, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(permute_thread_get(&K, srv.thread)->timeout_armed, 1);
    park(drv.thread);

    // Now block in the covered state: the timer activates and fires.
    CHECK_EQ(permute_ipc_receive(&K, srv.thread, srv.chid, NULL, 0, NULL, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_eventq_count(&K.eventq), 1);
    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_RUNNABLE);
    CHECK_EQ(permute_thread_get(&K, srv.thread)->pending.err, ETIMEDOUT);
}

static void test_disarm_returns_previous_deadline(void)
{
    permute_kernel_init(&K, 62);
    ep_t srv;
    make_ep(&srv, "srv", 10, 0);

    uint64_t t = 50 * PERMUTE_TICK_NS;
    uint64_t otime = 0;
    arm(srv.thread, PERMUTE_STATE_BIT_RECEIVE, t, &otime);
    CHECK_EQ(otime, 0); // nothing armed before

    // Vendor disarm: flags == 0.
    permute_result_t r;
    uint64_t prev = 0;
    CHECK_EQ(permute_timeout_set(&K, srv.thread, 0, NULL, NULL, &prev, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(prev, t);
    CHECK_EQ(permute_thread_get(&K, srv.thread)->timeout_armed, 0);

    // PRD 3.23: ntime == NULL disarms and returns the previous deadline.
    arm(srv.thread, PERMUTE_STATE_BIT_RECEIVE, 30 * PERMUTE_TICK_NS, NULL);
    prev = 0;
    CHECK_EQ(permute_timeout_set(&K, srv.thread,
                                 PERMUTE_STATE_BIT_RECEIVE, NULL, NULL, &prev,
                                 &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(prev, 30 * PERMUTE_TICK_NS);
    CHECK_EQ(permute_thread_get(&K, srv.thread)->timeout_armed, 0);
}

static void test_replacement_uses_latest(void)
{
    permute_kernel_init(&K, 63);
    ep_t srv;
    make_ep(&srv, "srv", 10, 0);

    arm(srv.thread, PERMUTE_STATE_BIT_RECEIVE, 10 * PERMUTE_TICK_NS, NULL);
    arm(srv.thread, PERMUTE_STATE_BIT_RECEIVE, 20 * PERMUTE_TICK_NS, NULL);

    permute_result_t r;
    CHECK_EQ(permute_ipc_receive(&K, srv.thread, srv.chid, NULL, 0, NULL, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_eventq_count(&K.eventq), 1); // only the live intent

    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_RUNNABLE);
    CHECK_EQ(permute_vclock_now(&K), 20 * PERMUTE_TICK_NS);
}

static void test_send_timeout_discards_message(void)
{
    permute_kernel_init(&K, 64);
    ep_t srv, cli;
    make_ep(&srv, "srv", 10, 0);
    make_ep(&cli, "cli", 10, 0);
    int coid = connect_to(cli.proc, srv.proc, srv.chid);

    arm(cli.thread, PERMUTE_STATE_BIT_SEND, 10 * PERMUTE_TICK_NS, NULL);

    permute_result_t r;
    CHECK_EQ(permute_ipc_send(&K, cli.thread, coid, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(K.messages.live, 1);
    park(srv.thread);

    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_RUNNABLE);
    CHECK_EQ(permute_thread_get(&K, cli.thread)->pending.err, ETIMEDOUT);
    // The message must not be left for a server to receive from a call that
    // already failed.
    CHECK_EQ(K.messages.live, 0);
    CHECK_EQ(permute_ipc_receive(&K, srv.thread, srv.chid, NULL, 0, NULL, &r),
             PERMUTE_BLOCKED);
}

// ---------------------------------------------------------------------------
// The _NTO_CHF_UNBLOCK exception

static void test_reply_timeout_with_unblock(void)
{
    permute_kernel_init(&K, 65);
    ep_t srv, cli;
    make_ep(&srv, "srv", 10, PERMUTE_CHF_UNBLOCK);
    make_ep(&cli, "cli", 10, 0);
    int coid = connect_to(cli.proc, srv.proc, srv.chid);

    arm(cli.thread, PERMUTE_STATE_BIT_SEND | PERMUTE_STATE_BIT_REPLY,
        10 * PERMUTE_TICK_NS, NULL);

    permute_result_t r;
    CHECK_EQ(permute_ipc_send(&K, cli.thread, coid, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_receive(&K, srv.thread, srv.chid, NULL, 0, NULL, &r),
             PERMUTE_CONTINUE);
    int32_t rcvid = r.rcvid;
    CHECK_EQ(permute_thread_get(&K, cli.thread)->state, PERMUTE_THREAD_REPLY);

    park(srv.thread);
    // The client stays blocked and the server is parked, so the timer fires
    // without making anything runnable.
    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_IDLE);

    // The client stays REPLY-blocked; the server is told instead.
    CHECK_EQ(permute_thread_get(&K, cli.thread)->state, PERMUTE_THREAD_REPLY);

    CHECK_EQ(permute_ipc_receive_pulse(&K, srv.thread, srv.chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.pulse.code, PERMUTE_PULSE_CODE_UNBLOCK);
    CHECK_EQ((uint64_t)r.pulse.value, (uint64_t)rcvid);

    // The server's reply completes the original send.
    CHECK_EQ(permute_ipc_reply(&K, srv.thread, rcvid, 0, NULL, 0, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(permute_thread_get(&K, cli.thread)->state, PERMUTE_THREAD_READY);
    CHECK_EQ(permute_thread_get(&K, cli.thread)->pending.ret, 0);
}

static void test_reply_timeout_without_unblock(void)
{
    permute_kernel_init(&K, 66);
    ep_t srv, cli;
    make_ep(&srv, "srv", 10, 0);
    make_ep(&cli, "cli", 10, 0);
    int coid = connect_to(cli.proc, srv.proc, srv.chid);

    arm(cli.thread, PERMUTE_STATE_BIT_SEND | PERMUTE_STATE_BIT_REPLY,
        10 * PERMUTE_TICK_NS, NULL);

    permute_result_t r;
    CHECK_EQ(permute_ipc_send(&K, cli.thread, coid, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_receive(&K, srv.thread, srv.chid, NULL, 0, NULL, &r),
             PERMUTE_CONTINUE);
    int32_t rcvid = r.rcvid;

    park(srv.thread);
    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_RUNNABLE);

    CHECK_EQ(permute_thread_get(&K, cli.thread)->state, PERMUTE_THREAD_READY);
    CHECK_EQ(permute_thread_get(&K, cli.thread)->pending.err, ETIMEDOUT);

    // The message is gone, so the late reply fails.
    CHECK_EQ(permute_ipc_reply(&K, srv.thread, rcvid, 0, NULL, 0, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.err, ESRCH);
}

static void test_mutex_timeout_reverts_boost(void)
{
    permute_kernel_init(&K, 67);
    permute_handle_t proc, owner, waiter, m;
    CHECK_EQ(permute_process_create(&K, "p", &proc), 0);
    CHECK_EQ(permute_thread_create(&K, proc, 5, PERMUTE_SCHED_FIFO, &owner), 0);
    CHECK_EQ(permute_thread_create(&K, proc, 20, PERMUTE_SCHED_FIFO, &waiter),
             0);
    CHECK_EQ(permute_mutex_create(&K, PERMUTE_MUTEX_INHERIT, 0, 0, &m), 0);

    permute_result_t r;
    CHECK_EQ(permute_mutex_lock(&K, owner, m, &r), PERMUTE_CONTINUE);

    uint64_t t = 10 * PERMUTE_TICK_NS;
    arm(waiter, PERMUTE_STATE_BIT_MUTEX, t, NULL);
    CHECK_EQ(permute_mutex_lock(&K, waiter, m, &r), PERMUTE_BLOCKED);
    CHECK_EQ(permute_thread_get(&K, owner)->eff_prio, 20);

    park(owner);
    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_RUNNABLE);

    CHECK_EQ(permute_thread_get(&K, waiter)->pending.err, ETIMEDOUT);
    // A waiter that no longer exists must not keep the owner boosted.
    CHECK_EQ(permute_thread_get(&K, owner)->mutex_boost, 0);
    CHECK_EQ(permute_thread_get(&K, owner)->eff_prio, 5);
    CHECK_EQ(permute_mutex_get(&K, m)->owner, owner);
}

// ---------------------------------------------------------------------------
// POSIX timers

typedef struct {
    ep_t cli;
    int self_coid;
} ptimer_fixture_t;

static void ptimer_setup(ptimer_fixture_t *f)
{
    permute_kernel_init(&K, 70);
    make_ep(&f->cli, "cli", 10, 0);
    f->self_coid = connect_to(f->cli.proc, f->cli.proc, f->cli.chid);
}

static permute_sigevent_t pulse_event(int coid, int16_t code, uint64_t value,
                                      int16_t prio)
{
    permute_sigevent_t e;
    memset(&e, 0, sizeof e);
    e.notify = PERMUTE_SIGEV_PULSE;
    e.coid = coid;
    e.code = code;
    e.value = value;
    e.priority = prio;
    return e;
}

static void test_posix_timer_one_shot(void)
{
    ptimer_fixture_t f;
    ptimer_setup(&f);

    permute_sigevent_t ev = pulse_event(f.self_coid, 0x21, 0xABC, 5);
    permute_handle_t timer;
    CHECK_EQ(permute_timer_create(&K, f.cli.proc, &ev, &timer), 0);
    CHECK_EQ(permute_timer_settime(&K, timer, 5 * PERMUTE_TICK_NS, 0), 0);
    park(f.cli.thread);

    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_IDLE);
    CHECK_EQ(permute_vclock_now(&K), 5 * PERMUTE_TICK_NS);

    permute_result_t r;
    CHECK_EQ(permute_ipc_receive_pulse(&K, f.cli.thread, f.cli.chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(r.pulse.code, 0x21);
    CHECK_EQ((uint64_t)r.pulse.value, 0xABCULL);

    // One-shot: nothing left.
    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_DEADLOCK);
}

static void test_posix_timer_periodic(void)
{
    ptimer_fixture_t f;
    ptimer_setup(&f);

    permute_sigevent_t ev = pulse_event(f.self_coid, 0x22, 1, 5);
    permute_handle_t timer;
    CHECK_EQ(permute_timer_create(&K, f.cli.proc, &ev, &timer), 0);
    CHECK_EQ(permute_timer_settime(&K, timer, 5 * PERMUTE_TICK_NS,
                                   5 * PERMUTE_TICK_NS),
             0);
    park(f.cli.thread);

    permute_result_t r;
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_IDLE);
        CHECK_EQ(permute_ipc_receive_pulse(&K, f.cli.thread, f.cli.chid, NULL,
                                           PERMUTE_PULSE_WIRE_SIZE, &r),
                 PERMUTE_CONTINUE);
        CHECK_EQ(r.pulse.code, 0x22);
    }
    CHECK_EQ(permute_vclock_now(&K), 15 * PERMUTE_TICK_NS);
}

static void test_posix_timer_settime_replaces_and_disarms(void)
{
    ptimer_fixture_t f;
    ptimer_setup(&f);

    permute_sigevent_t ev = pulse_event(f.self_coid, 0x23, 1, 5);
    permute_handle_t timer;
    CHECK_EQ(permute_timer_create(&K, f.cli.proc, &ev, &timer), 0);

    CHECK_EQ(permute_timer_settime(&K, timer, 5 * PERMUTE_TICK_NS, 0), 0);
    CHECK_EQ(permute_timer_settime(&K, timer, 9 * PERMUTE_TICK_NS, 0), 0);
    park(f.cli.thread);

    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_IDLE);
    CHECK_EQ(permute_vclock_now(&K), 9 * PERMUTE_TICK_NS); // not 5

    permute_result_t r;
    CHECK_EQ(permute_ipc_receive_pulse(&K, f.cli.thread, f.cli.chid, NULL,
                                       PERMUTE_PULSE_WIRE_SIZE, &r),
             PERMUTE_CONTINUE);

    // A zero initial value disarms.
    CHECK_EQ(permute_timer_settime(&K, timer, 3 * PERMUTE_TICK_NS, 0), 0);
    CHECK_EQ(permute_timer_settime(&K, timer, 0, 0), 0);
    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_DEADLOCK);
}

static void test_posix_timer_delete(void)
{
    ptimer_fixture_t f;
    ptimer_setup(&f);

    permute_sigevent_t ev = pulse_event(f.self_coid, 0x24, 1, 5);
    permute_handle_t timer;
    CHECK_EQ(permute_timer_create(&K, f.cli.proc, &ev, &timer), 0);
    CHECK_EQ(permute_timer_settime(&K, timer, 5 * PERMUTE_TICK_NS, 0), 0);
    CHECK_EQ(permute_timer_delete(&K, timer), 0);
    park(f.cli.thread);

    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_DEADLOCK);
}

static void test_posix_signal_event_refused(void)
{
    ptimer_fixture_t f;
    ptimer_setup(&f);

    permute_sigevent_t ev;
    memset(&ev, 0, sizeof ev);
    ev.notify = PERMUTE_SIGEV_SIGNAL;

    permute_handle_t timer;
    CHECK_EQ(permute_timer_create(&K, f.cli.proc, &ev, &timer), -ENOTSUP);
}

// ---------------------------------------------------------------------------
// Clock, heap, and outcome

static void test_deadlock_outcome(void)
{
    permute_kernel_init(&K, 80);
    CHECK_EQ(permute_sim_advance(&K), PERMUTE_SIM_DEADLOCK);
    CHECK_EQ(permute_sim_run(&K), PERMUTE_SIM_DEADLOCK);
}

static void test_clock_never_goes_backwards(void)
{
    permute_kernel_init(&K, 81);

    CHECK_EQ(permute_vclock_advance(&K, 100), 0);
    CHECK_EQ(permute_vclock_now(&K), 100u);
    CHECK_EQ(permute_vclock_advance(&K, 50), -EINVAL);
    CHECK_EQ(permute_vclock_now(&K), 100u);
}

static uint64_t next_rand(uint64_t *s)
{
    *s = *s * 6364136223846793005ull + 1442695040888963407ull;
    return *s >> 33;
}

static void test_eventq_fuzz(void)
{
    permute_eventq_t q;
    permute_eventq_init(&q);

    uint64_t s = 0x1234567890ABCDEFull;
    int failures = 0;

    for (int round = 0; round < 10000; round++) {
        uint32_t n = (uint32_t)(next_rand(&s) % 65);
        for (uint32_t i = 0; i < n; i++) {
            uint64_t deadline = next_rand(&s) % 1000;
            if (permute_eventq_push(&q, deadline, i,
                                    PERMUTE_HANDLE_MAKE(i, 0),
                                    PERMUTE_TIMER_THREAD) != 0)
                failures++;
        }
        if (permute_eventq_check(&q) != 0)
            failures++;

        uint64_t last = 0;
        uint32_t popped = 0;
        permute_timer_entry_t e;
        while (popped <= n && permute_eventq_pop(&q, &e) == 0) {
            if (e.deadline_ns < last)
                failures++; // heap order broken
            last = e.deadline_ns;
            popped++;
        }
        if (popped != n)
            failures++;
        if (permute_eventq_count(&q) != 0)
            failures++;
        if (failures != 0)
            break;
    }

    CHECK_EQ(failures, 0);
}

static void test_pending_timer_keeps_sim_idle(void)
{
    permute_kernel_init(&K, 82);
    ep_t srv;
    make_ep(&srv, "srv", 10, 0);

    // Timeout far in the future; the clock must jump straight to it rather
    // than stepping.
    arm(srv.thread, PERMUTE_STATE_BIT_RECEIVE, 1000 * PERMUTE_TICK_NS, NULL);
    permute_result_t r;
    CHECK_EQ(permute_ipc_receive(&K, srv.thread, srv.chid, NULL, 0, NULL, &r),
             PERMUTE_BLOCKED);

    CHECK_EQ(permute_sim_run(&K), PERMUTE_SIM_RUNNABLE);
    CHECK_EQ(permute_vclock_now(&K), 1000 * PERMUTE_TICK_NS);
}

// ---------------------------------------------------------------------------

int main(void)
{
    test_receive_timeout();
    test_non_matching_state_stays_armed();
    test_disarm_returns_previous_deadline();
    test_replacement_uses_latest();
    test_send_timeout_discards_message();
    test_reply_timeout_with_unblock();
    test_reply_timeout_without_unblock();
    test_mutex_timeout_reverts_boost();
    test_posix_timer_one_shot();
    test_posix_timer_periodic();
    test_posix_timer_settime_replaces_and_disarms();
    test_posix_timer_delete();
    test_posix_signal_event_refused();
    test_deadlock_outcome();
    test_clock_never_goes_backwards();
    test_eventq_fuzz();
    test_pending_timer_keeps_sim_idle();

    return TEST_RESULT("test_time");
}
