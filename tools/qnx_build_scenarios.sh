#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Cross-build every conformance scenario for QNX 8.0 x86_64 with the vendor
# toolchain. Requires SDP 8.0 sourced from QNX_SDP (default ~/qnx800).
#
# Usage: tools/qnx_build_scenarios.sh [output-dir]
# Default output-dir: build/qnx

set -euo pipefail

QNX_SDP=${QNX_SDP:-$HOME/qnx800}
out=${1:-build/qnx}

[ -f "$QNX_SDP/qnxsdp-env.sh" ] || {
    echo "error: no qnxsdp-env.sh under $QNX_SDP (set QNX_SDP)" >&2
    exit 1
}

# qnxsdp-env.sh refuses to run outside bash, which is why this script is bash.
# shellcheck disable=SC1091
. "$QNX_SDP/qnxsdp-env.sh" >/dev/null

mkdir -p "$out"

echo "sdp:       $QNX_SDP"
echo "qcc:       $(command -v qcc)"
qcc -Vgcc_ntox86_64 --version 2>&1 | sed 's/^/toolchain: /'

built=0
for f in conformance/scenarios/c*.c; do
    n=$(basename "$f" .c)
    qcc -Vgcc_ntox86_64 -O2 -Wall -Wextra -I. -o "$out/$n" "$f"
    echo "built $out/$n"
    built=$((built + 1))
done

echo "scenarios: $built built, 0 failure(s)"
