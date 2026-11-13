// SPDX-License-Identifier: Apache-2.0

#include "kernel/timer.h"

#include <errno.h>
#include <string.h>

#include "kernel/ipc_internal.h"
#include "kernel/objects.h"
#include "kernel/vclock.h"

permute_posix_timer_t *permute_posix_timer_get(permute_kernel_t *k,
                                               permute_handle_t h)
{
    if (!permute_table_valid(&k->posix_timers, h))
        return NULL;
    return &k->posix_timer_pool[PERMUTE_HANDLE_SLOT(h)];
}

int permute_timer_create(permute_kernel_t *k, permute_handle_t process,
                         const permute_sigevent_t *event,
                         permute_handle_t *out)
{
    permute_process_t *p = permute_process_get(k, process);
    if (p == NULL)
        return -ESRCH;
    if (event == NULL)
        return -EINVAL;

    switch (event->notify) {
    case PERMUTE_SIGEV_PULSE:
    case PERMUTE_SIGEV_UNBLOCK:
        break;
    case PERMUTE_SIGEV_SIGNAL:
    case PERMUTE_SIGEV_SIGNAL_CODE:
    case PERMUTE_SIGEV_SIGNAL_THREAD:
        return -ENOTSUP; // signals are out of scope; refuse, do not approximate
    default:
        return -EINVAL;
    }

    uint32_t slot, gen;
    int rc = permute_table_alloc(&k->posix_timers, &slot, &gen);
    if (rc != 0)
        return rc;

    permute_posix_timer_t *t = &k->posix_timer_pool[slot];
    memset(t, 0, sizeof *t);
    t->event = *event;
    t->event.reserved0 = 0;
    t->event.reserved1 = 0;
    t->event.reserved2 = 0;
    t->owner_process = process;
    t->owner_pid = p->pid;
    t->armed = 0;

    *out = PERMUTE_HANDLE_MAKE(slot, gen);
    return 0;
}

int permute_timer_settime(permute_kernel_t *k, permute_handle_t timer,
                          uint64_t initial_ns, uint64_t interval_ns)
{
    permute_posix_timer_t *t = permute_posix_timer_get(k, timer);
    if (t == NULL)
        return -ESRCH;

    // POSIX: a zero initial value disarms, whatever the interval says.
    if (initial_ns == 0) {
        t->armed = 0;
        t->interval_ns = 0;
        t->seq = ++k->sequence; // invalidate any outstanding heap entry
        return 0;
    }

    if (UINT64_MAX - k->now_ns < initial_ns)
        return -EOVERFLOW;

    t->interval_ns = interval_ns;
    t->next_ns = k->now_ns + initial_ns;
    t->armed = 1;
    t->seq = ++k->sequence;

    if (permute_eventq_push(&k->eventq, t->next_ns, t->seq, timer,
                            PERMUTE_TIMER_POSIX) != 0) {
        t->armed = 0;
        return -EAGAIN;
    }
    return 0;
}

int permute_timer_delete(permute_kernel_t *k, permute_handle_t timer)
{
    permute_posix_timer_t *t = permute_posix_timer_get(k, timer);
    if (t == NULL)
        return -ESRCH;

    t->armed = 0;
    t->seq = ++k->sequence;
    permute_table_free(&k->posix_timers, PERMUTE_HANDLE_SLOT(timer));
    return 0;
}

int permute_timer_fire(permute_kernel_t *k, permute_handle_t timer, uint64_t seq)
{
    permute_posix_timer_t *t = permute_posix_timer_get(k, timer);
    if (t == NULL || !t->armed || t->seq != seq)
        return 0; // deleted or re-armed since the entry was queued

    if (t->event.notify == PERMUTE_SIGEV_PULSE) {
        permute_handle_t chan;
        int32_t scoid;
        if (permute_coid_to_channel(k, t->owner_process, t->event.coid, &chan,
                                    &scoid) == 0) {
            int32_t prio = t->event.priority;
            if (prio <= 0)
                prio = 1;
            if (prio > PERMUTE_PRIO_MAX)
                prio = PERMUTE_PRIO_MAX;

            permute_result_t ignored;
            (void)permute_pulse_post(k, chan, t->event.code, t->event.value,
                                     prio, scoid, t->owner_pid, 0, &ignored);
        }
        // A dead connection drops the notification, which is what a pulse to
        // a closed channel does; it is not an error at timer level.
    }

    if (t->interval_ns > 0) {
        t->next_ns += t->interval_ns;
        t->seq = ++k->sequence;
        if (permute_eventq_push(&k->eventq, t->next_ns, t->seq, timer,
                                PERMUTE_TIMER_POSIX) != 0) {
            t->armed = 0;
            return 0;
        }
        t->armed = 1;
    } else {
        t->armed = 0;
    }

    return 1;
}
