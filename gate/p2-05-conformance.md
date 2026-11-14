<!-- SPDX-License-Identifier: Apache-2.0 -->

# P2-05 report — conformance harness

**Date:** 2026-11-14
**Scope:** dual-target scenario sources, canonical trace format, Mazurkiewicz comparator, committed references, `make conformance`, and the timeout-mask check.
**Result:** **12 scenarios execute, 2 skipped, 0 failures**; **11/11** TimerTimeout mask bits. The committed references are simulator output — **this is not yet QNX evidence**, and the report says so.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `conformance/scenarios/c01..c15` | 14 dual-target sources; `__QNXNTO__` selects vendor headers, a target `main` calls `permute_main` |
| `conformance/tracecmp.py` | canonicaliser, causal graph, canonical linear extension, edge-set comparison, CLI |
| `conformance/runner/run_conformance.py` | build, run, canonicalise, compare, write the report |
| `conformance/oracle/simulator/` | 12 committed canonical traces and outcomes |
| `conformance/oracle/qnx/` | empty; the capture procedure and manifest format |
| `conformance/ALLOWED-DIFFERENCES.md` | every permitted divergence, per field |
| `conformance/REPORT.md` | generated; states the oracle source |
| `docs/CONFORMANCE.md` | status, set, procedure, what remains for G5 |

`make conformance` runs the 9 comparator unit checks, builds and runs the set,
compares, checks the mask bits, and exits non-zero on any mismatch.

---

## 2. The gate, measured

```
test_conformance: 9 checks, 0 failure(s)
timer mask bits: 11/11 (CLEAN)
conformance: 12 executed, 2 skipped, 0 failure(s);
             mandatory 10/12 executed; oracle: simulator reference
```

| ID | Behaviour | Result |
|---|---|---|
| C01 | request/reply, 1,000 messages | equivalent (9,006 events) |
| C02 | queue ordered 30, 20, 10 | equivalent |
| C03 | FIFO within equal priority | equivalent |
| C04 | pulse and message both delivered | equivalent |
| C05 | pulse while processing | equivalent |
| C06 | pulse-only waiter ignores a message | equivalent |
| C07 | timeout, unblock pulse, late reply ESRCH | equivalent |
| C09 | reply after client exit | equivalent |
| C10 | cancelled sender leaves no message | equivalent |
| C11 | server death with reply-blocked clients | **skipped**: one guest process |
| C12 | message PI and `_NTO_CHF_FIXED_PRIORITY` | equivalent |
| C13 | mutex PI chain | **skipped**: mutex shim pending |
| C14 | round-robin alternation | equivalent |
| C15 | `ChannelDestroy` unblocks senders | equivalent |

### What the numbers do not say

The committed references were produced by the simulator itself
(`--record`). Therefore:

- the 12/12 executed match is a **regression result**, not target fidelity;
- the mandatory set is **10/12 executed**, because C11 needs multi-process
  death and C13 needs the mutex shim;
- FR-11.1's `qcc` build has not been attempted (no SDP), so "dual-target" is
  proven only on the host side;
- G5 is **not met**. The physical SDP capture is documented in
  `docs/CONFORMANCE.md` and `conformance/oracle/qnx/README.md` as external
  input, and no capture is committed.

The runner refuses to call this QNX evidence: the report header says
`Oracle source: simulator reference (not a QNX capture)`, and it selects
`oracle/qnx/` automatically once a manifest is present.

---

## 3. Canonical format and comparator

One JSONL event per line, schema `permute-canonical/1`:

- **Identifiers** become labels. Objects are scoped to the thread that created
  or used them, so reordering two independent channel creations cannot rename
  them (IMP-70). Threads are labelled by first appearance.
- **Payloads** become SHA-256 plus length; the bytes are compared, not the
  encoding.
- **Causal edges** are listed per event: program order within a thread,
  receive → reply by rcvid, send → receive by identical payload.
- **Timestamps** stay for evidence and are excluded from comparison unless
  `--strict-time` is passed.

Equivalence checks both the canonical linear extension and the causal edge
set (IMP-71). The extension alone can hide a missing edge when the content
sort happens to produce the same order; the edge multiset closes that hole.
The 9 unit checks cover: identical traces, independent reordering, payload
difference, reply-before-receive, event count, timestamp policy, and errno.

---

## 4. Mask bits

The runner builds and runs `scenarios/s03_timer_timeout/scenario.c`, requires
`CLEAN`, and counts the 11 `_NTO_TIMEOUT_*` names in the source. The suite also
asserts each bit individually, and the executor check runs s03 end to end.

---

## 5. Boundaries

| Gap | Reason |
|---|---|
| No QNX reference traces | SDP and QEMU are external; the capture procedure is documented, not executed (LIMITS #59) |
| C11 skipped | The simulator runs one guest process; server death needs process teardown (LIMITS #60) |
| C13 skipped | Mutex shim pending (LIMITS #37, #60) |
| Thread labels by first appearance | A target creating independent threads in the opposite order can swap labels (LIMITS #61) |
| `qcc` build unproven | No SDP (LIMITS #62) |
| C14 is cooperative | Guest code is not preempted between shim calls; the QNX run is authoritative (LIMITS #63) |
| Negative control, mutation score | FR-11.5 needs the target; FR-11.6 is P2-06 |

---

## 6. Next: P2-06, mutation

The conformance pipeline now has a comparator that can tell a real causal
difference from independent reordering. P2-06 uses it differently: twelve
seeded kernel defects, each of which at least one scenario must catch. The
scenario set is in place, so the mutants can reuse it.
