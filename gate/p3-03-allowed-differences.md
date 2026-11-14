<!-- SPDX-License-Identifier: Apache-2.0 -->

# P3-03 report — allowed differences

**Date:** 2026-11-14
**Scope:** FR-11.7 and risk R2 — every permitted simulator/target divergence is
documented per field, with an architectural reason, in at most eight entries.
**Result:** **PASS.** `conformance/ALLOWED-DIFFERENCES.md` has **7 entries**
(cap 8); each names an exact canonical field, its treatment and a rationale.
The cap and table shape are enforced by `make docs-check`.

---

## 1. Entries

| # | Field | Treatment | Rationale in one line |
|---|---|---|---|
| 1 | `src`, `dst`, `obj` | labels `tN`/`pN`/`ch@owner:N`/`co@owner:N`/`r@owner:N` | kernel ids are allocation-order artefacts; object labels are scoped to the creator |
| 2 | `aux` on `CONNECT_ATTACH` | channel id remapped to a label | this is the one `aux` that carries an id; every other `aux` is semantic |
| 3 | `payload` on data events | SHA-256/128 + length | keeps payload identity, removes capture representation |
| 4 | `payload` on `THREAD_START` | dropped | the process name is host packaging, not scheduler state |
| 5 | `t_ns` | excluded unless `--strict-time` | virtual and real clocks share no epoch or rate |
| 6 | line order and `causal` indices | canonical linear extension; edges compared as event-key pairs | FR-11.3 Mazurkiewicz equivalence |
| 7 | host-only data | never in `permute-canonical/1` | not reproducible and not QNX semantics |

Compared exactly (any difference fails): `kind`, event count, causal edges,
per-thread order, outcome class, errno, payload hashes, effective priority,
pulse code/value, rcvid validity, and `aux` outside entry 2.

The known canonicalisation limit (thread labels by first appearance) is stated
in the file and cross-referenced as LIMITS #61.

---

## 2. Enforcement

`tools/docs_check.py` now parses the entry table and fails on:

- a missing file or missing table header;
- zero entries or more than eight (R2's early-warning signal);
- a row with the wrong column count;
- an entry whose field, treatment or rationale is empty.

`make docs-check` runs in `make ci`, so the constraint cannot drift silently.

```
$ make docs-check
docs-check: PASSED (12 document(s) with owner and status; allowed differences within 8)
```

Negative control — a synthetic file with nine entries:

```
problems: ['ALLOWED-DIFFERENCES.md: 9 entries, cap is 8 (R2 early warning)']
```

---

## 3. Verification

```
make docs-check   PASSED, allowed differences within 8
negative test     9 entries -> cap violation reported
```

Evidence is tool output, not inspection by eye: the parser reads the same
Markdown a reviewer reads.
