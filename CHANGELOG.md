<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes to Permute are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- Pulse delivery no longer applies message-driven priority inheritance. A
  pulse is fire-and-forget: unlike a message it carries no work back to the
  sender, so it leaves the receiver's scheduling state alone. The simulator
  reference trace for C06 was re-recorded (one `SCHED_PICK` priority) to match.

### Fixed

- The live wait-for graph resolves a mutex waiter to the lock's current owner,
  so a `PTHREAD_PRIO_INHERIT` cycle is reported by `permute-run` instead of
  only by the post-hoc trace oracle.

### Performance

- The executor baton puts `turn` and `yielded` on separate cache lines, uses a
  `pause`/`yield` spin hint, and spins longer before the `sched_yield`
  fallback. `permute shrink` keeps scratch traces in `/dev/shm` and returns on
  a conclusive runner verdict without decoding the trace.

## [0.1.0] - 2026-11-14

First feature-complete release through the P3 evidence milestone.
Dual-target cross-compilation and on-target execution verified across 14
scenarios under genuine QNX 8.0 SDP (`gcc_ntox86_64` 12.2.0); see
`docs/FIDELITY.md` and `docs/QNX-EVIDENCE.md`.

### Added

- Cooperative, single-threaded simulated kernel for QNX message passing:
  channels, connections, `MsgSend`/`MsgReceive`/`MsgReply`/`MsgError`,
  pulses, receive queues, strict-priority scheduling, priority inheritance,
  timers, and the virtual clock.
- Strong-symbol interposition layer with a 33-symbol, six-group audited ABI
  (`abi/permute_iface.def`), a static guest archive, and atomic group
  enforcement at build time.
- `permute` CLI: `init`, `build`, `run`, `show`, `replay`, `shrink`, `verify`,
  `report`, and `--version` (version, git SHA, kernel model).
- Canonical trace log with chunk CRCs, a BLAKE3 hash chain, JSON/JSONL
  reports, JUnit, SARIF 2.1.0, and offline HTML.
- Fault injection, seven oracle families, deterministic exploration, and the
  causal-cone shrinker with a proven-minimality result.
- Conformance pipeline with 12 executable scenarios, the canonical
  comparator, and a per-field allowed-differences list.
- Gate scorecards: `make gate1` (G1) and `make gate2` (G2).
- Packaging: `install.sh` with manifest-based `make uninstall`, `make dist`
  source tarballs with SHA-256 checksums, and measured documentation.
- Examples: `hello-ipc`, `db-server`, and `telemetry`, each a complete project
  with its trace digest pinned in `expected.txt` and checked by `make examples`.

### Verification

- `make ci`: unit, codec, replay, allocator, interposition, determinism,
  coverage, docs, quickstart, and licence gates.
- `make gate1`: 9/9 criteria, including 1,000/1,000 same-seed determinism and
  12/12 mutation kills.
- Shrink benchmark: p95 0.381 s over 20 failures, 12-event reduced traces.
- Nightly: 1,000 seeds × 1,000 repeats per configuration, ASLR on and off,
  thread backend (fiber backend not implemented, LIMITS #88).

[Unreleased]: https://github.com/Divyanshu-kumar14/Permute/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/Divyanshu-kumar14/Permute/releases/tag/v0.1.0
