// SPDX-License-Identifier: Apache-2.0

#include "kernel/decisions.h"
#include "kernel/rng.h"

void permute_stream_init(permute_stream_t *s, uint64_t seed)
{
    s->seed = seed;
    for (int d = 0; d < PERMUTE_D_COUNT; d++)
        s->counter[d] = 0;
}

uint64_t permute_stream_next(permute_stream_t *s, permute_domain_t domain)
{
    return permute_stream_at(s, domain, s->counter[domain]++);
}

uint64_t permute_stream_at(const permute_stream_t *s, permute_domain_t domain,
                           uint64_t index)
{
    return permute_rnd(s->seed, (uint32_t)domain, index);
}
