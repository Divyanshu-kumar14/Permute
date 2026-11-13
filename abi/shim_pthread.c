// SPDX-License-Identifier: Apache-2.0
//
// Thread identity shims (FR-4.9).
//
// pthread_self must return a stable token that is not an address: a real
// pthread_t is a pointer, so leaving it alone would leak host layout into the
// guest's observable behaviour. The kernel supplies an ordinal token.
//
// pthread_create is not here yet. It needs the executor, and a shim that
// returned a host thread would reintroduce real scheduling.

#include <errno.h>
#include <string.h>

#include "abi/permute_iface.h"
#include "guest/hypercall.h"

_Static_assert(sizeof(pthread_t) <= sizeof(uint64_t),
               "pthread_t must fit in the kernel's 64-bit ordinal token");

pthread_t pthread_self(void)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->thread_token == NULL) {
        errno = ENOSYS;
        return (pthread_t)0;
    }

    uint64_t token = ops->thread_token();
    pthread_t self = (pthread_t)0;
    memcpy(&self, &token, sizeof self);
    return self;
}

int pthread_equal(pthread_t a, pthread_t b)
{
    return a == b;
}
