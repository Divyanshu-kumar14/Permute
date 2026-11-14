<!-- SPDX-License-Identifier: Apache-2.0 -->

# Permute

**Deterministic simulation testing for QNX message-passing services.**

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** v0.1 feature-complete through P3. The kernel model, scheduler, priority inheritance, timers, executor, fault injection, oracles, shrinker, exploration, reports, benchmarks and the G1 scorecard are built and tested. **Target conformance is pending a QNX SDP** — see [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md) and [`docs/FIDELITY.md`](docs/FIDELITY.md). [`docs/LIMITS.md`](docs/LIMITS.md) is authoritative for what is not modelled.

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

## Install

```sh
make
sudo make install                 # /usr/local; PREFIX=... and DESTDIR=... work
permute --version
# permute 0.1.0 (git 32d9151, kernel model 1)
sudo make uninstall               # removes exactly what install created
```

`install.sh` writes a manifest under `<prefix>/lib/permute`; uninstall reads it,
so a staged `DESTDIR` install can be extended or removed without residue. The
installed launcher sets `PERMUTE_HOME`, bakes the git SHA and suppresses
bytecode, so a system install needs no checkout and leaves no `__pycache__`.
Release artifacts are `make dist`, which writes
`dist/permute-<version>.tar.gz` and `dist/SHA256SUMS` from a clean commit.

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
| CLI | `init`, `build`, `run` (sweep, `--explore`, `--verify`, `--shard`), `show`, `replay`, `shrink`, `clean`, `--json`, closed-schema config, the exit-code matrix |
| Scenarios | seven workloads including sensor fanout and every `TimerTimeout` mask bit |
| Executor | cooperative baton over host threads; guest `.so` loaded, bound, and scheduled from the ready queue |
| Fault injection | drop, duplicate, delay, timeout, and kill at send/receive/reply/pulse, materialised from `D_FAULT` |
| Invariant oracles | seven post-hoc predicates over the trace: deadlock (with the wait-for cycle), lost pulse, unbounded block, priority inversion, starvation, orphan reply, queue depth |
| Shrinker | causal pre-slice, stimulus ddmin, window minimisation, tape elimination, fault materialisation; 1,500 executions / 90 s; `proven` or `budget_limited` |
| Bug benchmark | six seeded defects (B1–B6) with one-change patches; 6/6 found and 0 false positives on 10,000 fixed seeds, in ~31 s |
| Conformance | 14 dual-target scenario sources, canonical JSONL, Mazurkiewicz `tracecmp`; 12 execute against committed references, 2 compile-only; 11/11 timeout masks; QNX capture pending the SDP |
| Mutation | twelve PRD §14.4 kernel mutants behind compile-time defines; 12/12 killed by the conformance set, focused fixtures, or targeted unit tests |
| Exploration | `--explore random|novelty`, six deterministic input mutators, novelty bitmap, corpus, escalation, shards, `LIVELOCK_SUSPECT`; benchmark over B1–B6 |
| Determinism check | `permute run --verify RATE` re-executes sampled seeds and compares BLAKE3-64 digests; mismatch exits 5 with both |
| Nightly soak | 1,000 seeds × 1,000 repeats per configuration (1,000,000 runs), ASLR on and off, 8 shards each; `tools/soak_report.py` aggregates. Thread backend only (LIMITS #88) |
| Reports | `report.json`, `junit.xml`, `sarif.json`, and a self-contained `index.html` with inline CSS and no network references |
| G1 scorecard | `make gate1` re-runs every G1 check, prints measured values and writes `gate/gate1-scorecard.md` |
| G2 scorecard | `make gate2` prints conformance, outcome-class, mutation, defects, shrink p95, NFR-2..6 and NFR-12; writes `gate/gate2-scorecard.md`. Outcome-class is **blocked** on C11/C13 and says so |
| Packaging | `make install` / `make uninstall` via a manifest, `make dist` source tarball with SHA-256, `--version` prints version, git SHA and kernel model |
| Benchmarks | `make bench` measures kernel-call, switch, throughput, RSS and seed throughput; results with hardware and commit in `bench/results.md` |
| Shrink benchmark | `make shrink-bench` measures 20 failures end to end; p95 0.381 s and 12 events on the reference machine, in `bench/shrink-results.md` |

## What is not built yet

- **Scenario execution is opt-in.** `permute run` still executes the built-in
  seeded workload by default; `permute run --guest PATH` executes a compiled
  scenario through the executor. The scenario loader is not yet wired into the
  worker's default path.
- **Several APIs resolve to libc rather than the model.** Mutexes, condition
  variables, semaphores, files, `stdio`, signals and `timer_create` have no
  shim, so a guest that calls them gets host behaviour — outside the determinism
  claim. `s07` compiles but is skipped for this reason.
  [`docs/LIMITS.md`](docs/LIMITS.md) §2.3 lists every such API and whether it is
  a link error or a silent host call.
- **Fallback link modes are design-only.** `--wrap` and `LD_PRELOAD` are
  documented in ADR-001 but not built; the static-archive shadow path is what
  every test uses (LIMITS #87).
- **Target conformance.** The harness and the dual-target sources are in
  place, but the committed references are simulator output. Comparing against
  real QNX 8.0 needs the SDP and a QEMU boot; see
  [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md).
- **Resource managers (Tier 2).** Dropped after the P2-11 kill criterion: the
  canonical Bones skeleton has no headers, dispatch loop, pathname namespace or
  `iofunc_*` defaults. `name_attach` remains the only API in scope and is not
  implemented. See [`docs/DECISIONS.md`](docs/DECISIONS.md) §4.1.

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
| [`docs/FIDELITY.md`](docs/FIDELITY.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | host proven, target pending |
| [`docs/DETERMINISM.md`](docs/DETERMINISM.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | normative |
| [`docs/ABI.md`](docs/ABI.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | current |
| [`docs/KERNEL-CONTRACT.md`](docs/KERNEL-CONTRACT.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | normative |
| [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | blocked on SDP |
| [`docs/REPRODUCE.md`](docs/REPRODUCE.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | current |
| [`docs/FAQ.md`](docs/FAQ.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | current |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | current |
| [`SECURITY.md`](SECURITY.md) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | current |
| [`gate/`](gate/) | [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14) | per-task evidence |

## Licence

Apache-2.0. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). No vendor headers
are shipped; the ABI declarations are written from public documentation
(ADR-011).
