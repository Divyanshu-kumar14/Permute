#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Symbol audit. The .def file is the single source of truth: if the archive does
# not define a strong symbol for every entry, the build is lying about coverage.

set -eu

def="abi/permute_iface.def"
lib="${1:-build/libpermute_guest.a}"

[ -f "$def" ] || { echo "symcheck: missing $def"; exit 1; }
[ -f "$lib" ] || { echo "symcheck: missing $lib (run make build)"; exit 1; }

expected=$(sed -nE 's/^PERMUTE_SYMBOL\(([^,]+),[^,]+,[[:space:]]*([A-Za-z_][A-Za-z0-9_]*),.*/\2/p' "$def" | sort)

if [ -z "$expected" ]; then
    echo "symcheck: no PERMUTE_SYMBOL entries found in $def"
    exit 1
fi

defined=$(nm -g --defined-only "$lib" | awk '$2 == "T" || $2 == "W" { print $3 }' | sort)

fail=0
for sym in $expected; do
    if ! printf '%s\n' "$defined" | grep -qx "$sym"; then
        echo "symcheck: MISSING strong definition in $lib: $sym"
        fail=1
    fi
done

bin="build/spike_shadow"
if [ -f "$bin" ]; then
    undef=$(nm -u "$bin" | grep -E 'ChannelCreate|ConnectAttach|MsgSend|MsgReceive|MsgReply' || true)
    if [ -n "$undef" ]; then
        echo "symcheck: symbols still undefined in $bin:"
        printf '%s\n' "$undef"
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "symcheck: FAILED"
    exit 1
fi

count=$(printf '%s\n' "$expected" | wc -l | tr -d ' ')
echo "symcheck: PASSED ($count symbol(s) checked)"
