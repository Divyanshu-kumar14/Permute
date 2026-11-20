<!-- SPDX-License-Identifier: Apache-2.0 -->

# Submission integrity audit — QNX Global Hackathon

**Date:** 2026-11-20 (QNX target evidence re-run; first audit 2026-11-19)
**Scope:** `README.md`, `QUICKSTART.md`, `docs/`, `bench/`, `conformance/`, `gate/`, `tools/qnx_image.sh`, tracked sources, and committed evidence.
**Audit standard:** `submission-integrity` skill (Phases 0–7), plus the repository's own gates (`tools/submission_check.py`, `tools/claim_check.py`, `make test`, `tools/gate1.sh`).
**Status:** certified passing on the working tree that carries this record.

---

## VERDICT: CLEAR (READY FOR DEMO & SUBMISSION)

Every hard gate is backed by a tracked file or a command that was run against
this tree. Numbers in the prose documents were checked mechanically against the
evidence directories; no unverified number remains.

| # | Requirement | Evidence (file / command) | Verdict |
|---|---|---|---|
| 1 | Public GitHub repository | Remote `github.com/Divyanshu-kumar14/Permute`; CI and nightly workflows tracked under `.github/workflows/` | **PASS** (platform setting, confirmed in the GitHub UI) |
| 2 | Open-source licence detectable at root | `LICENSE` (Apache-2.0), `NOTICE`; `tools/submission_check.py` row *Licence at root* | **PASS** |
| 3 | Created within the submission window | First commit `66828cb`, 2026-11-14; the Devpost listing states the window as Nov 2 – Dec 15, 2026 | **PASS** |
| 4 | Project builds for and runs on QNX 8.0 | `tools/qnx_image.sh` cross-builds, boots, and exits non-zero unless 14/14 return 0; `docs/evidence/qnx/build.log` (`qcc -Vgcc_ntox86_64`, SDP 8.0: 14 built, 0 failures); `docs/evidence/qnx/serial.log` (14/14 `rc=0`) | **PASS** |
| 5 | Clean-room implementation | `NOTICE`; `tools/submission_check.py` row *Prohibited content* (no vendor files, images, binaries, or secrets in the tracked tree) | **PASS** |
| 6 | Exact sponsor category aligned | `README.md` names *Developer Tooling & Host Tools*; `tools/submission_check.py` row *Category and prior work* | **PASS** |
| 7 | Replay determinism | `tools/gate1.sh` row *Replay determinism* (1000/1000 same-seed); `gate/p3-05-nightly-soak.md` (nightly workflow: 2,000,000 runs, 0 failures) | **PASS** |
| 8 | Defect detection efficiency | `gate/p2-04-benchmark.md`; `make test` prints `bug benchmark: 6/6 found, 0 false positive(s) on the fixed build` | **PASS** |
| 9 | Mutation kill score | `gate/p2-06-mutation.md`; `tools/gate1.sh` row *Mutation score* (12/12 killed) | **PASS** |
| 10 | Trace minimization | `bench/shrink-results.md` (20 artifacts, 12-event reduced traces, p95 0.149 s, all `proven`) | **PASS** |

### Verified claims

- **Target execution:** 14/14 conformance scenarios cross-built with QNX SDP 8.0
  and executed inside a booted QNX 8.0 x86_64 image under QEMU (4 vCPU, 2 GB,
  `procnto-smp-instr`); every scenario returned `rc=0`. The evidence was
  regenerated on 2026-11-20 from `9df026ce4f7064a10991ab724eaa829a3fcd7875`
  with target sources clean; the serial log, build log, QEMU command line, and
  image hashes are committed under `docs/evidence/qnx/` and indexed in
  `docs/QNX-EVIDENCE.md`. `tools/qnx_image.sh` repeats the run end to end.
- **Host-side determinism:** the failure trace in the evaluator walkthrough is
  reproducible across separate builds of the same project; replay verifies the
  chunk chain, event order, and BLAKE3-64 run digest.
- **Claim check:** every number in `README.md`, `QUICKSTART.md`, and
  `docs/DEMO_WALKTHROUGH.md` resolves to a number present in `bench/`,
  `conformance/`, `docs/evidence/`, or `gate/`, or to an environment fact
  allowlisted with its source in `tools/claim_allow.txt`. The check reports
  `0 unverified number(s)`.

### Claims deliberately not made

These are scope boundaries, not defects. Each is stated in the document that a
judge would read, so no reader is surprised later.

| Boundary | State | Where it is disclosed |
|---|---|---|
| Target trace capture (`permute-capture`) | Not implemented. The target run validates each scenario's own assertions; scheduler traces are compared on the host. | `docs/QNX-EVIDENCE.md` §1 and §4; `tools/submission_check.py` row *QNX oracle capture* (WARN) |
| Fiber backend | Not implemented; the soak covers the thread backend only. | `docs/LIMITS.md` #88 |
| Mutex-based guest scenarios (`s07_priority_ceiling`) | Skipped on the host because glibc returns `ENOTSUP` for `PTHREAD_PRIO_PROTECT`; the ceiling protocol itself is covered by the `test_pi` unit fixtures. | `docs/REPRODUCE.md` §6; `docs/LIMITS.md` #37 |
| NFR-1 as a green nightly badge | The nightly workflow measured 2,000,000 runs with 0 failures on 2026-11-16. The badge reflects the workflow definition, not a per-commit run. | `gate/p3-05-nightly-soak.md` §3 |

---

## Claims corrected in this pass

| Claim | Why it was unverified | Resolution |
|---|---|---|
| Python `3.12+` in `README.md`, `QUICKSTART.md`, `docs/REPRODUCE.md` | No file pinned 3.12; `tomllib` is standard from 3.11, and no 3.12-only API is used | Requirement is now `3.11+`; `tools/claim_allow.txt` names the `tomllib` import as the source |
| `888-1182 ns` context switch in the `docs/REPRODUCE.md` gate1 sample | The value appears in no artifact; `tools/gate1.sh` prints one measured value | Replaced with `360 ns`, the value in the regenerated `gate/gate1-scorecard.md` |
| "Target build and execution requirements are completely satisfied" in `docs/QNX-EVIDENCE.md` | Absolute wording covering the two trace rows, which are not implemented | Bounded to the build and execution fields of PRD §17 row 4; trace fields remain listed as not implemented |

---

## Re-running this audit

From the checkout root:

```sh
python3 tools/submission_check.py
python3 ~/.config/opencode/skills/submission-integrity/tools/claim_check.py \
  --prose README.md QUICKSTART.md docs/DEMO_WALKTHROUGH.md \
          docs/QNX-EVIDENCE.md docs/REPRODUCE.md \
  --evidence bench/ conformance/ docs/evidence/ gate/ \
  --allow tools/claim_allow.txt
make test
make quickstart
bash tools/gate1.sh
tools/qnx_image.sh --evidence docs/evidence/qnx   # needs QNX SDP 8.0 + QEMU
```

Expected: `submission: PASSED` with one documented warning (oracle capture),
`0 unverified number(s)`, `quickstart: PASSED`, and a G1 scorecard whose result
line is `PASS`. A failure in any of these is a real regression, not a
documentation problem.
