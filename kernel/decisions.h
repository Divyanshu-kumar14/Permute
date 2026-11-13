// SPDX-License-Identifier: Apache-2.0
//
// Decision domains and the per-run decision stream.
//
// Seven independent domains exist so that adding a call site in one area cannot
// shift the values seen by another. A fault-injection site added tomorrow must
// not change the scheduling stream; that is the whole point of the split.

#ifndef PERMUTE_DECISIONS_H
#define PERMUTE_DECISIONS_H

#include <stdint.h>

typedef enum {
    PERMUTE_D_SCHED = 0,   // which runnable thread runs next
    PERMUTE_D_TIMER_TIE,   // equal-deadline ordering
    PERMUTE_D_RECV_ORDER,  // equal-priority arrival ordering
    PERMUTE_D_QUANTUM,     // round-robin quantum variation
    PERMUTE_D_STIMULUS,    // when a stimulus fires
    PERMUTE_D_FAULT,       // whether a fault fires
    PERMUTE_D_CPUJITTER,   // modelled CPU jitter (unused on one core)
    PERMUTE_D_COUNT
} permute_domain_t;

typedef struct {
    uint64_t seed;
    uint64_t counter[PERMUTE_D_COUNT];
} permute_stream_t;

void permute_stream_init(permute_stream_t *s, uint64_t seed);

// Advances the domain counter and returns the decision for that index.
uint64_t permute_stream_next(permute_stream_t *s, permute_domain_t domain);

// Pure accessor: the value the domain would produce at `index`, without
// advancing anything. The shrinker uses this to reconstruct a decision that a
// tape did not override.
uint64_t permute_stream_at(const permute_stream_t *s, permute_domain_t domain,
                           uint64_t index);

#endif // PERMUTE_DECISIONS_H
