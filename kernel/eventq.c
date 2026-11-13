// SPDX-License-Identifier: Apache-2.0

#include "kernel/vclock.h"

#include <errno.h>
#include <string.h>

static int entry_before(const permute_timer_entry_t *a,
                        const permute_timer_entry_t *b)
{
    if (a->deadline_ns != b->deadline_ns)
        return a->deadline_ns < b->deadline_ns;
    return a->seq < b->seq;
}

void permute_eventq_init(permute_eventq_t *q)
{
    memset(q, 0, sizeof *q);
}

int permute_eventq_push(permute_eventq_t *q, uint64_t deadline_ns,
                        uint64_t seq, permute_handle_t target, uint8_t kind)
{
    if (q->count >= PERMUTE_MAX_TIMERS)
        return -EAGAIN;

    uint32_t i = q->count++;
    q->heap[i].deadline_ns = deadline_ns;
    q->heap[i].seq = seq;
    q->heap[i].target = target;
    q->heap[i].kind = kind;

    while (i > 0) {
        uint32_t parent = (i - 1) / 2;
        if (!entry_before(&q->heap[i], &q->heap[parent]))
            break;
        permute_timer_entry_t tmp = q->heap[i];
        q->heap[i] = q->heap[parent];
        q->heap[parent] = tmp;
        i = parent;
    }
    return 0;
}

int permute_eventq_peek(const permute_eventq_t *q, permute_timer_entry_t *out)
{
    if (q->count == 0)
        return -EAGAIN;
    *out = q->heap[0];
    return 0;
}

static void sift_down(permute_eventq_t *q, uint32_t i)
{
    for (;;) {
        uint32_t left = 2 * i + 1;
        uint32_t right = left + 1;
        uint32_t best = i;

        if (left < q->count && entry_before(&q->heap[left], &q->heap[best]))
            best = left;
        if (right < q->count && entry_before(&q->heap[right], &q->heap[best]))
            best = right;
        if (best == i)
            return;

        permute_timer_entry_t tmp = q->heap[i];
        q->heap[i] = q->heap[best];
        q->heap[best] = tmp;
        i = best;
    }
}

int permute_eventq_pop(permute_eventq_t *q, permute_timer_entry_t *out)
{
    if (q->count == 0)
        return -EAGAIN;

    *out = q->heap[0];
    q->count--;
    if (q->count > 0) {
        q->heap[0] = q->heap[q->count];
        sift_down(q, 0);
    }
    return 0;
}

uint32_t permute_eventq_count(const permute_eventq_t *q)
{
    return q->count;
}

int permute_eventq_check(const permute_eventq_t *q)
{
    if (q->count > PERMUTE_MAX_TIMERS)
        return -1;

    for (uint32_t i = 1; i < q->count; i++) {
        uint32_t parent = (i - 1) / 2;
        if (entry_before(&q->heap[i], &q->heap[parent]))
            return -1; // heap order violated
    }
    return 0;
}
