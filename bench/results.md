<!-- SPDX-License-Identifier: Apache-2.0 -->

# Bench results

**Commit:** `39cdd04`  
**Date:** 2026-11-14  
**CPU:** 13th Gen Intel(R) Core(TM) i5-13450HX  
**Cores:** 16  
**Kernel:** 7.3.3-1-cachyos  
**OS:** Linux-7.3.3-1-cachyos-x86_64-with-glibc2.44

| Criterion | NFR | Measured | Verdict | Note |
|---|---|---|---|---|
| Kernel call (non-blocking) | 60-150 ns | 5 ns | PASS | below the modelled band |
| Cooperative switch | 1.5-4 us | 903 ns | PASS | below the modelled band, faster |
| Message throughput | >= 100,000 msg/s | 712,214 msg/s | PASS | 100,000 round trips |
| Worker + SUT RSS (4 threads) | <= 70 MB | 15.2 MB | PASS |  |
| 20,000 seeds wall | <= 180 s on 16 workers | 9.3 s (2,146 seeds/s) | PASS |  |
| Seed throughput per worker | >= 15 seeds/s | 134.1 seeds/s/worker | PASS |  |

Microbenchmarks run with the trace disabled; throughput counts both the
send and the reply. The seed run uses the four-thread fixture over
20,000 seeds on 16 workers.

**Result: PASS**
