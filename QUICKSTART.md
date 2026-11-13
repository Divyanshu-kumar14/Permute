<!-- SPDX-License-Identifier: Apache-2.0 -->

# Quick start

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** current for the P1 build.

From a clean checkout to a green run takes about a minute. Every command below
is the command actually run; the output blocks are real.

---

## 1. Prerequisites

```sh
cc --version          # GCC 12+ or Clang 16+
cmake --version       # 3.28+
ninja --version
python3 --version     # 3.12+
```

On Debian/Ubuntu:

```sh
sudo apt-get install -y build-essential cmake ninja-build python3
```

No QNX licence is needed for the simulator.

## 2. Build and test

```sh
make build
make test
```

`make test` runs the C unit suites, the Python suites, the determinism and
coverage gates, and builds the seven scenarios. It prints one line per suite;
every line must end in `0 failure(s)`.

## 3. Create a project

```sh
tools/permute init demo
```

```text
initialised demo
  permute.toml, scenarios/smoke.c, symbol-audit.txt, header-gaps.md
next: cd demo && permute run
```

`init` also writes `symbol-audit.txt` (the interposed surface and the live
gate result) and `header-gaps.md` (what is not verified without a QNX SDP).

## 4. Run

```sh
cd demo
tools/permute run
```

```text
3 seeds: 3 clean, 0 crash, 0 budget, 0 violation, 0 internal
report: demo/.permute/run/report.json
```

The report is JSON; `--json` prints the same summary to stdout. A run exits 0
when every seed is clean or budget-limited, 1 when there is a crash or a
violation, and 7 when the harness itself is misconfigured.

## 5. Inspect a trace

```sh
tools/permute show --seed 0
```

```text
demo/.permute/run/traces/seed-0.pmt
  714 records in 1 chunk(s)
  digest 3620eff1f3e7ab51
```

Other formats:

```sh
tools/permute show --seed 0 --format ndjson > seed-0.ndjson
tools/permute show --seed 0 --format chrome --json > seed-0.json
```

The Chrome trace opens in Perfetto or `chrome://tracing`.

## 6. Replay the artifact

```sh
tools/permute replay artifact .permute/run/traces/seed-0.pmt
```

```text
verified 714 records in 1 chunk(s)
run_digest 3620eff1f3e7ab51
violation comparison is not implemented yet (exit 10 reserved)
```

Replay checks the chunk chain, the event ordering, and the digest. It refuses
a build, config, or kernel mismatch with exit 4 unless `--force` is given, and
refuses a digest mismatch with exit 11 unconditionally.

## 7. Reproduce a seed

```sh
tools/permute replay seed 42
```

This runs seed 42 twice in separate processes and compares the digests. A
difference exits 5; the two digests are printed.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `P3001: permute-run not found` | harness not built | `make build` from the checkout |
| `P1002: unknown key 'run.seedx'` | closed-schema config | fix the key; the message lists the valid ones |
| `batch ... busy, skipping` | another worker holds the lease | wait, or pass `--steal` if the owner is gone |
| `make asan` fails to configure | stale `build-asan` | `cmake -E remove_directory build-asan`, then `make asan` |
| `hypercall page: fallback` under ASan | ASan occupies the fixed candidates | expected; the sanitizer build uses the fallback table |

## Where to go next

- [`README.md`](README.md) — what the project is and what is built.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — how the pieces fit.
- [`docs/LIMITS.md`](docs/LIMITS.md) — what is deliberately not modelled.
- [`docs/FAQ.md`](docs/FAQ.md) — the questions reviewers ask first.
