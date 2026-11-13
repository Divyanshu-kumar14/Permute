// SPDX-License-Identifier: Apache-2.0
//
// Scheduler policy on top of the ready queue.
//
// Dispatch decisions return the handle that should run next rather than a
// bespoke "did we switch" enum: the caller compares it with the previous
// handle. That keeps one meaning for each value and avoids inventing a third
// state that would need to be kept consistent with CONTINUE/BLOCKED.

#ifndef PERMUTE_SCHED_H
#define PERMUTE_SCHED_H

#include "kernel/state.h"

void permute_sched_init(permute_kernel_t *k);

// Make a READY thread runnable. at_head puts it ahead of equal-priority peers.
int permute_sched_add(permute_kernel_t *k, permute_handle_t h, int at_head);
int permute_sched_remove(permute_kernel_t *k, permute_handle_t h);

// Re-place a queued thread after its effective priority changed. No-op if the
// thread is not currently queued (it is running, or blocked).
int permute_sched_move(permute_kernel_t *k, permute_handle_t h, int at_head);

// Highest-priority runnable thread, moved to RUNNING. NONE if nothing is
// ready.
permute_handle_t permute_sched_pick(permute_kernel_t *k);

// Charge dt_ns to the running thread and expire an RR timeslice if it is
// spent. Returns the thread that should be running afterwards.
permute_handle_t permute_sched_tick(permute_kernel_t *k, uint64_t dt_ns);

// SchedYield: yields only to a thread at the same priority. Returns the
// thread that should be running afterwards.
permute_handle_t permute_sched_yield(permute_kernel_t *k, permute_handle_t self);

permute_handle_t permute_sched_running(const permute_kernel_t *k);

// Debug invariant: queue internals agree, and the running thread is not also
// sitting in the queue.
int permute_sched_check(const permute_kernel_t *k);

#endif // PERMUTE_SCHED_H
