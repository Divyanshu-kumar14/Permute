<!-- SPDX-License-Identifier: Apache-2.0 -->

# P2-01 report — fault engine

**Date:** 2026-11-14
**Scope:** the fault plan (`kernel/fault.c`), its decision domain (`D_FAULT`), rule parsing and validation (`cli/permute/config.py`), and application at send / receive / reply / pulse in the guest bridge.
**Result:** **124 unit checks + 20 end-to-end checks, 0 failures**. The FR-4.3 property holds at three levels: the `D_SCHED` stream is untouched after 40,000 fault draws, a zero-probability rule leaves the trace byte-identical, and the pre-existing s01 digest is unchanged by the new call sites. `make ci` green.

---

## 1. The gate, measured

> Adding a fault site does not change a scheduling-only trace digest. (FR-4.3)

| Level | What was compared | Result |
|---|---|---|
| Historical | s01 digest before the fault engine, and with four fault sites instrumented but no plan | `19e159f35bbd8d08` both times |
| Unit | 40,000 draws across all four sites, then `D_SCHED` values 0–63 against a fresh stream | identical, `counter[D_SCHED] == 0` |
| End to end | full trace with `send:drop@0` armed on every send vs. no fault config | identical digest, 30,000 records both |

The end-to-end comparison is stronger than "scheduling-only": no fault record is
emitted for a rule that does not fire, so the whole trace — not just its
scheduling subset — is byte-identical. `test_faults_do_not_touch_scheduling`
proves the narrower claim directly on the decision stream.

---

## 2. Observable behaviour

s01 (10,000 round trips), seed 1:

| Config | Outcome | Records | Digest |
|---|---|---|---|
| none | CLEAN | 30,000 | `19e159f35bbd8d08` |
| `send:drop@0` | CLEAN | 30,000 | `19e159f35bbd8d08` |
| `send:timeout@1` | DEADLOCK | 1 | `65c9839926e413ae` |
| `send:delay@1:1000` | CLEAN | 40,000 | `11974f04e78d5c3a` |
| `send:duplicate@1` | DEADLOCK | 30,002 | `db7a233569d4ea7e` |

`send:timeout@1` is the clearest one: the very first send fails with
`ETIMEDOUT`, the scenario's guest-side check gives up, and the run is reported
as a deadlock rather than a clean pass. `send:duplicate@1` deadlocks the
protocol in a different way: every message is delivered twice, so the server
reaches its expected count of 10,000 after half the client's loop, exits, and
the client's remaining send is never received. The trace shows it plainly —
10,000 `RECV`, 10,000 `REPLY`, 5,001 `SEND`, and a final send with no receiver.

Other checks in `tests/python/test_fault.py`:

- `pulse:drop@1` on s04 (which loses one pulse on purpose) is no longer
  reported as the expected `EFAULT` path.
- An unparsable rule is a configuration error (`exit 7` from `permute-run`,
  `P1003` from the CLI), never a silent no-op.
- `permute run` with a non-empty `[faults] inject` and no `--guest` exits 2
  with `P1003`: the rule would otherwise be ignored, since the built-in
  scenario does not cross the bridge.
- Every trace written under a fault still verifies with the reader.

---

## 3. Design

A plan is a fixed array of rules plus its own `permute_stream_t`:

```
struct { rules[32]; count; stream; decisions; fires; }
```

`permute_fault_decide` iterates the rules, and for each rule matching the site
draws **one** `D_FAULT` value and compares `value % 1e6` to the rule's
numerator. An unarmed site draws nothing. That is the property the gate needs:
instrumentation is free until a rule is armed, and no fault decision can
influence the scheduling stream because it never touches it.

Rules parse without floating point (`send:drop@0.01` → 10,000 ppm), so the
same text produces the same number on every host. The C parser and the Python
validator implement the same grammar; the Python side rejects the same inputs
at `permute init` / `permute run` time.

### Application per site

| Site | drop | duplicate | delay / reorder | timeout | kill |
|---|---|---|---|---|---|
| send | sender blocks in `SEND` with no message | send performed twice | wait, then send | `ETIMEDOUT` | thread terminates |
| recv | `EAGAIN`, queue untouched | not modelled | wait, then receive | `ETIMEDOUT` | terminates |
| reply | no reply; client stays blocked | second reply fails `ESRCH` | wait, then reply | no reply | terminates |
| pulse | discarded | sent twice | wait, then send | discarded | terminates |

`DELAY` and `REORDER` share an implementation (`virtual_delay`): the caller
sleeps on the virtual clock, so a later operation can overtake. The model does
not claim to reorder the queue itself. `KILL` takes effect at the target's next
shim boundary, the same constraint as `pthread_cancel`.

---

## 4. What is not modelled, and why

| Gap | Reason |
|---|---|
| Duplicate at receive | Needs a second kernel-owned copy and a second rcvid, which changes the reply contract. Named in LIMITS #41 rather than approximated |
| Faults on the built-in scenario | It does not cross the bridge; the CLI rejects the combination instead of ignoring it (LIMITS #42) |
| Asynchronous kill mid-instruction | Same constraint as `pthread_cancel`; the scenario-level effect is identical (LIMITS #43) |
| Reorder as a queue permutation | Only the observable effect — a later operation overtaking — is modelled |

---

## 5. Verification

```
test_fault: 124 checks, 0 failure(s)
test_fault_e2e: 20 checks, 0 failure(s)
kernel/fault.c: 98/110 lines = 89.1%
make ci: green (unit, codec, replay, gate, scenarios, executor, faults,
                determinism, coverage, quickstart, ci-check)
make asan: test_fault green
```

---

## 6. Next: P2-02, oracles

The fault engine exists so behaviour can be broken deterministically; the
oracles decide whether the resulting behaviour is a bug. P2-02 needs seven
oracles with a positive and negative fixture each, and a deadlock report that
prints a cycle (`A(REPLY rcvid) -> B(REPLY rcvid) -> A`). The trace currently
carries SEND/RECV/REPLY/ERROR/PULSE/EVENT/TIMER/FAULT records, which is the
input the deadlock and `lost_pulse` oracles need.
