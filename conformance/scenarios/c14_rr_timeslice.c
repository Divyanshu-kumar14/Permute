// SPDX-License-Identifier: Apache-2.0
// C14 — two round-robin threads alternate.
//
// The simulator's guest is cooperative, so CPU-bound preemption is not
// observable; the threads yield through nanosleep, which is what the committed
// reference captures. The QNX run is the authoritative timeslice check.
#include "conformance/scenarios/common.h"

#define ROUNDS 10

static volatile int g_failed;
static volatile int g_sequence[2 * ROUNDS];
static volatile int g_position;
static volatile int g_counts[2];

static void note(int id)
{
    int at = g_position;
    if (at < 2 * ROUNDS) {
        g_sequence[at] = id;
        g_position = at + 1;
    }
    g_counts[id]++;
}

static void *runner_thread(void *arg)
{
    int id = *(int *)arg;
    struct sched_param param;
    memset(&param, 0, sizeof param);
    param.sched_priority = 10;
    (void)SchedSet(0, 0, SCHED_RR, &param);

    for (int i = 0; i < ROUNDS; i++) {
        note(id);
        c_sleep_ms(1);
    }
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int ids[2] = { 0, 1 };
    pthread_t threads[2];
    for (int i = 0; i < 2; i++)
        if (pthread_create(&threads[i], NULL, runner_thread, &ids[i]) != 0)
            return 1;
    for (int i = 0; i < 2; i++) pthread_join(threads[i], NULL);

    if (g_counts[0] != ROUNDS || g_counts[1] != ROUNDS) g_failed = 1;
    // Both threads must appear, and no more than two consecutive turns of the
    // same thread (a ±1 alternation tolerance).
    int run = 1;
    for (int i = 1; i < g_position; i++) {
        if (g_sequence[i] == g_sequence[i - 1]) {
            run++;
            if (run > 2) g_failed = 1;
        } else {
            run = 1;
        }
    }
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
