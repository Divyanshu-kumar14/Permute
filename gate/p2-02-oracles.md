<!-- SPDX-License-Identifier: Apache-2.0 -->

# P2-02 report — invariant oracles

**Date:** 2026-11-14
**Scope:** seven post-hoc predicates, per-kind TOML validation, trace instrumentation the predicates need, CLI integration and exit-code mapping, and a live wait-for detector used by `permute-run`.
**Result:** **14 fixtures (one positive and one negative per oracle) green**; 34 Python checks and 37 C checks; `make ci` green; the oracle coverage group is live at **77.0%**.

---

## 1. The gate, measured

```
test_oracle: 34 checks, 0 failure(s) (7 oracle(s), 2 fixtures each)
test_cycle:  37 checks, 0 failure(s)
```

The deadlock fixture renders exactly:

```
T1(REPLY 65537) -> T2(REPLY 65538) -> T1
```

`tests/python/test_oracle.py` asserts that string character for character, not
a substring or a shape. It is the form FR-10.3 names.

---

## 2. Fixtures

| Oracle | Positive | Negative |
|---|---|---|
| `deadlock` | two threads reply-blocked on each other | one is released |
| `lost_pulse` | `PULSE_RECV aux=1` (short-buffer drop) | pulse delivered |
| `unbounded_block` | `SEND` for 300 ms against a 250 ms budget | 100 ms block |
| `priority_inversion` | inheritance configured, owner ran at 10 for a prio-20 waiter | owner inherited prio 20 |
| `starvation` | equal-priority thread passed over at 4+ dispatches | higher-priority thread ready at every dispatch |
| `orphan_reply` | reply with no receive | matched `RECV` then `REPLY` |
| `queue_depth` | three blocked senders against a limit of two | two senders |

The PI fixture also runs the same inversion with inheritance *unconfigured*
and asserts the classification changes to `no_inheritance_configured` with
`info` severity — FR-10.4's distinction, tested rather than implied.

Real traces, not only fixtures:

| Run | Finding |
|---|---|
| s01, no faults | zero findings; every oracle is *supported* (not silently skipped) |
| s04 (short-buffer pulse) | `lost_pulse` fires |
| s01 + `reply:duplicate@1` | `orphan_reply` fires on the second reply (`ESRCH`) |
| s01 + `send:drop@1` | `deadlock` fires: both live threads blocked, no cycle |

---

## 3. Instrumentation added

The oracles could not be honest without records the bridge did not write. This
task added:

| Record | Emitted by | Why |
|---|---|---|
| `THREAD_START` / `THREAD_EXIT` | executor | live set, task name, base priority |
| `SCHED_PICK` | executor dispatch | winning thread and its effective priority |
| `BLOCK` / `UNBLOCK` | bridge on block, executor on dispatch | wait state, object, peer, duration |
| `CHANNEL_CREATE`, `CONNECT_ATTACH`, `CONNECT_DETACH` | bridge | channel/connection identity |
| `PULSE_RECV aux=1` | bridge | an explicit lost-pulse signal |
| `REPLY`/`ERROR aux=errno` | bridge | the kernel's verdict on the reply |

`BLOCK` has no `dst` in its wire schema, so the peer travels in a 4-byte blob
(IMP-57). One shared sequence counter serves the executor and the bridge: the
writer rejects a decreasing seq, and two counters would collide (IMP-56).

s01 grew from 30,000 to 90,015 records for 10,000 round trips. The trace still
verifies, and the digest is stable across runs.

---

## 4. Inversion, starvation, and queue depth: stated precisely

- **priority_inversion** reports `pi_not_applied` only when inheritance was
  configured and the owner still ran below the waiter (IMP-60). With no
  inheritance configured the finding is `info` and classified
  `no_inheritance_configured`. The mutex shim is still pending, so the
  predicate has record-stream fixtures, not a live one (LIMITS #44).
- **starvation** counts *dispatch decisions* at which a ready thread was
  passed over, not every trace record (IMP-59). Under FIFO a running thread is
  not preempted; counting events would call ordinary cooperative scheduling
  starvation. If any ready thread has a higher priority, the counter resets,
  which is FR-10.5 exactly.
- **queue_depth** identifies a channel by its coid. Channels have no names, and
  accepting `"db-conn"` would silently never match, so a name is rejected with
  a fix (IMP-61, LIMITS #45).
- **deadlock** requires every live thread blocked and none holding an armed
  timeout, and prints a cycle only when the peer edges contain one (IMP-58).
  This separates the dropped-message deadlock (no cycle) from the reply cycle.

---

## 5. Configuration

Per-kind schemas, validated by `cli/permute/config.py`:

| Kind | Parameters |
|---|---|
| `deadlock`, `lost_pulse`, `orphan_reply` | none |
| `unbounded_block` | `task`, `state`, `max_virtual_ms` (required) |
| `priority_inversion` | `max_virtual_us` (required) |
| `starvation` | `task`, `max_ready_gap_events` (required) |
| `queue_depth` | `channel`, `max_senders` (required) |

Rejected, with a stable code: an unknown key (`P1002`), a parameter belonging
to another kind, an unknown kind, a missing required parameter, a non-positive
budget, a channel name, a duplicate invariant id. `severity` is `error`,
`warn`, or `info`; only `error` changes the exit code.

CLI behaviour, observed:

```
  lost_pulse[lp] seed=0: T1 lost a pulse before delivery
3 seeds: 0 clean, 0 crash, 0 budget, 3 violation, 0 internal
exit=1
```

The seed is reclassified from clean to violation, the finding is in
`report.json` under `violations` and in `summary.json` under
`oracle_findings`, and `--json` stays valid JSON.

---

## 6. Live detector

`kernel/oracle.c` walks the kernel's blocked threads when the executor reports
no runnable thread, follows each `REPLY` wait to the thread holding the
message (and each `JOIN` to its target), and returns the lowest-id cycle.
`permute-run` prints `CYCLE=...`; the post-hoc oracle must derive the same
cycle from the trace, so the two views cross-check.

Unit-tested for a reply cycle, a join cycle, and the absence of a cycle when
one thread can run. No committed scenario constructs a live reply cycle yet
(LIMITS #47); the detector is exercised directly, and the post-hoc path is
exercised with the exact required rendering.

---

## 7. Verification

```
test_oracle:  34 checks, 0 failure(s)   (14 fixtures + 11 config + 9 real-trace)
test_cycle:   37 checks, 0 failure(s)
test_fault:  124 checks, 0 failure(s)
test_cli:     43 checks, 0 failure(s)
executor:     32 checks, 0 failure(s)
make ci:      green
make asan:    test_cycle green
coverage:     kernel 81.9%  shim 70.6%  log 92.7%  oracle 77.0%
```

---

## 8. Boundaries

- Mutex records do not exist in a run yet, so PI is fixture-tested (#44).
- The built-in workload has no thread records; thread-state oracles are
  reported as *not evaluated*, never as passing (#46).
- `lost_pulse` fires on the short-buffer drop; queue overflow is not modelled
  (#48).
- No scenario produces a live reply cycle (#47).
- Oracle evaluation is one pass per invariant; fusing the passes belongs with
  throughput work (#49).

---

## 9. Next: P2-03, the shrinker

P2-03 needs a failing event and a causal cone. The oracle now produces the
first half: a stable finding with `seq`, `t_ns`, and for deadlock the complete
cycle. The shrinker's gate — a 10,000-event failure reduced to ≤20 events with
the same invariant, byte-identical on repeat — is the next measurable step.
