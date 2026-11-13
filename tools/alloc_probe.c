// SPDX-License-Identifier: Apache-2.0
//
// Interposed-allocator probe.
//
// This executable links abi/alloc.c without PERMUTE_ALLOC_PREFIX, so the whole
// process — including stdio — uses the deterministic allocator. It exercises
// the public malloc family exactly as a guest would, and checks that a libc
// pointer is recognised as foreign and forwarded rather than corrupted.
//
//   alloc_probe <ops> <seed>
//
// Prints DIGEST, HIGH_WATER, LIVE, OWNS_LIBC and RSS_KB.

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "abi/alloc.h"

enum { MAX_LIVE = 50000 };

static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v;
    h *= 1099511628211ull;
    return h;
}

static uint64_t offset_of(const void *p)
{
    return (uint64_t)((const uint8_t *)p - (const uint8_t *)permute_alloc_base());
}

int main(int argc, char **argv)
{
    int ops = argc > 1 ? atoi(argv[1]) : 200000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;

    static void *live[MAX_LIVE];
    int live_n = 0;
    uint64_t h = 1469598103934665603ull;

    // A pointer this process did not allocate: address-range dispatch must
    // recognise it and forward the free.
    extern void *__libc_malloc(size_t);
    void *foreign = __libc_malloc(64);
    int owns_libc = permute_alloc_owns(foreign);
    free(foreign);

    for (int i = 0; i < ops; i++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t r = (uint32_t)(seed >> 33);

        int do_alloc = (live_n == 0) || (live_n < MAX_LIVE && (r & 1u));
        if (do_alloc) {
            size_t size = 1 + (r >> 8) % 256;
            void *p;

            switch (r % 8u) {
            case 0:
                p = calloc(1, size);
                break;
            case 1: {
                p = NULL;
                if (posix_memalign(&p, 64, size) != 0)
                    p = NULL;
                break;
            }
            case 2:
                p = strdup("permute");
                size = 8;
                break;
            default:
                p = malloc(size);
                break;
            }

            if (p == NULL) {
                fprintf(stderr, "allocation failed at %d\n", i);
                return 1;
            }
            memset(p, 0x5A, size > 8 ? 8 : size);
            live[live_n++] = p;
            h = mix(h, offset_of(p) / 16);
            h = mix(h, size);
        } else {
            int idx = (int)((r >> 8) % (uint32_t)live_n);
            void *p = live[idx];
            h = mix(h, offset_of(p) / 16);
            h = mix(h, 0xFFFF);

            // Occasionally realloc instead of freeing.
            if ((r & 0x100u) != 0) {
                size_t grow = 1 + (r >> 9) % 300;
                void *q = realloc(p, grow);
                if (q == NULL) {
                    fprintf(stderr, "realloc failed at %d\n", i);
                    return 1;
                }
                h = mix(h, offset_of(q) / 16);
                h = mix(h, grow);
                p = q;
            }

            free(p);
            live[idx] = live[--live_n];
        }
    }

    for (int i = 0; i < live_n; i++)
        free(live[i]);

    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return 1;

    printf("DIGEST=%016llx\n", (unsigned long long)h);
    printf("HIGH_WATER=%zu\n", permute_alloc_high_water());
    printf("LIVE=%llu\n", (unsigned long long)permute_alloc_live_blocks());
    printf("OWNS_LIBC=%d\n", owns_libc);
    printf("RSS_KB=%ld\n", ru.ru_maxrss);
    return 0;
}
