#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Vendor-duplicate detection (FR-2.6).
#
# A vendor archive that defines an interposed symbol must make the link fail,
# and the diagnostic must name both objects. Anything else — a successful
# link, or an error that names only one side — is a failure of the gate.
#
#   vendor_dup_check.sh [build-dir] [cc]

set -u

build="${1:-build}"
cc_bin="${2:-${CC:-cc}}"
out="${TMPDIR:-/tmp}/permute_vendor_dup.$$"
err="${out}.err"

if [ ! -f "$build/libvendor_dup.a" ] || [ ! -f "$build/libpermute_guest.a" ]; then
    echo "vendor-dup: FAIL (missing archives in $build)"
    exit 1
fi

if "$cc_bin" -I. tests/fixtures/vendor_dup/probe.c \
        -L"$build" -lvendor_dup -lpermute_guest -o "$out" 2>"$err"; then
    echo "vendor-dup: FAIL (link succeeded; duplicate was not detected)"
    rm -f "$out" "$err"
    exit 1
fi

if grep -q "shim_ipc.c.o" "$err" && grep -q "vendor.c.o" "$err"; then
    echo "vendor-dup: PASSED (linker named both objects)"
    rm -f "$out" "$err"
    exit 0
fi

echo "vendor-dup: FAIL (diagnostic did not name both objects)"
cat "$err"
rm -f "$out" "$err"
exit 1
