#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run every scenario through the executor, not just through the compiler.

This is the check P1 could not make: guest code actually executing on the
simulated kernel, with the baton deciding which thread runs and the shims
routing every call.

For each scenario: exit 0, OUTCOME=CLEAN, a trace with records, and a digest
that the reader agrees with. s01 is run twice to confirm the executor itself
is deterministic.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from cli.permute.trace import codec  # noqa: E402

RUNNER = REPO / "build" / "permute-run"
SCENARIOS = REPO / "build" / "scenarios"

# s07 needs PTHREAD_PRIO_PROTECT, which glibc does not implement and our mutex
# shim does not model yet. It compiles (the scenario gate covers that) but
# cannot execute honestly, so it is skipped by name rather than faked.
SKIP_EXEC = {"s07_priority_ceiling"}

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def run(name: str, trace: Path, seed: int = 1) -> dict:
    guest = SCENARIOS / f"proc_{name}__0.so"
    env = {**os.environ, "PERMUTE_GUEST": str(guest)}
    result = subprocess.run([str(RUNNER), str(seed), str(trace)], env=env,
                            capture_output=True, text=True, timeout=120)
    fields = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            fields[key] = value
    fields["exit"] = str(result.returncode)
    fields["stderr"] = result.stderr
    return fields


def main() -> int:
    if not RUNNER.is_file():
        raise SystemExit("missing build/permute-run; run make build")

    scenarios = []
    for path in sorted(SCENARIOS.glob("proc_*__0.so")):
        stem = path.name[len("proc_"):]
        scenarios.append(stem.rsplit("__", 1)[0])
    if not scenarios:
        raise SystemExit("no scenario objects; run make scenarios")

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)

        for name in scenarios:
            if name in SKIP_EXEC:
                print(f"  {name}: skipped (mutex shim pending)")
                continue
            trace = tmp_path / f"{name}.pmt"
            fields = run(name, trace)
            check(fields.get("exit") == "0",
                  f"{name}: exit {fields.get('exit')} {fields.get('stderr')}")
            check(fields.get("OUTCOME") == "CLEAN",
                  f"{name}: outcome {fields.get('OUTCOME')}")
            check(int(fields.get("RECORDS", "0")) > 0,
                  f"{name}: no trace records")

            parsed = codec.read(trace)
            check(parsed.ok, f"{name}: trace does not verify")
            if parsed.run_digest is not None:
                check(f"{parsed.run_digest:016x}" == fields.get("DIGEST"),
                      f"{name}: digest mismatch with the writer")

        # The executor is deterministic: same seed, same digest.
        a = run("s01_hello", tmp_path / "a.pmt")
        b = run("s01_hello", tmp_path / "b.pmt")
        check(a.get("DIGEST") == b.get("DIGEST"),
              "s01: executor digest is not stable")
        check(int(a.get("RECORDS", "0")) == 30000,
              f"s01: expected 30,000 records, got {a.get('RECORDS')}")

    executed = len(scenarios) - len(SKIP_EXEC & set(scenarios))
    print(f"executor: {checks} checks, {failures} failure(s) "
          f"({executed} scenario(s) executed, {len(SKIP_EXEC)} skipped)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
