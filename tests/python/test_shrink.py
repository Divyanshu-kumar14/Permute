# SPDX-License-Identifier: Apache-2.0
"""P2-03: the shrinker's gate and its budget.

The headline fixtures are deliberately large: 10,000 inputs must reduce to the
handful the predicate actually needs, and a tape must shrink by more than an
order of magnitude. The budget tests assert the *exact* bound, not an
approximation: 1,500 means at most 1,500 executions.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from cli.permute.shrink import Budget, ShrinkInput, Shrinker, causal_cone  # noqa: E402
from cli.permute.shrink.hdd import BudgetExhausted, ddmin  # noqa: E402
from cli.permute.shrink.decisions import Decision  # noqa: E402
from cli.permute.shrink.runner import write_artifact  # noqa: E402

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def rec(kind, seq, src=1, obj=0, blob=None):
    return {"kind": kind, "seq": seq, "src": src, "obj": obj, "blob": blob}


# ---------------------------------------------------------------------------
# Algorithm


def test_ten_thousand_to_three():
    count = 10000
    needed = {10, 5000, 9999}
    stimuli = [{"id": i, "at_ns": i * 1000} for i in range(count)]
    inp = ShrinkInput(
        seed=1,
        stimuli=stimuli,
        touches={f"stimulus:{i}": [i] for i in range(count)},
    )

    def test(candidate):
        return needed <= {item["id"] for item in candidate.stimuli}

    budget = Budget()
    result = Shrinker(test, budget, cone=set(needed)).shrink(inp)

    check(result.input.size() <= 20,
          f"10k: reduced to {result.input.size()} inputs (want <= 20)")
    check(result.original_size // max(1, result.input.size()) >= 100,
          f"10k: reduction {result.original_size}->{result.input.size()} "
          "is under 100x")
    check({item["id"] for item in result.input.stimuli} == needed,
          "10k: the three required stimuli survive")
    check(result.minimality == "proven", "10k: minimality is proven")
    check(result.executions < 50,
          f"10k: used {result.executions} executions (want < 50)")
    first = Shrinker(test, Budget(), cone=set(needed)).shrink(inp)
    check(first.to_json() == result.to_json(), "10k: repeat is byte-identical")


def test_budget_is_strict():
    inp = ShrinkInput(stimuli=[{"id": i} for i in range(10000)])

    def test(candidate):
        return bool(candidate.stimuli)

    budget = Budget(max_executions=10)
    result = Shrinker(test, budget).shrink(inp)

    check(result.minimality == "budget_limited",
          "budget: exhaustion reports budget_limited")
    check(result.executions == 10,
          f"budget: used exactly 10 executions, got {result.executions}")
    check(budget.executions == 10, "budget: the budget object agrees")


def test_wall_budget_is_strict():
    inp = ShrinkInput(stimuli=[{"id": i} for i in range(50)])

    def test(candidate):
        time.sleep(0.01)
        return bool(candidate.stimuli)

    budget = Budget(max_seconds=0.05)
    result = Shrinker(test, budget).shrink(inp)

    check(result.minimality == "budget_limited",
          "wall budget: exhaustion reports budget_limited")
    check(result.executions < 10,
          f"wall budget: stopped early, used {result.executions}")
    check(budget.elapsed() >= 0.05, "wall budget: the bound was reached")


def test_window_minimisation():
    stimuli = [{"id": "keep", "at_ns": 4500}, {"id": "late", "at_ns": 8000}]
    inp = ShrinkInput(stimuli=stimuli, window=(0, 10000))

    def test(candidate):
        lo, hi = candidate.window
        return any(item["id"] == "keep" and lo <= item["at_ns"] < hi
                   for item in candidate.stimuli)

    result = Shrinker(test, Budget()).shrink(inp)
    check(result.input.window == (4500, 4501),
          f"window: got {result.input.window}, want (4500, 4501)")
    check(result.minimality == "proven", "window: minimality is proven")


def test_decision_tape():
    needed = {Decision(0, i, i) for i in (3, 17, 42, 99, 101, 150, 175, 199)}
    decisions = [Decision(0, i, i) for i in range(200)]
    inp = ShrinkInput(decisions=decisions)

    def test(candidate):
        return needed <= set(candidate.decisions)

    result = Shrinker(test, Budget()).shrink(inp)
    check(len(result.input.decisions) <= 20,
          f"tape: reduced to {len(result.input.decisions)} overrides")
    check(result.original_size // max(1, result.input.size()) >= 10,
          f"tape: reduction {result.original_size}->{result.input.size()} "
          "is under 10x")
    check(set(result.input.decisions) == needed,
          "tape: exactly the required overrides survive")
    check(result.minimality == "proven", "tape: minimality is proven")


def test_fault_plan():
    keep = "send:drop@1"
    faults = [f"send:drop@1" if i == 7 else f"recv:timeout@0.01"
              for i in range(20)]
    inp = ShrinkInput(faults=faults)

    def test(candidate):
        return keep in candidate.faults

    result = Shrinker(test, Budget()).shrink(inp)
    check(result.input.faults == [keep],
          f"faults: got {result.input.faults}")
    check(result.minimality == "proven", "faults: minimality is proven")


def test_causal_slice():
    records = [
        rec("THREAD_START", 1, src=1),
        rec("SEND", 2, src=1, blob=b"A"),
        rec("RECV", 3, src=2, obj=7, blob=b"A"),
        rec("REPLY", 4, src=2, obj=7),
        rec("SEND", 5, src=1, blob=b"B"),
        rec("RECV", 6, src=2, obj=8, blob=b"B"),
    ]
    cone = causal_cone(records, 4)
    check({1, 2, 3, 4} <= cone, f"cone: missing ancestors, got {sorted(cone)}")
    check(5 not in cone and 6 not in cone,
          f"cone: unrelated records included, got {sorted(cone)}")


def test_ddmin_shrinks_and_pull_is_minimal():
    items = list(range(500))
    needed = {4, 250, 499}

    def test(candidate):
        return needed <= set(candidate)

    reduced = ddmin(items, test)
    check(set(reduced) == needed,
          f"ddmin: got {sorted(reduced)[:5]}...")
    check(reduced, "ddmin: non-empty")

    # pull removes one at a time and stops at a fixed point.
    stable = ddmin(reduced, test)
    check(stable == reduced, "ddmin: fixed point is stable")


# ---------------------------------------------------------------------------
# Result shape


def test_result_is_json_stable():
    inp = ShrinkInput(stimuli=[{"id": i} for i in range(100)])

    def test(candidate):
        return any(item["id"] == 50 for item in candidate.stimuli)

    result = Shrinker(test, Budget()).shrink(inp)
    payload = json.loads(result.to_json())
    check(payload["input"]["stimuli"] == [{"id": 50}],
          f"json: got {payload['input']['stimuli']}")
    check("elapsed" not in result.to_json(),
          "json: wall clock is not serialised")
    check(payload["minimality"] == "proven", "json: minimality present")
    check("stages" in payload and payload["stages"], "json: stages present")


def test_cli_artifact(tmp: Path):
    guest = REPO / "build" / "scenarios" / "proc_s01_hello__0.so"
    if not guest.is_file():
        raise SystemExit("missing scenarios; run make build")

    artifact = tmp / "artifact.json"
    write_artifact(
        artifact, seed=1, guest=str(guest),
        faults=["pulse:drop@0", "recv:timeout@0", "send:drop@1"],
        stimuli=[], invariant={"id": "dl", "kind": "deadlock",
                               "severity": "error"},
        finding={"invariant": "dl", "kind": "deadlock", "severity": "error",
                 "seq": 0, "t_ns": 0, "detail": "seed"},
        trace=tmp / "missing.pmt", digest=None,
    )

    permute = REPO / "tools" / "permute"
    first = subprocess.run([str(permute), "shrink", str(artifact), "--json"],
                           capture_output=True, text=True, timeout=300)
    check(first.returncode == 0,
          f"cli: shrink exit {first.returncode}: {first.stderr}")
    payload = json.loads(first.stdout)
    check(payload["input"]["faults"] == ["send:drop@1"],
          f"cli: reduced faults {payload['input']['faults']}")
    check(payload["original_size"] == 3 and payload["final_size"] == 1,
          "cli: 3 -> 1")
    check(payload["minimality"] == "proven", "cli: proven")

    written = artifact.with_suffix(".shrunk.json").read_text()
    second = subprocess.run([str(permute), "shrink", str(artifact), "--json"],
                            capture_output=True, text=True, timeout=300)
    check(second.stdout.strip() == first.stdout.strip(),
          "cli: repeat output is byte-identical")
    check(artifact.with_suffix(".shrunk.json").read_text() == written,
          "cli: artifact is byte-identical")


def test_stale_artifact(tmp: Path):
    guest = REPO / "build" / "scenarios" / "proc_s01_hello__0.so"
    artifact = tmp / "stale.json"
    write_artifact(
        artifact, seed=1, guest=str(guest), faults=[],
        stimuli=[], invariant={"id": "dl", "kind": "deadlock",
                               "severity": "error"},
        finding={"invariant": "dl", "kind": "deadlock", "severity": "error",
                 "seq": 0, "t_ns": 0, "detail": "seed"},
        trace=tmp / "missing.pmt", digest=None,
    )
    permute = REPO / "tools" / "permute"
    result = subprocess.run([str(permute), "shrink", str(artifact)],
                            capture_output=True, text=True, timeout=300)
    check(result.returncode == 4,
          f"stale: exit {result.returncode} (want 4)")
    check("does not reproduce" in result.stderr,
          f"stale: stderr {result.stderr.strip()!r}")


def main() -> int:
    test_causal_slice()
    test_ten_thousand_to_three()
    test_budget_is_strict()
    test_wall_budget_is_strict()
    test_window_minimisation()
    test_decision_tape()
    test_fault_plan()
    test_ddmin_shrinks_and_pull_is_minimal()
    test_result_is_json_stable()

    with tempfile.TemporaryDirectory() as tmp:
        test_cli_artifact(Path(tmp))
        test_stale_artifact(Path(tmp))

    print(f"test_shrink: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
