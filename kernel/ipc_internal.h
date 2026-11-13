// SPDX-License-Identifier: Apache-2.0
//
// Internal seam between the message path (msg.c) and the pulse/event path
// (pulse.c). Not part of the guest-facing surface.

#ifndef PERMUTE_IPC_INTERNAL_H
#define PERMUTE_IPC_INTERNAL_H

#include "kernel/ipc.h"

// Post a pulse to a channel: hand it to the highest-priority waiter that can
// accept a pulse, or queue it. Never blocks and never touches priority.
permute_action_t permute_pulse_post(permute_kernel_t *k, permute_handle_t chan,
                                    int32_t code, uint64_t value,
                                    int32_t priority, int32_t scoid,
                                    uint32_t src_pid, uint32_t src_tid,
                                    permute_result_t *res);

// Called after a connection row has been freed. Emits _PULSE_CODE_DISCONNECT
// when that was the client's last connection to a channel asking for it.
void permute_pulse_on_detach(permute_kernel_t *k, permute_handle_t chan,
                             int32_t scoid, uint32_t client_pid);

#endif // PERMUTE_IPC_INTERNAL_H
