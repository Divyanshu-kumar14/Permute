// SPDX-License-Identifier: Apache-2.0
//
// Counter-based PRNG.
//
// A decision is a pure function of (seed, domain, decision index). There is no
// stream state, so forcing decision 17 cannot perturb decision 18 - which is the
// property that makes shrinking monotone (ADR-004).
//
// The Python shrinker reimplements this bit-for-bit; tools/seedrun.py checks
// that the two agree. Drift here would leave normal runs green while shrinking
// silently produced wrong results.

#ifndef PERMUTE_RNG_H
#define PERMUTE_RNG_H

#include <stdint.h>

// splitmix64 output function: add the golden gamma, then the two xor-shift
// multiply rounds. Returning the mixed value keeps the mapping from
// (seed, domain, index) to output a bijection on 64 bits.
uint64_t permute_splitmix64(uint64_t x);

// The only entropy entry point in the simulator. `domain` is one of the
// PERMUTE_D_* values (it is shifted into bits 40..63, so the limit is 2^24
// domains). `index` is the per-domain decision counter.
uint64_t permute_rnd(uint64_t seed, uint32_t domain, uint64_t index);

#endif // PERMUTE_RNG_H
