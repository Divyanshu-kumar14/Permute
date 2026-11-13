// SPDX-License-Identifier: Apache-2.0
//
// Batch leases.
//
// A worker claims a batch by creating its lease file with O_EXCL: the
// filesystem provides the atomicity, so two workers cannot both own a batch.
// A crashed worker leaves its lease behind; `steal` reclaims it when the
// recorded process is gone, which is what keeps one crash from stranding a
// batch forever.

#ifndef PERMUTE_LEASE_H
#define PERMUTE_LEASE_H

#include <stdint.h>

#define PERMUTE_LEASE_PATH_MAX 512

typedef struct {
    int fd;
    char path[PERMUTE_LEASE_PATH_MAX];
    uint32_t first;
    uint32_t last;
} permute_lease_t;

// Returns 0 when the lease was taken, -EBUSY when another live worker holds
// it, or another negative errno. With `steal`, a lease whose owner process no
// longer exists is removed and retried once.
int permute_lease_claim(const char *dir, uint32_t first, uint32_t last,
                        int steal, permute_lease_t *out);

void permute_lease_release(permute_lease_t *lease);

// 1 when the lease is unreadable or its owner is gone, 0 when it looks live.
int permute_lease_is_stale(const char *path);

#endif // PERMUTE_LEASE_H
