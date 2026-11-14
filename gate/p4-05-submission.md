<!-- SPDX-License-Identifier: Apache-2.0 -->

# P4-05 report — submission checklist

**Date:** 2026-11-14
**Scope:** PRD §17 table (7 rows) and §17.1 prohibited-content scan.
**Result:** **5 of 7 rows verified, 2 blocked on the QNX SDP, §17.1 scan clean.**
`tools/submission_check.py` checks the repository mechanically; it exits 0 with
blocked rows reported, and exits 1 on a missing licence, a missing disclosure,
a vendor marker, an image, a binary, or a secret.

---

## 1. Measured

```
$ make submission
check                    verdict  detail
Licence at root          PASS     LICENSE is Apache-2.0
Source completeness      PASS     312 tracked files, build and tests present
QNX build instructions   PASS     SDP 8.0, x86_64, qcc -Vgcc_ntox86_64 (GCC 12 base)
Category and prior work  PASS     category named; prior work disclosed in README and NOTICE
QNX evidence document    BLOCKED  fields listed, no SDP installed: nothing claimed
Prohibited content       PASS     no vendor files, images, binaries, or secrets in 312 tracked files
QNX oracle capture       BLOCKED  no capture; conformance reports simulator references
SPDX headers             PASS     licence scan: PASSED (SPDX-License-Identifier: Apache-2.0)

submission: PASSED (6 check(s) verified, 2 blocked on the QNX SDP)
```

The two blocked rows are the same gap, counted twice: §17 requires
`docs/QNX-EVIDENCE.md` to contain a real capture, and the capture directory to
hold QNX traces. Neither exists without an SDP, and neither is faked.

| §17 row | State | Evidence |
|---|---|---|
| Public repository | PASS (external) | `origin` is `github.com/Divyanshu-kumar14/Permute`; visibility is a GitHub setting |
| Licence detectable at root | PASS | `LICENSE` is the Apache-2.0 text; `README` and `NOTICE` name it |
| Source completeness | PASS | 312 tracked files: kernel, runtime, worker, CLI, tests, conformance, docs, examples, packaging |
| Build and run instructions | PASS | `docs/REPRODUCE.md` §5 names SDP 8.0, x86_64, `qcc -Vgcc_ntox86_64` (GCC 12 base) |
| Genuine QNX use | **BLOCKED** | no SDP, no QEMU image, no capture, no `permute-capture`; `docs/QNX-EVIDENCE.md` lists every required field as not available |
| Host-only tool justification | PASS | `docs/QNX-EVIDENCE.md` §3: host-first by design, target twin is the evidence |
| Category | PASS | README "Category and prior work": Developer Tooling and Host Tools |
| Prior work disclosure | PASS | README states no pre-existing harness is bundled; `NOTICE` records the third-party position |
| §17.1 prohibited content | PASS | scan over tracked files |

## 2. The §17.1 scan

The scan runs over `git ls-files` (or every file when git is absent, so it also
works from a source tarball) and fails on:

- **vendor material** — any tracked line that names `QNX Software Systems`,
  `BlackBerry Limited`, `BlackBerry QNX`, `qnx700`, or `qnx800` **and**
  attributes copyright or a licence on that line. A vendor name in prose (this
  report, for instance) is not vendored material; an attribution line is. The
  ABI header `abi/permute_qnx.h` is clean-room and carries no such line;
- **images** — no `.png/.jpg/.gif/.svg/.ico/.webp` is tracked, so no logo can
  be redistributed;
- **binaries** — no `.so/.a/.o/.elf/.bin/.ifs/.img/.pyc` is tracked;
- **secrets** — AWS, GitHub, OpenAI-style, Slack token patterns and PEM private
  key headers.

The two blocked rows do not weaken the scan: `submission` still fails if a
future capture is accompanied by an SDP header or a binary.

## 3. Reproduce

```sh
make submission
```

Exit 0 means every checkable obligation holds and the QNX-dependent ones are
labelled. Exit 1 prints the failing file and the rule it broke. The check is
part of `make ci`, so the licence, the disclosure and the scan cannot regress.

## 4. What would turn the blocked rows green

Exactly the P4-03 procedure already committed in `docs/QNX-EVIDENCE.md` §4:
install SDP 8.0, capture `uname -a`, image hash and QEMU command line,
cross-build `permute-capture` and the scenarios with `qcc`, run them under
QEMU, commit the traces with a manifest, and replace the **not available**
cells. No code change is required.
