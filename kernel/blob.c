// SPDX-License-Identifier: Apache-2.0

#include "kernel/blob.h"

#include <string.h>

static const uint32_t g_class_sizes[PERMUTE_BLOB_CLASSES] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536,
};

void permute_blob_init(permute_blob_arena_t *a, void *arena, size_t size)
{
    memset(a, 0, sizeof *a);
    a->arena = (uint8_t *)arena;
    a->size = size;
    for (int i = 0; i < PERMUTE_BLOB_CLASSES; i++)
        a->class_size[i] = g_class_sizes[i];
}

static int class_for(size_t n)
{
    for (int i = 0; i < PERMUTE_BLOB_CLASSES; i++) {
        if (n <= g_class_sizes[i])
            return i;
    }
    return -1;
}

void *permute_blob_alloc(permute_blob_arena_t *a, size_t n)
{
    if (n == 0)
        return NULL;

    int c = class_for(n);
    if (c < 0)
        return NULL;

    if (a->freelist[c] != NULL) {
        void *p = a->freelist[c];
        a->freelist[c] = *(void **)p;
        a->allocs++;
        return p;
    }

    size_t need = PERMUTE_BLOB_HEADER + a->class_size[c];
    if (a->used + need > a->size)
        return NULL;

    uint64_t *header = (uint64_t *)(void *)(a->arena + a->used);
    a->used += need;
    *header = (uint64_t)c;
    a->allocs++;
    return (void *)(header + 1);
}

void permute_blob_free(permute_blob_arena_t *a, void *p)
{
    if (p == NULL)
        return;

    uint64_t *header = (uint64_t *)p - 1;
    uint32_t c = (uint32_t)*header;
    if (c >= PERMUTE_BLOB_CLASSES)
        return; // corrupted header: refuse rather than corrupt the free list

    *(void **)p = a->freelist[c];
    a->freelist[c] = p;
    a->frees++;
}

size_t permute_blob_usable(const void *p)
{
    if (p == NULL)
        return 0;
    const uint64_t *header = (const uint64_t *)p - 1;
    uint32_t c = (uint32_t)*header;
    if (c >= PERMUTE_BLOB_CLASSES)
        return 0;
    return g_class_sizes[c];
}
