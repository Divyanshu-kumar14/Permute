# SPDX-License-Identifier: Apache-2.0
"""The six deterministic mutation operators (FR-5.5).

A tape is the input side of a run: seed, fault rules, stimuli. Every operator
is a pure function of the tape, the corpus, and an index, so the same chain is
reproduced on every machine and every shard.
"""

from __future__ import annotations

import copy
from dataclasses import dataclass, field

MASK64 = (1 << 64) - 1


def splitmix64(x: int) -> int:
    x = (x + 0x9E3779B97F4A7C15) & MASK64
    z = x
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return z ^ (z >> 31)


@dataclass
class Tape:
    seed: int = 0
    faults: list[str] = field(default_factory=list)
    stimuli: list[dict] = field(default_factory=list)

    def clone(self) -> "Tape":
        return Tape(self.seed, list(self.faults),
                    copy.deepcopy(self.stimuli))

    def to_dict(self) -> dict:
        return {"seed": self.seed, "faults": self.faults,
                "stimuli": self.stimuli}


OPERATORS = (
    "pick_flip",
    "suffix_splice",
    "run_delete_duplicate",
    "parameter_perturb",
    "stimulus_shift",
    "reseed",
)


def pick_flip(tape: Tape, index: int, corpus: list[Tape]) -> Tape:
    out = tape.clone()
    out.seed ^= 1 << (splitmix64(index) % 63)
    return out


def suffix_splice(tape: Tape, index: int, corpus: list[Tape]) -> Tape:
    out = tape.clone()
    if not corpus:
        return out
    other = corpus[splitmix64(index + 1) % len(corpus)]
    if other.faults:
        cut = splitmix64(index + 2) % (len(other.faults) + 1)
        out.faults = out.faults + other.faults[cut:]
    if other.stimuli:
        cut = splitmix64(index + 3) % (len(other.stimuli) + 1)
        out.stimuli = out.stimuli + copy.deepcopy(other.stimuli[cut:])
    return out


def run_delete_duplicate(tape: Tape, index: int, corpus: list[Tape]) -> Tape:
    out = tape.clone()
    total = len(out.faults) + len(out.stimuli)
    if total == 0:
        return out
    choice = splitmix64(index + 4) % total
    duplicate = (splitmix64(index + 5) & 1) == 1
    if choice < len(out.faults):
        if duplicate:
            out.faults.insert(choice, out.faults[choice])
        else:
            out.faults.pop(choice)
    else:
        at = choice - len(out.faults)
        if duplicate:
            out.stimuli.insert(at, copy.deepcopy(out.stimuli[at]))
        else:
            out.stimuli.pop(at)
    return out


def parameter_perturb(tape: Tape, index: int, corpus: list[Tape]) -> Tape:
    out = tape.clone()
    if not out.faults:
        out.seed = splitmix64(out.seed + index)
        return out
    at = splitmix64(index + 6) % len(out.faults)
    rule = out.faults[at]
    head, _, rest = rule.partition("@")
    if not rest:
        return out
    probability, _, tail = rest.partition(":")
    try:
        value = float(probability)
    except ValueError:
        return out
    step = (splitmix64(index + 7) % 3) - 1  # -1, 0, +1
    value = min(1.0, max(0.0, value + step * 0.05))
    text = f"{value:.2f}".rstrip("0").rstrip(".")
    out.faults[at] = f"{head}@{text}" + (f":{tail}" if tail else "")
    return out


def stimulus_shift(tape: Tape, index: int, corpus: list[Tape]) -> Tape:
    out = tape.clone()
    if not out.stimuli:
        return out
    at = splitmix64(index + 8) % len(out.stimuli)
    stimulus = out.stimuli[at]
    try:
        current = int(stimulus.get("at_ns", 0))
    except (TypeError, ValueError):
        current = 0
    stimulus["at_ns"] = current + 1 + (splitmix64(index + 9) % 1000)
    return out


def reseed(tape: Tape, index: int, corpus: list[Tape]) -> Tape:
    out = tape.clone()
    out.seed = splitmix64(tape.seed + index + 1)
    return out


_FUNCS = (pick_flip, suffix_splice, run_delete_duplicate, parameter_perturb,
          stimulus_shift, reseed)


def apply(index: int, tape: Tape, corpus: list[Tape]) -> tuple[Tape, str]:
    """Apply operator `index % 6`; return the tape and the operator name."""
    slot = index % len(_FUNCS)
    return _FUNCS[slot](tape, index, corpus), OPERATORS[slot]
