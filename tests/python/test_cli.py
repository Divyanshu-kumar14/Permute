# SPDX-License-Identifier: Apache-2.0
"""CLI: scaffold, build, run, show, and the exit-code matrix.

The matrix is asserted end to end through the executable, not by importing the
constants, because FR-9.2 is about what a caller sees.
"""

import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from cli.permute.trace import codec  # noqa: E402

PERMUTE = REPO / "tools" / "permute"

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def cli(*args, cwd=None, env=None):
    merged = {**os.environ, **(env or {})}
    return subprocess.run([str(PERMUTE), *map(str, args)], cwd=cwd,
                          env=merged, capture_output=True, text=True)


def test_version():
    result = cli("--version")
    check(result.returncode == 0, f"--version exit {result.returncode}")
    check("permute 0.1.0" in result.stdout, f"--version output {result.stdout!r}")


def test_init_and_run(tmp: Path):
    project = tmp / "demo"
    started = time.monotonic()
    result = cli("init", project)
    check(result.returncode == 0, f"init exit {result.returncode}: {result.stderr}")
    check(project.is_dir(), "project directory")
    check((project / "permute.toml").is_file(), "permute.toml written")
    check((project / "scenarios" / "smoke.c").is_file(), "smoke scenario written")
    check((project / "symbol-audit.txt").is_file(), "symbol audit written")
    check((project / "header-gaps.md").is_file(), "header gaps written")

    audit = (project / "symbol-audit.txt").read_text()
    check("surface:" in audit and "permute_main" not in audit.split("|", 1)[0],
          "audit has a surface line")
    check("| ipc |" in audit, "audit lists the ipc group")
    gaps = (project / "header-gaps.md").read_text()
    check("Header gaps" in gaps and "QNX SDP" in gaps, "header gaps content")

    # The verification: from an empty directory to a green run in <= 60 s.
    run = cli("run", cwd=project)
    elapsed = time.monotonic() - started
    check(run.returncode == 0, f"run exit {run.returncode}: {run.stderr}")
    check(elapsed <= 60, f"init+run took {elapsed:.1f}s")
    check((project / ".permute" / "run" / "report.json").is_file(),
          "run report written")
    report = json.loads((project / ".permute" / "run" / "report.json").read_text())
    check(report["seeds"] == 3 and report["clean"] == 3, f"report {report}")

    # A second init must not clobber a project.
    again = cli("init", project)
    check(again.returncode == 2, f"second init exit {again.returncode}")

    return project


def test_json_and_show(project: Path):
    run = cli("run", "--json", cwd=project)
    check(run.returncode == 0, f"run --json exit {run.returncode}")
    payload = json.loads(run.stdout)
    check(payload["verdict"] == "clean", f"json verdict {payload['verdict']}")
    check(payload["exit_code"] == 0, "json exit code")

    show = cli("show", "--seed", "0", cwd=project)
    check(show.returncode == 0, f"show exit {show.returncode}: {show.stderr}")
    check("digest" in show.stdout, "show prints a digest")

    nd = cli("show", "--seed", "0", "--format", "ndjson", cwd=project)
    check(nd.returncode == 0, f"show ndjson exit {nd.returncode}")
    check(len(nd.stdout.strip().splitlines()) > 0, "ndjson has records")

    chrome = cli("show", "--seed", "0", "--format", "chrome", "--json",
                 cwd=project)
    check(chrome.returncode == 0, f"show chrome exit {chrome.returncode}")
    check(len(json.loads(chrome.stdout)["traceEvents"]) > 0, "chrome events")


def test_build(project: Path):
    built = cli("build", cwd=project)
    check(built.returncode == 0, f"build exit {built.returncode}: {built.stderr}")
    check((project / ".permute" / "build" / "proc_smoke__0.so").is_file(),
          "guest artifact")

    source = project / "scenarios" / "smoke.c"
    original = source.read_text()
    source.write_text("int broken(\n")
    try:
        broken = cli("build", cwd=project)
        check(broken.returncode == 3, f"broken build exit {broken.returncode}")
    finally:
        source.write_text(original)


def test_fault_requires_guest(project: Path):
    config_path = project / "permute.toml"
    original = config_path.read_text()
    config_path.write_text(original.replace("inject = []",
                                            'inject = ["send:drop@0.5"]'))
    try:
        rejected = cli("run", cwd=project)
        check(rejected.returncode == 2,
              f"faults without --guest exit {rejected.returncode}")
        check("P1003" in rejected.stderr,
              f"faults without --guest: {rejected.stderr.strip()}")

        guest = ".permute/build/proc_smoke__0.so"
        accepted = cli("run", "--guest", guest, "--seeds", "0", cwd=project)
        check(accepted.returncode == 0,
              f"faults with --guest exit {accepted.returncode}: "
              f"{accepted.stderr.strip()}")
    finally:
        config_path.write_text(original)


def test_exit_matrix(tmp: Path, project: Path):
    trace = project / ".permute" / "run" / "traces" / "seed-0.pmt"
    check(trace.is_file(), "seed 0 trace exists")

    cases = {}

    cases[0] = cli("--version")
    cases[1] = cli("run", cwd=project, env={"PERMUTE_CRASH_SEED": "1"})

    # 2: a closed-schema config rejects an unknown key.
    config_path = project / "permute.toml"
    original = config_path.read_text()
    config_path.write_text(original + "\nbogus = 1\n")
    try:
        cases[2] = cli("run", cwd=project)
    finally:
        config_path.write_text(original)

    source = project / "scenarios" / "smoke.c"
    original_source = source.read_text()
    source.write_text("int broken(\n")
    try:
        cases[3] = cli("build", cwd=project)
    finally:
        source.write_text(original_source)

    cases[4] = cli("replay", "artifact", trace, "--build-id", "00" * 32)
    cases[11] = cli("replay", "artifact", trace, "--digest", "0")

    # 5: a fixture that changes its seed between runs must be reported as
    # nondeterminism rather than silently accepted.
    counter = tmp / "fixture-count"
    script = tmp / "alternating-fixture.sh"
    script.write_text(
        "#!/bin/sh\n"
        f'n=$(cat "{counter}" 2>/dev/null || echo 0)\n'
        "n=$((n+1))\n"
        f'echo $n > "{counter}"\n'
        f'exec "{REPO}/build/replay_fixture" "$n" "$2"\n'
    )
    script.chmod(0o755)
    cases[5] = cli("replay", "seed", "1", "--fixture", script)

    # 6: the run-level deadline kills the workers and keeps partial evidence.
    cases[6] = cli("run", "--max-wall-ms", "1", "--wall-ms", "30000",
                   cwd=project, env={"PERMUTE_SPIN_SEED": "0"})

    cases[7] = cli("run", cwd=project,
                   env={"PERMUTE_RUNNER": str(tmp / "missing-runner")})
    cases[10] = cli("internal-exit", "10")

    # 130: SIGINT reaches the CLI, which stops its workers and reports it.
    slow = subprocess.Popen(
        [str(PERMUTE), "run", "--wall-ms", "60000"],
        cwd=project, env={**os.environ, "PERMUTE_SPIN_SEED": "0"},
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    time.sleep(0.6)
    slow.send_signal(signal.SIGINT)
    try:
        slow.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        slow.kill()
        slow.communicate()

    expected = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 130}
    actual = {code: result.returncode for code, result in cases.items()}
    actual[130] = slow.returncode

    for code in sorted(expected):
        check(actual[code] == code,
              f"exit {code}: got {actual[code]} "
              f"({cases[code].stderr.strip() if code in cases else ''})")


def main():
    if not PERMUTE.is_file():
        raise SystemExit(f"missing {PERMUTE}")
    if not (REPO / "build" / "permute-run").is_file():
        raise SystemExit("missing build/permute-run; run make build")

    test_version()
    with tempfile.TemporaryDirectory() as tmp_text:
        tmp = Path(tmp_text)
        project = test_init_and_run(tmp)
        test_json_and_show(project)
        test_build(project)
        test_fault_requires_guest(project)
        test_exit_matrix(tmp, project)

    print(f"test_cli: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
