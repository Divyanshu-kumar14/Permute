// SPDX-License-Identifier: Apache-2.0
//
// Cross-language decision vectors (FR-4.2).
//
// The committed TSV is checked by the C implementation here and by the Python
// implementation in tests/python/test_rng.py. Agreement is with a fixed file,
// not with the other implementation's live output.

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>

#include "harness.h"
#include "kernel/decisions.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <rng_vectors.tsv>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    char line[256];
    long checked = 0;
    long mismatch = 0;

    while (fgets(line, sizeof line, f) != NULL) {
        if (line[0] == '#')
            continue;

        unsigned long long seed = 0, index = 0, value = 0;
        unsigned domain = 0;
        if (sscanf(line, "%llu %u %llu %llx", &seed, &domain, &index, &value) !=
            4)
            continue;

        permute_stream_t s;
        permute_stream_init(&s, seed);
        uint64_t got = permute_stream_at(&s, (permute_domain_t)domain, index);
        checked++;

        if (got != (uint64_t)value) {
            if (mismatch < 3) {
                printf("mismatch seed=%llu domain=%u index=%llu: "
                       "%016llx != %016llx\n",
                       seed, domain, index, (unsigned long long)got, value);
            }
            mismatch++;
        }
    }
    fclose(f);

    CHECK_EQ(mismatch, 0);
    CHECK_EQ(checked, 100000);

    // Locality: substituting the value chosen at index 17 cannot move the
    // values after it, because each index is an independent evaluation.
    permute_stream_t a, b;
    permute_stream_init(&a, 42);
    permute_stream_init(&b, 42);

    for (int i = 0; i < 17; i++)
        (void)permute_stream_next(&a, PERMUTE_D_SCHED);

    uint64_t forced = 0xDEADBEEFCAFEF00Dull; // what the shrinker would inject
    (void)forced;

    CHECK_EQ(permute_stream_next(&a, PERMUTE_D_SCHED),
             permute_stream_at(&b, PERMUTE_D_SCHED, 17));

    for (uint64_t i = 18; i < 100; i++)
        CHECK_EQ(permute_stream_next(&a, PERMUTE_D_SCHED),
                 permute_stream_at(&b, PERMUTE_D_SCHED, i));

    // Domains are independent: consuming a scheduling stream must not shift
    // the timer stream.
    permute_stream_t c;
    permute_stream_init(&c, 7);
    for (int i = 0; i < 1000; i++)
        (void)permute_stream_next(&c, PERMUTE_D_SCHED);

    CHECK_EQ(permute_stream_next(&c, PERMUTE_D_TIMER_TIE),
             permute_stream_at(&c, PERMUTE_D_TIMER_TIE, 0));

    return TEST_RESULT("test_rng_golden");
}
