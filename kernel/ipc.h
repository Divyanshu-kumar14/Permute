// SPDX-License-Identifier: Apache-2.0
//
// IPC core: the message path and its state machine.
//
// The kernel never decides who runs next. Each entry point returns CONTINUE if
// the calling thread can proceed, or BLOCKED if it must be suspended; the
// executor (P1-04) owns the scheduling decision. That split is what lets this
// state machine be tested exhaustively without a scheduler.
//
// States used here:
//   SEND   - message queued on a channel, not yet received by a server
//   REPLY  - message received by a server, reply outstanding
//   RECEIVE- thread waiting on a channel with an empty queue
//
// The sender moves SEND -> REPLY when the server receives, not when it replies.
// That is QNX behaviour: the client is blocked in MsgSend the whole time, but
// the two states have different cancellation and unblock semantics.

#ifndef PERMUTE_IPC_H
#define PERMUTE_IPC_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/objects.h"

typedef struct {
    uint32_t src_pid;
    uint32_t src_tid;
    uint32_t dst_pid;
    uint32_t dst_tid;
    uint32_t msglen;     // full payload length, even when truncated
    int32_t priority;
} permute_msg_info_t;

permute_action_t permute_ipc_send(permute_kernel_t *k, permute_handle_t thread,
                                  int coid, const void *smsg, size_t sbytes,
                                  void *rmsg, size_t rbytes,
                                  permute_result_t *res);

permute_action_t permute_ipc_receive(permute_kernel_t *k, permute_handle_t thread,
                                     int chid, void *msg, size_t bytes,
                                     permute_msg_info_t *info,
                                     permute_result_t *res);

permute_action_t permute_ipc_reply(permute_kernel_t *k, permute_handle_t thread,
                                   int32_t rcvid, long status, const void *msg,
                                   size_t bytes, permute_result_t *res);

permute_action_t permute_ipc_error(permute_kernel_t *k, permute_handle_t thread,
                                   int32_t rcvid, int error,
                                   permute_result_t *res);

// Non-blocking. Enqueues the pulse, or delivers it to a waiting receiver.
permute_action_t permute_ipc_send_pulse(permute_kernel_t *k,
                                        permute_handle_t thread, int coid,
                                        int priority, int32_t code,
                                        uint64_t value, permute_result_t *res);

#endif // PERMUTE_IPC_H
