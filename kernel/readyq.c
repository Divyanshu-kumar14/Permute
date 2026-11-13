// SPDX-License-Identifier: Apache-2.0

#include "kernel/readyq.h"

#include <errno.h>
#include <string.h>

static inline int bit_test(const permute_readyq_t *q, uint32_t prio)
{
    return (q->bitmap[prio >> 6] >> (prio & 63)) & 1u;
}

static inline void bit_set(permute_readyq_t *q, uint32_t prio)
{
    q->bitmap[prio >> 6] |= 1ull << (prio & 63);
}

static inline void bit_clear(permute_readyq_t *q, uint32_t prio)
{
    q->bitmap[prio >> 6] &= ~(1ull << (prio & 63));
}

void permute_readyq_init(permute_readyq_t *q)
{
    memset(q, 0, sizeof *q);
    for (uint32_t i = 0; i < PERMUTE_PRIO_BUCKETS; i++) {
        q->head[i] = PERMUTE_RQ_NONE;
        q->tail[i] = PERMUTE_RQ_NONE;
    }
    for (uint32_t i = 0; i < PERMUTE_MAX_THREADS; i++) {
        q->prio[i] = 0;
        q->next[i] = PERMUTE_RQ_NONE;
        q->prev[i] = PERMUTE_RQ_NONE;
        q->on[i] = 0;
    }
}

int permute_readyq_push(permute_readyq_t *q, uint32_t slot, uint32_t prio,
                        int at_head)
{
    if (slot >= PERMUTE_MAX_THREADS || prio >= PERMUTE_PRIO_BUCKETS)
        return -EINVAL;
    if (q->on[slot])
        return -EEXIST;

    q->prio[slot] = (uint16_t)prio;
    q->on[slot] = 1;

    if (at_head) {
        uint16_t first = q->head[prio];
        q->next[slot] = first;
        q->prev[slot] = PERMUTE_RQ_NONE;
        if (first != PERMUTE_RQ_NONE)
            q->prev[first] = (uint16_t)slot;
        else
            q->tail[prio] = (uint16_t)slot;
        q->head[prio] = (uint16_t)slot;
    } else {
        uint16_t last = q->tail[prio];
        q->prev[slot] = last;
        q->next[slot] = PERMUTE_RQ_NONE;
        if (last != PERMUTE_RQ_NONE)
            q->next[last] = (uint16_t)slot;
        else
            q->head[prio] = (uint16_t)slot;
        q->tail[prio] = (uint16_t)slot;
    }

    q->count[prio]++;
    q->total++;
    bit_set(q, prio);
    return 0;
}

int permute_readyq_remove(permute_readyq_t *q, uint32_t slot)
{
    if (slot >= PERMUTE_MAX_THREADS)
        return -EINVAL;
    if (!q->on[slot])
        return -ENOENT;

    uint32_t prio = q->prio[slot];
    uint16_t prev = q->prev[slot];
    uint16_t next = q->next[slot];

    if (prev != PERMUTE_RQ_NONE)
        q->next[prev] = next;
    else
        q->head[prio] = next;

    if (next != PERMUTE_RQ_NONE)
        q->prev[next] = prev;
    else
        q->tail[prio] = prev;

    q->on[slot] = 0;
    q->next[slot] = PERMUTE_RQ_NONE;
    q->prev[slot] = PERMUTE_RQ_NONE;
    q->count[prio]--;
    q->total--;

    if (q->count[prio] == 0)
        bit_clear(q, prio);
    return 0;
}

static int highest_priority(const permute_readyq_t *q, uint32_t *prio_out)
{
    // Four words, checked from the top. Bounded, not a scan over threads.
    for (int w = (PERMUTE_PRIO_BUCKETS / 64) - 1; w >= 0; w--) {
        uint64_t bits = q->bitmap[w];
        if (bits == 0)
            continue;
        uint32_t bit = 63u - (uint32_t)__builtin_clzll(bits);
        *prio_out = (uint32_t)w * 64u + bit;
        return 0;
    }
    return -EAGAIN;
}

int permute_readyq_pop(permute_readyq_t *q, uint32_t *slot_out)
{
    uint32_t prio;
    if (highest_priority(q, &prio) != 0)
        return -EAGAIN;

    uint16_t slot = q->head[prio];
    if (slot == PERMUTE_RQ_NONE)
        return -EAGAIN; // bitmap says occupied but the bucket is empty

    permute_readyq_remove(q, slot);
    *slot_out = slot;
    return 0;
}

int permute_readyq_peek(const permute_readyq_t *q, uint32_t *slot_out)
{
    uint32_t prio;
    if (highest_priority(q, &prio) != 0)
        return -EAGAIN;
    *slot_out = q->head[prio];
    return 0;
}

uint32_t permute_readyq_count(const permute_readyq_t *q)
{
    return q->total;
}

uint32_t permute_readyq_count_at(const permute_readyq_t *q, uint32_t prio)
{
    return prio < PERMUTE_PRIO_BUCKETS ? q->count[prio] : 0;
}

int permute_readyq_contains(const permute_readyq_t *q, uint32_t slot)
{
    return slot < PERMUTE_MAX_THREADS && q->on[slot];
}

int permute_readyq_check(const permute_readyq_t *q)
{
    uint32_t counted = 0;

    for (uint32_t prio = 0; prio < PERMUTE_PRIO_BUCKETS; prio++) {
        uint32_t n = 0;
        uint16_t prev = PERMUTE_RQ_NONE;
        uint16_t slot = q->head[prio];

        while (slot != PERMUTE_RQ_NONE) {
            if (slot >= PERMUTE_MAX_THREADS)
                return -1;
            if (!q->on[slot])
                return -1;
            if (q->prio[slot] != prio)
                return -1;
            if (q->prev[slot] != prev)
                return -1;
            if (n > PERMUTE_MAX_THREADS)
                return -1; // cycle
            n++;
            prev = slot;
            slot = q->next[slot];
        }

        if (prev != q->tail[prio])
            return -1;
        if (n != q->count[prio])
            return -1;
        if ((n != 0) != (uint32_t)bit_test(q, prio))
            return -1;
        counted += n;
    }

    if (counted != q->total)
        return -1;

    // A slot marked present must be reachable from exactly one bucket; the
    // walk above proved reachability for whatever the lists contain, and the
    // count comparison proves nothing extra is marked.
    for (uint32_t slot = 0; slot < PERMUTE_MAX_THREADS; slot++) {
        if (q->on[slot])
            continue;
        if (q->next[slot] != PERMUTE_RQ_NONE || q->prev[slot] != PERMUTE_RQ_NONE)
            return -1;
    }

    return 0;
}
