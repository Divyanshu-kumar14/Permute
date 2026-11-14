// SPDX-License-Identifier: Apache-2.0
//
// Fault plan: which failure to inject at which message-path site.
//
// Every decision comes from `D_FAULT`, a domain of its own. Adding a fault site
// cannot perturb the scheduling stream, which is the whole reason the decision
// domains exist (FR-4.3). A site with no matching rule draws nothing at all, so
// instrumenting a new call site is free until a rule arms it.
//
// The plan is materialised from the seed, not stored: the same seed and the
// same rule list produce the same fires in the same order.

#ifndef PERMUTE_FAULT_H
#define PERMUTE_FAULT_H

#include <stdint.h>

#include "kernel/state.h"

#define PERMUTE_MAX_FAULT_RULES 32
#define PERMUTE_FAULT_SCALE 1000000u // probabilities are parts per million

typedef enum {
    PERMUTE_FAULT_SEND = 0,
    PERMUTE_FAULT_RECV,
    PERMUTE_FAULT_REPLY,
    PERMUTE_FAULT_PULSE,
    PERMUTE_FAULT_SITE_COUNT
} permute_fault_site_t;

typedef enum {
    PERMUTE_FAULT_DROP = 0,
    PERMUTE_FAULT_DUPLICATE,
    PERMUTE_FAULT_REORDER,
    PERMUTE_FAULT_DELAY,
    PERMUTE_FAULT_TIMEOUT,
    PERMUTE_FAULT_KILL,
    PERMUTE_FAULT_ACTION_COUNT
} permute_fault_action_t;

typedef struct {
    uint8_t site;
    uint8_t action;
    uint32_t numerator;   // fires when rnd % SCALE < numerator
    uint32_t denominator; // always PERMUTE_FAULT_SCALE today
    uint64_t delay_ns;    // DELAY, and the bound REORDER models
} permute_fault_rule_t;

typedef struct {
    permute_fault_rule_t rules[PERMUTE_MAX_FAULT_RULES];
    uint32_t count;
    permute_stream_t stream;
    uint64_t decisions;
    uint64_t fires;
} permute_fault_plan_t;

const char *permute_fault_site_name(uint8_t site);
const char *permute_fault_action_name(uint8_t action);

void permute_fault_init(permute_fault_plan_t *plan, uint64_t seed);
int permute_fault_add(permute_fault_plan_t *plan, uint8_t site, uint8_t action,
                      uint32_t numerator, uint32_t denominator,
                      uint64_t delay_ns);

// Parse `site:action@probability[:delay_ns]`, e.g. `send:drop@0.01`.
// Probability is a decimal in [0,1], converted without floating point.
int permute_fault_add_string(permute_fault_plan_t *plan, const char *text);

// Ask whether a rule fires at this site. Returns 1 and fills the action when
// it does; 0 when it does not. Advances only the D_FAULT counter.
int permute_fault_decide(permute_fault_plan_t *plan, uint8_t site,
                         uint8_t *action, uint64_t *delay_ns);

#endif // PERMUTE_FAULT_H
