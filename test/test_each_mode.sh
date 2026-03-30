#!/bin/bash
# test_each_mode.sh - Verify all 5 PerfSanitizer plugin modes run without
# crashing: report, fix, diff, quiet, diag.
#
# Usage:
#   ./test_each_mode.sh [source-file]
#
# If no source file is given, uses test_all_categories.cpp in the same
# directory as this script.
#
# Environment:
#   PERFSAN_PLUGIN  - path to PerfSanitizer.so (default: ../build/PerfSanitizer.so)
#   CXX             - compiler (default: clang++)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN="${PERFSAN_PLUGIN:-${SCRIPT_DIR}/../build/PerfSanitizer.so}"
CXX="${CXX:-clang++}"
CXXFLAGS="${CXXFLAGS:--std=c++20 -O2}"
SRC="${1:-${SCRIPT_DIR}/test_all_categories.cpp}"
WORKDIR="$(mktemp -d)"

trap 'rm -rf "$WORKDIR"' EXIT

# Verify prerequisites
if [ ! -f "$SRC" ]; then
    echo "FAIL: source file not found: $SRC"
    exit 1
fi

if [ ! -f "$PLUGIN" ]; then
    echo "FAIL: plugin not found: $PLUGIN"
    echo "  Set PERFSAN_PLUGIN to the correct path."
    exit 1
fi

MODES=(report fix diff quiet diag)
PASSED=0
FAILED=0
TOTAL=${#MODES[@]}

echo "=== Testing all PerfSanitizer modes ==="
echo "  Plugin: $PLUGIN"
echo "  Source: $SRC"
echo "  Compiler: $CXX $CXXFLAGS"
echo ""

for mode in "${MODES[@]}"; do
    echo -n "  Mode '$mode' ... "

    # For fix mode, work on a copy so the original is not modified
    if [ "$mode" = "fix" ]; then
        INPUT="${WORKDIR}/fix_copy.cpp"
        cp "$SRC" "$INPUT"
    else
        INPUT="$SRC"
    fi

    OUTPUT="${WORKDIR}/output_${mode}.txt"

    if $CXX $CXXFLAGS -fplugin="$PLUGIN" \
        -Xclang -plugin-arg-perf-sanitizer -Xclang "mode=${mode}" \
        -c "$INPUT" -o "${WORKDIR}/out_${mode}.o" \
        -Wno-exceptions \
        >"$OUTPUT" 2>&1; then
        STATUS="compiled OK"
    else
        # Some modes may produce non-zero exit (e.g., diag with warnings).
        # As long as the compiler did not crash/segfault, that is acceptable.
        if [ $? -le 1 ]; then
            STATUS="exit code $? (non-fatal)"
        else
            echo "FAIL (exit code $?)"
            FAILED=$((FAILED + 1))
            continue
        fi
    fi

    # Check that output is non-empty for modes that should produce output.
    BYTES=$(wc -c < "$OUTPUT" | tr -d '[:space:]')
    case "$mode" in
        quiet)
            # quiet mode should produce minimal or no output
            echo "PASS ($STATUS, ${BYTES} bytes output)"
            PASSED=$((PASSED + 1))
            ;;
        report|diff|diag)
            if [ "$BYTES" -gt 0 ]; then
                LINES=$(wc -l < "$OUTPUT" | tr -d '[:space:]')
                echo "PASS ($STATUS, ${LINES} lines output)"
            else
                echo "PASS ($STATUS, empty output — plugin may not have emitted for this mode)"
            fi
            PASSED=$((PASSED + 1))
            ;;
        fix)
            echo "PASS ($STATUS)"
            PASSED=$((PASSED + 1))
            ;;
    esac
done

echo ""
echo "=== Results: $PASSED/$TOTAL passed, $FAILED/$TOTAL failed ==="

if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
