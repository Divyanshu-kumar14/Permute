// SPDX-License-Identifier: Apache-2.0
//
// Guest packaging: separate statics per instance, constructor order, and a
// loud failure when an interposed symbol is called with no kernel installed.
//
// The two objects are the same source compiled twice, so any sharing of
// .data/.bss would be visible immediately.

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guest/dlmain.h"
#include "harness.h"

static int read_log(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <proc_counter__0.so> <proc_counter__1.so>\n",
                argv[0]);
        return 2;
    }

    char log_path[] = "/tmp/permute_ctor_XXXXXX";
    int fd = mkstemp(log_path);
    if (fd < 0) {
        perror("mkstemp");
        return 2;
    }
    close(fd);
    if (setenv("PERMUTE_CTOR_LOG", log_path, 1) != 0) {
        perror("setenv");
        return 2;
    }

    permute_guest_t a, b;

    // Load in spawn order. Constructors run here, not at start.
    CHECK_EQ(permute_guest_load(&a, argv[1]), 0);
    CHECK_EQ(permute_guest_load(&b, argv[2]), 0);
    CHECK(a.handle != NULL && b.handle != NULL);
    CHECK(a.handle != b.handle);
    CHECK(strstr(a.path, "proc_counter__0.so") != NULL);
    CHECK(strstr(b.path, "proc_counter__1.so") != NULL);

    char log[256] = { 0 };
    CHECK_EQ(read_log(log_path, log, sizeof log), 0);
    CHECK(strcmp(log, "0\n1\n") == 0); // constructor order == load order

    char *set42[] = { "counter", "set", "42", NULL };
    char *set7[] = { "counter", "set", "7", NULL };
    char *get[] = { "counter", "get", NULL };
    char *probe[] = { "counter", "probe", NULL };

    CHECK_EQ(permute_guest_start(&a, 3, set42), 42);
    CHECK_EQ(permute_guest_start(&a, 2, get), 42);

    // FR-4.14: a static written in instance 0 is invisible in instance 1.
    CHECK_EQ(permute_guest_start(&b, 2, get), 0);

    CHECK_EQ(permute_guest_start(&b, 3, set7), 7);
    CHECK_EQ(permute_guest_start(&a, 2, get), 42);
    CHECK_EQ(permute_guest_start(&b, 2, get), 7);

    // The shim is linked in and is ours: with no kernel table it fails loudly
    // instead of pretending to succeed.
    errno = 0;
    CHECK_EQ(permute_guest_start(&a, 2, probe), -1);
    CHECK_EQ(errno, ENOSYS);

    // A missing object reports an error rather than a null success.
    permute_guest_t missing;
    CHECK_EQ(permute_guest_load(&missing, "/nonexistent/permute-missing.so"), -1);
    CHECK(missing.error[0] != '\0');

    permute_guest_unload(&a);
    permute_guest_unload(&b);
    CHECK(a.handle == NULL && b.handle == NULL);

    unlink(log_path);
    return TEST_RESULT("test_guest");
}
