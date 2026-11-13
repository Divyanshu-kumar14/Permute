<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-07 report — trace codec

**Date:** 2026-11-14
**Scope:** `.pmt` writer and reader, CRC32C, BLAKE3-64 chain, `run_digest`, the 22 record kinds, NDJSON and Chrome export, truncation recovery.
**Result:** **547 cross-language checks, 0 failures**; BLAKE3 verified against 20 official vectors; `make test` green across all C suites and the codec test.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `kernel/blake3.h/.c` | Portable unkeyed BLAKE3, 32-byte output, 64-bit truncation helper |
| `kernel/trace.h/.c` | Header, chunk framing, CRC32C, hash chain, index, footer, run digest |
| `tools/trace_gen.c` | Golden-trace generator used by the cross-language test |
| `cli/permute/trace/blake3.py` | Same algorithm in Python, so the reader verifies rather than trusts |
| `cli/permute/trace/codec.py` | Reader, partial/corrupt handling, NDJSON and Chrome exporters |
| `tests/unit/test_blake3.c` | Official vectors |
| `tests/python/test_codec.py` | Round-trip, truncation, corruption, chain, exports |

---

## 2. Results

```
test_objects:   326 checks, 0 failure(s)
test_ipc:    120149 checks, 0 failure(s)
test_pulse:   21351 checks, 0 failure(s)
test_sched:     277 checks, 0 failure(s)
test_pi:        232 checks, 0 failure(s)
test_time:      191 checks, 0 failure(s)
test_blake3:     20 checks, 0 failure(s)
test_codec:     547 checks, 0 failure(s)
```

| Behaviour | Result |
|---|---|
| BLAKE3 vectors 0, 1, 63, 64, 65, 127, 128, 1023, 1024, 1025, 2048, 2049, 3072, 4096, 5120, 8192, 16384, 31744, 102400 | all match the official digests |
| One record of every kind, C write → Python read | every field equal, including blobs and inline events |
| `run_digest` | C and Python agree |
| 5,000-record bulk across 6 chunks | complete read, blobs intact |
| Average encoded size | **14.6 B/record** over a 64-byte payload (78.6 B total), budget 24 |
| Truncation at 9 offsets (inside the header, mid-chunk, chunk boundary, index, footer) | recovered records are a prefix, never a crash |
| Byte flip in a chunk | `corrupt`, earlier chunks' records returned |
| `prev_hash` mutation with a repaired CRC | `chain_ok == False`, named as a chain break |
| NDJSON / Chrome export | one line per record / one event per record |

---

## 3. The hash had to be real, not a placeholder

`run_digest` is a headline claim ("identical across machines for the same inputs"), and the reader has to be able to check a trace it did not write. So BLAKE3 is implemented twice — portable C for the writer, pure Python for the reader — and both are checked against the official vectors.

The vector set matters: single-block inputs (0–64 bytes) only exercise the compression function. 1025, 2048 and 102400 exercise the chunk tree, the stack merge, and the root flag. A single-block test would have passed with a broken tree.

The Python implementation was wrong on the first attempt in exactly that area: `finalize` omitted the `ROOT` flag on the last compression. The 1025-byte vector caught it — which is why the test uses that length and not just the empty input. IMP-25.

---

## 4. Format decisions

**Header is 72 bytes, not 64.** The prose says 64; its own field list is 4+2+2+16+8+32+8 = 72. The fields win (C-11). Dropping a field to fit the sentence would cost either the build id or the creation time.

**Field presence is a schema table.** The requirements describe which kinds carry `dst`/`obj`/`aux`/blob/event in prose. That is not precise enough to decode from, so `kernel/trace.h` carries a `schema[22]` array and the Python codec mirrors it. The table is the definition; the prose is a summary. This is the same resolution as C-07 (record kinds).

**`created_unix_ns` is in the header but not the digest.** It is a wall-clock value, so including it would make two runs of one seed disagree. Excluding it keeps the digest reproducible while preserving the field for human readers.

**The chain covers the chunk header.** `hash = BLAKE3-64(prev_hash ‖ chunk minus CRC)`. The header carries `prev_hash`, so covering it means the chain commits to the link as well as the payload. The CRC is checked first because it is cheaper and gives a clearer failure, and is not part of the hash input. IMP-26.

**Chunk `first_seq` is 32 bits.** That caps a single file at 2³² records. The PRD specifies the width; it is recorded here rather than silently widened.

---

## 5. Damage behaviour

The reader never raises. A truncated file is the expected outcome of a killed run, so it returns the records that survived and sets `partial`. Nine truncation points are tested: inside the header, at the header boundary, inside the records, one byte short of a full chunk, exactly at a chunk boundary, and through the index and footer.

Two among them are subtle: a file cut just before the footer leaves a complete chunk plus 20 bytes of index. Those 20 bytes parse as a plausible chunk header (`first_seq`, `n_records`, `blob_len`), so the reader validates `n_records` against the available bytes before decoding. Without that guard it would emit a handful of nonsense records from an index entry. The test covers it.

Blobs in a partially written chunk cannot be recovered: the blob area is anchored at the chunk's far end, which the truncation removed. Those records survive with `blob = None` (divergence #15). A complete chunk always recovers its blobs.

---

## 6. Boundary

| In scope | Out of scope |
|---|---|
| Writer, reader, integrity, exports, golden and damage tests | Emitting records from the kernel call sites (the event kinds exist; instrumentation lands with the executor and the run loop) |
| Index written | Index used for seeking (`show` will) |
| Whole-file read in Python | Streaming for multi-GB traces |

The reader loads the file into memory. At the modelled sizes (~1 MB per 50k events) that is fine; a streaming reader is a change to `read()` and not to the format.

---

## 7. Next

**P1-08 — replay.** Same seed twice must produce an identical digest; a mutated build id must be refused with exit 4. The codec now has the two pieces that needs: a digest that is a pure function of the run, and a reader that can prove it read the file the writer wrote.
