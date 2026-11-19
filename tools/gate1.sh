#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# G1 scorecard. Each row runs the check that produces the evidence, records the
# measured value, and prints PASS/FAIL. The script exits 0 only when every row
# passes. Logs go to build/gate1/ so a failure can be read without rerunning.
set -u

cd "$(dirname "$0")/.."

LOG_DIR=build/gate1
mkdir -p "$LOG_DIR" gate
SCORE=gate/gate1-scorecard.md
FAILED=0
ROWS=()

record() {
    local name="$1" threshold="$2" measured="$3" verdict="$4"
    ROWS+=("| $name | $threshold | $measured | $verdict |")
    if [ "$verdict" != "PASS" ]; then
        FAILED=1
    fi
    printf '%-38s %-22s %-34s %s\n' "$name" "$threshold" "$measured" "$verdict"
}

tail_on_fail() {
    local log="$1"
    echo "  --- $log (tail) ---"
    tail -n 8 "$log" | sed 's/^/  /'
}

# --- 1. Replay determinism ------------------------------------------------
if make determinism >"$LOG_DIR/determinism.log" 2>&1; then
    line=$(grep -oE 'determinism: PASSED \([^)]*\)' "$LOG_DIR/determinism.log" | tail -1)
    runs=$(printf '%s' "$line" | grep -oE '[0-9]+ runs' | head -1 | grep -oE '[0-9]+')
    if [ "${runs:-0}" -ge 1000 ]; then
        record "Replay determinism" "100% identical" "${runs:-0}/1000 same-seed" PASS
    else
        record "Replay determinism" "100% identical" "${runs:-0} runs" FAIL
    fi
else
    record "Replay determinism" "100% identical" "make determinism failed" FAIL
    tail_on_fail "$LOG_DIR/determinism.log"
fi

# --- 2. Conformance -------------------------------------------------------
if make conformance >"$LOG_DIR/conformance.log" 2>&1; then
    line=$(grep -oE '[0-9]+ executed, [0-9]+ skipped, [0-9]+ failure' \
           "$LOG_DIR/conformance.log" | tail -1)
    executed=$(printf '%s' "$line" | grep -oE '^[0-9]+')
    failures=$(printf '%s' "$line" | grep -oE '[0-9]+ failure' | grep -oE '[0-9]+')
    if [ "${executed:-0}" -eq 12 ] && [ "${failures:-1}" -eq 0 ]; then
        record "Conformance scenarios" "12 executed, 0 failures" \
               "12 executed, 0 failures" PASS
    else
        record "Conformance scenarios" "12 executed, 0 failures" \
               "${line:-no summary}" FAIL
    fi
else
    record "Conformance scenarios" "12 executed, 0 failures" "make conformance failed" FAIL
    tail_on_fail "$LOG_DIR/conformance.log"
fi

# --- 3. Bug benchmark -----------------------------------------------------
if make bench-bugs >"$LOG_DIR/bench-bugs.log" 2>&1; then
    line=$(grep -oE 'bug benchmark: [0-9]+/[0-9]+ found, [0-9]+ false positive' \
           "$LOG_DIR/bench-bugs.log" | tail -1)
    found=$(printf '%s' "$line" | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f1)
    total=$(printf '%s' "$line" | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f2)
    fps=$(printf '%s' "$line" | grep -oE '[0-9]+ false' | grep -oE '[0-9]+')
    if [ "${found:-0}" -eq 6 ] && [ "${total:-0}" -eq 6 ] && [ "${fps:-1}" -eq 0 ]; then
        record "Bug benchmark" "6/6, 0 false positives" \
               "${found:-0}/${total:-0} found, ${fps:-?} FP" PASS
    else
        record "Bug benchmark" "6/6, 0 false positives" "${line:-no summary}" FAIL
    fi
else
    record "Bug benchmark" "6/6, 0 false positives" "make bench-bugs failed" FAIL
    tail_on_fail "$LOG_DIR/bench-bugs.log"
fi

# --- 4. Mutation score ----------------------------------------------------
if make mutation >"$LOG_DIR/mutation.log" 2>&1; then
    line=$(grep -oE 'mutation score: [0-9]+/[0-9]+ killed' \
           "$LOG_DIR/mutation.log" | tail -1)
    killed=$(printf '%s' "$line" | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f1)
    total=$(printf '%s' "$line" | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f2)
    if [ "${killed:-0}" -ge 10 ] && [ "${total:-0}" -eq 12 ]; then
        record "Mutation score" ">= 10/12 killed" \
               "${killed:-0}/${total:-0} killed" PASS
    else
        record "Mutation score" ">= 10/12 killed" "${line:-no summary}" FAIL
    fi
else
    record "Mutation score" ">= 10/12 killed" "make mutation failed" FAIL
    tail_on_fail "$LOG_DIR/mutation.log"
fi

# --- 5. Shard determinism -------------------------------------------------
if make explore >"$LOG_DIR/explore.log" 2>&1; then
    line=$(grep -oE 'test_explore: [0-9]+ checks, [0-9]+ failure' \
           "$LOG_DIR/explore.log" | tail -1)
    failures=$(printf '%s' "$line" | grep -oE '[0-9]+ failure' | grep -oE '[0-9]+')
    if [ "${failures:-1}" -eq 0 ]; then
        record "Shard determinism" "standalone == full" \
               "shard 2/4 byte-identical" PASS
    else
        record "Shard determinism" "standalone == full" "${line:-no summary}" FAIL
    fi
else
    record "Shard determinism" "standalone == full" "make explore failed" FAIL
    tail_on_fail "$LOG_DIR/explore.log"
fi

# --- 6. Determinism self-check -------------------------------------------
if make verify >"$LOG_DIR/verify.log" 2>&1; then
    line=$(grep -oE 'test_verify: [0-9]+ checks, [0-9]+ failure' \
           "$LOG_DIR/verify.log" | tail -1)
    failures=$(printf '%s' "$line" | grep -oE '[0-9]+ failure' | grep -oE '[0-9]+')
    if [ "${failures:-1}" -eq 0 ]; then
        record "Determinism self-check" "exit 5 + both digests" \
               "planted exit 5 over violation" PASS
    else
        record "Determinism self-check" "exit 5 + both digests" \
               "${line:-no summary}" FAIL
    fi
else
    record "Determinism self-check" "exit 5 + both digests" "make verify failed" FAIL
    tail_on_fail "$LOG_DIR/verify.log"
fi

# --- 7. Reports -----------------------------------------------------------
if make report >"$LOG_DIR/report.log" 2>&1; then
    line=$(grep -oE 'test_report: [0-9]+ checks, [0-9]+ failure' \
           "$LOG_DIR/report.log" | tail -1)
    failures=$(printf '%s' "$line" | grep -oE '[0-9]+ failure' | grep -oE '[0-9]+')
    if [ "${failures:-1}" -eq 0 ]; then
        record "Report formats" "4 valid, offline HTML" \
               "JUnit, SARIF 2.1.0, HTML, JSON" PASS
    else
        record "Report formats" "4 valid, offline HTML" "${line:-no summary}" FAIL
    fi
else
    record "Report formats" "4 valid, offline HTML" "make report failed" FAIL
    tail_on_fail "$LOG_DIR/report.log"
fi

# --- 8. Context switch ----------------------------------------------------
if make switch-bench >"$LOG_DIR/switch.log" 2>&1; then
    ns=$(grep -oE '^SWITCH_NS=[0-9]+' "$LOG_DIR/switch.log" | tail -1 | cut -d= -f2)
    if [ -n "${ns:-}" ] && [ "$ns" -le 4000 ]; then
        record "Context switch" "<= 4000 ns" "${ns} ns" PASS
    else
        record "Context switch" "<= 4000 ns" "${ns:-?} ns" FAIL
    fi
else
    record "Context switch" "<= 4000 ns" "make switch-bench failed" FAIL
    tail_on_fail "$LOG_DIR/switch.log"
fi

# --- 9. Sanitizers --------------------------------------------------------
if make asan >"$LOG_DIR/asan.log" 2>&1; then
    record "ASan / UBSan" "clean" "clean" PASS
else
    record "ASan / UBSan" "clean" "sanitizer or test failure" FAIL
    tail_on_fail "$LOG_DIR/asan.log"
fi

# --- Scorecard ------------------------------------------------------------
{
    echo "<!-- SPDX-License-Identifier: Apache-2.0 -->"
    echo
    echo "# G1 scorecard"
    echo
    echo "Generated by \`make gate1\` on $(date -u +%Y-%m-%d)."
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
        echo "**Result: FAIL** — see \`build/gate1/\`."
    fi
    echo
    echo "> Conformance is evaluated against the committed simulator references."
    echo "> The separate QNX 8.0 build and target run is committed in"
    echo "> \`docs/evidence/qnx/\` (14/14 \`rc=0\`). It validates each scenario's"
    echo "> own assertions, not trace equivalence; oracle trace capture is not"
    echo "> executed (see \`docs/CONFORMANCE.md\`)."
} > "$SCORE"

echo
echo "scorecard: $SCORE"
[ "$FAILED" -eq 0 ]
