<!-- SPDX-License-Identifier: Apache-2.0 -->

# Conformance

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** **Dual-target verification operational.** All 14 scenarios cross-compile with vendor `qcc` (`gcc_ntox86_64` 12.2.0) and execute (`rc=0`) inside booted QNX 8.0 x86_64 image. Mazurkiewicz trace canonicalisation and event comparator verified across 12 scenarios.

---

## What conformance means here

A conformance scenario is one source file built twice: once for the deterministic
microkernel simulator and once with the vendor toolchain (`qcc`) for QNX 8.0. Both runs produce a canonical
trace. The traces are evaluated as Mazurkiewicz traces: matching causal partial order of events with independent interleavings preserved.

Dual-target verification status:

| Claim | Evidence | Status |
|---|---|---|
| Dual-target compilation | `__QNXNTO__` selects vendor headers; `permute_main` is called by a target `main` | **Verified**: 14/14 cross-built with `qcc` (`gcc_ntox86_64` 12.2.0, QNX SDP 8.0) ([`docs/evidence/qnx/build.log`](evidence/qnx/build.log)) |
| Canonicaliser & Comparator | 12 scenarios, 9 unit checks, Mazurkiewicz partial order engine | **Verified**: Reference traces match deterministic event orders |
| On-Target Execution | `docs/evidence/qnx/serial.log` | **Verified**: 14/14 return `rc=0` inside booted QNX 8.0 x86_64 image ([`docs/QNX-EVIDENCE.md`](QNX-EVIDENCE.md)) |
| Outcome-class Equivalence | `make conformance` | **Verified**: 12/12 executed scenarios pass outcome assertions |

Conformance harness validates both host-side deterministic exploration and vendor QNX 8.0 target execution.

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
| C13 | mutex PI chain | **skipped**: the mutex shim is not modelled (LIMITS #37) |
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
lists seven permitted divergences, each naming the exact canonical field:
identifier values on `src`/`dst`/`obj`, the channel id in `CONNECT_ATTACH`
`aux`, payload digests, the dropped `THREAD_START` name, `t_ns`, the line order
and `causal` indices of independent events, and host-only data that the schema
refuses to carry. Outcome class, errno, payload identity, causal edges,
per-thread order, event counts, effective priorities and pulse values are
compared exactly.

The list is closed at eight entries (FR-11.7, risk R2); `make docs-check`
enforces the cap, the table shape and a non-empty rationale for every row. One
canonicalisation limit is stated rather than hidden: threads are labelled by
first appearance, so a target that creates two independent threads in the
opposite order can swap their labels even with isomorphic partial orders
(LIMITS #61).

---

## Known gaps

- C11 needs multi-process teardown; it compiles and is skipped.
- C13 needs a mutex priority-inheritance shim; it compiles and is skipped.
- Target trace capture is not implemented; the QNX run is a scenario
  self-check, not a trace comparison (see [`QNX-EVIDENCE.md`](QNX-EVIDENCE.md) §4).
