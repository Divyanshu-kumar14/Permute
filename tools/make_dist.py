#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the source tarball and its checksum (P4-01).

A release artifact has to come from a commit, not from a working tree. This
refuses a dirty checkout unless `--allow-dirty` is explicit, so the tarball's
name and its contents always describe the same SHA.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def fail(message: str) -> int:
    print(f"dist: {message}", file=sys.stderr)
    return 1


def version() -> str:
    text = (REPO / "CMakeLists.txt").read_text()
    match = re.search(r"project\s*\(\s*permute\s+VERSION\s+([0-9.]+)", text)
    if not match:
        raise SystemExit("dist: no project VERSION in CMakeLists.txt")
    return match.group(1)


def git(*args: str) -> str:
    result = subprocess.run(["git", "-C", str(REPO), *args],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"dist: git {' '.join(args)} failed: "
                         f"{result.stderr.strip()}")
    return result.stdout.strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--allow-dirty", action="store_true",
                    help="build from HEAD even when the tree has changes")
    args = ap.parse_args()

    try:
        commit = git("rev-parse", "--short", "HEAD")
        dirty = git("status", "--porcelain")
    except SystemExit as exc:
        return fail(str(exc))

    if dirty and not args.allow_dirty:
        return fail(
            "the working tree is not clean; commit first, or pass "
            "--allow-dirty to package HEAD anyway\n" + dirty
        )

    number = version()
    dist = REPO / "dist"
    dist.mkdir(exist_ok=True)
    tarball = dist / f"permute-{number}.tar.gz"

    result = subprocess.run(
        ["git", "-C", str(REPO), "archive", "--format=tar.gz",
         f"--prefix=permute-{number}/", "-o", str(tarball), "HEAD"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        return fail(f"git archive failed: {result.stderr.strip()}")
    if not tarball.is_file() or tarball.stat().st_size == 0:
        return fail(f"{tarball} was not written")

    digest = hashlib.sha256(tarball.read_bytes()).hexdigest()
    sums = dist / "SHA256SUMS"
    sums.write_text(f"{digest}  {tarball.name}\n")

    size = tarball.stat().st_size
    print(f"dist: {tarball.relative_to(REPO)} ({size} bytes, commit {commit})")
    print(f"dist: {sums.relative_to(REPO)}")
    print(f"dist: sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
