# SPDX-License-Identifier: Apache-2.0
"""P2-09: the four report formats and their schemas."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from cli.permute import report  # noqa: E402

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def make_run(out: Path, merged: dict, summary: dict,
             project: str = "demo") -> list[Path]:
    out.mkdir(parents=True, exist_ok=True)
    (out / "traces").mkdir(exist_ok=True)
    for seed in (0, 1, 2):
        (out / "traces" / f"seed-{seed}.pmt").write_bytes(b"")
    (out / "report.json").write_text(json.dumps(merged) + "\n")
    (out / "summary.json").write_text(json.dumps(summary) + "\n")
    return report.write_all(out, merged, summary, project)


def test_all_formats(tmp: Path):
    merged = {
        "seeds": 3, "clean": 1, "crash": 1, "budget": 1, "violation": 0,
        "internal": 0,
        "findings": [{"seed": 1, "outcome": "CRASH", "signal": 11},
                     {"seed": 0, "outcome": "BUDGET", "signal": 0}],
        "violations": [{"seed": 2, "kind": "deadlock", "invariant": "dl",
                        "severity": "error", "detail": "T1 -> T2 -> T1"}],
        "verify": {"rate": 0.5, "sampled": 1, "checked": 1, "mismatches": []},
    }
    summary = report.summarise(merged)
    summary["verdict"] = "findings"
    paths = make_run(tmp / "run", merged, summary)
    names = {path.name for path in paths}
    check(names == {"report.json", "junit.xml", "sarif.json", "index.html"},
          f"formats: {sorted(names)}")

    problems = report.validate_junit(tmp / "run" / "junit.xml")
    check(not problems, f"junit: {problems}")
    root = ET.parse(tmp / "run" / "junit.xml").getroot()
    check(root.attrib["tests"] == "3", "junit: three test cases")
    check(root.attrib["failures"] == "2", "junit: crash + violation fail")
    check(root.attrib["skipped"] == "1", "junit: budget is skipped")

    problems = report.validate_sarif(tmp / "run" / "sarif.json")
    check(not problems, f"sarif schema: {problems[:3]}")
    sarif = json.loads((tmp / "run" / "sarif.json").read_text())
    check(sarif["version"] == "2.1.0", "sarif: version")
    levels = {result["ruleId"]: result["level"]
              for result in sarif["runs"][0]["results"]}
    check(levels.get("seed-crash") == "error", "sarif: crash is an error")
    check(levels.get("oracle-deadlock") == "error", "sarif: oracle error")

    page = (tmp / "run" / "index.html").read_text()
    check("<style>" in page, "html: inline stylesheet")
    for bad in ("<link", "<script", "@import", "http://", "https://", "url("):
        check(bad not in page, f"html: no {bad}")


def test_warn_and_verify(tmp: Path):
    merged = {
        "seeds": 2, "clean": 2, "crash": 0, "budget": 0, "violation": 0,
        "internal": 0, "findings": [],
        "violations": [{"seed": 0, "kind": "starvation", "invariant": "st",
                        "severity": "warn", "detail": "slow"}],
        "verify": {"rate": 0.5, "sampled": 1, "checked": 1,
                   "mismatches": [{"seed": 1, "first": "aa", "second": "bb"}]},
    }
    summary = report.summarise(merged)
    summary["verdict"] = "nondeterminism"
    make_run(tmp / "run", merged, summary)
    root = ET.parse(tmp / "run" / "junit.xml").getroot()
    check(root.attrib["failures"] == "1",
          "verify: a digest mismatch is a JUnit failure")
    kinds = [case.find("failure").attrib.get("type")
             for case in root.findall("./testsuite/testcase")
             if case.find("failure") is not None]
    check("nondeterminism" in kinds, f"verify: failure type {kinds}")
    sarif = json.loads((tmp / "run" / "sarif.json").read_text())
    ids = [result["ruleId"] for result in sarif["runs"][0]["results"]]
    check("nondeterminism" in ids, f"verify: sarif {ids}")


def test_escaping(tmp: Path):
    merged = {"seeds": 0, "clean": 0, "crash": 0, "budget": 0,
              "violation": 0, "internal": 0, "findings": [],
              "violations": []}
    summary = report.summarise(merged)
    make_run(tmp / "run", merged, summary, project="<script>alert(1)</script>")
    page = (tmp / "run" / "index.html").read_text()
    check("<script>" not in page, "html: project name is escaped")


def _cli(args, cwd, env=None):
    import os
    merged = {**os.environ, **(env or {})}
    return subprocess.run([str(REPO / "tools" / "permute"), *map(str, args)],
                          cwd=cwd, env=merged, capture_output=True, text=True,
                          timeout=600)


def test_cli_outputs(tmp: Path):
    tmp.mkdir(parents=True, exist_ok=True)
    guest = REPO / "build" / "scenarios" / "proc_s01_hello__0.so"
    if not guest.is_file():
        raise SystemExit("missing scenarios; run make build")

    project = tmp / "project"
    result = _cli(["init", project], cwd=tmp)
    check(result.returncode == 0, f"cli: init {result.stderr}")

    result = _cli(["run", "--seeds", "0-2", "--guest", guest], cwd=project)
    check(result.returncode == 0, f"cli clean: {result.stderr}")
    out = project / ".permute/run"
    for name in ("report.json", "junit.xml", "sarif.json", "index.html"):
        check((out / name).is_file(), f"cli clean: {name} exists")
    check(not report.validate_junit(out / "junit.xml"), "cli clean: junit")
    check(not report.validate_sarif(out / "sarif.json"), "cli clean: sarif")
    root = ET.parse(out / "junit.xml").getroot()
    check(root.attrib["failures"] == "0", "cli clean: no failures")

    config = project / "permute.toml"
    config.write_text(config.read_text().replace(
        "inject = []", 'inject = ["send:drop@1"]'))
    result = _cli(["run", "--seeds", "0", "--guest", guest], cwd=project)
    check(result.returncode == 1, f"cli violation: exit {result.returncode}")
    root = ET.parse(project / ".permute/run/junit.xml").getroot()
    check(root.attrib["failures"] == "1", "cli violation: JUnit failure")
    sarif = json.loads((project / ".permute/run/sarif.json").read_text())
    check(any(item["ruleId"].startswith("oracle-")
              for item in sarif["runs"][0]["results"]),
          "cli violation: SARIF oracle result")


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        test_all_formats(root)
        test_warn_and_verify(root / "warn")
        test_escaping(root / "escape")
        test_cli_outputs(root / "cli")
    print(f"test_report: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
