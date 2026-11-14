// SPDX-License-Identifier: Apache-2.0
//
// Context-switch benchmark guest: two equal-priority threads hand the baton
// back and forth. Each SchedYield is one cooperative switch.
#include <pthread.h>
#include <sched.h>

#include "abi/permute_iface.h"

#define YIELD_COUNT 20000

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < YIELD_COUNT; i++)
        (void)SchedYield();
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pthread_t a, b;
    if (pthread_create(&a, NULL, worker, NULL) != 0)
        return 1;
    if (pthread_create(&b, NULL, worker, NULL) != 0)
        return 1;
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    return 0;
}
