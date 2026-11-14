# SPDX-License-Identifier: Apache-2.0
"""P2-07: exploration, mutation determinism, shards and livelock."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from cli.permute.explore import OPERATORS, Tape, apply, explore  # noqa: E402
from cli.permute.explore.bitmap import NoveltyBitmap, features  # noqa: E402
from cli.permute.trace import codec  # noqa: E402

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def rec(kind, seq, src=1, obj=0, aux=0, blob=None):
    return {"kind": kind, "seq": seq, "src": src, "dst": 0, "obj": obj,
            "aux": aux, "blob": blob, "t_ns": seq * 1000}


def synthetic(seed: int):
    """A tiny trace whose feature set depends on the seed."""
    records = [
        rec("THREAD_START", 1, src=1, obj=10),
        rec("THREAD_START", 2, src=2, obj=10),
        rec("SCHED_PICK", 3, src=1, obj=10),
        rec("SEND", 4, src=1, obj=0, blob=(seed & 0xFFFFFFFF).to_bytes(4, "little")),
        rec("RECV", 5, src=2, obj=1 + seed, blob=(seed & 0xFFFFFFFF).to_bytes(4, "little")),
        rec("REPLY", 6, src=2, obj=1 + seed),
        rec("FAULT_INJECT", 7, src=1, obj=seed % 4, aux=seed % 6),
    ]
    return records


def test_bitmap():
    bitmap = NoveltyBitmap()
    first = bitmap.add(features(synthetic(1)))
    check(first > 0, "bitmap: features are counted")
    check(bitmap.coverage == first, "bitmap: coverage equals the first add")
    second = bitmap.add(features(synthetic(1)))
    check(second == 0, "bitmap: the same trace adds nothing")
    third = bitmap.add(features(synthetic(2)))
    check(third > 0, "bitmap: a different trace adds features")


def test_mutations_deterministic():
    corpus = [Tape(seed=7, faults=["send:drop@0.50"],
                   stimuli=[{"at_ns": 10, "kind": "pulse"}])]
    seen = set()
    for index in range(6):
        a, name = apply(index, corpus[0], corpus)
        b, name_b = apply(index, corpus[0], corpus)
        check(name == OPERATORS[index], f"mutation: operator {index} {name}")
        check(a.to_dict() == b.to_dict(),
              f"mutation: {name} is deterministic")
        seen.add(name)
    check(len(seen) == 6, "mutation: six distinct operators")


def test_engine_policies():
    def fake(tape, budget):
        return "CLEAN", f"{tape.seed:016x}", synthetic(tape.seed)

    for policy in ("random", "novelty"):
        result = explore(policy, 12, Tape(seed=1), runner=fake)
        check(len(result.entries) == 12, f"{policy}: 12 entries")
        check(result.coverage > 0, f"{policy}: coverage counted")
        check(result.corpus_size >= 1, f"{policy}: corpus exists")
        check(result.findings == 0, f"{policy}: clean outcomes are not findings")


def test_escalation():
    calls = {}

    def fake(tape, budget):
        key = (tape.seed, budget)
        calls[key] = calls.get(key, 0) + 1
        records = synthetic(tape.seed)
        if budget <= 2.0 and calls[key] == 1:
            return "BUDGET", None, records
        return "CLEAN", "0" * 16, records

    result = explore("novelty", 2, Tape(seed=1), runner=fake, timeout=2.0)
    check(result.escalations == 1, "escalation: one retry at 4x")
    check(result.entries[0].escalated, "escalation: entry is marked")

    def always_budget(tape, budget):
        return "BUDGET", None, synthetic(tape.seed)

    result = explore("random", 8, Tape(seed=1), runner=always_budget,
                     timeout=2.0)
    check(result.escalations == 3, "escalation: capped at three")


def test_engine_shard_equals_full():
    def fake(tape, budget):
        return "CLEAN", f"{tape.seed:016x}", synthetic(tape.seed)

    full = explore("novelty", 12, Tape(seed=1), shard="0/1", runner=fake)
    shard = explore("novelty", 12, Tape(seed=1), shard="2/4", runner=fake)
    expected = [entry for entry in full.entries if entry.index % 4 == 2]
    check([e.as_dict() for e in shard.entries] ==
          [e.as_dict() for e in expected],
          "shard: 2/4 alone equals shard 2 of the full run")
    check(len(shard.entries) == 3, "shard: three iterations recorded")


def _cli(args, cwd, env=None):
    merged = {**__import__("os").environ, **(env or {})}
    return subprocess.run([str(REPO / "tools" / "permute"), *args], cwd=cwd,
                          env=merged, capture_output=True, text=True,
                          timeout=300)


def test_cli_shard_and_livelock(tmp: Path):
    guest = REPO / "build" / "scenarios" / "proc_s01_hello__0.so"
    if not guest.is_file():
        raise SystemExit("missing scenarios; run make build")

    def project(name):
        path = tmp / name
        result = _cli(["init", str(path)], cwd=tmp)
        if result.returncode != 0:
            raise SystemExit(result.stderr)
        return path

    shard_dir = project("shard")
    result = _cli(["run", "--seeds", "0-7", "--shard", "2/4",
                   "--guest", str(guest)], cwd=shard_dir)
    check(result.returncode == 0, f"shard cli: {result.stderr}")

    full_dir = project("full")
    result = _cli(["run", "--seeds", "0-7", "--guest", str(guest)],
                  cwd=full_dir)
    check(result.returncode == 0, f"full cli: {result.stderr}")

    shard_traces = sorted((shard_dir / ".permute/run/traces").glob("*.pmt"))
    check(len(shard_traces) == 2, f"shard cli: {len(shard_traces)} traces")
    for trace in shard_traces:
        other = full_dir / ".permute/run/traces" / trace.name
        check(other.is_file() and trace.read_bytes() == other.read_bytes(),
              f"shard cli: {trace.name} is byte-identical")

    spin_dir = project("spin")
    result = _cli(["run", "--seeds", "0-2", "--wall-ms", "150",
                   "--guest", str(guest)], cwd=spin_dir,
                  env={"PERMUTE_SPIN_SEED": "0"})
    check(result.returncode == 1, f"livelock: exit {result.returncode}")
    check("LIVELOCK_SUSPECT" in result.stdout,
          f"livelock: output {result.stdout.strip()!r}")


def main() -> int:
    test_bitmap()
    test_mutations_deterministic()
    test_engine_policies()
    test_escalation()
    test_engine_shard_equals_full()
    with tempfile.TemporaryDirectory() as tmp:
        test_cli_shard_and_livelock(Path(tmp))
    print(f"test_explore: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
