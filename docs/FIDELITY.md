<!-- SPDX-License-Identifier: Apache-2.0 -->

# Fidelity

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** **host fidelity proven; target fidelity unproven.** The harness, determinism and mutation results are measured. The physical QNX 8.0 capture has not been executed; G5 is open.
**Companion documents:** [`CONFORMANCE.md`](CONFORMANCE.md) (the harness), [`LIMITS.md`](LIMITS.md) (what is not modelled), [`DETERMINISM.md`](DETERMINISM.md) (the invariants), [`DECISIONS.md`](DECISIONS.md) (why).

Fidelity is a claim about a model matching a target. This document separates
what is measured from what is assumed, and names the test that would falsify
each claim. It is deliberately shorter than `ARCHITECTURE.md`: architecture says
how the model works, this says how much of it is proven.

---

## 1. The claim, in one paragraph

For the 12 mandatory conformance scenarios, the simulator and QNX 8.0 must
produce the same outcome class, the same errno, the same per-thread event order,
the same causal edges, the same event counts, the same message bytes, and the
same effective priority at receive. Independent events may move. Anything else
that differs must be listed in `conformance/ALLOWED-DIFFERENCES.md` with a
reason. The target is ≥ 10/12 strict trace equivalence and 12/12 outcome-class
equivalence, plus a mutation score of ≥ 10/12 and a negative control that
proves the comparator can see nondeterminism.

That is the whole claim. It is not a claim of completeness, of hardware timing,
or of correctness for untested behaviour.

---

## 2. The four tests

| Test | What it proves | Command | Status |
|---|---|---|---|
| Self-determinism | same seed, same build, same config → byte-identical trace, ASLR on or off | `make determinism` | **PASS**: 1,000/1,000 runs, one digest per seed, ≥ 95 distinct seeds |
| Cross-target conformance | the model matches the target on 12 scenarios | `make conformance` | **HARNESS PASS, TARGET PENDING**: 12 executed against simulator references, 2 skipped; `conformance/oracle/qnx/` is empty |
| Negative control | the comparator can detect a genuinely racy program | `make negative-control` (P3-02) | **NOT RUN**: needs real QNX runs; a host run alone would not prove the oracle sees target nondeterminism |
| Model mutation | the suite catches seeded semantic defects | `make mutation` | **PASS**: 12/12 killed |

Read the conformance row carefully. A green `make conformance` today says the
scenarios still match the committed simulator output. It does **not** say they
match QNX. The runner labels the oracle source in every report, and prefers
`conformance/oracle/qnx/` when a manifest exists.

---

## 3. Support by requirement group

This is the anchor table: every FR group names the document that specifies it
and the command that evidences it. “Partial” lists the sub-requirements that are
not met, so a reader does not have to diff the PRD against the code.

| Group | Doc anchor | Evidence | Status |
|---|---|---|---|
| **FR-2** interposition | [`ABI.md`](ABI.md) §2, §6, §7; [`ARCHITECTURE.md`](ARCHITECTURE.md) §2.4 | `make symcheck`, `make negative`, `make vendordup` | **Partial**: strong-symbol shadowing, audit, atomic groups and duplicate detection are met. FR-2.5 (`--wrap`/`LD_PRELOAD` fallbacks) is not implemented (LIMITS #87) |
| **FR-3** kernel, scheduler, time | [`KERNEL-CONTRACT.md`](KERNEL-CONTRACT.md) §3; [`ARCHITECTURE.md`](ARCHITECTURE.md) §3–§7; [`LIMITS.md`](LIMITS.md) §2 | `make unit`, `make executor`, `make conformance` | **Partial**: core IPC, scheduler, PI, timers and deadlock are met and exercised. Missing: vector/MsgInfo variants (FR-3.2), `MsgSendPulsePtr` (FR-3.5), guest-visible mutex PI (FR-3.18, C13 skipped), `timer_create` (FR-3.24), `pthread_setschedprio` (FR-3.20). FR-3.9 is moot because resource managers are dropped |
| **FR-4** determinism | [`DETERMINISM.md`](DETERMINISM.md); [`ARCHITECTURE.md`](ARCHITECTURE.md) §11 | `make determinism`, `make shadow`, `make symcheck`, `make verify` | **Partial**: one CPU, counter-based decisions, virtual clock, identity, allocator and separate guest globals are met. Signals as scheduled events (FR-4.10) and read-only file I/O (FR-4.12) are not implemented; `EV_SPAWN` ordering is proven at load, not yet as a scheduled event (LIMITS #18) |
| **FR-5** exploration | [`ARCHITECTURE.md`](ARCHITECTURE.md) §9.3 | `make explore`, `bench/explore-compare.py` | **Met**: six mutators, novelty bitmap, corpus, escalation, shards; shard 2/4 is byte-identical to the full run; 74 vs 70 findings against the random baseline |
| **FR-6** log, replay, shrink | [`ARCHITECTURE.md`](ARCHITECTURE.md) §8, §9, §9.1, §9.4 | `make codec`, `make replay`, `make shrink`, `make verify` | **Partial**: log, chunk integrity, digest, manifest refusal, shrink order/budget/determinism and `--verify` are met. Replay does not yet compare violations, so exit 10 is reserved (LIMITS #17) |
| **FR-7** dispatch/resmgr | [`DECISIONS.md`](DECISIONS.md) §4.1; [`LIMITS.md`](LIMITS.md) §2.2, #81–#83 | `gate/p2-11-tier2-evaluation.md` | **Dropped**: kill criterion failed; FR-7.2/7.3 dropped, FR-7.1 has no consumer, FR-7.5 is a recorded deviation, `name_attach` is retained but not implemented |
| **FR-9** CLI and reporting | [`README.md`](../README.md), [`QUICKSTART.md`](../QUICKSTART.md), [`ARCHITECTURE.md`](ARCHITECTURE.md) §2.6 | `make cli`, `make report`, `make quickstart` | **Met** for the M items (init/build/run/replay, exit matrix, JSON report). The S items `doctor` (FR-9.6), `ls`/`merge`/`cov` and a complete `P####` catalogue are not built; `log` is an option on `show`, not a verb |
| **FR-10** oracles | [`ARCHITECTURE.md`](ARCHITECTURE.md) §10 | `make oracles`, `make faults` | **Met**: seven post-hoc predicates, 14 fixtures, deadlock cycle rendering, PI classification, starvation negative fixture, TOML severity |
| **FR-11** conformance | [`CONFORMANCE.md`](CONFORMANCE.md); `conformance/ALLOWED-DIFFERENCES.md` | `make conformance`, `make mutation`, `make bench-bugs` | **Partial**: harness, canonicaliser, mutation 12/12 and the bug benchmark 6/6 with 0 false positives are met. QNX capture (FR-11.1/11.4), negative control (FR-11.5) and capture-backed allowed differences (FR-11.7) are pending the SDP |

The traceability gate is therefore satisfied in the narrow sense — every group
has an anchor and an evidence command — but three groups carry explicit
partials. Those partials are the honest scope of v0.1, not omissions in this
table.

---

## 4. What is proven on the host

| Claim | Evidence |
|---|---|
| One guest thread runs at a time | executor assert; `make executor` (32 checks, 6 scenarios) |
| A seed is a pure function of `(seed, config, build)` | `make determinism` 1,000/1,000; `make shadow` with ASLR on/off |
| The trace is self-verifying | CRC32C per chunk, BLAKE3-64 chain, `make codec`, `make replay` |
| The shrinker is deterministic and bounded | `make shrink` 37 checks; 10,000 → ≤ 20 events; `proven`/`budget_limited` |
| The oracles detect the defects they name | `make oracles` 34 checks, 14 fixtures |
| The suite catches twelve semantic mutations | `make mutation` 12/12 |
| Six real bugs are found with no false positives | `make bench-bugs` 6/6, 0 FP |
| The reports validate | `make report` 32 checks; SARIF against the OASIS schema; offline HTML |
| The gated numbers hold on the reference machine | `make bench`, `make gate1` 9/9 |

---

## 5. What is not proven

| Claim | Why not | What closes it |
|---|---|---|
| The model matches QNX on any scenario | no SDP, no target capture; committed references are simulator output (LIMITS #59) | P3-01: `qcc` build + QEMU capture, 12 scenarios |
| The comparator can see target nondeterminism | a negative control has not run on the target | P3-02 |
| Any allowed difference is the right one | differences are specified from documentation, not observed at capture | P3-03 once captures exist |
| Signals, files, resource managers behave like the target | not modelled at all | out of scope; `LIMITS.md` §3 |
| Performance numbers describe the target | the simulator is host code; virtual time is not target time | never claimed |

---

## 6. Unverified assumptions carried forward

These are the claims that could still be wrong even after every host test is
green. Each one is a real risk, not a formality.

| Assumption | Impact if wrong | Resolution |
|---|---|---|
| QNX 8.0 headers declare the IPC surface as plain `extern` with no macros or inlines | interposition needs the fallback path, a scope increase | signature probe when an SDP is present (P0-09 follow-up) |
| Receive-time priority assignment across multiple channels is the maximum over pending messages | C12/C13 outcome changes; the model makes it a one-line change | two-channel capture |
| `resmgr_attr_t.msg_max_size` defaults to 2,048 | only affects the dropped Tier 2 error reproduction | no longer relevant (DECISIONS §4.1) |
| Pulse compression preserves delivery order and count | C04/C05 observable sequence | documented by the vendor; recorded as divergence #6 |
| `MsgReceivePulsePtr` is absent from the 8.0 library | an unimplemented API would be missing from the surface | verified against the library index; out of scope |

The full list, including source-document conflicts, is `LIMITS.md` §5 and PRD
§18.3.

---

## 7. Falsifiers

A fidelity claim that cannot fail is marketing. These are the outcomes that
would force a change:

1. **A same-seed digest mismatch** in any run. The model is nondeterministic;
   release blocker (exit 5, `DETERMINISM.md`).
2. **A conformance mismatch on a mandatory scenario** against a real capture.
   The model is wrong; fix the model or record an allowed difference with a
   reason.
3. **A negative control that produces identical target traces.** The comparator
   cannot detect nondeterminism; the conformance job must fail, not pass.
4. **A surviving mutant.** The suite has a hole; add the fixture that kills it.
5. **A bug-benchmark false positive.** The oracle is wrong; fix the predicate
   before adding more seeds.

The gate scripts implement 1, 4 and 5 today. 2 and 3 need the SDP.

---

## 8. What this document is not

- It is not a conformance report. That is [`CONFORMANCE.md`](CONFORMANCE.md)
  and `conformance/REPORT.md`.
- It is not a limit list. That is [`LIMITS.md`](LIMITS.md).
- It is not a proof. Passing seeds are evidence that paths were exercised, not
  that the model is complete.

The shortest accurate summary: **the harness is trustworthy; the model is
unverified against the target.** Both halves are required for G5, and the second
half is external input.
