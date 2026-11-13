<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-12 report — `permute-run` and `permute-worker`

**Date:** 2026-11-14
**Scope:** one process per seed, crash isolation, wall budget, batch leases, shard reproducibility, partial reports.
**Result:** **46 worker checks + 13 lease checks, 0 failures**; full `make test` green.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `kernel/scenario.c/.h` | The seeded scenario, extracted so replay and `permute-run` share one implementation |
| `worker/run.c` | `permute-run`: execute one seed, write its trace, exit |
| `worker/lease.c/.h` | `O_EXCL` batch leases, staleness, reclamation |
| `worker/main.c` | `permute-worker`: fork per seed, classify, budget, report |
| `cli/permute/run.py` | `execute` subcommand: split a range across workers and merge reports |
| `tests/unit/test_lease.c` | Claim, conflict, release, stale, corrupt lease |
| `tests/python/test_worker.py` | 46 checks across the five behaviours |

---

## 2. Results

```
test_lease:    13 checks, 0 failures
test_worker:   46 checks, 0 failures
all other suites unchanged and green, ASan clean
```

| Behaviour | Result |
|---|---|
| 5 clean seeds | exit 0, `clean: 5`, every trace verifies |
| `PERMUTE_CRASH_SEED=2`, seeds 0–4 | **exit 1**, `crash: 1`, `clean: 4`, finding `{seed:2, CRASH, signal:11}` |
| Seeds after the crash | seeds 3 and 4 completed and their traces verify |
| Crashed seed's trace | absent, as it should be |
| `PERMUTE_SPIN_SEED=1`, 100 ms budget | exit **0**, `budget: 1`, `clean: 2`, no finding |
| `--shard 1/2` over 0–9 | exactly seeds 1,3,5,7,9; digests equal to the full run's |
| `--jobs 1` vs `--jobs 4` over 0–7 | same digest for every seed |
| Lease claim / conflict / release | second claim `-EBUSY`; release frees it |
| Stale lease (dead pid) | refused without `--steal`, reclaimed with it |
| Corrupt lease | treated as stale, not as an eternal block |

---

## 3. The crash path

The worker never executes SUT code. Each seed is a child process:

```
worker ──fork/exec──► permute-run <seed> <trace>
   │                        │
   │◄── wait status ────────┘
   │
   ├─ exited 0        → CLEAN
   ├─ killed by signal → CRASH (signal recorded)
   ├─ exited non-zero  → VIOLATION / INTERNAL
   └─ past wall budget → SIGKILL, BUDGET
```

A `SIGSEGV` in the SUT is therefore just a wait status. The test runs five seeds with seed 2 crashing, and seeds 3 and 4 complete normally with valid traces. That is FR-5.1 and NFR-8 in one observation.

The crash is induced through `PERMUTE_CRASH_SEED`, a deliberate test hook in the runner. The worker does not know about it; from its side this is an ordinary fatal signal.

---

## 4. Budgets are outcomes

FR-5.6 says hitting a budget must be reported as `Budget`, not as a failure. The worker kills an overrunning child with `SIGKILL`, records `BUDGET`, and **does not** change the exit code. A run of three seeds with one spinner exits 0 and reports `budget: 1`.

The wall deadline uses `CLOCK_MONOTONIC` in the worker. That is correct and deliberate: the wall budget is a host concept, and the determinism gate only governs `kernel/`, `abi/`, and `guest/` — the simulated side.

---

## 5. Leases and sharding

Batches are claimed by creating `leases/batch-<first>.lease` with `O_CREAT | O_EXCL`. The kernel's `open` is the arbiter; no lock server, no race window. The file records the worker's pid, so a batch left behind by a dead worker is detectable: `kill(pid, 0)` returning `ESRCH` means stale. Reclamation is explicit (`--steal`) rather than automatic, because stealing a live worker's batch would duplicate work silently.

`--shard i/n` filters on `seed % n == i` **before** batching. The test compares a shard against the corresponding seeds of a full run and gets identical digests, which is FR-5.9 without depending on how the work was divided.

Job-count independence (FR-5.2) is tested by running seeds 0–7 under one and four workers: identical digest per seed, because the seed's trace is a function of the seed alone and the worker never influences it.

---

## 6. Partial reports

Every seed appends one line to `report.jsonl` as it finishes. The summary is rewritten at the end. A worker killed mid-run therefore leaves per-seed evidence on disk, and several workers can share an output directory without coordination beyond the leases.

This is the seed of the failure-artifact pipeline: the JSONL is already the crash-survivable record, and the artifact format will be built on top of it rather than replacing it.

---

## 7. Boundary

| In scope | Out of scope |
|---|---|
| Process isolation, budgets, leases, shards, reports | The real executor and scenario loader (#26) |
| `--jobs` across worker processes | Threads inside a worker (#28) |
| Per-seed 2 s wall budget | A run-level global deadline (#27) |
| Scenario shared with replay | Oracles; a crash is recorded, not classified further |

---

## 8. Next

**P1-13 — CLI skeleton.** `init`, `build`, `run`, `show`, `--version`, `--json`, config parse and validation, the full exit-code matrix, and `permute init` writing the symbol audit and header-gap report. That turns the pieces built so far into something a user can drive from a clean checkout.
