<!-- SPDX-License-Identifier: Apache-2.0 -->

# Determinism

**Status:** normative for v0.1. A determinism violation is a release blocker.
**Last updated:** 2026-11-14
**Companion documents:** [`ARCHITECTURE.md`](ARCHITECTURE.md) §11 (invariants), [`DECISIONS.md`](DECISIONS.md) (ADR-003, ADR-004, ADR-006).

---

## 1. The claim

Two runs of the same seed, same build, same resolved configuration produce byte-identical canonical traces — with address-space randomisation on or off, and with any number of worker processes.

Formally, a run is a function of `(seed, config_hash, sut_build_id, kernel_model_version)`. Nothing else may influence the trace. The verification form of this claim:

```
run_digest = BLAKE3-64(hash_chain ‖ config_hash ‖ sut_build_id ‖ kernel_model_version)
```

`run_digest` must be identical across machines for the same inputs, and identical under `--jobs 1` and `--jobs 16`.

---

## 2. Why it is achievable here and hard elsewhere

Determinism is not free; it is bought by removing whole categories of nondeterminism rather than trying to control them.

| Category | How it is removed |
|---|---|
| Symmetric multiprocessing | Exactly one simulated CPU. No migration, no per-core queues, no memory-ordering questions |
| Kernel preemption | Cooperative: the kernel runs to completion inside a call and returns `Continue` or `Blocked` |
| Real time | A virtual clock that advances only when the ready queue is empty |
| Host scheduling | The schedule is chosen, not observed. Host wake order is transport, never a decision |
| Entropy | Counter-based PRNG; no shared stream, no ambient randomness |
| Address space | ASLR disabled before the first guest instruction; no address ever enters the trace |
| Allocator history | Arena with size-class free lists ordered by offset |

The residual risk is not randomness in the usual sense; it is *accidental state* — something read that was not part of the declared input. The hazard register below exists to find those.

---

## 3. Hazard register

Each hazard names the mechanism that neutralises it and the test that proves the mechanism works.

| ID | Hazard | Mechanism | Test |
|---|---|---|---|
| H-01 | Address-space randomisation changes layout | Re-exec with `ADDR_NO_RANDOMIZE`; assert identical memory map across launches | 200/200 identical digests; empty memory-map diff |
| H-02 | A pointer leaks into a record or a comparison | Trace audit; identifiers are ordinals; no address is serialisable | `symcheck --trace-audit`; review checklist |
| H-03 | Container iteration order is observable | Ordered containers only; hash containers with observable order are banned | Grep gate plus a planted-container fixture that fails the build |
| H-04 | Floating-point time arithmetic | Integer nanoseconds only (`vtime_t = uint64_t`) | Grep gate on kernel sources; unit tests |
| H-05 | A real clock is read | All time routes to the virtual clock; the symbol allowlist rejects clock sources | Planted real-clock call fails the build |
| H-06 | Random stream drift under shrinking | Counter-based `rnd(seed, domain, index)`; independent per-domain counters | Forcing decision *k* leaves decisions *k+1…N* unchanged |
| H-07 | Allocation history changes behaviour | Arena with sorted free lists; poison-on-free | 10⁶ mixed alloc/free operations produce an identical digest |
| H-08 | Locale or timezone changes formatting | `LC_ALL=C` and `TZ=UTC` forced before the first guest instruction | Harness assertion; golden output |
| H-09 | A real signal interrupts guest code | Signals are scheduled events; host signals blocked in guest workers | `sigprocmask` interposed and asserted |
| H-10 | Static initialisers run in a host-dependent order | Instances are opened only at a scheduled spawn event with a fixed sequence number | Constructor-order test |
| H-11 | Globals are merged between simulated processes | One shared object per process instance | Write a static in instance 0, read it in instance 1, expect the initial value |
| H-12 | Host thread identifiers leak | `gettid`, `getpid`, `pthread_self` return spawn-order ordinals | Golden trace comparison |
| H-13 | Host wake order influences the next thread | Wake order never consulted; the next thread is computed before any wake | 10⁶-switch stress with randomised host timing |
| H-14 | A fixed virtual address is unavailable (39-bit user VA) | Candidate bases tried in order; chosen base recorded in the manifest and excluded from the digest | Portable build job on a 39-bit configuration |
| H-15 | A libc-allocated pointer crosses the shim | Address-range dispatch in `free`/`realloc`; non-arena pointers forwarded | Free a libc pointer from guest code; no crash, no corruption |
| H-16 | Parallel workers influence each other | A seed depends only on its inputs; no shared mutable state between workers | `--jobs 1` vs `--jobs 16` digest equality |
| H-17 | Configuration drift between runs | `config_hash` in the digest; unknown configuration keys rejected | Mutate one config value, expect a different digest; add an unknown key, expect a refusal |

---

## 4. Proof obligations

A change is not complete until the relevant obligations pass. These are the five gates, in order of strength.

### 4.1 Same-seed identity

```
permute run --seed S --repeat 100
```

100 runs, one digest. Both backends. ASLR on and off. Any difference is a P0 defect with the two digests attached.

### 4.2 Parallel invariance

```
permute run --seeds 0..999 --jobs 1
permute run --seeds 0..999 --jobs 16
```

Per-seed digests must match exactly. This catches shared state, ordering assumptions in result collection, and batch-size sensitivity.

### 4.3 Decision locality

Force decision index *k* to a different value, then compare:

- decisions before *k*: unchanged
- decision *k*: changed
- decisions after *k*: unchanged

This is the property that makes shrinking monotone. It fails the moment a stateful random stream is reintroduced.

### 4.4 Cross-language vectors

The PRNG is implemented in C (kernel) and Python (shrinker). A 10⁵-vector golden file is generated from one implementation and checked against the other in CI. Drift here silently breaks shrinking while leaving normal runs green — which is the worst failure mode, because it looks like a shrinker bug.

### 4.5 Allocation independence

Run the same seed with different allocation pressure (an environment variable that adds a dummy allocation at boot). The digest must not move. If it does, the arena is leaking host memory state into the model.

---

## 5. Continuous verification

| Cadence | Check | Failure action |
|---|---|---|
| Every pull request | 100 seeds × 10 runs; parallel invariance on a 200-seed slice | Block the merge |
| Nightly | 1,000 seeds × 1,000 repeats; both backends; ASLR on and off | Open a P0 issue; stop feature work |
| Every release candidate | Full soak plus the negative control | Block the release |

A same-seed mismatch is never "flaky infrastructure". It is either a hazard that the register does not yet name or a mechanism that does not work. Add the hazard, fix the mechanism, add the test.

---

## 6. What determinism does not guarantee

- It does not mean the model is faithful. A perfectly deterministic wrong answer is still wrong; that is what conformance and mutation testing are for.
- It does not mean the SUT is correct. It means the same schedule is reproduced.
- It does not extend to target execution. Real hardware is nondeterministic; that is the problem this tool exists to work around, not something it can fix.
