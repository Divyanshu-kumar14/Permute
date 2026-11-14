<!-- SPDX-License-Identifier: Apache-2.0 -->

# Permute

**Deterministic simulation testing for QNX message-passing services.**

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** Tier 0 complete (phase P1). The kernel model, scheduler, priority inheritance, timers, trace codec, replay, worker and CLI are built and tested. **Target conformance is pending a QNX SDP** — see [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md). The executor that runs a scenario source end to end is not built yet; [`docs/LIMITS.md`](docs/LIMITS.md) is the authoritative list.

---

## What it is

QNX services fail in ways that only appear under a specific schedule: a pulse
sent while the server is between arming and blocking, a client that times out
and leaves a late reply pointing at freed state, a priority that never reverts.
On a developer's desk these do not reproduce; in the field they are rare hangs.

Permute runs the same service code against a simulated microkernel where every
scheduling decision is a function of a seed. A run is reproducible, an
artifact replays it, and a different schedule is one seed away.

It is **not** an emulator. It does not execute QNX binaries, and it does not
model hardware. It models the IPC and scheduling semantics that
message-passing services depend on.

## Quick start

```sh
make build          # compile the harness
make test           # unit, integration, scenarios, gates
tools/permute init demo
cd demo
tools/permute run
tools/permute show --seed 0
```

Expected output is in [`QUICKSTART.md`](QUICKSTART.md). A green `permute run`
means the seeded workload completed with no crash, no budget hit, and no
invariant violation.

## What works today

| Area | What is built |
|---|---|
| Kernel objects | handles with 32-bit generations, static pools, deterministic teardown |
| IPC | send, receive, reply, error; priority-ordered queue; payload copied at send |
| Pulses and events | local coids, short-buffer `EFAULT` and drop, secure-event handles, disconnect notification |
| Scheduling | 256-bucket ready queue with an occupancy bitmap, FIFO within priority, RR quantum, `SchedYield` |
| Priority inheritance | sticky message-driven PI with no revert on reply, multi-hop chains, ceiling clamp, reverting mutex PI |
| Time | shared timer heap, `TimerTimeout` masks with the non-matching latch, virtual clock, `_NTO_CHF_UNBLOCK` |
| Trace | `.pmt` chunks with CRC32C and a BLAKE3-64 chain, `run_digest`, NDJSON and Chrome export, truncation salvage |
| Replay | from seed and from artifact, manifest refusal with exit 4, digest mismatch with exit 11 |
| Execution | one process per seed, crash isolation, wall budget, batch leases, sharding |
| CLI | `init`, `build`, `run`, `show`, `replay`, `shrink`, `clean`, `--json`, closed-schema config, the exit-code matrix |
| Conformance | `make conformance`, `make conformance-record`, canonical trace comparison, 11/11 timeout masks |
| Scenarios | seven workloads including sensor fanout and every `TimerTimeout` mask bit |
| Executor | cooperative baton over host threads; guest `.so` loaded, bound, and scheduled from the ready queue |
| Fault injection | drop, duplicate, delay, timeout, and kill at send/receive/reply/pulse, materialised from `D_FAULT` |
| Invariant oracles | seven post-hoc predicates over the trace: deadlock (with the wait-for cycle), lost pulse, unbounded block, priority inversion, starvation, orphan reply, queue depth |
| Shrinker | causal pre-slice, stimulus ddmin, window minimisation, tape elimination, fault materialisation; 1,500 executions / 90 s; `proven` or `budget_limited` |
| Bug benchmark | six seeded defects (B1–B6) with one-change patches; 6/6 found and 0 false positives on 10,000 fixed seeds, in ~31 s |
| Conformance | 14 dual-target scenario sources, canonical JSONL, Mazurkiewicz `tracecmp`; 12 execute against committed references, 2 compile-only; QNX capture pending the SDP |
| Mutation | twelve PRD §14.4 kernel mutants behind compile-time defines; 12/12 killed by the conformance set, focused fixtures, or targeted unit tests |

## What is not built yet

- **Scenario execution is opt-in.** `permute run` still executes the built-in
  seeded workload by default; `permute run --guest PATH` executes a compiled
  scenario through the executor. The scenario loader is not yet wired into the
  worker's default path.
- **Mutexes and signals are not interposed**, so `s07` compiles but is skipped
  at execution.
- **Target conformance.** The harness and the dual-target sources are in
  place, but the committed references are simulator output. Comparing against
  real QNX 8.0 needs the SDP and a QEMU boot; see
  [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md).
- **The HTML report.** Planned for P2.

## Requirements

- Linux x86_64 (aarch64 and musl are nightly CI targets)
- GCC 12+ or Clang 16+, CMake 3.28+, Ninja, Python 3.12+
- No QNX licence for the simulator. The SDP is needed only to capture target
  traces and to build the target side of conformance.

## Documentation

| Document | Owner | Status |
|---|---|---|
| [`QUICKSTART.md`](QUICKSTART.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | current |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | living specification |
| [`docs/DECISIONS.md`](docs/DECISIONS.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | authoritative |
| [`docs/LIMITS.md`](docs/LIMITS.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | authoritative |
| [`docs/DETERMINISM.md`](docs/DETERMINISM.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | normative |
| [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | blocked on SDP |
| [`docs/FAQ.md`](docs/FAQ.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | current |
| [`gate/`](gate/) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | per-task evidence |

## Licence

Apache-2.0. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). No vendor headers
are shipped; the ABI declarations are written from public documentation
(ADR-011).
