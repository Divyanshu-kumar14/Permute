#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run every example from a clean copy and compare its output to expected.txt.

An example that has drifted from the code teaches a new reader something that
does not work. Each directory under examples/ with a permute.toml is copied to
a temporary directory (so nothing from a previous local run can be reused),
built, run against three seeds, and its exact output compared with the
committed expected.txt.

    check_examples.py            verify every example
    check_examples.py --update   rewrite expected.txt from the real output
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PERMUTE = REPO / "tools" / "permute"
EXAMPLES = REPO / "examples"
HEADER = "# SPDX-License-Identifier: Apache-2.0"


def run(args, cwd, expect_ok=True):
    result = subprocess.run([str(PERMUTE), *args], cwd=cwd,
                            capture_output=True, text=True)
    if expect_ok and result.returncode != 0:
        raise RuntimeError(
            f"permute {' '.join(args)} exited {result.returncode}\n"
            f"{result.stdout}{result.stderr}")
    return result


def seeds_from_config(project: Path) -> list[int]:
    """Read the seed range the example's permute.toml asks for."""

    text = (project / "permute.toml").read_text()
    section = ""
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            continue
        if section != "run" or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key.strip() != "seeds":
            continue
        value = value.strip().strip('"')
        if "-" in value:
            first, last = value.split("-", 1)
            return list(range(int(first), int(last) + 1))
        return [int(value)]
    return [0]


def observe(project: Path) -> list[str]:
    """Build and run one example, returning the output a reader should see."""

    run(["build"], project)
    run_out = run(["run", "--guest", ".permute/build/proc_smoke__0.so"],
                  project).stdout
    summary = ""
    for line in run_out.splitlines():
        if " seeds: " in line:
            summary = line.strip()
            break
    if not summary:
        raise RuntimeError(f"no run summary in:\n{run_out}")

    lines = [f"seeds: {summary.split(': ', 1)[1]}"]
    for seed in seeds_from_config(project):
        shown = json.loads(run(["show", "--seed", str(seed), "--json"],
                               project).stdout)
        lines.append(f"seed {seed}: {shown['records']} records, "
                     f"digest {shown['digest']}")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--update", action="store_true",
                        help="rewrite expected.txt instead of comparing")
    args = parser.parse_args()

    if not PERMUTE.is_file():
        print(f"check_examples: missing {PERMUTE}", file=sys.stderr)
        return 2
    if not (REPO / "build" / "permute-run").is_file():
        print("check_examples: build the harness first (make build)",
              file=sys.stderr)
        return 2

    directories = sorted(p.parent for p in EXAMPLES.glob("*/permute.toml"))
    if not directories:
        print("check_examples: no examples found", file=sys.stderr)
        return 2

    failures = 0
    for source in directories:
        expected_path = source / "expected.txt"
        try:
            with tempfile.TemporaryDirectory() as tmp:
                project = Path(tmp) / source.name
                project.mkdir()
                shutil.copy2(source / "permute.toml", project / "permute.toml")
                shutil.copytree(source / "scenarios", project / "scenarios")
                observed = observe(project)

            if args.update:
                expected_path.write_text(HEADER + "\n" + "\n".join(observed) + "\n")
                print(f"{source.name}: wrote {expected_path.name}")
                continue

            if not expected_path.is_file():
                failures += 1
                print(f"FAIL {source.name}: no {expected_path}")
                continue
            expected = [
                line for line in expected_path.read_text().splitlines()
                if line.strip() and not line.startswith("#")
            ]
            if expected != observed:
                failures += 1
                print(f"FAIL {source.name}: output differs")
                for i in range(max(len(expected), len(observed))):
                    want = expected[i] if i < len(expected) else "<missing>"
                    have = observed[i] if i < len(observed) else "<missing>"
                    marker = " " if want == have else ">"
                    print(f"  {marker} expected: {want}")
                    print(f"  {marker} actual:   {have}")
            else:
                print(f"{source.name}: PASS ({observed[0]})")
        except (OSError, RuntimeError, json.JSONDecodeError) as exc:
            failures += 1
            print(f"FAIL {source.name}: {exc}")

    if failures:
        print(f"examples: FAILED ({failures} example(s))")
        return 1

    print(f"examples: PASSED ({len(directories)} example(s), "
          f"output matches expected.txt)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
