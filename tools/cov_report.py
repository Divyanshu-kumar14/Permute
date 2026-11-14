#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Aggregate gcov line coverage into the four groups NFR-15 names.

Coverage is measured per source line, unioned across the object files that
instrumented it (a source can appear in more than one target). The gate applies
to kernel, shim, log, and oracle; a group with no sources yet reports `n/a`
rather than a misleading zero.
"""

from __future__ import annotations

import gzip
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

LOG_FILES = {"kernel/trace.c", "kernel/blake3.c"}
SCAN_DIRS = ("kernel", "abi", "oracle")

THRESHOLD = 70.0


def group_of(rel: str) -> str | None:
    if rel in LOG_FILES:
        return "log"
    if rel == "kernel/oracle.c":
        return "oracle"
    if rel.startswith("abi/"):
        return "shim"
    if rel.startswith("kernel/"):
        if rel.endswith("scenario.c"):
            return None  # a driver, exercised through tools rather than units
        return "kernel"
    if rel.startswith("oracle/"):
        return "oracle"
    return None


def sources() -> list[Path]:
    found = []
    for directory in SCAN_DIRS:
        root = REPO / directory
        if root.is_dir():
            found.extend(sorted(root.glob("*.c")))
    return found


def gcno_files(build: Path, source: Path) -> list[Path]:
    # CMake names the object msg.c.o, so the notes file is msg.c.gcno. gcov
    # derives the expected name from the source extension, so pass the notes
    # file itself rather than -o <dir> <source>.
    return sorted(build.rglob(f"{source.name}.gcno"))


def line_counts(build: Path, source: Path, notes: Path):
    """Return (covered_line_numbers, instrumented_line_numbers)."""
    with tempfile.TemporaryDirectory() as tmp:
        result = subprocess.run(
            ["gcov", "--json-format", str(notes)],
            cwd=tmp, capture_output=True, text=True,
        )
        if result.returncode != 0:
            return set(), set()

        candidates = list(Path(tmp).glob("*.gcov.json.gz"))
        if not candidates:
            return set(), set()

        with gzip.open(candidates[0], "rt") as handle:
            data = json.load(handle)

    covered: set[int] = set()
    instrumented: set[int] = set()
    for entry in data.get("files", []):
        for line in entry.get("lines", []):
            number = int(line.get("line_number", 0))
            count = int(line.get("count", 0))
            instrumented.add(number)
            if count > 0:
                covered.add(number)
    return covered, instrumented


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: cov_report.py <coverage-build>", file=sys.stderr)
        return 2

    build = Path(sys.argv[1]).resolve()
    if not build.is_dir():
        print(f"coverage build not found: {build}", file=sys.stderr)
        return 2

    totals = {name: [0, 0] for name in ("kernel", "shim", "log", "oracle")}
    per_file = []

    for source in sources():
        rel = source.relative_to(REPO).as_posix()
        group = group_of(rel)
        if group is None:
            continue

        notes = gcno_files(build, source)
        if not notes:
            per_file.append((rel, group, 0, 0))
            continue

        covered: set[int] = set()
        instrumented: set[int] = set()
        for note in notes:
            c, i = line_counts(build, source, note)
            covered |= c
            instrumented |= i

        totals[group][0] += len(covered)
        totals[group][1] += len(instrumented)
        per_file.append((rel, group, len(covered), len(instrumented)))

    print(f"{'file':44} {'group':8} {'lines':>6} {'cov':>6} {'%':>7}")
    for rel, group, covered, total in per_file:
        pct = (100.0 * covered / total) if total else 0.0
        print(f"{rel:44} {group:8} {total:6d} {covered:6d} {pct:7.1f}")

    print()
    failures = 0
    for group in ("kernel", "shim", "log", "oracle"):
        covered, total = totals[group]
        if total == 0:
            print(f"{group:8} n/a (no sources yet)")
            continue
        pct = 100.0 * covered / total
        status = "PASS" if pct >= THRESHOLD else "FAIL"
        if pct < THRESHOLD:
            failures += 1
        print(f"{group:8} {covered}/{total} lines = {pct:5.1f}%  "
              f"[{status} >= {THRESHOLD:.0f}%]")

    print(f"\ncoverage gate: {'FAILED' if failures else 'PASSED'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
