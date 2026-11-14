<!-- SPDX-License-Identifier: Apache-2.0 -->

# P3-04 report — shrink performance

**Date:** 2026-11-14
**Scope:** G2 shrink criterion — p95 wall ≤ 30 s and a reduced reproduction of
≤ 20 events, measured over 20 failures through the real `permute shrink` CLI.
**Result:** **PASS.** p95 **0.381 s**, p50 **0.374 s**, every reduced case at
**12 events**, all 20 `proven`. Evidence: [`bench/shrink-results.md`](../bench/shrink-results.md).

---

## 1. Measured

| Metric | G2 limit | Measured | Verdict |
|---|---|---|---|
| p50 wall, 20 failures | — | **0.374 s** | — |
| p95 wall, 20 failures | ≤ 30 s | **0.381 s** | PASS |
| Reduced reproduction | ≤ 20 events | **12 events** (max) | PASS |
| Reduced inputs | — | **1** (all cases) | — |
| Minimality | — | `proven` on 20/20 | PASS |

Hardware: 13th Gen Intel Core i5-13450HX, 16 cores, Linux 7.3.3,
Python 3.15.2. Commit `dabf99c` plus the P3-04 worktree changes; rerun
`make shrink-bench` at any commit to restamp the file.

Cases are the `s01_hello` deadlock with **3–8 fault rules**: the necessary
`send:drop@1` plus 2–7 redundant rules. The shrinker has to discard the
redundant rules, so the measurement covers ddmin and fault materialisation, not
a one-item special case.

---

## 2. The bottleneck and the fix

The first measurement was **11.4 s** for one six-rule case. Profiling showed
the shrinker itself was not the cost: a candidate that does *not* reproduce
runs the guest to its seed budget and emits a 90,015-record trace, and the
predicate spent **3.55 s** verifying its CRC32C and BLAKE3 chain in Python.

That verification is meaningless for a scratch file: the trace was written
moments earlier by the shrinker's own runner into a temporary directory and
never becomes evidence. `codec.read` now takes `verify=False` for exactly that
case, and `make_predicate` uses it. Recorded traces, replay inputs and reports
still verify by default.

| Path | Before | After |
|---|---|---|
| Non-reproducing candidate (90,015 records) | 3.55 s read + 0.1 s run | 0.10 s read + 0.1 s run |
| Six-rule case, end to end | 11.37 s | 0.97 s |
| 20-case p95 | not measurable before the fix | 0.381 s |

The 20-case bench with the fix is 9 s of wall time including 20 CLI launches.

---

## 3. Method

`tools/shrink_bench.py`:

1. Writes 20 artifacts with `write_artifact` (seed, guest, 3–8 fault rules,
   deadlock invariant).
2. Times `tools/permute shrink ARTIFACT --json` for each.
3. Re-executes the reduced input and counts trace records with
   `codec.read(..., verify=False)`; that count is the event count.
4. Reports nearest-rank p50/p95 and fails on any timeout, non-zero exit,
   `budget_limited`, > 20 events, or p95 > 30 s.

`make shrink-bench` writes the Markdown evidence and exits non-zero on a
failure. It is not part of `make ci` because it is a performance gate, not a
deterministic unit test; G2/P3-10 consumes it.

---

## 4. Reproduce

```sh
make shrink-bench
```

```
shrink-bench: 20 failures, p50 0.374 s, p95 0.381 s (limit 30 s)
...
Result: PASS
```

---

## 5. Verification

```
make codec        547 checks, 0 failure(s)
make shrink        37 checks, 0 failure(s)
make shrink-bench  20/20 cases, p95 0.381 s, max 12 events, PASS
```
