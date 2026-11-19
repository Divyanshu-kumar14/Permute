<!-- SPDX-License-Identifier: Apache-2.0 -->

# Reproduce

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** host flow verified on the reference machine (2026-11-14). The QNX 8.0 target flow was executed on 2026-11-17; its build and serial logs are committed. Target oracle capture is not executed and is labelled as such.
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
| `make gate2` | the G2 scorecard: conformance, outcome class, mutation, defects, shrink p95, NFR-2..6 and NFR-12; writes `gate/gate2-scorecard.md` | **79 s** |
| `make examples` | builds each example from a clean copy and compares its output with the committed `expected.txt` | **about 8 s** |
| `make asan` | AddressSanitizer + UBSan build and suite | runs after C changes |
| `make bench` | NFR-2..6 measurements; writes `bench/results.md` | hardware-dependent |
| `make shrink-bench` | G2 shrink performance over 20 failures; writes `bench/shrink-results.md` | **about 10 s** (p95 0.149 s on the reference machine) |

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

`make gate2` is allowed to be red for a documented reason. It reports
conformance equivalence separately from outcome-class matching: all ten
executed mandatory scenarios are equivalent (PASS, `>= 10/12`), but C11 and C13
cannot execute (LIMITS #60), so the outcome-class row prints `10/12 executed`
and FAILs. Every other row — mutation, defects, shrink p95, shrink events,
NFR-2..6 and NFR-12 — passes on the reference machine. Exit 0 means G2 is met;
exit 1 with only that row failing means the gate is correctly reporting the
QNX-dependent gap, not a regression.

### Install and package

```sh
make
sudo make install
permute --version        # permute 0.1.0 (git <sha>, kernel model 1)
sudo make uninstall      # zero residue
make dist                # dist/permute-0.1.0.tar.gz + dist/SHA256SUMS
```

`make install` writes a manifest at `<prefix>/lib/permute/install-manifest.txt`
and `make uninstall` removes exactly those files, pruning empty directories. A
staged install uses the standard pair:
`make install DESTDIR=/tmp/pkg PREFIX=/usr/local`. The tarball is built from a
clean commit with `git archive`; an extracted tarball has no `.git`, and
`permute-version.txt` carries the substituted SHA so `--version` still names the
commit.

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

### 4.1 Clean-machine dry run (P4-03)

The acceptance flow is one command:

```sh
tools/clean_room.sh
```

It exports `git archive HEAD` (a dirty tree is refused, so the evidence names a
commit), extracts the tarball into an empty directory, and runs `make build`,
`make ci`, `make examples`, `make install`, `permute --version`,
`init`/`build`/`run` from the installed tree, and `make uninstall` — all inside
a fresh network namespace created with `unshare -rn`. Before any build it
asserts that an outbound TCP connection and a DNS lookup both fail, so a
machine that silently has network access cannot produce a false pass. It fails
if the installed prefix survives uninstall, and if the total exceeds 900 s.

Measured on the reference machine at commit `3417926`:

| Phase | What it runs | Wall |
|---|---|---|
| `build` | `make build` (100 targets) | **1.4 s** |
| `ci` | `make ci`: unit, codec, replay, interposition, determinism (1,000 runs), coverage, docs, quickstart, examples, licence, CI-check | **189.2 s** |
| `examples` | `make examples`: three examples from clean copies | **3.7 s** |
| `install` | `make install PREFIX=<temp>` (39 files) | **0.2 s** |
| `version` | installed `permute --version` | **0.07 s** |
| `project` | installed `init` → `build` → `run` | **0.9 s** |
| `uninstall` | `make uninstall PREFIX=<temp>` | **0.09 s** |
| **total** | | **195.5 s** (3 min 16 s) |

```text
offline:  no outbound TCP connection is possible
routes:   0 route line(s)
installed files: 39; uninstall residue: 0
version: permute 0.1.0 (git 3417926, kernel model 1)
clean-room: PASSED (195457 ms inside the namespace, 195612 ms wall, limit 900 s)
```

3 min 16 s against the 15 min budget. This satisfies NFR-9 (full run with no
network) and NFR-10 (no QNX licence; no SDP is installed anywhere in the flow).
What it does not prove: a different CPU, a different distro, musl or aarch64 —
those are the nightly CI matrix (NFR-11) — and target fidelity, which needs the
SDP (section 5).

---

## 5. QNX target flow

The target flow was executed on 2026-11-17 against QNX SDP 8.0. The committed
evidence is in [`docs/evidence/qnx/`](evidence/qnx/):

| Item | Value |
|---|---|
| SDP | QNX Software Development Platform **8.0** (baseline `com.qnx.qnx800/8.0.0.00141T202311271501L`) |
| Target | **x86_64** under QEMU 11.1.3 with KVM |
| Toolchain | `qcc -Vgcc_ntox86_64` (GCC 12.2.0) |
| Build | 14/14 scenarios built ([`build.log`](evidence/qnx/build.log)) |
| Run | 14/14 `rc=0` in booted `procnto-smp-instr` ([`serial.log`](evidence/qnx/serial.log)) |
| Image hashes and kernel string | [`manifest.log`](evidence/qnx/manifest.log) |
| QEMU command line | [`qemu.log`](evidence/qnx/qemu.log) |

Reproduce the build and run:

```sh
tools/qnx_build_scenarios.sh          # qcc build, writes build/qnx/
# boot the QNX 8.0 image under QEMU and execute the scenario binaries;
# the procedure and image inputs are recorded in docs/QNX-EVIDENCE.md §5.
```

This is a genuine QNX 8.0 build and target execution: each scenario asserts its
own outcome and returns 0. It is **not** a trace comparison. Oracle capture is a
separate step: `permute-capture` does not exist yet, so
`conformance/oracle/qnx/` holds the procedure rather than traces, and
`make conformance` continues to report a simulator oracle. The target
self-check and the simulator regression suite are independent evidence.

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
