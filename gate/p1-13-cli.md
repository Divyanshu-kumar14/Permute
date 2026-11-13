<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-13 report — CLI skeleton

**Date:** 2026-11-14
**Scope:** `init`, `build`, `run`, `show`, `replay`, `clean`, `--version`, `--json`, closed-schema config, symbol audit, header-gap report, exit-code matrix.
**Result:** **40 CLI checks, 0 failures**; `init` + `run` green in **0.09 s**; full `make test` green.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `cli/permute/__main__.py` | Commands, exit-code mapping, `--json`, harness hook |
| `cli/permute/config.py` | TOML load and closed-schema validation with `P####` errors |
| `cli/permute/report.py` | Load, summarise, write run reports |
| `tools/header_gap.py` | Header-gap report from the definition file |
| `tools/permute` | Launcher for a source checkout |
| `tests/python/test_cli.py` | 40 checks including the full matrix |

---

## 2. The verification

```
$ permute init demo
initialised demo
  permute.toml, scenarios/smoke.c, symbol-audit.txt, header-gaps.md
next: cd demo && permute run

$ cd demo && permute run
3 seeds: 3 clean, 0 crash, 0 budget, 0 violation, 0 internal
report: demo/.permute/run/report.json
```

Elapsed: **0.086 s** against the 60 s budget.

`symbol-audit.txt` contains the 17 modelled symbols with their groups, the archive path, and the gate result. `header-gaps.md` lists the surface and states plainly what is not verified without a QNX SDP.

---

## 3. The exit-code matrix

Every code is asserted through the executable, not by importing constants.

| Code | Trigger used by the test | Result |
|---|---|---|
| 0 | `--version` | 0 |
| 1 | `run` with `PERMUTE_CRASH_SEED=1` | 1 |
| 2 | unknown key in `permute.toml` | 2 |
| 3 | syntax error in `scenarios/smoke.c`, then `build` | 3 |
| 4 | `replay artifact` with a wrong build id | 4 |
| 5 | a fixture that changes its seed between runs | 5 |
| 6 | `run --max-wall-ms 1` with a spinning seed | 6 |
| 7 | `PERMUTE_RUNNER` pointing at a missing file | 7 |
| 10 | `internal-exit 10` (harness hook) | 10 |
| 11 | `replay artifact --digest 0` | 11 |
| 130 | SIGINT during a spinning run | 130 |

Codes 0–7, 11, and 130 are reached through real paths. Code 10 has no user-facing trigger until the oracle engine exists, so it is asserted through a hidden `internal-exit` command — the alternative was an untested branch in the contract.

### Two bugs the matrix found

**A missing harness binary was exit 2.** `_binary` raised `ConfigError`, which is a usage error. A missing `permute-run` is an environment problem, not something the user typed wrong; it now raises `HarnessError` and returns 7.

**SIGINT left the SUT spinning.** The CLI told its workers to terminate, but a worker was blocked in `waitpid` on a spinning child until the per-seed wall budget expired — 60 s later. The worker's signal handler now kills its child, and the CLI gives workers two seconds before `SIGKILL`. The test asserts the whole thing returns 130 promptly.

---

## 4. Configuration is a closed schema

Unknown keys are rejected at every level with a dotted path:

```
P1002: unknown key 'run.seedsx'
  fix: remove it, or use one of: jobs, seeds, wall_ms
```

Types, enums (`scheduler`, `seed_policy`, `severity`), seed-range syntax, and required fields are all checked. A typo in a budget that silently fell back to a default would surface much later as a result nobody can reproduce.

`ConfigError` carries a stable code, a cause, and a fix (FR-9.5). The CLI maps it to exit 2.

---

## 5. Scaffolding

`init` writes:

```
demo/
  permute.toml
  scenarios/smoke.c
  symbol-audit.txt
  header-gaps.md
  .gitignore
```

The audit and the gap report are generated from `abi/permute_iface.def` and the real archive at init time, so a fresh project starts with the coverage of the checkout that created it, not a stale template. `init` refuses a non-empty directory unless `--force` is given.

`build` compiles the smoke scenario through `make_guest.py` into `.permute/build/proc_smoke__0.so`, proving the guest toolchain path works from a scaffold. A compile failure exits 3 with the compiler output.

---

## 6. Boundary

| In scope | Out of scope |
|---|---|
| Scaffold, config, run, show, replay delegation, exit codes | `shrink`, `log`, `ls`, `merge`, `verify`, `cov` (later tasks) |
| `run` through the existing worker | Loading the scaffolded `.so` into the runner (#29) |
| `--json` on every implemented command | JUnit, SARIF, HTML reports (FR-9.3, later) |
| Launcher for a checkout | Installed console script / packaging (#30) |

---

## 7. Next

**P1-14 — scenarios.** The ping-pong, sensor-fanout, and named scenario sources (`s02_pi_chain`, `s03_timer_timeout`, `s04_pulse_drop`, `s06_unblock_pulse`, `s07_priority_ceiling`), all buildable for the simulator. That is where the pieces built so far are exercised together for the first time.
