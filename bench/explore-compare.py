#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Random versus novelty-guided exploration on B1-B6 (FR-5.4 acceptance).

Both policies get the same iteration budget and the same base tape; the
measurement is how often each finds the seeded defect and how early.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from cli.permute.explore import Tape, explore, subprocess_runner  # noqa: E402

BASELINE = REPO / "bench" / "baseline.toml"
ITERATIONS = 32


def build_buggy(bug: dict) -> Path:
    name = bug["id"].lower() + "b"
    out = REPO / "build" / "explore-bench" / name
    out.mkdir(parents=True, exist_ok=True)
    guest = out / f"proc_{name}__0.so"
    source = REPO / bug["dir"] / "scenario.c"
    if not guest.is_file():
        result = subprocess.run(
            [sys.executable, str(REPO / "tools" / "make_guest.py"),
             "--source", str(source), "--name", name, "--instance", "0",
             "--out", str(out), "--build-dir", str(REPO / "build")],
            capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(f"build failed: {source}\n{result.stderr}")
    return guest


def main() -> int:
    baseline = tomllib.loads(BASELINE.read_text())
    runner_path = REPO / "build" / "permute-run"
    if not runner_path.is_file():
        raise SystemExit("missing build/permute-run; run make build")

    rows = []
    totals = {policy: {"findings": 0, "first": 0, "found": 0}
              for policy in ("random", "novelty")}

    for bug in baseline.get("bug", []):
        guest = build_buggy(bug)
        runner = subprocess_runner(str(guest), str(runner_path), timeout=0.5)
        row = {"id": bug["id"], "class": bug.get("class", "")}
        for policy in ("random", "novelty"):
            result = explore(policy, ITERATIONS, Tape(seed=0), runner=runner,
                             timeout=0.5)
            interesting = [entry for entry in result.entries
                           if entry.outcome not in ("CLEAN", "BUDGET")]
            first = interesting[0].index if interesting else None
            row[policy] = {
                "findings": len(interesting),
                "first": first,
                "coverage": result.coverage,
                "corpus": result.corpus_size,
            }
            totals[policy]["findings"] += len(interesting)
            totals[policy]["found"] += 1 if interesting else 0
            if first is not None:
                totals[policy]["first"] += first
        rows.append(row)
        print(f"{row['id']} {row['class']:38s} "
              f"random {row['random']['findings']:3d} "
              f"(first {row['random']['first']}), "
              f"novelty {row['novelty']['findings']:3d} "
              f"(first {row['novelty']['first']})")

    print(f"totals: random {totals['random']}, novelty {totals['novelty']}")
    payload = {"iterations": ITERATIONS, "rows": rows, "totals": totals}
    (REPO / "bench" / "explore-results.json").write_text(
        json.dumps(payload, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
