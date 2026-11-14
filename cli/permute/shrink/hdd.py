# SPDX-License-Identifier: Apache-2.0
"""Hierarchical delta debugging over input lists.

The algorithm is Zeller's ddmin with a fixed iteration order: chunks in index
order, complements in index order, granularity two until a reduction is found
and then back to two. There is no randomness, so the same list and the same
predicate always take the same path (FR-6.10).

`test` may raise `BudgetExhausted`; callers catch it at the pipeline boundary.
"""

from __future__ import annotations

from typing import Callable, Sequence, TypeVar

T = TypeVar("T")


class BudgetExhausted(Exception):
    """The execution budget cannot pay for another test."""


def _split(items: Sequence[T], n: int) -> list[tuple[int, int]]:
    """Return half-open index ranges covering `items` in `n` near-equal parts."""
    size = len(items)
    parts = min(n, size)
    ranges = []
    for i in range(parts):
        start = i * size // parts
        end = (i + 1) * size // parts
        ranges.append((start, end))
    return ranges


def ddmin(items: Sequence[T], test: Callable[[list[T]], bool]) -> list[T]:
    """Reduce `items` while `test` still succeeds.

    The predicate must accept a candidate list and return True when the failure
    still reproduces. Monotonicity is not required; a non-monotone predicate can
    stop at a local minimum, which is what `minimality` reports as proven
    relative to this operation set.
    """
    current = list(items)
    if len(current) < 2:
        return current

    granularity = 2
    while len(current) >= 2:
        ranges = _split(current, granularity)
        reduced = False

        for start, end in ranges:
            chunk = current[start:end]
            if not chunk:
                continue
            if test(chunk):
                current = chunk
                granularity = 2
                reduced = True
                break

        if reduced:
            continue

        for start, end in ranges:
            complement = current[:start] + current[end:]
            if len(complement) == len(current):
                continue
            if test(complement):
                current = complement
                granularity = max(granularity - 1, 2)
                reduced = True
                break

        if reduced:
            continue

        if granularity >= len(current):
            break
        granularity = min(granularity * 2, len(current))

    return current


def pull(items: Sequence[T], test: Callable[[list[T]], bool]) -> list[T]:
    """Delete elements one at a time, in order, while `test` still succeeds.

    A fixed point here is what makes the result 1-minimal: every remaining
    element was tried and removing it lost the failure.
    """
    current = list(items)
    index = 0
    while index < len(current):
        candidate = current[:index] + current[index + 1:]
        if test(candidate):
            current = candidate
        else:
            index += 1
    return current
