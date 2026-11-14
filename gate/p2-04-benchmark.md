<!-- SPDX-License-Identifier: Apache-2.0 -->

# P2-04 report — bug benchmark

**Date:** 2026-11-14
**Scope:** six seeded defects (B1–B6) across the PRD's four classes, one-change patches, expected seed ranges, and a fixed-build baseline.
**Result:** **6/6 found**, **0 false positives across 60,000 fixed-build runs** (6 × 10,000). Full gate run: **31 s** on eight jobs.

---

## 1. The gate, measured

```
$ make bench-bugs
B1 lost wakeup                               0-63 ->    8 finding(s)  found  | fixed 0-9999: 0 finding(s)
B2 reply after timeout                       0-63 ->   14 finding(s)  found  | fixed 0-9999: 0 finding(s)
B3 priority inversion deadlock               0-63 ->   14 finding(s)  found  | fixed 0-9999: 0 finding(s)
B4 cross-priority FIFO assumption            0-63 ->   50 finding(s)  found  | fixed 0-9999: 0 finding(s)
B5 pulse coalescing assumption               0-63 ->   32 finding(s)  found  | fixed 0-9999: 0 finding(s)
B6 client death                              0-63 ->   23 finding(s)  found  | fixed 0-9999: 0 finding(s)
bug benchmark: 6/6 found, 0 false positive(s) on the fixed build
```

A finding is a CRASH or VIOLATION from the worker. The fixed baseline also
counts BUDGET and INTERNAL as failures: a clean build must not exhaust a
budget or break the harness on a ten-instruction scenario.

`make bench-bugs-quick` runs the same six fixtures over 64 buggy seeds and 100
fixed seeds in a few seconds; `make ci` includes it so the fixtures cannot rot.

---

## 2. The six defects

| ID | Class | Defect | One-change fix | Failing seeds (0–63) |
|---|---|---|---|---|
| B1 | lost wakeup | `MsgRegisterEvent` after `MsgSend`; the server's delivery finds no registration | register before sending | 8 |
| B2 | reply after timeout | server sleeps 2 ms; a client with a 1 ms reply deadline abandons the rcvid | reply promptly | 14 |
| B3 | priority inversion deadlock | server `SchedSet(5)` while holding a prio-20 client, then waits on a prio-15 helper | do not lower while serving | 14 |
| B4 | cross-priority FIFO assumption | first arrival assumed to be the prio-5 sender; the queue delivers prio 15 first | identify by content | 50 |
| B5 | pulse coalescing assumption | one pulse drained, the channel asserted empty; a second event is still queued | drain until empty | 32 |
| B6 | client death | rcvid table never expires a client that timed out and exited | expire entries older than the deadline | 23 |

Each fixture is `scenario.c` + `fix.patch`. The benchmark copies the source to
a temporary directory, applies the patch with `patch -p1`, and builds both
variants through `tools/make_guest.py`, the same path a user's scenario takes.

---

## 3. How the race is made reproducible

All six defects are races whose losing path is chosen by a **seeded `rand()`
branch**. `rand()` is interposed and seeded from the run seed (FR-4.11), so the
branch is deterministic per seed and the failing set is a property of the
fixture. B1 fails on 8 of 64 seeds, B4 on 50 — the expected range, not a guess.

This is the honest construction for this simulator: same-priority threads are
FIFO, so priorities alone do not create seed-dependent interleavings. The
baseline records that the fixtures model the race outcome rather than proving
the scheduler explores it (LIMITS #55).

---

## 4. A modelling gap the benchmark found

B2 initially never failed. The trace showed why: when the client's send blocks
in **SEND** (no receiver yet) and the message is later received, the client
becomes REPLY-blocked, but a `_NTO_TIMEOUT_REPLY` timer armed before the send
does not activate on that transition. The timer only activates on a blocking
*call* in a matching state, not on a matching state entered by delivery.

The fixture now lets the server reach `MsgReceive` first, so the send blocks in
REPLY and the timeout behaves as the scenario expects. Whether QNX fires on
the SEND→REPLY transition is a conformance question that needs the SDP; it is
recorded as a modelling gap in LIMITS #56 rather than papered over.

---

## 5. Files

| Path | Purpose |
|---|---|
| `tests/fixtures/bugs/b1..b6/scenario.c` | the buggy source |
| `tests/fixtures/bugs/b1..b6/fix.patch` | the one-change fix |
| `bench/baseline.toml` | expected ranges, fixed baseline, jobs, requirements |
| `tools/bench_bugs.py` | build both variants, run both ranges, enforce the gate |
| `Makefile` | `bench-bugs`, `bench-bugs-quick` |

---

## 6. Verification

```
make bench-bugs        6/6 found, 0 false positives, 60,000 fixed runs, 31 s
make bench-bugs-quick  included in make test and make ci
make ci                green
```

No C or trace changes: the benchmark is fixtures plus a runner. `make asan`
is unaffected and was green at the previous commit.

---

## 7. Boundary

- The fixtures model race outcomes with a seeded branch; they do not prove
  schedule exploration (LIMITS #55).
- The SEND→REPLY timeout transition is a known gap (LIMITS #56).
- The 10,000-seed baseline is a dedicated gate, not part of CI; CI runs the
  100-seed quick form (LIMITS #57).
- The bug corpus is six fixtures, not a general mutator. Exploration-guided
  search on these builds is P2-07's measurement; this task proves the harness
  finds each one deterministically.

---

## 8. Next: P2-05, conformance

The benchmark is the first end-to-end evidence that a defect class survives
from SUT code to a reproducible seed. P2-05 turns to the other direction:
twelve scenarios buildable for both targets, canonical traces, and an outcome
comparison that runs without a QNX licence.
