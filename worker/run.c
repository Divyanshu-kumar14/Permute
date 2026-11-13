// SPDX-License-Identifier: Apache-2.0
//
// permute-run: execute exactly one seed.
//
// This is the process the worker isolates. It runs the seeded scenario, writes
// the trace, and exits. A SUT fault kills *this* process, never the worker.
//
//   permute-run <seed> <trace.pmt>
//
// Two environment hooks exist for the fault fixture, so the worker's crash
// path can be tested without a real bug:
//   PERMUTE_CRASH_SEED=<n>  raise SIGSEGV instead of running seed n
//   PERMUTE_SPIN_SEED=<n>   spin forever on seed n (exercises the wall budget)

#define _GNU_SOURCE

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "kernel/scenario.h"

static int env_seed(const char *name, uint64_t *value)
{
    const char *text = getenv(name);
    if (text == NULL || text[0] == '\0')
        return 0;
    *value = strtoull(text, NULL, 0);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <seed> <trace.pmt>\n", argv[0]);
        return 2;
    }

    uint64_t seed = strtoull(argv[1], NULL, 0);
    const char *trace = argv[2];

    uint64_t value = 0;
    if (env_seed("PERMUTE_CRASH_SEED", &value) && value == seed) {
        // Stand-in for a SUT fault. The default disposition terminates this
        // process with SIGSEGV, which is what the worker must survive.
        raise(SIGSEGV);
    }

    if (env_seed("PERMUTE_SPIN_SEED", &value) && value == seed) {
        volatile uint64_t spin = 0;
        for (;;) {
            spin++;
            if (spin == 0) // impossible; the read keeps the loop honest
                break;
        }
    }

    uint64_t digest = 0;
    uint64_t records = 0;
    if (permute_scenario_run(seed, trace, &digest, &records) != 0)
        return 7;

    printf("SEED=%llu\n", (unsigned long long)seed);
    printf("DIGEST=%016llx\n", (unsigned long long)digest);
    printf("RECORDS=%llu\n", (unsigned long long)records);
    return 0;
}
