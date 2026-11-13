# SPDX-License-Identifier: Apache-2.0
"""Interposed-allocator probe: determinism and the RSS ceiling.

The probe process replaces malloc/free for the whole program, so this checks
the build a guest actually gets, not just the core API.
"""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PROBE = REPO / "build" / "alloc_probe"

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def run(ops=200000, seed=1) -> dict:
    out = subprocess.run([str(PROBE), str(ops), str(seed)],
                         capture_output=True, text=True, check=True)
    fields = {}
    for line in out.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            fields[key] = value
    return fields


def main():
    if not PROBE.exists():
        raise SystemExit(f"missing {PROBE}; run make build")

    a = run(200000, 1)
    b = run(200000, 1)
    c = run(200000, 2)

    check(a["DIGEST"] == b["DIGEST"], f"same seed digest {a['DIGEST']} != {b['DIGEST']}")
    check(a["HIGH_WATER"] == b["HIGH_WATER"], "same seed high-water")
    check(a["LIVE"] == b["LIVE"], "same seed live blocks")
    check(a["DIGEST"] != c["DIGEST"], "different seeds must differ")

    # Address-range dispatch: a pointer from libc is not ours and must be
    # forwarded rather than corrupted. The probe frees it; reaching here at
    # all means the process survived.
    check(a["OWNS_LIBC"] == "0", f"libc pointer claimed as arena memory: {a['OWNS_LIBC']}")

    rss_kb = int(a["RSS_KB"])
    high_water_kb = int(a["HIGH_WATER"]) // 1024
    print(f"alloc probe: {rss_kb} KiB RSS, {high_water_kb} KiB arena high-water")
    check(rss_kb <= 70 * 1024, f"RSS {rss_kb} KiB exceeds 70 MiB")

    print(f"test_alloc_probe: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
