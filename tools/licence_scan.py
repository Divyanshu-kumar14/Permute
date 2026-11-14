#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Licence gate: every source file declares the project licence.

The repository is Apache-2.0 (ADR-011). A file without the SPDX line is either
vendored under another licence or forgotten; both need a human decision, so the
scan fails rather than guessing.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SPDX = "SPDX-License-Identifier: Apache-2.0"

SKIP_DIRS = {
    ".git", "build", "build-asan", "build-cov", ".orchestrator", "specs",
    "__pycache__", ".pytest_cache",
}
SKIP_NAMES = {"LICENSE", "NOTICE", ".gitignore"}

SCAN_SUFFIXES = {
    ".c", ".h", ".py", ".sh", ".toml", ".md", ".yml", ".yaml", ".ld", ".def",
    ".tsv", ".txt",
}


def scan() -> list[str]:
    missing = []
    for path in sorted(REPO.rglob("*")):
        if not path.is_file():
            continue
        if any(part in SKIP_DIRS or part.startswith("build-")
               for part in path.parts):
            continue
        if path.name in SKIP_NAMES:
            continue
        if path.suffix not in SCAN_SUFFIXES and path.name != "Dockerfile":
            continue

        try:
            head = "".join(path.read_text(errors="replace").splitlines(True)[:3])
        except OSError as exc:
            missing.append(f"{path.relative_to(REPO)}: unreadable ({exc})")
            continue

        if SPDX not in head:
            missing.append(str(path.relative_to(REPO)))

    return missing


def main() -> int:
    missing = scan()
    if missing:
        print(f"licence scan: FAIL ({len(missing)} file(s) without {SPDX})")
        for name in missing:
            print(f"  {name}")
        return 1
    print(f"licence scan: PASSED ({SPDX})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
