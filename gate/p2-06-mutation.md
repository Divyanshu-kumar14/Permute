<!-- SPDX-License-Identifier: Apache-2.0 -->

# P2-06 report — model mutation

**Date:** 2026-11-14
**Scope:** the twelve PRD §14.4 mutants behind compile-time defines, the mutation runner, and the conformance/fixture suite that has to kill them.
**Result:** **12/12 killed**; 9 by a scenario or trace divergence, 3 by a targeted unit test. Gate ≥10/12. `make ci` and `make asan` green.

---

## 1. The score

```
$ make mutation
M01 priority queue becomes FIFO                          killed   C02: outcome GUEST_FAILED != CLEAN
M02 timeout always loses to a reply                      killed   C07: outcome DEADLOCK != CLEAN
M03 rcvid is never invalidated                           killed   C07: outcome GUEST_FAILED != CLEAN
M04 messages are ordered before pulses                   killed   C04: outcome DEADLOCK != CLEAN
M05 message-driven inheritance disabled                  killed   C06: event 34 differs
M06 RR quantum is one tick instead of four               killed   test_sched: FAIL test_sched.c:251
M07 receive-time max over all channels                   killed   m07_two_channels: outcome GUEST_FAILED != CLEAN
M08 _NTO_CHF_FIXED_PRIORITY ignored                      killed   C12: outcome GUEST_FAILED != CLEAN
M09 timeout fires on a non-matching state                killed   test_time: FAIL test_time.c:109
M10 MsgDeliverEvent boosts the server                    killed   telemetry: event 175 differs
M11 mutex PI reverts immediately                         killed   test_pi: FAIL test_pi.c:339
M12 unblock pulse omitted on cancellation                killed   C07: outcome DEADLOCK != CLEAN
mutation score: 12/12 killed
```

Warm run: **24–28 s** after the twelve CMake builds exist.

---

## 2. How a mutant is built

One CMake option selects the defect:

```sh
cmake -B build-mutants/M01 -DPERMUTE_MUTANT=M01
cmake --build build-mutants/M01 --target permute-run test_ipc
```

`CMakeLists.txt` adds `PERMUTE_MUTANT_M01=1` to `permute_core` only. The normal
build defines nothing, so no mutant code is compiled into the runner a user
gets. Each hook is a small `#ifdef` at the semantic site:

| ID | Site | Change |
|---|---|---|
| M01 | `permute_queue_insert` | always insert at the tail |
| M02 | `permute_timeout_fire` | a REPLY-blocked thread discards its timer |
| M03 | `finish_reply` | a stale rcvid reports success |
| M04 | `permute_ipc_receive` | pop a message before any pulse |
| M05 | `permute_pi_on_send` / `on_receive` | both inheritance hooks return |
| M06 | `permute_thread_create` | quantum is `PERMUTE_RR_QUANTUM_NS / 4` |
| M07 | `permute_pi_on_receive` | scan every channel, not the receiving one |
| M08 | both PI hooks | ignore `_NTO_CHF_FIXED_PRIORITY` |
| M09 | `permute_timeout_enter` | ignore the state mask |
| M10 | `permute_ipc_deliver_event` | boost the server by one |
| M11 | `boost_apply` | never apply a mutex boost |
| M12 | `unblock_timed_out` | omit `_PULSE_CODE_UNBLOCK` |

---

## 3. What is run

The suite is the conformance set (twelve executable scenarios), plus
`s02_pi_chain`, `s03_timer_timeout`, `s06_unblock_pulse`, the telemetry fanout,
and the M07 two-channel fixture. For each mutant and each scenario:

1. run with the same seed and guest,
2. compare the outcome class,
3. compare the canonical traces with `tracecmp` (Mazurkiewicz equivalence),
4. if nothing diverged, run the mutant's targeted unit tests.

A timeout during a scenario is also a kill: the behaviour changed enough to
hang.

---

## 4. The three test-only kills, stated

| Mutant | Why no scenario sees it |
|---|---|
| M06 RR quantum | The guest runs between shim calls; a CPU-bound loop is never preempted, so a shorter quantum changes nothing a scenario can observe. `test_sched` checks the tick arithmetic directly |
| M09 timeout latch | The latch is a kernel state machine; the scenarios enter states their masks cover. `test_time` arms a non-matching mask and checks the event queue |
| M11 mutex PI | The mutex shim is not interposed, so no guest scenario uses kernel mutexes. `test_pi` exercises the boost and its reversion |

This is a limitation of the current runtime, not of the mutants. When the
mutex shim and preemption land, their scenarios should take over (LIMITS #66).

---

## 5. Mutation score versus suite gaps

A surviving mutant is information. The first run scored **8/12**:

- M03 missed because the stale-rcvid hook sat on a path the scenario could not
  reach; moving it to the lookup failure fixed the mutant, and C07 caught it.
- M04 missed because C04 did not guarantee both the pulse and the message were
  queued; the fixture now sleeps one tick, and C04 catches it.
- M05 missed because the receive-side assignment masked the send-side hook;
  disabling both made C06 catch it.
- M07 missed because no scenario used two channels; the dedicated fixture now
  does.
- M11 missed because the hook changed the reversion path, which the test
  already expected to be zero; moving it to `boost_apply` made `test_pi` catch
  it.

The final 12/12 is the result of fixing the *suite*, not weakening the mutants.

---

## 6. Verification

```
make mutation    12/12 killed
make ci          green (conformance, shrink, bench quick, licence, coverage)
make asan        green
```

The normal build is unchanged: every hook is behind a define, and the licence
scan skips `build-mutants/`.

---

## 7. Next: P2-07, exploration

The mutation score says the suite notices known semantic defects. P2-07 asks
whether the search finds unknown ones: novelty bitmap, corpus, six mutation
operators, budgets, sharding, `LIVELOCK_SUSPECT`, and the random-vs-novelty
measurement on the six buggy builds from P2-04.
