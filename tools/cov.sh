#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Coverage gate (NFR-15).
#
# Builds with --coverage, runs every C suite and the tools that exercise the
# log writer and the seeded scenario, then aggregates line coverage by group.
# The threshold lives in tools/cov_report.py (70%).

set -eu

cd "$(dirname "$0")/.."

BUILD="${1:-build-cov}"

if [ ! -f build/libpermute_guest.a ]; then
    echo "cov: build/libpermute_guest.a missing; run make build first" >&2
    exit 2
fi
CMAKE_BIN="${CMAKE:-cmake}"
OUT="${TMPDIR:-/tmp}/permute-cov.$$"
mkdir -p "$OUT"

"$CMAKE_BIN" -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="--coverage -O0 -g"
"$CMAKE_BIN" --build "$BUILD"

# Guest fixtures are built outside CMake; they must match the coverage build.
python3 tools/make_guest.py --source tests/fixtures/guest/counter.c \
    --name counter --instance 0 --out "$BUILD" --build-dir build >/dev/null
python3 tools/make_guest.py --source tests/fixtures/guest/counter.c \
    --name counter --instance 1 --out "$BUILD" --build-dir build >/dev/null

"$BUILD/test_objects" >/dev/null
"$BUILD/test_ipc" >/dev/null
"$BUILD/test_pulse" >/dev/null
"$BUILD/test_sched" >/dev/null
"$BUILD/test_pi" >/dev/null
"$BUILD/test_time" >/dev/null
"$BUILD/test_blake3" >/dev/null
"$BUILD/test_alloc" >/dev/null
"$BUILD/test_shim" >/dev/null
"$BUILD/test_rng_golden" tests/golden/rng_vectors.tsv >/dev/null
"$BUILD/test_lease" >/dev/null
"$BUILD/test_fault" >/dev/null
"$BUILD/test_guest" "$BUILD/proc_counter__0.so" \
    "$BUILD/proc_counter__1.so" >/dev/null

# Tools exercise the writers and the seeded scenario, which unit tests do not.
"$BUILD/trace_gen" "$OUT/golden.pmt" golden >/dev/null
"$BUILD/replay_fixture" 42 "$OUT/seed-42.pmt" >/dev/null
"$BUILD/permute-run" 7 "$OUT/seed-7.pmt" >/dev/null

python3 tools/cov_report.py "$BUILD" | tee "$BUILD/coverage.txt"
