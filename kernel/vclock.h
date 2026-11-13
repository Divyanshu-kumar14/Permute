// SPDX-License-Identifier: Apache-2.0
//
// Virtual clock and the absolute timer heap.
//
// Time advances only when there is nothing else to do: the ready queue is
// empty and the earliest timer decides how far to jump. That is what makes a
// run reproducible without any wall-clock input, and it is why an empty ready
// queue plus an empty heap is a defined outcome (DEADLOCK) rather than a hang.

#ifndef PERMUTE_VCLOCK_H
#define PERMUTE_VCLOCK_H

#include "kernel/state.h"

typedef enum {
    PERMUTE_SIM_RUNNABLE = 0, // at least one thread can run
    PERMUTE_SIM_IDLE,         // events fired, still nothing runnable
    PERMUTE_SIM_DEADLOCK      // nothing runnable and nothing pending
} permute_sim_outcome_t;

void permute_eventq_init(permute_eventq_t *q);
int  permute_eventq_push(permute_eventq_t *q, uint64_t deadline_ns,
                         uint64_t seq, permute_handle_t target, uint8_t kind);
int  permute_eventq_peek(const permute_eventq_t *q, permute_timer_entry_t *out);
int  permute_eventq_pop(permute_eventq_t *q, permute_timer_entry_t *out);
uint32_t permute_eventq_count(const permute_eventq_t *q);
int  permute_eventq_check(const permute_eventq_t *q);

void permute_vclock_init(permute_kernel_t *k);
uint64_t permute_vclock_now(const permute_kernel_t *k);

// Advance to when_ns. Never goes backwards.
int permute_vclock_advance(permute_kernel_t *k, uint64_t when_ns);

#endif // PERMUTE_VCLOCK_H
