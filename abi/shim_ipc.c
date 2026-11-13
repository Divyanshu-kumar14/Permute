// SPDX-License-Identifier: Apache-2.0
//
// Strong-symbol shims. Each function forwards to the installed kernel table.
//
// Design rules enforced here:
//   1. No state. The shim owns nothing; every decision belongs to the kernel.
//   2. No fallback. If no kernel is installed, return -1/ENOSYS instead of
//      pretending to succeed.
//   3. Payload pointers are passed through; the kernel copies before returning
//      (the shim must never retain them).

#include "abi/permute_iface.h"

const permute_kernel_ops_t *permute_kernel_ops = NULL;

#define PERMUTE_REQUIRE(op)                                                   \
    do {                                                                      \
        if (!permute_kernel_ops || !permute_kernel_ops->op) {                 \
            errno = ENOSYS;                                                   \
            return -1;                                                        \
        }                                                                     \
    } while (0)

int ChannelCreate(unsigned flags)
{
    PERMUTE_REQUIRE(channel_create);
    return permute_kernel_ops->channel_create(flags);
}

int ConnectAttach(uint32_t reserved, pid_t pid, int chid, unsigned index,
                  int flags)
{
    PERMUTE_REQUIRE(connect_attach);
    return permute_kernel_ops->connect_attach(reserved, pid, chid, index, flags);
}

long MsgSend(int coid, const void *smsg, size_t sbytes, void *rmsg,
             size_t rbytes)
{
    PERMUTE_REQUIRE(msg_send);
    return permute_kernel_ops->msg_send(coid, smsg, sbytes, rmsg, rbytes);
}

rcvid_t MsgReceive(int chid, void *msg, size_t bytes, struct _msg_info *info)
{
    if (!permute_kernel_ops || !permute_kernel_ops->msg_receive) {
        errno = ENOSYS;
        return -1;
    }
    return permute_kernel_ops->msg_receive(chid, msg, bytes, info);
}

int MsgReply(rcvid_t rcvid, long status, const void *msg, size_t bytes)
{
    PERMUTE_REQUIRE(msg_reply);
    return permute_kernel_ops->msg_reply(rcvid, status, msg, bytes);
}
