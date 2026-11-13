// SPDX-License-Identifier: Apache-2.0
//
// Deterministic seeded run fixture for replay tests.
//
//   replay_fixture <seed> <path.pmt>
//
// Prints DIGEST and RECORDS for the caller to compare. The scenario itself
// lives in kernel/scenario.c because permute-run uses the same one.

#include <stdio.h>
#include <stdlib.h>

#include "kernel/scenario.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <seed> <path.pmt>\n", argv[0]);
        return 2;
    }

    uint64_t seed = strtoull(argv[1], NULL, 0);
    uint64_t digest = 0;
    uint64_t records = 0;

    if (permute_scenario_run(seed, argv[2], &digest, &records) != 0)
        return 1;

    printf("DIGEST=%016llx\n", (unsigned long long)digest);
    printf("RECORDS=%llu\n", (unsigned long long)records);
    printf("CONFIG=%016llx\n", PERMUTE_SCENARIO_CONFIG_HASH);
    printf("BUILD=");
    for (int i = 0; i < 32; i++)
        printf("%02x", i);
    printf("\n");
    printf("KERNEL=1\n");
    return 0;
}
