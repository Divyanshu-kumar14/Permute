<!-- SPDX-License-Identifier: Apache-2.0 -->

# P3-10 report — G2 scorecard

**Date:** 2026-11-14
**Scope:** PRD §13.3 — `tools/gate2.sh` prints conformance rate, outcome-class
matching, mutation score, the 6/6 bug benchmark, shrink p95 and the NFR rows,
then writes `gate/gate2-scorecard.md`.
**Result:** **13 of 14 rows PASS; the outcome-class row FAILs honestly.**
C11 and C13 are mandatory conformance scenarios that cannot execute in this
simulator (LIMITS #60), so a 12/12 mandatory outcome-class claim is not
supported. `make gate2` therefore exits 1 with that one row red, rather than
redefining the threshold.

---

## 1. Measured

Reference machine: 13th Gen Intel Core i5-13450HX, 16 cores, Linux 7.3.3,
Python 3.15.2. Wall time for `make gate2`: **79 s**.

| Criterion | G2 threshold | Measured | Verdict |
|---|---|---|---|
| Conformance equivalence | ≥ 10/12 | **10/12 mandatory equivalent** (12 executed, 0 failures) | PASS |
| Outcome-class match | 12/12 mandatory | **10/12 executed, 0 wrong, 2 skipped** | **FAIL** |
| Mutation score | ≥ 10/12 | **12/12 killed** | PASS |
| Bug benchmark | 6/6, 0 false positives | **6/6 found, 0 FP** | PASS |
| Shrink p95 | ≤ 30 s | **0.388 s** | PASS |
| Shrink reduced reproduction | ≤ 20 events | **12 events** (max) | PASS |
| NFR-2 seed throughput | ≥ 15 seeds/s/worker | **118.1 seeds/s/worker** | PASS |
| NFR-3 kernel call | 60–150 ns | **3 ns** | PASS |
| NFR-4 context switch | 1.5–4 µs | **935 ns** | PASS |
| NFR-5 20,000 seeds | ≤ 180 s on 16 workers | **10.6 s** | PASS |
| NFR-6 memory per worker | ≤ 70 MB | **15.4 MB** | PASS |
| G1 message throughput | ≥ 100,000 msg/s | **681,216 msg/s** | PASS |
| NFR-12 `--version` | ≤ 200 ms | **84 ms** | PASS |
| NFR-12 `run --seeds 1` | ≤ 2 s | **193 ms** | PASS |

NFR-12 is one scorecard row (both measurements) but is listed split here for
readability.

---

## 2. Why the outcome-class row is red

`make conformance` reports:

```
conformance: 12 executed, 2 skipped, 0 failure(s); mandatory 10/12 executed; oracle: simulator reference (not a QNX capture)
```

C11 needs process death and the simulator runs one guest process; C13 needs the
mutex shim (LIMITS #60). Both compile and stay in the set, but they are skipped
with a reason. Outcome class for them is **unproven**, so the 12/12 mandatory
row cannot pass even though every executed scenario matched (0 wrong).

The scorecard prints the blocked reason on the terminal and repeats it in the
footer. This is the R2 discipline: a gate that cannot be met is reported, not
weakened.

---

## 3. Method

`tools/gate2.sh` follows `gate1.sh`: each row runs the command that produces
the evidence, parses that command's own summary line, records the measured
value, and logs to `build/gate2/`.

| Row | Command | Parsed from |
|---|---|---|
| Conformance, outcome class | `make conformance` | runner summary + `conformance/REPORT.md` result column |
| Mutation | `make mutation` | `mutation score: N/12 killed` |
| Defects | `make bench-bugs` | `bug benchmark: N/N found, N false positive` |
| Shrink | `make shrink-bench` | `p95 X s` + `bench/shrink-results.md` maximum |
| NFR-2..6 | `make bench` | `bench/results.md` verdict column |
| NFR-12 | direct timing of `tools/permute` | worst of 3 version runs, worst of 2 seeded runs |

No value is hard-coded. Re-running the gate on another machine restamps every
measurement and can only change verdicts through the parsed output.

---

## 4. Reproduce

```sh
make gate2
# ... rows ...
# Result: FAIL — one row (outcome-class 12/12) blocked on C11/C13
# scorecard: gate/gate2-scorecard.md
```

Exit 0 means G2 is met. Exit 1 with only the outcome-class row failing means the
gate is correctly reporting the QNX-dependent gap, not a regression. The full
scorecard is committed at [`gate2-scorecard.md`](gate2-scorecard.md).
