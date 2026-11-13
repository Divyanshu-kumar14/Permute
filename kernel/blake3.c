// SPDX-License-Identifier: Apache-2.0

#include "kernel/blake3.h"

#include <string.h>

#define B3_BLOCK_LEN 64u
#define B3_CHUNK_LEN 1024u
#define B3_MAX_DEPTH 54u

#define B3_CHUNK_START 1u
#define B3_CHUNK_END   2u
#define B3_PARENT      4u
#define B3_ROOT        8u

static const uint32_t B3_IV[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

static const uint8_t B3_MSG_PERMUTATION[16] = {
    2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8,
};

static inline uint32_t rotr32(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static inline void g(uint32_t *s, int a, int b, int c, int d, uint32_t mx,
                     uint32_t my)
{
    s[a] = s[a] + s[b] + mx;
    s[d] = rotr32(s[d] ^ s[a], 16);
    s[c] = s[c] + s[d];
    s[b] = rotr32(s[b] ^ s[c], 12);
    s[a] = s[a] + s[b] + my;
    s[d] = rotr32(s[d] ^ s[a], 8);
    s[c] = s[c] + s[d];
    s[b] = rotr32(s[b] ^ s[c], 7);
}

static void round_fn(uint32_t v[16], const uint32_t m[16])
{
    g(v, 0, 4, 8, 12, m[0], m[1]);
    g(v, 1, 5, 9, 13, m[2], m[3]);
    g(v, 2, 6, 10, 14, m[4], m[5]);
    g(v, 3, 7, 11, 15, m[6], m[7]);
    g(v, 0, 5, 10, 15, m[8], m[9]);
    g(v, 1, 6, 11, 12, m[10], m[11]);
    g(v, 2, 7, 8, 13, m[12], m[13]);
    g(v, 3, 4, 9, 14, m[14], m[15]);
}

static void permute_msg(uint32_t m[16])
{
    uint32_t tmp[16];
    for (int i = 0; i < 16; i++)
        tmp[i] = m[B3_MSG_PERMUTATION[i]];
    memcpy(m, tmp, sizeof tmp);
}

static void compress(const uint32_t cv[8], const uint32_t block[16],
                     uint64_t counter, uint32_t block_len, uint32_t flags,
                     uint32_t out[16])
{
    uint32_t v[16];
    uint32_t m[16];

    for (int i = 0; i < 8; i++)
        v[i] = cv[i];
    for (int i = 0; i < 4; i++)
        v[8 + i] = B3_IV[i];
    v[12] = (uint32_t)counter;
    v[13] = (uint32_t)(counter >> 32);
    v[14] = block_len;
    v[15] = flags;

    memcpy(m, block, sizeof m);

    for (int r = 0; r < 7; r++) {
        round_fn(v, m);
        permute_msg(m);
    }

    for (int i = 0; i < 8; i++) {
        out[i] = v[i] ^ v[i + 8];
        out[i + 8] = v[i + 8] ^ cv[i];
    }
}

// ---------------------------------------------------------------------------
// Output node: either a chunk's final block or a parent node. Keeping it as a
// value is what lets the tree code treat both cases the same way.

typedef struct {
    uint32_t input_cv[8];
    uint32_t block[16];
    uint64_t counter;
    uint32_t block_len;
    uint32_t flags;
} output_t;

static void output_chaining_value(const output_t *o, uint32_t cv[8])
{
    uint32_t out[16];
    compress(o->input_cv, o->block, o->counter, o->block_len, o->flags, out);
    memcpy(cv, out, 8 * sizeof(uint32_t));
}

static void output_root_bytes(const output_t *o,
                              uint8_t out[PERMUTE_BLAKE3_OUT_LEN])
{
    uint32_t words[16];
    compress(o->input_cv, o->block, 0, o->block_len, o->flags | B3_ROOT, words);

    for (int i = 0; i < 8; i++) {
        out[4 * i + 0] = (uint8_t)(words[i] >> 0);
        out[4 * i + 1] = (uint8_t)(words[i] >> 8);
        out[4 * i + 2] = (uint8_t)(words[i] >> 16);
        out[4 * i + 3] = (uint8_t)(words[i] >> 24);
    }
}

static void words_from_block(const uint8_t block[B3_BLOCK_LEN], uint32_t out[16])
{
    for (int i = 0; i < 16; i++) {
        out[i] = (uint32_t)block[4 * i] | ((uint32_t)block[4 * i + 1] << 8) |
                 ((uint32_t)block[4 * i + 2] << 16) |
                 ((uint32_t)block[4 * i + 3] << 24);
    }
}

// ---------------------------------------------------------------------------
// Chunk state

typedef struct {
    uint32_t cv[8];
    uint64_t counter;
    uint8_t block[B3_BLOCK_LEN];
    uint8_t block_len;
    uint8_t blocks_compressed;
    uint32_t flags;
} chunk_state;

static void chunk_init(chunk_state *c, const uint32_t key[8], uint64_t counter,
                       uint32_t flags)
{
    memcpy(c->cv, key, sizeof c->cv);
    c->counter = counter;
    memset(c->block, 0, sizeof c->block);
    c->block_len = 0;
    c->blocks_compressed = 0;
    c->flags = flags;
}

static size_t chunk_len(const chunk_state *c)
{
    return (size_t)c->blocks_compressed * B3_BLOCK_LEN + c->block_len;
}

static void chunk_update(chunk_state *c, const uint8_t *input, size_t len)
{
    while (len > 0) {
        if (c->block_len == B3_BLOCK_LEN) {
            uint32_t block_words[16];
            uint32_t out[16];
            uint32_t block_flags =
                c->flags | (c->blocks_compressed == 0 ? B3_CHUNK_START : 0u);

            words_from_block(c->block, block_words);
            compress(c->cv, block_words, c->counter, B3_BLOCK_LEN, block_flags,
                     out);
            memcpy(c->cv, out, 8 * sizeof(uint32_t));
            c->blocks_compressed++;
            c->block_len = 0;
            memset(c->block, 0, sizeof c->block);
        }

        size_t take = B3_BLOCK_LEN - c->block_len;
        if (take > len)
            take = len;
        memcpy(c->block + c->block_len, input, take);
        c->block_len += (uint8_t)take;
        input += take;
        len -= take;
    }
}

static output_t chunk_output(const chunk_state *c)
{
    output_t o;
    memcpy(o.input_cv, c->cv, sizeof o.input_cv);
    memset(o.block, 0, sizeof o.block);
    words_from_block(c->block, o.block);
    o.counter = c->counter;
    o.block_len = c->block_len;
    o.flags = c->flags | (c->blocks_compressed == 0 ? B3_CHUNK_START : 0u) |
              B3_CHUNK_END;
    return o;
}

// ---------------------------------------------------------------------------
// Parent nodes and the hasher

static void parent_cv(const uint32_t left[8], const uint32_t right[8],
                      uint32_t out[8], const uint32_t key[8], uint32_t flags)
{
    output_t o;
    memcpy(o.input_cv, key, sizeof o.input_cv);
    memcpy(&o.block[0], left, 8 * sizeof(uint32_t));
    memcpy(&o.block[8], right, 8 * sizeof(uint32_t));
    o.counter = 0;
    o.block_len = B3_BLOCK_LEN;
    o.flags = flags | B3_PARENT;
    output_chaining_value(&o, out);
}

typedef struct {
    chunk_state chunk;
    uint32_t cv_stack[B3_MAX_DEPTH][8];
    uint8_t stack_len;
    uint32_t key[8];
    uint32_t flags;
    uint64_t chunks_compressed;
} hasher;

static void hasher_init(hasher *h)
{
    memset(h, 0, sizeof *h);
    memcpy(h->key, B3_IV, sizeof h->key);
    chunk_init(&h->chunk, h->key, 0, 0);
}

static void hasher_add_chunk_cv(hasher *h, const uint32_t new_cv[8],
                                uint64_t total_chunks)
{
    uint32_t cv[8];
    memcpy(cv, new_cv, sizeof cv);

    uint64_t tc = total_chunks;
    while ((tc & 1u) == 0) {
        uint32_t merged[8];
        h->stack_len--;
        parent_cv(h->cv_stack[h->stack_len], cv, merged, h->key, h->flags);
        memcpy(cv, merged, sizeof cv);
        tc >>= 1;
    }

    memcpy(h->cv_stack[h->stack_len], cv, sizeof cv);
    h->stack_len++;
}

static void hasher_update(hasher *h, const uint8_t *input, size_t len)
{
    while (len > 0) {
        if (chunk_len(&h->chunk) == B3_CHUNK_LEN) {
            output_t o = chunk_output(&h->chunk);
            uint32_t cv[8];
            output_chaining_value(&o, cv);
            h->chunks_compressed++;
            hasher_add_chunk_cv(h, cv, h->chunks_compressed);
            chunk_init(&h->chunk, h->key, h->chunks_compressed, h->flags);
        }

        size_t want = B3_CHUNK_LEN - chunk_len(&h->chunk);
        size_t take = want < len ? want : len;
        chunk_update(&h->chunk, input, take);
        input += take;
        len -= take;
    }
}

static void hasher_finalize(const hasher *h,
                            uint8_t out[PERMUTE_BLAKE3_OUT_LEN])
{
    output_t o = chunk_output(&h->chunk);

    // Merge the stack from the top. The last merge becomes the root, and a
    // single-chunk input has no merge at all.
    for (int i = h->stack_len - 1; i >= 0; i--) {
        uint32_t right[8];
        output_chaining_value(&o, right);

        output_t parent;
        memcpy(parent.input_cv, h->key, sizeof parent.input_cv);
        memcpy(&parent.block[0], h->cv_stack[i], 8 * sizeof(uint32_t));
        memcpy(&parent.block[8], right, 8 * sizeof(uint32_t));
        parent.counter = 0;
        parent.block_len = B3_BLOCK_LEN;
        parent.flags = h->flags | B3_PARENT;
        o = parent;
    }

    output_root_bytes(&o, out);
}

void permute_blake3(const void *data, size_t len,
                    uint8_t out[PERMUTE_BLAKE3_OUT_LEN])
{
    hasher h;
    hasher_init(&h);
    if (len > 0)
        hasher_update(&h, (const uint8_t *)data, len);
    hasher_finalize(&h, out);
}

uint64_t permute_blake3_64(const void *data, size_t len)
{
    uint8_t out[PERMUTE_BLAKE3_OUT_LEN];
    permute_blake3(data, len, out);

    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)out[i] << (8 * i);
    return v;
}

uint64_t permute_blake3_64_pair(const void *a, size_t alen, const void *b,
                                size_t blen)
{
    hasher h;
    uint8_t out[PERMUTE_BLAKE3_OUT_LEN];

    hasher_init(&h);
    if (alen > 0)
        hasher_update(&h, (const uint8_t *)a, alen);
    if (blen > 0)
        hasher_update(&h, (const uint8_t *)b, blen);
    hasher_finalize(&h, out);

    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)out[i] << (8 * i);
    return v;
}
