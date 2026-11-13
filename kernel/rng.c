// SPDX-License-Identifier: Apache-2.0

#include "kernel/rng.h"

uint64_t permute_splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;

    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint64_t permute_rnd(uint64_t seed, uint32_t domain, uint64_t index)
{
    return permute_splitmix64(seed ^ ((uint64_t)domain << 40) ^ index);
}
