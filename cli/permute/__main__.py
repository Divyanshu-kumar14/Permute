#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The `permute` command.

Exit codes are the product contract (PRD 9.5):

    0 success        1 violation      2 usage/config   3 build failed
    4 replay refused 5 nondeterminism 6 deadline       7 internal
    10 violations differ              11 digest mismatch
    130 interrupted

Every command supports `--json`. Configuration errors are closed-schema: an
unknown key is a rejection with a stable `P####` code, a cause, and a fix.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from . import config, report
from .oracle import evaluate
from .explore import Tape, explore, subprocess_runner
from .shrink import Budget, ShrinkInput, Shrinker, causal_cone
from .shrink.runner import fault_touches, load_artifact, make_predicate, write_artifact
from .run import Manifest, execute, replay_artifact, replay_seed
from .trace import codec

VERSION = "0.1.0"
HOME = Path(os.environ.get("PERMUTE_HOME", Path(__file__).resolve().parents[2]))

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

CONFIG_TEMPLATE = """\
# Permute project configuration.
# Unknown keys are rejected: a typo in a budget must fail loudly.

[project]
name = "{name}"
version = "0.1.0"

[run]
seeds = "0-2"
jobs = 1
wall_ms = 2000

[kernel]
max_events = 200000
max_virtual_ms = 60000
scheduler = "priority-fifo"
seed_policy = "random"

[faults]
inject = []

[[invariant]]
id = "dl"
kind = "deadlock"
severity = "error"
"""

SMOKE_SOURCE = """\
// SPDX-License-Identifier: Apache-2.0
//
// Smoke scenario: the smallest guest that links against the simulated kernel.
// Replace the body with real service code; only the link line changes.

#include <stddef.h>

#include "abi/permute_iface.h"

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}
"""


class HarnessError(Exception):
    """A missing or broken harness binary: internal, not a usage error."""

    def __init__(self, code: str, cause: str, fix: str):
        super().__init__(cause)
        self.code = code
        self.cause = cause
        self.fix = fix

    def render(self) -> str:
        return f"{self.code}: {self.cause}\n  fix: {self.fix}"


def _binary(name: str, env: str) -> Path:
    override = os.environ.get(env)
    path = Path(override) if override else HOME / "build" / name
    if not path.is_file():
        raise HarnessError(
            "P3001",
            f"{name} not found at {path}",
            "build the harness first: make build",
        )
    return path


def _project_dir() -> Path:
    return config.find().parent


def _emit(args, payload: dict, text: str) -> None:
    if getattr(args, "json", False):
        print(json.dumps(payload, indent=2))
    else:
        print(text)


# ---------------------------------------------------------------------------
# init


def _symbol_rows() -> list[tuple[str, str, str, str]]:
    import re

    pattern = re.compile(
        r"PERMUTE_SYMBOL\(\s*(\w+),\s*([^,]+),\s*(\w+),\s*(\()")
    text = (HOME / "abi" / "permute_iface.def").read_text()
    text = "\n".join(line.split("//", 1)[0] for line in text.splitlines())
    rows = []
    for match in pattern.finditer(text):
        rest = text[match.start(4):]
        depth = 0
        end = 0
        for end, char in enumerate(rest):
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    break
        rows.append((match.group(1), match.group(2).strip(), match.group(3),
                     rest[:end + 1]))
    return rows


def _audit_text(name: str) -> str:
    rows = _symbol_rows()
    groups = sorted({row[0] for row in rows})
    archive = HOME / "build" / "libpermute_guest.a"

    gate = subprocess.run(
        [sys.executable, str(HOME / "tools" / "symcheck.py")],
        capture_output=True, text=True,
    )
    gate_lines = [line for line in gate.stdout.splitlines() if line.strip()]

    lines = [
        "Permute symbol audit",
        f"project: {name}",
        f"surface: {len(rows)} symbols in {len(groups)} groups (tier 0)",
        f"archive: {archive}"
        + ("" if archive.is_file() else " (not built: run make build)"),
        "gate:",
    ]
    lines += [f"  {line}" for line in gate_lines]
    lines += ["", "| Group | Return | Symbol | Parameters |", "|---|---|---|---|"]
    lines += [f"| {g} | {r} | {s} | {p} |" for g, r, s, p in rows]
    lines.append("")
    return "\n".join(lines)


def cmd_init(args) -> int:
    target = Path(args.directory)
    if target.exists() and any(target.iterdir()) and not args.force:
        raise config.ConfigError(
            "P1005", f"{target} is not empty",
            "choose an empty directory or pass --force",
        )

    (target / "scenarios").mkdir(parents=True, exist_ok=True)
    project_name = target.resolve().name or "permute-project"

    files = {}
    files["permute.toml"] = CONFIG_TEMPLATE.format(name=project_name)
    files["scenarios/smoke.c"] = SMOKE_SOURCE
    files[".gitignore"] = ".permute/\n"
    files["symbol-audit.txt"] = _audit_text(project_name)

    header_gaps = subprocess.run(
        [sys.executable, str(HOME / "tools" / "header_gap.py")],
        capture_output=True, text=True,
    )
    files["header-gaps.md"] = header_gaps.stdout

    for relative, content in files.items():
        path = target / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    _emit(args,
          {"directory": str(target), "files": sorted(files)},
          f"initialised {target}\n"
          f"  permute.toml, scenarios/smoke.c, symbol-audit.txt, "
          f"header-gaps.md\n"
          f"next: cd {target} && permute run")
    return EXIT_OK


# ---------------------------------------------------------------------------
# build / run / show / clean


def cmd_build(args) -> int:
    path = config.find(args.config)
    data = config.load(path)
    project = path.parent

    source = project / "scenarios" / "smoke.c"
    if not source.is_file():
        raise config.ConfigError("P2002", f"no scenario at {source}",
                                 "add scenarios/smoke.c")

    out = project / ".permute" / "build"
    out.mkdir(parents=True, exist_ok=True)

    result = subprocess.run(
        [sys.executable, str(HOME / "tools" / "make_guest.py"),
         "--source", str(source), "--name", "smoke", "--instance", "0",
         "--out", str(out), "--build-dir", str(HOME / "build")],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        print("P2001: scenario build failed", file=sys.stderr)
        return EXIT_BUILD

    artifact = out / "proc_smoke__0.so"
    _emit(args,
          {"project": data["project"]["name"], "artifact": str(artifact)},
          f"built {artifact}")
    return EXIT_OK


def _stimuli(raw: list[dict]) -> list[dict]:
    """Give the exploration engine a numeric time for each stimulus."""
    out = []
    for item in raw:
        clone = dict(item)
        text = str(item.get("at", "0"))
        ns = 0
        try:
            if text.endswith("ms"):
                ns = int(text[:-2]) * 1_000_000
            elif text.endswith("s"):
                ns = int(text[:-1]) * 1_000_000_000
            else:
                ns = int(text)
        except ValueError:
            ns = 0
        clone["at_ns"] = ns
        out.append(clone)
    return out


def cmd_run(args) -> int:
    path = config.find(args.config)
    data = config.load(path)
    project = path.parent

    runner = _binary("permute-run", "PERMUTE_RUNNER")
    worker = _binary("permute-worker", "PERMUTE_WORKER")

    seeds = args.seeds or data["run"]["seeds"]
    jobs = args.jobs if args.jobs is not None else data["run"]["jobs"]
    wall_ms = (args.wall_ms if args.wall_ms is not None
               else data["run"]["wall_ms"])

    faults = data.get("faults", {}).get("inject", [])
    if faults and not args.guest:
        # Fault sites live on the guest bridge. Silently ignoring a configured
        # rule would report a clean run that never exercised it.
        raise config.ConfigError(
            "P1003", "faults are configured but no guest is selected",
            "pass --guest PATH; fault injection applies on the guest bridge",
        )

    out = project / ".permute" / "run"
    out.mkdir(parents=True, exist_ok=True)

    if args.explore:
        tape = Tape(seed=0, faults=list(faults),
                    stimuli=_stimuli(data.get("stimulus", [])))
        explorer = subprocess_runner(args.guest, str(runner),
                                     timeout=max(1.0, wall_ms / 1000.0))
        result = explore(args.explore, args.iterations, tape,
                         shard=args.shard, runner=explorer,
                         timeout=max(1.0, wall_ms / 1000.0))
        explore_report = result.as_dict()
        explore_report["livelock_suspect"] = result.livelock_suspect
        explore_report["text"] = (
            f"explore {result.policy}: {result.iterations} iteration(s), "
            f"corpus {result.corpus_size}, coverage {result.coverage}, "
            f"findings {result.findings}, "
            f"escalations {result.escalations}")
        explore_dir = project / ".permute" / "explore"
        explore_dir.mkdir(parents=True, exist_ok=True)
        (explore_dir / "report.json").write_text(
            json.dumps(explore_report, indent=2) + "\n")
        _emit(args, explore_report,
              explore_report["text"] + f"\nreport: {explore_dir}")
        if result.findings or result.livelock_suspect:
            if result.livelock_suspect and not args.json:
                print("LIVELOCK_SUSPECT: more than 2% of runs hit the budget")
            return EXIT_VIOLATION
        return EXIT_OK

    code = execute(seeds, str(out), jobs=jobs, runner=str(runner),
                   worker=str(worker), wall_ms=wall_ms, quiet=not args.verbose,
                   max_wall_ms=args.max_wall_ms, guest=args.guest,
                   faults=faults, shard=args.shard)

    merged = report.load(out / "report.json") if (out / "report.json").is_file() else {}

    invariants = data.get("invariant", [])
    oracle_findings = []
    oracle_skipped = []
    parsed_traces = {}
    if invariants:
        for trace_path in sorted((out / "traces").glob("seed-*.pmt")):
            seed = int(trace_path.stem.split("-", 1)[1])
            trace = codec.read(trace_path)
            if not trace.ok:
                # A crashed seed can leave a truncated trace; it already has a
                # finding of its own.
                continue
            parsed_traces[seed] = trace
            result = evaluate(trace.records, invariants, seed)
            oracle_findings.extend(finding.as_dict()
                                   for finding in result.findings)
            for invariant_id, reason in result.skipped:
                if (invariant_id, reason) not in oracle_skipped:
                    oracle_skipped.append((invariant_id, reason))

        merged["violations"] = oracle_findings
        errors = [f for f in oracle_findings if f["severity"] == "error"]
        already = {f["seed"] for f in merged.get("findings", [])}
        new_seeds = {f["seed"] for f in errors if f["seed"] not in already}
        if new_seeds:
            merged["violation"] = merged.get("violation", 0) + len(new_seeds)
            merged["clean"] = max(0, merged.get("clean", 0) - len(new_seeds))
            if code == EXIT_OK:
                code = EXIT_VIOLATION

        # A failure artifact is the shrinker's input: seed, faults, stimuli,
        # the invariant, and the finding. One per failing seed, first error in
        # trace order.
        artifacts = []
        first_error = {}
        for finding in errors:
            if finding["seed"] is not None:
                first_error.setdefault(finding["seed"], finding)
        for seed, finding in sorted(first_error.items()):
            rule = next((item for item in invariants
                         if item.get("id") == finding["invariant"]), None)
            trace = parsed_traces.get(seed)
            if rule is None or trace is None:
                continue
            artifact_path = out / "artifacts" / f"seed-{seed}.json"
            write_artifact(
                artifact_path, seed=seed, guest=args.guest, faults=faults,
                stimuli=data.get("stimulus", []), invariant=rule,
                finding=finding,
                trace=out / "traces" / f"seed-{seed}.pmt",
                digest=(f"{trace.run_digest:016x}"
                        if trace.run_digest is not None else None),
            )
            artifacts.append(str(artifact_path))
        if artifacts:
            merged["artifacts"] = artifacts

        if not args.json:
            for finding in oracle_findings:
                print(f"  {finding['kind']}[{finding['invariant']}] "
                      f"seed={finding['seed']}: {finding['detail']}")
            for invariant_id, reason in oracle_skipped:
                print(f"  {invariant_id}: not evaluated - {reason}")

    summary = report.summarise(merged)
    if summary["seeds"] and (
            summary["budget"] / summary["seeds"] > 0.02):
        summary["livelock_suspect"] = True
        summary["text"] += "; LIVELOCK_SUSPECT (budget > 2%)"
        if code == EXIT_OK:
            code = EXIT_VIOLATION
    summary["oracle_findings"] = oracle_findings
    summary["oracle_violations"] = len(
        [f for f in oracle_findings if f["severity"] == "error"])
    summary["exit_code"] = code
    summary["project"] = data["project"]["name"]
    summary["seeds"] = summary.get("seeds", 0)
    report.write(out / "summary.json", summary)

    _emit(args, summary,
          f"{summary['text']}\nreport: {out / 'report.json'}")
    return code


def cmd_shrink(args) -> int:
    path = Path(args.artifact)
    try:
        artifact = load_artifact(path)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        raise config.ConfigError(
            "P5001", f"cannot read artifact {path}: {exc}",
            "pass a file written by `permute run` under .permute/run/artifacts")

    if args.max_execs < 1 or args.max_seconds <= 0:
        raise config.ConfigError(
            "P5002", "shrink budgets must be positive",
            "use --max-execs 1500 --max-seconds 90")

    runner = _binary("permute-run", "PERMUTE_RUNNER")
    candidate = ShrinkInput.from_dict({
        "seed": artifact["seed"],
        "stimuli": artifact.get("stimuli", []),
        "faults": artifact.get("faults", []),
        "decisions": artifact.get("decisions", []),
        "window": artifact.get("window"),
    })

    trace_path = Path(artifact.get("trace", ""))
    if trace_path.is_file():
        recorded = codec.read(trace_path)
        if recorded.ok:
            seq = artifact["finding"].get("seq")
            if seq:
                candidate.touches = fault_touches(recorded.records,
                                                  candidate.faults)

    predicate = make_predicate(artifact, runner)
    budget = Budget(max_executions=args.max_execs,
                    max_seconds=args.max_seconds)

    # Reproduce before shrinking. A stale artifact is refused, not silently
    # reduced to an input that no longer fails.
    budget.spend()
    if not predicate(candidate):
        print(f"P5003: the recorded failure does not reproduce from {path}",
              file=sys.stderr)
        return EXIT_REFUSED

    cone = None
    if trace_path.is_file():
        recorded = codec.read(trace_path)
        seq = artifact["finding"].get("seq")
        if recorded.ok and seq:
            cone = causal_cone(recorded.records, seq)

    result = Shrinker(predicate, budget, cone=cone).shrink(candidate)
    result.input.touches = {}

    output = path.with_suffix(".shrunk.json")
    output.write_text(result.to_json() + "\n")

    if args.json:
        print(result.to_json())
    else:
        print(f"shrink: {result.original_size} -> {result.input.size()} "
              f"input(s), {result.minimality}, {result.executions} exec(s)")
        for stage in result.stages:
            print(f"  {stage.stage}: {stage.before} -> {stage.after}")
        print(f"artifact: {output}")
    return EXIT_OK


def cmd_show(args) -> int:
    path = config.find(args.config)
    project = path.parent
    run_dir = Path(args.run) if args.run else project / ".permute" / "run"

    seed = args.seed
    if seed is None:
        seed = 0

    trace_path = run_dir / "traces" / f"seed-{seed}.pmt"
    if not trace_path.is_file():
        raise config.ConfigError(
            "P4001", f"no trace at {trace_path}",
            "run `permute run` first, or pass --seed",
        )

    trace = codec.read(trace_path)
    if not trace.ok:
        print(f"P4002: trace is not readable: {trace.error}", file=sys.stderr)
        return EXIT_USAGE

    if args.format == "ndjson":
        sys.stdout.write(codec.to_ndjson(trace))
        return EXIT_OK
    if args.format == "chrome":
        print(json.dumps(codec.to_chrome(trace), indent=2))
        return EXIT_OK

    summary = {
        "path": str(trace_path),
        "seed": seed,
        "records": len(trace.records),
        "chunks": trace.n_chunks,
        "digest": f"{trace.run_digest:016x}",
        "first_kind": trace.records[0]["kind"] if trace.records else None,
        "last_kind": trace.records[-1]["kind"] if trace.records else None,
    }
    _emit(args, summary,
          f"{trace_path}\n"
          f"  {summary['records']} records in {summary['chunks']} chunk(s)\n"
          f"  digest {summary['digest']}")
    return EXIT_OK


def cmd_clean(args) -> int:
    project = _project_dir()
    target = project / ".permute"
    if target.exists():
        shutil.rmtree(target)
    _emit(args, {"removed": str(target)}, f"removed {target}")
    return EXIT_OK


# ---------------------------------------------------------------------------
# replay / internal hooks


def cmd_replay(args) -> int:
    from . import run as run_module

    return run_module.main(args.rest)


def cmd_internal_exit(args) -> int:
    allowed = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 130}
    if args.code not in allowed:
        print(f"unknown exit code {args.code}", file=sys.stderr)
        return EXIT_USAGE
    return args.code


# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="permute",
        description="Deterministic simulation testing for QNX message passing.",
    )
    parser.add_argument("--version", action="version",
                        version=f"permute {VERSION} "
                                f"(kernel model {codec.MODEL_VERSION})")

    sub = parser.add_subparsers(dest="command", required=True)

    init = sub.add_parser("init", help="scaffold a project")
    init.add_argument("directory")
    init.add_argument("--force", action="store_true")
    init.add_argument("--json", action="store_true")
    init.set_defaults(func=cmd_init)

    build = sub.add_parser("build", help="build the scenario")
    build.add_argument("--config")
    build.add_argument("--json", action="store_true")
    build.set_defaults(func=cmd_build)

    run = sub.add_parser("run", help="run seeds")
    run.add_argument("--config")
    run.add_argument("--seeds")
    run.add_argument("--jobs", type=int)
    run.add_argument("--wall-ms", type=int)
    run.add_argument("--max-wall-ms", type=int, default=0)
    run.add_argument("--guest", help="execute a guest .so instead of the built-in scenario")
    run.add_argument("--shard", default="0/1",
                     help="run only shard i/n of the seed range")
    run.add_argument("--explore", choices=("random", "novelty"),
                     help="explore instead of sweeping a seed range")
    run.add_argument("--iterations", type=int, default=32,
                     help="exploration iterations")
    run.add_argument("--verbose", action="store_true")
    run.add_argument("--json", action="store_true")
    run.set_defaults(func=cmd_run)

    shrink = sub.add_parser("shrink",
                            help="minimise the inputs of a recorded failure")
    shrink.add_argument("artifact")
    shrink.add_argument("--max-execs", type=int, default=1500)
    shrink.add_argument("--max-seconds", type=float, default=90.0)
    shrink.add_argument("--json", action="store_true")
    shrink.set_defaults(func=cmd_shrink)

    show = sub.add_parser("show", help="summarise or export a trace")
    show.add_argument("--config")
    show.add_argument("--run")
    show.add_argument("--seed", type=int)
    show.add_argument("--format", choices=("summary", "ndjson", "chrome"),
                      default="summary")
    show.add_argument("--json", action="store_true")
    show.set_defaults(func=cmd_show)

    replay = sub.add_parser("replay", help="verify from seed or artifact")
    replay.add_argument("rest", nargs=argparse.REMAINDER)
    replay.set_defaults(func=cmd_replay)

    clean = sub.add_parser("clean", help="remove .permute/")
    clean.add_argument("--json", action="store_true")
    clean.set_defaults(func=cmd_clean)

    # Harness hook: codes 10 (and any future staged code) have no user-facing
    # trigger yet, and FR-9.2 requires the matrix to be asserted end to end.
    internal = sub.add_parser("internal-exit", help=argparse.SUPPRESS)
    internal.add_argument("code", type=int)
    internal.set_defaults(func=cmd_internal_exit)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        return args.func(args)
    except config.ConfigError as exc:
        print(exc.render(), file=sys.stderr)
        return EXIT_USAGE
    except HarnessError as exc:
        print(exc.render(), file=sys.stderr)
        return EXIT_INTERNAL
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return EXIT_INTERRUPTED
    except BrokenPipeError:
        return EXIT_OK
    except Exception as exc:  # noqa: BLE001 - last line of defence
        print(f"P9000: internal error: {exc}", file=sys.stderr)
        return EXIT_INTERNAL


if __name__ == "__main__":
    sys.exit(main())
