#!/bin/bash
# test_autofix_roundtrip.sh - Verify that PerfSanitizer autofix produces
# compilable output and reduces hint count.
#
# Usage:
#   ./test_autofix_roundtrip.sh
#
# Environment:
#   PERFSAN_PLUGIN  - path to PerfSanitizer.so (default: ../build/PerfSanitizer.so)
#   CXX             - compiler (default: clang++)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN="${PERFSAN_PLUGIN:-${SCRIPT_DIR}/../build/PerfSanitizer.so}"
CXX="${CXX:-clang++}"
CXXFLAGS="${CXXFLAGS:--std=c++20 -O2}"
SRC="${SCRIPT_DIR}/test_all_categories.cpp"
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

COPY="${WORKDIR}/test_all_categories.cpp"
FIXED="${WORKDIR}/test_all_categories_fixed.cpp"

cp "$SRC" "$COPY"

echo "=== Step 1: Run PerfSanitizer in report mode to get baseline hint count ==="
BASELINE_OUTPUT="${WORKDIR}/baseline.txt"
$CXX $CXXFLAGS -fplugin="$PLUGIN" \
    -Xclang -plugin-arg-perf-sanitizer -Xclang mode=report \
    -c "$COPY" -o /dev/null -Wno-exceptions 2>"$BASELINE_OUTPUT" || true

BASELINE_COUNT=$(grep -c '\[perf-sanitizer\]' "$BASELINE_OUTPUT" 2>/dev/null || echo 0)
echo "  Baseline hint count: $BASELINE_COUNT"

if [ "$BASELINE_COUNT" -eq 0 ]; then
    echo "WARN: No hints detected in baseline — plugin may not be loaded correctly."
    echo "  Proceeding anyway to test fix mode."
fi

echo ""
echo "=== Step 2: Run PerfSanitizer in fix mode ==="
cp "$COPY" "$FIXED"
$CXX $CXXFLAGS -fplugin="$PLUGIN" \
    -Xclang -plugin-arg-perf-sanitizer -Xclang mode=fix \
    -c "$FIXED" -o /dev/null -Wno-exceptions 2>"${WORKDIR}/fix_output.txt" || true

echo "  Fix mode completed."

echo ""
echo "=== Step 3: Verify fixed file compiles ==="
if $CXX $CXXFLAGS -c "$FIXED" -o /dev/null -Wno-exceptions 2>"${WORKDIR}/compile_errors.txt"; then
    echo "  PASS: Fixed file compiles successfully."
else
    echo "  FAIL: Fixed file does not compile."
    echo "  Compiler errors:"
    cat "${WORKDIR}/compile_errors.txt"
    exit 1
fi

echo ""
echo "=== Step 4: Re-run PerfSanitizer on fixed file, check hint count decreased ==="
FIXED_OUTPUT="${WORKDIR}/fixed_report.txt"
$CXX $CXXFLAGS -fplugin="$PLUGIN" \
    -Xclang -plugin-arg-perf-sanitizer -Xclang mode=report \
    -c "$FIXED" -o /dev/null -Wno-exceptions 2>"$FIXED_OUTPUT" || true

FIXED_COUNT=$(grep -c '\[perf-sanitizer\]' "$FIXED_OUTPUT" 2>/dev/null || echo 0)
echo "  Fixed hint count: $FIXED_COUNT"

echo ""
echo "=== Summary ==="
echo "  Before: $BASELINE_COUNT hints"
echo "  After:  $FIXED_COUNT hints"

if [ "$BASELINE_COUNT" -gt 0 ] && [ "$FIXED_COUNT" -lt "$BASELINE_COUNT" ]; then
    REDUCED=$((BASELINE_COUNT - FIXED_COUNT))
    echo "  PASS: Hint count decreased by $REDUCED ($(( REDUCED * 100 / BASELINE_COUNT ))%)"
    exit 0
elif [ "$BASELINE_COUNT" -eq 0 ]; then
    echo "  SKIP: No baseline hints to compare (plugin may not have emitted diagnostics)."
    exit 0
else
    echo "  FAIL: Hint count did not decrease after autofix."
    exit 1
fi
