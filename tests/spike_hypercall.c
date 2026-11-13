// SPDX-License-Identifier: Apache-2.0
//
// P0-05 spike: the hypercall page is real, at a fixed address, and actually
// probed.
//
// Three things are proven, and one would be enough to fool a careless test:
//   1. the page lands on the first candidate and the shim reads through it;
//   2. when that candidate is already taken, init moves to the next one
//      (an implementation with a hardcoded address would pass test 1 and fail
//      this one);
//   3. the fallback path still works when fixed mappings are unavailable,
//      and reports itself honestly as absent.

#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "abi/permute_iface.h"
#include "guest/hypercall.h"

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static unsigned long n_send;

static long mock_msg_send(int coid, const void *smsg, size_t sbytes,
                          void *rmsg, size_t rbytes)
{
    (void)coid; (void)smsg; (void)sbytes; (void)rmsg; (void)rbytes;
    n_send++;
    return 0x1234;
}

static const permute_kernel_ops_t mock_ops = {
    .msg_send = mock_msg_send,
};

static int failures;

static void check(int condition, const char *what)
{
    printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        failures++;
}

int main(int argc, char **argv)
{
    int occupy_first = 0;
    int force_fallback = 0;
    int bench = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--occupy-first") == 0)
            occupy_first = 1;
        else if (strcmp(argv[i], "--fallback") == 0)
            force_fallback = 1;
        else if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc)
            bench = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--occupy-first] [--fallback] [--bench N]\n",
                    argv[0]);
            return 2;
        }
    }

    if (occupy_first) {
        void *p = mmap((void *)permute_hypercall_candidates[0],
                       PERMUTE_HYPERCALL_SIZE, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        // If the address is already taken we do not care who took it: init must
        // avoid it either way.
        (void)p;
    }

    if (force_fallback)
        (void)setenv("PERMUTE_NO_HYPERCALL", "1", 1);

    uintptr_t base = permute_hypercall_init();
    printf("MODE=%s\n", force_fallback ? "fallback"
                                      : occupy_first ? "occupy-first"
                                                     : "default");

    if (force_fallback) {
        printf("HYPERCALL=fallback\n");
        check(base == 0, "fallback selected, no fixed mapping claimed");
        check(permute_hypercall_base() == 0, "no page is bound");
    } else {
        printf("HYPERCALL=0x%016llx\n", (unsigned long long)base);
        check(base != 0, "a candidate address was claimed");
        check(permute_hypercall_base() == base, "the page is bound");

        if (occupy_first)
            check(base != permute_hypercall_candidates[0],
                  "probe skipped the occupied candidate");
        else
            check(base == permute_hypercall_candidates[0],
                  "probe took the first candidate");

        permute_hypercall_page_t *page = (permute_hypercall_page_t *)base;
        check(page->magic == PERMUTE_HYPERCALL_MAGIC &&
                  page->version == PERMUTE_HYPERCALL_VERSION,
              "page magic and version are valid");
    }

    permute_ops_install(&mock_ops);

    if (bench > 0) {
        for (int i = 0; i < 1000; i++)
            (void)MsgSend(1, "x", 1, NULL, 0);

        uint64_t t0 = now_ns();
        for (int i = 0; i < bench; i++)
            (void)MsgSend(1, "x", 1, NULL, 0);
        uint64_t t1 = now_ns();

        printf("BENCH_CALLS=%d\n", bench);
        printf("NS_PER_CALL=%.2f\n", (double)(t1 - t0) / (double)bench);
        return 0;
    }

    check(permute_ops() == &mock_ops, "installed table is visible through ops()");

    long rc = MsgSend(1, "x", 1, NULL, 0);
    check(rc == 0x1234 && n_send == 1,
          "MsgSend reaches the kernel through the active table");

    permute_ops_install(NULL);
    errno = 0;
    check(MsgSend(1, "x", 1, NULL, 0) == -1 && errno == ENOSYS,
          "clearing the table yields -1/ENOSYS, not a stale call");

    permute_ops_install(&mock_ops);
    check(MsgSend(1, "x", 1, NULL, 0) == 0x1234 && n_send == 2,
          "reinstalling the table restores the call path");

    printf("RESULT=%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
