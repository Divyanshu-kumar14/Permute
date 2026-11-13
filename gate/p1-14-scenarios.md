<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-14 report — scenarios

**Date:** 2026-11-14
**Scope:** the seven scenario sources, the QNX-compat header they need, the expanded interposition surface, and the static coverage checks.
**Result:** **7 scenarios built, 0 failures**; fanout covers all 20 primitives; s03 covers all 11 mask bits; full `make test` green.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `abi/permute_qnx.h` | `sigevent`, `_pulse`, mask bits, channel flags, pulse codes, init macros |
| `abi/shim_sched.c` | `SchedSet`, `SchedGet`, `SchedYield` |
| `abi/shim_ipc.c` | `ChannelDestroy`, `ConnectDetach`, `MsgError`, `MsgSendPulse`, `MsgReceivePulse`, `MsgRegisterEvent`, `MsgDeliverEvent`, `MsgVerifyEvent` |
| `abi/shim_time.c` | `TimerTimeout` |
| `scenarios/s01_hello … s07_priority_ceiling/scenario.c` | One scenario per named workload |
| `examples/telemetry/scenario.c` | The sensor-fanout workload |
| `tools/build_scenarios.py` | Builds all sources and checks the two coverage properties |

The interposition surface grew from 17 to **29 symbols**.

---

## 2. Results

```
s01_hello:            built
s02_pi_chain:         built
s03_timer_timeout:    s03 covers all 11 timer mask bits
s04_pulse_drop:       built
s06_unblock_pulse:    built
s07_priority_ceiling: built
telemetry:            fanout covers all 20 primitives
scenarios: 7 built, 0 failure(s)
```

`make test` includes `make scenarios`; all other suites unchanged and green, ASan clean.

| Scenario | What it encodes |
|---|---|
| s01 | 10,000 round trips with a 64-byte payload and byte comparison |
| s02 | three-hop chain; the driver must observe priority 10 |
| s03 | all eleven `_NTO_TIMEOUT_*` bits, one per block |
| s04 | short receive buffer returns `EFAULT` and the pulse is lost |
| s06 | `_NTO_CHF_UNBLOCK`: timeout in `REPLY`, server gets the rcvid back |
| s07 | `PTHREAD_PRIO_PROTECT` ceiling and refusal above it |
| telemetry | 1 server, 3 clients at 5/15/25, pulses, timer, timeout, client kill |

---

## 3. Why the coverage checks are static

The scenarios are written against the pthread API a real service uses, and they compile and link for the simulator. They do **not** yet run end to end: `permute-run` executes the built-in seeded scenario, and `pthread_create`/mutexes resolve to libc until the executor and pthread shim exist.

So `make scenarios` proves exactly two things, and says so:

1. every source compiles against the modelled ABI (`--no-undefined`, version script, `nm` audit);
2. the coverage each scenario promises is present in its source — the fanout names all 20 primitives, and s03 names all 11 mask bits.

That is the strongest claim available today. When the executor lands, the same checks become runtime assertions rather than being deleted.

---

## 4. The compat header

Scenario sources must compile unchanged for QNX and the simulator (FR-2.1), which means they use QNX names: `struct sigevent`, `SIGEV_PULSE_INIT`, `_NTO_TIMEOUT_*`, `_PULSE_CODE_*`.

`abi/permute_qnx.h` supplies those from public documentation. Two details matter:

- `struct sigevent` is **32 bytes** and `struct _pulse` **24 bytes**, both asserted at compile time. The trace format and the kernel already rely on those sizes.
- The header must not include `<signal.h>`: glibc defines a different `struct sigevent` there, and including it would either collide or silently change the layout. Signal-API scenarios wait for the signal shims.

The timeout bits and channel flags are duplicated from `kernel/state.h` deliberately — a guest cannot include kernel internals. Both sets are checked against the same values by `test_shim`/`test_time` behaviour, and a mismatch would show up as a wrong timeout or a channel that ignores its flag.

---

## 5. Boundary

| In scope | Out of scope |
|---|---|
| Seven sources, compiling and statically complete | Running them through an executor (#32) |
| 29 interposed symbols | pthread, signal, condvar, semaphore, interrupt shims (#33) |
| Every fanout primitive | Oracles over the resulting traces |

---

## 6. Next

**P1-15 — CI baseline.** Build, unit, 100 seeds × 10 runs, determinism check, licence scan, coverage gate, and the portability matrix (x86_64 glibc on PR; aarch64 + musl nightly). That is where all of the checks built so far become a pipeline somebody else can run.
