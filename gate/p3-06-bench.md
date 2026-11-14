<!-- SPDX-License-Identifier: Apache-2.0 -->

# P3-06 report — bench suite

**Date:** 2026-11-14
**Scope:** message throughput, non-blocking kernel-call cost, cooperative
switch cost, worker RSS, and multi-worker seed throughput (NFR-2 through
NFR-6).
**Result:** **6/6 criteria pass.** Published with hardware and commit in
[`bench/results.md`](../bench/results.md).

---

## 1. Measured

| Criterion | NFR | Measured | Verdict |
|---|---|---|---|
| Kernel call, non-blocking | 60–150 ns | **5 ns** | PASS (below the band) |
| Cooperative switch | 1.5–4 µs | **903 ns** | PASS (below the band) |
| Message throughput | ≥ 100,000 msg/s | **712,214 msg/s** | PASS |
| Worker + SUT RSS, 4 threads | ≤ 70 MB | **15.2 MB** | PASS |
| 20,000 seeds wall | ≤ 180 s on 16 workers | **9.3 s** | PASS |
| Seed throughput per worker | ≥ 15 seeds/s | **134.1 seeds/s/worker** | PASS |

Hardware: 13th Gen Intel Core i5-13450HX, 16 cores, Linux 7.3.3, glibc 2.44.
Commit `39cdd04`. The full table, including the note on each measurement, is in
[`bench/results.md`](../bench/results.md).

---

## 2. Method

| Fixture | What it does | Units |
|---|---|---|
| `tests/fixtures/bench/kernel_call.c` | 2,000,000 `SchedGet` calls through the shim | calls |
| `tests/fixtures/bench/yield_switch.c` | two threads hand the baton back and forth | switches |
| `tests/fixtures/bench/throughput.c` | 100,000 request/reply round trips, 64-byte payload | messages (send + reply) |
| `tests/fixtures/bench/four_threads.c` | main + three workers, a few `SchedYield`s each | seeds |

`tools/bench_guest.c` runs a guest with the trace pointer set to `NULL`; the
microbenchmarks do not pay for a log record. `tools/switch_bench.c` already
worked this way and is reused.

The RSS number is `RUSAGE_CHILDREN.ru_maxrss` from a **fresh interpreter** that
runs one worker over 64 seeds and waits. That covers the worker and the
`permute-run` child together, avoiding `/proc` polling gaps for short-lived
children (IMP-90).

The 20,000-seed run uses `permute.run.execute` with `jobs=min(16, cores)` and
the trace-free four-thread fixture. Seeds per worker is `seeds/s ÷ jobs`.

---

## 3. Reading the numbers honestly

- **Kernel call 5 ns** is below the NFR-3 band because it is an interposition
  path, not a kernel entry or syscall. The band assumes a real kernel call;
  being faster is not a failure (LIMITS #85).
- **Switch 0.9 µs** is below the thread band for the same reason: the baton is
  a pair of atomics, not a host context switch. The gate is ≤ 4,000 ns and the
  measurement is 903 ns.
- **Seed throughput** uses a synthetic four-thread fixture; it measures process
  setup, thread creation and the baton, not a real SUT's work (LIMITS #86).
- **Results are from one machine.** They name the CPU, kernel and commit; a
  different machine must run `make bench` itself (LIMITS #84).

---

## 4. Reproduce

```sh
make bench
```

`make bench` builds the four fixtures, runs the five measurements, writes
`bench/results.md`, prints the table, and exits non-zero if a threshold is
missed. It is not part of `make ci` or `make gate1`, because the values are
hardware-dependent; the deterministic suite is what CI gates on.

---

## 5. Verification

```
make bench   6/6 criteria pass
make ci      green
make gate1   9/9 PASS, exit 0
```
