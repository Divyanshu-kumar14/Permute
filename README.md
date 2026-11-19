<!-- SPDX-License-Identifier: Apache-2.0 -->

# Permute

[![ci](https://github.com/Divyanshu-kumar14/Permute/actions/workflows/ci.yml/badge.svg)](https://github.com/Divyanshu-kumar14/Permute/actions/workflows/ci.yml)
[![nightly](https://github.com/Divyanshu-kumar14/Permute/actions/workflows/nightly.yml/badge.svg)](https://github.com/Divyanshu-kumar14/Permute/actions/workflows/nightly.yml)

Deterministic simulation testing for QNX 8.0 message-passing services.

Permute runs QNX service code against a simulated Neutrino microkernel in which
every scheduling decision, message-queue order, pulse delivery, and timer expiry
is a deterministic function of a 64-bit seed. A failing schedule is replayed
byte-for-byte and reduced to a minimal causal input set.

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)  
**Status:** v0.1.0 — host simulator and QNX 8.0 target build verified; Apache-2.0.  
**Built for:** QNX Global Hackathon 2026 — Developer Tooling & Host Tools.

---

## Why

QNX services fail on timing: a pulse dropped during an unblock race, a reply that
arrives after the client dies, a priority-inheritance chain that never unwinds.
On a workstation these bugs disappear under a debugger. On target they surface as
intermittent hangs after long exposure.

Permute moves schedule exploration to the host. It interposes the QNX IPC and
scheduling contract, runs a campaign over seeds, records a verifiable trace, and
reports invariant violations post hoc. The same scenario sources build for the
host simulator and for QNX 8.0.

## How it works

- **QNX ABI shim** — 33 kernel symbols: `MsgSend`, `MsgReceive`, `MsgReply`,
  `MsgDeliverEvent`, `ChannelCreate`, `TimerTimeout`, pulses, and priority
  inheritance. The full surface is in [`docs/ABI.md`](docs/ABI.md).
- **Deterministic scheduler** — 256 priority levels, FIFO and round-robin
  queues, inheritance and ceiling protocols, and a virtual clock.
- **Fault injection** — seed-controlled drops, duplicates, reordering, delays,
  timeouts, and process death on the guest bridge.
- **Invariant oracles** — deadlock, priority inversion, pulse loss, unbounded
  block, and reply-to-dead-client checks over the completed trace.
- **Shrinker** — delta debugging (`ddmin`) removes inputs, never events, and
  re-executes every candidate to prove the reduction.
- **Trace format** — binary `.pmt` chunks with CRC32C and a BLAKE3-64 chain;
  `permute replay artifact` re-verifies the chain and run digest.

## Build

Requirements: Linux x86_64, GCC 12+ or Clang 16+, CMake 3.28+, Ninja, Python
3.12+.

```sh
make build
make test
```

`make test` runs the C and Python suites, the determinism and coverage checks,
the conformance set, and the documentation gate.

## Quick start

```sh
make quickstart   # init -> run -> show -> replay -> guest, in a scratch project
make examples     # build and compare the three IPC examples
```

A full failing campaign:

```sh
tools/permute init myproject
# add a fault rule and an invariant to myproject/permute.toml
tools/permute build --config myproject/permute.toml
tools/permute run --config myproject/permute.toml \
  --guest myproject/.permute/build/proc_smoke__0.so
tools/permute shrink myproject/.permute/run/artifacts/seed-0.json
tools/permute show --config myproject/permute.toml --seed 0
```

[`QUICKSTART.md`](QUICKSTART.md) documents every command and its output.

## QNX 8.0

Every conformance scenario has a host and a QNX build path. The QNX path selects
the vendor headers under `__QNXNTO__`; the host path links the Permute ABI.

- 14/14 scenarios cross-compile with `qcc -Vgcc_ntox86_64` from QNX SDP 8.0
  ([`docs/evidence/qnx/build.log`](docs/evidence/qnx/build.log)).
- 14/14 return `rc=0` inside a booted QNX 8.0 x86_64 image running
  `procnto-smp-instr` ([`docs/evidence/qnx/serial.log`](docs/evidence/qnx/serial.log)).
- The target run validates each scenario's own assertions. It is not a trace
  comparison; the simulator event sequence is checked by the conformance
  harness. See [`docs/QNX-EVIDENCE.md`](docs/QNX-EVIDENCE.md).

## Measured results

| Metric | Result | Evidence |
| :--- | :--- | :--- |
| Seeded defect detection | 6/6 defects, 0 false positives over 60,000 fixed-build runs | [`gate/p2-04-benchmark.md`](gate/p2-04-benchmark.md) |
| Mutation kill rate | 12/12 mutants killed | [`gate/p2-06-mutation.md`](gate/p2-06-mutation.md) |
| QNX 8.0 target execution | 14/14 scenarios `rc=0` | [`docs/evidence/qnx/serial.log`](docs/evidence/qnx/serial.log) |
| Automated shrinking | 20 artifacts → 12-event trace, p95 0.149 s | [`bench/shrink-results.md`](bench/shrink-results.md) |
| Determinism soak | 2,000,000 same-seed runs, 0 digest mismatches | [`gate/p3-05-nightly-soak.md`](gate/p3-05-nightly-soak.md) |
| Same-seed replay | 1,000/1,000 identical digests | [`gate/gate1-scorecard.md`](gate/gate1-scorecard.md) |
| Throughput | 973,935 msg/s; 20,000 seeds in 29.0 s on 16 workers | [`bench/results.md`](bench/results.md) |

## Documentation

| Document | Purpose |
| :--- | :--- |
| [`QUICKSTART.md`](QUICKSTART.md) | Setup and first run |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Microkernel model, virtual clock, scheduler |
| [`docs/ABI.md`](docs/ABI.md) | 33-symbol interposition surface |
| [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md) | Trace comparison and scenario suite |
| [`docs/DETERMINISM.md`](docs/DETERMINISM.md) | Determinism guarantees and trace hashing |
| [`docs/QNX-EVIDENCE.md`](docs/QNX-EVIDENCE.md) | QNX SDP 8.0 build and target run |
| [`docs/REPRODUCE.md`](docs/REPRODUCE.md) | Host and target reproduction steps |
| [`docs/LIMITS.md`](docs/LIMITS.md) | Modelling boundaries and open items |
| [`docs/FAQ.md`](docs/FAQ.md) | Common questions |

## Provenance

Permute is an original, clean-room implementation developed during the QNX
Global Hackathon submission window. No pre-existing simulator harness,
proprietary vendor headers, or third-party sample code is bundled; ABI
declarations are derived from publicly documented QNX Neutrino interfaces. See
[`NOTICE`](NOTICE).

## License

Apache-2.0. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).
