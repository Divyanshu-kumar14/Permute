# SPDX-License-Identifier: Apache-2.0
"""Failure artifacts and the execution callback the CLI shrinker uses.

An artifact is the smallest set of inputs that can reproduce a finding: the
seed, the guest object, the fault plan, the stimulus table, and the invariant
that fired. `permute shrink` re-executes candidates through the same runner the
run used, so every reduction is a real execution, not an edit to a log.

The current runtime consumes the seed and the fault plan. Stimuli, a time
window, and a decision tape are recorded but not applied yet, so their stages
reduce them to empty; that is correct for this runner and is stated in the
artifact's limits section.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

from ..trace import codec
from ..oracle import evaluate
from .shrinker import ShrinkInput

ARTIFACT_FORMAT = "permute-artifact/1"

FAULT_RE = re.compile(
    r"^(send|recv|reply|pulse):(drop|duplicate|reorder|delay|timeout|kill)@"
)


def write_artifact(path: Path, *, seed: int, guest: str | None,
                   faults: list[str], stimuli: list[dict], invariant: dict,
                   finding: dict, trace: Path, digest: str | None,
                   window: tuple[int, int] | None = None,
                   decisions: list[dict] | None = None) -> None:
    payload = {
        "format": ARTIFACT_FORMAT,
        "seed": seed,
        "guest": guest,
        "faults": list(faults),
        "stimuli": list(stimuli),
        "decisions": list(decisions or []),
        "window": list(window) if window is not None else None,
        "invariant": dict(invariant),
        "finding": dict(finding),
        "trace": str(trace),
        "run_digest": digest,
        "minimality": "unshrunk",
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def load_artifact(path: Path) -> dict:
    data = json.loads(Path(path).read_text())
    if data.get("format") != ARTIFACT_FORMAT:
        raise ValueError(f"{path} is not a {ARTIFACT_FORMAT} artifact")
    for key in ("seed", "invariant", "finding", "faults"):
        if key not in data:
            raise ValueError(f"artifact is missing '{key}'")
    return data


def fault_touches(records, faults: list[str]) -> dict[str, list[int]]:
    """Map each fault rule to the FAULT_INJECT records it produced.

    A rule that never fired has no records and therefore no touch entry; the
    pre-slice keeps it, because absence of evidence is not evidence of
    irrelevance.
    """
    touches: dict[str, list[int]] = {f"fault:{rule}": [] for rule in faults}
    by_action = {}
    for rule in faults:
        match = FAULT_RE.match(rule)
        if match:
            by_action.setdefault((match.group(1), match.group(2)), []).append(rule)

    for record in records:
        if record["kind"] != "FAULT_INJECT":
            continue
        site = record.get("obj") or 0
        action = record.get("aux") or 0
        # The bridge numbers sites and actions in the same order as the
        # parser's tables.
        names = (
            ("send", "recv", "reply", "pulse"),
            ("drop", "duplicate", "reorder", "delay", "timeout", "kill"),
        )
        if site >= len(names[0]) or action >= len(names[1]):
            continue
        for rule in by_action.get((names[0][site], names[1][action]), []):
            touches[f"fault:{rule}"].append(record["seq"])
    return touches


def make_predicate(artifact: dict, runner: Path, timeout: float = 30.0):
    """Return `test(candidate) -> bool`: does the same invariant still fire?"""
    invariant = artifact["invariant"]
    wanted = artifact["finding"]["kind"]
    guest = artifact.get("guest")

    def test(candidate: ShrinkInput) -> bool:
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / "shrink.pmt"
            env = dict(os.environ)
            env.pop("PERMUTE_GUEST", None)
            env.pop("PERMUTE_FAULTS", None)
            if guest:
                env["PERMUTE_GUEST"] = guest
            if candidate.faults:
                env["PERMUTE_FAULTS"] = ",".join(candidate.faults)

            try:
                subprocess.run(
                    [str(runner), str(candidate.seed), str(trace)],
                    env=env, capture_output=True, text=True, timeout=timeout,
                )
            except subprocess.TimeoutExpired:
                return False

            if not trace.is_file():
                return False
            # Scratch trace: it was written moments ago by our own runner and
            # never leaves this temporary directory. Integrity verification is
            # the dominant cost of a non-reproducing candidate (90k records,
            # ~3.5 s), so the predicate decodes without it.
            parsed = codec.read(trace, verify=False)
            if not parsed.ok:
                return False
            result = evaluate(parsed.records, [invariant],
                              seed=candidate.seed)
            return any(finding.kind == wanted for finding in result.findings)

    return test
