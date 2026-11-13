// SPDX-License-Identifier: Apache-2.0
//
// Ready queue: 256 FIFO buckets plus an occupancy bitmap (FR-3.11).
//
// Every operation here is O(1). `permute_readyq_check` is the debug invariant:
// it rebuilds the bitmap, the counts, and the links from the lists and fails
// if any of them disagree. The test suite calls it throughout a long random
// operation sequence, which is the only way the bitmap is allowed to be
// trusted.

#ifndef PERMUTE_READYQ_H
#define PERMUTE_READYQ_H

#include "kernel/state.h"

void permute_readyq_init(permute_readyq_t *q);

// slot is a thread-pool slot, prio is 0..255. `at_head` puts the thread ahead
// of equal-priority peers (a thread becoming ready preempts its peers);
// otherwise it joins the tail.
int permute_readyq_push(permute_readyq_t *q, uint32_t slot, uint32_t prio,
                        int at_head);
int permute_readyq_remove(permute_readyq_t *q, uint32_t slot);

// Pops the highest-priority head. Returns 0, or -EAGAIN when empty.
int permute_readyq_pop(permute_readyq_t *q, uint32_t *slot_out);
int permute_readyq_peek(const permute_readyq_t *q, uint32_t *slot_out);

uint32_t permute_readyq_count(const permute_readyq_t *q);
uint32_t permute_readyq_count_at(const permute_readyq_t *q, uint32_t prio);
int permute_readyq_contains(const permute_readyq_t *q, uint32_t slot);

// 0 when the bitmap, counts, total, and links all agree; -1 otherwise.
int permute_readyq_check(const permute_readyq_t *q);

#endif // PERMUTE_READYQ_H
