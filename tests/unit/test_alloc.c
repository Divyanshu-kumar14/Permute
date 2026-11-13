// SPDX-License-Identifier: Apache-2.0
//
// P1-10 unit tests: size classes, alignment, reuse order, realloc, foreign
// pointers, high-water determinism, and a 10^6-operation digest.
//
// Built with PERMUTE_ALLOC_PREFIX, so the test process keeps libc's malloc and
// calls the deterministic allocator explicitly. The interposed build is
// exercised separately by tools/alloc_probe.

#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "abi/alloc.h"
#include "harness.h"

static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v;
    h *= 1099511628211ull;
    return h;
}

static uint64_t offset_of(const void *p)
{
    return (uint64_t)((const uint8_t *)p - (const uint8_t *)permute_alloc_base());
}

// ---------------------------------------------------------------------------
// Basics

static void test_basic_allocation(void)
{
    permute_alloc_reset();
    CHECK_EQ(permute_alloc_init(), 0);
    CHECK(permute_alloc_base() != NULL);
    CHECK_EQ(permute_alloc_high_water(), 0);

    void *p = permute_alloc_malloc(10);
    CHECK(p != NULL);
    CHECK_EQ((uintptr_t)p % 16, 0);
    CHECK(permute_alloc_owns(p));
    CHECK_EQ(permute_alloc_live_blocks(), 1);

    memset(p, 0xAB, 10);
    CHECK_EQ(((uint8_t *)p)[9], 0xAB);

    permute_alloc_free(p);
    CHECK_EQ(permute_alloc_live_blocks(), 0);
    CHECK_EQ(permute_alloc_error_count(), 0);
}

static void test_calloc_zeroes(void)
{
    void *p = permute_alloc_calloc(16, 8);
    CHECK(p != NULL);
    for (int i = 0; i < 128; i++)
        CHECK_EQ(((uint8_t *)p)[i], 0);
    permute_alloc_free(p);
}

static void test_realloc_preserves(void)
{
    char *p = permute_alloc_malloc(8);
    memcpy(p, "abcdefg", 8);

    char *q = permute_alloc_realloc(p, 4000);
    CHECK(q != NULL);
    CHECK(strcmp(q, "abcdefg") == 0);

    // Shrinking inside the same class keeps the block.
    char *r = permute_alloc_realloc(q, 8);
    CHECK_EQ(r, q);
    CHECK(strcmp(r, "abcdefg") == 0);

    permute_alloc_free(r);
}

static void test_posix_memalign(void)
{
    void *p = NULL;
    CHECK_EQ(permute_alloc_posix_memalign(&p, 64, 100), 0);
    CHECK(p != NULL);
    CHECK_EQ((uintptr_t)p % 64, 0);
    memset(p, 0x5A, 100);
    CHECK_EQ(((uint8_t *)p)[99], 0x5A);
    permute_alloc_free(p);

    CHECK_EQ(permute_alloc_posix_memalign(&p, 4096, 10), 0);
    CHECK_EQ((uintptr_t)p % 4096, 0);
    permute_alloc_free(p);

    // Bad alignments are rejected, not rounded.
    CHECK_EQ(permute_alloc_posix_memalign(&p, 3, 10), EINVAL);
    CHECK_EQ(permute_alloc_posix_memalign(&p, 0, 10), EINVAL);
}

static void test_strdup_and_limits(void)
{
    char *s = permute_alloc_strdup("permute");
    CHECK(s != NULL);
    CHECK(strcmp(s, "permute") == 0);
    permute_alloc_free(s);

    // Above the largest class: refuse rather than approximate.
    errno = 0;
    void *big = permute_alloc_malloc((size_t)3 << 20);
    CHECK(big == NULL);
    CHECK_EQ(errno, ENOMEM);
}

// ---------------------------------------------------------------------------
// Ordering and reuse

static void test_lowest_offset_first(void)
{
    permute_alloc_reset();
    CHECK_EQ(permute_alloc_init(), 0);

    void *a = permute_alloc_malloc(32);
    void *b = permute_alloc_malloc(32);
    void *c = permute_alloc_malloc(32);
    CHECK(a < b && b < c);

    uint64_t ao = offset_of(a), bo = offset_of(b), co = offset_of(c);

    // Free out of order; allocation must still take the lowest address.
    permute_alloc_free(c);
    permute_alloc_free(a);
    permute_alloc_free(b);

    void *x = permute_alloc_malloc(32);
    void *y = permute_alloc_malloc(32);
    void *z = permute_alloc_malloc(32);
    CHECK_EQ(offset_of(x), ao);
    CHECK_EQ(offset_of(y), bo);
    CHECK_EQ(offset_of(z), co);

    permute_alloc_free(x);
    permute_alloc_free(y);
    permute_alloc_free(z);
}

static void test_double_free_and_foreign(void)
{
    permute_alloc_reset();
    CHECK_EQ(permute_alloc_init(), 0);

    void *p = permute_alloc_malloc(16);
    permute_alloc_free(p);
    uint64_t before = permute_alloc_error_count();
    permute_alloc_free(p); // double free
    CHECK_EQ(permute_alloc_error_count(), before + 1);

    int stack_value = 0;
    CHECK_EQ(permute_alloc_owns(&stack_value), 0);
    before = permute_alloc_error_count();
    permute_alloc_free(&stack_value); // foreign
    CHECK_EQ(permute_alloc_error_count(), before + 1);
}

static void test_high_water_is_history_independent(void)
{
    permute_alloc_reset();
    CHECK_EQ(permute_alloc_init(), 0);

    // First history: allocate, free, allocate again.
    void *a = permute_alloc_malloc(100);
    permute_alloc_free(a);
    void *b = permute_alloc_malloc(100);
    size_t h1 = permute_alloc_high_water();
    uint64_t o1 = offset_of(b);
    permute_alloc_free(b);

    permute_alloc_reset();
    CHECK_EQ(permute_alloc_init(), 0);

    // Second history: allocate once.
    void *c = permute_alloc_malloc(100);
    size_t h2 = permute_alloc_high_water();
    uint64_t o2 = offset_of(c);
    permute_alloc_free(c);

    // Same peak and same address: reuse means history does not shift layout.
    CHECK_EQ(h1, h2);
    CHECK_EQ(o1, o2);
}

// ---------------------------------------------------------------------------
// Long mixed sequence

static uint64_t run_mixed(int operations, int live_max, size_t *peak_out)
{
    enum { MAX_LIVE = 200000 };
    static void *live[MAX_LIVE];

    permute_alloc_reset();
    if (permute_alloc_init() != 0)
        return 0;

    uint64_t h = 1469598103934665603ull;
    uint64_t s = 0x9E3779B97F4A7C15ull;
    int live_n = 0;

    for (int i = 0; i < operations; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t r = (uint32_t)(s >> 33);

        int do_alloc = (live_n == 0) || (live_n < live_max && (r & 1u));
        if (do_alloc) {
            size_t size = 1 + (r >> 8) % 128;
            void *p = permute_alloc_malloc(size);
            if (p == NULL)
                break;
            *(uint8_t *)p = (uint8_t)i;
            live[live_n++] = p;
            h = mix(h, offset_of(p) / 16);
            h = mix(h, size);
        } else {
            int idx = (int)((r >> 8) % (uint32_t)live_n);
            void *p = live[idx];
            h = mix(h, offset_of(p) / 16);
            h = mix(h, 0xFFFF);
            permute_alloc_free(p);
            live[idx] = live[--live_n];
        }
    }

    *peak_out = permute_alloc_high_water();

    // Leave nothing to keep the next run clean.
    for (int i = 0; i < live_n; i++)
        permute_alloc_free(live[i]);

    return h;
}

static void test_million_operations_deterministic(void)
{
    size_t peak1 = 0, peak2 = 0;
    uint64_t d1 = run_mixed(1000000, 50000, &peak1);
    uint64_t d2 = run_mixed(1000000, 50000, &peak2);

    CHECK(d1 != 0);
    CHECK_EQ(d1, d2);
    CHECK_EQ(peak1, peak2);
    CHECK_EQ(permute_alloc_error_count(), 0);
}

static void test_rss_ceiling(void)
{
#if defined(__SANITIZE_ADDRESS__)
    // ASan shadows the whole address space; RSS is not comparable.
    printf("rss: skipped under ASan\n");
#else
    size_t peak = 0;
    (void)run_mixed(1000000, 50000, &peak);

    struct rusage ru;
    CHECK_EQ(getrusage(RUSAGE_SELF, &ru), 0);
    long kb = ru.ru_maxrss; // KiB on Linux
    printf("rss: %ld KiB after 10^6 operations (arena high-water %zu KiB)\n",
           kb, peak / 1024);
    CHECK(kb <= 70 * 1024);
#endif
}

// ---------------------------------------------------------------------------

int main(void)
{
    test_basic_allocation();
    test_calloc_zeroes();
    test_realloc_preserves();
    test_posix_memalign();
    test_strdup_and_limits();
    test_lowest_offset_first();
    test_double_free_and_foreign();
    test_high_water_is_history_independent();
    test_million_operations_deterministic();
    test_rss_ceiling();

    return TEST_RESULT("test_alloc");
}
