<!-- SPDX-License-Identifier: Apache-2.0 -->

# Shrink bench results

**Commit:** `f9ab9ed`  
**Date:** 2026-11-15  
**CPU:** 13th Gen Intel(R) Core(TM) i5-13450HX  
**Cores:** 16  
**Kernel:** 7.3.4-1-cachyos  
**OS:** Linux-7.3.4-1-cachyos-x86_64-with-glibc2.44  
**Python:** 3.15.2

Twenty s01 deadlock artifacts with 3-8 fault rules, each shrunk through
`permute shrink` and re-executed to count the reduced trace records.

| Case | Seed | Inputs (orig -> reduced) | Events | Wall (s) | Minimality |
|---|---|---|---|---|---|
| 0 | 1 | 3 -> 1 | 12 | 0.142 | proven |
| 1 | 2 | 4 -> 1 | 12 | 0.140 | proven |
| 2 | 3 | 5 -> 1 | 12 | 0.138 | proven |
| 3 | 4 | 6 -> 1 | 12 | 0.149 | proven |
| 4 | 5 | 7 -> 1 | 12 | 0.138 | proven |
| 5 | 6 | 7 -> 1 | 12 | 0.139 | proven |
| 6 | 7 | 3 -> 1 | 12 | 0.139 | proven |
| 7 | 8 | 4 -> 1 | 12 | 0.137 | proven |
| 8 | 9 | 5 -> 1 | 12 | 0.143 | proven |
| 9 | 10 | 6 -> 1 | 12 | 0.143 | proven |
| 10 | 11 | 7 -> 1 | 12 | 0.145 | proven |
| 11 | 12 | 7 -> 1 | 12 | 0.142 | proven |
| 12 | 13 | 3 -> 1 | 12 | 0.170 | proven |
| 13 | 14 | 4 -> 1 | 12 | 0.145 | proven |
| 14 | 15 | 5 -> 1 | 12 | 0.139 | proven |
| 15 | 16 | 6 -> 1 | 12 | 0.141 | proven |
| 16 | 17 | 7 -> 1 | 12 | 0.138 | proven |
| 17 | 18 | 7 -> 1 | 12 | 0.139 | proven |
| 18 | 19 | 3 -> 1 | 12 | 0.142 | proven |
| 19 | 20 | 4 -> 1 | 12 | 0.143 | proven |

| Metric | Limit | Measured | Verdict |
|---|---|---|---|
| p50 wall | - | 0.141 s | - |
| p95 wall | <= 30 s | 0.149 s | PASS |
| Max reduced events | <= 20 | 12 | PASS |

**Result: PASS**
