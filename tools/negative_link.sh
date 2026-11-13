#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Negative control for the shadowing claim.
#
# A positive test ("the program prints PASS") can be satisfied by accident.
# The real proof is that the SAME program fails to link without our archive:
# that is what shows the symbol came from us and not from somewhere else.

set -eu

CC="${CC:-cc}"
out=$(mktemp)
err=$(mktemp)
trap 'rm -f "$out" "$err"' EXIT

if "$CC" -std=c11 -D_GNU_SOURCE -I. tests/spike_shadow.c -o "$out" 2>"$err"; then
    echo "FAIL: linked without libpermute_guest.a; shadowing is not proven"
    exit 1
fi

if ! grep -q 'MsgSend' "$err"; then
    echo "FAIL: link failed, but not because of MsgSend:"
    cat "$err"
    exit 1
fi

echo "PASS: link without libpermute_guest.a fails on an undefined MsgSend"
