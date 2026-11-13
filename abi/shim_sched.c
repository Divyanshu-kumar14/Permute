// SPDX-License-Identifier: Apache-2.0
//
// Scheduling shims (FR-3.19, FR-3.20, FR-4.9).
//
// The kernel owns policy and priority; the shim only forwards. SchedSet clears
// sticky inherited priority in the kernel, not here.

#include <errno.h>

#include "abi/permute_iface.h"
#include "guest/hypercall.h"

int SchedSet(pid_t pid, int tid, int policy, const struct sched_param *param)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->sched_set == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->sched_set(pid, tid, policy, param);
}

int SchedGet(pid_t pid, int tid, struct sched_param *param)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->sched_get == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->sched_get(pid, tid, param);
}

int SchedYield(void)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->sched_yield == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->sched_yield();
}
