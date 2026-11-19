<!-- SPDX-License-Identifier: Apache-2.0 -->

# Bench results

**Commit:** `d138e28`  
**Date:** 2026-11-14  
**CPU:** 13th Gen Intel(R) Core(TM) i5-13450HX  
**Cores:** 16  
**Kernel:** 7.3.3-1-cachyos  
**OS:** Linux-7.3.3-1-cachyos-x86_64-with-glibc2.44

| Criterion | NFR | Measured | Verdict | Note |
|---|---|---|---|---|
| Kernel call (non-blocking) | 60-150 ns | 5 ns | PASS | faster than the band; band is a target, not a floor (LIMITS #85) |
| Cooperative switch | 1.5-4 us | 691 ns | PASS | faster than the band; band is a target, not a floor (LIMITS #85) |
| Message throughput | >= 100,000 msg/s | 973,935 msg/s | PASS | 100,000 round trips |
| Worker + SUT RSS (4 threads) | <= 70 MB | 15.4 MB | PASS |  |
| 20,000 seeds wall | <= 180 s on 16 workers | 29.0 s (691 seeds/s) | PASS |  |
| Seed throughput per worker | >= 15 seeds/s | 43.2 seeds/s/worker | PASS |  |

Microbenchmarks run with the trace disabled; throughput counts both the
send and the reply. The seed run uses the four-thread fixture over
20,000 seeds on 16 workers.

**Result: PASS**
