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

    out = project / ".permute" / "run"
    out.mkdir(parents=True, exist_ok=True)

    code = execute(seeds, str(out), jobs=jobs, runner=str(runner),
                   worker=str(worker), wall_ms=wall_ms, quiet=not args.verbose,
                   max_wall_ms=args.max_wall_ms, guest=args.guest)

    merged = report.load(out / "report.json") if (out / "report.json").is_file() else {}
    summary = report.summarise(merged)
    summary["exit_code"] = code
    summary["project"] = data["project"]["name"]
    summary["seeds"] = summary.get("seeds", 0)
    report.write(out / "summary.json", summary)

    _emit(args, summary,
          f"{summary['text']}\nreport: {out / 'report.json'}")
    return code


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
    run.add_argument("--verbose", action="store_true")
    run.add_argument("--json", action="store_true")
    run.set_defaults(func=cmd_run)

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
