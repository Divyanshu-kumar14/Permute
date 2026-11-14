#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""PRD §17 submission checklist, mechanically.

Two kinds of row:
  PASS     proven by a file or a scan of tracked content
  BLOCKED  requires the QNX SDP; reported, never faked

The scan runs over `git ls-files`, so build output and local artifacts cannot
hide a violation or create one. It exits 1 only on a real failure (missing
licence, prohibited content, secret, missing documented procedure). A blocked
row does not fail: the repository is honest about what it cannot prove yet.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

SDP = "8.0"
TARGET = "x86_64"
TOOLCHAIN = "qcc -Vgcc_ntox86_64"
GCC_BASE = "GCC 12"

# Fingerprints of vendor headers/binaries we must not redistribute.
VENDOR_MARKERS = (
    "QNX Software Systems",
    "BlackBerry Limited",
    "BlackBerry QNX",
    "qnx700",
    "qnx800",
)

SECRET_PATTERNS = (
    re.compile(r"AKIA[0-9A-Z]{16}"),
    re.compile(r"gh[pousr]_[A-Za-z0-9]{30,}"),
    re.compile(r"sk-(?:ant-|proj-)?[A-Za-z0-9_-]{20,}"),
    re.compile(r"xox[baprs]-[A-Za-z0-9-]{10,}"),
    re.compile(r"-----BEGIN (?:RSA|OPENSSH|EC|DSA) PRIVATE KEY-----"),
)

BINARY_SUFFIXES = {
    ".so", ".a", ".o", ".obj", ".elf", ".bin", ".ifs", ".img", ".pyc",
}
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".svg", ".ico", ".webp"}

QNX_EVIDENCE_FIELDS = (
    "SDP version",
    "`uname -a`",
    "Image SHA-256",
    "QEMU invocation",
    "Serial log excerpt",
    "Commit SHA",
    "Oracle traces",
    "`permute-capture`",
)

rows: list[tuple[str, str, str]] = []


def record(name: str, verdict: str, detail: str) -> None:
    rows.append((name, verdict, detail))


def tracked() -> list[Path]:
    """Every tracked file, or every file when git is absent (source tarball)."""

    try:
        out = subprocess.run(["git", "-C", str(REPO), "ls-files"],
                             capture_output=True, text=True, check=True)
        names = [line for line in out.stdout.splitlines() if line]
        if names:
            return [REPO / line for line in names]
    except (OSError, subprocess.CalledProcessError):
        pass

    skip = {".git", "build", "build-asan", "build-cov", "build-mutants",
            "dist", "__pycache__", ".pytest_cache", ".orchestrator"}
    found = []
    for path in sorted(REPO.rglob("*")):
        if not path.is_file():
            continue
        if any(part in skip or part.startswith("build-") for part in path.parts):
            continue
        found.append(path)
    return found


def read(relative: str) -> str:
    path = REPO / relative
    try:
        return path.read_text(errors="replace")
    except OSError:
        return ""


def main() -> int:
    files = tracked()
    failures = 0

    # 1. Licence at root, detectable.
    text = read("LICENSE")
    if "Apache License" in text and "Version 2.0" in text:
        record("Licence at root", "PASS", "LICENSE is Apache-2.0")
    else:
        record("Licence at root", "FAIL", "LICENSE missing or not Apache-2.0")
        failures += 1

    # 2. Source completeness: the build, the docs, the tests.
    required = ["CMakeLists.txt", "Makefile", "README.md", "NOTICE",
                "docs/REPRODUCE.md", "tests", "tools"]
    missing = [name for name in required if not (REPO / name).exists()]
    if missing:
        record("Source completeness", "FAIL", f"missing {', '.join(missing)}")
        failures += 1
    else:
        record("Source completeness", "PASS",
               f"{len(files)} tracked files, build and tests present")

    # 3. Build and run instructions name the QNX side.
    reproduce = read("docs/REPRODUCE.md")
    absent = [item for item in (SDP, TARGET, TOOLCHAIN, GCC_BASE)
              if item not in reproduce]
    if absent:
        record("QNX build instructions", "FAIL",
               f"docs/REPRODUCE.md does not name {', '.join(absent)}")
        failures += 1
    else:
        record("QNX build instructions", "PASS",
               f"SDP {SDP}, {TARGET}, {TOOLCHAIN} ({GCC_BASE} base)")

    # 4. Category and prior-work disclosure.
    readme = read("README.md")
    notice = read("NOTICE")
    problems = []
    if "Developer Tooling and Host Tools" not in readme:
        problems.append("README does not name the submission category")
    if "prior work" not in readme.lower() and "pre-existing" not in readme.lower():
        problems.append("README does not disclose prior work")
    if "Third-party" not in notice:
        problems.append("NOTICE has no third-party section")
    if problems:
        record("Category and prior work", "FAIL", "; ".join(problems))
        failures += 1
    else:
        record("Category and prior work", "PASS",
               "category named; prior work disclosed in README and NOTICE")

    # 5. QNX evidence: required fields present, values honestly absent.
    evidence = read("docs/QNX-EVIDENCE.md")
    if not evidence:
        record("QNX evidence document", "FAIL", "docs/QNX-EVIDENCE.md missing")
        failures += 1
    else:
        absent = [field for field in QNX_EVIDENCE_FIELDS
                  if field not in evidence]
        if absent:
            record("QNX evidence document", "FAIL",
                   f"fields not listed: {', '.join(absent)}")
            failures += 1
        elif "not available" not in evidence:
            record("QNX evidence document", "FAIL",
                   "fields are not marked unavailable; do not invent values")
            failures += 1
        else:
            record("QNX evidence document", "BLOCKED",
                   "fields listed, no SDP installed: nothing claimed")

    # 6. Prohibited content: vendor files, images, binaries, secrets.
    violations: list[str] = []
    for path in files:
        relative = path.relative_to(REPO)
        if path.suffix in BINARY_SUFFIXES:
            violations.append(f"{relative}: binary")
            continue
        if path.suffix in IMAGE_SUFFIXES:
            violations.append(f"{relative}: image (logo check is manual)")
            continue
        try:
            body = path.read_text(errors="replace")
        except OSError:
            continue
        for marker in VENDOR_MARKERS:
            if marker in body:
                violations.append(f"{relative}: vendor marker {marker!r}")
        for pattern in SECRET_PATTERNS:
            if pattern.search(body):
                violations.append(f"{relative}: secret-like token")
    if violations:
        record("Prohibited content", "FAIL", "; ".join(violations[:5]))
        failures += 1
    else:
        record("Prohibited content", "PASS",
               "no vendor files, images, binaries, or secrets in "
               f"{len(files)} tracked files")

    # 7. The QNX capture itself: absent until the SDP exists.
    manifest = REPO / "conformance" / "oracle" / "qnx" / "manifest.toml"
    if manifest.is_file():
        record("QNX oracle capture", "PASS", "manifest present")
    else:
        record("QNX oracle capture", "BLOCKED",
               "no capture; conformance reports simulator references")

    # 8. The licence scan that every source file carries SPDX.
    spdx = subprocess.run([sys.executable, str(REPO / "tools" /
                                               "licence_scan.py")],
                          capture_output=True, text=True)
    if spdx.returncode == 0:
        record("SPDX headers", "PASS", spdx.stdout.strip())
    else:
        record("SPDX headers", "FAIL", spdx.stdout.strip().splitlines()[0])
        failures += 1

    width = max(len(name) for name, _, _ in rows)
    print(f"{'check'.ljust(width)}  verdict  detail")
    for name, verdict, detail in rows:
        print(f"{name.ljust(width)}  {verdict:<7}  {detail}")

    passed = sum(1 for _, verdict, _ in rows if verdict == "PASS")
    blocked = sum(1 for _, verdict, _ in rows if verdict == "BLOCKED")
    print()
    if failures:
        print(f"submission: FAILED ({failures} check(s))")
        return 1
    print(f"submission: PASSED ({passed} check(s) verified, "
          f"{blocked} blocked on the QNX SDP)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
