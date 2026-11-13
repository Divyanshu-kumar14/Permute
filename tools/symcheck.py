#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build gate for interposition coverage and determinism invariants.

Two independent checks:

1. **Symbol audit** (FR-2.2/2.3): every symbol named in
   `abi/permute_iface.def` must be defined by `libpermute_guest.a`.

2. **Determinism gate** (FR-4.7/4.8/4.9/4.11): production C sources may not
   call a real clock, real entropy, host identity, or a hash container whose
   iteration order is observable. The shim files that exist to route those
   calls are allowlisted by path, so the exception is explicit and reviewable.

Exit 0 only if both pass.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

SYMBOL_RE = re.compile(r"PERMUTE_SYMBOL\(\s*[^,]+,\s*[^,]+,\s*(\w+)")

# Patterns are matched against comment-stripped C source.
BANNED = {
    "container": [
        r"\bhcreate\b", r"\bhsearch\b", r"\bhdestroy\b",
        r"\bhcreate_r\b", r"\bhsearch_r\b", r"\bhdestroy_r\b",
        r"\bqHash\b", r"\bunordered_map\b", r"\bstd::map\b",
        r"\bstd::set\b", r"\buthash\b",
    ],
    "entropy": [
        r"\bsrand\s*\(", r"\brand\s*\(", r"\bsrandom\s*\(",
        r"\brandom\s*\(", r"\bgetrandom\s*\(", r"\barc4random\b",
        r"/dev/urandom", r"\bRAND_bytes\b",
    ],
    "time": [
        r"\bclock_gettime\s*\(", r"\bgettimeofday\s*\(", r"\btime\s*\(",
        r"\bnanosleep\s*\(", r"\bclock_nanosleep\s*\(",
        r"\btimer_create\s*\(", r"\btimer_settime\s*\(",
    ],
    "identity": [
        r"\bgetpid\s*\(", r"\bgettid\s*\(", r"\bpthread_self\s*\(",
    ],
}

# The only files allowed to name each category: the shims that route the call
# into the kernel. An empty set means "no exceptions".
ALLOW = {
    "container": set(),
    "entropy": {"abi/shim_rand.c"},
    "time": {"abi/shim_time.c"},
    "identity": {"abi/shim_proc.c", "abi/shim_pthread.c"},
}

SCAN_DIRS = ("kernel", "abi", "guest")

_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)


def strip_comments(text: str) -> str:
    """Remove C comments, preserving line count so reported lines are right."""
    def blank(match: re.Match) -> str:
        return "\n" * match.group(0).count("\n")

    return _COMMENT_RE.sub(blank, text)


def parse_symbols(path: Path) -> list[str]:
    raw = path.read_text()
    text = "\n".join(line.split("//", 1)[0] for line in raw.splitlines())
    seen: list[str] = []
    for name in SYMBOL_RE.findall(text):
        if name not in seen:
            seen.append(name)
    return seen


def scan_tree(root: Path) -> list[str]:
    """Return human-readable violations under root/{kernel,abi,guest}."""
    violations: list[str] = []

    for sub in SCAN_DIRS:
        directory = root / sub
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*.c")):
            rel = path.relative_to(root).as_posix()
            text = strip_comments(path.read_text())

            for category, patterns in BANNED.items():
                if rel in ALLOW[category]:
                    continue
                for pattern in patterns:
                    for match in re.finditer(pattern, text):
                        line = text.count("\n", 0, match.start()) + 1
                        violations.append(
                            f"{rel}:{line}: {category}: {match.group(0)}"
                        )
    return violations


def check_archive(root: Path, archive: Path) -> list[str]:
    if not archive.is_file():
        return [f"archive not found: {archive} (run make build)"]

    symbols = parse_symbols(root / "abi" / "permute_iface.def")
    if not symbols:
        return ["no symbols parsed from abi/permute_iface.def"]

    nm = subprocess.run(["nm", "-g", "--defined-only", str(archive)],
                        capture_output=True, text=True)
    if nm.returncode != 0:
        return [f"nm failed on {archive}: {nm.stderr.strip()}"]

    defined = set()
    for line in nm.stdout.splitlines():
        fields = line.split()
        if fields:
            defined.add(fields[-1])

    return [f"undefined interposed symbol: {name}"
            for name in symbols if name not in defined]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(REPO))
    ap.add_argument("--archive", default=None)
    ap.add_argument("--gate-only", action="store_true",
                    help="skip the archive symbol audit")
    args = ap.parse_args(argv)

    root = Path(args.root)
    archive = (Path(args.archive) if args.archive
               else root / "build" / "libpermute_guest.a")

    failures = 0

    if not args.gate_only:
        problems = check_archive(root, archive)
        if problems:
            failures += len(problems)
            print(f"symcheck: FAIL ({len(problems)} symbol problem(s))")
            for problem in problems:
                print(f"  {problem}")
        else:
            count = len(parse_symbols(root / "abi" / "permute_iface.def"))
            print(f"symcheck: PASSED ({count} symbol(s) defined)")

    violations = scan_tree(root)
    if violations:
        failures += len(violations)
        print(f"determinism gate: FAIL ({len(violations)} violation(s))")
        for violation in violations:
            print(f"  {violation}")
    else:
        print("determinism gate: PASSED (no real time, entropy, identity, "
              "or unordered containers)")

    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
