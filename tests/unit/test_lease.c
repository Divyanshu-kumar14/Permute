// SPDX-License-Identifier: Apache-2.0
//
// Lease claim, conflict, release, and stale reclamation (FR-5.3).

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "worker/lease.h"

int main(void)
{
    char tmpl[] = "/tmp/permute_lease_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (dir == NULL) {
        perror("mkdtemp");
        return 2;
    }

    char leases[512];
    snprintf(leases, sizeof leases, "%s/leases", dir);
    if (mkdir(leases, 0755) != 0) {
        perror("mkdir");
        return 2;
    }

    permute_lease_t a, b;

    // One owner, second claim refused while the first is live.
    CHECK_EQ(permute_lease_claim(dir, 0, 63, 0, &a), 0);
    CHECK(a.fd >= 0);
    CHECK_EQ(permute_lease_claim(dir, 0, 63, 0, &b), -EBUSY);

    permute_lease_release(&a);
    CHECK_EQ(a.fd, -1);
    CHECK_EQ(permute_lease_claim(dir, 0, 63, 0, &b), 0);
    permute_lease_release(&b);

    // A lease whose owner is gone is reclaimable only with --steal.
    char stale[600];
    snprintf(stale, sizeof stale, "%s/leases/batch-00000064.lease", dir);
    FILE *f = fopen(stale, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fprintf(f, "{\"pid\":999999,\"first\":64,\"last\":127}\n");
        fclose(f);
    }

    CHECK_EQ(permute_lease_is_stale(stale), 1);
    CHECK_EQ(permute_lease_claim(dir, 64, 127, 0, &a), -EBUSY);
    CHECK_EQ(permute_lease_claim(dir, 64, 127, 1, &a), 0);
    permute_lease_release(&a);
    CHECK_EQ(access(stale, F_OK), -1); // released removes the file

    // An unreadable lease is treated as stale rather than blocking forever.
    f = fopen(stale, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fprintf(f, "garbage\n");
        fclose(f);
    }
    CHECK_EQ(permute_lease_is_stale(stale), 1);
    CHECK_EQ(permute_lease_claim(dir, 64, 127, 1, &a), 0);
    permute_lease_release(&a);

    rmdir(leases);
    rmdir(dir);
    return TEST_RESULT("test_lease");
}
