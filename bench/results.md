<!-- SPDX-License-Identifier: Apache-2.0 -->

# Bench results

**Commit:** `32d9151`  
**Date:** 2026-11-14  
**CPU:** 13th Gen Intel(R) Core(TM) i5-13450HX  
**Cores:** 16  
**Kernel:** 7.3.3-1-cachyos  
**OS:** Linux-7.3.3-1-cachyos-x86_64-with-glibc2.44

| Criterion | NFR | Measured | Verdict | Note |
|---|---|---|---|---|
| Kernel call (non-blocking) | 60-150 ns | 3 ns | PASS | below the modelled band |
| Cooperative switch | 1.5-4 us | 935 ns | PASS | below the modelled band, faster |
| Message throughput | >= 100,000 msg/s | 681,216 msg/s | PASS | 100,000 round trips |
| Worker + SUT RSS (4 threads) | <= 70 MB | 15.4 MB | PASS |  |
| 20,000 seeds wall | <= 180 s on 16 workers | 10.6 s (1,890 seeds/s) | PASS |  |
| Seed throughput per worker | >= 15 seeds/s | 118.1 seeds/s/worker | PASS |  |

Microbenchmarks run with the trace disabled; throughput counts both the
send and the reply. The seed run uses the four-thread fixture over
20,000 seeds on 16 workers.

**Result: PASS**
