// SPDX-License-Identifier: Apache-2.0
//
// P0-06 spike: prove the determinism bootstrap, not just assume it.
//
// Four independent checks, each of which can fail on its own:
//   1. repeatability   - two fresh streams at the same seed agree
//   2. domain isolation - draining one domain does not move another
//   3. locality        - a value depends only on (seed, domain, index)
//   4. seed separation - different seeds produce different streams
//
// Output is machine-readable so tools/seedrun.py can check layout stability
// across separate process launches.

#define _GNU_SOURCE

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/decisions.h"
#include "kernel/determinism.h"
#include "kernel/rng.h"

#define DRAW_ROUNDS 64

static uint64_t fnv1a_update(uint64_t h, const void *data, size_t len)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// A synthetic "run": DRAW_ROUNDS rounds, one decision per domain per round,
// hashed in order. Sensitive to the interleaving pattern, which is what a real
// trace digest must also be.
static uint64_t stream_digest(uint64_t seed)
{
    permute_stream_t s;
    permute_stream_init(&s, seed);

    uint64_t h = 0xCBF29CE484222325ull;
    for (int round = 0; round < DRAW_ROUNDS; round++) {
        for (int d = 0; d < PERMUTE_D_COUNT; d++) {
            uint64_t v = permute_stream_next(&s, (permute_domain_t)d);
            uint32_t tag[2] = { (uint32_t)d, (uint32_t)round };
            h = fnv1a_update(h, tag, sizeof tag);
            h = fnv1a_update(h, &v, sizeof v);
        }
    }
    return h;
}

int main(int argc, char **argv)
{
    permute_aslr_status_t aslr = permute_determinism_init(argc, argv);

    uint64_t seed = 0;
    int vectors = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--vectors") == 0 && i + 1 < argc) {
            vectors = atoi(argv[++i]);
        } else {
            fprintf(stderr, "usage: %s [--seed N] [--vectors N]\n", argv[0]);
            return 2;
        }
    }

    // Take the layout fingerprint before any stdio output: the first printf
    // allocates the stdout buffer and would perturb [heap].
    uint64_t maps = permute_maps_fingerprint();

    if (vectors > 0) {
        for (int i = 0; i < vectors; i++) {
            uint32_t d = (uint32_t)(i % PERMUTE_D_COUNT);
            uint64_t idx = (uint64_t)(i / PERMUTE_D_COUNT);
            uint64_t v = permute_rnd(seed, d, idx);
            printf("%" PRIu64 ",%u,%" PRIu64 ",%016" PRIx64 "\n",
                   seed, d, idx, v);
        }
        return 0;
    }

    int passed = 0;
    int total = 4;

    // 1. repeatability
    uint64_t d1 = stream_digest(seed);
    uint64_t d2 = stream_digest(seed);
    int ok1 = (d1 == d2);
    passed += ok1;
    printf("CHECK repeatability %s\n", ok1 ? "PASS" : "FAIL");

    // 2. domain isolation: draining one domain must not advance another's
    // counter. Read the *next* value from both streams, not the pure accessor.
    permute_stream_t drained;
    permute_stream_init(&drained, seed);
    for (int i = 0; i < 10; i++)
        (void)permute_stream_next(&drained, PERMUTE_D_SCHED);
    uint64_t fault_after = permute_stream_next(&drained, PERMUTE_D_FAULT);

    permute_stream_t fresh;
    permute_stream_init(&fresh, seed);
    uint64_t fault_fresh = permute_stream_next(&fresh, PERMUTE_D_FAULT);

    int ok2 = (fault_after == fault_fresh);
    passed += ok2;
    printf("CHECK domain_isolation %s\n", ok2 ? "PASS" : "FAIL");

    // 3. locality: the nth draw of a domain is permute_rnd(seed, domain, n),
    // independent of access order and of other domains.
    permute_stream_t seq;
    permute_stream_init(&seq, seed);
    (void)permute_stream_next(&seq, PERMUTE_D_STIMULUS);
    (void)permute_stream_next(&seq, PERMUTE_D_STIMULUS);

    uint64_t v0 = permute_stream_next(&seq, PERMUTE_D_RECV_ORDER);
    uint64_t v1 = permute_stream_next(&seq, PERMUTE_D_RECV_ORDER);
    uint64_t v2 = permute_stream_next(&seq, PERMUTE_D_RECV_ORDER);
    uint64_t v3 = permute_stream_at(&seq, PERMUTE_D_RECV_ORDER, 3);

    int ok3 = (v0 == permute_rnd(seed, PERMUTE_D_RECV_ORDER, 0)) &&
              (v1 == permute_rnd(seed, PERMUTE_D_RECV_ORDER, 1)) &&
              (v2 == permute_rnd(seed, PERMUTE_D_RECV_ORDER, 2)) &&
              (v3 == permute_rnd(seed, PERMUTE_D_RECV_ORDER, 3));
    passed += ok3;
    printf("CHECK locality %s\n", ok3 ? "PASS" : "FAIL");

    // 4. seed separation
    uint64_t other = stream_digest(seed + 1);
    int ok4 = (d1 != other);
    passed += ok4;
    printf("CHECK seed_separation %s\n", ok4 ? "PASS" : "FAIL");

    printf("ASLR=%s\n",
           aslr == PERMUTE_ASLR_OFF ? "off" : "unavailable");
    printf("PERSONA=%d\n", permute_aslr_bit_observed());
    printf("MAPS=%016" PRIx64 "\n", maps);
    printf("SEED=%" PRIu64 "\n", seed);
    printf("DIGEST=%016" PRIx64 "\n", d1);
    printf("CHECKS=%d/%d\n", passed, total);

    return (passed == total) ? 0 : 1;
}
