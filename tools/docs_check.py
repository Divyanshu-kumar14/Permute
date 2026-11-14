#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Documentation gate: every reader-facing document names an owner and status.

A document without an owner is a document nobody maintains. A document without
a status is a reader guessing whether it describes the build or an intention.

Scope is the reader-facing set: the two root documents and `docs/*.md`. Gate
reports under `gate/` are per-task evidence; they carry a date, scope, and
result instead, and are not claimed to be current.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

REQUIRED = ("**Owner:**", "**Status:**")

ALLOWED_DIFFERENCES = REPO / "conformance" / "ALLOWED-DIFFERENCES.md"
MAX_ALLOWED_DIFFERENCES = 8
DIFFERENCE_COLUMNS = ("#", "Field", "Treatment", "Architectural rationale")


def documents() -> list[Path]:
    found = [REPO / "README.md", REPO / "QUICKSTART.md"]
    docs = REPO / "docs"
    if docs.is_dir():
        found.extend(sorted(docs.glob("*.md")))
    return [path for path in found if path.is_file()]


def check_allowed_differences() -> list[str]:
    """FR-11.7 / R2: a closed, field-exact list of at most eight entries."""
    if not ALLOWED_DIFFERENCES.is_file():
        return [f"{ALLOWED_DIFFERENCES.relative_to(REPO)}: missing"]

    problems = []
    rows = 0
    header_seen = False

    for number, line in enumerate(ALLOWED_DIFFERENCES.read_text().splitlines(), 1):
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if tuple(cells) == DIFFERENCE_COLUMNS:
            header_seen = True
            continue
        if not cells or not cells[0].isdigit():
            continue
        rows += 1
        if len(cells) != len(DIFFERENCE_COLUMNS):
            problems.append(
                f"ALLOWED-DIFFERENCES.md:{number}: {len(cells)} column(s), "
                f"expected {len(DIFFERENCE_COLUMNS)}"
            )
        elif not cells[1] or not cells[2] or not cells[3]:
            problems.append(
                f"ALLOWED-DIFFERENCES.md:{number}: entry {cells[0]} has an "
                "empty field, treatment or rationale"
            )

    if not header_seen:
        problems.append("ALLOWED-DIFFERENCES.md: missing the entry table")
    if rows == 0:
        problems.append("ALLOWED-DIFFERENCES.md: no numbered entries")
    elif rows > MAX_ALLOWED_DIFFERENCES:
        problems.append(
            f"ALLOWED-DIFFERENCES.md: {rows} entries, cap is "
            f"{MAX_ALLOWED_DIFFERENCES} (R2 early warning)"
        )
    return problems


def main() -> int:
    problems = check_allowed_differences()
    checked = 0

    for path in documents():
        checked += 1
        text = path.read_text()
        for marker in REQUIRED:
            if marker not in text:
                problems.append(f"{path.relative_to(REPO)}: missing {marker}")

    if problems:
        print(f"docs-check: FAIL ({len(problems)} problem(s))")
        for problem in problems:
            print(f"  {problem}")
        return 1

    print(f"docs-check: PASSED ({checked} document(s) with owner and status; "
          f"allowed differences within 8)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
