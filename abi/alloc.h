// SPDX-License-Identifier: Apache-2.0
//
// Deterministic allocator.
//
// A 2 GiB region is reserved once at a fixed address with PROT_NONE and
// committed lazily in 2 MiB windows, so reserving costs no RSS. Blocks come
// from a bump pointer and are recycled through per-size-class free lists that
// stay ordered by offset: an allocation always returns the lowest-addressed
// free block of its class. That is what makes the address sequence a pure
// function of the allocation history rather than of libc's mood.
//
// The arena base is recorded, not assumed. If the fixed address is taken the
// allocator falls back to an arbitrary mapping; offsets are unchanged, so a
// digest computed from offsets stays comparable across hosts.
//
// Pointers outside the arena belong to libc. `permute_alloc_owns` answers that
// question; the interposed `free`/`realloc` use it to forward rather than
// corrupt. A host application legitimately receives libc pointers from
// `getline`, `realpath`, `dlopen` and friends.

#ifndef PERMUTE_ALLOC_H
#define PERMUTE_ALLOC_H

#include <stddef.h>
#include <stdint.h>

#define PERMUTE_ARENA_SIZE   (2ull << 30)  // 2 GiB reserved
#define PERMUTE_ARENA_WINDOW (2u << 20)    // 2 MiB commit granularity
#define PERMUTE_ARENA_WINDOWS (PERMUTE_ARENA_SIZE / PERMUTE_ARENA_WINDOW)
#define PERMUTE_ARENA_BASE   ((void *)(uintptr_t)0x20000000000ull) // 2 TiB

// Returns 0 on success. Idempotent. Called automatically by the first
// allocation, so a guest that allocates before main still works.
int permute_alloc_init(void);

// Releases the mapping and forgets all state. Tests use this to run a
// sequence twice in one process; production runs do not need it.
void permute_alloc_reset(void);

void *permute_alloc_malloc(size_t bytes);
void *permute_alloc_calloc(size_t count, size_t bytes);
void *permute_alloc_realloc(void *ptr, size_t bytes);
void  permute_alloc_free(void *ptr);
int   permute_alloc_posix_memalign(void **memptr, size_t alignment,
                                   size_t bytes);
char *permute_alloc_strdup(const char *s);

// 1 when the pointer lies inside the arena, 0 otherwise.
int permute_alloc_owns(const void *ptr);

// Base address and used high-water mark, both observable.
void *permute_alloc_base(void);
size_t permute_alloc_high_water(void);
size_t permute_alloc_live_bytes(void);
uint64_t permute_alloc_live_blocks(void);
uint64_t permute_alloc_error_count(void);

#endif // PERMUTE_ALLOC_H
