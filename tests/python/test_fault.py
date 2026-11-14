# SPDX-License-Identifier: Apache-2.0
"""Fault engine: materialisation locality and observable actions.

The negative test is FR-4.3: adding a fault site must not change a
scheduling-only digest. A zero-probability rule exercises the site on every
send and must leave the trace byte-identical; a firing rule must change it.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from cli.permute import config  # noqa: E402
from cli.permute.trace import codec  # noqa: E402

RUNNER = REPO / "build" / "permute-run"
SCENARIOS = REPO / "build" / "scenarios"

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def run(name: str, trace: Path, faults: str | None, seed: int = 1):
    env = {**os.environ,
           "PERMUTE_GUEST": str(SCENARIOS / f"proc_{name}__0.so")}
    if faults is not None:
        env["PERMUTE_FAULTS"] = faults

    result = subprocess.run([str(RUNNER), str(seed), str(trace)], env=env,
                            capture_output=True, text=True, timeout=180)
    fields = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            fields[key] = value
    fields["exit"] = result.returncode
    fields["stderr"] = result.stderr
    return fields


def digest(fields):
    return fields.get("DIGEST")


def main() -> int:
    global checks, failures

    if not RUNNER.is_file():
        raise SystemExit("missing build/permute-run; run make build")
    if not (SCENARIOS / "proc_s01_hello__0.so").is_file():
        raise SystemExit("missing scenarios; run make scenarios")

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)

        control = run("s01_hello", tmp_path / "control.pmt", None)
        check(control["exit"] == 0, f"control exit {control['exit']}")
        check(control.get("OUTCOME") == "CLEAN", "control outcome")
        d0 = digest(control)

        # FR-4.3: the site is exercised on every send, the rule never fires.
        noop = run("s01_hello", tmp_path / "noop.pmt", "send:drop@0")
        check(noop["exit"] == 0, f"zero-probability exit {noop['exit']}")
        check(digest(noop) == d0,
              f"adding a fault site changed the digest: {digest(noop)} != {d0}")

        timeout = run("s01_hello", tmp_path / "timeout.pmt", "send:timeout@1")
        check(timeout["exit"] != 0, "a fired timeout must be observable")
        check(digest(timeout) != d0, "a fired fault must change the digest")

        delay = run("s01_hello", tmp_path / "delay.pmt", "send:delay@1:1000")
        check(delay["exit"] == 0, f"delay exit {delay['exit']}")
        check(digest(delay) != d0, "a delay must change the trace")

        duplicate = run("s01_hello", tmp_path / "dup.pmt",
                        "send:duplicate@1")
        check(duplicate["exit"] != 0,
              "a duplicated stream must not be reported as clean")
        check(digest(duplicate) != d0, "duplicate must change the digest")

        # s04 sends one pulse and expects to lose it to a short buffer. If the
        # pulse is dropped instead, the receive has nothing and must not pass.
        pulse = run("s04_pulse_drop", tmp_path / "pulse.pmt", "pulse:drop@1")
        check(pulse["exit"] != 0, "a dropped pulse must not look like EFAULT")

        # A bad rule is a configuration error, not a silent no-op.
        bad = run("s01_hello", tmp_path / "bad.pmt", "send:explode@1")
        check(bad["exit"] == 7, f"bad rule exit {bad['exit']} (want 7)")

        # The reader agrees with every writer.
        for name in ("control", "noop", "delay"):
            parsed = codec.read(tmp_path / f"{name}.pmt")
            check(parsed.ok, f"{name}: trace does not verify")

    # Config validation uses the same grammar as the C parser.
    base = {"project": {"name": "x"}, "faults": {"inject": ["send:drop@0.01"]}}
    config.validate(base)
    checks += 1

    for rule in ("send:explode@0.1", "send:drop@2", "nope:drop@0.1",
                 "send:drop"):
        try:
            config.validate({"project": {"name": "x"},
                             "faults": {"inject": [rule]}})
        except config.ConfigError:
            checks += 1
            continue
        check(False, f"config accepted invalid fault rule {rule!r}")

    print(f"test_fault_e2e: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
