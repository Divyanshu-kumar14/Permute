// SPDX-License-Identifier: Apache-2.0
//
// NFR-2/NFR-6 probe: a four-thread SUT that is cheap enough to run 20,000
// times. Main plus three workers, each taking a few turns through the baton.
#include <pthread.h>
#include <sched.h>

#include "abi/permute_iface.h"

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 8; i++)
        (void)SchedYield();
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pthread_t threads[3];
    for (int i = 0; i < 3; i++)
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0)
            return 1;
    for (int i = 0; i < 3; i++)
        pthread_join(threads[i], NULL);
    return 0;
}
