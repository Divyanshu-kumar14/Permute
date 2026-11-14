// SPDX-License-Identifier: Apache-2.0
//
// Context-switch microbenchmark: run a guest that yields between two threads
// and report nanoseconds per cooperative switch. The trace is disabled so the
// measurement is the executor handoff (semaphores + scheduler + shim), not log
// formatting.
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "guest/dlmain.h"
#include "guest/hypercall.h"
#include "kernel/objects.h"
#include "runtime/runtime.h"

#define SWITCHES (2 * 20000) // two threads, YIELD_COUNT each

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <guest.so>\n", argv[0]);
        return 2;
    }

    static permute_kernel_t kernel;
    permute_kernel_init(&kernel, 1);

    permute_handle_t process;
    if (permute_process_create(&kernel, "bench", &process) != 0)
        return 1;

    uintptr_t page = permute_hypercall_init();
    if (page == 0)
        return 1;

    permute_bridge_init(&kernel, NULL);
    if (permute_ops_install(permute_bridge_ops()) != 0)
        return 1;

    permute_guest_t guest;
    if (permute_guest_load(&guest, argv[1]) != 0) {
        fprintf(stderr, "%s\n", guest.error);
        return 1;
    }
    if (permute_guest_bind(&guest, page) != 0)
        return 1;

    permute_executor_t ex;
    memset(&ex, 0, sizeof ex);
    ex.k = &kernel;
    ex.process = process;

    int rc = 0;
    permute_exec_outcome_t outcome = PERMUTE_EXEC_INTERNAL;
    uint64_t start = now_ns();
    if (permute_executor_run(&ex, guest.main_fn, &rc, &outcome) != 0)
        return 1;
    uint64_t elapsed = now_ns() - start;

    if (outcome != PERMUTE_EXEC_CLEAN || rc != 0) {
        fprintf(stderr, "bench guest failed: outcome=%d rc=%d\n", outcome, rc);
        return 1;
    }

    printf("SWITCHES=%d\n", SWITCHES);
    printf("ELAPSED_NS=%llu\n", (unsigned long long)elapsed);
    printf("SWITCH_NS=%llu\n",
           (unsigned long long)(elapsed / SWITCHES));
    return 0;
}
