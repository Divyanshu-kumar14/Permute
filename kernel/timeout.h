// SPDX-License-Identifier: Apache-2.0
//
// TimerTimeout and the simulation step.
//
// Two behaviours here are worth stating before the code:
//
//   1. The timeout is an *intent* until the thread actually blocks in a state
//      the mask covers. Only then does an entry enter the heap. That is what
//      makes FR-3.22's latch work: arming for RECEIVE and blocking in SEND
//      leaves the intent intact for the next RECEIVE.
//   2. Disarm is `flags == 0` (vendor) **and** `ntime == NULL` (PRD 3.23).
//      QNX 8.0 makes `ntime == NULL` an immediate timeout instead; the shim
//      translates the polling idiom to a zero-length timeout. Recorded as
//      conflict C-09.

#ifndef PERMUTE_TIMEOUT_H
#define PERMUTE_TIMEOUT_H

#include "kernel/state.h"
#include "kernel/vclock.h"

permute_action_t permute_timeout_set(permute_kernel_t *k,
                                     permute_handle_t thread, uint32_t flags,
                                     const permute_sigevent_t *event,
                                     const uint64_t *ntime, uint64_t *otime,
                                     permute_result_t *res);

// Activate the armed timeout if the state the thread just entered is in the
// mask. Safe to call unconditionally after every block.
void permute_timeout_enter(permute_kernel_t *k, permute_thread_t *t);

// Apply one heap entry. Returns 1 if it did anything, 0 if it was stale.
int permute_timeout_fire(permute_kernel_t *k, const permute_timer_entry_t *e);

// One step: jump to the earliest deadline and fire everything due there.
permute_sim_outcome_t permute_sim_advance(permute_kernel_t *k);

// Keep stepping until something is runnable or nothing is left.
permute_sim_outcome_t permute_sim_run(permute_kernel_t *k);

uint32_t permute_timeout_state_bit(permute_thread_state_t s);

#endif // PERMUTE_TIMEOUT_H
