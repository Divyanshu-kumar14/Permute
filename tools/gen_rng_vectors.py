#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerate tests/golden/rng_vectors.tsv (FR-4.2).

The file is committed so both implementations are checked against a fixed
artifact rather than against each other's live output. Run this only when the
decision function itself is meant to change; a diff here is a compatibility
break.
"""

from __future__ import annotations

from pathlib import Path

MASK = (1 << 64) - 1
GOLDEN_GAMMA = 0x9E3779B97F4A7C15
MIX_A = 0xBF58476D1CE4E5B9
MIX_B = 0x94D049BB133111EB

COUNT = 100_000
REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "tests" / "golden" / "rng_vectors.tsv"


def splitmix64(x: int) -> int:
    x = (x + GOLDEN_GAMMA) & MASK
    z = x
    z = ((z ^ (z >> 30)) * MIX_A) & MASK
    z = ((z ^ (z >> 27)) * MIX_B) & MASK
    return (z ^ (z >> 31)) & MASK


def rnd(seed: int, domain: int, index: int) -> int:
    return splitmix64(seed ^ ((domain << 40) & MASK) ^ index)


def main() -> int:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w") as f:
        f.write("# seed\tdomain\tindex\tvalue\n")
        for i in range(COUNT):
            seed = i % 997
            domain = (i // 997) % 7
            index = i
            f.write(f"{seed}\t{domain}\t{index}\t{rnd(seed, domain, index):016x}\n")

    print(f"wrote {COUNT} vectors to {OUT} ({OUT.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
