// SPDX-License-Identifier: Apache-2.0
//
// Golden-trace generator for the cross-language codec test.
//
//   trace_gen <path> golden        one record of every kind, JSON on stdout
//   trace_gen <path> bulk <n>      n filler records, count and digest only
//
// Each record is printed as JSON with the same field names the Python codec
// uses, so the test compares decoded bytes against the bytes that went in.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/trace.h"

static uint64_t g_seq;
static uint64_t g_time;

static void print_hex(const char *key, const void *data, unsigned len,
                      int last)
{
    printf("\"%s\":", key);
    if (data == NULL) {
        printf("null");
    } else {
        const unsigned char *p = (const unsigned char *)data;
        printf("\"");
        for (unsigned i = 0; i < len; i++)
            printf("%02x", p[i]);
        printf("\"");
    }
    if (!last)
        printf(",");
}

static void emit_and_print(permute_trace_t *t, uint8_t kind, uint16_t src,
                           uint16_t dst, uint32_t obj, uint32_t aux,
                           const uint8_t *blob, uint16_t blob_len,
                           const permute_sigevent_t *event, int print_json)
{
    g_seq++;
    g_time += 1000;

    if (permute_trace_emit(t, kind, g_seq, g_time, src, dst, obj, aux, blob,
                           blob_len, event) != 0) {
        fprintf(stderr, "emit failed at kind %u\n", kind);
        exit(1);
    }

    if (!print_json)
        return;

    uint8_t schema = permute_trace_schema[kind];
    printf("{\"kind\":\"%s\",\"seq\":%llu,\"t_ns\":%llu,\"src\":%u,",
           permute_trace_kind_name(kind), (unsigned long long)g_seq,
           (unsigned long long)g_time, src);

    if (schema & PERMUTE_TRACE_S_DST)
        printf("\"dst\":%u,", dst);
    else
        printf("\"dst\":null,");
    if (schema & PERMUTE_TRACE_S_OBJ)
        printf("\"obj\":%u,", obj);
    else
        printf("\"obj\":null,");
    if (schema & PERMUTE_TRACE_S_AUX)
        printf("\"aux\":%u,", aux);
    else
        printf("\"aux\":null,");

    print_hex("blob", blob_len ? blob : NULL, blob_len, 0);
    print_hex("event", event, event ? sizeof *event : 0, 1);
    printf("}\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <path> golden|bulk [n]\n", argv[0]);
        return 2;
    }

    const char *path = argv[1];
    const char *mode = argv[2];

    uint8_t build_id[32];
    uint8_t run_id[16];
    for (int i = 0; i < 32; i++)
        build_id[i] = (uint8_t)i;
    for (int i = 0; i < 16; i++)
        run_id[i] = (uint8_t)(0xA0 + i);

    permute_trace_t t;
    if (permute_trace_open(&t, path, 0x0123456789ABCDEFull, build_id, run_id,
                           0) != 0) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }

    if (strcmp(mode, "golden") == 0) {
        uint8_t blob[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        permute_sigevent_t event;
        memset(&event, 0, sizeof event);
        event.notify = PERMUTE_SIGEV_PULSE;
        event.coid = 7;
        event.value = 0x1122334455667788ull;
        event.code = 0x42;
        event.priority = 9;

        for (uint8_t kind = 0; kind < PERMUTE_TR_KIND_COUNT; kind++) {
            uint8_t schema = permute_trace_schema[kind];
            int has_blob = (kind == PERMUTE_TR_SEND || kind == PERMUTE_TR_RECV ||
                            kind == PERMUTE_TR_PULSE_SEND ||
                            kind == PERMUTE_TR_PULSE_RECV ||
                            kind == PERMUTE_TR_STIMULUS ||
                            kind == PERMUTE_TR_FAULT_INJECT ||
                            kind == PERMUTE_TR_ORACLE || kind == PERMUTE_TR_CRASH);
            int has_event = (kind == PERMUTE_TR_EVENT_DELIVER ||
                             kind == PERMUTE_TR_TIMER_ARM);

            emit_and_print(&t, kind, (uint16_t)(1 + kind),
                           (schema & PERMUTE_TRACE_S_DST) ? (uint16_t)(100 + kind)
                                                          : 0,
                           (uint32_t)(1000 + kind), (uint32_t)(7 + kind),
                           has_blob ? blob : NULL, has_blob ? 4 : 0,
                           has_event ? &event : NULL, 1);
        }
    } else if (strcmp(mode, "bulk") == 0) {
        if (argc < 4) {
            fprintf(stderr, "bulk needs a count\n");
            return 2;
        }
        long n = strtol(argv[3], NULL, 10);
        uint8_t payload[64];
        memset(payload, 0x5A, sizeof payload);

        for (long i = 0; i < n; i++)
            emit_and_print(&t, PERMUTE_TR_SEND, 1, 2, (uint32_t)i,
                           (uint32_t)sizeof payload, payload,
                           (uint16_t)sizeof payload, NULL, 0);
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }

    uint64_t digest = 0;
    if (permute_trace_close(&t, &digest) != 0) {
        fprintf(stderr, "close failed\n");
        return 1;
    }

    printf("DIGEST %016llx\n", (unsigned long long)digest);
    printf("RECORDS %llu\n", (unsigned long long)g_seq);
    return 0;
}
