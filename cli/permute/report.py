# SPDX-License-Identifier: Apache-2.0
"""Run reports: load, merge, summarise, write."""

from __future__ import annotations

import json
from pathlib import Path


def load(path: Path | str) -> dict:
    return json.loads(Path(path).read_text())


def write(path: Path | str, data: dict) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=False) + "\n")


def summarise(report: dict) -> dict:
    seeds = int(report.get("seeds", 0))
    counts = {
        "clean": int(report.get("clean", 0)),
        "crash": int(report.get("crash", 0)),
        "budget": int(report.get("budget", 0)),
        "violation": int(report.get("violation", 0)),
        "internal": int(report.get("internal", 0)),
    }
    findings = report.get("findings", [])

    if counts["crash"] or counts["violation"]:
        verdict = "findings"
    elif counts["internal"]:
        verdict = "internal"
    else:
        verdict = "clean"

    text = (
        f"{seeds} seeds: {counts['clean']} clean, {counts['crash']} crash, "
        f"{counts['budget']} budget, {counts['violation']} violation, "
        f"{counts['internal']} internal"
    )

    return {
        "seeds": seeds,
        **counts,
        "findings": findings,
        "verdict": verdict,
        "text": text,
    }
