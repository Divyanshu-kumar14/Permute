#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bug benchmark (P2-04, T1.6).

Six seeded defects across four classes. Each fixture is a source file plus a
one-change patch. The benchmark:

  1. builds the buggy source and the patched source,
  2. runs the buggy build over its expected seed range and requires at least
     one finding,
  3. runs the fixed build over 10,000 seeds and requires zero findings.

A finding is any outcome the worker reports as CRASH or VIOLATION. Budget and
internal outcomes are not findings, but they are not silent either: the fixed
baseline fails on them, because a clean build must not hit a budget under a
tiny scenario.

Usage:
    python3 tools/bench_bugs.py [--quick] [--jobs N]
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from cli.permute.run import execute  # noqa: E402
BASELINE = REPO / "bench" / "baseline.toml"
MAKE_GUEST = REPO / "tools" / "make_guest.py"
BUILD = REPO / "build"
RUNNER = BUILD / "permute-run"
WORKER = BUILD / "permute-worker"


def build_guest(source: Path, name: str, out: Path) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [sys.executable, str(MAKE_GUEST), "--source", str(source),
         "--name", name, "--instance", "0", "--out", str(out),
         "--build-dir", str(BUILD)],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise SystemExit(f"build failed for {source}:\n{result.stdout}\n"
                         f"{result.stderr}")
    return out / f"proc_{name}__0.so"


def run_range(seed_range: str, guest: Path, out: Path, jobs: int,
              wall_ms: int, batch: int) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    code = execute(seed_range, str(out), jobs=jobs, runner=str(RUNNER),
                   worker=str(WORKER), wall_ms=wall_ms, batch=batch,
                   quiet=True, guest=str(guest))
    report = out / "report.json"
    if not report.is_file():
        raise SystemExit(f"worker produced no report for {seed_range} "
                         f"(exit {code})")
    data = json.loads(report.read_text())
    data["exit"] = code
    return data


def prepare_fixed(source: Path, patch: Path, work: Path) -> Path:
    target = work / "scenario.c"
    shutil.copyfile(source, target)
    result = subprocess.run(["patch", "-p1", "-i", str(patch)],
                            cwd=work, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"patch failed for {source}:\n{result.stdout}\n"
                         f"{result.stderr}")
    return target


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true",
                        help="100 fixed seeds instead of 10,000")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()

    for required in (RUNNER, WORKER):
        if not required.is_file():
            raise SystemExit(f"missing {required}; run make build first")

    baseline = tomllib.loads(BASELINE.read_text())
    defaults = baseline.get("baseline", {})
    jobs = args.jobs or int(defaults.get("jobs", 8))
    wall_ms = int(defaults.get("wall_ms", 2000))
    batch = int(defaults.get("batch", 64))
    fixed_seeds = "0-99" if args.quick else str(defaults.get("fixed_seeds",
                                                             "0-9999"))

    bugs = baseline.get("bug", [])
    if not bugs:
        raise SystemExit("bench/baseline.toml has no [[bug]] entries")

    found = 0
    false_positives = 0
    rows = []

    with tempfile.TemporaryDirectory() as tmp_text:
        tmp = Path(tmp_text)
        for bug in bugs:
            ident = bug["id"]
            source_dir = REPO / bug["dir"]
            work = tmp / ident
            work.mkdir(parents=True, exist_ok=True)

            buggy_source = source_dir / "scenario.c"
            patch = source_dir / "fix.patch"
            fixed_source = prepare_fixed(buggy_source, patch, work)

            out_dir = BUILD / "bugs" / ident.lower()
            buggy = build_guest(buggy_source, f"{ident.lower()}b", out_dir)
            fixed = build_guest(fixed_source, f"{ident.lower()}f", out_dir)

            expected = bug.get("seeds", str(defaults.get("buggy_seeds",
                                                         "0-63")))
            buggy_report = run_range(expected, buggy, tmp / ident / "buggy",
                                     jobs, wall_ms, batch)
            hits = buggy_report.get("crash", 0) + buggy_report.get("violation", 0)
            if hits > 0:
                found += 1

            fixed_report = run_range(fixed_seeds, fixed,
                                     tmp / ident / "fixed", jobs, wall_ms,
                                     batch)
            bad = (fixed_report.get("crash", 0)
                   + fixed_report.get("violation", 0)
                   + fixed_report.get("budget", 0)
                   + fixed_report.get("internal", 0))
            false_positives += bad

            rows.append((ident, bug.get("class", "?"), expected, hits,
                         fixed_seeds, bad))
            status = "found" if hits else "MISSED"
            print(f"{ident} {bug.get('class', '?'):38s} "
                  f"{expected:>7s} -> {hits:4d} finding(s)  {status:6s} | "
                  f"fixed {fixed_seeds}: {bad} finding(s)")

    print(f"bug benchmark: {found}/{len(bugs)} found, "
          f"{false_positives} false positive(s) on the fixed build")
    return 0 if found == len(bugs) and false_positives == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
