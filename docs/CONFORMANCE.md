<!-- SPDX-License-Identifier: Apache-2.0 -->

# Conformance

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** **not started — blocked on the QNX SDP.** No scenario has been compared against a real target. The model-level tests are green; that is a different claim.

---

## What conformance means here

A conformance scenario is one source file built twice: once for the simulator
and once with the vendor toolchain for QNX 8.0. Both runs produce a canonical
trace. The traces are compared field by field, with allowed differences listed
explicitly ([`LIMITS.md`](LIMITS.md) §4).

Passing the model's own tests is necessary but not sufficient. Until the same
scenario has run on a target, every fidelity statement in this repository is a
claim about the model, not evidence about QNX.

## The mandatory set

Twelve scenarios: **C01–C05, C07, C09–C14**. None has run yet.

| ID | Behaviour | Topology | Mode | Status |
|---|---|---|---|---|
| C01 | Request/reply, 1,000 messages | 1 client, 1 server | exact | not run |
| C02 | Priority queue ordering 10/20/30 | 3 clients, 1 server | exact | not run |
| C03 | FIFO within equal priority | 3 clients at 10 | exact | not run |
| C04 | Pulse vs message while blocked in `MsgReceive` | 1 server | exact | not run |
| C05 | Pulse while the server is processing | 1 + 1 | exact | not run |
| C07 | Client timeout, late reply, `_NTO_CHF_UNBLOCK`, `ESRCH` | 1 + 1 | exact outcome | not run |
| C09 | Reply after client death | 1 + 1 | exact | not run |
| C10 | Client death before receive | 1 + 1 | exact | not run |
| C11 | Server death while clients are reply-blocked | 1 + 3 | exact | not run |
| C12 | Message-driven PI, plus `_NTO_CHF_FIXED_PRIORITY` | 1 server, clients 5/20 | exact | not run |
| C13 | Mutex PI chain low/medium/high | 3 threads | exact | not run |
| C14 | RR timeslice, 2 threads, 20 ms | 2 threads | ±1 alternation | not run |

Optional, tier 2: C06, C08, C15, C16.

## What is verified today

| Claim | Evidence |
|---|---|
| Message path, queue ordering, truncation, error replies | `make test` → `test_ipc` |
| Pulses, local coids, `EFAULT`, secure events, disconnect | `test_pulse` |
| Ready queue, RR quantum, `SchedYield` | `test_sched` |
| Message-driven and mutex PI, ceilings, chains | `test_pi` |
| Timeout masks, latch, unblock contract, POSIX timers | `test_time` |
| Trace integrity, truncation salvage, exports | `test_codec` |
| Determinism, replay, manifest refusal | `test_replay`, `make determinism` |

These are model tests. They prove the implementation matches its specification;
they cannot prove the specification matches QNX.

## Re-capture procedure

Target capture needs the QNX SDP and a bootable QEMU image. Both are external
dependencies and are **not yet present**:

1. Build the capture agent with `qcc` for the target.
2. Build each scenario source for the target and run it under QEMU.
3. Record canonical traces; commit them under `conformance/traces/`.
4. Build the same sources for the simulator and compare with
   `tools/tracecmp` (a later task).
5. Record every observed difference in `conformance/ALLOWED-DIFFERENCES.md`
   with a reason a reviewer can check.

The negative control is part of the procedure: an intentionally racy scenario
must produce different traces across repeated target runs. If it does not, the
comparison tool cannot detect nondeterminism and the conformance job fails
regardless of the other results.

## What this document will say when it is finished

Outcome class, errno, which thread unblocks, message bytes, receive-id
validity, per-thread event order, send→receive→reply edges, event counts,
effective priority at receive, and pulse code/value. Those must match. Timing,
addresses, identifier values, and the order of independent events are allowed
to differ, and each allowance is listed in [`LIMITS.md`](LIMITS.md) §4.
