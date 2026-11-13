// SPDX-License-Identifier: Apache-2.0
//
// Minimal assertion harness.
//
// Unity is the framework of record and arrives when the suite is split into
// per-module binaries (P1-15). Until then this keeps unit tests dependency-free
// and readable; the assertion surface is intentionally the same shape.

#ifndef PERMUTE_HARNESS_H
#define PERMUTE_HARNESS_H

#include <stdio.h>

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                           \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_failures++;                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                     \
    } while (0)

#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        g_checks++;                                                           \
        long long a_ = (long long)(actual);                                   \
        long long e_ = (long long)(expected);                                 \
        if (a_ != e_) {                                                       \
            g_failures++;                                                     \
            printf("FAIL %s:%d  %s == %s (%lld != %lld)\n", __FILE__,         \
                   __LINE__, #actual, #expected, a_, e_);                     \
        }                                                                     \
    } while (0)

#define TEST_RESULT(name)                                                     \
    (printf("%s: %d checks, %d failure(s)\n", name, g_checks, g_failures),    \
     g_failures == 0 ? 0 : 1)

#endif // PERMUTE_HARNESS_H
