// SPDX-License-Identifier: Apache-2.0
//
// Shared header for the conformance scenarios.
//
// The same source builds for QNX and for the simulator: on QNX the vendor
// headers are used, on the host the clean-room ABI. `permute_main` is the
// simulator entry point; on QNX a main() wrapper calls it.

#ifndef PERMUTE_CONFORMANCE_COMMON_H
#define PERMUTE_CONFORMANCE_COMMON_H

#ifdef __QNXNTO__
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#else
#include "abi/permute_iface.h"
#endif

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <time.h>

#define C_MS(x) ((uint64_t)(x) * 1000ull * 1000ull)

static inline void c_set_prio(int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof param);
    param.sched_priority = priority;
    (void)SchedSet(0, 0, SCHED_FIFO, &param);
}

static inline int c_arm(uint32_t mask, uint64_t ns)
{
    struct sigevent unblock;
    memset(&unblock, 0, sizeof unblock);
    SIGEV_UNBLOCK_INIT(&unblock);
    uint64_t ntime = ns;
    uint64_t otime = 0;
    return TimerTimeout(CLOCK_MONOTONIC, mask, &unblock, &ntime, &otime);
}

static inline int c_arm_abstime(uint32_t mask, uint64_t ns)
{
    struct sigevent unblock;
    memset(&unblock, 0, sizeof unblock);
    SIGEV_UNBLOCK_INIT(&unblock);
    uint64_t ntime = ns;
    uint64_t otime = 0;
    return TimerTimeout(CLOCK_MONOTONIC, mask | TIMER_ABSTIME, &unblock,
                        &ntime, &otime);
}

static inline void c_sleep_ms(int ms)
{
    struct timespec nap = { 0, (long)ms * 1000L * 1000L };
    (void)nanosleep(&nap, NULL);
}

#endif // PERMUTE_CONFORMANCE_COMMON_H
