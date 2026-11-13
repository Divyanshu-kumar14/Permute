// SPDX-License-Identifier: Apache-2.0

#define _GNU_SOURCE

#include "kernel/determinism.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <time.h>
#include <unistd.h>

#define PERMUTE_ASLR_ENV "PERMUTE_ASLR_OFF"
#define PERMUTE_PERSONA_FILE "/proc/self/personality"
#define PERMUTE_MAPS_FILE "/proc/self/maps"

static uint64_t fnv1a_update(uint64_t h, const void *data, size_t len)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static int parse_hex_file(const char *path, unsigned long *out)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;

    char buf[64];
    if (fgets(buf, sizeof buf, f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);

    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(buf, &end, 16);
    if (errno != 0 || end == buf)
        return -1;

    *out = value;
    return 0;
}

int permute_aslr_bit_observed(void)
{
    unsigned long persona = 0;
    if (parse_hex_file(PERMUTE_PERSONA_FILE, &persona) != 0)
        return -1;
    return (persona & (unsigned long)ADDR_NO_RANDOMIZE) != 0 ? 1 : 0;
}

uint64_t permute_maps_fingerprint(void)
{
    FILE *f = fopen(PERMUTE_MAPS_FILE, "r");
    if (f == NULL)
        return 0;

    uint64_t h = 0xCBF29CE484222325ull;
    char line[8192];
    while (fgets(line, sizeof line, f) != NULL)
        h = fnv1a_update(h, line, strlen(line));

    fclose(f);
    return h;
}

permute_aslr_status_t permute_determinism_init(int argc, char **argv)
{
    (void)argc;

    // Pin locale and timezone before anything can format a value.
    (void)setenv("LC_ALL", "C", 1);
    (void)setenv("TZ", "UTC", 1);
    tzset();

    // Test-only escape hatch. Used by the negative control to prove that the
    // layout fingerprint can actually detect address-space variation; never
    // set this for a real run.
    if (getenv("PERMUTE_NO_ASLR_REEXEC") != NULL)
        return PERMUTE_ASLR_UNAVAILABLE;

    if (getenv(PERMUTE_ASLR_ENV) != NULL) {
        // Second launch: verify rather than assume the bit took effect.
        return (permute_aslr_bit_observed() == 1) ? PERMUTE_ASLR_OFF
                                                  : PERMUTE_ASLR_UNAVAILABLE;
    }

    int current = personality(0xffffffffU);
    if (current == -1)
        return PERMUTE_ASLR_UNAVAILABLE;

    if (personality(current | ADDR_NO_RANDOMIZE) == -1)
        return PERMUTE_ASLR_UNAVAILABLE;

    if (setenv(PERMUTE_ASLR_ENV, "1", 1) != 0)
        return PERMUTE_ASLR_UNAVAILABLE;

    execv("/proc/self/exe", argv);

    // Unreachable unless exec failed.
    return PERMUTE_ASLR_UNAVAILABLE;
}
