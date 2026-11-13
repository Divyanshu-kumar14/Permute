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
#include <stdlib.h>
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

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->pthread_create == NULL) {
        errno = ENOSYS;
        return -1;
    }

    uint64_t token = 0;
    int rc = ops->pthread_create(&token, attr, start_routine, arg);
    if (rc != 0) {
        errno = rc;
        return -1;
    }

    memcpy(thread, &token, sizeof *thread); // declared nonnull by the header
    return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->pthread_join == NULL) {
        errno = ENOSYS;
        return -1;
    }

    uint64_t token = 0;
    memcpy(&token, &thread, sizeof thread);
    int rc = ops->pthread_join(token, retval);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

void pthread_exit(void *retval)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops != NULL && ops->pthread_exit != NULL)
        ops->pthread_exit(retval);

    abort(); // no table: fail loudly rather than return from a noreturn call
}

int pthread_cancel(pthread_t thread)
{
    const permute_kernel_ops_t *ops = permute_ops();
    if (ops == NULL || ops->pthread_cancel == NULL) {
        errno = ENOSYS;
        return -1;
    }

    uint64_t token = 0;
    memcpy(&token, &thread, sizeof thread);
    int rc = ops->pthread_cancel(token);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}
