// SPDX-License-Identifier: Apache-2.0

#include "kernel/trace.h"

#include <string.h>

#include "kernel/blake3.h"

const uint8_t permute_trace_schema[PERMUTE_TR_KIND_COUNT] = {
    /* CHANNEL_CREATE  */ PERMUTE_TRACE_S_OBJ | PERMUTE_TRACE_S_AUX,
    /* CONNECT_ATTACH  */ PERMUTE_TRACE_S_DST | PERMUTE_TRACE_S_OBJ |
                          PERMUTE_TRACE_S_AUX,
    /* CONNECT_DETACH  */ PERMUTE_TRACE_S_DST | PERMUTE_TRACE_S_OBJ,
    /* SEND            */ PERMUTE_TRACE_S_DST | PERMUTE_TRACE_S_OBJ |
                          PERMUTE_TRACE_S_AUX,
    /* RECV            */ PERMUTE_TRACE_S_DST | PERMUTE_TRACE_S_OBJ |
                          PERMUTE_TRACE_S_AUX,
    /* REPLY           */ PERMUTE_TRACE_S_DST | PERMUTE_TRACE_S_OBJ |
                          PERMUTE_TRACE_S_AUX,
    /* ERROR           */ PERMUTE_TRACE_S_DST | PERMUTE_TRACE_S_OBJ |
                          PERMUTE_TRACE_S_AUX,
    /* PULSE_SEND      */ PERMUTE_TRACE_S_DST | PERMUTE_TRACE_S_OBJ |
                          PERMUTE_TRACE_S_AUX,
    /* PULSE_RECV      */ PERMUTE_TRACE_S_DST | PERMUTE_TRACE_S_OBJ |
                          PERMUTE_TRACE_S_AUX,
    /* EVENT_DELIVER   */ PERMUTE_TRACE_S_DST | PERMUTE_TRACE_S_OBJ,
    /* TIMER_ARM       */ PERMUTE_TRACE_S_OBJ | PERMUTE_TRACE_S_AUX,
    /* TIMER_FIRE      */ PERMUTE_TRACE_S_OBJ | PERMUTE_TRACE_S_AUX,
    /* SCHED_PICK      */ PERMUTE_TRACE_S_OBJ | PERMUTE_TRACE_S_AUX,
    /* BLOCK           */ PERMUTE_TRACE_S_OBJ | PERMUTE_TRACE_S_AUX,
    /* UNBLOCK         */ PERMUTE_TRACE_S_OBJ | PERMUTE_TRACE_S_AUX,
    /* SYNC            */ PERMUTE_TRACE_S_OBJ | PERMUTE_TRACE_S_AUX,
    /* THREAD_START    */ PERMUTE_TRACE_S_OBJ | PERMUTE_TRACE_S_AUX,
    /* THREAD_EXIT     */ PERMUTE_TRACE_S_OBJ | PERMUTE_TRACE_S_AUX,
    /* FAULT_INJECT    */ PERMUTE_TRACE_S_OBJ | PERMUTE_TRACE_S_AUX,
    /* ORACLE          */ PERMUTE_TRACE_S_OBJ | PERMUTE_TRACE_S_AUX,
    /* STIMULUS        */ PERMUTE_TRACE_S_DST | PERMUTE_TRACE_S_OBJ |
                          PERMUTE_TRACE_S_AUX,
    /* CRASH           */ PERMUTE_TRACE_S_OBJ | PERMUTE_TRACE_S_AUX,
};

static const char *const kind_names[PERMUTE_TR_KIND_COUNT] = {
    "CHANNEL_CREATE", "CONNECT_ATTACH", "CONNECT_DETACH", "SEND",
    "RECV",           "REPLY",          "ERROR",          "PULSE_SEND",
    "PULSE_RECV",     "EVENT_DELIVER",  "TIMER_ARM",      "TIMER_FIRE",
    "SCHED_PICK",     "BLOCK",          "UNBLOCK",        "SYNC",
    "THREAD_START",   "THREAD_EXIT",    "FAULT_INJECT",   "ORACLE",
    "STIMULUS",       "CRASH",
};

const char *permute_trace_kind_name(uint8_t kind)
{
    return kind < PERMUTE_TR_KIND_COUNT ? kind_names[kind] : "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Byte helpers

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t put_varint(uint8_t *p, uint64_t v)
{
    uint32_t n = 0;
    while (v >= 0x80) {
        p[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    p[n++] = (uint8_t)v;
    return n;
}

// ---------------------------------------------------------------------------
// CRC32C (Castagnoli), reflected, polynomial 0x82F63B78

uint32_t permute_crc32c(const void *data, size_t len)
{
    static uint32_t table[256];
    static int ready;

    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = 1;
    }

    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Writer

int permute_trace_open(permute_trace_t *t, const char *path,
                       uint64_t config_hash, const uint8_t build_id[32],
                       const uint8_t run_id[16], uint64_t created_unix_ns)
{
    memset(t, 0, sizeof *t);
    t->f = fopen(path, "wb");
    if (t->f == NULL)
        return -1;

    t->config_hash = config_hash;
    if (build_id != NULL)
        memcpy(t->build_id, build_id, 32);
    if (run_id != NULL)
        memcpy(t->run_id, run_id, 16);
    t->created_unix_ns = created_unix_ns;
    t->blob_len = 0;

    uint8_t hdr[PERMUTE_TRACE_HEADER_SIZE];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "PMT1", 4);
    put_u16(hdr + 4, PERMUTE_TRACE_VERSION);
    put_u16(hdr + 6, 0); // flags
    memcpy(hdr + 8, t->run_id, 16);
    put_u64(hdr + 24, config_hash);
    memcpy(hdr + 32, t->build_id, 32);
    put_u64(hdr + 64, created_unix_ns);

    if (fwrite(hdr, 1, sizeof hdr, t->f) != sizeof hdr) {
        t->failed = 1;
        return -1;
    }
    return 0;
}

static int flush_chunk(permute_trace_t *t)
{
    if (t->n_records == 0)
        return 0;

    uint8_t *c = t->chunk;
    put_u32(c + 0, t->first_seq);
    put_u16(c + 4, t->n_records);
    put_u16(c + 6, t->blob_len);
    put_u64(c + 8, t->prev_hash);

    // Zero the gap between the records and the blob area so the bytes are
    // deterministic.
    uint32_t records_end = PERMUTE_TRACE_CHUNK_HDR + t->record_len;
    uint32_t blob_start = PERMUTE_TRACE_CHUNK_SIZE - 4 - t->blob_len;
    if (records_end < blob_start)
        memset(c + records_end, 0, blob_start - records_end);

    uint32_t crc = permute_crc32c(c, PERMUTE_TRACE_CHUNK_SIZE - 4);
    put_u32(c + PERMUTE_TRACE_CHUNK_SIZE - 4, crc);

    uint8_t prefix[8];
    put_u64(prefix, t->prev_hash);
    uint64_t hash = permute_blake3_64_pair(prefix, sizeof prefix, c,
                                           PERMUTE_TRACE_CHUNK_SIZE - 4);

    if (fwrite(c, 1, PERMUTE_TRACE_CHUNK_SIZE, t->f) !=
        PERMUTE_TRACE_CHUNK_SIZE) {
        t->failed = 1;
        return -1;
    }

    t->prev_hash = hash;
    t->final_hash = hash;
    t->n_chunks++;

    t->record_len = 0;
    t->n_records = 0;
    t->blob_len = 0;
    return 0;
}

int permute_trace_emit(permute_trace_t *t, uint8_t kind, uint64_t seq,
                       uint64_t time_ns, uint16_t src, uint16_t dst,
                       uint32_t obj, uint32_t aux, const void *blob,
                       uint16_t blob_len, const permute_sigevent_t *event)
{
    if (t->failed || kind >= PERMUTE_TR_KIND_COUNT)
        return -1;
    if (blob_len > 0 && blob == NULL)
        return -1;
    if (seq < t->last_seq || time_ns < t->last_time)
        return -1; // deltas are encoded unsigned; a reversal is a bug

    uint8_t tmp[128];
    uint32_t n = 0;

    uint8_t flags = 0;
    if (blob_len > 0)
        flags |= PERMUTE_TRACE_F_BLOB;
    if (event != NULL)
        flags |= PERMUTE_TRACE_F_EVENT;

    uint8_t schema = permute_trace_schema[kind];
    uint32_t blob_off = 0;
    if (blob_len > 0)
        blob_off = PERMUTE_TRACE_CHUNK_SIZE - 4 - t->blob_len - blob_len;

    // Make room first, so the sizes used below are final.
    uint32_t need = PERMUTE_TRACE_CHUNK_HDR + t->record_len + blob_len + 96 +
                    t->blob_len + 4;
    if (need > PERMUTE_TRACE_CHUNK_SIZE) {
        if (flush_chunk(t) != 0)
            return -1;
        if (blob_len > 0)
            blob_off = PERMUTE_TRACE_CHUNK_SIZE - 4 - blob_len;
    }

    tmp[n++] = (uint8_t)((kind << 3) | flags);
    n += put_varint(tmp + n, seq - t->last_seq);
    n += put_varint(tmp + n, time_ns - t->last_time);
    n += put_varint(tmp + n, src);
    if (schema & PERMUTE_TRACE_S_DST)
        n += put_varint(tmp + n, dst);
    if (schema & PERMUTE_TRACE_S_OBJ)
        n += put_varint(tmp + n, obj);
    if (schema & PERMUTE_TRACE_S_AUX)
        n += put_varint(tmp + n, aux);
    if (blob_len > 0) {
        n += put_varint(tmp + n, blob_len);
        n += put_varint(tmp + n, blob_off);
    }
    if (event != NULL) {
        memcpy(tmp + n, event, sizeof *event);
        n += sizeof *event;
    }

    if (t->n_records == 0) {
        t->first_seq = (uint32_t)seq;

        // Index every 1024th chunk at its start, which is the only place a
        // seek can land.
        if (t->n_chunks % PERMUTE_TRACE_INDEX_EVERY == 0 &&
            t->n_index < PERMUTE_TRACE_MAX_INDEX) {
            t->index[t->n_index].first_seq = (uint32_t)seq;
            t->index[t->n_index].offset =
                PERMUTE_TRACE_HEADER_SIZE +
                (uint64_t)t->n_chunks * PERMUTE_TRACE_CHUNK_SIZE;
            t->index[t->n_index].vtime = time_ns;
            t->n_index++;
        }
    }

    memcpy(t->chunk + PERMUTE_TRACE_CHUNK_HDR + t->record_len, tmp, n);
    t->record_len += n;

    if (blob_len > 0) {
        t->blob_len = (uint16_t)(t->blob_len + blob_len);
        memcpy(t->chunk + blob_off, blob, blob_len);
    }

    t->n_records++;
    t->n_records_total++;
    t->last_seq = seq;
    t->last_time = time_ns;
    return 0;
}

int permute_trace_close(permute_trace_t *t, uint64_t *run_digest)
{
    if (t->failed) {
        if (t->f != NULL)
            fclose(t->f);
        t->f = NULL;
        return -1;
    }

    if (flush_chunk(t) != 0) {
        fclose(t->f);
        t->f = NULL;
        return -1;
    }

    for (uint32_t i = 0; i < t->n_index; i++) {
        uint8_t e[20];
        put_u32(e + 0, t->index[i].first_seq);
        put_u64(e + 4, t->index[i].offset);
        put_u64(e + 12, t->index[i].vtime);
        if (fwrite(e, 1, sizeof e, t->f) != sizeof e) {
            fclose(t->f);
            t->f = NULL;
            return -1;
        }
    }

    uint8_t footer[PERMUTE_TRACE_FOOTER_SIZE];
    memset(footer, 0, sizeof footer);
    put_u64(footer + 0, t->n_records_total);
    put_u32(footer + 8, t->n_chunks);
    put_u64(footer + 12, t->final_hash);
    memcpy(footer + 20, "PMT1", 4);
    // The reserved word carries the kernel model version, so replay can refuse
    // a kernel mismatch with exit 4 instead of reporting a digest difference.
    put_u64(footer + 24, PERMUTE_KERNEL_MODEL_VERSION);

    if (fwrite(footer, 1, sizeof footer, t->f) != sizeof footer) {
        fclose(t->f);
        t->f = NULL;
        return -1;
    }

    if (run_digest != NULL) {
        uint8_t input[52];
        memset(input, 0, sizeof input);
        put_u64(input + 0, t->final_hash);
        put_u64(input + 8, t->config_hash);
        memcpy(input + 16, t->build_id, 32);
        put_u32(input + 48, PERMUTE_KERNEL_MODEL_VERSION);
        *run_digest = permute_blake3_64(input, sizeof input);
    }

    fclose(t->f);
    t->f = NULL;
    return 0;
}
