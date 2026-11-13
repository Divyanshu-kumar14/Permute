// SPDX-License-Identifier: Apache-2.0
//
// Day-1 spike: cost of a baton handoff between two OS threads.
//
// This is the number that decides whether the thread backend can carry the
// throughput budget. It is measured, not assumed.
//
// Reporting discipline: a "round trip" is post + wait (two context switches).
// Per-switch cost is half of that. Conflating the two is a 2x error in the
// throughput budget, so both are printed.

#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <linux/futex.h>
#include <sys/syscall.h>

#if defined(__x86_64__) || defined(__i386__)
#  define CPU_RELAX() __builtin_ia32_pause()
#elif defined(__aarch64__)
#  define CPU_RELAX() __asm__ __volatile__("yield")
#else
#  define CPU_RELAX() ((void)0)
#endif

typedef struct {
    _Atomic int seq;
} baton_t;

static baton_t baton_a;
static baton_t baton_b;
static int g_iters;
static int g_spins = 128;

static void baton_wait(baton_t *b, int target)
{
    int spins = 0;
    for (;;) {
        int cur = atomic_load_explicit(&b->seq, memory_order_acquire);
        if (cur == target)
            return;
        if (++spins < g_spins) {
            CPU_RELAX();
            continue;
        }
        (void)syscall(SYS_futex, (int *)(void *)&b->seq, FUTEX_WAIT_PRIVATE,
                      cur, NULL, NULL, 0);
        spins = 0;
    }
}

static void baton_post(baton_t *b, int value)
{
    atomic_store_explicit(&b->seq, value, memory_order_release);
    (void)syscall(SYS_futex, (int *)(void *)&b->seq, FUTEX_WAKE_PRIVATE, 1,
                  NULL, NULL, 0);
}

static void *worker(void *arg)
{
    (void)arg;
    for (int k = 1; k <= g_iters; k++) {
        baton_wait(&baton_b, k);
        baton_post(&baton_a, k);
    }
    return NULL;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

#define TRIALS 6

int main(int argc, char **argv)
{
    g_iters = (argc > 1) ? atoi(argv[1]) : 20000;
    if (g_iters < 1000) {
        fprintf(stderr, "iterations must be >= 1000\n");
        return 2;
    }

    const char *spin_env = getenv("PERMUTE_SPINS");
    if (spin_env != NULL)
        g_spins = atoi(spin_env);
    if (g_spins < 0)
        g_spins = 0;

    printf("config: %d handoffs, %d spins before parking (PERMUTE_SPINS)\n\n",
           g_iters, g_spins);

    uint64_t per_roundtrip[TRIALS];

    for (int t = 0; t < TRIALS; t++) {
        atomic_store(&baton_a.seq, 0);
        atomic_store(&baton_b.seq, 0);

        pthread_t th;
        if (pthread_create(&th, NULL, worker, NULL) != 0) {
            perror("pthread_create");
            return 2;
        }

        uint64_t t0 = now_ns();
        for (int k = 1; k <= g_iters; k++) {
            baton_post(&baton_b, k);
            baton_wait(&baton_a, k);
        }
        uint64_t t1 = now_ns();

        pthread_join(th, NULL);
        per_roundtrip[t] = (t1 - t0) / (uint64_t)g_iters;

        printf("trial %d: %8llu ns/roundtrip  %8llu ns/switch\n",
               t, (unsigned long long)per_roundtrip[t],
               (unsigned long long)(per_roundtrip[t] / 2));
    }

    // Trial 0 is warmup (page faults, first futex park).
    qsort(&per_roundtrip[1], TRIALS - 1, sizeof(uint64_t), cmp_u64);
    uint64_t median = per_roundtrip[1 + (TRIALS - 1) / 2];
    uint64_t best = per_roundtrip[1];

    printf("\nRESULT median %llu ns/roundtrip, %llu ns/switch\n",
           (unsigned long long)median, (unsigned long long)(median / 2));
    printf("RESULT best   %llu ns/roundtrip, %llu ns/switch\n",
           (unsigned long long)best, (unsigned long long)(best / 2));
    printf("BUDGET <= 4000 ns/switch: %s\n",
           (median / 2) <= 4000 ? "PASS" : "FAIL");

    return ((median / 2) <= 4000) ? 0 : 1;
}
