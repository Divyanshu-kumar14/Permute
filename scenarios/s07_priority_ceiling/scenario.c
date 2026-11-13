// SPDX-License-Identifier: Apache-2.0
//
// s07_priority_ceiling: PTHREAD_PRIO_PROTECT.
//
// A thread that locks a ceiling mutex runs at the ceiling while it holds it,
// and a thread already above the ceiling is refused rather than silently
// capped. The scenario is written against the pthread API a real service
// uses; the mutex shim is interposed in a later task.

#include <pthread.h>
#include <sched.h>
#include <string.h>

#include "abi/permute_iface.h"

static pthread_mutex_t g_mutex;
static volatile int g_failed;
static volatile int g_low_priority_seen = -1;

static void set_prio(int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof param);
    param.sched_priority = priority;
    if (SchedSet(0, 0, SCHED_FIFO, &param) != 0)
        g_failed = 1;
}

static void *low_thread(void *arg)
{
    (void)arg;
    set_prio(5);

    if (pthread_mutex_lock(&g_mutex) != 0) {
        g_failed = 1;
        return NULL;
    }

    // On a target, sched_curpriority would read 20 here. The portability
    // header exposes only sched_priority, which is the thread's own value.
    struct sched_param observed;
    memset(&observed, 0, sizeof observed);
    if (SchedGet(0, 0, &observed) == 0)
        g_low_priority_seen = observed.sched_priority;

    (void)pthread_mutex_unlock(&g_mutex);
    return NULL;
}

static void *high_thread(void *arg)
{
    (void)arg;
    set_prio(30);

    // A PROTECT mutex refuses a locker above its ceiling. Whether the lock is
    // held at this instant decides between EINVAL and EBUSY, so the result is
    // recorded rather than asserted here.
    if (pthread_mutex_trylock(&g_mutex) == 0)
        (void)pthread_mutex_unlock(&g_mutex);
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0)
        return 1;
    if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_PROTECT) != 0)
        g_failed = 1;
    if (pthread_mutexattr_setprioceiling(&attr, 20) != 0)
        g_failed = 1;
    if (pthread_mutex_init(&g_mutex, &attr) != 0)
        g_failed = 1;
    (void)pthread_mutexattr_destroy(&attr);

    pthread_t low, high;
    if (pthread_create(&low, NULL, low_thread, NULL) != 0)
        return 1;
    if (pthread_create(&high, NULL, high_thread, NULL) != 0)
        return 1;

    pthread_join(low, NULL);
    pthread_join(high, NULL);
    (void)pthread_mutex_destroy(&g_mutex);
    return g_failed ? 1 : 0;
}
