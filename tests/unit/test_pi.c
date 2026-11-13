// SPDX-License-Identifier: Apache-2.0
//
// P1-05 unit tests: message-driven inheritance (send, receive, no revert,
// multi-hop, ceiling, fixed-priority) and the two mutex protocols (reverting
// PI and priority ceiling).

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "kernel/ipc.h"
#include "kernel/objects.h"
#include "kernel/pi.h"

static permute_kernel_t K;

typedef struct {
    permute_handle_t proc;
    permute_handle_t thread;
    int chid;
} endpoint_t;

static void make_endpoint(endpoint_t *e, const char *name, int32_t prio,
                          uint32_t flags)
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

// Fresh kernel with one server and one client, connected and ready to talk.
typedef struct {
    endpoint_t srv;
    endpoint_t cli;
    int coid;
} pair_t;

static void make_pair(pair_t *p, int32_t srv_prio, int32_t cli_prio,
                      uint32_t flags)
{
    permute_kernel_init(&K, 42);
    make_endpoint(&p->srv, "srv", srv_prio, flags);
    make_endpoint(&p->cli, "cli", cli_prio, 0);
    p->coid = connect_to(p->cli.proc, p->srv.proc, p->srv.chid);
}

// ---------------------------------------------------------------------------
// Message-driven inheritance

static void test_send_raises_receive_blocked(void)
{
    pair_t p;
    make_pair(&p, 5, 30, 0);

    permute_result_t r;
    CHECK_EQ(permute_ipc_receive(&K, p.srv.thread, p.srv.chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_BLOCKED);

    permute_thread_t *srv = permute_thread_get(&K, p.srv.thread);
    CHECK_EQ(srv->eff_prio, 5);

    CHECK_EQ(permute_ipc_send(&K, p.cli.thread, p.coid, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);

    // Raised at send time, so the server cannot be delayed by a
    // medium-priority thread before it receives.
    CHECK_EQ(srv->sticky_prio, 30);
    CHECK_EQ(srv->eff_prio, 30);
}

static void test_receive_assignment_never_drops_below_base(void)
{
    pair_t p;
    make_pair(&p, 22, 10, 0);

    permute_result_t r;
    CHECK_EQ(permute_ipc_receive(&K, p.srv.thread, p.srv.chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_send(&K, p.cli.thread, p.coid, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);

    // The vendor text says a receive from a lower-priority sender lowers the
    // thread to that sender's priority. PRD 9.1 defines effective priority as
    // max(base, sticky, ...), so the inherited value is assigned (10) but the
    // effective priority stays at the server's own 22. Recorded as a
    // documented divergence; this test pins the PRD behaviour.
    permute_thread_t *srv = permute_thread_get(&K, p.srv.thread);
    CHECK_EQ(srv->sticky_prio, 10);
    CHECK_EQ(srv->eff_prio, 22);
}

static void test_three_pending_messages(void)
{
    permute_kernel_init(&K, 43);

    endpoint_t srv;
    make_endpoint(&srv, "srv", 5, 0);

    int prios[3] = { 10, 30, 20 };
    endpoint_t cli[3];
    int coids[3];

    for (int i = 0; i < 3; i++) {
        make_endpoint(&cli[i], "cli", prios[i], 0);
        coids[i] = connect_to(cli[i].proc, srv.proc, srv.chid);

        permute_result_t r;
        CHECK_EQ(permute_ipc_send(&K, cli[i].thread, coids[i], &i, sizeof i,
                                  NULL, 0, &r),
                 PERMUTE_BLOCKED);
    }

    // Receive in priority order; the assignment is the maximum still pending
    // including the dequeued message, so it steps 30, 20, 10 (FR-3.14).
    int expect[3] = { 30, 20, 10 };
    for (int i = 0; i < 3; i++) {
        permute_result_t r;
        CHECK_EQ(permute_ipc_receive(&K, srv.thread, srv.chid, NULL, 0, NULL,
                                     &r),
                 PERMUTE_CONTINUE);

        permute_thread_t *t = permute_thread_get(&K, srv.thread);
        CHECK_EQ(t->sticky_prio, expect[i]);
        CHECK_EQ(t->eff_prio, expect[i]);
    }
}

static void test_no_revert_on_reply(void)
{
    pair_t p;
    make_pair(&p, 5, 30, 0);

    permute_result_t r;
    CHECK_EQ(permute_ipc_send(&K, p.cli.thread, p.coid, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_receive(&K, p.srv.thread, p.srv.chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_CONTINUE);
    CHECK(r.rcvid > 0);

    permute_thread_t *srv = permute_thread_get(&K, p.srv.thread);
    CHECK_EQ(srv->eff_prio, 30);

    CHECK_EQ(permute_ipc_reply(&K, p.srv.thread, r.rcvid, 0, NULL, 0, &r),
             PERMUTE_CONTINUE);

    // FR-3.13: replying does not return the server to its base priority.
    CHECK_EQ(srv->sticky_prio, 30);
    CHECK_EQ(srv->eff_prio, 30);
}

static void test_multihop_chain(void)
{
    permute_kernel_init(&K, 44);

    endpoint_t client, srv, drv;
    make_endpoint(&client, "client", 10, 0);
    make_endpoint(&srv, "srv", 5, 0);
    make_endpoint(&drv, "drv", 5, 0);

    int cli_coid = connect_to(client.proc, srv.proc, srv.chid);
    int srv_coid = connect_to(srv.proc, drv.proc, drv.chid);

    permute_result_t r;
    CHECK_EQ(permute_ipc_send(&K, client.thread, cli_coid, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);

    CHECK_EQ(permute_ipc_receive(&K, srv.thread, srv.chid, NULL, 0, NULL, &r),
             PERMUTE_CONTINUE);
    permute_thread_t *st = permute_thread_get(&K, srv.thread);
    CHECK_EQ(st->eff_prio, 10);

    // The server forwards at its inherited priority; no donor list is needed.
    CHECK_EQ(permute_ipc_send(&K, srv.thread, srv_coid, "y", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_receive(&K, drv.thread, drv.chid, NULL, 0, NULL, &r),
             PERMUTE_CONTINUE);

    permute_thread_t *dt = permute_thread_get(&K, drv.thread);
    CHECK_EQ(dt->sticky_prio, 10);
    CHECK_EQ(dt->eff_prio, 10);
}

static void test_ceiling_clamp(void)
{
    pair_t p;
    make_pair(&p, 5, 30, 0);

    // Without the priority ability, a donation stops at the process ceiling.
    permute_process_t *sp = permute_process_get(&K, p.srv.proc);
    sp->prio_ceiling = 15;
    sp->abilities = 0;

    permute_result_t r;
    CHECK_EQ(permute_ipc_receive(&K, p.srv.thread, p.srv.chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_send(&K, p.cli.thread, p.coid, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);

    permute_thread_t *srv = permute_thread_get(&K, p.srv.thread);
    CHECK_EQ(srv->sticky_prio, 15);
    CHECK_EQ(srv->eff_prio, 15);

    // With the ability, the same donation arrives whole.
    permute_kernel_init(&K, 45);
    make_endpoint(&p.srv, "srv", 5, 0);
    make_endpoint(&p.cli, "cli", 30, 0);
    p.coid = connect_to(p.cli.proc, p.srv.proc, p.srv.chid);
    sp = permute_process_get(&K, p.srv.proc);
    sp->prio_ceiling = 15;
    sp->abilities |= PERMUTE_AID_PRIORITY;

    CHECK_EQ(permute_ipc_receive(&K, p.srv.thread, p.srv.chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_send(&K, p.cli.thread, p.coid, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);
    srv = permute_thread_get(&K, p.srv.thread);
    CHECK_EQ(srv->eff_prio, 30);
}

static void test_fixed_priority_channel(void)
{
    pair_t p;
    make_pair(&p, 5, 30, PERMUTE_CHF_FIXED_PRIORITY);

    permute_result_t r;
    CHECK_EQ(permute_ipc_receive(&K, p.srv.thread, p.srv.chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_send(&K, p.cli.thread, p.coid, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);

    // FR-3.17: inheritance is disabled for this channel, in both directions.
    permute_thread_t *srv = permute_thread_get(&K, p.srv.thread);
    CHECK_EQ(srv->sticky_prio, 0);
    CHECK_EQ(srv->eff_prio, 5);
}

static void test_schedset_clears_sticky(void)
{
    pair_t p;
    make_pair(&p, 5, 30, 0);

    permute_result_t r;
    CHECK_EQ(permute_ipc_send(&K, p.cli.thread, p.coid, "x", 1, NULL, 0, &r),
             PERMUTE_BLOCKED);
    CHECK_EQ(permute_ipc_receive(&K, p.srv.thread, p.srv.chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_CONTINUE);

    CHECK_EQ(permute_thread_get(&K, p.srv.thread)->eff_prio, 30);

    // FR-3.20.
    CHECK_EQ(permute_thread_set_prio(&K, p.srv.thread, 8), 0);
    permute_thread_t *srv = permute_thread_get(&K, p.srv.thread);
    CHECK_EQ(srv->sticky_prio, 0);
    CHECK_EQ(srv->eff_prio, 8);
}

static void test_pulse_receive_assigns_priority(void)
{
    pair_t p;
    make_pair(&p, 5, 10, 0);

    permute_result_t r;

    // A high-priority pulse raises the receiver.
    CHECK_EQ(permute_ipc_send_pulse(&K, p.cli.thread, p.coid, 20, 1, 0, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(permute_ipc_receive(&K, p.srv.thread, p.srv.chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(permute_thread_get(&K, p.srv.thread)->sticky_prio, 20);

    // A low-priority pulse lowers the inherited value, never the effective
    // priority below base.
    CHECK_EQ(permute_ipc_send_pulse(&K, p.cli.thread, p.coid, 2, 1, 0, &r),
             PERMUTE_CONTINUE);
    CHECK_EQ(permute_ipc_receive(&K, p.srv.thread, p.srv.chid, NULL, 0, NULL,
                                 &r),
             PERMUTE_CONTINUE);
    permute_thread_t *srv = permute_thread_get(&K, p.srv.thread);
    CHECK_EQ(srv->sticky_prio, 2);
    CHECK_EQ(srv->eff_prio, 5);
}

// ---------------------------------------------------------------------------
// Mutex inheritance

typedef struct {
    permute_handle_t proc;
    permute_handle_t low, high;
    permute_handle_t mutex;
} mfixture_t;

static void make_mutex_fixture(mfixture_t *f, permute_mutex_proto_t proto,
                               int32_t ceiling)
{
    permute_kernel_init(&K, 50);
    CHECK_EQ(permute_process_create(&K, "p", &f->proc), 0);
    CHECK_EQ(permute_thread_create(&K, f->proc, 5, PERMUTE_SCHED_FIFO, &f->low),
             0);
    CHECK_EQ(permute_thread_create(&K, f->proc, 20, PERMUTE_SCHED_FIFO,
                                   &f->high),
             0);
    CHECK_EQ(permute_mutex_create(&K, proto, ceiling, 0, &f->mutex), 0);
}

static void test_mutex_inherit_reverts(void)
{
    mfixture_t f;
    make_mutex_fixture(&f, PERMUTE_MUTEX_INHERIT, 0);

    permute_result_t r;
    CHECK_EQ(permute_mutex_lock(&K, f.low, f.mutex, &r), PERMUTE_CONTINUE);
    CHECK_EQ(r.ret, 0);

    CHECK_EQ(permute_mutex_lock(&K, f.high, f.mutex, &r), PERMUTE_BLOCKED);
    CHECK_EQ(permute_thread_get(&K, f.high)->state, PERMUTE_THREAD_MUTEX);

    permute_thread_t *owner = permute_thread_get(&K, f.low);
    CHECK_EQ(owner->mutex_boost, 20);
    CHECK_EQ(owner->eff_prio, 20);

    CHECK_EQ(permute_mutex_unlock(&K, f.low, f.mutex, &r), PERMUTE_CONTINUE);
    CHECK_EQ(r.ret, 0);

    // Reverting PI: the owner drops, the waiter takes the lock.
    CHECK_EQ(owner->mutex_boost, 0);
    CHECK_EQ(owner->eff_prio, 5);
    CHECK_EQ(permute_thread_get(&K, f.high)->state, PERMUTE_THREAD_READY);
    CHECK_EQ(permute_mutex_get(&K, f.mutex)->owner, f.high);
}

static void test_mutex_inherit_transitive(void)
{
    permute_kernel_init(&K, 51);
    permute_handle_t proc, top, mid, bot;
    permute_handle_t m1, m2;

    CHECK_EQ(permute_process_create(&K, "p", &proc), 0);
    CHECK_EQ(permute_thread_create(&K, proc, 30, PERMUTE_SCHED_FIFO, &top), 0);
    CHECK_EQ(permute_thread_create(&K, proc, 10, PERMUTE_SCHED_FIFO, &mid), 0);
    CHECK_EQ(permute_thread_create(&K, proc, 5, PERMUTE_SCHED_FIFO, &bot), 0);
    CHECK_EQ(permute_mutex_create(&K, PERMUTE_MUTEX_INHERIT, 0, 0, &m1), 0);
    CHECK_EQ(permute_mutex_create(&K, PERMUTE_MUTEX_INHERIT, 0, 0, &m2), 0);

    permute_result_t r;

    // bot holds m1; mid holds m2 and blocks on m1; top blocks on m2.
    CHECK_EQ(permute_mutex_lock(&K, bot, m1, &r), PERMUTE_CONTINUE);
    CHECK_EQ(permute_mutex_lock(&K, mid, m2, &r), PERMUTE_CONTINUE);
    CHECK_EQ(permute_mutex_lock(&K, mid, m1, &r), PERMUTE_BLOCKED);
    CHECK_EQ(permute_thread_get(&K, bot)->eff_prio, 10);

    CHECK_EQ(permute_mutex_lock(&K, top, m2, &r), PERMUTE_BLOCKED);

    // "directly or indirectly": the priority climbs the whole chain.
    CHECK_EQ(permute_thread_get(&K, mid)->eff_prio, 30);
    CHECK_EQ(permute_thread_get(&K, bot)->eff_prio, 30);
}

static void test_mutex_protect(void)
{
    mfixture_t f;
    make_mutex_fixture(&f, PERMUTE_MUTEX_PROTECT, 20);

    permute_result_t r;
    CHECK_EQ(permute_mutex_lock(&K, f.low, f.mutex, &r), PERMUTE_CONTINUE);

    permute_thread_t *t = permute_thread_get(&K, f.low);
    CHECK_EQ(t->prot_ceil, 20);
    CHECK_EQ(t->eff_prio, 20);

    CHECK_EQ(permute_mutex_unlock(&K, f.low, f.mutex, &r), PERMUTE_CONTINUE);
    CHECK_EQ(t->prot_ceil, 0);
    CHECK_EQ(t->eff_prio, 5);
}

static void test_mutex_protect_ceiling_violation(void)
{
    mfixture_t f;
    make_mutex_fixture(&f, PERMUTE_MUTEX_PROTECT, 20);

    // The high thread is at 20, which equals the ceiling; a thread above it
    // must be refused rather than silently capped.
    permute_result_t r;
    CHECK_EQ(permute_mutex_lock(&K, f.high, f.mutex, &r), PERMUTE_CONTINUE);
    CHECK_EQ(permute_mutex_unlock(&K, f.high, f.mutex, &r), PERMUTE_CONTINUE);

    permute_handle_t above;
    CHECK_EQ(permute_thread_create(&K, f.proc, 30, PERMUTE_SCHED_FIFO, &above),
             0);
    CHECK_EQ(permute_mutex_lock(&K, above, f.mutex, &r), PERMUTE_CONTINUE);
    CHECK_EQ(r.err, EINVAL);
    CHECK_EQ(r.ret, -1);
}

static void test_mutex_normal_does_not_boost(void)
{
    mfixture_t f;
    make_mutex_fixture(&f, PERMUTE_MUTEX_NORMAL, 0);

    permute_result_t r;
    CHECK_EQ(permute_mutex_lock(&K, f.low, f.mutex, &r), PERMUTE_CONTINUE);
    CHECK_EQ(permute_mutex_lock(&K, f.high, f.mutex, &r), PERMUTE_BLOCKED);

    CHECK_EQ(permute_thread_get(&K, f.low)->eff_prio, 5);
}

static void test_mutex_recursive_and_errors(void)
{
    permute_kernel_init(&K, 52);
    permute_handle_t proc, a, b;
    permute_handle_t rec, plain;
    CHECK_EQ(permute_process_create(&K, "p", &proc), 0);
    CHECK_EQ(permute_thread_create(&K, proc, 5, PERMUTE_SCHED_FIFO, &a), 0);
    CHECK_EQ(permute_thread_create(&K, proc, 10, PERMUTE_SCHED_FIFO, &b), 0);
    CHECK_EQ(permute_mutex_create(&K, PERMUTE_MUTEX_NORMAL, 0, 1, &rec), 0);
    CHECK_EQ(permute_mutex_create(&K, PERMUTE_MUTEX_NORMAL, 0, 0, &plain), 0);

    permute_result_t r;

    CHECK_EQ(permute_mutex_lock(&K, a, rec, &r), PERMUTE_CONTINUE);
    CHECK_EQ(permute_mutex_lock(&K, a, rec, &r), PERMUTE_CONTINUE);
    CHECK_EQ(permute_mutex_get(&K, rec)->lock_count, 2);
    CHECK_EQ(permute_mutex_unlock(&K, a, rec, &r), PERMUTE_CONTINUE);
    CHECK_EQ(permute_mutex_get(&K, rec)->owner, a);
    CHECK_EQ(permute_mutex_unlock(&K, a, rec, &r), PERMUTE_CONTINUE);
    CHECK_EQ(permute_mutex_get(&K, rec)->owner, PERMUTE_HANDLE_NONE);

    // Non-recursive self-lock is a deadlock, not a second lock.
    CHECK_EQ(permute_mutex_lock(&K, a, plain, &r), PERMUTE_CONTINUE);
    CHECK_EQ(permute_mutex_lock(&K, a, plain, &r), PERMUTE_CONTINUE);
    CHECK_EQ(r.err, EDEADLK);

    // A non-owner cannot unlock.
    CHECK_EQ(permute_mutex_unlock(&K, b, plain, &r), PERMUTE_CONTINUE);
    CHECK_EQ(r.err, EPERM);

    // Destroying a held mutex is refused.
    CHECK_EQ(permute_mutex_destroy(&K, plain), -EBUSY);
    CHECK_EQ(permute_mutex_unlock(&K, a, plain, &r), PERMUTE_CONTINUE);
    CHECK_EQ(permute_mutex_destroy(&K, plain), 0);
}

static void test_mutex_handoff_highest_first(void)
{
    permute_kernel_init(&K, 53);
    permute_handle_t proc, owner, w10, w20, w30;
    permute_handle_t m;
    CHECK_EQ(permute_process_create(&K, "p", &proc), 0);
    CHECK_EQ(permute_thread_create(&K, proc, 5, PERMUTE_SCHED_FIFO, &owner), 0);
    CHECK_EQ(permute_thread_create(&K, proc, 10, PERMUTE_SCHED_FIFO, &w10), 0);
    CHECK_EQ(permute_thread_create(&K, proc, 20, PERMUTE_SCHED_FIFO, &w20), 0);
    CHECK_EQ(permute_thread_create(&K, proc, 30, PERMUTE_SCHED_FIFO, &w30), 0);
    CHECK_EQ(permute_mutex_create(&K, PERMUTE_MUTEX_INHERIT, 0, 0, &m), 0);

    permute_result_t r;
    CHECK_EQ(permute_mutex_lock(&K, owner, m, &r), PERMUTE_CONTINUE);
    CHECK_EQ(permute_mutex_lock(&K, w10, m, &r), PERMUTE_BLOCKED);
    CHECK_EQ(permute_mutex_lock(&K, w20, m, &r), PERMUTE_BLOCKED);
    CHECK_EQ(permute_mutex_lock(&K, w30, m, &r), PERMUTE_BLOCKED);

    CHECK_EQ(permute_thread_get(&K, owner)->eff_prio, 30);
    CHECK_EQ(permute_mutex_unlock(&K, owner, m, &r), PERMUTE_CONTINUE);

    CHECK_EQ(permute_mutex_get(&K, m)->owner, w30);
    CHECK_EQ(permute_thread_get(&K, w30)->state, PERMUTE_THREAD_READY);
    CHECK_EQ(permute_thread_get(&K, w30)->eff_prio, 30);
}

// ---------------------------------------------------------------------------

int main(void)
{
    test_send_raises_receive_blocked();
    test_receive_assignment_never_drops_below_base();
    test_three_pending_messages();
    test_no_revert_on_reply();
    test_multihop_chain();
    test_ceiling_clamp();
    test_fixed_priority_channel();
    test_schedset_clears_sticky();
    test_pulse_receive_assigns_priority();
    test_mutex_inherit_reverts();
    test_mutex_inherit_transitive();
    test_mutex_protect();
    test_mutex_protect_ceiling_violation();
    test_mutex_normal_does_not_boost();
    test_mutex_recursive_and_errors();
    test_mutex_handoff_highest_first();

    return TEST_RESULT("test_pi");
}
