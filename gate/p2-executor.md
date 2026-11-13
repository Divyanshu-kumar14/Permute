<!-- SPDX-License-Identifier: Apache-2.0 -->

# P2 report — executor and scenario loader

**Date:** 2026-11-14
**Scope:** the cooperative executor, the host operation bridge, `pthread_create`/`join`/`exit`/`cancel`, and loading a scenario `.so` into `permute-run`. This closes staging items #26 and #29 and is the prerequisite for P2-01.
**Result:** **32 executor checks, 0 failures**; six scenarios execute end to end; s01 produces 30,000 records with a stable digest; `make ci` green.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `runtime/executor.c` | Baton scheduler: one host thread per simulated thread, semaphore handoff |
| `runtime/bridge.c` | Host implementation of all 33 kernel operations, with trace emission |
| `runtime/runtime.h` | Executor and bridge contract |
| `kernel/objects.c` | `permute_thread_yield`: requeue self and clear the running slot |
| `abi/shim_pthread.c` | `pthread_create`, `pthread_join`, `pthread_exit`, `pthread_cancel` |
| `guest/dlmain.c` | `permute_guest_bind`: bind an object to the mapped hypercall page |
| `worker/run.c` | `--guest PATH` executes a compiled scenario |
| `tools/executor_check.py` | Runs every scenario through the executor |

The interposition surface is now **33 symbols**.

---

## 2. Results

```
executor: 32 checks, 0 failures (6 scenario(s) executed, 1 skipped)
```

| Scenario | Outcome | Records |
|---|---|---|
| s01_hello (10,000 round trips) | CLEAN | 30,000 |
| s02_pi_chain | CLEAN | 6 |
| s03_timer_timeout | CLEAN | 15 |
| s04_pulse_drop | CLEAN | 2 |
| s06_unblock_pulse | CLEAN | 5 |
| telemetry (fanout) | CLEAN | 302 |
| s07_priority_ceiling | skipped: mutex shim pending | — |

s01 run twice gives the same digest (`19e159f35bbd8d08`). s02's guest-side check
passes: the driver observes priority **10** through a three-process chain, so
message-driven inheritance works through the real shim path, not just in the
unit tests.

---

## 3. The bug that mattered: a yield that did not move the baton

s02 deadlocked with zero records. The cause was in the bridge:

```c
permute_sched_yield(k, t->self);   // picks the next thread *inside this host thread*
permute_executor_park();           // then parks, leaving that thread marked RUNNING
```

`permute_sched_yield` returns the thread that should run next, but the caller's
host thread keeps the baton. The scheduler then sees a thread marked RUNNING
that it can never wake, and the ready queue is empty: a reported deadlock that
is entirely the harness's fault.

Fix: `permute_thread_yield` requeues self at the tail, clears `k->running`, and
the bridge parks. Only the scheduler loop ever moves the baton. IMP-50.

A second, quieter bug: a scaffolded guest whose `permute_main` calls nothing did
not pull `hypercall.o` from the archive, so `permute_hypercall_bind` was absent
and the loader refused it. Every guest now links with `-u
permute_hypercall_bind`, because every guest must be bindable even if it makes
no calls.

A third was in the scenario, not the runtime: telemetry passed a `SIGEV_PULSE`
to `TimerTimeout`, which accepts only `SIGEV_UNBLOCK`, and delivered its own
registered event instead of the client's. The fanout now registers client-side,
sends the handle in the message, and the server verifies and delivers it — the
pattern `MsgDeliverEvent` exists for.

---

## 4. How the baton works

```
scheduler                     guest thread
   pick()  ──► state RUNNING
   post wake  ──────────────►  resumes
                               runs guest code
                               shim → kernel → BLOCKED
   wait yielded ◄────────────  posts yielded, waits wake
   pick()  ...
```

Exactly one guest thread is between `wake` and `yielded` at any instant. A shim
call is the only yield point, so guest code runs uninterrupted between kernel
calls — cooperative, as specified. The scheduler adds no policy: it takes the
ready queue head and, when the queue is empty, advances virtual time.

Thread creation allocates a kernel thread (which enters the ready queue) and a
host thread that waits for its first wake. Join blocks in `JOIN`; exit wakes
joiners before the thread slot is released.

---

## 5. Boundary

| In scope | Out of scope |
|---|---|
| Baton scheduling, all 33 operations, guest loading, join/exit/cancel | Mutex, condition-variable, semaphore, and signal shims (#37) |
| Trace records for SEND/RECV/REPLY/ERROR/PULSE/EVENT/TIMER | Scheduler and block records (#40) |
| `permute-run --guest` | Making guest execution the worker's default (#38/README) |
| `pthread_cancel` at shim boundaries | Asynchronous cancellation mid-instruction (#39) |

s07 compiles and is skipped at execution: glibc does not implement
`PTHREAD_PRIO_PROTECT`, and the mutex shim is still pending. The check names
the skip rather than pretending the scenario ran.

---

## 6. Next: P2-01, the fault engine

The executor makes P2-01 testable end to end. The fault engine's own gate is
still the plan's exact one: **adding a fault site must not change a
scheduling-only trace digest** (FR-4.3). The sites are send, receive, reply, and
pulse; the actions are drop, duplicate, reorder, delay, timeout, and kill;
materialisation is driven by `D_FAULT`; the plan is configured under `[faults]`
in `permute.toml`.

The executor is what lets a dropped or delayed message be observed as a
behaviour, rather than as a unit-test assertion about a queue.
