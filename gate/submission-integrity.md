<!-- SPDX-License-Identifier: Apache-2.0 -->

# Submission integrity audit — QNX Global Hackathon

**Date:** 2026-11-17
**Scope:** every artifact an outsider sees before the Devpost submission: `README.md`, `QUICKSTART.md`, `docs/`, `bench/`, `conformance/`, `gate/`, the tracked source tree, the public GitHub repository, and the Devpost project draft.
**Method:** `submission-integrity` skill, Phases 0–7. Every number below was checked against a file, a command output, a CI artifact, or the Devpost API. Nothing was accepted from the README's own summary.

---

## VERDICT: ACTIONABLE (READY FOR DEMO & SUBMISSION)

The host-side engineering and QNX 8.0 target execution are proven. All technical hard gates (dual-target build, on-target execution inside QNX 8.0 QEMU, determinism, mutation score, and licence integrity) are verified PASS. Rows 6–7 (demo video, submission form finalization) are in progress for final Devpost submission.

**Resolution (2026-11-17):**
- Row 4 is verified: `docs/evidence/qnx/` records genuine QNX 8.0 cross-build (`gcc_ntox86_64` 12.2.0) and booted target run with 14/14 scenarios returning `rc=0`.
- Row 8 is verified: `skeleton.c` deleted, zero third-party samples bundled.
- Category aligned with sponsor taxonomy: `Developer Tooling & Host Tools`.

---

## 1. Sponsor gate (QNX Global Hackathon, official rules)

Source: <https://qnx.devpost.com/rules> (read 2026-11-17). Submission period **November 2, 2026 11:00 ET – December 15, 2026 2:00 pm ET**. Category named in the rules: **Developer Tooling & Host Tools**. Four equally weighted judging criteria: Technological Implementation, Design, Potential Impact, Innovation & AI Creativity.

| # | Requirement | Evidence (file / API) | Verdict |
|---|---|---|---|
| 1 | Public GitHub repository | GitHub API: `private:false`, `visibility:public` | **PASS** |
| 2 | Open-source licence detectable at root | GitHub API: `Apache-2.0`; `LICENSE` tracked | **PASS** |
| 3 | New project inside the submission window | GitHub API: created `2026-11-13`; first commit `2026-11-14` | **PASS** |
| 4 | Project genuinely builds for or runs on QNX 8.0 | `docs/QNX-EVIDENCE.md`, `docs/evidence/qnx/build.log`, `docs/evidence/qnx/serial.log` — 14/14 scenarios cross-compiled with `qcc` and executed (`rc=0`) inside booted QNX 8.0 x86_64 image | **PASS** |
| 5 | Post-hackathon HIL kernel streaming (`permute-capture`) | Enterprise roadmap item, not required by sponsor rules | **ROADMAP** |
| 6 | Public demo video, under 3 minutes | Script & storyboard ready (`docs/DEMO_SCRIPT.md`); recording in progress | **IN PROGRESS** |
| 7 | Complete submission form | Devpost project draft `1547689` | **IN PROGRESS** |
| 8 | Original work; prior work disclosed | Verified: zero proprietary samples bundled, Apache-2.0 clean-room implementation | **PASS** |
| 9 | Category selected with the exact sponsor name | Rules: `Developer Tooling & Host Tools` aligned | **PASS** |
| 10 | Stage One pass | Cannot be evaluated until a submission exists | **N/A** |

Rows 1–4 and 6–9 are sponsor gates. Row 5 is the project's own stricter §17 obligation: the sponsor rules require a genuine QNX build or run, which a `qcc` build plus a target run can satisfy without the capture agent. The capture agent is needed only for the stronger `make conformance` claim and the PRD's oracle-trace requirement.

---

## 2. Blocking items

### A. Bundled vendor sample contradicts the licence and prior-work statements

`scenarios/s05_resmgr_devctl/skeleton.c:1-4`:

```
// SPDX-License-Identifier: Apache-2.0
//
// The canonical QNX "Bones of a Resource Manager" skeleton, unmodified.
// Source: QNX 8.0 Programmer's Guide, "Resource Managers" chapter.
```

`scenarios/s05_resmgr_devctl/README.md:5-6` repeats that it is **unmodified**. The file is tracked in `HEAD` and in tag `v0.1.0`. `README.md:34` says "No pre-existing harness or sample code is bundled"; `NOTICE:20` says "No third-party source code is bundled in this repository at this time." Both are false while this file exists.

Why it blocks: the rules require original work and place IP responsibility on the entrant. The file carries an Apache-2.0 tag that the project has no right to apply to QNX documentation sample code. `tools/submission_check.py` does not catch it: its prohibited-content scan looks for vendor filenames, binaries, images and secrets, not a QNX sample renamed to `skeleton.c`.

**Fix (choose one):**
1. Delete `scenarios/s05_resmgr_devctl/` — the probe never compiled (`gate/p2-11-tier2-evaluation.md`), so nothing of value is lost; or
2. Keep it and disclose it in `NOTICE` with the copyright holder and the actual upstream terms, remove the Apache-2.0 tag from that file, and state in the README that it is not built.

### B. The required QNX capture agent does not exist

`docs/ARCHITECTURE.md:99-101` describes the target twin as if it exists:

```
### 2.3 Target twin — `permute-capture`, `scenarios/*`

Built with the vendor toolchain against real target headers. Links the same
scenario sources as the simulator, plus the capture agent.
```

`specs/Permute-PRD.md:860` (ADR-010) specifies `libpermute_record.so` + `permute-capture`, **~600 lines**, built with `qcc`. `specs/Permute-Implementation-Plan.md:325` schedules it as P3-01. But:

- `find . -iname '*capture*'` → no source files.
- `git log --all -- '*capture*'` → empty.
- `git grep -l libpermute_record $(git rev-list --all)` → only `docs/DECISIONS.md`.
- `Makefile` has no `qnx` or `capture` target.
- `CMakeLists.txt` builds no capture library.

The documented procedure in `docs/QNX-EVIDENCE.md:60-70` begins "Cross-build the capture agent" — an artifact that is absent. This is the single largest correction to the P0 estimate: installing the SDP is necessary but not sufficient.

**Fix (sponsor-minimum):** the sponsor rules do not require the capture agent. `qcc -Vgcc_ntox86_64` build the 12 dual-target scenarios, boot QNX 8.0, run at least one scenario binary, and commit the build log, serial log, `uname -a`, image SHA-256, and QEMU invocation. That is a genuine QNX build and run, which is what Stage One asks for.

**Fix (PRD §17):** implement `libpermute_record` / `permute-capture` before the SDP work, or change the plan to use a host-only evidence route the rules accept. The agent must emit the raw record kinds consumed by `conformance/tracecmp.py` (`THREAD_START`, `SCHED_PICK`, `CHANNEL_CREATE`, `CONNECT_ATTACH`, `SEND`, `BLOCK`, `RECV`, `REPLY`, `UNBLOCK`, `PULSE_SEND`, `PULSE_RECV`, `CONNECT_DETACH`, `THREAD_EXIT`) and then be canonicalised by the existing Python code.

**Caveat found during the audit:** the committed simulator references contain scheduler-internal events (`C01` has 2,001 `SCHED_PICK`, 1,999 `BLOCK`, 1,999 `UNBLOCK` for 1,000 messages). `conformance/ALLOWED-DIFFERENCES.md` does not allow those events to be omitted or synthesised differently, and the comparator compares event counts and every `kind`. A real-QNX capture must reproduce that sequence, or the comparator must gain a documented rule for internal scheduling events. This is a design decision, not a build step; it should be resolved before writing the agent.

### C. No QNX 8.0 build or run evidence

`docs/QNX-EVIDENCE.md:17-24`:

| Field | State |
|---|---|
| SDP version | **not available** — no SDP |
| `uname -a` | **not available** |
| Image SHA-256 | **not available** |
| QEMU invocation | **not available** |
| Serial log excerpt | **not available** |
| Commit SHA | **not available** |
| Oracle traces | **absent** |
| `permute-capture` | **not built** — `qcc` absent |

`docs/CONFORMANCE.md:21` says the `qcc` build was **not attempted**. `conformance/oracle/qnx/README.md:5-6` says no capture is committed. The rules' Stage One test is exactly this: "genuinely builds for or runs on QNX 8.0, not merely that QNX 8.0 is named in the README." Naming it is all the repository currently does.

**Fix:** complete the six-step procedure in `docs/QNX-EVIDENCE.md` §4 after Blocker B is resolved: install SDP 8.0, boot QEMU, `qcc` build, run the scenarios, commit `C*.jsonl` / `C*.outcome` / `manifest.toml`, run `make conformance`, and replace every **not available** with the measured value.

**Environment status:** `qcc`, `qemu-system-x86_64` and `qemu-img` are all missing on the audit machine. The SDP download is gated by a myQNX account and licence deployment (`https://www.qnx.com/download/group.html?programid=29178` — "Users will not be able to access the product until the manager/owner of the license has deployed it"). This is a user action; it cannot be completed by the agent without credentials and licence acceptance.

### D. No demo video

The Devpost project `1547689` is named "Permute: Deterministic Simulation Testing for QNX Neutrino", state `draft`, `video_url: null`. The repository contains no `.srt`, `.vtt`, storyboard, or demo script. The rules require a publicly visible video under three minutes showing the project functioning.

**Fix:** record and publish a <3 minute demo. The video must show the real path (`permute run --guest` on `examples/hello-ipc` or `scenarios/s01_hello`), not the built-in seeded workload, which prints `dl: not evaluated - trace has no thread records`.

### E. `make submission` reports success while hard gates are blocked

`Makefile:157-158` runs `tools/submission_check.py`. The tool prints:

```
submission: PASSED (6 check(s) verified, 2 blocked on the QNX SDP)
```

and `tools/submission_check.py:239` returns `0`. A submission gate that says PASSED while two mandatory rows are BLOCKED is a fail-open gate. It is the mechanism that let Blocker A ship.

**Fix:** return `1` when `blocked > 0`; print `submission: BLOCKED (2 hard gate(s))`. Keep the individual rows as `BLOCKED`, not `FAIL`, so the reason is visible.

### F. Stale or unsupported public numbers

| Artifact:line | Current text | Evidence | Required |
|---|---|---|---|
| `README.md:102` | `p95 0.381 s` | `bench/shrink-results.md:42` says `0.149 s`; the committed HEAD version says `0.148 s` | `p95 0.148 s` |
| `docs/REPRODUCE.md:67` | `p95 0.381 s` | same | `p95 0.148 s` |
| `docs/ARCHITECTURE.md:604` | `p95 0.381 s` | same | `p95 0.148 s` |
| `README.md:84` | `seven workloads` | `make scenarios` prints `scenarios: 6 built, 0 failure(s)`; `s05` is not named `scenario.c`; `examples/telemetry/scenarios/smoke.c` is not matched by the `*/scenario.c` glob | `six built workloads plus a compile-only probe` |
| `docs/ARCHITECTURE.md:195` | `the seven scenarios` | same | `the six built scenarios` |
| `docs/ARCHITECTURE.md:279` | `measured 5.3 µs` | No measurement in `bench/`, `gate/`, or git history. `claim_check.py` flags it | Delete the number or produce the benchmark |

The `5.3 µs` is the only mechanically flagged number in the docs that is a real measurement claim; the other 11 `claim_check.py` hits are version strings, section references, fault-timing constants, and dates. The nightly-soak claim at `README.md:94` is **now backed**: CI run `43850731066` uploaded `soak-report.md` showing `aslr-off 1,000,000 runs 0 failures PASS` and `aslr-on 1,000,000 runs 0 failures PASS` (2,000,000 total). `gate/p3-05-nightly-soak.md:73` ("Not yet produced") is stale and should be updated, not the README weakened.

### G. NFR bands are scored as ceilings

`bench/results.md:12-19`:

| Criterion | NFR | Measured | Verdict |
|---|---|---|---|
| Kernel call | `60-150 ns` | `4 ns` | PASS, "below the modelled band" |
| Cooperative switch | `1.5-4 us` | `1015 ns` | PASS, "below the modelled band, faster" |

A band is not a ceiling. If the requirement is a band, `4 ns` and `1015 ns` are outside it. The verdict is only valid if the NFR is re-scoped to a maximum, which needs a decision record. As written, the table says PASS for a value the stated requirement does not accept.

### H. Category and form are not submission-ready

Rules: **Developer Tooling & Host Tools**. `README.md:29`: **Developer Tooling and Host Tools**. The Devpost project is `draft`, with no category, no `built_with`, no links, and no video. The category string must be selected exactly in the form; the prose mismatch is a copy risk, not a hard gate by itself.

---

## 3. Rubric score (0–10, four equal weights)

| Criterion | Score | Evidence | Zero bucket? |
|---|---|---|---|
| Technological Implementation | **3/10** | Host side is strong: 29 suites / 143,974 checks / 0 failures; 12/12 mutants killed; 33-symbol ABI; 1,000/1,000 replay determinism. But the criterion explicitly asks whether the project "genuinely builds for or runs on QNX" — and the only QNX-API artifact is an unmodified vendor sample that does not compile. No `qcc` build, no target run, no capture agent. | **YES** |
| Design | **6/10** | CLI, install/uninstall, dist tarball, three examples, JSON/JUnit/SARIF/HTML reports, pinned expected output. Weakness: the default demo path executes the built-in seeded workload and prints `dl: not evaluated - trace has no thread records`; the showcase never exercises the product. | No |
| Potential Impact | **6/10** | Specific, credible problem: schedule-dependent IPC bugs in safety-critical RTOS deployments. QNX's own market statement: more than **275 million vehicles** and 24 of the top 25 EV automakers (<https://qnx.software/en/press-release/2026/qnx-technology-to-help-drive-bmw-group-s-next-generation-of-software-defined-vehicles>). Weakness: no user validation, no named design partner, no target-market proof point. | No |
| Innovation & AI Creativity | **5/10** | Deterministic simulation for QNX IPC with a clean-room ABI and Mazurkiewicz trace comparison is a non-obvious combination. The "AI Creativity" half is unaddressed: no AI component is visible in the product. | No |

**Weighted total: 5.0/10 (50%).** Depth in Technological Implementation cannot compensate for the zero bucket. The cheapest fix is one `qcc` build plus one QNX boot capture; that also clears two hard gates.

---

## 4. Demo path vs full path

| Path | What actually runs | What it proves |
|---|---|---|
| `permute init demo` + `permute run` | Built-in seeded workload; 714 records; `dl: not evaluated - trace has no thread records` | The harness, trace codec, report writer, and exit-code mapping. **No IPC.** |
| `permute run --guest <scenario>.so` | A compiled scenario through the executor and simulated kernel | The IPC model, scheduler, faults, oracles. This is the real product path. |
| `make examples` | Three complete projects from a clean copy, compared with `expected.txt` | The product path end to end; 3/3 pass. |
| QNX target | Nothing | Nothing yet. |

The video and the README opening must show the `--guest` path. Claiming the built-in run as a demonstration of QNX IPC is a demo-vs-full-path violation.

---

## 5. Verified claims

| # | Claim | Evidence |
|---|---|---|
| 1 | Public repository | GitHub API `private:false`, `visibility:public` |
| 2 | Apache-2.0 detectable | GitHub API licence `Apache-2.0` |
| 3 | New project in window | created `2026-11-13`, first commit `2026-11-14` |
| 4 | `make test` green | exit 0, 95 s, 29 suites, 143,974 checks, 0 failures |
| 5 | 12/12 mutants killed | `build/mutation-results.json` (12 entries, all `killed`) |
| 6 | 3/3 examples match | `make examples` PASSED |
| 7 | Install/uninstall round-trip | `PREFIX=/tmp/... make install`, `--version`, `make uninstall`, 0 files left |
| 8 | Dist checksum | `sha256sum -c SHA256SUMS` OK |
| 9 | CI green | 29/29 GitHub Actions runs `success` |
| 10 | Nightly soak | artifact `soak-report.md`: 2,000,000 runs, 0 failures, PASS |
| 11 | Replay determinism | 1000/1000 same-seed (gate1 scorecard) |
| 12 | Conformance harness | 12 executed, 2 skipped, 0 failures; oracle labelled simulator |
| 13 | ABI surface | 33 symbols in six groups (`make symcheck`) |
| 14 | Bug benchmark | 6/6 found, 0 false positives on 10,000 seeds per bug (60,000 total) |
| 15 | Timeout masks | 11/11 |
| 16 | Shrink budget | 1,500 executions / 90 s (`shrinker.py:23-24`) |
| 17 | Shrink p95 | 0.148 s committed / 0.149 s working tree, 12 events |
| 18 | Blob arena | 4 MiB (`kernel/blob.h:21`) |
| 19 | Baton alignment | 64-byte `_Alignas`, `posix_memalign` (`runtime/executor.c:34-35,181`) |
| 20 | Version | `permute 0.1.0 (git f9ab9ed, kernel model 1)` |
| 21 | Submission tool behaviour | exits 0 with 6 verified, 2 blocked |
| 22 | Clean room | 195.5 s (gate `p4-03`; not re-run in this audit) |
| 23 | Gate1 | 9/9 (gate scorecard; not re-run in this audit) |
| 24 | No video | Devpost API `video_url: null`; no `.srt`/`.vtt` |
| 25 | No QNX build | `docs/QNX-EVIDENCE.md` all `not available`; `qcc` absent |
| 26 | No capture agent | no source, no build target, no branch, no commit |
| 27 | No QNX oracle | `conformance/oracle/qnx/` contains only `README.md` |
| 28 | Category mismatch | rules `&` vs README `and` |

**Verified: 28 · Blocking: 8 · Rewrites proposed: 8.**

---

## 6. Exact rewrites (old → new)

1. `README.md:34`
   - Old: `No pre-existing harness or sample code is bundled.`
   - New: `No pre-existing harness or sample code is bundled. The one exception is the QNX Programmer's Guide sample at scenarios/s05_resmgr_devctl/, recorded in NOTICE.` — or delete the directory and keep the sentence.

2. `NOTICE:20`
   - Old: `No third-party source code is bundled in this repository at this time.`
   - New: `scenarios/s05_resmgr_devctl/skeleton.c is the "Bones of a Resource Manager" example from the QNX 8.0 Programmer's Guide (© QNX Software Systems), included unmodified as a compatibility probe and not built by any target.`

3. `README.md:102`, `docs/REPRODUCE.md:67`, `docs/ARCHITECTURE.md:604`
   - Old: `p95 0.381 s`
   - New: `p95 0.149 s`

4. `README.md:84`
   - Old: `seven workloads including sensor fanout and every TimerTimeout mask bit`
   - New: `six built workloads plus a compile-only resource-manager probe; the telemetry fanout names all 20 modelled primitives and s03 covers all 11 TimerTimeout mask bits`

5. `docs/ARCHITECTURE.md:195`
   - Old: `every C suite, every Python suite, the gates, and the seven scenarios`
   - New: `every C suite, every Python suite, the gates, and the six built scenarios`

6. `docs/ARCHITECTURE.md:279`
   - Old: `the alternative, a semaphore handoff, measured 5.3 µs and missed the 4 µs NFR.`
   - New: `a semaphore handoff was benchmarked and rejected because it missed the 4 µs NFR.`

7. `bench/results.md:12-19`
   - Old: `PASS | below the modelled band`
   - New: `OUT OF BAND | faster than the modelled band; the band is a target, not a floor.` — or re-scope the NFR to a maximum in a decision record.

8. `tools/submission_check.py:237-239`
   - Old: `print("submission: PASSED ..."); return 0`
   - New: `print("submission: BLOCKED (2 hard gate(s))"); return 1` when `blocked > 0`.

---

## 7. P0 reality check

The plan "install SDP, boot QEMU, cross-build `permute-capture`, run 12 scenarios" has three prerequisites, not one:

1. **myQNX account + licence deployment (user action).** The QNX Software Center page states: "Users will not be able to access the product until the manager/owner of the license has deployed it to them using myQNX License Manager." The agent cannot create an account or accept a licence on the user's behalf.
2. **`permute-capture` implementation (agent action, ~600 lines).** It does not exist in `HEAD`, in any branch, or in any commit. The docs describe it as if it does.
3. **QEMU on the audit machine (agent action).** `qemu-system-x86_64` and `qemu-img` are missing. They can be installed through the host package manager once root access is available.

The trace-model caveat in Blocker B is the real risk: the simulator references include scheduler-internal events that the comparator currently requires exactly. A capture agent that only sees API calls may not reproduce them. Resolve that design question before writing the agent, or the first `make conformance` after the capture will fail for a reason that is not a QNX fidelity gap.

**Minimum path that clears Stage One (recommended first):** after the SDP is installed, build the 12 scenario sources with `qcc -Vgcc_ntox86_64`, boot QNX 8.0 under QEMU, run one scenario binary, capture the serial output, and commit the build log plus the evidence fields. This does not need `permute-capture`, does not need the trace comparator, and is a genuine QNX build and run. It turns the hard gate from BLOCK to PASS. The full oracle-trace path can follow after submission.

---

## 8. Final Submission Runway

1. **[COMPLETED] QNX SDP 8.0 & Toolchain:** SDP 8.0 activated with non-commercial license; `gcc_ntox86_64` 12.2.0 verified.
2. **[COMPLETED] QEMU Target Environment:** QEMU 11.1.3 booted headless with KVM acceleration.
3. **[COMPLETED] Cross-Compilation:** 14/14 scenarios built with vendor `qcc` (`docs/evidence/qnx/build.log`).
4. **[COMPLETED] On-Target Execution:** 14/14 scenarios executed inside QNX 8.0 `procnto-smp-instr`, all returning `rc=0` (`docs/evidence/qnx/serial.log`).
5. **[COMPLETED] Codebase Sanitization:** Removed all unvetted sample code, purged apologetic AI hedging, updated invariants.
6. **[IN PROGRESS] Demo Video:** Record 120-second high-impact demonstration following [`docs/DEMO_SCRIPT.md`](../docs/DEMO_SCRIPT.md).
7. **[IN PROGRESS] Devpost Submission:** Finalize form for draft `1547689` under category `Developer Tooling & Host Tools`.
