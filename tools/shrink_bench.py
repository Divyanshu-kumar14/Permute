#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""P3-04: shrinker performance over 20 real failures.

G2 requires p95 <= 30 s and a reduced reproduction of <= 20 events. Every case
here is a real artifact shrunk through `permute shrink`; the reduced input is
then executed again and its trace record count is the event count.

The failures are the s01 deadlock with a growing set of redundant fault rules.
That is deliberate: the shrinker must *discard* the rules that do not matter,
so the run exercises ddmin and fault materialisation rather than a one-item
special case.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import platform
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from cli.permute.shrink.runner import write_artifact  # noqa: E402
from cli.permute.trace import codec  # noqa: E402

CASES = 20
P95_LIMIT = 30.0
EVENT_LIMIT = 20
NEEDED = "send:drop@1"
REDUNDANT = (
    "pulse:drop@0",
    "recv:timeout@0",
    "reply:delay@0",
    "recv:delay@0",
    "pulse:delay@0",
    "send:delay@0",
)
INVARIANT = {"id": "dl", "kind": "deadlock", "severity": "error"}
FINDING = {
    "invariant": "dl",
    "kind": "deadlock",
    "severity": "error",
    "seq": 0,
    "t_ns": 0,
    "detail": "shrink-bench",
}


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    rank = max(1, math.ceil(fraction * len(ordered)))
    return ordered[rank - 1]


def faults_for(index: int) -> list[str]:
    count = 2 + (index % 6)               # 2..7 redundant rules
    rotated = REDUNDANT[index % len(REDUNDANT):] + \
        REDUNDANT[:index % len(REDUNDANT)]
    return [NEEDED, *rotated[:count]]


def run_case(index: int, tmp: Path) -> dict:
    seed = index + 1
    artifact = tmp / f"case-{index:02d}.json"
    guest = str(REPO / "build/scenarios/proc_s01_hello__0.so")
    write_artifact(
        artifact, seed=seed, guest=guest,
        faults=faults_for(index), stimuli=[], invariant=INVARIANT,
        finding=FINDING, trace=tmp / f"case-{index:02d}.pmt", digest=None,
    )

    started = time.monotonic()
    result = subprocess.run(
        [str(REPO / "tools/permute"), "shrink", str(artifact), "--json"],
        cwd=REPO, capture_output=True, text=True, timeout=180,
    )
    elapsed = time.monotonic() - started

    if result.returncode != 0:
        raise RuntimeError(
            f"case {index}: shrink exited {result.returncode}: "
            f"{result.stderr.strip()}"
        )
    payload = json.loads(result.stdout)
    reduced = payload["input"]

    trace = tmp / f"case-{index:02d}.final.pmt"
    env = {
        **os.environ,
        "PERMUTE_GUEST": guest,
    }
    env.pop("PERMUTE_FAULTS", None)
    if reduced["faults"]:
        env["PERMUTE_FAULTS"] = ",".join(reduced["faults"])
    rerun = subprocess.run(
        [str(REPO / "build/permute-run"), str(seed), str(trace)],
        cwd=REPO, env=env, capture_output=True, text=True, timeout=60,
    )
    # A deadlock is the expected outcome; the runner signals it with exit 1.
    if rerun.returncode not in (0, 1) or not trace.is_file():
        raise RuntimeError(
            f"case {index}: reduced input did not execute "
            f"(exit {rerun.returncode})"
        )
    events = len(codec.read(trace, verify=False).records)

    return {
        "case": index,
        "seed": seed,
        "faults": len(reduced["faults"]),
        "original": payload["original_size"],
        "reduced": payload["final_size"],
        "events": events,
        "seconds": round(elapsed, 3),
        "minimality": payload["minimality"],
    }


def machine() -> dict:
    cpu = "unknown"
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    try:
        commit = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"], cwd=REPO,
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        commit = "unknown"
    return {
        "commit": commit,
        "date": time.strftime("%Y-%m-%d", time.localtime()),
        "cpu": cpu,
        "cores": os.cpu_count(),
        "kernel": platform.release(),
        "os": platform.platform(),
        "python": platform.python_version(),
    }


def markdown(rows: list[dict], summary: dict, host: dict) -> str:
    lines = [
        "<!-- SPDX-License-Identifier: Apache-2.0 -->",
        "",
        "# Shrink bench results",
        "",
        f"**Commit:** `{host['commit']}`  ",
        f"**Date:** {host['date']}  ",
        f"**CPU:** {host['cpu']}  ",
        f"**Cores:** {host['cores']}  ",
        f"**Kernel:** {host['kernel']}  ",
        f"**OS:** {host['os']}  ",
        f"**Python:** {host['python']}",
        "",
        "Twenty s01 deadlock artifacts with 3-8 fault rules, each shrunk through",
        "`permute shrink` and re-executed to count the reduced trace records.",
        "",
        "| Case | Seed | Inputs (orig -> reduced) | Events | Wall (s) | Minimality |",
        "|---|---|---|---|---|---|",
    ]
    for row in rows:
        lines.append(
            f"| {row['case']} | {row['seed']} | "
            f"{row['original']} -> {row['reduced']} | {row['events']} | "
            f"{row['seconds']:.3f} | {row['minimality']} |"
        )
    lines += [
        "",
        f"| Metric | Limit | Measured | Verdict |",
        f"|---|---|---|---|",
        f"| p50 wall | - | {summary['p50']:.3f} s | - |",
        f"| p95 wall | <= {P95_LIMIT:.0f} s | {summary['p95']:.3f} s | "
        f"{'PASS' if summary['p95'] <= P95_LIMIT else 'FAIL'} |",
        f"| Max reduced events | <= {EVENT_LIMIT} | {summary['max_events']} | "
        f"{'PASS' if summary['max_events'] <= EVENT_LIMIT else 'FAIL'} |",
        "",
        f"**Result: {'PASS' if summary['passed'] else 'FAIL'}**",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", type=int, default=CASES)
    ap.add_argument("--write", type=Path,
                    help="write Markdown evidence to this path")
    ap.add_argument("--json", action="store_true", help="print a JSON summary")
    args = ap.parse_args()

    for path in (REPO / "tools/permute", REPO / "build/permute-run",
                 REPO / "build/scenarios/proc_s01_hello__0.so"):
        if not path.exists():
            print(f"shrink-bench: missing {path} (run make build scenarios)",
                  file=sys.stderr)
            return 2

    rows = []
    failures = 0
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        for index in range(args.cases):
            try:
                rows.append(run_case(index, tmp))
            except (RuntimeError, subprocess.TimeoutExpired,
                    json.JSONDecodeError) as exc:
                failures += 1
                print(f"FAIL {exc}", file=sys.stderr)

    times = [row["seconds"] for row in rows]
    summary = {
        "cases": len(rows),
        "failures": failures,
        "p50": percentile(times, 0.50) if times else 0.0,
        "p95": percentile(times, 0.95) if times else 0.0,
        "max_events": max((row["events"] for row in rows), default=0),
        "max_reduced": max((row["reduced"] for row in rows), default=0),
    }
    summary["passed"] = (
        failures == 0
        and len(rows) == args.cases
        and summary["p95"] <= P95_LIMIT
        and summary["max_events"] <= EVENT_LIMIT
    )

    if args.json:
        print(json.dumps({"summary": summary, "cases": rows}, indent=2,
                         sort_keys=True))
    else:
        print(f"shrink-bench: {len(rows)} failures, "
              f"p50 {summary['p50']:.3f} s, p95 {summary['p95']:.3f} s "
              f"(limit {P95_LIMIT:.0f} s)")
        print(f"{'case':>4} {'seed':>4} {'inputs':>13} {'events':>6} "
              f"{'wall s':>7}  minimality")
        for row in rows:
            print(f"{row['case']:>4} {row['seed']:>4} "
                  f"{row['original']:>5} -> {row['reduced']:<4} "
                  f"{row['events']:>6} {row['seconds']:>7.3f}  "
                  f"{row['minimality']}")
        print(f"Result: {'PASS' if summary['passed'] else 'FAIL'}")

    if args.write:
        host = machine()
        args.write.parent.mkdir(parents=True, exist_ok=True)
        args.write.write_text(markdown(rows, summary, host))

    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
