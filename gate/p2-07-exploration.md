<!-- SPDX-License-Identifier: Apache-2.0 -->

# P2-07 report — exploration engine

**Date:** 2026-11-14
**Scope:** novelty bitmap, corpus, six deterministic mutations, budgets, escalation, sharding, `LIVELOCK_SUSPECT`, and the random-vs-novelty measurement on B1–B6.
**Result:** both policies operational; **`--shard 2/4` standalone is byte-identical to shard 2 of a full run**; the spin fixture reports `LIVELOCK_SUSPECT`; novelty found **74** non-clean outcomes against random's **70**, with 6/6 bugs found by both. `make ci` and `make asan` green.

---

## 1. The gate, measured

```
test_explore: 37 checks, 0 failure(s)
```

| Check | Result |
|---|---|
| `--explore random` and `--explore novelty` both run | 12 iterations each in the unit test, 32 in the benchmark |
| `--shard 2/4` alone equals shard 2 of a full run | seeds 2, 6, 10, 14: traces **byte-identical** |
| Spin fixture ⇒ `LIVELOCK_SUSPECT` | 3 seeds, seed 0 spins, 1 budget = 33% > 2%; exit 1 |
| Random-vs-novelty metric | 70 vs 74 findings; both 6/6; see §3 |

The shard test is end to end through `permute run`: a standalone shard writes
two trace files, a full run writes eight, and the shard's files compare equal
byte for byte to the corresponding full-run files.

---

## 2. What was built

| Path | Purpose |
|---|---|
| `cli/permute/explore/bitmap.py` | decision-state and causal-edge features hashed into a 64 KiB bitmap |
| `cli/permute/explore/mutate.py` | six operators: pick flip, suffix splice, run delete/duplicate, parameter perturbation, stimulus shift, reseed |
| `cli/permute/explore/engine.py` | corpus chain, novelty scoring, escalation, shard slicing, findings |
| `cli/permute/__main__.py` | `--explore`, `--iterations`, `--shard`; `LIVELOCK_SUSPECT` for both sweep and exploration |
| `tests/python/test_explore.py` | 37 checks: bitmap, operator determinism, policies, escalation cap, shard equality, CLI livelock |
| `bench/explore-compare.py` | the B1–B6 measurement |

### Determinism

No RNG. Operators use `splitmix64(index)`; the parent is
`corpus[i % len(corpus)]`; random policy reseeds from the same function. Two
runs with the same inputs produce the same entries.

### Sharding

Every shard computes the full chain and records only `index % n == shard`.
Skipping earlier iterations would change the corpus and the later mutations.
Correctness costs recomputation (LIMITS #70); the gate asks for equality, not
for shard-local work.

### Escalation

A budgeted run that produced new features is retried once at 4× the budget;
the count is capped at three per exploration. Tested with a fake runner:
one retry, and a hard cap of three.

### Livelock

`permute run` compares budget outcomes to seeds (or iterations) and reports
`LIVELOCK_SUSPECT` above 2%, exiting 1 when nothing else failed. The spin
fixture is the positive case. This is a signal, not proof: an expensive seed
can hit a budget too (LIMITS #72).

---

## 3. Random versus novelty on B1–B6

`make bench-explore`, 32 iterations per policy per bug:

| Bug | Random findings (first) | Novelty findings (first) |
|---|---|---|
| B1 lost wakeup | 3 (9) | 8 (3) |
| B2 reply after timeout | 11 (0) | 9 (3) |
| B3 PI deadlock | 11 (0) | 9 (3) |
| B4 FIFO assumption | 21 (1) | 29 (0) |
| B5 coalescing | 12 (1) | 9 (5) |
| B6 client death | 12 (2) | 10 (0) |
| **Total** | **70**, found 6/6, first-sum 13 | **74**, found 6/6, first-sum 14 |

Novelty found **5.7% more** non-clean outcomes and the same six bugs. Time to
first finding was marginally worse (14 vs 13 summed indices). The honest claim
is "more coverage, no fewer bugs", not "novelty is faster". B1–B6 are the P2-04
fixtures, not a held-out set (LIMITS #71); FR-5.4's held-out measurement remains
open.

---

## 4. Boundary

- The tape is the **input** tape (seed, faults, stimuli). The kernel has no
  decision-override API, so the FR-5.5 operators act on inputs, not a persisted
  decision sequence (IMP-77, LIMITS #69).
- Every shard recomputes the chain (LIMITS #70).
- The benchmark is not held out (LIMITS #71).
- `LIVELOCK_SUSPECT` is a budget ratio (LIMITS #72).

---

## 5. Verification

```
make explore     37 checks, 0 failures
make bench-explore  70 vs 74 findings; 6/6 both
make mutation    12/12 killed (unchanged)
make conformance 12 executed, 0 failures (unchanged)
make ci          green
make asan        green
```

---

## 6. Next: P2-08, determinism self-check

`permute run --verify 0.02` re-execs sampled seeds and compares digests; a
planted nondeterminism must exit 5 with both digests. The shard work here
already proves that a seed's trace is independent of how the run was
partitioned.
