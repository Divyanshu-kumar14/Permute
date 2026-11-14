<!-- SPDX-License-Identifier: Apache-2.0 -->

# Reproduce

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** host flow verified on the reference machine (2026-11-14). The QNX target flow is specified but **not executed**; no SDP is installed.
**Companion documents:** [`QUICKSTART.md`](../QUICKSTART.md) (first run), [`CONFORMANCE.md`](CONFORMANCE.md) (capture procedure), [`LIMITS.md`](LIMITS.md) (scope).

This document is the shortest path from a clean clone to the evidence, and the
number each step is expected to print. It is written so a reviewer can follow
it without network access and without a QNX licence.

---

## 1. Host prerequisites

| Tool | Minimum | Tested |
|---|---|---|
| Linux | x86_64, glibc | CachyOS, kernel 7.3.3 |
| C compiler | GCC 12 or Clang 16 | GCC 16.2.1, Clang 23.1.5 |
| CMake | 3.28 | 4.5.0 |
| Ninja | any | 1.13.2 |
| Python | 3.12 (uses `tomllib`) | 3.15.2 |
| `make` | GNU make | present |

No QNX SDP is needed for the simulator. No Python package is installed: the CLI
uses the standard library only. musl and aarch64 are nightly CI targets, not host
requirements.

---

## 2. Clean build and test

```sh
git clone <repository> Permute
cd Permute
make build
make test
```

`make build` configures `build/` with CMake + Ninja and compiles the harness. It
is incremental after the first run. `make test` runs the C unit suites, the
Python suites, the scenario build, and the build gates. Success looks like:

```
executor: 32 checks, 0 failure(s) (6 scenario(s) executed, 1 skipped)
conformance: 12 executed, 2 skipped, 0 failure(s); mandatory 10/12 executed; oracle: simulator reference (not a QNX capture)
bug benchmark: 6/6 found, 0 false positive(s) on the fixed build
docs-check: PASSED
```

Every suite line must end in `0 failure(s)`. Two conformance scenarios are
compile-only: C11 needs process death, C13 needs a mutex shim (LIMITS #60).

---

## 3. The gates

| Command | What it proves | Time on the reference machine |
|---|---|---|
| `make ci` | the merge gate: test, docs, quickstart, licence, determinism, coverage, CI-check | **3 min 29 s** (209 s) |
| `make gate1` | the G1 scorecard: re-runs all nine criteria and writes `gate/gate1-scorecard.md` | **3 min 15 s** (195 s) |
| `make asan` | AddressSanitizer + UBSan build and suite | runs after C changes |
| `make bench` | NFR-2..6 measurements; writes `bench/results.md` | hardware-dependent |
| `make shrink-bench` | G2 shrink performance over 20 failures; writes `bench/shrink-results.md` | **about 10 s** (p95 0.381 s on the reference machine) |

`make ci` prints the determinism result twice by design: a 32-run quick check
during `make test` and the full 1,000-run check in the `determinism` target.

Expected `make gate1`:

```
Replay determinism        100% identical         1000/1000 same-seed                PASS
Conformance scenarios     12 executed, 0 failures 12 executed, 0 failures            PASS
Bug benchmark             6/6, 0 false positives 6/6 found, 0 FP                    PASS
Mutation score            >= 10/12 killed        12/12 killed                       PASS
Shard determinism         standalone == full     shard 2/4 byte-identical           PASS
Determinism self-check    exit 5 + both digests  planted exit 5 over violation      PASS
Report formats            4 valid, offline HTML  JUnit, SARIF 2.1.0, HTML, JSON     PASS
Context switch            <= 4000 ns             888-1182 ns                        PASS
ASan / UBSan              clean                  clean                              PASS
exit 0
```

The scorecard is regenerated, not read from a previous run. If a row fails, the
script prints the tail of `build/gate1/<row>.log` and exits non-zero.

---

## 4. Offline and container use

- `make ci` needs no network. The repository vendors the SARIF schema and
  commits the RNG vectors; the CLI has no third-party dependencies.
- `docker/Dockerfile.musl` builds the musl image used by the nightly CI job. It
  needs network once for the base image and packages, not for the test run.
- A container run should mount the checkout read-only and provide no
  credentials. A guest object runs in the host process; this is not a sandbox
  (see [`SECURITY.md`](../SECURITY.md)).
- The nightly soak is sharded and reproducible by hand:
  `tools/determinism_check.py --seeds 1000 --runs 1000 --shard i/8 --jobs 4
  --label aslr-on --json-out soak-aslr-on-i.json`. Run the second
  configuration under `setarch $(uname -m) -R`, then aggregate with
  `tools/soak_report.py --dir soak --require-labels aslr-on,aslr-off
  --expected-per-label 1000000`. The report fails on a missing shard, a short
  total, or any digest mismatch. Only the thread backend exists (LIMITS #88).

---

## 5. QNX target flow (not executed)

The target side is external input. The required versions are:

| Item | Value |
|---|---|
| SDP | QNX Software Development Platform **8.0** |
| Target | **x86_64** under QEMU |
| Toolchain | `qcc -Vgcc_ntox86_64` (GCC 12 base) |

The intended sequence is:

1. Install the SDP and a bootable QEMU image.
2. Build the capture agent and each `conformance/scenarios/c*.c` with `qcc`.
   The source selects vendor headers under `__QNXNTO__`; its `main` calls
   `permute_main`.
3. Run each binary under QEMU; the agent writes canonical JSONL and the outcome
   class.
4. Commit the captures under `conformance/oracle/qnx/` with a `manifest.toml`.
5. Run `make conformance`; the report says `oracle: QNX capture`.

Status: **not executed.** `qcc` and the SDP are absent here, `permute-capture`
is not built, and `conformance/oracle/qnx/` contains only its README. Until the
capture exists, every conformance result is a simulator regression check, not
target evidence. [`CONFORMANCE.md`](CONFORMANCE.md) and
`conformance/oracle/qnx/README.md` carry the procedure and the honest status.

---

## 6. Common failures

| Symptom | Cause | Fix |
|---|---|---|
| `fatal error: sys/iofunc.h: No such file` | you tried to build the Tier 2 probe; resource managers are dropped | expected; see DECISIONS §4.1 |
| `s07_priority_ceiling: skipped` | glibc lacks `PTHREAD_PRIO_PROTECT` and the mutex shim is pending | expected (LIMITS #37) |
| `gcov: cannot open ...` | wrong invocation | `gcov --json-format <path>/foo.c.gcno`; `gcov -o dir src` does not work here |
| musl build fails on `__libc_malloc` | the interposed allocator is glibc-only | configure with `-DPERMUTE_NO_INTERPOSED_ALLOC=ON` |
| `make gate1` row fails on this machine | a hardware-dependent row (context switch) or a real regression | read `build/gate1/<row>.log`; compare against `bench/results.md` |
| `permute doctor` is missing | it is a planned SHOULD (FR-9.6), not built | use the prerequisite table above |

---

## 7. What to keep

If you are reproducing for a review, keep:

- the commit (`git rev-parse HEAD`), which `bench/results.md` and the scorecard
  also name;
- `gate/gate1-scorecard.md`;
- `bench/results.md`;
- the suite summary lines from `make ci`, not the full log.

These four are enough to check the claim without trusting the machine that
produced it.
