<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-16 report — documentation skeleton, and the Tier 0 close-out

**Date:** 2026-11-14
**Scope:** README, QUICKSTART, CONFORMANCE, FAQ, owner and status on every reader-facing document, and an automated quickstart.
**Result:** `docs-check` passes on 8 documents; `quickstart` completes `init → run → show → replay`; `make ci` green in **18.7 s**.

---

## 1. What was written

| Path | Purpose |
|---|---|
| `README.md` | what it is, what works, what does not, requirements, doc map |
| `QUICKSTART.md` | build → test → init → run → show → replay, with real output |
| `docs/CONFORMANCE.md` | the 12 mandatory scenarios, all marked **not run**, and the capture procedure |
| `docs/FAQ.md` | emulator? binary? QEMU? licence? real-time? |
| `docs/ARCHITECTURE.md` | owner added; stale "nothing implemented" status corrected |
| `docs/DECISIONS.md`, `docs/LIMITS.md`, `docs/DETERMINISM.md` | owner added |
| `tools/docs_check.py` | every reader-facing doc names an owner and a status |
| `tools/quickstart_check.py` | follows QUICKSTART and asserts its output |

---

## 2. Verification

```
docs-check: PASSED (8 document(s) with owner and status)
quickstart: PASSED (init -> run -> show -> replay)
licence scan: PASSED
determinism: PASSED (1000 runs, 100 seeds, 10 runs/seed, 100 distinct digests)
coverage gate: PASSED (kernel 81.4%, shim 77.4%, log 92.7%)
ci-check: PASSED (2 workflow(s), musl image)

make ci  18.7 s
```

The quickstart output in `QUICKSTART.md` is copied from a real run, and
`quickstart_check.py` re-runs those commands against the current build, so the
document cannot drift without a test failing.

---

## 3. Tier 0 exit criteria — status

| P1 exit criterion | Status | Evidence |
|---|---|---|
| Clean clone → `make && make test` ≤ 10 min | **Met locally** | `make ci` 18.7 s; the clean-clone run is CI's job and has not been executed on a runner yet |
| 1,000/1,000 same-seed determinism | **Met** | `make determinism`, 100 seeds × 10 runs, one digest each |
| C01 exact match against a real target | **Blocked** | no QNX SDP; `docs/CONFORMANCE.md` records it as not run |
| ≥ 100,000 messages/s at the product boundary | **Not yet measured** | the model's 10k-message loop is far faster, but that is not the shim/executor boundary and is not quoted as the claim |

Two of the four are met, one is blocked on an external dependency, one has no
measurement yet. **Tier 0 is complete as a build milestone, not as a fidelity
claim.** Saying otherwise would contradict the conformance document three
directories over.

---

## 4. What this milestone does mean

- Every requirement implemented so far has a committed test and a gate report.
- A new reader can go from a clean checkout to a green, inspectable, replayable
  run using only `README.md` and `QUICKSTART.md`.
- Every reader-facing document has an owner and states what it describes.
- The determinism, coverage, licence, and workflow gates run with one command,
  and CI runs the same commands.

## 5. What it does not mean

- The simulator has not been compared to QNX. The model tests prove the
  implementation matches its specification, not that the specification matches
  the target.
- The executor does not exist. Scenarios compile; they are not yet scheduled.
- Oracles, shrinking, and the HTML report are P2.

The two standing external items are unchanged: **the QNX SDP download has not
started**, and **OD-02 (staffing) remains unsigned**.

---

## 6. Next

**Phase P2 — Tier 1 product.** The executor and the scenario loader first, so a
scenario source actually runs; then oracles over the trace, the shrinker, the
fault injector, and the report. The conformance harness becomes meaningful once
scenarios run, which makes the SDP the critical path rather than a parallel
chore.
