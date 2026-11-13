#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Gate for P0-05.
#
# 1. Across 100 separate processes the page must land on the same address.
#    A fixed address that moves between runs is not a fixed address.
# 2. With the first candidate occupied, the probe must move to another one.
# 3. With fixed mappings disabled, the fallback must work and say so.

set -eu

bin="${1:-build/spike_hypercall}"
trials="${2:-100}"
fail=0

[ -x "$bin" ] || { echo "error: $bin not found (run make build)"; exit 2; }

# 1. stability
first=$("$bin" | sed -n 's/^HYPERCALL=//p')
if [ -z "$first" ]; then
    echo "FAIL: no HYPERCALL line from $bin"
    exit 1
fi
if [ "$first" = "fallback" ]; then
    echo "FAIL: fixed mapping unavailable; the primary path is not exercised"
    exit 1
fi

mismatch=0
i=0
while [ "$i" -lt "$trials" ]; do
    v=$("$bin" | sed -n 's/^HYPERCALL=//p')
    if [ "$v" != "$first" ]; then
        mismatch=$((mismatch + 1))
        echo "  run $i used $v, expected $first"
    fi
    i=$((i + 1))
done

if [ "$mismatch" -eq 0 ]; then
    echo "PASS: $trials runs all mapped the page at $first"
else
    echo "FAIL: $mismatch/$trials runs used a different address"
    fail=1
fi

# 2. probe actually probes
if "$bin" --occupy-first >/dev/null 2>&1; then
    echo "PASS: probe moved off an occupied candidate"
else
    echo "FAIL: probe did not recover from an occupied candidate"
    fail=1
fi

# 3. fallback
if "$bin" --fallback >/dev/null 2>&1; then
    echo "PASS: fallback path works with fixed mappings disabled"
else
    echo "FAIL: fallback path broken"
    fail=1
fi

[ "$fail" -eq 0 ] && echo "hypercall check PASSED" || echo "hypercall check FAILED"
exit "$fail"
