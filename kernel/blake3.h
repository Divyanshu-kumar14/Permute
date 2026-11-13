// SPDX-License-Identifier: Apache-2.0
//
// Portable BLAKE3 for the trace hash chain and run digest.
//
// Only the unkeyed hash mode and fixed 32-byte output are implemented: that is
// everything the trace format uses. No SIMD, no allocation, no threading, so
// it produces the same bytes on every host.
//
// The chain stores a 64-bit value: the first eight output bytes read as a
// little-endian integer.

#ifndef PERMUTE_BLAKE3_H
#define PERMUTE_BLAKE3_H

#include <stddef.h>
#include <stdint.h>

#define PERMUTE_BLAKE3_OUT_LEN 32

void permute_blake3(const void *data, size_t len,
                    uint8_t out[PERMUTE_BLAKE3_OUT_LEN]);

// First eight bytes of the digest, little-endian.
uint64_t permute_blake3_64(const void *data, size_t len);

// The hash of a‖b without allocating a joined buffer.
uint64_t permute_blake3_64_pair(const void *a, size_t alen, const void *b,
                                size_t blen);

#endif // PERMUTE_BLAKE3_H
