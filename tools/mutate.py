#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Model mutation runner (P2-06, FR-11.6).

Builds the kernel with each of the twelve PRD mutants, runs the conformance
scenario set plus the timeout, PI, unblock and telemetry scenarios, and asks
whether anything notices: a canonical trace difference, an outcome change, a
timeout, or a targeted unit test failure.

A mutant that no scenario and no test catches is a gap in the suite. The plan
requires at least ten of twelve killed; the report shows exactly which.

Usage:
    python3 tools/mutate.py [--only M01,M02] [--keep]
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from cli.permute.trace import codec  # noqa: E402
from conformance.tracecmp import compare_records  # noqa: E402

MUTANTS = REPO / "tests" / "fixtures" / "mutants" / "mutants.toml"
CONF = REPO / "conformance" / "scenarios.toml"
BASE_BUILD = REPO / "build"
MUTANT_ROOT = REPO / "build-mutants"
RUNNER = BASE_BUILD / "permute-run"
SEED = 1


def build_guest(source: Path, name: str, out: Path) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [sys.executable, str(REPO / "tools" / "make_guest.py"),
         "--source", str(source), "--name", name, "--instance", "0",
         "--out", str(out), "--build-dir", str(BASE_BUILD)],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise SystemExit(f"guest build failed: {source}\n{result.stdout}"
                         f"{result.stderr}")
    return out / f"proc_{name}__0.so"


def suite() -> list[tuple[str, Path]]:
    """Every guest the mutants are judged against."""
    scenarios = tomllib.loads(CONF.read_text()).get("scenario", [])
    entries: list[tuple[str, Path]] = []
    for scenario in scenarios:
        if scenario.get("skip"):
            continue
        ident = scenario["id"]
        source = REPO / scenario["file"]
        guest = build_guest(source, ident, BASE_BUILD / "conformance" / ident)
        entries.append((ident, guest))

    extras = [
        ("s02_pi_chain", REPO / "scenarios" / "s02_pi_chain" / "scenario.c"),
        ("s03_timer_timeout",
         REPO / "scenarios" / "s03_timer_timeout" / "scenario.c"),
        ("s06_unblock_pulse",
         REPO / "scenarios" / "s06_unblock_pulse" / "scenario.c"),
        ("telemetry", REPO / "examples" / "telemetry" / "scenario.c"),
        ("m07_two_channels",
         REPO / "tests" / "fixtures" / "mutants" / "m07_two_channels.c"),
    ]
    for name, source in extras:
        out = BASE_BUILD / "mutants" / name
        entries.append((name, build_guest(source, name, out)))
    return entries


def run(runner: Path, guest: Path, trace: Path) -> tuple[dict, list | None]:
    env = {**os.environ, "PERMUTE_GUEST": str(guest)}
    try:
        result = subprocess.run([str(runner), str(SEED), str(trace)], env=env,
                                capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return {"OUTCOME": "TIMEOUT"}, None
    fields = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            fields[key] = value
    if not trace.is_file():
        return fields, None
    parsed = codec.read(trace)
    return fields, (parsed.records if parsed.ok else None)


def configure(mutant: dict) -> Path:
    build = MUTANT_ROOT / mutant["id"]
    if not (build / "CMakeCache.txt").is_file():
        result = subprocess.run(
            ["cmake", "-S", str(REPO), "-B", str(build), "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Debug",
             f"-DPERMUTE_MUTANT={mutant['id']}"],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise SystemExit(f"configure failed for {mutant['id']}:\n"
                             f"{result.stdout}{result.stderr}")
    targets = ["permute-run", *mutant.get("tests", [])]
    result = subprocess.run(
        ["cmake", "--build", str(build), "--target", *targets],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise SystemExit(f"build failed for {mutant['id']}:\n"
                         f"{result.stdout}{result.stderr}")
    return build


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", help="comma-separated mutant ids")
    args = parser.parse_args()

    if not RUNNER.is_file():
        raise SystemExit("missing build/permute-run; run make build")

    mutants = tomllib.loads(MUTANTS.read_text()).get("mutant", [])
    if args.only:
        wanted = {item.strip().upper() for item in args.only.split(",")}
        mutants = [m for m in mutants if m["id"].upper() in wanted]

    guests = suite()
    baseline = {}
    for name, guest in guests:
        fields, records = run(RUNNER, guest, BASE_BUILD / "mutants" /
                              f"base-{name}.pmt")
        if records is None:
            raise SystemExit(f"baseline trace unusable for {name}: "
                             f"{fields.get('OUTCOME')}")
        baseline[name] = (fields.get("OUTCOME", "?"), records)

    killed = 0
    rows = []
    for mutant in mutants:
        build = configure(mutant)
        reason = None

        for name, guest in guests:
            trace = build / f"mutation-{name}.pmt"
            fields, records = run(build / "permute-run", guest, trace)
            outcome, base_records = baseline[name]
            if fields.get("OUTCOME") != outcome:
                reason = f"{name}: outcome {fields.get('OUTCOME')} != {outcome}"
                break
            if records is None:
                reason = f"{name}: no usable trace"
                break
            result = compare_records(base_records, records)
            if not result.equal:
                reason = f"{name}: {result.detail}"
                break

        if reason is None:
            for test in mutant.get("tests", []):
                binary = build / test
                if not binary.is_file():
                    continue
                result = subprocess.run([str(binary)], capture_output=True,
                                        text=True)
                if result.returncode != 0:
                    failing = [line for line in result.stdout.splitlines()
                               if line.startswith("FAIL")]
                    reason = f"{test}: {failing[0] if failing else 'failed'}"
                    break

        status = "killed" if reason else "SURVIVED"
        if reason:
            killed += 1
        rows.append((mutant["id"], mutant["summary"], status, reason or "-"))
        print(f"{mutant['id']} {mutant['summary']:52s} {status:8s} "
              f"{reason or ''}")

    total = len(mutants)
    print(f"mutation score: {killed}/{total} killed")
    (BASE_BUILD / "mutation-results.json").write_text(json.dumps(
        [{"id": i, "summary": s, "status": st, "reason": r}
         for i, s, st, r in rows], indent=2) + "\n")

    return 0 if killed >= 10 and total == 12 else 1


if __name__ == "__main__":
    sys.exit(main())
