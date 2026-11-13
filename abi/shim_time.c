// SPDX-License-Identifier: Apache-2.0
//
// Time shims (FR-4.8).
//
// Every time source a guest can reach routes to the virtual clock. There is no
// fallback to the host clock: if the kernel table is missing the call fails,
// because a silent real-time reading would make a run unreproducible in a way
// that is very hard to see afterwards.
//
// This is the only file allowed to name the real time functions; the
// determinism gate in tools/symcheck.py enforces that.

#define _GNU_SOURCE

#include <errno.h>

#include "abi/permute_iface.h"
#include "guest/hypercall.h"

int clock_gettime(clockid_t clock_id, struct timespec *ts)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->clock_gettime == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->clock_gettime((int)clock_id, ts);
}

time_t time(time_t *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return (time_t)-1;
    if (out != NULL)
        *out = ts.tv_sec;
    return ts.tv_sec;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz; // obsolete on every platform we care about

    // glibc declares tv nonnull; a NULL here is the caller's bug, and adding a
    // check trips -Wnonnull-compare for no benefit.
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return -1;

    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = (suseconds_t)(ts.tv_nsec / 1000);
    return 0;
}

int TimerTimeout(clockid_t id, uint32_t flags, const struct sigevent *event,
                 const uint64_t *ntime, uint64_t *otime)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->timer_timeout == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->timer_timeout((int)id, flags, event, ntime, otime);
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->nanosleep == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->nanosleep(req, rem);
}
