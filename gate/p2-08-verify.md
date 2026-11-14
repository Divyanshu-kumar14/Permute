<!-- SPDX-License-Identifier: Apache-2.0 -->

# P2-08 report — determinism self-check

**Date:** 2026-11-14
**Scope:** `permute run --verify RATE`, fresh-process re-execution, BLAKE3-64 digest comparison, exit 5 diagnostics, and a planted nondeterminism fixture.
**Result:** deterministic runs pass with **0 mismatches**; the planted fixture **reliably exits 5** showing both digests. `test_verify` 17 checks. `make ci` and `make asan` green.

---

## 1. The gate, measured

```
test_verify: 17 checks, 0 failure(s)
```

Deterministic, 3 seeds of s01:

```
verify: 3 seed(s) re-executed, 0 mismatch(es)
3 seeds: 3 clean, ...
exit 0
```

Planted, `PERMUTE_NONDETERMINISTIC_SEED=1`:

```
verify: 3 seed(s) re-executed, 1 mismatch(es)
  NONDETERMINISM seed 1: f8662b82884c963b != 9879b9b345b17706
3 seeds: 3 clean, ...
exit 5
```

`summary.json` carries the same evidence:

```json
{"rate": 1.0, "sampled": 3, "checked": 3,
 "mismatches": [{"seed": 1, "first": "f8662b82884c963b",
                 "second": "9879b9b345b17706"}]}
```

---

## 2. How it works

`--verify` takes an optional rate (default 0.02 when the flag is bare). The
sampler walks the seeds whose traces are present in an even, deterministic
spacing that always includes the first; the count is `max(1, round(rate*n))`.

Each sampled seed is re-executed by a **fresh `permute-run` process** with the
same guest and fault plan, in a new temporary directory. The new trace's
BLAKE3-64 `run_digest` is compared with the original. A partial trace at either
end is skipped: it cannot prove identity (LIMITS #73).

A mismatch prints both digests, sets `NONDETERMINISM`, and exits 5. Exit 5
takes precedence over a violation from the original run: a finding the harness
cannot reproduce is not evidence (IMP-81).

The planted fixture flips one byte of the build id on every second execution of
the named seed, using a counter file under `PERMUTE_NONDET_DIR`. The build id
is part of the digest, so the scenario is unchanged while the digest moves. The
hook is environment-gated and does nothing unless asked (IMP-82).

---

## 3. What the tests cover

| Check | Result |
|---|---|
| even sampling, rate 0, rate 1, first seed included | pass |
| deterministic guest, 3 seeds, 100% sample | exit 0, 0 mismatches |
| built-in workload, 3 seeds | exit 0, 0 mismatches |
| planted nondeterminism on seed 1 | exit 5, `NONDETERMINISM`, both digests, summary mismatch |
| invalid rate (`2.0`) | exit 2 |
| shard interaction | checks only that shard's traces (LIMITS #74) |

---

## 4. Boundary

- Verification covers traces that exist; a crashed seed with no complete trace
  is already a finding and is skipped (LIMITS #73).
- In a shard run, only the shard's seeds are verified (LIMITS #74).
- The planted hook is a test device in the runner, not a product feature
  (LIMITS #75).
- Digest equality proves log identity, not SUT-output equality beyond what the
  log records; the trace is the evidence the product uses.

---

## 5. Verification

```
make verify   17 checks, 0 failures
make ci       green
make asan     green
```

The normal path is unchanged: `--verify` defaults to 0 and the runner has no
active hook without the environment variable.

---

## 6. Next: P2-09, reports

`report.json`, `junit.xml`, `sarif.json`, and a self-contained `index.html`
with inlined CSS and no network access. The summary from this task (`verify`,
`oracle_findings`, counts, verdict) is the input those renderers will consume.
