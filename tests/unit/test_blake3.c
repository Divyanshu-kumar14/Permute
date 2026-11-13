// SPDX-License-Identifier: Apache-2.0
//
// BLAKE3 against the official test vectors. The input for a length N is the
// repeating byte sequence 0,1,2,...,250,0,1,... for N bytes, as the vector
// file specifies.

#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "kernel/blake3.h"

static void fill(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)(i % 251);
}

static void check_vector(size_t n, const char *hex)
{
    static uint8_t buf[102400];
    uint8_t out[PERMUTE_BLAKE3_OUT_LEN];
    uint8_t want[PERMUTE_BLAKE3_OUT_LEN];

    for (int i = 0; i < 32; i++) {
        unsigned hi = 0, lo = 0;
        char a = hex[2 * i], b = hex[2 * i + 1];
        hi = (a <= '9') ? (unsigned)(a - '0') : (unsigned)(a - 'a' + 10);
        lo = (b <= '9') ? (unsigned)(b - '0') : (unsigned)(b - 'a' + 10);
        want[i] = (uint8_t)((hi << 4) | lo);
    }

    fill(buf, n);
    permute_blake3(buf, n, out);

    if (memcmp(out, want, sizeof out) != 0) {
        printf("FAIL blake3 len=%zu\n  got  ", n);
        for (int i = 0; i < 32; i++)
            printf("%02x", out[i]);
        printf("\n  want %s\n", hex);
        g_failures++;
    }
    g_checks++;
}

int main(void)
{
    check_vector(0,
                 "af1349b9f5f9a1a6a0404dea36dcc949"
                 "9bcb25c9adc112b7cc9a93cae41f3262");
    check_vector(1,
                 "2d3adedff11b61f14c886e35afa03673"
                 "6dcd87a74d27b5c1510225d0f592e213");
    check_vector(63,
                 "e9bc37a594daad83be9470df7f7b3798"
                 "297c3d834ce80ba85d6e207627b7db7b");
    check_vector(64,
                 "4eed7141ea4a5cd4b788606bd23f46e2"
                 "12af9cacebacdc7d1f4c6dc7f2511b98");
    check_vector(65,
                 "de1e5fa0be70df6d2be8fffd0e99ceaa"
                 "8eb6e8c93a63f2d8d1c30ecb6b263dee");
    check_vector(127,
                 "d81293fda863f008c09e92fc382a81f5"
                 "a0b4a1251cba1634016a0f86a6bd640d");
    check_vector(128,
                 "f17e570564b26578c33bb7f44643f539"
                 "624b05df1a76c81f30acd548c44b45ef");
    check_vector(1023,
                 "10108970eeda3eb932baac1428c7a216"
                 "3b0e924c9a9e25b35bba72b28f70bd11");
    check_vector(1024,
                 "42214739f095a406f3fc83deb889744a"
                 "c00df831c10daa55189b5d121c855af7");
    check_vector(1025,
                 "d00278ae47eb27b34faecf67b4fe263f"
                 "82d5412916c1ffd97c8cb7fb814b8444");
    check_vector(2048,
                 "e776b6028c7cd22a4d0ba182a8bf6220"
                 "5d2ef576467e838ed6f2529b85fba24a");
    check_vector(2049,
                 "5f4d72f40d7a5f82b15ca2b2e44b1de3"
                 "c2ef86c426c95c1af0b6879522563030");
    check_vector(3072,
                 "b98cb0ff3623be03326b373de6b90952"
                 "18513e64f1ee2edd2525c7ad1e5cffd2");
    check_vector(4096,
                 "015094013f57a5277b59d8475c050104"
                 "2c0b642e531b0a1c8f58d2163229e969");
    check_vector(5120,
                 "9cadc15fed8b5d854562b26a9536d970"
                 "7cadeda9b143978f319ab34230535833");
    check_vector(8192,
                 "aae792484c8efe4f19e2ca7d371d8c46"
                 "7ffb10748d8a5a1ae579948f718a2a63");
    check_vector(16384,
                 "f875d6646de28985646f34ee13be9a57"
                 "6fd515f76b5b0a26bb324735041ddde4");
    check_vector(31744,
                 "62b6960e1a44bcc1eb1a611a8d6235b6"
                 "b4b78f32e7abc4fb4c6cdcce94895c47");
    check_vector(102400,
                 "bc3e3d41a1146b069abffad3c0d44860"
                 "cf664390afce4d9661f7902e7943e085");

    // The 64-bit chain value is the first eight digest bytes, little-endian.
    static const uint8_t empty[1] = { 0 };
    CHECK_EQ(permute_blake3_64(empty, 0), 0xa6a1f9f5b94913afULL);

    return TEST_RESULT("test_blake3");
}
