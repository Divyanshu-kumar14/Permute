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


def documents() -> list[Path]:
    found = [REPO / "README.md", REPO / "QUICKSTART.md"]
    docs = REPO / "docs"
    if docs.is_dir():
        found.extend(sorted(docs.glob("*.md")))
    return [path for path in found if path.is_file()]


def main() -> int:
    problems = []
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

    print(f"docs-check: PASSED ({checked} document(s) with owner and status)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
