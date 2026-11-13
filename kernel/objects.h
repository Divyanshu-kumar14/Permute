// SPDX-License-Identifier: Apache-2.0
//
// Object lifecycle API. All functions return 0 or a negative errno; the shim
// layer (P1-02) converts those into -1/errno at the guest boundary.

#ifndef PERMUTE_OBJECTS_H
#define PERMUTE_OBJECTS_H

#include "kernel/state.h"

// Table primitives (exposed for tests and for the pools in state.h).
void permute_table_init(permute_table_t *t, uint32_t capacity);
int  permute_table_alloc(permute_table_t *t, uint32_t *slot, uint32_t *gen);
void permute_table_free(permute_table_t *t, uint32_t slot);
int  permute_table_valid(const permute_table_t *t, permute_handle_t h);

// Kernel
void permute_kernel_init(permute_kernel_t *k, uint64_t seed);
void permute_kernel_destroy(permute_kernel_t *k);

// Processes
int permute_process_create(permute_kernel_t *k, const char *name,
                           permute_handle_t *out);
int permute_process_destroy(permute_kernel_t *k, permute_handle_t h);
permute_process_t *permute_process_get(permute_kernel_t *k, permute_handle_t h);
permute_handle_t permute_process_handle_by_pid(permute_kernel_t *k,
                                               uint32_t pid);
permute_process_t *permute_process_by_pid(permute_kernel_t *k, uint32_t pid);

// Threads
int permute_thread_create(permute_kernel_t *k, permute_handle_t process,
                          int32_t base_prio, permute_policy_t policy,
                          permute_handle_t *out);
int permute_thread_destroy(permute_kernel_t *k, permute_handle_t h);
permute_thread_t *permute_thread_get(permute_kernel_t *k, permute_handle_t h);
permute_thread_t *permute_thread_by_tid(permute_kernel_t *k, uint32_t pid,
                                        uint32_t tid);
permute_thread_t *permute_thread_by_global_tid(permute_kernel_t *k,
                                               uint32_t tid);

// Channels (chid is process-local)
int permute_channel_create(permute_kernel_t *k, permute_handle_t process,
                           uint32_t flags, int *chid_out,
                           permute_handle_t *out);
int permute_channel_destroy(permute_kernel_t *k, permute_handle_t process,
                            int chid);
permute_channel_t *permute_channel_get(permute_kernel_t *k, permute_handle_t h);
int permute_channel_lookup(permute_kernel_t *k, permute_handle_t process,
                           int chid, permute_handle_t *out);

// Connections (coid equals the fd value; process-local)
int permute_connect_attach(permute_kernel_t *k, permute_handle_t client,
                           permute_handle_t server, int chid, uint32_t flags,
                           int *coid_out, permute_handle_t *out);
int permute_connect_detach(permute_kernel_t *k, permute_handle_t client,
                           int coid);
permute_connection_t *permute_connection_get(permute_kernel_t *k,
                                             permute_handle_t h);
int permute_connection_lookup(permute_kernel_t *k, permute_handle_t process,
                              int coid, permute_handle_t *out);

// Resolve a coid to a channel. Negative coids address the caller's own
// channel (-chid), which is how a thread signals itself without a connection.
int permute_coid_to_channel(permute_kernel_t *k, permute_handle_t process,
                            int coid, permute_handle_t *chan_out,
                            int32_t *scoid_out);

// Messages (the message slot is the rcvid slot)
int permute_message_create(permute_kernel_t *k, permute_handle_t sender,
                           uint32_t sender_tid, uint32_t dst_pid,
                           int32_t send_prio, const void *payload,
                           uint32_t payload_len, permute_handle_t *out);
int permute_message_destroy(permute_kernel_t *k, permute_handle_t h);
permute_message_t *permute_message_get(permute_kernel_t *k,
                                       permute_handle_t h);
int permute_message_lookup_rcvid(permute_kernel_t *k, int32_t rcvid,
                                 permute_handle_t *out);

// Pulses
int permute_pulse_create(permute_kernel_t *k, uint32_t src_pid,
                         uint32_t src_tid, int32_t code, uint64_t value,
                         int32_t priority, permute_handle_t *out);
int permute_pulse_destroy(permute_kernel_t *k, permute_handle_t h);
permute_pulse_t *permute_pulse_get(permute_kernel_t *k, permute_handle_t h);

// Queue: priority-ordered, FIFO within equal priority (FR-3.8).
void permute_queue_init(permute_queue_t *q);
int  permute_queue_insert(permute_queue_t *q, permute_handle_t h,
                          int32_t priority, uint64_t sequence, uint8_t kind);
int  permute_queue_pop(permute_queue_t *q, permute_queue_entry_t *out);
// Pops the first entry whose kind intersects kind_mask. Used to find the
// highest-priority waiter that can accept the item being delivered.
int  permute_queue_pop_matching(permute_queue_t *q, uint8_t kind_mask,
                                permute_queue_entry_t *out);
int  permute_queue_remove(permute_queue_t *q, permute_handle_t h);
uint32_t permute_queue_length(const permute_queue_t *q);

// Thread helpers
void permute_thread_recompute_prio(permute_thread_t *t);
// SchedSet: sets base priority, clears sticky, and re-places the thread in the
// ready queue if it is queued.
int  permute_thread_set_prio(permute_kernel_t *k, permute_handle_t h,
                             int32_t base_prio);

// Block and wake are the only places that change state and ready-queue
// membership together. Keeping them paired is what stops a blocked thread
// from being picked, and a woken thread from being invisible to the scheduler.
void permute_thread_block(permute_kernel_t *k, permute_thread_t *t,
                          permute_thread_state_t state, permute_handle_t wobj);
void permute_thread_wake(permute_kernel_t *k, permute_thread_t *t, long ret,
                         int err, int at_head);

#endif // PERMUTE_OBJECTS_H
