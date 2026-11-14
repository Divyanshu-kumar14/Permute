// SPDX-License-Identifier: Apache-2.0
//
// P2-01 unit tests: fault-rule parsing, deterministic materialisation, site
// filtering, and the FR-4.3 property that fault decisions cannot touch the
// scheduling stream.

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "kernel/fault.h"

static void test_parse_and_site_filter(void)
{
    permute_fault_plan_t plan;
    permute_fault_init(&plan, 42);

    CHECK_EQ(permute_fault_add_string(&plan, "send:drop@1"), 0);
    CHECK_EQ(plan.count, 1);

    uint8_t action = 0;
    uint64_t delay = 0;

    CHECK_EQ(permute_fault_decide(&plan, PERMUTE_FAULT_SEND, &action, &delay),
             1);
    CHECK_EQ(action, PERMUTE_FAULT_DROP);

    // A rule for one site never fires at another.
    CHECK_EQ(permute_fault_decide(&plan, PERMUTE_FAULT_RECV, &action, &delay),
             0);

    CHECK_EQ(permute_fault_add_string(&plan, "send:explode@1"), -EINVAL);
    CHECK_EQ(permute_fault_add_string(&plan, "bogus:drop@1"), -EINVAL);
    CHECK_EQ(permute_fault_add_string(&plan, "send:drop@2"), -EINVAL);
    CHECK_EQ(permute_fault_add_string(&plan, "send:drop"), -EINVAL);
    CHECK_EQ(permute_fault_add_string(&plan, "send:drop@0.5:1000"), 0);
    CHECK_EQ(plan.rules[1].numerator, 500000);
    CHECK_EQ(plan.rules[1].delay_ns, 1000);
}

static void test_probability_extremes(void)
{
    permute_fault_plan_t never, always;
    permute_fault_init(&never, 7);
    permute_fault_init(&always, 7);
    CHECK_EQ(permute_fault_add_string(&never, "recv:timeout@0"), 0);
    CHECK_EQ(permute_fault_add_string(&always, "recv:timeout@1"), 0);

    uint8_t action;
    uint64_t delay;
    int never_fired = 0, always_fired = 0;

    for (int i = 0; i < 5000; i++) {
        never_fired += permute_fault_decide(&never, PERMUTE_FAULT_RECV,
                                            &action, &delay);
        always_fired += permute_fault_decide(&always, PERMUTE_FAULT_RECV,
                                             &action, &delay);
    }

    CHECK_EQ(never_fired, 0);
    CHECK_EQ(always_fired, 5000);
}

static void test_materialisation_is_deterministic(void)
{
    permute_fault_plan_t a, b;
    permute_fault_init(&a, 99);
    permute_fault_init(&b, 99);
    CHECK_EQ(permute_fault_add_string(&a, "send:drop@0.3"), 0);
    CHECK_EQ(permute_fault_add_string(&b, "send:drop@0.3"), 0);

    int mismatches = 0;
    for (int i = 0; i < 10000; i++) {
        uint8_t aa, ab;
        uint64_t da, db;
        int fa = permute_fault_decide(&a, PERMUTE_FAULT_SEND, &aa, &da);
        int fb = permute_fault_decide(&b, PERMUTE_FAULT_SEND, &ab, &db);
        if (fa != fb || (fa && (aa != ab || da != db)))
            mismatches++;
    }

    CHECK_EQ(mismatches, 0);
    CHECK_EQ(a.fires, b.fires);
    CHECK(a.fires > 0 && a.fires < 10000);
}

// FR-4.3: the fault stream is its own domain. Exercising it, at a new site or
// ten thousand times, cannot move a single scheduling decision.
static void test_faults_do_not_touch_scheduling(void)
{
    uint64_t expected[64];

    permute_stream_t control;
    permute_stream_init(&control, 1234);
    for (uint64_t i = 0; i < 64; i++)
        expected[i] = permute_stream_at(&control, PERMUTE_D_SCHED, i);

    permute_fault_plan_t plan;
    permute_fault_init(&plan, 1234);
    CHECK_EQ(permute_fault_add_string(&plan, "send:drop@1"), 0);
    CHECK_EQ(permute_fault_add_string(&plan, "recv:timeout@1"), 0);
    CHECK_EQ(permute_fault_add_string(&plan, "reply:delay@1:100"), 0);
    CHECK_EQ(permute_fault_add_string(&plan, "pulse:kill@1"), 0);

    for (int i = 0; i < 10000; i++) {
        uint8_t action;
        uint64_t delay;
        for (uint8_t site = 0; site < PERMUTE_FAULT_SITE_COUNT; site++)
            (void)permute_fault_decide(&plan, site, &action, &delay);
    }

    // The fault stream advanced; the scheduling domain did not.
    CHECK(plan.stream.counter[PERMUTE_D_FAULT] > 0);
    CHECK_EQ(plan.stream.counter[PERMUTE_D_SCHED], 0);

    permute_stream_t after;
    permute_stream_init(&after, 1234);
    for (uint64_t i = 0; i < 64; i++)
        CHECK_EQ(permute_stream_at(&after, PERMUTE_D_SCHED, i), expected[i]);
}

static void test_rule_capacity(void)
{
    permute_fault_plan_t plan;
    permute_fault_init(&plan, 1);

    int added = 0;
    while (added < PERMUTE_MAX_FAULT_RULES) {
        CHECK_EQ(permute_fault_add_string(&plan, "send:drop@0.5"), 0);
        added++;
    }
    CHECK_EQ(permute_fault_add_string(&plan, "send:drop@0.5"), -ENOSPC);
}

int main(void)
{
    test_parse_and_site_filter();
    test_probability_extremes();
    test_materialisation_is_deterministic();
    test_faults_do_not_touch_scheduling();
    test_rule_capacity();

    return TEST_RESULT("test_fault");
}
