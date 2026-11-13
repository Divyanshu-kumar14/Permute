// SPDX-License-Identifier: Apache-2.0

#define _GNU_SOURCE

#include "abi/alloc.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PERMUTE_ALLOC_MAGIC 0xA110CA7Eu
#define PERMUTE_ALLOC_NONE  0xFFFFFFFFu
#define PERMUTE_ALLOC_HDR   16u
#define PERMUTE_ALLOC_ALIGN 16u

// Payload sizes. Small requests round to the next class; anything above the
// largest class fails loudly rather than being approximated.
static const uint32_t g_class_size[] = {
    16,    24,    32,    48,    64,    96,    128,   192,
    256,   384,   512,   768,   1024,  1536,  2048,  3072,
    4096,  6144,  8192,  12288, 16384, 24576, 32768, 49152,
    65536, 98304, 131072, 196608, 262144, 393216, 524288, 786432,
    1048576, 1572864, 2097152,
};
#define PERMUTE_ALLOC_CLASSES                                                  \
    (sizeof g_class_size / sizeof g_class_size[0])

typedef struct {
    uint32_t block_off;   // offset of the block start (aligned allocations)
    uint16_t class_index;
    uint16_t reserved;
    uint32_t size;        // requested bytes
    uint32_t magic;
} alloc_hdr_t;

typedef struct {
    uint32_t next;
    uint32_t prev;
} free_node_t;

typedef struct {
    uint8_t *base;
    size_t bump;
    size_t high_water;

    uint32_t free_head[PERMUTE_ALLOC_CLASSES];
    uint32_t free_tail[PERMUTE_ALLOC_CLASSES];
    uint64_t committed[PERMUTE_ARENA_WINDOWS / 64];

    uint64_t live_blocks;
    size_t live_bytes;
    uint64_t errors;
    int ready;
} alloc_state_t;

static alloc_state_t g_alloc;

static inline size_t align_up(size_t n, size_t a)
{
    return (n + (a - 1)) & ~(a - 1);
}

static inline size_t block_size(size_t cls)
{
    return align_up(PERMUTE_ALLOC_HDR + g_class_size[cls], PERMUTE_ALLOC_ALIGN);
}

static int class_for(size_t bytes)
{
    for (size_t i = 0; i < PERMUTE_ALLOC_CLASSES; i++) {
        if (bytes <= g_class_size[i])
            return (int)i;
    }
    return -1;
}

static int ensure_committed(size_t off, size_t len)
{
    size_t first = off / PERMUTE_ARENA_WINDOW;
    size_t last = (off + len - 1) / PERMUTE_ARENA_WINDOW;

    for (size_t w = first; w <= last; w++) {
        uint64_t bit = 1ull << (w & 63);
        if (g_alloc.committed[w >> 6] & bit)
            continue;
        if (mprotect(g_alloc.base + w * PERMUTE_ARENA_WINDOW,
                     PERMUTE_ARENA_WINDOW, PROT_READ | PROT_WRITE) != 0)
            return -1;
        g_alloc.committed[w >> 6] |= bit;
    }
    return 0;
}

int permute_alloc_init(void)
{
    if (g_alloc.ready)
        return 0;

    memset(&g_alloc, 0, sizeof g_alloc);
    for (size_t i = 0; i < PERMUTE_ALLOC_CLASSES; i++) {
        g_alloc.free_head[i] = PERMUTE_ALLOC_NONE;
        g_alloc.free_tail[i] = PERMUTE_ALLOC_NONE;
    }

    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    void *p = mmap(PERMUTE_ARENA_BASE, PERMUTE_ARENA_SIZE, PROT_NONE,
                   flags | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        // The fixed address is not available on this host. Offsets remain the
        // same, so anything computed from offsets is still comparable.
        p = mmap(NULL, PERMUTE_ARENA_SIZE, PROT_NONE, flags, -1, 0);
        if (p == MAP_FAILED)
            return -1;
    }

    g_alloc.base = (uint8_t *)p;
    g_alloc.bump = 0;
    g_alloc.high_water = 0;
    g_alloc.ready = 1;
    return 0;
}

void permute_alloc_reset(void)
{
    if (g_alloc.ready && g_alloc.base != NULL)
        munmap(g_alloc.base, PERMUTE_ARENA_SIZE);
    memset(&g_alloc, 0, sizeof g_alloc);
}

void *permute_alloc_base(void)
{
    return g_alloc.base;
}

size_t permute_alloc_high_water(void)
{
    return g_alloc.high_water;
}

size_t permute_alloc_live_bytes(void)
{
    return g_alloc.live_bytes;
}

uint64_t permute_alloc_live_blocks(void)
{
    return g_alloc.live_blocks;
}

uint64_t permute_alloc_error_count(void)
{
    return g_alloc.errors;
}

int permute_alloc_owns(const void *ptr)
{
    if (!g_alloc.ready || ptr == NULL)
        return 0;
    const uint8_t *p = (const uint8_t *)ptr;
    return p >= g_alloc.base && p < g_alloc.base + PERMUTE_ARENA_SIZE;
}

// ---------------------------------------------------------------------------
// Free lists: doubly linked, kept in ascending offset order

static free_node_t *node_at(uint32_t off)
{
    return (free_node_t *)(void *)(g_alloc.base + off + PERMUTE_ALLOC_HDR);
}

static void free_list_insert(uint32_t cls, uint32_t off)
{
    uint32_t head = g_alloc.free_head[cls];
    uint32_t tail = g_alloc.free_tail[cls];
    free_node_t *n = node_at(off);

    if (head == PERMUTE_ALLOC_NONE) {
        n->next = PERMUTE_ALLOC_NONE;
        n->prev = PERMUTE_ALLOC_NONE;
        g_alloc.free_head[cls] = off;
        g_alloc.free_tail[cls] = off;
        return;
    }

    if (off < head) {
        n->next = head;
        n->prev = PERMUTE_ALLOC_NONE;
        node_at(head)->prev = off;
        g_alloc.free_head[cls] = off;
        return;
    }

    if (off > tail) {
        n->next = PERMUTE_ALLOC_NONE;
        n->prev = tail;
        node_at(tail)->next = off;
        g_alloc.free_tail[cls] = off;
        return;
    }

    // Middle insertion: walk from the head. The head/tail fast paths make
    // LIFO and FIFO free patterns O(1); a random pattern pays the scan.
    uint32_t cur = head;
    while (cur != PERMUTE_ALLOC_NONE && cur < off) {
        uint32_t next = node_at(cur)->next;
        if (next == PERMUTE_ALLOC_NONE || next > off)
            break;
        cur = next;
    }

    uint32_t next = node_at(cur)->next;
    n->next = next;
    n->prev = cur;
    node_at(cur)->next = off;
    if (next != PERMUTE_ALLOC_NONE)
        node_at(next)->prev = off;
    else
        g_alloc.free_tail[cls] = off;
}

static uint32_t free_list_pop(uint32_t cls)
{
    uint32_t off = g_alloc.free_head[cls];
    if (off == PERMUTE_ALLOC_NONE)
        return PERMUTE_ALLOC_NONE;

    uint32_t next = node_at(off)->next;
    g_alloc.free_head[cls] = next;
    if (next != PERMUTE_ALLOC_NONE)
        node_at(next)->prev = PERMUTE_ALLOC_NONE;
    else
        g_alloc.free_tail[cls] = PERMUTE_ALLOC_NONE;
    return off;
}

// ---------------------------------------------------------------------------
// Allocation

static void *block_alloc(size_t bytes, uint32_t *off_out)
{
    size_t off = g_alloc.bump;
    if (off + bytes > PERMUTE_ARENA_SIZE) {
        errno = ENOMEM;
        return NULL;
    }
    if (ensure_committed(off, bytes) != 0) {
        errno = ENOMEM;
        return NULL;
    }
    g_alloc.bump = off + bytes;
    if (g_alloc.bump > g_alloc.high_water)
        g_alloc.high_water = g_alloc.bump;
    *off_out = (uint32_t)off;
    return g_alloc.base + off;
}

void *permute_alloc_malloc(size_t bytes)
{
    if (permute_alloc_init() != 0) {
        errno = ENOMEM;
        return NULL;
    }
    if (bytes == 0)
        bytes = 1;

    int cls = class_for(bytes);
    if (cls < 0) {
        errno = ENOMEM; // above the largest class: refuse, do not approximate
        return NULL;
    }

    uint32_t off;
    if (g_alloc.free_head[cls] != PERMUTE_ALLOC_NONE) {
        off = free_list_pop((uint32_t)cls);
    } else {
        uint32_t fresh;
        if (block_alloc(block_size((size_t)cls), &fresh) == NULL)
            return NULL;
        off = fresh;
    }

    alloc_hdr_t *hdr = (alloc_hdr_t *)(void *)(g_alloc.base + off);
    hdr->block_off = off;
    hdr->class_index = (uint16_t)cls;
    hdr->reserved = 0;
    hdr->size = (uint32_t)bytes;
    hdr->magic = PERMUTE_ALLOC_MAGIC;

    g_alloc.live_blocks++;
    g_alloc.live_bytes += bytes;
    return hdr + 1;
}

void permute_alloc_free(void *ptr)
{
    if (ptr == NULL)
        return;

    if (!permute_alloc_owns(ptr)) {
        // Foreign pointer: the interposed wrapper forwards it to libc. The
        // core refuses, and says so, rather than corrupting the arena.
        g_alloc.errors++;
        return;
    }

    alloc_hdr_t *hdr = (alloc_hdr_t *)ptr - 1;
    if (hdr->magic != PERMUTE_ALLOC_MAGIC) {
        g_alloc.errors++; // double free or overwritten header
        return;
    }

    hdr->magic = 0;
    g_alloc.live_blocks--;
    g_alloc.live_bytes -= hdr->size;

    free_list_insert(hdr->class_index, hdr->block_off);
}

void *permute_alloc_calloc(size_t count, size_t bytes)
{
    if (bytes != 0 && count > SIZE_MAX / bytes) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = count * bytes;
    void *p = permute_alloc_malloc(total);
    if (p != NULL)
        memset(p, 0, total);
    return p;
}

void *permute_alloc_realloc(void *ptr, size_t bytes)
{
    if (ptr == NULL)
        return permute_alloc_malloc(bytes);
    if (bytes == 0) {
        permute_alloc_free(ptr);
        return NULL;
    }

    if (!permute_alloc_owns(ptr)) {
        g_alloc.errors++;
        errno = EINVAL;
        return NULL;
    }

    alloc_hdr_t *hdr = (alloc_hdr_t *)ptr - 1;
    if (hdr->magic != PERMUTE_ALLOC_MAGIC) {
        g_alloc.errors++;
        errno = EINVAL;
        return NULL;
    }

    size_t capacity = g_class_size[hdr->class_index];
    if (bytes <= capacity) {
        g_alloc.live_bytes = g_alloc.live_bytes - hdr->size + bytes;
        hdr->size = (uint32_t)bytes;
        return ptr;
    }

    void *q = permute_alloc_malloc(bytes);
    if (q == NULL)
        return NULL;
    memcpy(q, ptr, hdr->size);
    permute_alloc_free(ptr);
    return q;
}

int permute_alloc_posix_memalign(void **memptr, size_t alignment, size_t bytes)
{
    if (memptr == NULL)
        return EINVAL;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        alignment % sizeof(void *) != 0)
        return EINVAL;

    if (bytes == 0)
        bytes = 1;

    if (alignment <= PERMUTE_ALLOC_ALIGN) {
        void *p = permute_alloc_malloc(bytes);
        if (p == NULL)
            return ENOMEM;
        *memptr = p;
        return 0;
    }

    // Reserve enough for the header, the alignment gap, and the payload, then
    // place the header immediately before the aligned user pointer. Free finds
    // the block through block_off, which is why the header carries it.
    size_t need = PERMUTE_ALLOC_HDR + alignment + bytes;
    int cls = class_for(need);
    if (cls < 0)
        return ENOMEM;

    uint32_t off;
    uint8_t *block;
    if (g_alloc.free_head[cls] != PERMUTE_ALLOC_NONE) {
        off = free_list_pop((uint32_t)cls);
        block = g_alloc.base + off;
    } else {
        uint32_t fresh;
        block = block_alloc(block_size((size_t)cls), &fresh);
        if (block == NULL)
            return ENOMEM;
        off = fresh;
    }

    uintptr_t user = ((uintptr_t)block + PERMUTE_ALLOC_HDR + alignment - 1) &
                     ~(uintptr_t)(alignment - 1);
    alloc_hdr_t *hdr = (alloc_hdr_t *)(void *)(user - PERMUTE_ALLOC_HDR);
    hdr->block_off = off;
    hdr->class_index = (uint16_t)cls;
    hdr->reserved = 0;
    hdr->size = (uint32_t)bytes;
    hdr->magic = PERMUTE_ALLOC_MAGIC;

    g_alloc.live_blocks++;
    g_alloc.live_bytes += bytes;
    *memptr = (void *)user;
    return 0;
}

char *permute_alloc_strdup(const char *s)
{
    if (s == NULL) {
        errno = EINVAL;
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *p = permute_alloc_malloc(n);
    if (p != NULL)
        memcpy(p, s, n);
    return p;
}

// ---------------------------------------------------------------------------
// Interposition
//
// The guest archive defines the real symbols. The unit test builds with
// PERMUTE_ALLOC_PREFIX so it can call the core without replacing libc for the
// test process itself.

#ifndef PERMUTE_ALLOC_PREFIX

extern void __libc_free(void *);
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);

void *malloc(size_t bytes)
{
    return permute_alloc_malloc(bytes);
}

void free(void *ptr)
{
    if (permute_alloc_owns(ptr))
        permute_alloc_free(ptr);
    else
        __libc_free(ptr);
}

void *calloc(size_t count, size_t bytes)
{
    return permute_alloc_calloc(count, bytes);
}

void *realloc(void *ptr, size_t bytes)
{
    if (ptr != NULL && !permute_alloc_owns(ptr))
        return __libc_realloc(ptr, bytes);
    return permute_alloc_realloc(ptr, bytes);
}

int posix_memalign(void **memptr, size_t alignment, size_t bytes)
{
    return permute_alloc_posix_memalign(memptr, alignment, bytes);
}

char *strdup(const char *s)
{
    return permute_alloc_strdup(s);
}

#endif // PERMUTE_ALLOC_PREFIX
