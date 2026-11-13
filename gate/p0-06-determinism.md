<!-- SPDX-License-Identifier: Apache-2.0 -->

# P0-06 report — determinism bootstrap

**Date:** 2026-11-14
**Commit:** `1fb1d16` + this work
**Scope:** ASLR control, layout stability, locale pinning, counter-based PRNG, cross-language vectors.
**Result:** **all five checks pass**, including a negative control that proves the layout check is not blind.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `kernel/rng.h/.c` | `splitmix64` and the single entropy entry point `permute_rnd(seed, domain, index)` |
| `kernel/decisions.h/.c` | 7 decision domains, per-domain counters, pure `permute_stream_at` for the shrinker |
| `kernel/determinism.h/.c` | Locale/TZ pin, one-shot ASLR re-exec, persona-bit verification, `/proc/self/maps` fingerprint |
| `tests/spike_determinism.c` | Machine-readable run: `ASLR=`, `PERSONA=`, `MAPS=`, `DIGEST=`, `CHECKS=` |
| `tools/seedrun.py` | The gate: stability, negative control, seed spread, C↔Python vectors |

---

## 2. Results

```
[PASS] same seed: 200 runs -> 1 digest(s), 1 layout(s)
[PASS] ASLR disabled and verified via /proc/self/personality
[PASS] negative control: layouts differ with ASLR on (fdd9432c5c59e6cf vs 0d44359250a9b9e9)
[PASS] seed spread: 100 distinct digests over 100 seeds (need >= 95)
[PASS] cross-language: 5000/5000 vectors agree between C and Python

PASSED in 0.6s
```

| Check | Threshold | Measured |
|---|---|---|
| Same-seed stability | 1 digest, 1 layout over N runs | 200/200, 1 and 1 |
| ASLR control | persona bit `ADDR_NO_RANDOMIZE` observed set | `PERSONA=1`, `ASLR=off` |
| Layout sensitivity | layouts must differ with ASLR on | differ (`fdd9…` vs `0d44…`) |
| Seed spread | ≥95 distinct per 100 seeds | 100/100 |
| C↔Python agreement | 100% | 5000/5000 |

The `MAPS` value is a fingerprint of *this binary's* address space, so it changes whenever the code changes. That is expected: it is a cross-run comparison, not a project constant. The stream digest for seed 42 (`c59aeb06c7633fcd`) is stable across builds and can be used as a golden value until `DRAW_ROUNDS` or the hash changes deliberately.

Reproduce with `make determinism`.

---

## 3. Why each check exists

**Same-seed stability** is the only check that can catch address-space or layout drift, because it compares *separate process launches*, not two calls inside one process.

**The negative control** is the important one. "One layout across 200 runs" is also what a blind or constant fingerprint would report. Running the same binary with the re-exec disabled must produce *different* layouts; if it did not, the stability result would be meaningless. It does differ, so the measurement has teeth.

**ASLR verification** reads `/proc/self/personality` and tests the `ADDR_NO_RANDOMIZE` bit rather than trusting that the `personality()` call succeeded. The call also only takes effect on `exec`, which is why the process re-executes exactly once and gates on an environment variable.

**Seed spread** rules out the degenerate case: a PRNG that ignores its input would give perfect determinism and zero exploration value. 100 distinct digests from 100 seeds is the cheap guard.

**Cross-language vectors** lock the C and Python implementations together now rather than at P1-11. If they drift, normal runs stay green and the shrinker silently produces wrong minimal cases — the worst possible failure mode, because it looks like a shrinker bug rather than a PRNG bug.

---

## 4. Design notes worth keeping

- The digest in this spike is **FNV-1a 64**, deliberately named `DIGEST`, not `run_digest`. The real `run_digest` is BLAKE3-64 over the trace hash chain (ADR-007) and does not exist yet. Nothing here should be quoted as the product digest.
- `permute_rnd` is a pure function of `(seed, domain, index)`, so decision locality is structural rather than enforced by discipline. The spike's `locality` check exists to catch a future refactor that reintroduces a stateful stream.
- `PERMUTE_NO_ASLR_REEXEC=1` is a **test-only** escape hatch for the negative control. It makes the bootstrap report `ASLR=unavailable`; it cannot silently claim success.
- Locale and timezone are pinned inside the bootstrap, before any formatting, so `LC_ALL=C`/`TZ=UTC` cannot be forgotten by a later entry point.

---

## 5. What is still missing

| Gap | Consequence | Task |
|---|---|---|
| No trace, no BLAKE3 | `run_digest` is not implemented; this digest is a stand-in | P1-07 |
| No scheduler | `D_SCHED` values are generated but not consumed | P1-04 |
| No allocation determinism | Arena does not exist; allocation history could still leak | P1-10 |
| ASLR is verified, not enforced against a hostile `LD_PRELOAD` | Out of scope for v0.1 | — |
| musl / aarch64 | Not yet exercised | P1-15 |

The `PERSONA` and `MAPS` fields stay in the binary's output permanently; they are the evidence a reviewer can re-run.

---

## 6. Next

**P0-05** — hypercall page: publish the kernel op table at a probed fixed address (`MAP_FIXED_NOREPLACE` from a candidate list), record the chosen base in the run manifest, and keep the plain pointer as the fallback. That replaces the day-1 global with the structure the architecture specifies, without changing the shim contract.
