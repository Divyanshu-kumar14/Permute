# SPDX-License-Identifier: Apache-2.0
"""Determinism self-check (FR-6.11).

`permute run --verify RATE` re-executes a deterministic sample of the seeds in
fresh processes and compares the BLAKE3-64 `run_digest` with the original
trace. A mismatch is reported with both digests and the run exits 5: the
harness cannot be trusted to explain a failure it cannot reproduce.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

from .trace import codec


def sample_seeds(seeds: list[int], rate: float) -> list[int]:
    if rate <= 0 or not seeds:
        return []
    count = max(1, round(rate * len(seeds)))
    picked = []
    for index in range(count):
        seed = seeds[(index * len(seeds)) // count]
        if seed not in picked:
            picked.append(seed)
    return picked


def _trace_digest(path: Path) -> str | None:
    parsed = codec.read(path)
    if not parsed.ok or parsed.run_digest is None:
        return None
    return f"{parsed.run_digest:016x}"


def verify(out_dir: Path, rate: float, runner: Path,
           guest: str | None, faults: list[str],
           timeout: float = 30.0) -> dict:
    traces = sorted(Path(out_dir).glob("traces/seed-*.pmt"))
    seeds = []
    original = {}
    for trace in traces:
        try:
            seed = int(trace.stem.split("-", 1)[1])
        except ValueError:
            continue
        digest = _trace_digest(trace)
        if digest is None:
            continue
        seeds.append(seed)
        original[seed] = digest

    picked = sample_seeds(sorted(seeds), rate)
    mismatches = []

    for seed in picked:
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / "verify.pmt"
            env = dict(os.environ)
            env.pop("PERMUTE_GUEST", None)
            env.pop("PERMUTE_FAULTS", None)
            if guest:
                env["PERMUTE_GUEST"] = guest
            if faults:
                env["PERMUTE_FAULTS"] = ",".join(faults)
            try:
                completed = subprocess.run(
                    [str(runner), str(seed), str(trace)], env=env,
                    capture_output=True, text=True, timeout=timeout)
            except subprocess.TimeoutExpired:
                mismatches.append({"seed": seed, "first": original[seed],
                                   "second": "timeout"})
                continue
            second = _trace_digest(trace) if trace.is_file() else None
            if second is None:
                mismatches.append({
                    "seed": seed, "first": original[seed],
                    "second": f"no trace (exit {completed.returncode})",
                })
            elif second != original[seed]:
                mismatches.append({"seed": seed, "first": original[seed],
                                   "second": second})

    return {
        "rate": rate,
        "sampled": len(picked),
        "checked": len(picked),
        "mismatches": mismatches,
    }
