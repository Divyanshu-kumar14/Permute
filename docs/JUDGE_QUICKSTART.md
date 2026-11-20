# Permute: quick start for judges

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** current for v0.1.0 — every command below was executed on a Linux x86_64 workstation; QNX evidence is committed and read without an SDP.

This takes five minutes and needs no QNX licence or target hardware. The demo path is `init → build → run → shrink → replay` in a scratch project; the QNX evidence is committed separately.

> Permute is deterministic simulation testing for QNX 8.0 message-passing services. It is a host tool; QNX use is evidenced by a real cross-build, a real boot, and 14/14 scenarios returning `rc=0` inside a booted QNX 8.0 image.

## 1. Build and verify the ABI

```sh
git clone https://github.com/Divyanshu-kumar14/Permute.git && cd Permute
make build && make symcheck
```

Expected:
```
symcheck: PASSED (33 symbol(s) defined)
```

`make symcheck` is the gate that keeps the 33-symbol QNX Neutrino interposition surface honest.

## 2. Scaffold a project and arm a fault

```sh
tools/permute init eval_demo
cp examples/hello-ipc/scenarios/smoke.c eval_demo/scenarios/smoke.c
cat > eval_demo/permute.toml <<'TOML'
[project]
name = "eval_demo"
version = "0.1.0"

[run]
seeds = "0-2"
jobs = 1
wall_ms = 2000

[kernel]
max_events = 200000
scheduler = "priority-fifo"

[faults]
inject = ["send:drop@1", "pulse:drop@0", "recv:timeout@0", "reply:delay@0",
          "recv:delay@0", "pulse:delay@0", "send:delay@0"]

[[invariant]]
id = "dl"
kind = "deadlock"
severity = "error"
TOML
```

The first rule drops 100% of sends, so the server never receives and the client waits forever — the failure appears on every seed.

## 3. Build and run: the failure appears deterministically

```sh
tools/permute build --config eval_demo/permute.toml
tools/permute run --config eval_demo/permute.toml \
  --guest eval_demo/.permute/build/proc_smoke__0.so
```

Expected:
```
  deadlock[dl] seed=0: 3 live thread(s) blocked, no wait cycle
  deadlock[dl] seed=1: 3 live thread(s) blocked, no wait cycle
  deadlock[dl] seed=2: 3 live thread(s) blocked, no wait cycle
3 seeds: 0 clean, 0 crash, 0 budget, 3 violation, 0 internal
```

The command exits `1` (contract: `0` success, `1` violation, `2` usage). Each failing seed writes its artifact under `eval_demo/.permute/run/artifacts/`.

## 4. Shrink: discard the inputs that do not matter

```sh
tools/permute shrink eval_demo/.permute/run/artifacts/seed-0.json
```

Expected:
```
shrink: 7 -> 1 input(s), proven, 6 exec(s)
```

`proven` means the reduced input still reproduces the same finding.

## 5. Replay: the same seed, the same digest

```sh
tools/permute replay artifact eval_demo/.permute/run/traces/seed-0.pmt
tools/permute replay seed 0
```

Expected:
```
verified 12 records in 1 chunk(s)
run_digest fa65babdb51074f5
seed 0: 2 runs, one digest 3620eff1f3e7ab51
```

`replay seed` exits `5` on a digest difference. The digest is a function of seed, scenario, and kernel model — not wall-clock, ASLR, or allocation history.

## 6. QNX 8.0 target proof (committed, no SDP needed to read)

Every scenario source compiles for the host simulator **and** for target QNX 8.0:

| Claim | Evidence |
|---|---|
| 14/14 cross-compile with vendor `qcc -Vgcc_ntox86_64` | [`docs/evidence/qnx/build.log`](docs/evidence/qnx/build.log) |
| 14/14 return `rc=0` inside a booted QNX 8.0 x86_64 image (`procnto-smp-instr`, QEMU) | [`docs/evidence/qnx/serial.log`](docs/evidence/qnx/serial.log) |
| SDP version, `uname`, image SHA-256, QEMU invocation, commit | [`docs/QNX-EVIDENCE.md`](docs/QNX-EVIDENCE.md) |

Serial excerpt:
```
RESULT c01_request_reply rc=0
... 14/14 ...
```

The QNX run is a scenario self-check, not a trace comparison; target trace capture is not claimed.

## 7. Measured results

| Metric | Result | Evidence |
|---|---|---|
| Seeded defect detection | 6/6 defects, 0 false positives over 60,000 fixed-build runs | [`gate/p2-04-benchmark.md`](gate/p2-04-benchmark.md) |
| Mutation kill rate | 12/12 mutants killed | [`gate/p2-06-mutation.md`](gate/p2-06-mutation.md) |
| Same-seed replay | 1,000/1,000 identical digests | [`gate/gate1-scorecard.md`](gate/gate1-scorecard.md) |
| Determinism soak | 2,000,000 same-seed runs, 0 mismatches | [`gate/p3-05-nightly-soak.md`](gate/p3-05-nightly-soak.md) |
| Automated shrinking | 20 artifacts → 12-event traces, p95 0.149 s | [`bench/shrink-results.md`](bench/shrink-results.md) |
| IPC throughput | 973,935 msg/s; 20,000 seeds in 29.0 s on 16 workers | [`bench/results.md`](bench/results.md) |

## 8. What is not done yet

| Boundary | State | Where disclosed |
|---|---|---|
| Target trace capture (`permute-capture`) | Not implemented; QNX run is a scenario self-check | [`docs/QNX-EVIDENCE.md`](docs/QNX-EVIDENCE.md) §4 |
| Fiber backend | Not implemented; thread backend only | [`docs/LIMITS.md`](docs/LIMITS.md) #88 |
| Mutex guest scenario `s07_priority_ceiling` | Skipped on host (glibc `ENOTSUP`) | [`docs/REPRODUCE.md`](docs/REPRODUCE.md) §6 |

These are scope boundaries, not defects. Nothing is deleted to look better.

## 9. Run it yourself

```sh
make build
make test
make quickstart   # init → run → show → replay → guest, in a scratch project
make gate1        # full benchmark, mutation, sanitizers, scorecard
```

No QNX SDP is needed for steps 1–5. Steps 6 need `~/qnx800` (SDP 8.0) and QEMU; the committed logs are readable without them.

Licensed Apache-2.0. See [`LICENSE`](LICENSE).
