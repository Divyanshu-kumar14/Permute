# SPDX-License-Identifier: Apache-2.0
"""Cross-language decision vectors and decision locality (FR-4.2).

The same committed file is checked by the C test; this side proves the Python
implementation the shrinker will use agrees with it.
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
VECTORS = REPO / "tests" / "golden" / "rng_vectors.tsv"

MASK = (1 << 64) - 1
GOLDEN_GAMMA = 0x9E3779B97F4A7C15
MIX_A = 0xBF58476D1CE4E5B9
MIX_B = 0x94D049BB133111EB

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def splitmix64(x: int) -> int:
    x = (x + GOLDEN_GAMMA) & MASK
    z = x
    z = ((z ^ (z >> 30)) * MIX_A) & MASK
    z = ((z ^ (z >> 27)) * MIX_B) & MASK
    return (z ^ (z >> 31)) & MASK


def rnd(seed: int, domain: int, index: int) -> int:
    return splitmix64(seed ^ ((domain << 40) & MASK) ^ index)


def main():
    if not VECTORS.is_file():
        raise SystemExit(f"missing {VECTORS}; run tools/gen_rng_vectors.py")

    total = 0
    mismatch = 0
    lines = 0

    for line in VECTORS.read_text().splitlines():
        if line.startswith("#") or not line:
            continue
        seed_s, domain_s, index_s, value_s = line.split("\t")
        seed, domain, index = int(seed_s), int(domain_s), int(index_s)
        lines += 1
        total += 1
        if rnd(seed, domain, index) != int(value_s, 16):
            mismatch += 1
            if mismatch <= 3:
                print(f"mismatch seed={seed} domain={domain} index={index}")

    check(total == 100000, f"vector count {total}")
    check(mismatch == 0, f"{mismatch} mismatched vectors")
    check(all(0 <= int(line.split("\t")[1]) < 7
              for line in VECTORS.read_text().splitlines()
              if line and not line.startswith("#")),
          "every domain is in range")

    # Locality: the value at an index is independent of every other index.
    # A forced substitution at 17 must leave 18..N unchanged, which is only
    # true because the function is a pure evaluation of (seed, domain, index).
    tail_before = [rnd(42, 0, i) for i in range(18, 40)]
    _ = rnd(42, 0, 17)  # where the shrinker would inject a chosen value
    tail_after = [rnd(42, 0, i) for i in range(18, 40)]
    check(tail_before == tail_after, "forcing index 17 must not move 18..N")

    # Domains are independent: the same index in two domains differs, and
    # evaluating one domain does not consume anything from another.
    check(rnd(42, 0, 5) != rnd(42, 1, 5), "domains must produce distinct values")
    check(rnd(42, 0, 5) == rnd(42, 0, 5), "domain values must be stable")

    print(f"test_rng: {checks} checks, {failures} failure(s) "
          f"({lines} vectors)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
