// SPDX-License-Identifier: Apache-2.0
//
// Kernel payload arena.
//
// Message payloads are copied into kernel-owned memory at send time, so a SUT
// pointer is never retained (FR-3.7). This arena is deliberately separate from
// the guest allocator (abi/alloc.c, P1-10): different memory, different owner,
// different lifetime.
//
// Twelve size classes, free lists threaded through free blocks, bump
// allocation from a fixed backing store. No libc, no malloc, no history
// dependence.

#ifndef PERMUTE_BLOB_H
#define PERMUTE_BLOB_H

#include <stddef.h>
#include <stdint.h>

#define PERMUTE_BLOB_CLASSES 12
#define PERMUTE_BLOB_ARENA_SIZE (4u << 20) /* 4 MiB */
#define PERMUTE_BLOB_HEADER 8u

typedef struct {
    uint8_t *arena;
    size_t size;
    size_t used;
    void *freelist[PERMUTE_BLOB_CLASSES];
    uint32_t class_size[PERMUTE_BLOB_CLASSES];
    uint64_t allocs;
    uint64_t frees;
} permute_blob_arena_t;

void permute_blob_init(permute_blob_arena_t *a, void *arena, size_t size);

// Returns NULL when n == 0 or when the arena is exhausted. Callers must treat
// NULL as a hard failure, never as a zero-length success.
void *permute_blob_alloc(permute_blob_arena_t *a, size_t n);
void permute_blob_free(permute_blob_arena_t *a, void *p);

// Usable bytes in the block that p points to, or 0 for NULL.
size_t permute_blob_usable(const void *p);

#endif // PERMUTE_BLOB_H
