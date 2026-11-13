// SPDX-License-Identifier: Apache-2.0
//
// TEST FIXTURE ONLY.
//
// Interposes a symbol that libc genuinely defines (`write`), because the IPC
// symbols have no libc counterpart on Linux and therefore cannot demonstrate
// link-order precedence. The replacement issues the raw syscall so it cannot
// recurse into itself.

#define _GNU_SOURCE
#include <stdatomic.h>
#include <sys/syscall.h>
#include <unistd.h>

_Atomic unsigned long permute_probe_write_calls = 0;

ssize_t write(int fd, const void *buf, size_t count)
{
    atomic_fetch_add_explicit(&permute_probe_write_calls, 1, memory_order_relaxed);
    return (ssize_t)syscall(SYS_write, fd, buf, count);
}
