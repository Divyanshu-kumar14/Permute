// SPDX-License-Identifier: Apache-2.0
//
// Live wait-for graph over kernel threads.
//
// This is not the post-hoc oracle (that is cli/permute/oracle). It answers one
// question at the moment the executor finds no runnable thread: which blocked
// threads are waiting on each other? The answer is printed by permute-run, and
// the trace oracle must reach the same cycle from the log. Two independent
// views of the same run are a cross-check, not duplicated policy.

#ifndef PERMUTE_ORACLE_H
#define PERMUTE_ORACLE_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/state.h"

#define PERMUTE_ORACLE_MAX_CYCLE 16

typedef struct {
    uint32_t tid[PERMUTE_ORACLE_MAX_CYCLE];
    uint8_t state[PERMUTE_ORACLE_MAX_CYCLE];
    int32_t rcvid[PERMUTE_ORACLE_MAX_CYCLE];
    uint32_t len;
} permute_oracle_cycle_t;

// Returns 1 and fills `cycle` when the blocked threads contain a wait cycle.
// Returns 0 when there is none. The lowest thread id starts the cycle, so the
// rendering is stable across runs.
int permute_oracle_deadlock(permute_kernel_t *k, permute_oracle_cycle_t *cycle);

// Renders `T1(REPLY 65537) -> T2(REPLY 65538) -> T1`. Returns the length that
// would have been written (as snprintf).
int permute_oracle_format_cycle(const permute_oracle_cycle_t *cycle, char *buf,
                                size_t len);

#endif // PERMUTE_ORACLE_H
