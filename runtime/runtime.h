// SPDX-License-Identifier: Apache-2.0
//
// Runtime: the host-side executor and the bridge that implements the kernel
// operation table for guest code.
//
// Guest threads are host threads under a baton. Exactly one runs at a time;
// a shim call that blocks hands the baton back to the scheduler, which picks
// the next runnable guest thread from the ready queue. SUT code therefore runs
// unchanged, but its concurrency is decided by the model.

#ifndef PERMUTE_RUNTIME_H
#define PERMUTE_RUNTIME_H

#include <stdint.h>

#include "abi/permute_abi.h"
#include "kernel/state.h"
#include "kernel/trace.h"

typedef int (*permute_guest_main_fn)(int argc, char **argv);

typedef enum {
    PERMUTE_EXEC_CLEAN = 0,
    PERMUTE_EXEC_DEADLOCK,
    PERMUTE_EXEC_GUEST_FAILED,
    PERMUTE_EXEC_INTERNAL
} permute_exec_outcome_t;

typedef struct {
    permute_kernel_t *k;
    permute_handle_t process;
    permute_trace_t *trace;
    int argc;
    char **argv;
} permute_executor_t;

// Installs the host bridge. Must be called before the hypercall page is
// published, because the page carries this table.
void permute_bridge_init(permute_kernel_t *k, permute_trace_t *trace);
const permute_kernel_ops_t *permute_bridge_ops(void);

// Runs `main_fn` as the initial guest thread, then schedules until every
// thread finishes or the model reports a deadlock.
int permute_executor_run(permute_executor_t *ex, permute_guest_main_fn main_fn,
                         int *guest_rc, permute_exec_outcome_t *outcome);

// The calling guest thread gives up the CPU until the scheduler picks it.
// Called by the bridge after the kernel returns BLOCKED, and on a yield.
void permute_executor_park(void);

// Current guest thread handle for this host thread.
extern __thread permute_handle_t permute_runtime_current;

// pthread operations, implemented by the executor.
int permute_executor_thread_create(uint64_t *token, const pthread_attr_t *attr,
                                   void *(*start)(void *), void *arg);
int permute_executor_thread_join(uint64_t token, void **retval);
void permute_executor_thread_exit(void *retval);
int permute_executor_thread_cancel(uint64_t token);

#endif // PERMUTE_RUNTIME_H
