// SPDX-License-Identifier: Apache-2.0
//
// Determinism bootstrap.
//
// Order matters: locale and timezone are pinned before anything can format a
// timestamp, and address-space randomisation is removed before the first
// guest instruction. The ASLR flag only takes effect on exec, so the process
// re-executes itself exactly once and gates on an environment variable.

#ifndef PERMUTE_DETERMINISM_H
#define PERMUTE_DETERMINISM_H

#include <stdint.h>

typedef enum {
    PERMUTE_ASLR_OFF = 0,        // verified: the persona bit is set
    PERMUTE_ASLR_UNAVAILABLE = 1 // the kernel refused or /proc is unreadable
} permute_aslr_status_t;

// Must be the first thing main() does. May not return (re-exec).
permute_aslr_status_t permute_determinism_init(int argc, char **argv);

// 64-bit fingerprint of /proc/self/maps. Two runs of the same binary with ASLR
// disabled must produce the same value. This is a layout check, not a security
// hash: it exists to catch unexpected address-space variation.
uint64_t permute_maps_fingerprint(void);

// The persona bit as observed in /proc/self/personality, or -1 if unreadable.
int permute_aslr_bit_observed(void);

#endif // PERMUTE_DETERMINISM_H
