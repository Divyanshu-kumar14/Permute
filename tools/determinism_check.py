#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Same-seed determinism: many seeds, repeated runs, one digest each.

T0.9's shape is 100 seeds x 10 runs. Each run is a separate process, so a
digest difference catches ASLR, uninitialised state, and allocation-history
dependence, not just a wrong PRNG.

The nightly soak shards the seed range: `--shard i/n` owns seeds congruent to
`i` modulo `n`, so the shards are disjoint and their union is the full range.
`--jobs` runs seeds concurrently; each run deletes its trace after reading the
digest, so disk use is bounded by the worker count rather than the run count.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def run_once(binary: Path, seed: int, trace: Path) -> str:
    try:
        result = subprocess.run([str(binary), str(seed), str(trace)],
                                capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(
                f"{binary.name} seed {seed} exited {result.returncode}: "
                f"{result.stderr.strip()}"
            )
        for line in result.stdout.splitlines():
            if line.startswith("DIGEST="):
                return line.split("=", 1)[1]
        raise RuntimeError(f"{binary.name} seed {seed} printed no digest")
    finally:
        trace.unlink(missing_ok=True)


def check_seed(binary: Path, seed: int, runs: int, tmp: Path):
    seen = set()
    for run in range(runs):
        seen.add(run_once(binary, seed, tmp / f"{seed}-{run}.pmt"))
    return seed, sorted(seen)


def parse_shard(text: str):
    try:
        index, count = (int(part) for part in text.split("/", 1))
    except ValueError:
        raise argparse.ArgumentTypeError("expected I/N, e.g. 2/8")
    if count < 1 or not 0 <= index < count:
        raise argparse.ArgumentTypeError(f"shard {index}/{count} is out of range")
    return index, count


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=str(REPO / "build" / "permute-run"))
    ap.add_argument("--seeds", type=int, default=100)
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--shard", type=parse_shard, default=(0, 1),
                    help="seed shard I/N (default 0/1)")
    ap.add_argument("--jobs", type=int, default=1,
                    help="seeds checked concurrently (default 1)")
    ap.add_argument("--label", default="",
                    help="configuration name recorded in --json-out")
    ap.add_argument("--json-out", type=Path,
                    help="write a machine-readable shard summary")
    args = ap.parse_args()

    binary = Path(args.binary)
    if not binary.is_file():
        print(f"determinism: missing {binary} (run make build)", file=sys.stderr)
        return 2
    if args.seeds < 1 or args.runs < 1 or args.jobs < 1:
        print("determinism: seeds, runs and jobs must be positive",
              file=sys.stderr)
        return 2

    shard, shards = args.shard
    seeds = list(range(shard, args.seeds, shards))
    failures = 0
    digests: set[str] = set()
    results = []

    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        if args.jobs == 1:
            for seed in seeds:
                results.append(check_seed(binary, seed, args.runs, tmp))
        else:
            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                pending = {
                    pool.submit(check_seed, binary, seed, args.runs, tmp): seed
                    for seed in seeds
                }
                for future in as_completed(pending):
                    results.append(future.result())

    for seed, seen in sorted(results):
        if len(seen) != 1:
            failures += 1
            print(f"FAIL seed {seed}: {len(seen)} distinct digests {seen}")
        digests |= set(seen)

    needed = max(1, int(len(seeds) * 0.95))
    if len(digests) < needed:
        failures += 1
        print(f"FAIL seed spread: {len(digests)} distinct over {len(seeds)} "
              f"seeds (need >= {needed})")

    total = len(seeds) * args.runs
    summary = {
        "label": args.label,
        "binary": str(binary),
        "shard": shard,
        "shards": shards,
        "seeds": args.seeds,
        "shard_seeds": len(seeds),
        "runs": args.runs,
        "jobs": args.jobs,
        "total_runs": total,
        "distinct_digests": len(digests),
        "failures": failures,
        "passed": failures == 0,
    }
    if args.json_out:
        args.json_out.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n")

    if failures:
        print(f"determinism: FAILED ({failures} failure(s))")
        return 1

    shard_note = f" [shard {shard}/{shards}]" if shards != 1 else ""
    print(f"determinism: PASSED ({total} runs, {len(seeds)} seeds, "
          f"{args.runs} runs/seed, {len(digests)} distinct digests)"
          f"{shard_note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
