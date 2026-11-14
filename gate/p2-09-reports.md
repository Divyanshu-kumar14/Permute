<!-- SPDX-License-Identifier: Apache-2.0 -->

# P2-09 report — reports

**Date:** 2026-11-14
**Scope:** `report.json`, `junit.xml`, `sarif.json`, and a self-contained `index.html`.
**Result:** all four formats produced by every run; **32 report checks green**; JUnit structure validates; **SARIF validates against the OASIS 2.1.0 schema**; the HTML has no external reference. `make ci` green.

---

## 1. The gate, measured

```
test_report: 32 checks, 0 failure(s)
```

| Gate | Result |
|---|---|
| Four formats | `report.json`, `junit.xml`, `sarif.json`, `index.html` per run |
| JUnit schema valid | structural validation: nesting, required attributes, count agreement |
| SARIF 2.1.0 schema valid | `jsonschema.Draft4Validator` against the vendored OASIS schema |
| HTML self-contained | no `<link>`, `<script>`, `@import`, `url(`, `http://`, `https://`; one inline `<style>` |
| Clean run | 3 seeds, JUnit failures 0, SARIF results empty |
| Violation run | JUnit failure + SARIF `oracle-deadlock` error, exit 1 |

Live example from a run with `send:drop@1`:

```
1 seeds: 0 clean, 0 crash, 0 budget, 1 violation, 0 internal
report: .../report.json
reports: .../junit.xml, .../sarif.json, .../index.html
exit 1
```

`junit.xml`:

```xml
<testsuites name="permute" tests="1" failures="1" errors="0" skipped="0" time="0">
  <testsuite ...>
    <testcase classname="permute.seed" name="seed-0" time="0">
      <failure message="VIOLATION" type="violation">deadlock[dl]: ...</failure>
    </testcase>
  </testsuite>
</testsuites>
```

`sarif.json` names `permute` as the driver, carries the rules and one result
per non-clean seed plus one per oracle finding, with `level` mapped from the
invariant severity.

---

## 2. How the four stay consistent

`report.seed_rows` derives one row per seed from four sources:

1. the trace directory names every seed that ran;
2. worker findings give crash / budget / internal;
3. oracle findings of severity `error` give violations, `warn`/`info` remain
   warning results in SARIF and do not fail JUnit;
4. a verification mismatch marks the seed `nondeterminism`.

JUnit, SARIF and HTML all render that one list, so the counts cannot drift
between formats.

---

## 3. Schema validation without network

- **SARIF**: the OASIS 2.1.0 schema is vendored at
  `schemas/sarif-2.1.0.json` with its source in `schemas/README.md`. The test
  uses `Draft4Validator` explicitly because the schema's `$schema` is
  draft-04. `$schema` in the output is a string and is never fetched.
- **JUnit**: no XSD validator is installed. `validate_junit` checks what the
  schema enforces for the emitted subset — root and suite nesting, required
  `classname`/`name`, and `tests`/`failures`/`errors`/`skipped` agreeing with
  the elements found. If `jsonschema` is missing, the SARIF test says so
  rather than passing silently; the CI workflow installs it.

The HTML test rejects every external mechanism, and a project name containing
`<script>` is escaped rather than injected.

---

## 4. Files

| Path | Purpose |
|---|---|
| `cli/permute/report.py` | `seed_rows`, `write_junit`, `write_sarif`, `write_html`, `write_all`, the two validators |
| `cli/permute/__main__.py` | writes all four after the summary; exposes the paths in text and JSON |
| `schemas/sarif-2.1.0.json` | vendored OASIS schema |
| `schemas/README.md` | provenance |
| `tests/python/test_report.py` | 32 checks: formats, schemas, clean/violation, warn, verify, escaping, CLI |
| `.github/workflows/ci.yml` | installs `jsonschema` with `pyyaml` |

---

## 5. Boundary

- JUnit validation is structural, not XSD-backed (LIMITS #76).
- The SARIF schema is vendored for offline CI (LIMITS #77).
- Report rows cover seeds that ran; a shard reports its slice (LIMITS #78).
- The HTML carries no timestamp, so the same evidence renders the same bytes
  (IMP-85). `report.json` remains the machine-readable source.

---

## 6. Verification

```
make report   32 checks, 0 failures
make ci       green
```

The normal run path gains three files and no new failure mode: a report write
cannot change the run's exit code, and a clean run's JUnit/SARIF are empty of
failures/errors.

---

## 7. Next: P2-10, G1 scorecard

`tools/gate1.sh` prints each G1 criterion with its measured value and pass/fail.
The reports from this task, the conformance status, the mutation score, the
benchmark, and the determinism sweep are the inputs.
