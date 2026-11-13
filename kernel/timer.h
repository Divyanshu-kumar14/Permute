// SPDX-License-Identifier: Apache-2.0
//
// POSIX timers. They share the absolute-time heap with TimerTimeout, so the
// clock has one notion of the next event and ordering is total.

#ifndef PERMUTE_TIMER_H
#define PERMUTE_TIMER_H

#include "kernel/state.h"

int permute_timer_create(permute_kernel_t *k, permute_handle_t process,
                         const permute_sigevent_t *event,
                         permute_handle_t *out);
int permute_timer_settime(permute_kernel_t *k, permute_handle_t timer,
                          uint64_t initial_ns, uint64_t interval_ns);
int permute_timer_delete(permute_kernel_t *k, permute_handle_t timer);
permute_posix_timer_t *permute_posix_timer_get(permute_kernel_t *k,
                                               permute_handle_t h);

// Apply a due heap entry. Returns 1 if it delivered, 0 if it was stale.
int permute_timer_fire(permute_kernel_t *k, permute_handle_t timer,
                       uint64_t seq);

#endif // PERMUTE_TIMER_H
