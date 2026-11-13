// SPDX-License-Identifier: Apache-2.0
//
// Strong-symbol shims. Each function forwards to the active kernel table,
// which is read from the hypercall page.
//
// Design rules enforced here:
//   1. No state. The shim owns nothing; every decision belongs to the kernel.
//   2. No fallback. If no table is installed, return -1/ENOSYS instead of
//      pretending to succeed.
//   3. Payload pointers are passed through; the kernel copies before returning
//      (the shim must never retain them).

#include "abi/permute_iface.h"
#include "guest/hypercall.h"

int ChannelCreate(unsigned flags)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->channel_create == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->channel_create(flags);
}

int ConnectAttach(uint32_t reserved, pid_t pid, int chid, unsigned index,
                  int flags)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->connect_attach == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->connect_attach(reserved, pid, chid, index, flags);
}

long MsgSend(int coid, const void *smsg, size_t sbytes, void *rmsg,
             size_t rbytes)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->msg_send == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->msg_send(coid, smsg, sbytes, rmsg, rbytes);
}

rcvid_t MsgReceive(int chid, void *msg, size_t bytes, struct _msg_info *info)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->msg_receive == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->msg_receive(chid, msg, bytes, info);
}

int MsgReply(rcvid_t rcvid, long status, const void *msg, size_t bytes)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->msg_reply == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->msg_reply(rcvid, status, msg, bytes);
}
