<!-- SPDX-License-Identifier: Apache-2.0 -->

# Permute: interactive 90-second evaluator walkthrough

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** current for v0.1.0 — every command below was executed on a Linux
x86_64 workstation and the output blocks are copied from that run.

**Audience:** hackathon judges, embedded safety auditors, systems evaluators.
**Execution context:** standalone Linux x86_64 workstation. No target hardware,
no QNX licence, and no proprietary toolchain is required for any step before
the target log inspection.

`<checkout>` stands for the absolute path to your clone; the CLI prints it.
The whole script — build, run, shrink, replay — takes a couple of seconds on
the reference workstation. Ninety seconds is the budget with the build included.

---

## 1. Quick build

```sh
make build
make symcheck    # verifies the 33-symbol QNX Neutrino interposition ABI
```

```text
symcheck: PASSED (33 symbol(s) defined)
determinism gate: PASSED (no real time, entropy, identity, or unordered containers)
```

`make symcheck` is the gate that keeps the interposition surface honest: it
checks that every symbol declared in `abi/permute_iface.def` is defined by the
guest archive, and that the kernel model never reads real time, entropy,
process identity, or unordered containers.

---

## 2. Scaffold a project and arm a fault

```sh
tools/permute init eval_demo
```

```text
initialised eval_demo
  permute.toml, scenarios/smoke.c, symbol-audit.txt, header-gaps.md
next: cd eval_demo && permute run
```

Replace the scaffolded smoke scenario with the in-tree request/reply example:

```sh
cp examples/hello-ipc/scenarios/smoke.c eval_demo/scenarios/smoke.c
```

Now write the project configuration. The first rule is the failure: `send:drop@1`
drops 100% of sends, so the server never receives and the client waits forever.
The remaining six rules have probability `0`, so they never fire — they exist so
the shrinker has real, redundant inputs to discard in step 4.

```sh
cat > eval_demo/permute.toml <<'TOML'
# SPDX-License-Identifier: Apache-2.0
[project]
name = "eval_demo"
version = "0.1.0"

[run]
seeds = "0-2"
jobs = 1
wall_ms = 2000

[kernel]
max_events = 200000
max_virtual_ms = 60000
scheduler = "priority-fifo"
seed_policy = "random"

[faults]
inject = ["send:drop@1", "pulse:drop@0", "recv:timeout@0", "reply:delay@0",
          "recv:delay@0", "pulse:delay@0", "send:delay@0"]

[[invariant]]
id = "dl"
kind = "deadlock"
severity = "error"
TOML
```

---

## 3. Build and run: the failure appears deterministically

```sh
tools/permute build --config eval_demo/permute.toml
tools/permute run --config eval_demo/permute.toml \
  --guest eval_demo/.permute/build/proc_smoke__0.so
```

```text
built <checkout>/eval_demo/.permute/build/proc_smoke__0.so
  deadlock[dl] seed=0: 3 live thread(s) blocked, no wait cycle
  deadlock[dl] seed=1: 3 live thread(s) blocked, no wait cycle
  deadlock[dl] seed=2: 3 live thread(s) blocked, no wait cycle
3 seeds: 0 clean, 0 crash, 0 budget, 3 violation, 0 internal
report: <checkout>/eval_demo/.permute/run/report.json
reports: <checkout>/eval_demo/.permute/run/junit.xml, <checkout>/eval_demo/.permute/run/sarif.json, <checkout>/eval_demo/.permute/run/index.html
```

The command exits `1` (the exit code is the product contract: `0` success,
`1` violation, `2` usage/config). Each failing seed writes a failure artifact
under `eval_demo/.permute/run/artifacts/` — seed, faults, stimuli, invariant,
and the finding — which is the shrinker's input.

The failing trace holds twelve records:

```sh
tools/permute show --config eval_demo/permute.toml --seed 0
```

```text
<checkout>/eval_demo/.permute/run/traces/seed-0.pmt
  12 records in 1 chunk(s)
  digest fa65babdb51074f5
```

---

## 4. Shrink: discard the inputs that do not matter

```sh
tools/permute shrink eval_demo/.permute/run/artifacts/seed-0.json
```

```text
shrink: 7 -> 1 input(s), proven, 6 exec(s)
  causal-slice: 7 -> 7
  fault-materialisation: 7 -> 1
artifact: eval_demo/.permute/run/artifacts/seed-0.shrunk.json
```

The shrinker removes inputs, never events, and re-executes every candidate
against the simulator. `proven` means the reduced input still reproduces the
same finding; a budget-limited run says so instead of claiming minimality. The
twenty-case benchmark in [`bench/shrink-results.md`](../bench/shrink-results.md)
reports a p95 of 0.149 s with the same reduction shape.

Repeating the command on the same artifact produces byte-identical output.

---

## 5. Replay: the same seed, the same digest

Verify the recorded trace — chunk chain, event order, and run digest:

```sh
tools/permute replay artifact eval_demo/.permute/run/traces/seed-0.pmt
```

```text
verified 12 records in 1 chunk(s)
run_digest fa65babdb51074f5
violation comparison: exit 10 reserved for a paired run
```

Re-run a seed twice in separate processes and compare the digests:

```sh
tools/permute replay seed 0
```

```text
seed 0: 2 runs, one digest 3620eff1f3e7ab51
714 records
artifact contains no host data (created_unix_ns is zero)
```

`replay seed` exits `5` on a digest difference. The digest is a function of the
seed, the scenario, and the kernel model — not of wall-clock time, address
layout, or allocation history. The same project built and run a second time in a
separate directory reproduces `fa65babdb51074f5` exactly.

The 1,000/1,000 same-seed figure in the README comes from the gate run recorded
in [`gate/gate1-scorecard.md`](../gate/gate1-scorecard.md).

---

## 6. Dual-target proof: the same scenario on booted QNX 8.0

The scenario sources are not host-only. Every file under
`conformance/scenarios/` compiles with the host ABI and with the vendor `qcc`
from QNX SDP 8.0, and the committed serial log is from a booted QNX 8.0 x86_64
image running `procnto-smp-instr` under QEMU:

```sh
grep '^RESULT ' docs/evidence/qnx/serial.log
```

```text
RESULT c01_request_reply rc=0
RESULT c02_priority_order rc=0
RESULT c03_fifo_equal rc=0
RESULT c04_pulse_vs_message rc=0
RESULT c05_pulse_while_processing rc=0
RESULT c06_pulse_only rc=0
RESULT c07_timeout_unblock_esrch rc=0
RESULT c09_reply_after_death rc=0
RESULT c10_death_before_receive rc=0
RESULT c11_server_death rc=0
RESULT c12_message_pi rc=0
RESULT c13_mutex_pi_chain rc=0
RESULT c14_rr_timeslice rc=0
RESULT c15_channel_destroy_senders rc=0
```

All 14 conformance scenarios return exit code 0 on the target operating system.
The cross-build log is [`docs/evidence/qnx/build.log`](evidence/qnx/build.log);
the image hashes, QEMU invocation, and procedure are in
[`docs/QNX-EVIDENCE.md`](QNX-EVIDENCE.md).

This step proves that the scenarios pass on genuine QNX 8.0. It is a scenario
self-check, not a trace comparison; scheduler traces are compared on the host
against the simulator references in [`conformance/REPORT.md`](../conformance/REPORT.md).

---

## 7. Clean up

```sh
rm -r eval_demo
```

`eval_demo/` is scratch: `eval_demo/.permute/` is the run directory, the rest is
the scaffolded project. Nothing outside it is modified.
