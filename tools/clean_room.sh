#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Clean-machine dry run (P4-03): build, test, run the examples, install,
# exercise the installed CLI and uninstall from a source tarball, inside a
# fresh network namespace.
#
#   tools/clean_room.sh [TARBALL]
#
# With no argument a tarball is exported from HEAD. A dirty tree is refused:
# the result has to name a commit. The namespace is the offline proof — a step
# that reached for the network would fail instead of quietly succeeding.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${PERMUTE_CLEAN_DIR:-/tmp/opencode/permute-clean-$(date -u +%Y%m%dT%H%M%SZ)}
LIMIT_S=${PERMUTE_CLEAN_LIMIT:-900}

if ! unshare -rn true 2>/dev/null; then
    echo "clean-room: unshare -rn is unavailable; offline operation cannot be proven" >&2
    exit 2
fi

mkdir -p "$WORK/src"

if [ "$#" -ge 1 ]; then
    TAR=$1
    [ -f "$TAR" ] || { echo "clean-room: no tarball at $TAR" >&2; exit 2; }
else
    if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
        echo "clean-room: working tree is dirty; commit first or pass a tarball" >&2
        exit 2
    fi
    TAR=$WORK/permute.tar.gz
    git -C "$ROOT" archive --format=tar.gz --prefix=permute/ -o "$TAR" HEAD
fi

tar -xzf "$TAR" -C "$WORK/src"
SRC=$(find "$WORK/src" -mindepth 1 -maxdepth 1 -type d | head -1)
[ -n "$SRC" ] || { echo "clean-room: tarball has no directory" >&2; exit 2; }

cat >"$WORK/run.sh" <<'INNER'
set -euo pipefail
src=$1
work=$2
prefix=$work/prefix
timings=$work/timings.txt
: >"$timings"

phase() {
    local name=$1
    shift
    local start end
    start=$(date +%s%N)
    "$@" >"$work/$name.log" 2>&1
    end=$(date +%s%N)
    printf '%s %s\n' "$name" "$(( (end - start) / 1000000 ))" >>"$timings"
    printf '  %-10s %6d ms\n' "$name" "$(( (end - start) / 1000000 ))"
}

cd "$src"
echo "source:   $src"
echo "commit:   $(sed -n '/^#/d; /^[[:space:]]*$/d; p; q' permute-version.txt 2>/dev/null || echo unknown)"

# Offline proof: inside the namespace a TCP connect and DNS lookup must fail.
# A machine that silently has network access would invalidate the run, so this
# is an assertion, not decoration.
if python3 - <<'PY'
import socket, sys
for host, port in (("1.1.1.1", 80), ("example.com", 80)):
    try:
        socket.create_connection((host, port), timeout=2).close()
    except OSError:
        continue
    sys.exit(1)
sys.exit(0)
PY
then
    echo "offline:  no outbound TCP connection is possible"
else
    echo "clean-room: the namespace still reaches the network" >&2
    exit 1
fi
echo "routes:   $(ip route 2>/dev/null | wc -l) route line(s)"
echo

phase build make build
phase ci make ci
phase examples make examples
phase install make install PREFIX="$prefix"
phase version "$prefix/bin/permute" --version
phase project sh -c "\"$prefix/bin/permute\" init \"$work/demo\" >/dev/null &&
    cd \"$work/demo\" &&
    \"$prefix/bin/permute\" build >/dev/null &&
    \"$prefix/bin/permute\" run >/dev/null"

installed=$(find "$prefix" -type f | wc -l)
"$prefix/bin/permute" --version >"$work/version.txt"
phase uninstall make uninstall PREFIX="$prefix"
residue=$( { find "$prefix" -type f 2>/dev/null || true; } | wc -l )
if [ -e "$prefix" ]; then
    echo "clean-room: uninstall left $prefix behind" >&2
    exit 1
fi
if [ "$residue" -ne 0 ]; then
    echo "clean-room: $residue file(s) survived uninstall" >&2
    exit 1
fi

total=$(awk '{ sum += $2 } END { print sum }' "$timings")
echo
echo "installed files: $installed; uninstall residue: $residue"
echo "version: $(cat "$work/version.txt")"
echo "total: ${total} ms"
printf '%s\n' "$total" >"$work/total_ms.txt"
INNER

echo "clean-room: $WORK"
echo

start=$(date +%s%N)
set +e
PERMUTE_WORK=$WORK unshare -rn bash "$WORK/run.sh" "$SRC" "$WORK"
inner_rc=$?
set -e
end=$(date +%s%N)
wall_ms=$(( (end - start) / 1000000 ))

echo
if [ "$inner_rc" -ne 0 ]; then
    echo "clean-room: FAIL (inner flow exited $inner_rc after ${wall_ms} ms)" >&2
    if [ -f "$WORK/timings.txt" ]; then
        awk '{ printf "  %-10s %6d ms\n", $1, $2 }' "$WORK/timings.txt" >&2
    fi
    echo "logs: $WORK" >&2
    exit "$inner_rc"
fi

total=$(cat "$WORK/total_ms.txt")
if [ "$total" -le $(( LIMIT_S * 1000 )) ]; then
    echo "clean-room: PASSED (${total} ms inside the namespace, ${wall_ms} ms wall, limit ${LIMIT_S} s)"
else
    echo "clean-room: FAIL (${total} ms inside the namespace, limit ${LIMIT_S} s)" >&2
    exit 1
fi
echo "logs: $WORK"
