# SPDX-License-Identifier: Apache-2.0
"""Reader and exporters for the .pmt trace format.

The reader never raises on a damaged file. A truncated trace is a normal
outcome of a crashed run, so it returns the records that survived, sets
`partial`, and lets the caller decide. A CRC or hash mismatch sets `corrupt`
or `chain_ok = False` and stops at the first bad chunk rather than decoding
garbage.
"""

from __future__ import annotations

import json
from pathlib import Path

from . import blake3

HEADER_SIZE = 72
CHUNK_SIZE = 65536
CHUNK_HDR = 16
FOOTER_SIZE = 32
INDEX_EVERY = 1024
INDEX_ENTRY_SIZE = 20
MODEL_VERSION = 1

S_DST = 0x1
S_OBJ = 0x2
S_AUX = 0x4

F_BLOB = 0x1
F_EVENT = 0x2

KINDS = (
    "CHANNEL_CREATE", "CONNECT_ATTACH", "CONNECT_DETACH", "SEND",
    "RECV", "REPLY", "ERROR", "PULSE_SEND",
    "PULSE_RECV", "EVENT_DELIVER", "TIMER_ARM", "TIMER_FIRE",
    "SCHED_PICK", "BLOCK", "UNBLOCK", "SYNC",
    "THREAD_START", "THREAD_EXIT", "FAULT_INJECT", "ORACLE",
    "STIMULUS", "CRASH",
)

# Optional-field presence per kind. This table is the schema; the prose in the
# requirements document is approximate.
SCHEMA = (
    S_OBJ | S_AUX,                     # CHANNEL_CREATE
    S_DST | S_OBJ | S_AUX,             # CONNECT_ATTACH
    S_DST | S_OBJ,                     # CONNECT_DETACH
    S_DST | S_OBJ | S_AUX,             # SEND
    S_DST | S_OBJ | S_AUX,             # RECV
    S_DST | S_OBJ | S_AUX,             # REPLY
    S_DST | S_OBJ | S_AUX,             # ERROR
    S_DST | S_OBJ | S_AUX,             # PULSE_SEND
    S_DST | S_OBJ | S_AUX,             # PULSE_RECV
    S_DST | S_OBJ,                     # EVENT_DELIVER
    S_OBJ | S_AUX,                     # TIMER_ARM
    S_OBJ | S_AUX,                     # TIMER_FIRE
    S_OBJ | S_AUX,                     # SCHED_PICK
    S_OBJ | S_AUX,                     # BLOCK
    S_OBJ | S_AUX,                     # UNBLOCK
    S_OBJ | S_AUX,                     # SYNC
    S_OBJ | S_AUX,                     # THREAD_START
    S_OBJ | S_AUX,                     # THREAD_EXIT
    S_OBJ | S_AUX,                     # FAULT_INJECT
    S_OBJ | S_AUX,                     # ORACLE
    S_DST | S_OBJ | S_AUX,             # STIMULUS
    S_OBJ | S_AUX,                     # CRASH
)


def _crc_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (0x82F63B78 ^ (c >> 1)) if (c & 1) else (c >> 1)
        table.append(c)
    return tuple(table)


_CRC_TABLE = _crc_table()


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc = _CRC_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def _u16(buf, off):
    return int.from_bytes(buf[off:off + 2], "little")


def _u32(buf, off):
    return int.from_bytes(buf[off:off + 4], "little")


def _u64(buf, off):
    return int.from_bytes(buf[off:off + 8], "little")


def _varint(buf, pos, end):
    value = 0
    shift = 0
    while pos < end:
        b = buf[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        if not (b & 0x80):
            return value, pos
        shift += 7
        if shift > 63:
            break
    raise ValueError("truncated varint")


class Record(dict):
    """One decoded event. Missing optional fields are present with None."""


class Trace:
    def __init__(self, path):
        self.path = str(path)
        self.version = 0
        self.flags = 0
        self.run_id = b""
        self.config_hash = 0
        self.build_id = b""
        self.created_unix_ns = 0
        self.records: list[Record] = []
        self.n_chunks = 0
        self.final_hash = None
        self.kernel_model_version = None
        self.run_digest = None
        self.partial = False
        self.corrupt = False
        self.chain_ok = True
        self.error = None

    @property
    def ok(self):
        return self.error is None and not self.corrupt and self.chain_ok


class _State:
    __slots__ = ("seq", "time", "hash")

    def __init__(self):
        self.seq = 0
        self.time = 0
        self.hash = 0


def _decode_records(chunk, n_records, blob_len, state, tr, partial):
    """Decode records from one chunk. Returns False if the data ran out."""
    pos = CHUNK_HDR
    end = len(chunk) if partial else CHUNK_SIZE - 4 - blob_len
    blob_limit = len(chunk) if partial else CHUNK_SIZE - 4

    for _ in range(n_records):
        if pos >= end:
            return False

        kind_flags = chunk[pos]
        pos += 1
        kind = kind_flags >> 3
        flags = kind_flags & 0x7
        if kind >= len(KINDS):
            tr.corrupt = True
            tr.error = f"unknown record kind {kind}"
            return False

        try:
            seq, pos = _varint(chunk, pos, end)
            dt, pos = _varint(chunk, pos, end)
            src, pos = _varint(chunk, pos, end)
        except ValueError:
            return False

        state.seq += seq
        state.time += dt

        schema = SCHEMA[kind]
        dst = obj = aux = None
        blob = event = None

        try:
            if schema & S_DST:
                dst, pos = _varint(chunk, pos, end)
            if schema & S_OBJ:
                obj, pos = _varint(chunk, pos, end)
            if schema & S_AUX:
                aux, pos = _varint(chunk, pos, end)
            if flags & F_BLOB:
                blen, pos = _varint(chunk, pos, end)
                boff, pos = _varint(chunk, pos, end)
                if boff + blen <= blob_limit:
                    blob = bytes(chunk[boff:boff + blen])
            if flags & F_EVENT:
                if pos + 32 > len(chunk):
                    return False
                event = bytes(chunk[pos:pos + 32])
                pos += 32
        except ValueError:
            return False

        tr.records.append(Record(
            kind=KINDS[kind], seq=state.seq, t_ns=state.time, src=src,
            dst=dst, obj=obj, aux=aux, blob=blob, event=event,
        ))

    return True


def _footer(data):
    """Return (complete, n_chunks, index_size)."""
    if len(data) < FOOTER_SIZE:
        return False, 0, 0
    foot = data[-FOOTER_SIZE:]
    if foot[20:24] != b"PMT1":
        return False, 0, 0

    n_chunks = _u32(foot, 8)
    index_size = ((n_chunks + INDEX_EVERY - 1) // INDEX_EVERY) * INDEX_ENTRY_SIZE
    expected = HEADER_SIZE + n_chunks * CHUNK_SIZE + index_size + FOOTER_SIZE
    if expected != len(data):
        return False, n_chunks, index_size
    return True, n_chunks, index_size


def read(path) -> Trace:
    data = Path(path).read_bytes()
    tr = Trace(path)

    if len(data) < HEADER_SIZE:
        tr.partial = True
        tr.error = "shorter than a header"
        return tr
    if data[:4] != b"PMT1":
        tr.error = "bad magic"
        return tr

    tr.version = _u16(data, 4)
    tr.flags = _u16(data, 6)
    tr.run_id = bytes(data[8:24])
    tr.config_hash = _u64(data, 24)
    tr.build_id = bytes(data[32:64])
    tr.created_unix_ns = _u64(data, 64)

    complete, n_chunks, index_size = _footer(data)
    tr.n_chunks = n_chunks
    if not complete:
        tr.partial = True
        index_size = 0

    end = len(data) - (FOOTER_SIZE + index_size) if complete else len(data)
    state = _State()
    offset = HEADER_SIZE
    chunk_i = 0

    while offset < end:
        remaining = end - offset

        if remaining < CHUNK_SIZE:
            tr.partial = True
            if remaining < CHUNK_HDR:
                break

            # Try to read a truncated chunk header. Trailing index/footer
            # bytes can look like one, so validate before decoding.
            head = data[offset:offset + CHUNK_HDR]
            n_rec = _u16(head, 4)
            blob_len = _u16(head, 6)
            if n_rec > (remaining - CHUNK_HDR) // 2 + 1 or blob_len > remaining:
                break
            _decode_records(data[offset:offset + remaining], n_rec, blob_len,
                            state, tr, partial=True)
            break

        chunk = data[offset:offset + CHUNK_SIZE]
        stored_crc = _u32(chunk, CHUNK_SIZE - 4)
        if crc32c(chunk[:CHUNK_SIZE - 4]) != stored_crc:
            tr.corrupt = True
            tr.error = f"crc mismatch in chunk {chunk_i}"
            break

        first_seq = _u32(chunk, 0)
        n_rec = _u16(chunk, 4)
        blob_len = _u16(chunk, 6)
        prev_hash = _u64(chunk, 8)

        if prev_hash != state.hash:
            tr.chain_ok = False
            tr.error = f"hash chain broken at chunk {chunk_i}"
            break

        state.hash = blake3.hash64(
            prev_hash.to_bytes(8, "little") + chunk[:CHUNK_SIZE - 4]
        )
        del first_seq  # seeking metadata, not needed for a linear read

        if not _decode_records(chunk, n_rec, blob_len, state, tr, partial=False):
            tr.corrupt = True
            tr.error = f"record decode ran past chunk {chunk_i}"
            break

        offset += CHUNK_SIZE
        chunk_i += 1

    if complete:
        foot = data[-FOOTER_SIZE:]
        tr.final_hash = _u64(foot, 12)
        tr.kernel_model_version = _u64(foot, 24)
        if tr.final_hash != state.hash:
            tr.chain_ok = False
            tr.error = "final hash does not match the chain"
        tr.run_digest = _run_digest(tr)

    return tr


def _run_digest(tr: Trace):
    if tr.final_hash is None:
        return None
    payload = (
        tr.final_hash.to_bytes(8, "little")
        + tr.config_hash.to_bytes(8, "little")
        + tr.build_id
        + MODEL_VERSION.to_bytes(4, "little")
    )
    return blake3.hash64(payload)


def to_ndjson(tr: Trace) -> str:
    lines = []
    for rec in tr.records:
        out = dict(rec)
        out["blob"] = rec["blob"].hex() if rec["blob"] is not None else None
        out["event"] = rec["event"].hex() if rec["event"] is not None else None
        lines.append(json.dumps(out, sort_keys=True))
    return "\n".join(lines) + ("\n" if lines else "")


def to_chrome(tr: Trace) -> dict:
    events = []
    for rec in tr.records:
        args = {k: rec[k] for k in ("seq", "obj", "aux", "dst")
                if rec.get(k) is not None}
        if rec["blob"] is not None:
            args["blob_len"] = len(rec["blob"])
        events.append({
            "name": rec["kind"],
            "cat": "permute",
            "ph": "X",
            "ts": rec["t_ns"] / 1000.0,
            "dur": 0.0,
            "pid": 1,
            "tid": rec["src"] or 0,
            "args": args,
        })
    return {"traceEvents": events, "displayTimeUnit": "ms",
            "otherData": {"run_digest": f"{tr.run_digest:016x}"
                          if tr.run_digest is not None else None}}
