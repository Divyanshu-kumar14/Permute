#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""NFR bench suite (P3-06).

Measures the five numbers the PRD names, on this machine, with the commit that
produced them:

  NFR-2  seed throughput, per worker, four-thread SUT
  NFR-3  non-blocking kernel call cost
  NFR-4  cooperative switch cost
  NFR-5  20,000 seeds across the available cores
  NFR-6  worker RSS for a four-thread SUT

The trace is disabled for the microbenchmarks: the cost of a log record is a
different number, and mixing it in would hide the kernel path.
"""

from __future__ import annotations

import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from cli.permute.run import execute  # noqa: E402

BUILD = REPO / "build"
GUESTS = BUILD / "bench"
BENCH_GUEST = BUILD / "bench_guest"
SWITCH_BENCH = BUILD / "switch_bench"
WORKER = BUILD / "permute-worker"
RUNNER = BUILD / "permute-run"
RESULTS = REPO / "bench" / "results.md"

KERNEL_CALLS = 2_000_000
ROUND_TRIPS = 100_000
MESSAGES = ROUND_TRIPS * 2
SEEDS = 20_000
SEED_JOBS = min(16, os.cpu_count() or 1)


def build_guest(source: str, name: str) -> Path:
    out = GUESTS / name
    out.mkdir(parents=True, exist_ok=True)
    guest = out / f"proc_{name}__0.so"
    if guest.is_file() and guest.stat().st_mtime > (REPO / source).stat().st_mtime:
        return guest
    result = subprocess.run(
        [sys.executable, str(REPO / "tools" / "make_guest.py"),
         "--source", source, "--name", name, "--instance", "0",
         "--out", str(out), "--build-dir", str(BUILD)],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"guest build failed: {source}\n{result.stderr}")
    return guest


def run_guest(guest: Path, units: int) -> dict:
    result = subprocess.run([str(BENCH_GUEST), str(guest), str(units)],
                            capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        raise SystemExit(f"bench guest failed: {result.stderr}")
    fields = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            fields[key] = int(value)
    return fields


def worker_rss_mb(guest: Path) -> float:
    """Peak RSS of the worker and the SUT process it waits for.

    Measured in a fresh interpreter so RUSAGE_CHILDREN contains only this
    worker run, not the microbenchmarks that ran before it.
    """
    script = """
import os, resource, subprocess, sys, tempfile
with tempfile.TemporaryDirectory() as tmp:
    env = dict(os.environ)
    env["PERMUTE_GUEST"] = sys.argv[1]
    subprocess.run([sys.argv[2], "--seeds", "0-63", "--out", tmp,
                    "--runner", sys.argv[3], "--wall-ms", "2000",
                    "--shard", "0/1", "--batch", "64",
                    "--report", os.path.join(tmp, "report.json"), "--quiet"],
                   env=env, check=False)
print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
"""
    result = subprocess.run(
        [sys.executable, "-c", script, str(guest), str(WORKER), str(RUNNER)],
        capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        raise SystemExit(f"rss probe failed: {result.stderr}")
    return int(result.stdout.strip()) / 1024.0


def hardware() -> dict:
    model = "unknown"
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                model = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    try:
        sha = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short",
                              "HEAD"], capture_output=True, text=True,
                             check=True).stdout.strip()
    except Exception:
        sha = "unknown"
    return {
        "cpu": model,
        "cores": os.cpu_count() or 1,
        "kernel": platform.release(),
        "os": platform.platform(),
        "commit": sha,
        "date": time.strftime("%Y-%m-%d", time.gmtime()),
    }


def main() -> int:
    for required in (BENCH_GUEST, SWITCH_BENCH, WORKER, RUNNER):
        if not required.is_file():
            raise SystemExit(f"missing {required}; run make build")

    kernel_call = build_guest("tests/fixtures/bench/kernel_call.c",
                              "kernel_call")
    throughput = build_guest("tests/fixtures/bench/throughput.c",
                             "throughput")
    four_threads = build_guest("tests/fixtures/bench/four_threads.c",
                               "four_threads")

    calls = run_guest(kernel_call, KERNEL_CALLS)
    messages = run_guest(throughput, MESSAGES)
    switch = subprocess.run([str(SWITCH_BENCH), str(build_guest(
        "tests/fixtures/bench/yield_switch.c", "yield_switch"))],
        capture_output=True, text=True, timeout=600)
    switch_ns = 0
    for line in switch.stdout.splitlines():
        if line.startswith("SWITCH_NS="):
            switch_ns = int(line.split("=", 1)[1])
    if switch.returncode != 0 or switch_ns == 0:
        raise SystemExit(f"switch bench failed: {switch.stderr}")

    rss_mb = worker_rss_mb(four_threads)

    with tempfile.TemporaryDirectory() as tmp:
        start = time.monotonic()
        code = execute(f"0-{SEEDS - 1}", tmp, jobs=SEED_JOBS,
                       runner=str(RUNNER), worker=str(WORKER), wall_ms=2000,
                       quiet=True, guest=str(four_threads))
        wall = time.monotonic() - start
    seeds_per_s = SEEDS / wall if wall > 0 else 0
    seeds_per_worker = seeds_per_s / SEED_JOBS

    rows = []

    def row(name, nfr, measured, verdict, note=""):
        rows.append((name, nfr, measured, verdict, note))

    kernel_ns = calls["NS_PER_UNIT"]
    row("Kernel call (non-blocking)", "60-150 ns", f"{kernel_ns} ns",
        "PASS" if kernel_ns <= 150 else "FAIL",
        "below the modelled band" if kernel_ns < 60 else "")

    row("Cooperative switch", "1.5-4 us", f"{switch_ns} ns",
        "PASS" if switch_ns <= 4000 else "FAIL",
        "below the modelled band, faster" if switch_ns < 1500 else "")

    msg_s = messages["UNITS_PER_S"]
    row("Message throughput", ">= 100,000 msg/s", f"{msg_s:,} msg/s",
        "PASS" if msg_s >= 100000 else "FAIL",
        f"{ROUND_TRIPS:,} round trips")

    row("Worker + SUT RSS (4 threads)", "<= 70 MB", f"{rss_mb:.1f} MB",
        "PASS" if rss_mb <= 70 else "FAIL")

    row("20,000 seeds wall", f"<= 180 s on {SEED_JOBS} workers",
        f"{wall:.1f} s ({seeds_per_s:,.0f} seeds/s)",
        "PASS" if wall <= 180 and code == 0 else "FAIL")

    row("Seed throughput per worker", ">= 15 seeds/s",
        f"{seeds_per_worker:,.1f} seeds/s/worker",
        "PASS" if seeds_per_worker >= 15 else "FAIL")

    hw = hardware()
    failed = [item for item in rows if item[3] != "PASS"]

    lines = [
        "<!-- SPDX-License-Identifier: Apache-2.0 -->",
        "",
        "# Bench results",
        "",
        f"**Commit:** `{hw['commit']}`  ",
        f"**Date:** {hw['date']}  ",
        f"**CPU:** {hw['cpu']}  ",
        f"**Cores:** {hw['cores']}  ",
        f"**Kernel:** {hw['kernel']}  ",
        f"**OS:** {hw['os']}",
        "",
        "| Criterion | NFR | Measured | Verdict | Note |",
        "|---|---|---|---|---|",
    ]
    for name, nfr, measured, verdict, note in rows:
        lines.append(f"| {name} | {nfr} | {measured} | {verdict} | {note} |")
    lines += [
        "",
        "Microbenchmarks run with the trace disabled; throughput counts both the",
        "send and the reply. The seed run uses the four-thread fixture over",
        f"20,000 seeds on {SEED_JOBS} workers.",
        "",
        ("**Result: PASS**" if not failed
         else "**Result: FAIL** — " + ", ".join(item[0] for item in failed)),
        "",
    ]
    RESULTS.write_text("\n".join(lines))

    for name, nfr, measured, verdict, note in rows:
        print(f"{name:32s} {nfr:22s} {measured:28s} {verdict}"
              + (f"  ({note})" if note else ""))
    print(f"bench: {len(rows) - len(failed)}/{len(rows)} criteria pass; "
          f"results in {RESULTS}")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
