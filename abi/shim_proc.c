// SPDX-License-Identifier: Apache-2.0
//
// Process identity shims (FR-4.9).
//
// getpid and gettid return deterministic ordinals from the kernel, never the
// host's values. A guest that logs its pid therefore produces the same log on
// every machine, and two runs cannot differ because the host reused a pid.

#include <errno.h>

#include "abi/permute_iface.h"
#include "guest/hypercall.h"

pid_t getpid(void)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->getpid == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->getpid();
}

pid_t gettid(void)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->gettid == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return ops->gettid();
}
