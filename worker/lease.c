// SPDX-License-Identifier: Apache-2.0

#define _GNU_SOURCE

#include "worker/lease.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PERMUTE_LEASE_MAX_AGE_S 300

int permute_lease_is_stale(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 1;

    char buf[256];
    if (fgets(buf, sizeof buf, f) == NULL) {
        fclose(f);
        return 1;
    }
    fclose(f);

    long pid = 0;
    if (sscanf(buf, "{\"pid\":%ld", &pid) != 1 || pid <= 0)
        return 1;

    if (kill((pid_t)pid, 0) == 0)
        return 0; // owner is alive

    return errno == ESRCH ? 1 : 0;
}

int permute_lease_claim(const char *dir, uint32_t first, uint32_t last,
                        int steal, permute_lease_t *out)
{
    snprintf(out->path, sizeof out->path, "%s/leases/batch-%08u.lease", dir,
             first);
    out->fd = -1;
    out->first = first;
    out->last = last;

    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = open(out->path, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0) {
            char body[160];
            int n = snprintf(body, sizeof body,
                             "{\"pid\":%ld,\"first\":%u,\"last\":%u,"
                             "\"started\":%lld}\n",
                             (long)getpid(), first, last,
                             (long long)time(NULL));
            if (write(fd, body, (size_t)n) != n) {
                close(fd);
                unlink(out->path);
                return -EIO;
            }
            out->fd = fd;
            return 0;
        }

        if (errno != EEXIST)
            return -errno;

        if (!steal || attempt == 1 || !permute_lease_is_stale(out->path))
            return -EBUSY;

        if (unlink(out->path) != 0 && errno != ENOENT)
            return -errno;
    }

    return -EBUSY;
}

void permute_lease_release(permute_lease_t *lease)
{
    if (lease == NULL)
        return;
    if (lease->fd >= 0)
        close(lease->fd);
    if (lease->path[0] != '\0')
        unlink(lease->path);
    lease->fd = -1;
    lease->path[0] = '\0';
}
