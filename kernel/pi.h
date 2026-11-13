// SPDX-License-Identifier: Apache-2.0
//
// Priority inheritance: the message-driven kind and the mutex kind.
//
// They are deliberately separate mechanisms with different lifetimes:
//
//   sticky_prio  message-driven, assigned at receive, never reverts on reply
//   mutex_boost  PTHREAD_PRIO_INHERIT, recomputed on unlock
//   prot_ceil    PTHREAD_PRIO_PROTECT, recomputed on unlock
//
// Effective priority is the maximum of base, sticky, mutex_boost, and
// prot_ceil (PRD 9.1). A message assignment can therefore lower the inherited
// value but never the effective priority below the thread's own base.

#ifndef PERMUTE_PI_H
#define PERMUTE_PI_H

#include "kernel/state.h"

// Raise a receive-blocked thread to the sender's priority before it runs, so
// a medium-priority thread cannot delay the receive. No-op on a channel with
// _NTO_CHF_FIXED_PRIORITY or when the sender is not higher.
void permute_pi_on_send(permute_kernel_t *k, permute_handle_t channel,
                        permute_thread_t *receiver, int32_t sender_prio);

// Assign the inherited priority from the item just received: the maximum over
// everything still pending on the channel, including that item (FR-3.14).
// This can lower the inherited value, and it is what makes a multi-hop chain
// work without a donor list.
void permute_pi_on_receive(permute_kernel_t *k, permute_handle_t channel,
                           permute_thread_t *receiver, int32_t item_prio);

// Clamp a donated priority to the thread's process ceiling unless the process
// holds the priority ability (FR-3.16).
int32_t permute_pi_clamp(permute_kernel_t *k, const permute_thread_t *t,
                         int32_t prio);

// Mutexes. Lock returns CONTINUE on success (possibly after taking a free
// mutex) or BLOCKED when the caller is queued on the owner.
int permute_mutex_create(permute_kernel_t *k, permute_mutex_proto_t proto,
                         int32_t ceiling, int recursive,
                         permute_handle_t *out);
int permute_mutex_destroy(permute_kernel_t *k, permute_handle_t h);
permute_mutex_t *permute_mutex_get(permute_kernel_t *k, permute_handle_t h);

permute_action_t permute_mutex_lock(permute_kernel_t *k, permute_handle_t thread,
                                    permute_handle_t mutex,
                                    permute_result_t *res);
permute_action_t permute_mutex_unlock(permute_kernel_t *k,
                                      permute_handle_t thread,
                                      permute_handle_t mutex,
                                      permute_result_t *res);

// A waiter left the queue without taking the lock (timeout or death). The
// owner's inherited priority must be recomputed from those that remain.
void permute_pi_mutex_waiter_gone(permute_kernel_t *k, permute_mutex_t *m);

#endif // PERMUTE_PI_H
