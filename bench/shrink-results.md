<!-- SPDX-License-Identifier: Apache-2.0 -->

# Shrink bench results

**Commit:** `32d9151`  
**Date:** 2026-11-14  
**CPU:** 13th Gen Intel(R) Core(TM) i5-13450HX  
**Cores:** 16  
**Kernel:** 7.3.3-1-cachyos  
**OS:** Linux-7.3.3-1-cachyos-x86_64-with-glibc2.44  
**Python:** 3.15.2

Twenty s01 deadlock artifacts with 3-8 fault rules, each shrunk through
`permute shrink` and re-executed to count the reduced trace records.

| Case | Seed | Inputs (orig -> reduced) | Events | Wall (s) | Minimality |
|---|---|---|---|---|---|
| 0 | 1 | 3 -> 1 | 12 | 0.373 | proven |
| 1 | 2 | 4 -> 1 | 12 | 0.372 | proven |
| 2 | 3 | 5 -> 1 | 12 | 0.375 | proven |
| 3 | 4 | 6 -> 1 | 12 | 0.369 | proven |
| 4 | 5 | 7 -> 1 | 12 | 0.368 | proven |
| 5 | 6 | 7 -> 1 | 12 | 0.373 | proven |
| 6 | 7 | 3 -> 1 | 12 | 0.375 | proven |
| 7 | 8 | 4 -> 1 | 12 | 0.369 | proven |
| 8 | 9 | 5 -> 1 | 12 | 0.373 | proven |
| 9 | 10 | 6 -> 1 | 12 | 0.375 | proven |
| 10 | 11 | 7 -> 1 | 12 | 0.379 | proven |
| 11 | 12 | 7 -> 1 | 12 | 0.374 | proven |
| 12 | 13 | 3 -> 1 | 12 | 0.380 | proven |
| 13 | 14 | 4 -> 1 | 12 | 0.374 | proven |
| 14 | 15 | 5 -> 1 | 12 | 0.388 | proven |
| 15 | 16 | 6 -> 1 | 12 | 0.376 | proven |
| 16 | 17 | 7 -> 1 | 12 | 0.376 | proven |
| 17 | 18 | 7 -> 1 | 12 | 0.403 | proven |
| 18 | 19 | 3 -> 1 | 12 | 0.385 | proven |
| 19 | 20 | 4 -> 1 | 12 | 0.372 | proven |

| Metric | Limit | Measured | Verdict |
|---|---|---|---|
| p50 wall | - | 0.374 s | - |
| p95 wall | <= 30 s | 0.388 s | PASS |
| Max reduced events | <= 20 | 12 | PASS |

**Result: PASS**
