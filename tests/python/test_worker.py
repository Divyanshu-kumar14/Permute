# SPDX-License-Identifier: Apache-2.0
"""Worker: crash isolation, wall budget, sharding, and job-count determinism.

The crash case is the point of the task: a SUT SIGSEGV must be a finding, the
run must continue, and the exit code must say a finding was present.
"""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from cli.permute.run import execute  # noqa: E402
from cli.permute.trace import codec  # noqa: E402

RUNNER = REPO / "build" / "permute-run"
WORKER = REPO / "build" / "permute-worker"

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def run_worker(out: Path, seeds: str, env=None, wall_ms=5000, extra=()):
    merged = {**os.environ, **(env or {})}
    cmd = [str(WORKER), "--seeds", seeds, "--out", str(out),
           "--runner", str(RUNNER), "--wall-ms", str(wall_ms), "--quiet",
           *extra]
    return subprocess.run(cmd, env=merged, capture_output=True, text=True)


def read_report(out: Path) -> dict:
    return json.loads((out / "report.json").read_text())


def digest_of(out: Path, seed: int):
    trace = codec.read(out / "traces" / f"seed-{seed}.pmt")
    return trace.run_digest if trace.ok else None


def test_clean(tmp: Path):
    out = tmp / "clean"
    result = run_worker(out, "0-4")
    report = read_report(out)

    check(result.returncode == 0, f"clean exit {result.returncode}: {result.stderr}")
    check(report["seeds"] == 5 and report["clean"] == 5,
          f"clean counts {report}")
    check(report["findings"] == [], "clean findings")
    check((out / "report.jsonl").read_text().count("\n") == 5,
          "one jsonl line per seed")
    for seed in range(5):
        check(digest_of(out, seed) is not None, f"trace for seed {seed} verifies")


def test_crash_isolation(tmp: Path):
    out = tmp / "crash"
    result = run_worker(out, "0-4", env={"PERMUTE_CRASH_SEED": "2"})
    report = read_report(out)

    check(result.returncode == 1, f"crash exit {result.returncode} (want 1)")
    check(report["crash"] == 1, f"crash count {report['crash']}")
    check(report["clean"] == 4, f"clean count {report['clean']} (run continued)")
    check(report["seeds"] == 5, "all seeds attempted")

    finding = report["findings"][0]
    check(finding["seed"] == 2 and finding["outcome"] == "CRASH",
          f"finding {finding}")
    check(finding["signal"] == 11, f"signal {finding['signal']} (want SIGSEGV)")

    # Seeds after the crash completed and their traces verify.
    for seed in (0, 1, 3, 4):
        check(digest_of(out, seed) is not None, f"trace for seed {seed}")
    check(not (out / "traces" / "seed-2.pmt").exists(),
          "crashed seed has no trace")


def test_wall_budget(tmp: Path):
    out = tmp / "budget"
    result = run_worker(out, "0-2", env={"PERMUTE_SPIN_SEED": "1"}, wall_ms=100)
    report = read_report(out)

    check(result.returncode == 0, f"budget exit {result.returncode} (want 0)")
    check(report["budget"] == 1, f"budget count {report['budget']}")
    check(report["clean"] == 2, "budget did not stop the run")
    check(report["findings"] == [], "budget is not a finding")


def test_shard(tmp: Path):
    full = tmp / "full"
    shard = tmp / "shard"
    run_worker(full, "0-9")
    run_worker(shard, "0-9", extra=("--shard", "1/2"))

    report = read_report(shard)
    check(report["seeds"] == 5, f"shard selected {report['seeds']} seeds")

    for seed in (1, 3, 5, 7, 9):
        check(digest_of(shard, seed) == digest_of(full, seed),
              f"seed {seed} digest differs between shard and full run")
    for seed in (0, 2, 4, 6, 8):
        check(not (shard / "traces" / f"seed-{seed}.pmt").exists(),
              f"seed {seed} should not be in shard 1/2")


def test_jobs_determinism(tmp: Path):
    one = tmp / "jobs1"
    many = tmp / "jobs4"

    code_one = execute("0-7", str(one), jobs=1, runner=str(RUNNER),
                       worker=str(WORKER), wall_ms=5000, quiet=True)
    code_many = execute("0-7", str(many), jobs=4, runner=str(RUNNER),
                        worker=str(WORKER), wall_ms=5000, quiet=True)

    check(code_one == 0, f"jobs=1 exit {code_one}")
    check(code_many == 0, f"jobs=4 exit {code_many}")

    for seed in range(8):
        check(digest_of(one, seed) == digest_of(many, seed),
              f"seed {seed} digest depends on job count")

    merged = read_report(many)
    check(merged["seeds"] == 8 and merged["clean"] == 8,
          f"merged report {merged}")


def main():
    for binary in (RUNNER, WORKER):
        if not binary.exists():
            raise SystemExit(f"missing {binary}; run make build")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        test_clean(tmp)
        test_crash_isolation(tmp)
        test_wall_budget(tmp)
        test_shard(tmp)
        test_jobs_determinism(tmp)

    print(f"test_worker: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
