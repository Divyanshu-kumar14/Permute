# SPDX-License-Identifier: Apache-2.0
"""Exploration loop: corpus, six mutators, novelty, shards, escalation."""

from __future__ import annotations

import os
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from ..trace import codec
from .bitmap import NoveltyBitmap, features
from .mutate import Tape, apply, reseed, splitmix64

POLICIES = ("random", "novelty")
MAX_ESCALATIONS = 3
LIVELOCK_FRACTION = 0.02


@dataclass
class Entry:
    index: int
    operator: str
    tape: Tape
    outcome: str
    digest: str | None
    novelty: int
    coverage: int
    escalated: bool = False

    def as_dict(self) -> dict:
        return {
            "index": self.index,
            "operator": self.operator,
            "seed": self.tape.seed,
            "faults": self.tape.faults,
            "stimuli": self.tape.stimuli,
            "outcome": self.outcome,
            "digest": self.digest,
            "novelty": self.novelty,
            "coverage": self.coverage,
            "escalated": self.escalated,
        }


@dataclass
class Result:
    policy: str
    shard: str
    iterations: int
    entries: list[Entry] = field(default_factory=list)
    corpus_size: int = 0
    coverage: int = 0
    findings: int = 0
    escalations: int = 0
    budgets: int = 0

    def as_dict(self) -> dict:
        return {
            "policy": self.policy,
            "shard": self.shard,
            "iterations": self.iterations,
            "entries": [entry.as_dict() for entry in self.entries],
            "corpus_size": self.corpus_size,
            "coverage": self.coverage,
            "findings": self.findings,
            "escalations": self.escalations,
            "budgets": self.budgets,
        }

    @property
    def livelock_suspect(self) -> bool:
        return (self.iterations > 0 and
                self.budgets / self.iterations > LIVELOCK_FRACTION)


def parse_shard(shard: str) -> tuple[int, int]:
    index, _, count = shard.partition("/")
    index, count = int(index), int(count)
    if count < 1 or index < 0 or index >= count:
        raise ValueError(f"invalid shard {shard!r}; use i/n with 0 <= i < n")
    return index, count


def subprocess_runner(guest: str | None, runner: str,
                      timeout: float = 2.0):
    """Run one tape in a child process. Returns (outcome, digest, records)."""
    def run(tape: Tape, budget: float):
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / "explore.pmt"
            env = dict(os.environ)
            env.pop("PERMUTE_GUEST", None)
            env.pop("PERMUTE_FAULTS", None)
            if guest:
                env["PERMUTE_GUEST"] = guest
            if tape.faults:
                env["PERMUTE_FAULTS"] = ",".join(tape.faults)
            try:
                completed = subprocess.run(
                    [runner, str(tape.seed), str(trace)], env=env,
                    capture_output=True, text=True, timeout=budget)
            except subprocess.TimeoutExpired:
                return "BUDGET", None, None
            outcome = "INTERNAL"
            for line in completed.stdout.splitlines():
                if line.startswith("OUTCOME="):
                    outcome = line.partition("=")[2]
            if not trace.is_file():
                return outcome, None, None
            parsed = codec.read(trace)
            if not parsed.ok:
                return outcome, None, parsed.records or None
            digest = (f"{parsed.run_digest:016x}"
                      if parsed.run_digest is not None else None)
            return outcome, digest, parsed.records
    return run


def explore(policy: str, iterations: int, base: Tape, shard: str = "0/1",
            runner=None, timeout: float = 2.0) -> Result:
    if policy not in POLICIES:
        raise ValueError(f"unknown policy {policy!r}")
    if iterations < 0:
        raise ValueError("iterations must be non-negative")
    index, count = parse_shard(shard)
    if runner is None:
        raise ValueError("explore needs a runner")

    bitmap = NoveltyBitmap()
    corpus = [base]
    result = Result(policy=policy, shard=shard, iterations=iterations)
    escalations = 0

    for i in range(iterations):
        if policy == "random":
            tape = reseed(base, i, [])
            operator = "reseed"
        else:
            parent = corpus[i % len(corpus)]
            tape, operator = apply(i, parent, corpus)

        outcome, digest, records = runner(tape, timeout)
        escalated = False

        # FR-5.7: a budgeted run that produced new features is retried once at
        # four times the budget, at most three times per exploration.
        novelty = bitmap.add(features(records)) if records else 0
        if (outcome == "BUDGET" and novelty > 0
                and escalations < MAX_ESCALATIONS):
            escalations += 1
            escalated = True
            outcome, digest, records = runner(tape, timeout * 4)
            novelty += bitmap.add(features(records)) if records else 0
        elif outcome == "BUDGET":
            result.budgets += 1

        if novelty and policy == "novelty":
            corpus.append(tape)

        entry = Entry(index=i, operator=operator, tape=tape,
                      outcome=outcome, digest=digest, novelty=novelty,
                      coverage=bitmap.coverage, escalated=escalated)
        if i % count == index:
            result.entries.append(entry)
            if outcome not in ("TRACE", "CLEAN", "BUDGET"):
                result.findings += 1

    result.corpus_size = len(corpus)
    result.coverage = bitmap.coverage
    result.escalations = escalations
    return result
