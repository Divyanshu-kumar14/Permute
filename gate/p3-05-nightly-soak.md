<!-- SPDX-License-Identifier: Apache-2.0 -->

# P3-05 report — nightly determinism soak

**Date:** 2026-11-14
**Scope:** NFR-1 — 1,000 seeds × 1,000 repeats (1,000,000 same-seed runs per
configuration), ASLR on and off, in `.github/workflows/nightly.yml`.
**Result:** **workflow implemented and locally validated at reduced scale.**
The full 1,000,000-run configuration is evidence produced by the scheduled
GitHub run, not by this workstation. The fiber backend does not exist
(LIMITS #88), so the soak covers the thread backend and records that deviation
rather than faking a second backend.

---

## 1. Workflow

| Item | Value |
|---|---|
| Trigger | daily 03:00 UTC and `workflow_dispatch` |
| Configurations | `aslr-on`, `aslr-off` (`setarch $(uname -m) -R`) |
| Shards | 8 per configuration (`--shard i/8`, seeds `i, i+8, …`) |
| Runs per shard | 125 seeds × 1,000 repeats = 125,000 |
| Runs per configuration | **1,000,000** |
| Parallelism | 4 seeds per shard (`--jobs 4`), 16 matrix jobs |
| Aggregation | `tools/soak_report.py` checks shard coverage, totals and failures |
| Job timeout | 180 minutes per shard; 15 minutes for the report |

Each run is a separate `permute-run` process, so a digest difference catches
ASLR, uninitialised state and allocation-history dependence. Each trace is
deleted after its digest is read, so disk use is bounded by the four workers,
not by 125,000 files.

`soak-report` runs with `if: always()` so a failed shard still produces a
report. It fails on a missing label, a missing or duplicate shard, an
inconsistent seed/run shape, a short total, or any reported mismatch.

---

## 2. Local evidence

The exact commands were exercised at reduced scale (100 seeds × 100 repeats,
4 shards, 2 configurations):

```
$ python3 tools/determinism_check.py --seeds 100 --runs 100 \
    --shard 0/4 --jobs 4 --label aslr-on --json-out soak-aslr-on-0.json
determinism: PASSED (2500 runs, 25 seeds, 100 runs/seed, 25 distinct digests) [shard 0/4]

$ python3 tools/soak_report.py --dir soak \
    --require-labels aslr-on,aslr-off --expected-per-label 10000
| aslr-off | 4 | 100 | 100 | 10,000 | 0 | PASS |
| aslr-on  | 4 | 100 | 100 | 10,000 | 0 | PASS |
**Total: 20,000 runs; PASS**
```

The ASLR-off shards were launched under `setarch $(uname -m) -R`; the children
inherit `ADDR_NO_RANDOMIZE`.

Additional checks:

```
make determinism  determinism: PASSED (1000 runs, 100 seeds, 10 runs/seed, 100 distinct digests)
make ci-check     ci-check: PASSED (2 workflow(s), musl image)
```

---

## 3. What is not yet proven

| Claim | Status |
|---|---|
| 1,000,000/1,000,000 identical per configuration | **Not yet produced.** The first scheduled or dispatched nightly run is the evidence. The local smoke proves the sharding, aggregation and ASLR paths, at 1/100 of the run count. |
| Fiber backend soak | **Not possible.** The backend is not implemented; NFR-4's fiber figure is unmeasured (LIMITS #88). |
| Nightly job duration on GitHub runners | Estimated ~2–5 minutes per shard from 306 runs/s sequential locally; the 180-minute timeout is a safety bound, not a measurement. |

The workflow must not be reported as "NFR-1 met" until a nightly run has
uploaded 16 shard summaries and the report shows `2,000,000 runs; PASS`.

---

## 4. Reproduce

Locally, at any scale:

```sh
python3 tools/determinism_check.py --seeds 1000 --runs 1000 \
  --shard 0/8 --jobs 4 --label aslr-on --json-out soak-aslr-on-0.json
setarch "$(uname -m)" -R python3 tools/determinism_check.py \
  --seeds 1000 --runs 1000 --shard 0/8 --jobs 4 --label aslr-off \
  --json-out soak-aslr-off-0.json
python3 tools/soak_report.py --dir . --require-labels aslr-on,aslr-off \
  --expected-per-label 1000000
```

In CI: `gh workflow run nightly.yml`, then read the `soak-report` artifact and
the run summary.
