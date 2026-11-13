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
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

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
} permute_kernel_ops_t;

// Installed by the host harness before any guest code runs. The eventual
// implementation publishes this through the fixed-address hypercall page; the
// pointer indirection is the same either way.
extern const permute_kernel_ops_t *permute_kernel_ops;

#ifdef __cplusplus
}
#endif

#endif // PERMUTE_ABI_H
