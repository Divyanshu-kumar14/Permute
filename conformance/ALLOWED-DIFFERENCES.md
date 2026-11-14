<!-- SPDX-License-Identifier: Apache-2.0 -->

# Allowed differences

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** applies to `permute-canonical/1`. Every entry names the exact field
the comparator treats differently and why. Nothing else is allowed to differ.

FR-11.7 and risk R2 require a reviewer to map every observed divergence to a
documented reason, and cap the list at **8 entries**. The canonicaliser in
[`tracecmp.py`](tracecmp.py) applies entries 1–6 before comparison; entry 7 is
a property of the schema rather than a transform.

| # | Field | Treatment | Architectural rationale |
|---|---|---|---|
| 1 | `src`, `dst`, `obj` on every event | Raw kernel ids are replaced by labels: `tN` per thread, `pN` per process, and `ch@<owner>:N`, `co@<owner>:N`, `r@<owner>:N` for channels, connections and receive ids. Object labels are scoped to the thread that created or used them. | These values are allocation-order artefacts of the target's object table. QNX and the simulator allocate objects in different orders and from different numeric spaces, so comparing the numbers would report a difference where none is observable. The scoping stops two independently created objects from swapping labels when their creation events commute. Limit: labels can still swap if a target creates two independent threads in the opposite order (LIMITS #61). |
| 2 | `aux` on `CONNECT_ATTACH` only | The raw channel id is remapped through the channel label table before comparison. Everywhere else `aux` is compared exactly: the `BLOCK`/`UNBLOCK` state enum, the errno on `ERROR`, and the priority on `SCHED_PICK`. | `aux` carries an id in this one record, so it has the same allocation-order problem as entry 1. In every other record `aux` carries semantics the model must reproduce, which is why the exception is one field on one event kind and not a general rule. |
| 3 | `payload` on `SEND`, `RECV`, `REPLY`, `PULSE_SEND`, `PULSE_RECV`, `ERROR` | The bytes are replaced by the first 128 bits of their SHA-256, a colon, and the byte length (`<32 hex>:<length>`). | Payload identity is semantic: two runs that deliver different bytes must not compare equal. The digest keeps identity and length while removing representation differences (capture padding, alignment, byte order). Truncation to 128 bits is deliberate: a collision in a conformance run is negligible, and a full digest makes trace diffs unreadable. |
| 4 | `payload` on `THREAD_START` | Dropped (set to `null`). | The payload is the process name. The simulator loads a guest `.so` under `dlopen`; the target runs an executable with a different name. The name is a host packaging detail, not scheduler or IPC state, and no scenario asserts on it. |
| 5 | `t_ns` | Excluded by default. `--strict-time` includes it for investigation, never for a conformance verdict. | The simulator runs on a virtual clock that advances only when the ready queue is empty; the target runs on a real monotonic clock. They share no epoch and no rate, so a time comparison would fail every scenario for a reason that is not a fidelity gap. FR-11.3 requires causal order, not wall-clock order. |
| 6 | JSONL line order and the integer values in `causal` | Both traces are put in the canonical linear extension; causal edges are compared as `(parent event key, child event key)` pairs, not as index numbers. | This is FR-11.3's Mazurkiewicz equivalence: independent events commute, so any linear extension denotes the same trace. Indices change when independent events move; the edge set does not. A missing or extra real edge is still a failure. |
| 7 | Host-only data: wall-clock time, host paths, real pids, pointer values | Never emitted by `permute-canonical/1`. A capture that includes them fails schema validation rather than being normalised. | Two targets cannot produce reproducible host facts, and none of them is QNX semantics. Rejecting them at the schema boundary keeps them from silently entering the comparison. |

## Compared exactly — any difference fails

- `kind`, `schema`, and the event count.
- Causal edges: per-thread program order, send → receive, receive → reply,
  channel destroy → sender unblock.
- Outcome class, `ERROR` errno, and which thread unblocks.
- Effective priority at a dispatch (`SCHED_PICK`), pulse code and value, rcvid
  validity, and payload hashes (entry 3).
- `aux` outside `CONNECT_ATTACH` (entry 2) and `obj` after labelling (entry 1).

## Change control

The list is closed at 8 entries. Adding a ninth is R2's early-warning signal
and requires a `LIMITS.md` entry plus a decision record; widening an existing
entry to cover a new field needs the same review. `make docs-check` checks the
entry count, the table shape, and that each row names a field, a treatment and
a rationale, so a silent addition fails CI.
