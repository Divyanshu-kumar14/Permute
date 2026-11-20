<!-- SPDX-License-Identifier: Apache-2.0 -->

# Quick start

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** current for the P1 build.

From a clean checkout to a green run takes about a minute. Every command below
is the command actually run; the output blocks are real. `<checkout>` stands in
for the absolute checkout path the CLI prints.

---

## 1. Prerequisites

```sh
cc --version          # GCC 12+ or Clang 16+
cmake --version       # 3.28+
ninja --version
python3 --version     # 3.11+
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
tools/permute init hello
```

```text
initialised hello
  permute.toml, scenarios/smoke.c, symbol-audit.txt, header-gaps.md
next: cd hello && permute run
```

`init` also writes `symbol-audit.txt` (the interposed surface and the live
gate result) and `header-gaps.md` (what is not verified without a QNX SDP).

## 4. Run

```sh
tools/permute run --config hello/permute.toml
```

```text
  dl: not evaluated - trace has no thread records (the built-in workload does not instrument threads)
3 seeds: 3 clean, 0 crash, 0 budget, 0 violation, 0 internal
report: <checkout>/hello/.permute/run/report.json
reports: <checkout>/hello/.permute/run/junit.xml, <checkout>/hello/.permute/run/sarif.json, <checkout>/hello/.permute/run/index.html
```

The report is JSON; `--json` prints the same summary to stdout. A run exits 0
when every seed is clean or budget-limited, 1 when there is a crash or a
violation, and 7 when the harness itself is misconfigured.

## 5. Inspect a trace

```sh
tools/permute show --config hello/permute.toml --seed 0
```

```text
<checkout>/hello/.permute/run/traces/seed-0.pmt
  714 records in 1 chunk(s)
  digest 3620eff1f3e7ab51
```

Other formats:

```sh
tools/permute show --config hello/permute.toml --seed 0 --format ndjson > seed-0.ndjson
tools/permute show --config hello/permute.toml --seed 0 --format chrome --json > seed-0.json
```

The Chrome trace opens in Perfetto or `chrome://tracing`.

## 6. Replay the artifact

```sh
tools/permute replay artifact hello/.permute/run/traces/seed-0.pmt
```

```text
verified 714 records in 1 chunk(s)
run_digest 3620eff1f3e7ab51
violation comparison: exit 10 reserved for a paired run
```

Replay checks the chunk chain, the event ordering, and the digest. It refuses
a build, config, or kernel mismatch with exit 4 unless `--force` is given, and
refuses a digest mismatch with exit 11 unconditionally.

## 7. Execute a scenario source

```sh
tools/permute build --config hello/permute.toml
tools/permute run --config hello/permute.toml --guest hello/.permute/build/proc_smoke__0.so
```

```text
3 seeds: 3 clean, 0 crash, 0 budget, 0 violation, 0 internal
report: <checkout>/hello/.permute/run/report.json
reports: <checkout>/hello/.permute/run/junit.xml, <checkout>/hello/.permute/run/sarif.json, <checkout>/hello/.permute/run/index.html
```

This is the same worker and the same trace, but the child process now loads
the compiled guest object and runs its `permute_main` on the simulated kernel.
The scaffolded smoke scenario returns immediately; replace its body with
service code to exercise IPC.

## 8. Inject a fault

Fault rules live in `permute.toml`. Each rule is
`site:action@probability[:delay_ns]`, where site is `send`, `recv`, `reply`, or
`pulse`, and action is `drop`, `duplicate`, `reorder`, `delay`, `timeout`, or
`kill`.

```toml
[faults]
inject = ["send:drop@0.01", "reply:delay@0.1:200000"]
```

```sh
tools/permute run --config hello/permute.toml --guest hello/.permute/build/proc_smoke__0.so
```

A rule with probability `1` fires every time; `0` never fires. Faults apply on
the guest bridge, so `permute run` refuses a configured rule without `--guest`
rather than reporting a clean run that never exercised it.

To try it on a scenario in this repository:

```sh
PERMUTE_GUEST=build/scenarios/proc_s01_hello__0.so \
PERMUTE_FAULTS=send:timeout@1 ./build/permute-run 1 /tmp/fault.pmt
```

```text
SEED=1
OUTCOME=DEADLOCK
GUEST_RC=-1
DIGEST=2f6670f746c4a002
RECORDS=15
```

The run reports `OUTCOME=DEADLOCK` and exits 1: the sender's message never
arrives, and nothing else in the scenario can release it.

## 9. Configure an invariant

```toml
[[invariant]]
kind = "deadlock"
severity = "error"

[[invariant]]
id = "ipc-deadline"
kind = "unbounded_block"
task = "guest"
state = "SEND"
max_virtual_ms = 250
severity = "error"
```

`permute run --guest ...` evaluates them over each trace and exits 1 when an
error-severity invariant fires. On a scenario with a two-thread reply cycle the
deadlock rule prints, for example:

```text
  deadlock[deadlock-0] seed=0: T1(REPLY 65537) -> T2(REPLY 65538) -> T1
3 seeds: 0 clean, 0 crash, 0 budget, 3 violation, 0 internal
```

That trace is from the oracle fixture in [`gate/p2-02-oracles.md`](gate/p2-02-oracles.md).

The unknown-key rule applies here too, and per kind: a `deadlock` rule with
`max_senders` is rejected. The built-in workload has no thread records, so the
thread-state oracles are reported as *not evaluated* rather than passing
silently; use `--guest` for those.

## 10. Shrink a failure

When an invariant fires, `permute run` writes an artifact under
`.permute/run/artifacts/`. Shrinking removes inputs, never events, and re-runs
every candidate:

```sh
tools/permute shrink <project>/.permute/run/artifacts/seed-0.json
```

It prints the reduction, the strategy lines, and the shrunk artifact path. A
reduction is reported as `proven` only when the reduced input re-executes.

The budget is 1,500 executions or 90 seconds, whichever comes first; `--max-execs`
and `--max-seconds` lower it. A run that exhausts the budget records
`minimality: "budget_limited"` and keeps the best candidate found. Repeating the
command on the same artifact produces byte-identical output.

## 11. Reproduce a seed

```sh
tools/permute replay seed 42
```

```text
seed 42: 2 runs, one digest 5150741151ed0d93
716 records
artifact contains no host data (created_unix_ns is zero)
```

This runs seed 42 twice in separate processes and compares the digests. A
difference exits 5; the shared digest and record count are printed.

---

## 12. Run the complete examples

```sh
make examples
```

This builds each project under `examples/` from a clean copy and compares its
output with the committed `expected.txt`:

| Example | What it shows |
|---|---|
| `hello-ipc` | one server, one client, three request/reply round trips |
| `db-server` | a key/value server, three clients at priorities 5/10/15, `MsgReply` and `MsgError` |
| `telemetry` | one server, three clients at priorities 5/15/25, pulses, a sampling timer, a client timeout, and a client cancellation |

`make examples` prints `PASSED` when all three match. It is the shortest path
from a clone to a real IPC run; the built-in workload in section 4 is only a
harness smoke test.

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
