// SPDX-License-Identifier: Apache-2.0
//
// permute-run: execute exactly one seed.
//
// This is the process the worker isolates. It runs either the built-in seeded
// scenario or a guest shared object, writes the trace, and exits. A SUT fault
// kills *this* process, never the worker.
//
//   permute-run <seed> <trace.pmt> [--guest PATH]
//
// PERMUTE_GUEST also selects a guest object. The worker uses that so it does
// not need to know about guest loading.
//
// Two environment hooks exist for the fault fixture:
//   PERMUTE_CRASH_SEED=<n>  raise SIGSEGV instead of running seed n
//   PERMUTE_SPIN_SEED=<n>   spin forever on seed n (exercises the wall budget)

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guest/dlmain.h"
#include "guest/hypercall.h"
#include "kernel/fault.h"
#include "kernel/objects.h"
#include "kernel/oracle.h"
#include "kernel/scenario.h"
#include "kernel/trace.h"
#include "runtime/runtime.h"

static void build_ids(uint8_t build_id[32], uint8_t run_id[16])
{
    for (int i = 0; i < 32; i++)
        build_id[i] = (uint8_t)i;
    for (int i = 0; i < 16; i++)
        run_id[i] = (uint8_t)(0x10 + i);
}

static int env_seed(const char *name, uint64_t *value)
{
    const char *text = getenv(name);
    if (text == NULL || text[0] == '\0')
        return 0;
    *value = strtoull(text, NULL, 0);
    return 1;
}

static permute_fault_plan_t g_faults;

static int install_faults(uint64_t seed)
{
    const char *text = getenv("PERMUTE_FAULTS");
    if (text == NULL || text[0] == '\0')
        return 0;

    permute_fault_init(&g_faults, seed);

    const char *p = text;
    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        size_t len = comma != NULL ? (size_t)(comma - p) : strlen(p);
        if (len == 0 || len >= 128) {
            fprintf(stderr, "permute-run: bad fault rule\n");
            return -1;
        }

        char rule[128];
        memcpy(rule, p, len);
        rule[len] = '\0';

        if (permute_fault_add_string(&g_faults, rule) != 0) {
            fprintf(stderr, "permute-run: bad fault rule '%s'\n", rule);
            return -1;
        }

        p = comma != NULL ? comma + 1 : p + len;
    }

    permute_bridge_set_faults(&g_faults);
    return 0;
}

static const char *outcome_name(permute_exec_outcome_t outcome)
{
    switch (outcome) {
    case PERMUTE_EXEC_CLEAN:
        return "CLEAN";
    case PERMUTE_EXEC_DEADLOCK:
        return "DEADLOCK";
    case PERMUTE_EXEC_GUEST_FAILED:
        return "GUEST_FAILED";
    default:
        return "INTERNAL";
    }
}

static int run_guest(uint64_t seed, const char *trace_path,
                     const char *guest_path)
{
    static permute_kernel_t kernel;
    static permute_trace_t trace;

    permute_kernel_init(&kernel, seed);

    uint8_t build_id[32], run_id[16];
    build_ids(build_id, run_id);
    if (permute_trace_open(&trace, trace_path, PERMUTE_SCENARIO_CONFIG_HASH,
                           build_id, run_id, 0) != 0) {
        fprintf(stderr, "permute-run: cannot open trace %s\n", trace_path);
        return 7;
    }

    permute_handle_t process;
    if (permute_process_create(&kernel, "guest", &process) != 0) {
        fprintf(stderr, "permute-run: cannot create process\n");
        return 7;
    }

    uintptr_t page = permute_hypercall_init();
    if (page == 0) {
        fprintf(stderr, "permute-run: hypercall page unavailable\n");
        return 7;
    }

    permute_bridge_init(&kernel, &trace);
    if (install_faults(seed) != 0)
        return 7;
    if (permute_ops_install(permute_bridge_ops()) != 0) {
        fprintf(stderr, "permute-run: cannot install the operation table\n");
        return 7;
    }

    permute_guest_t guest;
    if (permute_guest_load(&guest, guest_path) != 0) {
        fprintf(stderr, "permute-run: %s\n", guest.error);
        return 7;
    }
    if (permute_guest_bind(&guest, page) != 0) {
        fprintf(stderr, "permute-run: %s\n", guest.error);
        return 7;
    }

    permute_executor_t ex;
    memset(&ex, 0, sizeof ex);
    ex.k = &kernel;
    ex.process = process;
    ex.trace = &trace;

    int guest_rc = 0;
    permute_exec_outcome_t outcome = PERMUTE_EXEC_INTERNAL;
    if (permute_executor_run(&ex, guest.main_fn, &guest_rc, &outcome) != 0) {
        fprintf(stderr, "permute-run: executor failed\n");
        return 7;
    }

    char cycle_text[256];
    cycle_text[0] = '\0';
    if (outcome == PERMUTE_EXEC_DEADLOCK) {
        permute_oracle_cycle_t cycle;
        if (permute_oracle_deadlock(&kernel, &cycle))
            permute_oracle_format_cycle(&cycle, cycle_text, sizeof cycle_text);
    }

    uint64_t digest = 0;
    if (permute_trace_close(&trace, &digest) != 0) {
        fprintf(stderr, "permute-run: trace close failed\n");
        return 7;
    }

    printf("SEED=%llu\n", (unsigned long long)seed);
    printf("OUTCOME=%s\n", outcome_name(outcome));
    printf("GUEST_RC=%d\n", guest_rc);
    printf("DIGEST=%016llx\n", (unsigned long long)digest);
    printf("RECORDS=%llu\n",
           (unsigned long long)trace.n_records_total);
    if (cycle_text[0] != '\0')
        printf("CYCLE=%s\n", cycle_text);

    if (outcome != PERMUTE_EXEC_CLEAN || guest_rc != 0)
        return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <seed> <trace.pmt> [--guest PATH]\n",
                argv[0]);
        return 2;
    }

    uint64_t seed = strtoull(argv[1], NULL, 0);
    const char *trace = argv[2];

    const char *guest = getenv("PERMUTE_GUEST");
    if (guest != NULL && guest[0] == '\0')
        guest = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--guest") == 0 && i + 1 < argc)
            guest = argv[++i];
    }

    uint64_t value = 0;
    if (env_seed("PERMUTE_CRASH_SEED", &value) && value == seed) {
        // Stand-in for a SUT fault. A null write terminates this process
        // with SIGSEGV, which is what the worker must survive. (signal.h
        // would collide with the QNX-layout sigevent in the ABI header.)
        *(volatile int *)(uintptr_t)0 = 1;
    }

    if (env_seed("PERMUTE_SPIN_SEED", &value) && value == seed) {
        volatile uint64_t spin = 0;
        for (;;) {
            spin++;
            if (spin == 0) // impossible; the read keeps the loop honest
                break;
        }
    }

    if (guest != NULL)
        return run_guest(seed, trace, guest);

    uint64_t digest = 0;
    uint64_t records = 0;
    if (permute_scenario_run(seed, trace, &digest, &records) != 0)
        return 7;

    printf("SEED=%llu\n", (unsigned long long)seed);
    printf("OUTCOME=CLEAN\n");
    printf("GUEST_RC=0\n");
    printf("DIGEST=%016llx\n", (unsigned long long)digest);
    printf("RECORDS=%llu\n", (unsigned long long)records);
    return 0;
}
