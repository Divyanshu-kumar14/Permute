#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# G2 scorecard (PRD §13.3). Each row runs the check that produces the
# evidence, records the measured value, and prints PASS/FAIL. Logs go to
# build/gate2/ so a failure can be read without rerunning.
#
# The gate is deliberately not all-or-nothing on things the repository cannot
# prove: outcome-class matching needs all twelve mandatory scenarios executed,
# and C11/C13 are skipped for documented reasons. The row fails; the reason is
# printed instead of being redefined away.
set -u

cd "$(dirname "$0")/.."
REPO=$(pwd)

LOG_DIR=$REPO/build/gate2
mkdir -p "$LOG_DIR" gate
SCORE=gate/gate2-scorecard.md
FAILED=0
ROWS=()

record() {
    local name="$1" threshold="$2" measured="$3" verdict="$4"
    ROWS+=("| $name | $threshold | $measured | $verdict |")
    if [ "$verdict" != "PASS" ]; then
        FAILED=1
    fi
    printf '%-40s %-24s %-38s %s\n' "$name" "$threshold" "$measured" "$verdict"
}

tail_on_fail() {
    local log="$1"
    echo "  --- $log (tail) ---"
    tail -n 8 "$log" | sed 's/^/  /'
}

le() {
    awk -v value="$1" -v limit="$2" 'BEGIN { exit !(value <= limit) }'
}

# --- 1. Conformance equivalence (G2: >= 10/12) ----------------------------
if make conformance >"$LOG_DIR/conformance.log" 2>&1; then
    line=$(grep -E '^conformance: ' "$LOG_DIR/conformance.log" | tail -1)
    executed=$(printf '%s' "$line" | sed -n 's/^conformance: \([0-9]*\) executed.*/\1/p')
    skipped=$(printf '%s' "$line" | sed -n 's/.*, \([0-9]*\) skipped.*/\1/p')
    failures=$(printf '%s' "$line" | sed -n 's/.* \([0-9]*\) failure(s).*/\1/p')
    mandatory=$(printf '%s' "$line" | sed -n 's/.*mandatory \([0-9]*\)\/12 executed.*/\1/p')
    ok=$(awk -F'|' '$4 ~ /^ *ok *$/ { n++ } END { print n + 0 }' \
         conformance/REPORT.md)

    if [ "${failures:-1}" -eq 0 ] && [ "${mandatory:-0}" -ge 10 ] \
       && [ "${ok:-0}" -ge 10 ]; then
        record "Conformance equivalence" ">= 10/12" \
               "${mandatory:-0}/12 mandatory equivalent (${ok:-0} executed, ${failures:-?} failures)" PASS
    else
        record "Conformance equivalence" ">= 10/12" \
               "${ok:-0}/12 equivalent, ${failures:-?} failures, ${mandatory:-?}/12 executed" FAIL
        tail_on_fail "$LOG_DIR/conformance.log"
    fi
else
    record "Conformance equivalence" ">= 10/12" "make conformance failed" FAIL
    tail_on_fail "$LOG_DIR/conformance.log"
    executed=0 skipped=0 failures=1 mandatory=0
fi

# --- 2. Outcome class (G2: 12/12) -----------------------------------------
if [ "${mandatory:-0}" -eq 12 ] && [ "${failures:-1}" -eq 0 ]; then
    record "Outcome-class match" "12/12 mandatory" \
           "12/12 executed, 0 wrong" PASS
else
    record "Outcome-class match" "12/12 mandatory" \
           "${mandatory:-0}/12 executed, ${failures:-?} wrong, ${skipped:-?} skipped" FAIL
    echo "  blocked: C11 (multi-process) and C13 (mutex shim not modelled);"
    echo "  see LIMITS #60 and conformance/scenarios.toml"
fi

# --- 3. Mutation score (G2: >= 10/12) -------------------------------------
if make mutation >"$LOG_DIR/mutation.log" 2>&1; then
    line=$(grep -oE 'mutation score: [0-9]+/[0-9]+ killed' \
           "$LOG_DIR/mutation.log" | tail -1)
    killed=$(printf '%s' "$line" | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f1)
    total=$(printf '%s' "$line" | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f2)
    if [ "${killed:-0}" -ge 10 ] && [ "${total:-0}" -eq 12 ]; then
        record "Mutation score" ">= 10/12" \
               "${killed:-0}/${total:-0} killed" PASS
    else
        record "Mutation score" ">= 10/12" "${line:-no summary}" FAIL
        tail_on_fail "$LOG_DIR/mutation.log"
    fi
else
    record "Mutation score" ">= 10/12" "make mutation failed" FAIL
    tail_on_fail "$LOG_DIR/mutation.log"
fi

# --- 4. Bug benchmark (G2: 6/6) -------------------------------------------
if make bench-bugs >"$LOG_DIR/bench-bugs.log" 2>&1; then
    line=$(grep -oE 'bug benchmark: [0-9]+/[0-9]+ found, [0-9]+ false positive' \
           "$LOG_DIR/bench-bugs.log" | tail -1)
    found=$(printf '%s' "$line" | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f1)
    total=$(printf '%s' "$line" | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f2)
    fps=$(printf '%s' "$line" | grep -oE '[0-9]+ false' | grep -oE '[0-9]+')
    if [ "${found:-0}" -eq 6 ] && [ "${total:-0}" -eq 6 ] \
       && [ "${fps:-1}" -eq 0 ]; then
        record "Bug benchmark" "6/6, 0 false positives" \
               "${found:-0}/${total:-0} found, ${fps:-?} FP" PASS
    else
        record "Bug benchmark" "6/6, 0 false positives" "${line:-no summary}" FAIL
        tail_on_fail "$LOG_DIR/bench-bugs.log"
    fi
else
    record "Bug benchmark" "6/6, 0 false positives" "make bench-bugs failed" FAIL
    tail_on_fail "$LOG_DIR/bench-bugs.log"
fi

# --- 5. Shrinker (G2: p95 <= 30 s, reduced repro <= 20 events) ------------
rm -f bench/shrink-results.md
if make shrink-bench >"$LOG_DIR/shrink-bench.log" 2>&1; then
    line=$(grep -E '^shrink-bench: ' "$LOG_DIR/shrink-bench.log" | tail -1)
    p95=$(printf '%s' "$line" | sed -n 's/.*p95 \([0-9.]*\) s.*/\1/p')
    if [ -n "${p95:-}" ] && le "$p95" 30; then
        record "Shrink p95 (20 failures)" "<= 30 s" "${p95} s" PASS
    else
        record "Shrink p95 (20 failures)" "<= 30 s" "${p95:-?} s" FAIL
        tail_on_fail "$LOG_DIR/shrink-bench.log"
    fi

    events=$(sed -n 's/^| Max reduced events |[^|]*|[[:space:]]*\([0-9]*\).*/\1/p' \
             bench/shrink-results.md 2>/dev/null | tail -1)
    if [ -n "${events:-}" ] && [ "$events" -le 20 ]; then
        record "Shrink reduced repro" "<= 20 events" "${events} events (max)" PASS
    else
        record "Shrink reduced repro" "<= 20 events" "${events:-?} events" FAIL
    fi
else
    record "Shrink p95 (20 failures)" "<= 30 s" "make shrink-bench failed" FAIL
    record "Shrink reduced repro" "<= 20 events" "make shrink-bench failed" FAIL
    tail_on_fail "$LOG_DIR/shrink-bench.log"
fi

# --- 6. NFR-2..6: bench suite --------------------------------------------
rm -f bench/results.md
if make bench >"$LOG_DIR/bench.log" 2>&1; then
    bench_ok=1
else
    bench_ok=0
fi
if [ -f bench/results.md ]; then
    while IFS='|' read -r _ name threshold measured verdict _; do
        name=$(printf '%s' "$name" | sed 's/^ *//; s/ *$//')
        threshold=$(printf '%s' "$threshold" | sed 's/^ *//; s/ *$//')
        measured=$(printf '%s' "$measured" | sed 's/^ *//; s/ *$//')
        verdict=$(printf '%s' "$verdict" | sed 's/^ *//; s/ *$//')
        case "$name" in
            "Seed throughput per worker") id="NFR-2 seed throughput" ;;
            "Kernel call (non-blocking)") id="NFR-3 kernel call cost" ;;
            "Cooperative switch") id="NFR-4 context switch" ;;
            "20,000 seeds wall") id="NFR-5 20,000 seeds" ;;
            "Worker + SUT RSS (4 threads)") id="NFR-6 memory per worker" ;;
            "Message throughput") id="G1 message throughput" ;;
            *) continue ;;
        esac
        record "$id" "$threshold" "$measured" "$verdict"
    done < <(grep -E '^\| (Seed throughput per worker|Kernel call \(non-blocking\)|Cooperative switch|20,000 seeds wall|Worker \+ SUT RSS \(4 threads\)|Message throughput) ' bench/results.md)
else
    record "NFR-2..6 bench suite" "6/6 criteria" "make bench wrote no results" FAIL
    tail_on_fail "$LOG_DIR/bench.log"
fi
if [ "$bench_ok" -eq 0 ] && [ -f bench/results.md ]; then
    echo "  note: make bench exited non-zero; see the failing rows above"
fi

# --- 7. NFR-12: startup latency ------------------------------------------
measure_ms() {
    local start end
    start=$(date +%s%N)
    "$@" >/dev/null 2>&1
    end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

version_ms=0
for _ in 1 2 3; do
    ms=$(measure_ms "$REPO/tools/permute" --version)
    [ "$ms" -gt "$version_ms" ] && version_ms=$ms
done

project="$REPO/build/gate2/startup"
cmake -E remove_directory "$project" >/dev/null 2>&1 || true
if "$REPO/tools/permute" init "$project" >"$LOG_DIR/startup.log" 2>&1 \
   && ( cd "$project" && "$REPO/tools/permute" build >>"$LOG_DIR/startup.log" 2>&1 ); then
    run_ms=0
    for _ in 1 2; do
        start=$(date +%s%N)
        ( cd "$project" && "$REPO/tools/permute" run --seeds 1 >>"$LOG_DIR/startup.log" 2>&1 )
        end=$(date +%s%N)
        ms=$(( (end - start) / 1000000 ))
        [ "$ms" -gt "$run_ms" ] && run_ms=$ms
    done
    if [ "$version_ms" -le 200 ] && [ "$run_ms" -le 2000 ]; then
        record "NFR-12 startup" "<= 200 ms; <= 2 s" \
               "version ${version_ms} ms; run ${run_ms} ms" PASS
    else
        record "NFR-12 startup" "<= 200 ms; <= 2 s" \
               "version ${version_ms} ms; run ${run_ms} ms" FAIL
    fi
else
    record "NFR-12 startup" "<= 200 ms; <= 2 s" "init or build failed" FAIL
    tail_on_fail "$LOG_DIR/startup.log"
fi

# --- Scorecard ------------------------------------------------------------
{
    echo "<!-- SPDX-License-Identifier: Apache-2.0 -->"
    echo
    echo "# G2 scorecard"
    echo
    echo "Generated by \`make gate2\` on $(date -u +%Y-%m-%d)."
    echo
    echo "| Criterion | Threshold | Measured | Verdict |"
    echo "|---|---|---|---|"
    for row in "${ROWS[@]}"; do
        echo "$row"
    done
    echo
    if [ "$FAILED" -eq 0 ]; then
        echo "**Result: PASS** — all criteria met."
    else
        echo "**Result: FAIL** — see \`build/gate2/\` for the failing command."
    fi
    echo
    echo "> Conformance is evaluated against committed simulator references."
    echo "> The separate QNX 8.0 build and target run is committed in"
    echo "> \`docs/evidence/qnx/\` (14/14 \`rc=0\`). It validates each scenario's"
    echo "> own assertions, not trace equivalence; oracle trace capture is not"
    echo "> executed. C11 and C13 are mandatory scenarios that do not execute"
    echo "> in this simulator yet, so outcome-class matching is not claimed."
} >"$SCORE"

echo
echo "scorecard: $SCORE"
[ "$FAILED" -eq 0 ]
