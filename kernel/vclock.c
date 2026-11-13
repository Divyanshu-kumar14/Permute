// SPDX-License-Identifier: Apache-2.0
//
// The virtual clock. Time is a counter, never a reading.

#include "kernel/vclock.h"

#include <errno.h>

void permute_vclock_init(permute_kernel_t *k)
{
    k->now_ns = 0;
}

uint64_t permute_vclock_now(const permute_kernel_t *k)
{
    return k->now_ns;
}

int permute_vclock_advance(permute_kernel_t *k, uint64_t when_ns)
{
    if (when_ns < k->now_ns)
        return -EINVAL; // time never runs backwards
    k->now_ns = when_ns;
    return 0;
}
