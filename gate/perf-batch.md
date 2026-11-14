<!-- SPDX-License-Identifier: Apache-2.0 -->

# Performance and accuracy batch — executor, pulses, mutex oracle, shrinker

**Date:** 2026-11-14
**Scope:** supervisor-directed optimizations on top of `d138e28` (post-`v0.1.0`).
**Result:** **all four changes landed, every required gate green.** The baton
change cuts the measured switch to a **689 ns median** (10 runs), the pulse
change moves exactly one record in one simulator reference trace, the mutex
peer edge closes a gap in the live wait-for graph, and the shrinker's p95 drops
from 0.388 s to 0.147 s. One measured trade-off is recorded in section 5.

---

## 1. What changed

| Area | File | Change |
|---|---|---|
| Baton | `runtime/executor.c` | `turn` and `yielded` each `_Alignas(64)`; allocation moved from `calloc` to `posix_memalign(…, 64, …)`; `cpu_relax()` (`pause` on x86, `yield` on aarch64, no-op elsewhere) in the spin loop; `sched_yield` every 2,048 spins instead of every 256 |
| Pulses | `kernel/pulse.c` | `permute_pulse_post()` no longer calls `permute_pi_on_send()` / `permute_pi_on_receive()` on direct delivery |
| Mutex oracle | `kernel/oracle.c` | `peer_of()` maps `PERMUTE_THREAD_MUTEX` to the mutex owner's tid |
| Shrinker | `cli/permute/shrink/runner.py` | scratch traces in `/dev/shm` when writable; `OUTCOME=` fast path for `CLEAN`+deadlock and `DEADLOCK` |

The aarch64 guard is not decoration: CI builds and tests this file under
`--platform linux/arm64`, where `__builtin_ia32_pause()` does not exist.

## 2. Switch latency

`make switch-bench` twice, then the binary ten times on an otherwise idle
machine:

```text
SWITCH_NS=710, 653, 635, 658, 723, 717, 699, … 
n=10 min=652 median=689 max=1019
make bench row: 691 ns
```

Before this batch the same fixture measured 835–935 ns in `make bench` and
952 ns in the committed gate1 scorecard. `make gate1` sampled **1016 ns** in
its single run; that is the high end of the same distribution (the max of the
ten-run sample is 1019), not a different code path. Every sample is well
inside the 4,000 ns gate.

## 3. Pulse semantics and the trace it moved

QNX pulses are fire-and-forget; message-driven priority inheritance applies to
messages, not pulses. With the two calls removed, C06's pulse-only waiter is
picked at its own priority (20) instead of the pulse's (30):

```diff
-{"kind":"SCHED_PICK","obj":30,"src":"t2",…}
+{"kind":"SCHED_PICK","obj":20,"src":"t2",…}
```

That is the only difference in the whole simulator oracle set. The reference
was re-recorded with `run_conformance.py --record`; C04 and C05 are unchanged
because their pulses are queued while the receiver is running, and the
examples are unchanged because telemetry's pulses are queued too. The queued
receive path still assigns priority per FR-3.14; only delivery to an already
blocked receiver changed.

## 4. Mutex peer and shrinker

`peer_of()` previously returned 0 for a mutex waiter, so a cycle formed only by
mutex edges could not be rendered. It now resolves `t->wobj` through
`permute_mutex_get()` and follows `m->owner`, guarding a missing mutex, a
`PERMUTE_HANDLE_NONE` owner, and a dead owner. A waiter cannot wait on a lock
it owns (non-recursive relock returns `EDEADLK`), so no self-edge is possible.

`make shrink-bench` over 20 real artifacts:

| Metric | Before | After |
|---|---|---|
| p50 wall | 0.374 s | 0.142 s |
| p95 wall (`<= 30 s`) | 0.388 s | 0.147 s |
| max reduced events (`<= 20`) | 12 | 12 |
| minimality | proven | proven |

The fast path still requires the scratch trace to exist before returning
`True`; a runner that printed `DEADLOCK` but wrote nothing is not a
reproduction.

## 5. The measured trade-off

The 2,048-spin ceiling improves the isolated ping-pong but costs throughput
when host threads outnumber cores. The 20,000-seed benchmark runs 16 workers ×
4 guest threads on this 16-thread host:

| Spin ceiling | Cooperative switch | 20,000 seeds | Seeds/s/worker |
|---|---|---|---|
| 256 (before) | 790 ns | 10.4 s | 120.6 |
| **2,048 (requested)** | **691 ns** | **24–29 s** | **43–52** |

Both settings pass every criterion (`<= 180 s`, `>= 15 seeds/s`), but the seed
workload is 2.3–2.8× slower with 2,048. The requested value is committed as
specified; if seed throughput matters more than the last ~15 % of the
microbenchmark, `0xffu` is the measured better balance and is a one-token
revert.

## 6. Verification

```text
make unit               green, 1 s   (test_pulse 21,351 checks, test_pi 232, …)
make bench-bugs-quick   6/6 found, 0 false positives, 3 s
make switch-bench       SWITCH_NS=691 (ten-run median 689)
make gate1              9/9 PASS, 199 s
                        12/12 mutants, 1000/1000 same-seed, ASan/UBSan clean
make shrink-bench       20/20 proven, p95 0.147 s
make conformance        12 executed, 2 skipped, 0 failures (C06 re-recorded)
make ci                 green, 188 s
                        examples 3/3, submission 6 verified / 2 blocked, coverage PASSED
make bench              6/6 criteria PASS
```
