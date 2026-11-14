# SPDX-License-Identifier: Apache-2.0
"""Decision-tape overrides.

The kernel already separates seven decision domains so one area cannot shift
another (FR-4.3). A tape is the recorded value for a (domain, index) pair; a
replay can override the counter-derived value without touching the stream that
produced the rest.

The shrinker treats the tape as inputs, never the log (FR-6.7). Elimination is
per domain first — a whole domain's overrides can often go — then a single
override sweep, both in canonical order.
"""

from __future__ import annotations

from dataclasses import dataclass

from .hdd import ddmin, pull

DOMAIN_NAMES = (
    "sched", "timer_tie", "recv_order", "quantum",
    "stimulus", "fault", "cpujitter",
)


@dataclass(frozen=True)
class Decision:
    domain: int
    index: int
    value: int

    def as_dict(self) -> dict:
        return {"domain": self.domain, "index": self.index,
                "value": self.value}

    def key(self) -> tuple[int, int]:
        return (self.domain, self.index)


def parse(raw: dict) -> Decision:
    return Decision(int(raw["domain"]), int(raw["index"]), int(raw["value"]))


def canonical(decisions: list[Decision]) -> list[Decision]:
    """Stable order and no duplicate (domain, index). Last value wins."""
    by_key = {}
    for decision in decisions:
        by_key[decision.key()] = decision
    return [by_key[key] for key in sorted(by_key)]


def group_by_domain(decisions: list[Decision]) -> list[list[Decision]]:
    groups: list[list[Decision]] = []
    for domain in range(len(DOMAIN_NAMES)):
        group = [d for d in decisions if d.domain == domain]
        if group:
            groups.append(group)
    return groups


def eliminate(decisions: list[Decision], test) -> list[Decision]:
    """Reduce the tape to the overrides the failure still needs."""
    current = canonical(decisions)

    # Whole domains first: the cheapest reduction, and the one that keeps the
    # remaining tape readable.
    for group in group_by_domain(current):
        domain = group[0].domain
        rest = [d for d in current if d.domain != domain]
        if len(rest) < len(current) and test(rest):
            current = rest

    current = ddmin(current, test)
    return pull(current, test)
