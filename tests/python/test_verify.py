# SPDX-License-Identifier: Apache-2.0
"""P2-08: the determinism self-check (FR-6.11)."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from cli.permute.verify import sample_seeds  # noqa: E402

checks = 0
failures = 0
DIGEST = re.compile(r"\b[0-9a-f]{16}\b")


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def cli(*args, cwd, env=None):
    merged = {**os.environ, **(env or {})}
    return subprocess.run([str(REPO / "tools" / "permute"), *map(str, args)],
                          cwd=cwd, env=merged, capture_output=True, text=True,
                          timeout=600)


def project(tmp: Path, name: str) -> Path:
    path = tmp / name
    result = cli("init", path, cwd=tmp)
    if result.returncode != 0:
        raise SystemExit(result.stderr)
    return path


def test_sampling():
    check(sample_seeds([], 0.5) == [], "sample: no seeds")
    check(sample_seeds(list(range(10)), 0.0) == [], "sample: rate 0")
    check(sample_seeds(list(range(10)), 1.0) == list(range(10)),
          "sample: rate 1 takes all")
    picked = sample_seeds(list(range(10)), 0.3)
    check(len(picked) == 3 and picked[0] == 0, f"sample: {picked}")


def test_deterministic(tmp: Path):
    guest = REPO / "build" / "scenarios" / "proc_s01_hello__0.so"
    if not guest.is_file():
        raise SystemExit("missing scenarios; run make build")
    path = project(tmp, "deterministic")
    result = cli("run", "--seeds", "0-2", "--verify", "1.0", "--guest", guest,
                 cwd=path)
    check(result.returncode == 0,
          f"deterministic: exit {result.returncode}: {result.stderr}")
    check("0 mismatch" in result.stdout,
          f"deterministic: {result.stdout.strip()!r}")
    summary = json.loads((path / ".permute/run/summary.json").read_text())
    check(summary["verify"]["checked"] == 3,
          f"deterministic: {summary['verify']}")
    check(not summary["verify"]["mismatches"], "deterministic: no mismatch")


def test_planted_nondeterminism(tmp: Path):
    guest = REPO / "build" / "scenarios" / "proc_s01_hello__0.so"
    path = project(tmp, "planted")
    counter = tmp / "counter"
    counter.mkdir()
    result = cli("run", "--seeds", "0-2", "--verify", "1.0", "--guest", guest,
                 cwd=path,
                 env={"PERMUTE_NONDETERMINISTIC_SEED": "1",
                      "PERMUTE_NONDET_DIR": str(counter)})
    check(result.returncode == 5,
          f"planted: exit {result.returncode} (want 5)")
    check("NONDETERMINISM" in result.stdout,
          f"planted: {result.stdout.strip()!r}")
    check("seed 1" in result.stdout, "planted: names the seed")
    digests = DIGEST.findall(result.stdout)
    check(len(digests) >= 2 and digests[0] != digests[1],
          f"planted: both digests shown, got {digests}")

    summary = json.loads((path / ".permute/run/summary.json").read_text())
    mismatches = summary["verify"]["mismatches"]
    check(len(mismatches) == 1 and mismatches[0]["seed"] == 1,
          f"planted: {mismatches}")
    check(mismatches[0]["first"] != mismatches[0]["second"],
          "planted: the digests differ")


def test_invalid_rate(tmp: Path):
    path = project(tmp, "rate")
    result = cli("run", "--seeds", "0", "--verify", "2.0", cwd=path)
    check(result.returncode == 2, f"rate: exit {result.returncode} (want 2)")


def test_builtin_is_deterministic(tmp: Path):
    path = project(tmp, "builtin")
    result = cli("run", "--seeds", "0-2", "--verify", "1.0", cwd=path)
    check(result.returncode == 0,
          f"builtin: exit {result.returncode}: {result.stderr}")
    check("0 mismatch" in result.stdout, "builtin: no mismatch")


def main() -> int:
    test_sampling()
    with tempfile.TemporaryDirectory() as tmp:
        test_deterministic(Path(tmp))
        test_planted_nondeterminism(Path(tmp))
        test_invalid_rate(Path(tmp))
        test_builtin_is_deterministic(Path(tmp))
    print(f"test_verify: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
