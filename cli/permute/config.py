# SPDX-License-Identifier: Apache-2.0
"""Project configuration: parse and validate.

The schema is closed. An unknown key is an error, not a silent default,
because a typo in a kernel budget would otherwise be discovered as a wrong
result much later. Every rejection carries a stable code, a one-line cause,
and a one-line fix (FR-9.5).
"""

from __future__ import annotations

import re
import tomllib
from pathlib import Path

DEFAULT_CONFIG = "permute.toml"

SEED_RE = re.compile(r"^\d+(-\d+)?$")
FAULT_RE = re.compile(
    r"^(send|recv|reply|pulse):(drop|duplicate|reorder|delay|timeout|kill)"
    r"@(0(?:\.\d{1,6})?|1(?:\.0{1,6})?)(?::\d+)?$"
)

SCHEDULERS = ("priority-fifo", "priority-rr")
SEED_POLICIES = ("random", "novelty", "corpus")
SEVERITIES = ("error", "warn", "info")

TOP_KEYS = {"project", "run", "kernel", "faults", "stimulus", "invariant"}
PROJECT_KEYS = {"name", "version", "license", "description"}
RUN_KEYS = {"seeds", "jobs", "wall_ms"}
KERNEL_KEYS = {"max_events", "max_virtual_ms", "scheduler", "seed_policy"}
FAULTS_KEYS = {"inject"}
STIMULUS_KEYS = {"at", "kind", "to", "code", "value", "every"}
INVARIANT_KEYS = {
    "id", "kind", "severity", "task", "state", "channel",
    "max_virtual_ms", "max_virtual_us", "max_ready_gap_events",
    "max_senders", "max_depth",
}


class ConfigError(Exception):
    """A configuration problem with a stable code and a fix."""

    def __init__(self, code: str, cause: str, fix: str):
        super().__init__(cause)
        self.code = code
        self.cause = cause
        self.fix = fix

    def render(self) -> str:
        return f"{self.code}: {self.cause}\n  fix: {self.fix}"


def _unknown(section: str, key: str, allowed: set[str]) -> ConfigError:
    return ConfigError(
        "P1002",
        f"unknown key '{section}.{key}'" if section else f"unknown key '{key}'",
        f"remove it, or use one of: {', '.join(sorted(allowed))}",
    )


def _want(table: dict, section: str, key: str, kind, default=None):
    value = table.get(key, default)
    if value is None:
        return None
    if kind is int and (not isinstance(value, int) or isinstance(value, bool)):
        raise ConfigError("P1003", f"'{section}.{key}' must be an integer",
                          f"set {key} to an integer")
    if kind is str and not isinstance(value, str):
        raise ConfigError("P1003", f"'{section}.{key}' must be a string",
                          f"quote the {key} value")
    if kind is list and not isinstance(value, list):
        raise ConfigError("P1003", f"'{section}.{key}' must be an array",
                          f"write {key} = [...]")
    return value


def _check_keys(section: str, table: dict, allowed: set[str]) -> None:
    for key in table:
        if key not in allowed:
            raise _unknown(section, key, allowed)


def validate(data: dict) -> dict:
    if not isinstance(data, dict):
        raise ConfigError("P1003", "configuration must be a table",
                          "start the file with a [project] section")

    _check_keys("", data, TOP_KEYS)

    project = data.get("project")
    if not isinstance(project, dict):
        raise ConfigError("P1003", "missing [project] table",
                          "add [project] with a name")
    _check_keys("project", project, PROJECT_KEYS)
    name = _want(project, "project", "name", str)
    if not name:
        raise ConfigError("P1003", "[project].name is required",
                          'add name = "my-project"')
    _want(project, "project", "version", str)
    _want(project, "project", "license", str)
    _want(project, "project", "description", str)

    run = data.get("run", {})
    if not isinstance(run, dict):
        raise ConfigError("P1003", "[run] must be a table", "see the docs")
    _check_keys("run", run, RUN_KEYS)
    seeds = _want(run, "run", "seeds", str, "0")
    if not SEED_RE.match(seeds or ""):
        raise ConfigError("P1003", f"[run].seeds is not a range: {seeds!r}",
                          'use seeds = "0-99" or seeds = "7"')
    jobs = _want(run, "run", "jobs", int, 1)
    if jobs < 1:
        raise ConfigError("P1003", "[run].jobs must be at least 1",
                          "set jobs = 1")
    wall_ms = _want(run, "run", "wall_ms", int, 2000)
    if wall_ms < 1:
        raise ConfigError("P1003", "[run].wall_ms must be positive",
                          "set wall_ms = 2000")

    kernel = data.get("kernel", {})
    if not isinstance(kernel, dict):
        raise ConfigError("P1003", "[kernel] must be a table", "see the docs")
    _check_keys("kernel", kernel, KERNEL_KEYS)
    for key, minimum in (("max_events", 1), ("max_virtual_ms", 1)):
        value = _want(kernel, "kernel", key, int)
        if value is not None and value < minimum:
            raise ConfigError("P1003", f"[kernel].{key} must be positive",
                              f"set {key} = {minimum}")
    scheduler = kernel.get("scheduler", "priority-fifo")
    if scheduler not in SCHEDULERS:
        raise ConfigError("P1003", f"invalid scheduler {scheduler!r}",
                          f"use one of: {', '.join(SCHEDULERS)}")
    policy = kernel.get("seed_policy", "random")
    if policy not in SEED_POLICIES:
        raise ConfigError("P1003", f"invalid seed_policy {policy!r}",
                          f"use one of: {', '.join(SEED_POLICIES)}")

    faults = data.get("faults", {})
    if not isinstance(faults, dict):
        raise ConfigError("P1003", "[faults] must be a table", "see the docs")
    _check_keys("faults", faults, FAULTS_KEYS)
    inject = _want(faults, "faults", "inject", list, [])
    for entry in inject:
        if not isinstance(entry, str):
            raise ConfigError("P1003", "faults.inject entries must be strings",
                              'write inject = ["send:drop@0.001"]')
        if not FAULT_RE.match(entry):
            raise ConfigError(
                "P1003", f"invalid fault rule {entry!r}",
                "use site:action@probability[:delay_ns], for example "
                "send:drop@0.01")

    stimuli = data.get("stimulus", [])
    if not isinstance(stimuli, list):
        raise ConfigError("P1003", "stimulus must be an array of tables",
                          "use [[stimulus]]")
    for index, stimulus in enumerate(stimuli):
        _check_keys(f"stimulus[{index}]", stimulus, STIMULUS_KEYS)
        for key in ("at", "kind"):
            if not isinstance(stimulus.get(key), str):
                raise ConfigError(
                    "P1003", f"stimulus[{index}].{key} is required",
                    f'add {key} = "..."')

    invariants = data.get("invariant", [])
    if not isinstance(invariants, list):
        raise ConfigError("P1003", "invariant must be an array of tables",
                          "use [[invariant]]")
    for index, invariant in enumerate(invariants):
        _check_keys(f"invariant[{index}]", invariant, INVARIANT_KEYS)
        if not isinstance(invariant.get("kind"), str):
            raise ConfigError("P1003", f"invariant[{index}].kind is required",
                              'add kind = "deadlock"')
        severity = invariant.get("severity", "error")
        if severity not in SEVERITIES:
            raise ConfigError("P1003", f"invalid severity {severity!r}",
                              f"use one of: {', '.join(SEVERITIES)}")

    return data


def load(path: Path | str = DEFAULT_CONFIG) -> dict:
    path = Path(path)
    if not path.is_file():
        raise ConfigError("P1001", f"no configuration at {path}",
                          "run `permute init .` or pass --config")
    try:
        data = tomllib.loads(path.read_text())
    except tomllib.TOMLDecodeError as exc:
        raise ConfigError("P1004", f"invalid TOML in {path}: {exc}",
                          "check the file with a TOML linter") from exc
    return validate(data)


def find(start: Path | None = None) -> Path:
    """Walk up from the working directory looking for permute.toml."""
    here = (start or Path.cwd()).resolve()
    for candidate in (here, *here.parents):
        path = candidate / DEFAULT_CONFIG
        if path.is_file():
            return path
    raise ConfigError("P1001", f"no {DEFAULT_CONFIG} found",
                      "run `permute init .` or run from a project directory")
