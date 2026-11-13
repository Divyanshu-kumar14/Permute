#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Follow QUICKSTART.md's commands and check the output it promises.

A quickstart that has drifted from the code is worse than none: it teaches a
new reader commands that no longer work. This runs the documented sequence in a
temporary directory and asserts the claimed lines appear.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PERMUTE = REPO / "tools" / "permute"

failures = 0


def run(args, cwd=None):
    result = subprocess.run([str(PERMUTE), *args], cwd=cwd,
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"permute {' '.join(args)} exited {result.returncode}\n"
            f"{result.stdout}{result.stderr}")
    return result.stdout


def expect(text: str, needle: str, what: str) -> None:
    global failures
    if needle not in text:
        failures += 1
        print(f"FAIL {what}: expected {needle!r}\n{text}")


def main() -> int:
    global failures

    if not PERMUTE.is_file():
        print(f"quickstart: missing {PERMUTE}", file=sys.stderr)
        return 2

    if not (REPO / "build" / "permute-run").is_file():
        print("quickstart: build the harness first (make build)", file=sys.stderr)
        return 2

    version = run(["--version"])
    expect(version, "permute 0.1.0", "version")

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        project = tmp_path / "demo"

        init = run(["init", str(project)])
        expect(init, "initialised", "init")
        for name in ("permute.toml", "symbol-audit.txt", "header-gaps.md"):
            if not (project / name).is_file():
                failures += 1
                print(f"FAIL init did not write {name}")

        run_out = run(["run"], cwd=project)
        expect(run_out, "3 seeds: 3 clean", "run")

        show = run(["show", "--seed", "0"], cwd=project)
        expect(show, "records in", "show")
        expect(show, "digest", "show")

        trace = project / ".permute" / "run" / "traces" / "seed-0.pmt"
        replay = run(["replay", "artifact", str(trace)], cwd=project)
        expect(replay, "verified", "replay")

    if failures:
        print(f"quickstart: FAILED ({failures} failure(s))")
        return 1

    print("quickstart: PASSED (init -> run -> show -> replay)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
