// SPDX-License-Identifier: Apache-2.0

#define _GNU_SOURCE

#include "guest/hypercall.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// 256 GiB is inside a 39-bit address space, so aarch64 hosts can use the first
// candidate too. The higher entries cover hosts where 256 GiB is occupied.
const uintptr_t permute_hypercall_candidates[] = {
    0x0000004000000000ull, // 256 GiB
    0x0000010000000000ull, //   1 TiB
    0x0000020000000000ull, //   2 TiB
    0x0000002000000000ull, // 128 GiB
    0x0000001000000000ull, //  64 GiB
};

const unsigned permute_hypercall_candidate_count =
    (unsigned)(sizeof permute_hypercall_candidates /
               sizeof permute_hypercall_candidates[0]);

static permute_hypercall_page_t *g_page;
static const permute_kernel_ops_t *g_fallback_ops;

uintptr_t permute_hypercall_init(void)
{
    if (g_page != NULL)
        return (uintptr_t)g_page;

    // Operational escape hatch for environments that deny fixed mappings.
    if (getenv("PERMUTE_NO_HYPERCALL") != NULL)
        return 0;

#ifdef MAP_FIXED_NOREPLACE
    for (unsigned i = 0; i < permute_hypercall_candidate_count; i++) {
        uintptr_t want = permute_hypercall_candidates[i];
        void *p = mmap((void *)want, PERMUTE_HYPERCALL_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

        if (p == MAP_FAILED)
            continue;

        // A kernel that ignores MAP_FIXED_NOREPLACE may place the mapping
        // elsewhere. Never accept that silently.
        if ((uintptr_t)p != want) {
            (void)munmap(p, PERMUTE_HYPERCALL_SIZE);
            continue;
        }

        permute_hypercall_page_t *page = (permute_hypercall_page_t *)p;
        memset(page, 0, sizeof *page);
        page->magic = PERMUTE_HYPERCALL_MAGIC;
        page->version = PERMUTE_HYPERCALL_VERSION;
        page->page_size = PERMUTE_HYPERCALL_SIZE;

        if (permute_hypercall_bind((uintptr_t)p) != 0) {
            (void)munmap(p, PERMUTE_HYPERCALL_SIZE);
            continue;
        }

        return (uintptr_t)p;
    }
#endif

    return 0;
}

int permute_hypercall_bind(uintptr_t base)
{
    if (base == 0) {
        g_page = NULL;
        return -1;
    }

    permute_hypercall_page_t *page = (permute_hypercall_page_t *)base;
    if (page->magic != PERMUTE_HYPERCALL_MAGIC ||
        page->version != PERMUTE_HYPERCALL_VERSION)
        return -1;

    g_page = page;
    return 0;
}

uintptr_t permute_hypercall_base(void)
{
    return (uintptr_t)g_page;
}

const permute_kernel_ops_t *permute_ops(void)
{
    if (g_page != NULL)
        return __atomic_load_n(&g_page->ops, __ATOMIC_ACQUIRE);
    return g_fallback_ops;
}

int permute_ops_install(const permute_kernel_ops_t *ops)
{
    if (g_page != NULL) {
        __atomic_store_n(&g_page->ops, ops, __ATOMIC_RELEASE);
        return 0;
    }
    g_fallback_ops = ops;
    return 0;
}
