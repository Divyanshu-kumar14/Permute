// SPDX-License-Identifier: Apache-2.0
//
// .pmt writer: append-only chunks with CRC32C and a BLAKE3-64 hash chain.
//
// Layout (all integers little-endian):
//
//   Header 72 B : magic "PMT1" | version u16 | flags u16 | run_id[16]
//                 | config_hash u64 | sut_build_id[32] | created_unix_ns u64
//   Chunk 64 KiB: hdr{ first_seq u32 | n_records u16 | blob_len u16
//                 | prev_hash u64 } | records[] | free | blobs[] | crc32c u32
//   Index       : every 1024 chunks -> { first_seq u32 | offset u64 | vtime u64 }
//   Footer 32 B : n_records u64 | n_chunks u32 | final_hash u64 | magic u32
//                 | reserved u64
//
// The header is 72 bytes: the PRD's field list does not fit in 64, and the
// fields are authoritative (conflict C-11). `created_unix_ns` is excluded from
// the run digest, so two runs of the same seed still agree.
//
// Per-kind field presence is a table, not a guess: the schema array below is
// the authoritative definition of what each record carries.

#ifndef PERMUTE_TRACE_H
#define PERMUTE_TRACE_H

#include <stdint.h>
#include <stdio.h>

#include "kernel/state.h"

#define PERMUTE_TRACE_VERSION       1
#define PERMUTE_TRACE_HEADER_SIZE   72u
#define PERMUTE_TRACE_CHUNK_SIZE    65536u
#define PERMUTE_TRACE_CHUNK_HDR     16u
#define PERMUTE_TRACE_FOOTER_SIZE   32u
#define PERMUTE_TRACE_INDEX_EVERY   1024u
#define PERMUTE_TRACE_MAX_INDEX     1024u
#define PERMUTE_KERNEL_MODEL_VERSION 1u

// Record kinds (22). The order is the wire value.
enum permute_trace_kind {
    PERMUTE_TR_CHANNEL_CREATE = 0,
    PERMUTE_TR_CONNECT_ATTACH,
    PERMUTE_TR_CONNECT_DETACH,
    PERMUTE_TR_SEND,
    PERMUTE_TR_RECV,
    PERMUTE_TR_REPLY,
    PERMUTE_TR_ERROR,
    PERMUTE_TR_PULSE_SEND,
    PERMUTE_TR_PULSE_RECV,
    PERMUTE_TR_EVENT_DELIVER,
    PERMUTE_TR_TIMER_ARM,
    PERMUTE_TR_TIMER_FIRE,
    PERMUTE_TR_SCHED_PICK,
    PERMUTE_TR_BLOCK,
    PERMUTE_TR_UNBLOCK,
    PERMUTE_TR_SYNC,
    PERMUTE_TR_THREAD_START,
    PERMUTE_TR_THREAD_EXIT,
    PERMUTE_TR_FAULT_INJECT,
    PERMUTE_TR_ORACLE,
    PERMUTE_TR_STIMULUS,
    PERMUTE_TR_CRASH,
    PERMUTE_TR_KIND_COUNT
};

// Record flags (3 bits after the kind in the first byte).
#define PERMUTE_TRACE_F_BLOB  0x1u
#define PERMUTE_TRACE_F_EVENT 0x2u

// Optional field presence, per kind.
#define PERMUTE_TRACE_S_DST 0x1u
#define PERMUTE_TRACE_S_OBJ 0x2u
#define PERMUTE_TRACE_S_AUX 0x4u

extern const uint8_t permute_trace_schema[PERMUTE_TR_KIND_COUNT];
const char *permute_trace_kind_name(uint8_t kind);

typedef struct {
    uint32_t first_seq;
    uint64_t offset;
    uint64_t vtime;
} permute_trace_index_t;

typedef struct {
    FILE *f;
    uint8_t chunk[PERMUTE_TRACE_CHUNK_SIZE];

    uint32_t record_len;   // bytes of records, starting at CHUNK_HDR
    uint16_t n_records;
    uint16_t blob_len;     // bytes of blobs, growing down from the CRC
    uint32_t first_seq;

    uint64_t prev_hash;
    uint64_t last_seq;
    uint64_t last_time;

    uint64_t n_records_total;
    uint32_t n_chunks;
    uint64_t config_hash;
    uint8_t build_id[32];
    uint8_t run_id[16];
    uint64_t created_unix_ns;
    uint64_t final_hash;

    permute_trace_index_t index[PERMUTE_TRACE_MAX_INDEX];
    uint32_t n_index;

    int failed;
} permute_trace_t;

int permute_trace_open(permute_trace_t *t, const char *path,
                       uint64_t config_hash, const uint8_t build_id[32],
                       const uint8_t run_id[16], uint64_t created_unix_ns);

// Emit one record. `blob` may be NULL when blob_len is 0; `event` may be NULL.
int permute_trace_emit(permute_trace_t *t, uint8_t kind, uint64_t seq,
                       uint64_t time_ns, uint16_t src, uint16_t dst,
                       uint32_t obj, uint32_t aux, const void *blob,
                       uint16_t blob_len, const permute_sigevent_t *event);

// Flush, write the index and footer, and return the run digest.
int permute_trace_close(permute_trace_t *t, uint64_t *run_digest);

// Exposed for the truncation tests and the Python cross-check.
uint32_t permute_crc32c(const void *data, size_t len);

#endif // PERMUTE_TRACE_H
