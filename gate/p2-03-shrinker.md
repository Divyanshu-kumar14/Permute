<!-- SPDX-License-Identifier: Apache-2.0 -->

# P2-03 report — input shrinker

**Date:** 2026-11-14
**Scope:** the five-stage pipeline (causal pre-slice → stimulus ddmin → time-window minimisation → tape elimination → fault materialisation), the execution budget, `minimality`, failure artifacts, and `permute shrink`.
**Result:** **37 checks, 0 failures**. A 10,000-input failure reduces to 3 inputs in 9 executions; a 200-decision tape to 8; a 20-rule fault plan to 1. The budget is exact at 10/10, exhaustion reports `budget_limited`, and repeated runs are byte-identical. `make ci` green.

---

## 1. The gate, measured

```
test_shrink: 37 checks, 0 failure(s)
```

| Fixture | Before | After | Reduction | Executions | Minimality |
|---|---|---|---|---|---|
| stimulus tape, 3 of 10,000 needed | 10,000 | 3 | 3333× | 9 | proven |
| decision tape, 8 of 200 needed | 200 | 8 | 25× | < 1,500 | proven |
| fault plan, 1 of 20 needed | 20 | 1 | 20× | bounded | proven |
| real artifact (s01 + 3 rules) | 3 | 1 | 3× | 8 | proven |

The first row is the plan's 10,000-event criterion. It is only reachable with
the causal pre-slice: ddmin over 10,000 sparse inputs would need thousands of
executions because each complement removes only a few items. The fixture marks
each stimulus with the record it affects, the cone starts at the finding, and
9,997 inputs leave before the first ddmin test. A tape with no recorded effects
is instead reported `budget_limited` — proven reduction is not claimed when it
was not verified.

The 200→8 decision tape is the 10×–100× sample: `25×`, reached by ddmin plus a
single-override sweep. The real artifact run reduces three configured fault
rules to the one that causes the deadlock and proves it.

---

## 2. Budget

Two independent bounds, both checked **before** an execution:

- **1,500 executions.** A test with `--max-execs 10` reports exactly
  `executions == 10`, `minimality == "budget_limited"`. Checking after the run
  would allow 11.
- **90 seconds.** A monotonic clock is sampled before each run; a single
  execution that overruns the bound stops the pipeline immediately after it,
  which is the closest a non-preemptible callback permits. The test uses a
  50 ms bound and asserts exhaustion plus an early stop.

The reducer never retries after exhaustion; the best candidate found so far is
kept and serialised.

---

## 3. Pipeline

| Stage | What it does | Verification |
|---|---|---|
| causal pre-slice | drops inputs whose recorded effects are outside the backward cone of the finding | one execution; falls back bucket-by-bucket if the cone was wrong |
| stimulus ddmin | Zeller ddmin, index-ordered splits, complements in index order | each candidate re-executed |
| window min | binary search on the window start, then the end | O(log range) executions |
| tape elimination | per-domain ddmin, then a single-override sweep | each candidate re-executed |
| fault materialisation | ddmin + single-rule sweep, canonical order | each candidate re-executed |

The cone walks backwards over program order within a thread, receive ↔ reply by
rcvid, and send → receive by identical payload. Ambiguous payload matches link
every candidate, making the cone larger and the slice conservative. As a
soundness rule, an input with no recorded effects is **kept**: a rule that never
fired may still matter.

No stage edits the event log. The predicate receives a complete input set and
answers whether the failure still reproduces (FR-6.7, ADR-009).

---

## 4. Determinism

`ShrinkResult.to_json()` is sorted, compact, and contains no wall-clock value
(IMP-64). Decision overrides are canonicalised by `(domain, index)` and
deduplicated; chunk order is index order; there is no RNG.

Verified: two runs of the 10,000-input fixture produce identical bytes; two
`permute shrink` invocations on the same artifact produce identical stdout and
an identical output file.

---

## 5. Real artifacts

`permute run` now writes one failure artifact per failing seed:

```json
{
  "format": "permute-artifact/1",
  "seed": 0,
  "guest": ".../proc_s01_hello__0.so",
  "faults": ["pulse:drop@0", "recv:timeout@0", "send:drop@1"],
  "stimuli": [],
  "decisions": [],
  "window": null,
  "invariant": {"id": "dl", "kind": "deadlock", "severity": "error"},
  "finding": {"kind": "deadlock", "seq": 0, "seed": 0, ...},
  "trace": ".permute/run/traces/seed-0.pmt",
  "run_digest": "...",
  "minimality": "unshrunk"
}
```

`permute shrink ARTIFACT` re-executes candidates through the same `permute-run`
the run used and checks that the same invariant kind still fires. An artifact
whose failure no longer reproduces is refused with exit 4 (IMP-65): shrinking
must not turn stale evidence into a smaller non-failure.

The real run's artifact records three fault rules; the shrinker keeps
`send:drop@1` and drops the two zero-probability rules, `proven`, in eight
executions (one reproduce, two slice, five reduction).

---

## 6. Boundaries

- The runtime consumes the seed and the fault plan. Stimuli, a time window, and
  a decision tape are recorded but not applied, so a real shrink reduces them
  to empty; their stage algorithms are exercised by input-tape fixtures
  (IMP-66, LIMITS #50).
- A candidate is not preempted mid-execution; the real runner has a 30 s
  per-candidate timeout (LIMITS #51).
- Artifacts carry the trace path, not a compressed tape (LIMITS #52).
- One artifact at a time; batch shrinking belongs with the corpus work
  (LIMITS #53).

---

## 7. Verification

```
test_shrink: 37 checks, 0 failure(s)
  algorithm: 10,000 -> 3, 200 -> 8, 20 -> 1, window (4500,4501)
  budget:    exactly 10/10, budget_limited; wall bound stops early
  determinism: repeat output byte-identical
  CLI:       real artifact 3 -> 1, proven, 8 executions; stale artifact exit 4
make ci: green (unit, codec, replay, gate, scenarios, executor, faults,
                oracles, shrink, determinism, coverage, quickstart, ci-check)
```

---

## 8. Next: P2-04, the bug benchmark

The shrinker now turns a failing seed into a small input set. P2-04 needs six
defects (B1–B6), patches, expected seed ranges, and a fixed-build baseline:
6/6 found, 0 violations across 10,000 seeds. The artifact writer and the
shrinker are what make each found defect a readable case rather than a seed
number.
