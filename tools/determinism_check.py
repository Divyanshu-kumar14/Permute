#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Same-seed determinism: many seeds, repeated runs, one digest each.

T0.9's shape is 100 seeds x 10 runs. Each run is a separate process, so a
digest difference catches ASLR, uninitialised state, and allocation-history
dependence, not just a wrong PRNG.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def run_once(binary: Path, seed: int, trace: Path) -> str:
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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=str(REPO / "build" / "permute-run"))
    ap.add_argument("--seeds", type=int, default=100)
    ap.add_argument("--runs", type=int, default=10)
    args = ap.parse_args()

    binary = Path(args.binary)
    if not binary.is_file():
        print(f"determinism: missing {binary} (run make build)", file=sys.stderr)
        return 2

    failures = 0
    digests: set[str] = set()

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        for seed in range(args.seeds):
            seen = set()
            for run in range(args.runs):
                trace = tmp_path / f"{seed}-{run}.pmt"
                seen.add(run_once(binary, seed, trace))
            if len(seen) != 1:
                failures += 1
                print(f"FAIL seed {seed}: {len(seen)} distinct digests "
                      f"{sorted(seen)}")
            digests |= seen

    needed = max(1, int(args.seeds * 0.95))
    if len(digests) < needed:
        failures += 1
        print(f"FAIL seed spread: {len(digests)} distinct over {args.seeds} "
              f"seeds (need >= {needed})")

    total = args.seeds * args.runs
    if failures:
        print(f"determinism: FAILED ({failures} failure(s))")
        return 1

    print(f"determinism: PASSED ({total} runs, {args.seeds} seeds, "
          f"{args.runs} runs/seed, {len(digests)} distinct digests)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
