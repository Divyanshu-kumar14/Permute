#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Aggregate nightly determinism shard summaries.

`tools/determinism_check.py --json-out` writes one file per shard. This tool
checks that the shards cover the range exactly once, that every run used the
same seed/run shape, that the totals match the expected soak size, and that no
shard reported a mismatch.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def load(path: Path) -> dict:
    data = json.loads(path.read_text())
    required = ("label", "shard", "shards", "seeds", "runs", "total_runs",
                "failures", "passed")
    missing = [key for key in required if key not in data]
    if missing:
        raise ValueError(f"{path}: missing {', '.join(missing)}")
    return data


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", type=Path, default=Path("."),
                    help="directory holding soak-*.json files")
    ap.add_argument("--require-labels", default="",
                    help="comma-separated labels that must be present")
    ap.add_argument("--expected-per-label", type=int,
                    help="expected total runs for each label")
    ap.add_argument("--write", type=Path, help="write Markdown evidence here")
    args = ap.parse_args()

    paths = sorted(args.dir.glob("soak-*.json"))
    problems: list[str] = []
    if not paths:
        problems.append(f"no soak-*.json files in {args.dir}")

    groups: dict[str, list[dict]] = {}
    for path in paths:
        try:
            data = load(path)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            problems.append(str(exc))
            continue
        label = str(data["label"])
        if not label:
            problems.append(f"{path.name}: empty label")
            continue
        groups.setdefault(label, []).append(data)

    required = [part.strip() for part in args.require_labels.split(",")
                if part.strip()]
    for label in required:
        if label not in groups:
            problems.append(f"missing soak label '{label}'")

    rows = []
    for label in sorted(groups):
        parts = groups[label]
        shards = {int(part["shards"]) for part in parts}
        seeds = {int(part["seeds"]) for part in parts}
        runs = {int(part["runs"]) for part in parts}
        indices = sorted(int(part["shard"]) for part in parts)
        total = sum(int(part["total_runs"]) for part in parts)
        failures = sum(int(part["failures"]) for part in parts)

        if len(shards) != 1:
            problems.append(f"{label}: inconsistent shard counts {sorted(shards)}")
            continue
        shard_count = shards.pop()
        if len(seeds) != 1 or len(runs) != 1:
            problems.append(f"{label}: inconsistent seed/run shape")
            continue
        seed_count, run_count = seeds.pop(), runs.pop()

        if indices != list(range(shard_count)):
            problems.append(
                f"{label}: shards {indices} do not cover 0..{shard_count - 1}")
        expected = args.expected_per_label or seed_count * run_count
        if total != expected:
            problems.append(
                f"{label}: {total} runs, expected {expected}")
        if failures:
            problems.append(f"{label}: {failures} shard(s) FAILED")
        if any(not part["passed"] for part in parts):
            problems.append(f"{label}: a shard reports passed=false")

        rows.append({
            "label": label,
            "shards": shard_count,
            "seeds": seed_count,
            "runs": run_count,
            "total": total,
            "expected": expected,
            "failures": failures,
            "verdict": "PASS" if (
                not failures and total == expected
                and indices == list(range(shard_count))
            ) else "FAIL",
        })

    overall = sum(row["total"] for row in rows)
    passed = not problems and all(row["verdict"] == "PASS" for row in rows)

    lines = [
        "| Config | Shards | Seeds | Runs/seed | Runs | Failures | Verdict |",
        "|---|---|---|---|---|---|---|",
    ]
    for row in rows:
        lines.append(
            f"| {row['label']} | {row['shards']} | {row['seeds']} | "
            f"{row['runs']} | {row['total']:,} | {row['failures']} | "
            f"{row['verdict']} |"
        )
    lines += ["", f"**Total: {overall:,} runs; "
                  f"{'PASS' if passed else 'FAIL'}**", ""]
    report = "\n".join(lines)

    print(report)
    if args.write:
        args.write.parent.mkdir(parents=True, exist_ok=True)
        args.write.write_text(report)

    for problem in problems:
        print(f"soak-report: {problem}", file=sys.stderr)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
