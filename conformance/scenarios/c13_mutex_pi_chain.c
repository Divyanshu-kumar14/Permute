// SPDX-License-Identifier: Apache-2.0
// C13 — mutex PI chain low/medium/high.
//
// This source is part of the mandatory set but is not executed by the
// simulator until the mutex shim exists (LIMITS #37). It compiles for both
// targets; the QNX build is the reference for the behaviour.
#include "conformance/scenarios/common.h"

static volatile int g_failed;
static pthread_mutex_t g_mutex;
static volatile int g_low_holds;
static volatile int g_done;

static void *low_thread(void *arg)
{
    (void)arg;
    c_set_prio(5);
    if (pthread_mutex_lock(&g_mutex) != 0) { g_failed = 1; return NULL; }
    g_low_holds = 1;
    while (!g_done)
        c_sleep_ms(1);
    if (pthread_mutex_unlock(&g_mutex) != 0) g_failed = 1;
    return NULL;
}

static void *high_thread(void *arg)
{
    (void)arg;
    c_set_prio(20);
    if (pthread_mutex_lock(&g_mutex) != 0) { g_failed = 1; return NULL; }
    if (pthread_mutex_unlock(&g_mutex) != 0) g_failed = 1;
    return NULL;
}

static void *medium_thread(void *arg)
{
    (void)arg;
    c_set_prio(15);
    c_sleep_ms(2);
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc; (void)argv;

    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) return 1;
    if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != 0)
        g_failed = 1;
    if (pthread_mutex_init(&g_mutex, &attr) != 0) return 1;

    pthread_t low, medium, high;
    if (pthread_create(&low, NULL, low_thread, NULL) != 0) return 1;
    while (!g_low_holds)
        SchedYield();
    if (pthread_create(&medium, NULL, medium_thread, NULL) != 0) return 1;
    if (pthread_create(&high, NULL, high_thread, NULL) != 0) return 1;

    pthread_join(high, NULL);
    g_done = 1;
    pthread_join(medium, NULL);
    pthread_join(low, NULL);
    pthread_mutex_destroy(&g_mutex);
    pthread_mutexattr_destroy(&attr);
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
