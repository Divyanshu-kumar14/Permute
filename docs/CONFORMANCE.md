<!-- SPDX-License-Identifier: Apache-2.0 -->

# Conformance

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** **harness operational; QNX capture pending the SDP.** Twelve scenarios execute and match committed simulator references; two mandatory scenarios are compile-only. This is **not** evidence of QNX fidelity yet.

---

## What conformance means here

A conformance scenario is one source file built twice: once for the simulator
and once with the vendor toolchain for QNX 8.0. Both runs produce a canonical
trace. The traces are compared as Mazurkiewicz traces: the same events with the
same causal partial order, independent events allowed to move.

The current state is deliberately split:

| Claim | Evidence | Honest status |
|---|---|---|
| The sources are dual-target | `__QNXNTO__` selects vendor headers; `permute_main` is called by a target `main` | host build proven; `qcc` build **not attempted** (no SDP) |
| The canonicaliser and comparator work | 12 scenarios, 9 unit checks, committed references | proven on the simulator |
| The scenarios match QNX | `conformance/oracle/qnx/` | **empty**; capture procedure documented, not executed |
| 12/12 outcome-class match | `make conformance` | 12 executed match **simulator references**; 2 mandatory scenarios skipped |

Do not read the green report as G5. G5 needs the QNX captures.

---

## The set

| ID | Behaviour | Status |
|---|---|---|
| C01 | request/reply, 1,000 messages | executed |
| C02 | receive queue ordered 30, 20, 10 | executed |
| C03 | FIFO within equal priority | executed |
| C04 | pulse and message both delivered to a blocked receive | executed |
| C05 | pulse queued while the server processes | executed |
| C06 | pulse-only waiter ignores a queued message (optional) | executed |
| C07 | timeout, `_NTO_CHF_UNBLOCK` pulse, late reply `ESRCH` | executed |
| C09 | reply after the client exited | executed |
| C10 | cancelled sender leaves no message | executed |
| C11 | server death with reply-blocked clients | **skipped**: the simulator runs one guest process |
| C12 | message-driven PI, plus `_NTO_CHF_FIXED_PRIORITY` | executed |
| C13 | mutex PI chain | **skipped**: the mutex shim is pending (LIMITS #37) |
| C14 | round-robin alternation | executed (cooperative; see LIMITS #63) |
| C15 | `ChannelDestroy` unblocks senders with `ESRCH` (optional) | executed |

The TimerTimeout mask check runs the existing `s03_timer_timeout` source and
reports **11/11** bits, with a `CLEAN` outcome.

---

## Canonical trace

JSONL, one event per line, schema `permute-canonical/1`:

```json
{"kind":"SEND","src":"t1","dst":null,"obj":"co@t1:1","aux":8,
 "payload":"3a7f...:8","t_ns":0,"causal":[],"schema":"permute-canonical/1"}
```

- **Identifiers** are labels, not numbers. Objects are scoped to the thread
  that created or used them, so two independent channels cannot swap names
  when their creation events are reordered. Threads are labelled by first
  appearance.
- **Payloads** are SHA-256 plus length. The bytes are compared, not the
  encoding.
- **Causal edges** are listed per event as indices into the canonical order:
  program order within a thread, receive → reply by rcvid, send → receive by
  identical payload.
- **Timestamps** are present for evidence and excluded from comparison unless
  `--strict-time` is given.

`python3 conformance/tracecmp.py A B` prints the verdict; `--json` is
machine-readable; it accepts `.pmt` or canonical `.jsonl`.

The comparator checks both the canonical linear extension and the causal edge
set. The extension alone can hide a missing edge when the content sort happens
to produce the same order, so the edge multiset is compared too.

---

## Running

```sh
make conformance          # build, run, compare
make conformance-record   # regenerate the simulator references
```

The runner selects `conformance/oracle/qnx/` when it has a manifest, otherwise
`conformance/oracle/simulator/`. It writes `conformance/REPORT.md` and exits
non-zero on any mismatch, missing reference, bad trace, or mask-bit shortfall.
Skipped scenarios are named with their reason and are not counted as passes.

---

## Capturing the real oracle

The physical step is external input, documented in
 [`conformance/oracle/qnx/README.md`](../conformance/oracle/qnx/README.md):

1. Install the QNX 8.0 SDP and a bootable QEMU image.
2. Build the capture agent with `qcc` and each scenario source for the target.
3. Run each under QEMU; the agent writes canonical JSONL and the outcome class.
4. Commit them under `conformance/oracle/qnx/` with a `manifest.toml`.
5. Run `make conformance`; the report then says `oracle: QNX capture`.

Until then the committed references are simulator output. They exist so the
pipeline is live and regressions are caught; they are labelled as such in every
report and are never presented as target evidence.

---

## Allowed differences

[`conformance/ALLOWED-DIFFERENCES.md`](../conformance/ALLOWED-DIFFERENCES.md)
lists every permitted divergence per field. Timing, identifier values, and the
order of independent events are allowed; outcome class, errno, payloads, causal
edges, per-thread order, event counts, and effective priorities are not.

One canonicalisation limit is stated rather than hidden: threads are labelled
by first appearance, so a target that creates two independent threads in the
opposite order can swap their labels even with isomorphic partial orders
(LIMITS #61).

---

## What remains for G5

- QNX captures for the 12 mandatory scenarios (needs the SDP).
- A `qcc` build of all sources, proving FR-11.1's second target.
- The negative control: a deliberately racy scenario must produce different
  traces across repeated QNX runs (FR-11.5).
- C11 and C13 on the target, and an execution path in the simulator for C11
  (multi-process) and C13 (mutex shim).
- Record any divergence in `ALLOWED-DIFFERENCES.md` with a reason.
