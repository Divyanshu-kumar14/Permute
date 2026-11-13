// SPDX-License-Identifier: Apache-2.0
//
// Hypercall page.
//
// The kernel publishes its operation table in a page mapped at a fixed virtual
// address. The shim reads that page instead of a mutable global, so the call
// path cannot silently fall back to libc and cannot be redirected by a stray
// assignment.
//
// Two-phase by necessity:
//   host:   permute_hypercall_init()  -> maps the page, returns its base
//   guest:  permute_hypercall_bind()  -> validates the magic, sets the module's
//                                        view of that page
//
// The bind step exists because each simulated process is a separate shared
// object with its own statics: a module that never calls bind() would see a
// null page and fail every call with ENOSYS. Binding per module is explicit
// rather than accidental.

#ifndef PERMUTE_HYPERCALL_H
#define PERMUTE_HYPERCALL_H

#include <stdint.h>

#include "abi/permute_abi.h"

#define PERMUTE_HYPERCALL_MAGIC   0x504D5448u /* "PMTH" */
#define PERMUTE_HYPERCALL_VERSION 1u
#define PERMUTE_HYPERCALL_SIZE    (1u << 20) /* 1 MiB reserved, one page used */

typedef struct permute_hypercall_page {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t page_size;
    uint32_t reserved0;
    const permute_kernel_ops_t *ops;
    uint64_t reserved[3];
} permute_hypercall_page_t;

// Candidate bases, tried in order. The first entry must be valid for a 39-bit
// user address space so the same binary works on aarch64 hosts.
extern const uintptr_t permute_hypercall_candidates[];
extern const unsigned permute_hypercall_candidate_count;

// Host: map the page and bind it in the current module. Returns the base, or 0
// when no candidate could be claimed (or PERMUTE_NO_HYPERCALL is set).
uintptr_t permute_hypercall_init(void);

// Guest: adopt an already-mapped page. Returns 0 on success, -1 if the magic or
// version does not match (which means the caller passed the wrong address).
int permute_hypercall_bind(uintptr_t base);

// Introspection: 0 when running on the fallback path.
uintptr_t permute_hypercall_base(void);

// The active operation table. Never returns a table the host did not install.
const permute_kernel_ops_t *permute_ops(void);

// Install (or clear, with NULL) the operation table.
int permute_ops_install(const permute_kernel_ops_t *ops);

#endif // PERMUTE_HYPERCALL_H
