#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""P0-06 gate: prove the determinism bootstrap.

Checks, in order of strength:

  1. Same seed, 200 separate process launches -> one digest and one memory-map
     fingerprint. This is the only check that can catch ASLR or layout drift.
  2. 100 seeds -> at least 95 distinct digests. Identical-but-broken runs are
     easy; this catches a PRNG that collapsed to a constant.
  3. Cross-language vectors: the C implementation and this Python one must
     agree bit-for-bit, because the shrinker will use the Python copy.

Exit code 0 only if every check passes.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

MASK = (1 << 64) - 1
GOLDEN_GAMMA = 0x9E3779B97F4A7C15
MIX_A = 0xBF58476D1CE4E5B9
MIX_B = 0x94D049BB133111EB


def splitmix64(x: int) -> int:
    x = (x + GOLDEN_GAMMA) & MASK
    z = x
    z = ((z ^ (z >> 30)) * MIX_A) & MASK
    z = ((z ^ (z >> 27)) * MIX_B) & MASK
    return (z ^ (z >> 31)) & MASK


def rnd(seed: int, domain: int, index: int) -> int:
    return splitmix64(seed ^ ((domain << 40) & MASK) ^ index)


def run(binary: Path, *args: str, env: dict[str, str] | None = None) -> dict[str, str]:
    merged = None
    if env is not None:
        merged = {**os.environ, **env}
    out = subprocess.run(
        [str(binary), *args], capture_output=True, text=True, check=False, env=merged
    )
    if out.returncode != 0:
        raise RuntimeError(
            f"{binary} {' '.join(args)} exited {out.returncode}\n{out.stderr}"
        )
    fields: dict[str, str] = {}
    for line in out.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            fields[key] = value
    return fields


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="build/spike_determinism")
    ap.add_argument("--trials", type=int, default=200)
    ap.add_argument("--seeds", type=int, default=100)
    ap.add_argument("--vectors", type=int, default=5000)
    ap.add_argument(
        "--allow-no-aslr",
        action="store_true",
        help="treat a missing ADDR_NO_RANDOMIZE bit as a warning (restricted containers)",
    )
    args = ap.parse_args()

    binary = Path(args.binary)
    if not binary.is_file():
        print(f"error: {binary} not found (run make build)")
        return 2

    failures = 0
    started = time.monotonic()

    # 1. same-seed stability across launches
    digests: set[str] = set()
    layouts: set[str] = set()
    aslr_states: set[str] = set()
    for _ in range(args.trials):
        f = run(binary, "--seed", "42")
        digests.add(f["DIGEST"])
        layouts.add(f["MAPS"])
        aslr_states.add(f["ASLR"])

    ok = len(digests) == 1 and len(layouts) == 1
    print(
        f"[{'PASS' if ok else 'FAIL'}] same seed: {args.trials} runs -> "
        f"{len(digests)} digest(s), {len(layouts)} layout(s)"
    )
    failures += 0 if ok else 1

    # An unreadable /proc/self/maps would report a constant 0 and look like a
    # perfect result, so reject the sentinel explicitly.
    if layouts == {"0000000000000000"}:
        print("[FAIL] layout fingerprint is all zeros (is /proc/self/maps readable?)")
        failures += 1

    if aslr_states == {"off"}:
        print("[PASS] ASLR disabled and verified via /proc/self/personality")
    else:
        msg = f"[FAIL] ASLR not disabled (observed: {sorted(aslr_states)})"
        if args.allow_no_aslr:
            print(msg + " - allowed by --allow-no-aslr, FR-4.5 NOT established")
        else:
            print(msg)
            failures += 1

    # 2. negative control: the layout check must be able to see variation.
    # Without this, "1 layout" could simply mean the fingerprint is blind.
    try:
        randomize = int(Path("/proc/sys/kernel/randomize_va_space").read_text().strip())
    except (OSError, ValueError):
        randomize = -1

    if randomize == 0:
        print("[SKIP] negative control: system ASLR is disabled, cannot prove sensitivity")
    else:
        a = run(binary, "--seed", "42", env={"PERMUTE_NO_ASLR_REEXEC": "1"})["MAPS"]
        b = run(binary, "--seed", "42", env={"PERMUTE_NO_ASLR_REEXEC": "1"})["MAPS"]
        ok = a != b
        print(
            f"[{'PASS' if ok else 'FAIL'}] negative control: layouts differ with ASLR on "
            f"({a} vs {b})"
        )
        failures += 0 if ok else 1

    # 3. seed spread
    seed_digests = set()
    for seed in range(args.seeds):
        seed_digests.add(run(binary, "--seed", str(seed))["DIGEST"])
    needed = max(1, int(args.seeds * 0.95))
    ok = len(seed_digests) >= needed
    print(
        f"[{'PASS' if ok else 'FAIL'}] seed spread: {len(seed_digests)} distinct "
        f"digests over {args.seeds} seeds (need >= {needed})"
    )
    failures += 0 if ok else 1

    # 4. cross-language vectors
    out = subprocess.run(
        [str(binary), "--vectors", str(args.vectors)],
        capture_output=True,
        text=True,
        check=False,
    )
    if out.returncode != 0:
        print(f"[FAIL] vector generation exited {out.returncode}")
        return 1

    mismatches = 0
    checked = 0
    for line in out.stdout.splitlines():
        seed_s, domain_s, index_s, value_s = line.split(",")
        expected = rnd(int(seed_s), int(domain_s), int(index_s))
        checked += 1
        if int(value_s, 16) != expected:
            mismatches += 1
            if mismatches <= 3:
                print(
                    f"  mismatch seed={seed_s} domain={domain_s} "
                    f"index={index_s}: C={value_s} py={expected:016x}"
                )

    ok = mismatches == 0 and checked == args.vectors
    print(
        f"[{'PASS' if ok else 'FAIL'}] cross-language: {checked - mismatches}/"
        f"{checked} vectors agree between C and Python"
    )
    failures += 0 if ok else 1

    elapsed = time.monotonic() - started
    print(f"\n{'PASSED' if failures == 0 else 'FAILED'} in {elapsed:.1f}s")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
