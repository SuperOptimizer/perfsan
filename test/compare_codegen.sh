#!/usr/bin/env bash
#
# compare_codegen.sh - Compare assembly output of two C++ files
#
# Usage: ./compare_codegen.sh <bad.cpp> <fixed.cpp>
#
# Compiles both files with clang++ -std=c++20 -O2, then compares:
#   - Total instruction count
#   - Branch instruction count
#   - Vectorized instruction count
#   - Object file size
#
# Exits 0 if fixed version has fewer instructions, 1 otherwise.

set -euo pipefail

if [ $# -ne 2 ]; then
    echo "Usage: $0 <bad.cpp> <fixed.cpp>"
    exit 2
fi

BAD_SRC="$1"
FIXED_SRC="$2"

for f in "$BAD_SRC" "$FIXED_SRC"; do
    if [ ! -f "$f" ]; then
        echo "Error: file not found: $f"
        exit 2
    fi
done

CXX="${CXX:-clang++}"
CXXFLAGS="${CXXFLAGS:--std=c++20 -O2}"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

BAD_ASM="$TMPDIR/bad.s"
FIXED_ASM="$TMPDIR/fixed.s"
BAD_OBJ="$TMPDIR/bad.o"
FIXED_OBJ="$TMPDIR/fixed.o"

echo "=== Codegen Comparison ==="
echo "  Bad:   $BAD_SRC"
echo "  Fixed: $FIXED_SRC"
echo "  Compiler: $CXX $CXXFLAGS"
echo ""

# Compile to assembly
echo "Compiling to assembly..."
$CXX $CXXFLAGS -S -o "$BAD_ASM" "$BAD_SRC" 2>&1 || { echo "Error: failed to compile $BAD_SRC"; exit 2; }
$CXX $CXXFLAGS -S -o "$FIXED_ASM" "$FIXED_SRC" 2>&1 || { echo "Error: failed to compile $FIXED_SRC"; exit 2; }

# Compile to object files
echo "Compiling to object files..."
$CXX $CXXFLAGS -c -o "$BAD_OBJ" "$BAD_SRC" 2>&1 || { echo "Error: failed to compile $BAD_SRC to .o"; exit 2; }
$CXX $CXXFLAGS -c -o "$FIXED_OBJ" "$FIXED_SRC" 2>&1 || { echo "Error: failed to compile $FIXED_SRC to .o"; exit 2; }

# --- Metric functions ---

# Count actual instructions (lines that start with a mnemonic, skip labels/directives/comments)
count_instructions() {
    grep -cE '^\s+[a-z]' "$1" | tr -d '[:space:]'
}

# Count branch instructions
count_branches() {
    grep -ciE '^\s+(je|jne|jg|jge|jl|jle|ja|jae|jb|jbe|jmp|jmpq|js|jns|jo|jno|jz|jnz|call|callq|ret|retq)\b' "$1" 2>/dev/null || echo 0
}

# Count vectorized instructions (SSE/AVX)
count_vector() {
    grep -ciE '^\s+v?(addps|addpd|subps|subpd|mulps|mulpd|divps|divpd|fmadd[0-9]*ps|fmadd[0-9]*pd|maxps|maxpd|minps|minpd|sqrtps|sqrtpd|movaps|movapd|movups|movupd|andps|andpd|orps|orpd|xorps|xorpd|cmpps|cmppd|shufps|shufpd|unpcklps|unpckhps|blendps|blendpd|dpps|dppd|haddps|haddpd|paddd|paddq|paddw|paddb|psubd|psubq|psubw|psubb|pmulld|pmullw|pmaddwd)\b' "$1" 2>/dev/null || echo 0
}

# Get file size in bytes
file_size() {
    wc -c < "$1" | tr -d '[:space:]'
}

# --- Gather metrics ---

BAD_INSNS=$(count_instructions "$BAD_ASM")
FIXED_INSNS=$(count_instructions "$FIXED_ASM")

BAD_BRANCHES=$(count_branches "$BAD_ASM")
FIXED_BRANCHES=$(count_branches "$FIXED_ASM")

BAD_VECTOR=$(count_vector "$BAD_ASM")
FIXED_VECTOR=$(count_vector "$FIXED_ASM")

BAD_SIZE=$(file_size "$BAD_OBJ")
FIXED_SIZE=$(file_size "$FIXED_OBJ")

# --- Percentage change (negative = improvement) ---
pct_change() {
    local old=$1 new=$2
    if [ "$old" -eq 0 ]; then
        if [ "$new" -eq 0 ]; then
            echo "0.0"
        else
            echo "+inf"
        fi
    else
        awk "BEGIN { printf \"%.1f\", (($new - $old) / $old) * 100 }"
    fi
}

INSN_PCT=$(pct_change "$BAD_INSNS" "$FIXED_INSNS")
BRANCH_PCT=$(pct_change "$BAD_BRANCHES" "$FIXED_BRANCHES")
VECTOR_PCT=$(pct_change "$BAD_VECTOR" "$FIXED_VECTOR")
SIZE_PCT=$(pct_change "$BAD_SIZE" "$FIXED_SIZE")

# --- Print summary table ---
echo ""
echo "┌──────────────────────┬──────────┬──────────┬──────────┐"
printf "│ %-20s │ %8s │ %8s │ %7s%% │\n" "Metric" "Bad" "Fixed" "Change"
echo "├──────────────────────┼──────────┼──────────┼──────────┤"
printf "│ %-20s │ %8s │ %8s │ %7s%% │\n" "Total instructions" "$BAD_INSNS" "$FIXED_INSNS" "$INSN_PCT"
printf "│ %-20s │ %8s │ %8s │ %7s%% │\n" "Branch instructions" "$BAD_BRANCHES" "$FIXED_BRANCHES" "$BRANCH_PCT"
printf "│ %-20s │ %8s │ %8s │ %7s%% │\n" "Vector instructions" "$BAD_VECTOR" "$FIXED_VECTOR" "$VECTOR_PCT"
printf "│ %-20s │ %8s │ %8s │ %7s%% │\n" "Object size (bytes)" "$BAD_SIZE" "$FIXED_SIZE" "$SIZE_PCT"
echo "└──────────────────────┴──────────┴──────────┴──────────┘"
echo ""

# --- Verdict ---
if [ "$FIXED_INSNS" -lt "$BAD_INSNS" ]; then
    echo "RESULT: Fixed version is BETTER (fewer instructions)"
    exit 0
elif [ "$FIXED_INSNS" -eq "$BAD_INSNS" ]; then
    echo "RESULT: No change in instruction count"
    exit 1
else
    echo "RESULT: Fixed version is WORSE (more instructions)"
    exit 1
fi
