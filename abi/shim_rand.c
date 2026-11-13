// SPDX-License-Identifier: Apache-2.0
//
// Entropy shims (FR-4.11).
//
// rand/random must be a pure function of the run seed and the number of draws.
// libc's generator is seeded from the host and is not specified across
// versions, so a guest that called it would produce a different program on
// every machine.
//
// The kernel supplies the per-process origin; this file expands it with the
// same splitmix64 used everywhere else.

#include <errno.h>

#include "abi/permute_iface.h"
#include "guest/hypercall.h"

static uint64_t g_state;
static int g_seeded;

static uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint64_t process_seed(void)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->rand_seed == NULL)
        return 0;
    return ops->rand_seed();
}

static uint64_t next_value(void)
{
    if (!g_seeded) {
        g_state = process_seed();
        g_seeded = 1;
    }
    g_state = splitmix64(g_state);
    return g_state >> 33;
}

void srand(unsigned seed)
{
    g_state = process_seed() ^ (uint64_t)seed;
    g_seeded = 1;
}

void srandom(unsigned seed)
{
    srand(seed);
}

int rand(void)
{
    return (int)(next_value() & 0x7FFFFFFFu);
}

long random(void)
{
    return (long)(next_value() & 0x7FFFFFFFu);
}
