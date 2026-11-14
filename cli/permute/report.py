# SPDX-License-Identifier: Apache-2.0
"""Run reports: JSON, JUnit, SARIF and a self-contained HTML page.

The four formats are written from the same merged run data. JUnit is for CI,
SARIF for code scanning, HTML for a human offline. No renderer reaches the
network: the HTML has one inline stylesheet and no scripts, links, fonts or
imports.
"""

from __future__ import annotations

import html
import json
import xml.etree.ElementTree as ET
from pathlib import Path

VERSION = "0.1.0"
SARIF_SCHEMA = "https://json.schemastore.org/sarif-2.1.0.json"

LEVELS = {
    "crash": "error",
    "violation": "error",
    "internal": "error",
    "nondeterminism": "error",
    "livelock": "error",
    "budget": "note",
}


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


# ---------------------------------------------------------------------------
# Per-seed rows


def seed_rows(merged: dict, out_dir: Path) -> list[dict]:
    """One row per seed, with the strongest status it earned."""
    rows: dict[int, dict] = {}

    for trace in sorted(Path(out_dir).glob("traces/seed-*.pmt")):
        try:
            seed = int(trace.stem.split("-", 1)[1])
        except ValueError:
            continue
        rows[seed] = {"seed": seed, "status": "clean", "messages": []}

    for finding in merged.get("findings", []):
        seed = finding.get("seed")
        if seed is None:
            continue
        status = str(finding.get("outcome", "internal")).lower()
        if status not in ("crash", "violation", "budget", "internal"):
            status = "internal"
        row = rows.setdefault(seed, {"seed": seed, "status": "clean",
                                     "messages": []})
        if row["status"] == "clean" or status in ("crash", "violation"):
            row["status"] = status
        row["messages"].append(
            f"worker: {finding.get('outcome')}"
            + (f" signal {finding['signal']}" if finding.get("signal") else ""))

    for violation in merged.get("violations", []):
        if violation.get("severity", "error") != "error":
            continue
        seed = violation.get("seed")
        if seed is None:
            continue
        row = rows.setdefault(seed, {"seed": seed, "status": "clean",
                                     "messages": []})
        row["status"] = "violation"
        row["messages"].append(
            f"{violation.get('kind')}[{violation.get('invariant')}]: "
            f"{violation.get('detail', '')}")

    verify = merged.get("verify") or {}
    for mismatch in verify.get("mismatches", []):
        seed = mismatch.get("seed")
        if seed is None:
            continue
        row = rows.setdefault(seed, {"seed": seed, "status": "clean",
                                     "messages": []})
        row["status"] = "nondeterminism"
        row["messages"].append(
            f"digest {mismatch.get('first')} != {mismatch.get('second')}")

    return [rows[seed] for seed in sorted(rows)]


# ---------------------------------------------------------------------------
# JUnit


def write_junit(path: Path, merged: dict, out_dir: Path) -> None:
    rows = seed_rows(merged, out_dir)
    failures = sum(1 for row in rows
                   if row["status"] in ("crash", "violation",
                                        "nondeterminism"))
    errors = sum(1 for row in rows if row["status"] == "internal")
    skipped = sum(1 for row in rows if row["status"] == "budget")

    root = ET.Element("testsuites", {
        "name": "permute",
        "tests": str(len(rows)),
        "failures": str(failures),
        "errors": str(errors),
        "skipped": str(skipped),
        "time": "0",
    })
    suite = ET.SubElement(root, "testsuite", {
        "name": "permute",
        "tests": str(len(rows)),
        "failures": str(failures),
        "errors": str(errors),
        "skipped": str(skipped),
        "time": "0",
    })

    for row in rows:
        case = ET.SubElement(suite, "testcase", {
            "classname": "permute.seed",
            "name": f"seed-{row['seed']}",
            "time": "0",
        })
        detail = "\n".join(row["messages"])
        if row["status"] in ("crash", "violation", "nondeterminism"):
            failure = ET.SubElement(case, "failure", {
                "message": row["status"].upper(),
                "type": row["status"],
            })
            failure.text = detail
        elif row["status"] == "internal":
            error = ET.SubElement(case, "error", {
                "message": "INTERNAL",
                "type": "internal",
            })
            error.text = detail
        elif row["status"] == "budget":
            skipped_el = ET.SubElement(case, "skipped", {
                "message": "BUDGET",
            })
            skipped_el.text = detail

    tree = ET.ElementTree(root)
    ET.indent(tree, space="  ")
    tree.write(path, encoding="utf-8", xml_declaration=True)


def validate_junit(path: Path) -> list[str]:
    """Structural JUnit validation: what a schema check would reject.

    The project does not vendor the JUnit XSD, so this checks the invariants a
    schema enforces for the subset emitted here: element nesting, required
    attributes, and count agreement.
    """
    problems: list[str] = []
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as exc:
        return [f"not XML: {exc}"]
    if root.tag != "testsuites":
        problems.append("root is not <testsuites>")
        return problems

    for attr in ("tests", "failures", "errors", "skipped"):
        if attr not in root.attrib:
            problems.append(f"<testsuites> missing {attr}")
    suites = root.findall("testsuite")
    if not suites:
        problems.append("no <testsuite>")
    cases = root.findall("./testsuite/testcase")
    for case in cases:
        for attr in ("classname", "name"):
            if attr not in case.attrib:
                problems.append(f"<testcase> missing {attr}")
    counts = {
        "tests": len(cases),
        "failures": len(root.findall("./testsuite/testcase/failure")),
        "errors": len(root.findall("./testsuite/testcase/error")),
        "skipped": len(root.findall("./testsuite/testcase/skipped")),
    }
    for attr, value in counts.items():
        if root.attrib.get(attr) != str(value):
            problems.append(f"testsuites {attr}={root.attrib.get(attr)} "
                            f"but found {value}")
    return problems


# ---------------------------------------------------------------------------
# SARIF


def write_sarif(path: Path, merged: dict, summary: dict,
                out_dir: Path) -> None:
    rules = [
        ("seed-crash", "The SUT crashed", "error"),
        ("seed-violation", "The SUT reported a violation", "error"),
        ("seed-internal", "The harness failed internally", "error"),
        ("nondeterminism", "Two runs of the same seed differ", "error"),
        ("livelock", "More than 2% of runs hit the budget", "error"),
        ("budget", "A run hit its budget", "note"),
    ]
    rule_ids = {rule[0] for rule in rules}
    results = []

    for row in seed_rows(merged, out_dir):
        if row["status"] == "clean":
            continue
        rule = ("nondeterminism" if row["status"] == "nondeterminism"
                else f"seed-{row['status']}")
        if rule == "seed-budget":
            rule = "budget"
        results.append({
            "ruleId": rule,
            "level": LEVELS.get(row["status"], "error"),
            "message": {"text": "; ".join(row["messages"]) or row["status"]},
            "locations": [{
                "physicalLocation": {
                    "artifactLocation": {
                        "uri": f"traces/seed-{row['seed']}.pmt",
                    },
                },
            }],
            "properties": {"seed": row["seed"], "status": row["status"]},
        })

    for violation in merged.get("violations", []):
        kind = violation.get("kind", "invariant")
        rule = f"oracle-{kind}"
        if rule not in rule_ids:
            rules.append((rule, f"Invariant {kind}", "warning"))
            rule_ids.add(rule)
        severity = violation.get("severity", "error")
        results.append({
            "ruleId": rule,
            "level": {"error": "error", "warn": "warning",
                      "info": "note"}.get(severity, "warning"),
            "message": {"text": f"{kind}[{violation.get('invariant')}]: "
                                f"{violation.get('detail', '')}"},
            "locations": [{
                "physicalLocation": {
                    "artifactLocation": {
                        "uri": f"traces/seed-{violation.get('seed', 0)}.pmt",
                    },
                },
            }],
            "properties": {"seed": violation.get("seed"),
                           "severity": severity},
        })

    if summary.get("livelock_suspect"):
        results.append({
            "ruleId": "livelock",
            "level": "error",
            "message": {"text": "more than 2% of runs hit the budget; "
                                "LIVELOCK_SUSPECT"},
        })

    payload = {
        "$schema": SARIF_SCHEMA,
        "version": "2.1.0",
        "runs": [{
            "tool": {
                "driver": {
                    "name": "permute",
                    "version": VERSION,
                    "informationUri":
                        "https://github.com/Divyanshu-kumar14/Permute",
                    "rules": [
                        {
                            "id": rule_id,
                            "name": rule_id,
                            "shortDescription": {"text": text},
                            "defaultConfiguration": {"level": level},
                        }
                        for rule_id, text, level in rules
                    ],
                },
            },
            "invocations": [{
                "executionSuccessful": summary.get("verdict") == "clean",
            }],
            "results": results,
        }],
    }
    path.write_text(json.dumps(payload, indent=2) + "\n")


def validate_sarif(path: Path) -> list[str]:
    try:
        import jsonschema
    except ImportError:
        return ["jsonschema is not installed"]
    schema_path = Path(__file__).resolve().parents[2] / "schemas" / \
        "sarif-2.1.0.json"
    if not schema_path.is_file():
        return [f"missing schema {schema_path}"]
    schema = json.loads(schema_path.read_text())
    document = json.loads(path.read_text())
    validator = jsonschema.Draft4Validator(schema)
    return [f"{error.json_path}: {error.message}"
            for error in sorted(validator.iter_errors(document),
                                key=lambda e: list(e.path))]


# ---------------------------------------------------------------------------
# HTML


def write_html(path: Path, merged: dict, summary: dict, out_dir: Path,
               project: str) -> None:
    rows = seed_rows(merged, out_dir)
    escape = html.escape

    def cell(value) -> str:
        return escape(str(value))

    summary_rows = "".join(
        f"<tr><th>{cell(name)}</th><td>{cell(summary.get(name, 0))}</td></tr>"
        for name in ("seeds", "clean", "crash", "budget", "violation",
                     "internal"))

    findings_rows = "".join(
        f"<tr><td>{cell(row['seed'])}</td><td class='{cell(row['status'])}'>"
        f"{cell(row['status'])}</td><td>{cell('; '.join(row['messages']))}"
        f"</td></tr>"
        for row in rows
    ) or "<tr><td colspan='3'>no findings</td></tr>"

    oracle_rows = "".join(
        f"<tr><td>{cell(item.get('seed'))}</td>"
        f"<td>{cell(item.get('kind'))}</td>"
        f"<td>{cell(item.get('severity'))}</td>"
        f"<td>{cell(item.get('detail'))}</td></tr>"
        for item in merged.get("violations", [])
    ) or "<tr><td colspan='4'>none</td></tr>"

    verify = summary.get("verify") or {}
    verify_rows = "".join(
        f"<tr><td>{cell(item.get('seed'))}</td><td>{cell(item.get('first'))}"
        f"</td><td>{cell(item.get('second'))}</td></tr>"
        for item in verify.get("mismatches", [])
    ) or "<tr><td colspan='3'>none</td></tr>"

    banner = ""
    if summary.get("livelock_suspect"):
        banner = ("<p class='banner'>LIVELOCK_SUSPECT: more than 2% of runs "
                  "hit the budget.</p>")

    document = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{escape(project)} — Permute report</title>
<style>
:root {{ color-scheme: light dark; }}
body {{ font: 15px/1.5 system-ui, sans-serif; margin: 0 auto; max-width: 60rem;
       padding: 2rem; }}
h1 {{ margin-bottom: 0.2rem; }}
h2 {{ margin-top: 2rem; }}
table {{ border-collapse: collapse; width: 100%; }}
th, td {{ text-align: left; padding: 0.35rem 0.5rem;
          border-bottom: 1px solid rgba(128,128,128,0.3); }}
th {{ width: 12rem; }}
.status {{ font-weight: 600; }}
.ok {{ color: #18794e; }}
.crash, .violation, .internal, .nondeterminism {{ color: #b42318; }}
.budget {{ color: #8a5a00; }}
.banner {{ padding: 0.6rem 0.8rem; border: 1px solid #b42318;
           background: rgba(180,35,24,0.08); }}
footer {{ margin-top: 2rem; font-size: 0.85rem; opacity: 0.7; }}
</style>
</head>
<body>
<h1>{escape(project)}</h1>
<p>Verdict: <strong class="status {cell(summary.get('verdict'))}">
{cell(summary.get('verdict'))}</strong> · {cell(summary.get('text', ''))}</p>
{banner}
<h2>Summary</h2>
<table>{summary_rows}</table>
<h2>Seeds</h2>
<table><thead><tr><th>Seed</th><th>Status</th><th>Detail</th></tr></thead>
<tbody>{findings_rows}</tbody></table>
<h2>Invariant findings</h2>
<table><thead><tr><th>Seed</th><th>Kind</th><th>Severity</th><th>Detail</th>
</tr></thead><tbody>{oracle_rows}</tbody></table>
<h2>Determinism check</h2>
<p>{cell(verify.get('checked', 0))} seed(s) re-executed,
{cell(len(verify.get('mismatches', [])))} mismatch(es).</p>
<table><thead><tr><th>Seed</th><th>First digest</th><th>Second digest</th>
</tr></thead><tbody>{verify_rows}</tbody></table>
<footer>Permute {VERSION} · static report, no network access.</footer>
</body>
</html>
"""
    path.write_text(document)


def write_all(out_dir: Path, merged: dict, summary: dict,
              project: str) -> list[Path]:
    out_dir = Path(out_dir)
    write_junit(out_dir / "junit.xml", merged, out_dir)
    write_sarif(out_dir / "sarif.json", merged, summary, out_dir)
    write_html(out_dir / "index.html", merged, summary, out_dir, project)
    return [out_dir / "report.json", out_dir / "junit.xml",
            out_dir / "sarif.json", out_dir / "index.html"]
