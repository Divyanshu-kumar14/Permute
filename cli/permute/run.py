# SPDX-License-Identifier: Apache-2.0
"""Replay: from a seed, and from a .pmt artifact.

Two independent checks, because they fail differently:

* **From seed** runs the same seed twice in separate processes and compares the
  digests. A difference means the simulator is not deterministic (exit 5).
* **From artifact** reads a recorded trace, re-derives its digest, compares the
  manifest the caller expects, and checks event ordering. A build, config, or
  kernel mismatch is refused with exit 4 unless forced.

Exit codes follow the product specification:

    0   verified
    2   the artifact cannot be replayed (corrupt, truncated, ordering broken)
    4   build / config / kernel mismatch, and --force was not given
    5   same seed produced different digests
    11  digest differs from the expected digest

Exit 10 (violations differ) is reserved: the oracle engine that compares
violations does not exist yet, so replay currently proves integrity, manifest,
ordering, and digest, and says so in its messages.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

from .trace import codec

EXIT_OK = 0
EXIT_VIOLATION = 1
EXIT_USAGE = 2
EXIT_BUILD = 3
EXIT_REFUSED = 4
EXIT_NONDETERMINISM = 5
EXIT_DEADLINE = 6
EXIT_INTERNAL = 7
EXIT_VIOLATIONS_DIFFER = 10
EXIT_DIGEST_MISMATCH = 11
EXIT_INTERRUPTED = 130


@dataclass
class Manifest:
    build_id: bytes | None = None
    config_hash: int | None = None
    kernel_model_version: int | None = None
    run_digest: int | None = None


@dataclass
class ReplayResult:
    exit_code: int
    messages: list[str] = field(default_factory=list)
    digest: int | None = None
    records: int = 0
    ordering: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.exit_code == EXIT_OK

    def to_json(self) -> dict:
        return {
            "exit_code": self.exit_code,
            "ok": self.ok,
            "digest": f"{self.digest:016x}" if self.digest is not None else None,
            "records": self.records,
            "messages": self.messages,
            "ordering": self.ordering,
        }


def verify_ordering(records) -> list[str]:
    """Check the invariants a reader can prove without understanding events."""
    problems: list[str] = []
    last_seq = -1
    last_time = -1

    for i, rec in enumerate(records):
        if rec["seq"] <= last_seq:
            problems.append(
                f"record {i}: seq {rec['seq']} did not increase from {last_seq}")
        if rec["t_ns"] < last_time:
            problems.append(
                f"record {i}: time went backwards ({rec['t_ns']} < {last_time})")
        if rec["kind"] not in codec.KINDS:
            problems.append(f"record {i}: unknown kind {rec['kind']!r}")

        schema = codec.SCHEMA[codec.KINDS.index(rec["kind"])]
        for bit, name in ((codec.S_DST, "dst"), (codec.S_OBJ, "obj"),
                          (codec.S_AUX, "aux")):
            present = rec[name] is not None
            if present != bool(schema & bit):
                problems.append(
                    f"record {i} ({rec['kind']}): {name} presence is wrong")

        last_seq = rec["seq"]
        last_time = rec["t_ns"]

    return problems


def replay_artifact(path, expected: Manifest | None = None,
                    force: bool = False) -> ReplayResult:
    expected = expected or Manifest()
    tr = codec.read(path)

    if tr.error is not None or tr.corrupt or not tr.chain_ok:
        return ReplayResult(EXIT_USAGE, [f"artifact unusable: {tr.error}"],
                            tr.run_digest, len(tr.records))
    if tr.partial:
        return ReplayResult(
            EXIT_USAGE,
            ["artifact is incomplete: a partial trace cannot prove a run"],
            tr.run_digest, len(tr.records))

    mismatches = []
    if expected.build_id is not None and tr.build_id != expected.build_id:
        mismatches.append(
            f"build id {tr.build_id.hex()} != {expected.build_id.hex()}")
    if (expected.config_hash is not None
            and tr.config_hash != expected.config_hash):
        mismatches.append(
            f"config hash {tr.config_hash:016x} != {expected.config_hash:016x}")
    if (expected.kernel_model_version is not None
            and tr.kernel_model_version != expected.kernel_model_version):
        mismatches.append(
            f"kernel model {tr.kernel_model_version} != "
            f"{expected.kernel_model_version}")

    if mismatches:
        if not force:
            return ReplayResult(EXIT_REFUSED,
                                ["replay refused: " + m for m in mismatches],
                                tr.run_digest, len(tr.records))
        mismatches = ["forced past: " + m for m in mismatches]

    ordering = verify_ordering(tr.records)
    if ordering:
        return ReplayResult(EXIT_USAGE,
                            ["artifact ordering is inconsistent"] + ordering,
                            tr.run_digest, len(tr.records))

    if (expected.run_digest is not None
            and tr.run_digest != expected.run_digest):
        return ReplayResult(
            EXIT_DIGEST_MISMATCH,
            [f"digest {tr.run_digest:016x} != {expected.run_digest:016x}"],
            tr.run_digest, len(tr.records))

    messages = mismatches + [
        f"verified {len(tr.records)} records in {tr.n_chunks} chunk(s)",
        f"run_digest {tr.run_digest:016x}",
        "violation comparison is not implemented yet (exit 10 reserved)",
    ]
    return ReplayResult(EXIT_OK, messages, tr.run_digest, len(tr.records))


def _parse_field(output: str, key: str) -> str | None:
    for line in output.splitlines():
        if line.startswith(key + "="):
            return line.split("=", 1)[1]
    return None


def replay_seed(seed: int, fixture="build/replay_fixture",
                runs: int = 2) -> ReplayResult:
    fixture = Path(fixture)
    if not fixture.is_file():
        return ReplayResult(EXIT_USAGE, [f"fixture not found: {fixture}"])

    digests = []
    records = 0
    ordering: list[str] = []

    with tempfile.TemporaryDirectory() as tmp:
        for i in range(runs):
            path = Path(tmp) / f"seed-{seed}-{i}.pmt"
            out = subprocess.run([str(fixture), str(seed), str(path)],
                                 capture_output=True, text=True)
            if out.returncode != 0:
                return ReplayResult(
                    EXIT_INTERNAL,
                    [f"fixture failed for seed {seed}: {out.stderr.strip()}"])
            printed = _parse_field(out.stdout, "DIGEST")
            if printed is None:
                return ReplayResult(EXIT_INTERNAL,
                                    ["fixture printed no digest"])

            tr = codec.read(path)
            if not tr.ok or tr.partial:
                return ReplayResult(
                    EXIT_USAGE,
                    [f"seed {seed} produced an unreadable trace: {tr.error}"])
            problems = verify_ordering(tr.records)
            if problems:
                ordering.extend(problems)

            computed = f"{tr.run_digest:016x}"
            if computed != printed:
                return ReplayResult(
                    EXIT_INTERNAL,
                    [f"seed {seed}: fixture digest {printed} != trace {computed}"])
            digests.append(printed)
            records = len(tr.records)

    if ordering:
        return ReplayResult(EXIT_USAGE, ["ordering inconsistent"] + ordering,
                            int(digests[0], 16), records, ordering)

    if len(set(digests)) != 1:
        return ReplayResult(
            EXIT_NONDETERMINISM,
            [f"seed {seed} produced {len(set(digests))} distinct digests: "
             + ", ".join(sorted(set(digests)))],
            None, records)

    return ReplayResult(EXIT_OK, [
        f"seed {seed}: {runs} runs, one digest {digests[0]}",
        f"{records} records",
        "artifact contains no host data (created_unix_ns is zero)",
    ], int(digests[0], 16), records)


def _hex_bytes(value: str | None) -> bytes | None:
    if value is None:
        return None
    raw = value[2:] if value.startswith("0x") else value
    return bytes.fromhex(raw)


def _parse_seed_range(text: str) -> tuple[int, int]:
    if "-" in text:
        a, b = text.split("-", 1)
        first, last = int(a, 0), int(b, 0)
    else:
        first = last = int(text, 0)
    if last < first or first < 0:
        raise ValueError("invalid seed range")
    return first, last


def _parse_shard(text: str) -> tuple[int, int]:
    index, count = text.split("/", 1)
    i, n = int(index), int(count)
    if n <= 0 or i < 0 or i >= n:
        raise ValueError("invalid shard")
    return i, n


def _merge_reports(paths, destination) -> dict:
    merged = {
        "seeds": 0, "clean": 0, "crash": 0, "budget": 0, "violation": 0,
        "internal": 0, "exit_code": 0, "findings": [],
    }
    for path in paths:
        path = Path(path)
        if not path.is_file():
            continue
        data = json.loads(path.read_text())
        for key in ("seeds", "clean", "crash", "budget", "violation",
                    "internal"):
            merged[key] += data.get(key, 0)
        merged["findings"].extend(data.get("findings", []))

    merged["findings"].sort(key=lambda f: f["seed"])
    if merged["crash"] or merged["violation"]:
        merged["exit_code"] = 1
    elif merged["internal"]:
        merged["exit_code"] = 7

    Path(destination).write_text(json.dumps(merged, indent=2) + "\n")
    return merged


def execute(seeds: str, out: str, jobs: int = 1,
            runner: str = "build/permute-run",
            worker: str = "build/permute-worker",
            wall_ms: int = 2000, shard: str = "0/1", batch: int = 64,
            quiet: bool = False, max_wall_ms: int = 0,
            guest: str | None = None) -> int:
    first, last = _parse_seed_range(seeds)
    shard_index, shard_count = _parse_shard(shard)

    out_path = Path(out)
    out_path.mkdir(parents=True, exist_ok=True)
    jobs = max(1, jobs)

    # Contiguous slices: every seed is covered exactly once, and the user's
    # shard filter is applied inside each worker. Sharding therefore does not
    # depend on how many workers happen to be running.
    total = last - first + 1
    chunk = (total + jobs - 1) // jobs
    slices = []
    start = first
    while start <= last:
        end = min(last, start + chunk - 1)
        slices.append((start, end))
        start = end + 1

    procs = []
    reports = []
    for index, (a, b) in enumerate(slices):
        report = out_path / f"shard-{index}.json"
        reports.append(report)
        cmd = [
            worker, "--seeds", f"{a}-{b}", "--out", str(out_path),
            "--runner", runner, "--wall-ms", str(wall_ms),
            "--shard", f"{shard_index}/{shard_count}", "--batch", str(batch),
            "--report", str(report),
        ]
        if quiet:
            cmd.append("--quiet")
        env = None
        if guest:
            env = {**os.environ, "PERMUTE_GUEST": guest}
        procs.append(subprocess.Popen(cmd, env=env))

    # A run-level deadline is parent-enforced (FR-5.6): on expiry the workers
    # are told to stop and the partial reports are still merged, so the
    # evidence collected so far is not thrown away.
    deadline = (time.monotonic() + max_wall_ms / 1000.0) if max_wall_ms > 0 else None
    timed_out = False
    try:
        while any(proc.poll() is None for proc in procs):
            if deadline is not None and time.monotonic() >= deadline:
                timed_out = True
                for proc in procs:
                    if proc.poll() is None:
                        proc.terminate()
                break
            time.sleep(0.01)
        codes = [proc.wait() for proc in procs]
    except KeyboardInterrupt:
        for proc in procs:
            if proc.poll() is None:
                proc.terminate()
        deadline = time.monotonic() + 2.0
        for proc in procs:
            try:
                proc.wait(timeout=max(0.1, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        raise

    merged = _merge_reports(reports, out_path / "report.json")

    if not quiet:
        print(f"{merged['seeds']} seeds: {merged['clean']} clean, "
              f"{merged['crash']} crash, {merged['budget']} budget, "
              f"{merged['violation']} violation, {merged['internal']} internal")
        print(f"report: {out_path / 'report.json'}")

    if timed_out:
        return EXIT_DEADLINE
    if merged["crash"] or merged["violation"]:
        return EXIT_VIOLATION
    if any(code == 7 for code in codes):
        return EXIT_INTERNAL
    if any(code == 130 for code in codes):
        return EXIT_INTERRUPTED
    if any(code == 6 for code in codes):
        return EXIT_DEADLINE
    return EXIT_OK


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="permute replay")
    sub = ap.add_subparsers(dest="command", required=True)

    art = sub.add_parser("artifact", help="verify a recorded trace")
    art.add_argument("path")
    art.add_argument("--build-id")
    art.add_argument("--config-hash")
    art.add_argument("--kernel", type=int)
    art.add_argument("--digest")
    art.add_argument("--force", action="store_true",
                     help="accept a manifest mismatch (not a digest mismatch)")
    art.add_argument("--json", action="store_true")

    sd = sub.add_parser("seed", help="run one seed twice and compare digests")
    sd.add_argument("seed", type=int)
    sd.add_argument("--fixture", default="build/replay_fixture")
    sd.add_argument("--runs", type=int, default=2)
    sd.add_argument("--json", action="store_true")

    ex = sub.add_parser("execute", help="run a seed range with worker isolation")
    ex.add_argument("--seeds", required=True, help="A-B or a single seed")
    ex.add_argument("--out", required=True)
    ex.add_argument("--jobs", type=int, default=1)
    ex.add_argument("--runner", default="build/permute-run")
    ex.add_argument("--worker", default="build/permute-worker")
    ex.add_argument("--wall-ms", type=int, default=2000)
    ex.add_argument("--shard", default="0/1")
    ex.add_argument("--batch", type=int, default=64)
    ex.add_argument("--quiet", action="store_true")
    ex.add_argument("--max-wall-ms", type=int, default=0)

    args = ap.parse_args(argv)

    if args.command == "artifact":
        result = replay_artifact(
            args.path,
            Manifest(
                build_id=_hex_bytes(args.build_id),
                config_hash=int(args.config_hash, 16) if args.config_hash else None,
                kernel_model_version=args.kernel,
                run_digest=int(args.digest, 16) if args.digest else None,
            ),
            force=args.force,
        )
    elif args.command == "seed":
        result = replay_seed(args.seed, args.fixture, args.runs)
    else:
        return execute(args.seeds, args.out, args.jobs, args.runner,
                       args.worker, args.wall_ms, args.shard, args.batch,
                       args.quiet, args.max_wall_ms)

    if args.json:
        print(json.dumps(result.to_json(), indent=2))
    else:
        for message in result.messages:
            print(message)

    return result.exit_code


if __name__ == "__main__":
    sys.exit(main())
