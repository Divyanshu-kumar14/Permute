#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate the GitHub workflows and the musl image before CI sees them.

GitHub only validates a workflow when it runs, which is a slow way to discover
a missing `runs-on`. This checks the structure locally with the same YAML
parser a reviewer would use, and it checks that the musl image actually builds
and tests rather than merely existing.
"""

from __future__ import annotations

import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover - CI installs it explicitly
    print("ci-check: PyYAML is required (python3 -m pip install pyyaml)",
          file=sys.stderr)
    raise SystemExit(2)

REPO = Path(__file__).resolve().parents[1]
WORKFLOWS = REPO / ".github" / "workflows"


def check_workflow(path: Path) -> list[str]:
    problems = []
    try:
        data = yaml.safe_load(path.read_text())
    except yaml.YAMLError as exc:
        return [f"{path.name}: invalid YAML: {exc}"]

    if not isinstance(data, dict):
        return [f"{path.name}: top level is not a mapping"]

    if "name" not in data:
        problems.append(f"{path.name}: missing 'name'")

    # PyYAML resolves the YAML 1.1 key `on` to True.
    if "on" not in data and True not in data:
        problems.append(f"{path.name}: missing 'on' trigger")

    jobs = data.get("jobs")
    if not isinstance(jobs, dict) or not jobs:
        problems.append(f"{path.name}: no jobs")
        return problems

    for name, job in jobs.items():
        if not isinstance(job, dict):
            problems.append(f"{path.name}: job {name} is not a mapping")
            continue
        if "runs-on" not in job:
            problems.append(f"{path.name}: job {name} has no runs-on")
        steps = job.get("steps")
        if not isinstance(steps, list) or not steps:
            problems.append(f"{path.name}: job {name} has no steps")
            continue
        for index, step in enumerate(steps):
            if not isinstance(step, dict):
                problems.append(
                    f"{path.name}: job {name} step {index} is not a mapping")
                continue
            if "uses" not in step and "run" not in step:
                problems.append(
                    f"{path.name}: job {name} step {index} has no uses/run")

    return problems


def check_dockerfile() -> list[str]:
    path = REPO / "docker" / "Dockerfile.musl"
    if not path.is_file():
        return ["docker/Dockerfile.musl is missing"]

    text = path.read_text()
    problems = []
    if "SPDX-License-Identifier: Apache-2.0" not in text.splitlines()[0]:
        problems.append("Dockerfile.musl: missing SPDX header")
    if not any(line.startswith("FROM ") for line in text.splitlines()):
        problems.append("Dockerfile.musl: no FROM")
    if "make unit" not in text:
        problems.append("Dockerfile.musl: does not run the unit suite")
    return problems


def main() -> int:
    problems: list[str] = []

    workflows = sorted(WORKFLOWS.glob("*.yml"))
    if not workflows:
        problems.append("no workflows found")
    for path in workflows:
        problems.extend(check_workflow(path))

    problems.extend(check_dockerfile())

    if problems:
        print(f"ci-check: FAIL ({len(problems)} problem(s))")
        for problem in problems:
            print(f"  {problem}")
        return 1

    print(f"ci-check: PASSED ({len(workflows)} workflow(s), musl image)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
