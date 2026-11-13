# SPDX-License-Identifier: Apache-2.0
"""Portable BLAKE3, matching kernel/blake3.c.

Only the unkeyed hash and a 32-byte output are implemented. The reader uses it
to recompute the chunk chain and the run digest, so a trace written by a
different build is checked rather than trusted.
"""

_IV = (
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
    0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
)

_MSG_PERMUTATION = (2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8)

_CHUNK_START = 1
_CHUNK_END = 2
_PARENT = 4
_ROOT = 8

_M = 0xFFFFFFFF
_BLOCK_LEN = 64
_CHUNK_LEN = 1024


def _rotr(x, n):
    return ((x >> n) | (x << (32 - n))) & _M


def _g(v, a, b, c, d, mx, my):
    v[a] = (v[a] + v[b] + mx) & _M
    v[d] = _rotr(v[d] ^ v[a], 16)
    v[c] = (v[c] + v[d]) & _M
    v[b] = _rotr(v[b] ^ v[c], 12)
    v[a] = (v[a] + v[b] + my) & _M
    v[d] = _rotr(v[d] ^ v[a], 8)
    v[c] = (v[c] + v[d]) & _M
    v[b] = _rotr(v[b] ^ v[c], 7)


def _round(v, m):
    _g(v, 0, 4, 8, 12, m[0], m[1])
    _g(v, 1, 5, 9, 13, m[2], m[3])
    _g(v, 2, 6, 10, 14, m[4], m[5])
    _g(v, 3, 7, 11, 15, m[6], m[7])
    _g(v, 0, 5, 10, 15, m[8], m[9])
    _g(v, 1, 6, 11, 12, m[10], m[11])
    _g(v, 2, 7, 8, 13, m[12], m[13])
    _g(v, 3, 4, 9, 14, m[14], m[15])


def _permute(m):
    return [m[i] for i in _MSG_PERMUTATION]


def _compress(cv, block, counter, block_len, flags):
    v = list(cv) + list(_IV[:4]) + [
        counter & _M, (counter >> 32) & _M, block_len, flags,
    ]
    m = list(block)
    for _ in range(7):
        _round(v, m)
        m = _permute(m)
    out = [(v[i] ^ v[i + 8]) & _M for i in range(8)]
    out += [(v[i + 8] ^ cv[i]) & _M for i in range(8)]
    return out


def _words(block):
    return [
        int.from_bytes(block[4 * i:4 * i + 4], "little") for i in range(16)
    ]


class _Chunk:
    __slots__ = ("cv", "counter", "block", "block_len", "blocks_compressed",
                 "flags")

    def __init__(self, key, counter, flags):
        self.cv = list(key)
        self.counter = counter
        self.block = bytearray(_BLOCK_LEN)
        self.block_len = 0
        self.blocks_compressed = 0
        self.flags = flags

    def length(self):
        return self.blocks_compressed * _BLOCK_LEN + self.block_len

    def update(self, data):
        pos = 0
        while pos < len(data):
            if self.block_len == _BLOCK_LEN:
                block_flags = self.flags | (
                    _CHUNK_START if self.blocks_compressed == 0 else 0
                )
                out = _compress(self.cv, _words(self.block), self.counter,
                                _BLOCK_LEN, block_flags)
                self.cv = out[:8]
                self.blocks_compressed += 1
                self.block_len = 0
                self.block = bytearray(_BLOCK_LEN)
            take = min(_BLOCK_LEN - self.block_len, len(data) - pos)
            self.block[self.block_len:self.block_len + take] = data[pos:pos + take]
            self.block_len += take
            pos += take

    def output(self):
        flags = self.flags | (
            _CHUNK_START if self.blocks_compressed == 0 else 0
        ) | _CHUNK_END
        return (list(self.cv), _words(self.block), self.counter, self.block_len,
                flags)

    def chaining_value(self):
        cv, block, counter, block_len, flags = self.output()
        return _compress(cv, block, counter, block_len, flags)[:8]


def _parent_cv(left, right):
    out = _compress(list(_IV), list(left) + list(right), 0, _BLOCK_LEN,
                    _PARENT)
    return out[:8]


class _Hasher:
    def __init__(self):
        self.chunk = _Chunk(_IV, 0, 0)
        self.stack = []
        self.chunks_compressed = 0

    def update(self, data):
        pos = 0
        while pos < len(data):
            if self.chunk.length() == _CHUNK_LEN:
                cv = self.chunk.chaining_value()
                self.chunks_compressed += 1
                self._add_chunk_cv(cv, self.chunks_compressed)
                self.chunk = _Chunk(_IV, self.chunks_compressed, 0)
            want = _CHUNK_LEN - self.chunk.length()
            take = min(want, len(data) - pos)
            self.chunk.update(data[pos:pos + take])
            pos += take

    def _add_chunk_cv(self, new_cv, total_chunks):
        cv = list(new_cv)
        tc = total_chunks
        while (tc & 1) == 0:
            cv = _parent_cv(self.stack.pop(), cv)
            tc >>= 1
        self.stack.append(cv)

    def finalize(self):
        cv, block, counter, block_len, flags = self.chunk.output()
        node = (cv, block, counter, block_len, flags)
        for left in reversed(self.stack):
            right = _compress(*node)[:8]
            node = (list(_IV), list(left) + list(right), 0, _BLOCK_LEN,
                    _PARENT)
        cv, block, counter, block_len, flags = node
        out = _compress(cv, block, counter, block_len, flags | _ROOT)
        return b"".join(w.to_bytes(4, "little") for w in out[:8])


def hash32(data):
    h = _Hasher()
    h.update(data)
    return h.finalize()


def hash64(data):
    return int.from_bytes(hash32(data)[:8], "little")


if __name__ == "__main__":
    # Self-check against one official vector: the empty input.
    assert hash32(b"").hex() == (
        "af1349b9f5f9a1a6a0404dea36dcc949"
        "9bcb25c9adc112b7cc9a93cae41f3262"
    ), hash32(b"").hex()
    print("blake3 self-check ok")
