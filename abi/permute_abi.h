// SPDX-License-Identifier: Apache-2.0
//
// Permute interposition ABI.
//
// Clean-room declarations written from public interface documentation.
// No vendor header is included or copied. Prototypes may drift if the vendor
// changes a signature; a signature test compiled against real headers (when an
// SDP is present) is the mitigation.

#ifndef PERMUTE_ABI_H
#define PERMUTE_ABI_H

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

#include "abi/permute_qnx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t rcvid_t;

// Public layout of the receive-info structure. Field order and widths are from
// public documentation; the layout is asserted in tests before it is trusted.
struct _msg_info {
    uint32_t nd;
    uint32_t srcnd;
    pid_t    pid;
    int32_t  tid;
    int32_t  chid;
    int32_t  scoid;
    int32_t  coid;
    int32_t  msglen;
    int32_t  srcmsglen;
    int32_t  dstmsglen;
    uint16_t priority;
    uint16_t flags;
};

// Kernel operation table. The shim never implements semantics; it forwards.
// A NULL entry (or a NULL table) is a hard error: the simulator must fail
// loudly rather than silently approximate.
typedef struct permute_kernel_ops {
    int      (*channel_create)(unsigned flags);
    int      (*connect_attach)(uint32_t reserved, pid_t pid, int chid,
                               unsigned index, int flags);
    long     (*msg_send)(int coid, const void *smsg, size_t sbytes,
                         void *rmsg, size_t rbytes);
    rcvid_t  (*msg_receive)(int chid, void *msg, size_t bytes,
                            struct _msg_info *info);
    int      (*msg_reply)(rcvid_t rcvid, long status, const void *msg,
                          size_t bytes);

    // Deterministic identity (FR-4.9). Ordinals, never addresses or host IDs.
    pid_t    (*getpid)(void);
    pid_t    (*gettid)(void);
    uint64_t (*thread_token)(void);

    // Virtual time (FR-4.8). The shim may not read a real clock.
    int      (*clock_gettime)(int clock_id, struct timespec *ts);
    int      (*nanosleep)(const struct timespec *req, struct timespec *rem);

    // Per-process entropy origin (FR-4.11). The shim expands it; libc's rand
    // is never used.
    uint64_t (*rand_seed)(void);

    // Object teardown.
    int      (*channel_destroy)(int chid);
    int      (*connect_detach)(int coid);

    // The rest of the message surface.
    int      (*msg_error)(rcvid_t rcvid, int error);
    int      (*msg_send_pulse)(int coid, int priority, int code, long value);
    int      (*msg_receive_pulse)(int chid, void *pulse, size_t bytes,
                                  struct _msg_info *info);

    // Secure events.
    int      (*msg_register_event)(struct sigevent *event, int coid);
    int      (*msg_deliver_event)(rcvid_t rcvid,
                                  const struct sigevent *event);
    int      (*msg_verify_event)(rcvid_t rcvid,
                                 const struct sigevent *event);

    // Timeouts (FR-3.21).
    int      (*timer_timeout)(int clock_id, uint32_t flags,
                              const struct sigevent *event,
                              const uint64_t *ntime, uint64_t *otime);

    // Scheduling.
    int      (*sched_set)(pid_t pid, int tid, int policy,
                          const struct sched_param *param);
    int      (*sched_get)(pid_t pid, int tid, struct sched_param *param);
    int      (*sched_yield)(void);
} permute_kernel_ops_t;

// The operation table is published through the hypercall page; see
// guest/hypercall.h for permute_ops() and permute_ops_install(). There is no
// public mutable pointer: the shim must read the page, not a global.

#ifdef __cplusplus
}
#endif

#endif // PERMUTE_ABI_H
