# SPDX-License-Identifier: Apache-2.0
"""Input shrinker: causal pre-slice, ddmin, window, tape, faults.

The shrinker only ever removes *inputs* — stimuli, decision overrides, fault
rules, and the time window in which they are active. It never edits an event
log (FR-6.7, ADR-009). Every candidate is a complete, valid execution, and the
predicate decides whether the failure still reproduces.

Order is fixed (FR-6.8) and every list operation is deterministic (FR-6.10):
no RNG, index-ordered splits, canonical decision order. The serialised result
contains no wall-clock value, so two runs are byte-identical.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field, replace

from . import decisions as tape
from .hdd import BudgetExhausted, ddmin, pull

MAX_EXECUTIONS = 1500
MAX_SECONDS = 90.0

# Domains present in a stimulus table entry; other keys are payload.
STIMULUS_ID = "id"


@dataclass
class ShrinkInput:
    seed: int = 0
    stimuli: list[dict] = field(default_factory=list)
    faults: list[str] = field(default_factory=list)
    decisions: list[tape.Decision] = field(default_factory=list)
    window: tuple[int, int] | None = None
    # Input key -> record seqs the input can influence. Conservative: an input
    # with no entry is kept by the pre-slice.
    touches: dict[str, list[int]] = field(default_factory=dict)

    def size(self) -> int:
        return (len(self.stimuli) + len(self.faults) + len(self.decisions)
                + (2 if self.window is not None else 0))

    def clone(self, **changes) -> "ShrinkInput":
        return replace(self, **changes)

    def to_dict(self) -> dict:
        return {
            "seed": self.seed,
            "stimuli": self.stimuli,
            "faults": self.faults,
            "decisions": [d.as_dict() for d in self.decisions],
            "window": list(self.window) if self.window is not None else None,
            "touches": {key: sorted(value)
                        for key, value in sorted(self.touches.items())},
        }

    @classmethod
    def from_dict(cls, raw: dict) -> "ShrinkInput":
        window = raw.get("window")
        return cls(
            seed=int(raw.get("seed", 0)),
            stimuli=[dict(item) for item in raw.get("stimuli", [])],
            faults=[str(rule) for rule in raw.get("faults", [])],
            decisions=tape.canonical([tape.parse(item)
                                      for item in raw.get("decisions", [])]),
            window=(int(window[0]), int(window[1])) if window else None,
            touches={str(key): [int(seq) for seq in value]
                     for key, value in raw.get("touches", {}).items()},
        )

    def stimulus_key(self, index: int) -> str:
        item = self.stimuli[index]
        identifier = item.get(STIMULUS_ID)
        return f"stimulus:{identifier}" if identifier is not None else \
               f"stimulus:{index}"

    @staticmethod
    def fault_key(rule: str) -> str:
        return f"fault:{rule}"

    @staticmethod
    def decision_key(decision: tape.Decision) -> str:
        return f"decision:{decision.domain}:{decision.index}"


# ---------------------------------------------------------------------------
# Budget


class Budget:
    """The 1,500-execution / 90-second bound, enforced before every run."""

    def __init__(self, max_executions: int = MAX_EXECUTIONS,
                 max_seconds: float = MAX_SECONDS, clock=time.monotonic):
        self.max_executions = max_executions
        self.max_seconds = max_seconds
        self.clock = clock
        self.started = clock()
        self.executions = 0

    def elapsed(self) -> float:
        return self.clock() - self.started

    def spend(self) -> None:
        # Checked before the execution, so the count can never exceed the
        # bound: 1,500 means at most 1,500, not 1,501.
        if self.executions >= self.max_executions:
            raise BudgetExhausted
        if self.elapsed() >= self.max_seconds:
            raise BudgetExhausted
        self.executions += 1


class Runner:
    def __init__(self, test, budget: Budget):
        self.test = test
        self.budget = budget

    def __call__(self, candidate: ShrinkInput) -> bool:
        self.budget.spend()
        result = bool(self.test(candidate))
        # A single execution can outlast the wall bound; stop immediately
        # rather than starting another one.
        if self.budget.elapsed() >= self.budget.max_seconds:
            raise BudgetExhausted
        return result


# ---------------------------------------------------------------------------
# Causal pre-slice


def causal_cone(records, start_seq: int) -> set[int]:
    """Ancestors of `start_seq` over a conservative causal graph.

    Edges that are certainly causal: program order within a thread, a receive
    and the reply that answers it (same rcvid), and a send and the receive that
    carries the same payload. Ambiguous matches link everything they could
    match, which makes the cone larger and the slice safer.
    """
    by_seq = {record["seq"]: record for record in records}
    parents: dict[int, set[int]] = {record["seq"]: set() for record in records}

    previous: dict[int, int] = {}
    rcvids: dict[tuple[int, int], list[int]] = {}
    sends: dict[bytes, list[int]] = {}

    for record in records:
        seq = record["seq"]
        kind = record["kind"]
        src = record.get("src") or 0

        # Program order: every record from a thread follows its previous one.
        if src in previous:
            parents[seq].add(previous[src])
        previous[src] = seq

        if kind == "RECV" and (record.get("obj") or 0) > 0:
            rcvids.setdefault((src, record["obj"]), []).append(seq)
        elif kind in ("REPLY", "ERROR") and (record.get("obj") or 0) > 0:
            # The reply depends on the receive that produced the rcvid, and the
            # receive depends on the reply when walking forward. Backwards, the
            # reply's ancestor is the receive.
            for recv_seq in rcvids.get((src, record["obj"]), []):
                parents[seq].add(recv_seq)

        blob = record.get("blob")
        if kind == "SEND" and blob:
            sends.setdefault(blob, []).append(seq)
        elif kind == "RECV" and blob:
            for send_seq in sends.get(blob, []):
                # An earlier send with the same payload may be this message.
                parents[seq].add(send_seq)

    cone: set[int] = set()
    stack = [start_seq]
    while stack:
        seq = stack.pop()
        if seq in cone:
            continue
        cone.add(seq)
        stack.extend(parents.get(seq, ()))
    return cone


# ---------------------------------------------------------------------------
# Pipeline


@dataclass
class StageResult:
    stage: str
    before: int
    after: int
    executions: int

    def as_dict(self) -> dict:
        return {"stage": self.stage, "before": self.before,
                "after": self.after, "executions": self.executions}


@dataclass
class ShrinkResult:
    input: ShrinkInput
    original_size: int
    minimality: str
    executions: int
    stages: list[StageResult] = field(default_factory=list)
    elapsed_seconds: float = 0.0  # display only, never serialised

    def to_dict(self) -> dict:
        return {
            "minimality": self.minimality,
            "executions": self.executions,
            "original_size": self.original_size,
            "final_size": self.input.size(),
            "stages": [stage.as_dict() for stage in self.stages],
            "input": self.input.to_dict(),
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), sort_keys=True,
                          separators=(",", ":"))


class Shrinker:
    def __init__(self, test, budget: Budget | None = None,
                 cone: set[int] | None = None):
        self.budget = budget or Budget()
        self.runner = Runner(test, self.budget)
        self.cone = cone
        self.current: ShrinkInput | None = None
        self.stages: list[StageResult] = []

    def _record(self, name: str, before: int, started: int) -> None:
        assert self.current is not None
        self.stages.append(StageResult(name, before, self.current.size(),
                                       self.budget.executions - started))

    def _test(self, candidate: ShrinkInput) -> bool:
        return self.runner(candidate)

    def shrink(self, inp: ShrinkInput) -> ShrinkResult:
        self.current = inp.clone()
        original = self.current.size()
        minimality = "proven"

        try:
            self._pre_slice()
            self._stimulus_ddmin()
            self._window_min()
            self._tape_elimination()
            self._fault_materialisation()
        except BudgetExhausted:
            minimality = "budget_limited"

        return ShrinkResult(
            input=self.current,
            original_size=original,
            minimality=minimality,
            executions=self.budget.executions,
            stages=self.stages,
            elapsed_seconds=self.budget.elapsed(),
        )

    # ------------------------------------------------------------------

    def _pre_slice(self) -> None:
        """Drop inputs whose recorded effects are outside the failure cone."""
        assert self.current is not None
        if not self.cone or not self.current.touches:
            return

        before = self.current.size()
        started = self.budget.executions

        def inside(key: str) -> bool:
            seqs = self.current.touches.get(key)
            return not seqs or bool(set(seqs) & self.cone)

        stimulus_index = [i for i in range(len(self.current.stimuli))
                          if inside(self.current.stimulus_key(i))]
        stimuli = [self.current.stimuli[i] for i in stimulus_index]
        faults = [rule for rule in self.current.faults
                  if inside(ShrinkInput.fault_key(rule))]
        decided = [d for d in self.current.decisions
                   if inside(ShrinkInput.decision_key(d))]

        candidate = self.current.clone(stimuli=stimuli, faults=faults,
                                       decisions=decided)
        if candidate.size() == before:
            return

        if self._test(candidate):
            self.current = candidate
        else:
            # The cone was wrong about at least one bucket. Try each alone so
            # a bad edge costs one bucket, not the whole slice.
            for field_name, value in (("stimuli", stimuli), ("faults", faults),
                                      ("decisions", decided)):
                trial = self.current.clone(**{field_name: value})
                if trial.size() < self.current.size() and self._test(trial):
                    self.current = trial

        self._record("causal-slice", before, started)

    def _stimulus_ddmin(self) -> None:
        assert self.current is not None
        if len(self.current.stimuli) < 2:
            return
        before = self.current.size()
        started = self.budget.executions

        def test(candidate: list[dict]) -> bool:
            return self._test(self.current.clone(stimuli=candidate))

        self.current = self.current.clone(
            stimuli=ddmin(self.current.stimuli, test))
        self._record("stimulus-ddmin", before, started)

    def _window_min(self) -> None:
        assert self.current is not None
        window = self.current.window
        if window is None:
            return
        before = self.current.size()
        started = self.budget.executions
        lo, hi = window

        def accepts(new_lo: int, new_hi: int) -> bool:
            if new_lo < 0 or new_hi <= new_lo:
                return False
            return self._test(self.current.clone(window=(new_lo, new_hi)))

        # Shrink the start upward, then the end downward. Each is a binary
        # search over the integer range, so the cost is logarithmic.
        original_hi = hi
        low, high = lo, hi
        while high - low > 1:
            middle = (low + high) // 2
            if accepts(middle, original_hi):
                low = middle
            else:
                high = middle
        lo = low

        low, high = lo, original_hi
        while high - low > 1:
            middle = (low + high) // 2
            if accepts(lo, middle):
                high = middle
            else:
                low = middle
        hi = high

        if (lo, hi) != window:
            self.current = self.current.clone(window=(lo, hi))
        self._record("window-min", before, started)

    def _tape_elimination(self) -> None:
        assert self.current is not None
        if len(self.current.decisions) < 2:
            return
        before = self.current.size()
        started = self.budget.executions

        def test(candidate: list[tape.Decision]) -> bool:
            return self._test(self.current.clone(decisions=candidate))

        self.current = self.current.clone(
            decisions=tape.eliminate(self.current.decisions, test))
        self._record("tape-elimination", before, started)

    def _fault_materialisation(self) -> None:
        assert self.current is not None
        if len(self.current.faults) < 2:
            return
        before = self.current.size()
        started = self.budget.executions

        def test(candidate: list[str]) -> bool:
            return self._test(self.current.clone(faults=candidate))

        reduced = ddmin(self.current.faults, test)
        reduced = pull(reduced, test)
        self.current = self.current.clone(faults=reduced)
        self._record("fault-materialisation", before, started)
