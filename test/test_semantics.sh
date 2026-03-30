#!/bin/bash
# test_semantics.sh — Verify auto-fix preserves program semantics
#
# Compiles+runs code before and after auto-fix, checks output matches.
# This catches fixes that change behavior (wrong constexpr, broken restrict, etc.)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN="${PERFSAN_PLUGIN:-$SCRIPT_DIR/../build/PerfSanitizer.so}"
CXX="${CXX:-clang++}"
PASS=0
FAIL=0
TOTAL=0

if [ ! -f "$PLUGIN" ]; then
  echo "ERROR: Plugin not found at $PLUGIN"
  exit 1
fi

run_test() {
  local NAME="$1"
  local SRC="$2"
  TOTAL=$((TOTAL + 1))

  # Write source to temp file
  local ORIG="/tmp/perfsan_sem_orig_${NAME}.cpp"
  local FIXED="/tmp/perfsan_sem_fixed_${NAME}.cpp"
  echo "$SRC" > "$ORIG"
  cp "$ORIG" "$FIXED"

  # Compile and run original
  if ! $CXX -std=c++20 -O2 "$ORIG" -o "/tmp/perfsan_sem_orig_${NAME}" -lm 2>/dev/null; then
    echo "SKIP $NAME: original doesn't compile"
    return
  fi
  local ORIG_OUT=$("/tmp/perfsan_sem_orig_${NAME}" 2>&1 || true)

  # Auto-fix
  $CXX -fplugin="$PLUGIN" -Xclang -plugin-arg-perf-sanitizer -Xclang fix \
    -std=c++20 -O2 -c "$FIXED" -o /dev/null 2>/dev/null || true

  # Compile fixed
  if ! $CXX -std=c++20 -O2 "$FIXED" -o "/tmp/perfsan_sem_fixed_${NAME}" -lm 2>/dev/null; then
    echo "FAIL $NAME: fixed code doesn't compile"
    FAIL=$((FAIL + 1))
    return
  fi
  local FIXED_OUT=$("/tmp/perfsan_sem_fixed_${NAME}" 2>&1 || true)

  # Compare
  if [ "$ORIG_OUT" = "$FIXED_OUT" ]; then
    echo "PASS $NAME"
    PASS=$((PASS + 1))
  else
    echo "FAIL $NAME: output differs"
    echo "  expected: $(echo "$ORIG_OUT" | head -3)"
    echo "  got:      $(echo "$FIXED_OUT" | head -3)"
    FAIL=$((FAIL + 1))
  fi
}

# --- Test Cases ---

run_test "factorial" '
#include <cstdio>
int factorial(int n) { if (n <= 1) return 1; return n * factorial(n-1); }
int main() { printf("%d %d %d\n", factorial(5), factorial(10), factorial(0)); return 0; }
'

run_test "math" '
#include <cstdio>
#include <cmath>
double dist(double x1, double y1, double x2, double y2) {
  return std::sqrt((x2-x1)*(x2-x1) + (y2-y1)*(y2-y1));
}
int main() { printf("%.2f\n", dist(0,0,3,4)); return 0; }
'

run_test "vectoradd" '
#include <cstdio>
void vadd(float *c, const float *a, const float *b, int n) {
  for (int i = 0; i < n; i++) c[i] = a[i] + b[i];
}
int main() {
  float a[4]={1,2,3,4}, b[4]={5,6,7,8}, c[4];
  vadd(c,a,b,4);
  printf("%.0f %.0f %.0f %.0f\n", c[0], c[1], c[2], c[3]);
  return 0;
}
'

run_test "branching" '
#include <cstdio>
int safediv(int a, int b) { if (b==0) return -1; return a/b; }
int absval(int x) { if (x < 0) return -x; else return x; }
int main() {
  printf("%d %d %d %d\n", safediv(10,3), safediv(10,0), absval(-5), absval(5));
  return 0;
}
'

run_test "struct_padding" '
#include <cstdio>
struct Bad { char a; double b; char c; int d; };
int main() { Bad x; x.a=1; x.b=2.0; x.c=3; x.d=4; printf("%d %.1f %d %d\n", x.a, x.b, x.c, x.d); return 0; }
'

run_test "vector_pushback" '
#include <cstdio>
#include <vector>
int main() {
  std::vector<int> v;
  for (int i = 0; i < 10; i++) v.push_back(i*i);
  for (int x : v) printf("%d ", x);
  printf("\n");
  return 0;
}
'

run_test "dotproduct" '
#include <cstdio>
void dot(const double *a, const double *b, int n, double *r) {
  *r = 0; for (int i = 0; i < n; i++) *r += a[i]*b[i];
}
int main() {
  double a[4]={1,2,3,4}, b[4]={5,6,7,8}, r;
  dot(a,b,4,&r); printf("%.0f\n", r); return 0;
}
'

run_test "string_ops" '
#include <cstdio>
#include <string>
int main() {
  std::string s = "hello";
  for (int i = 0; i < 5; i++) s += "x";
  printf("%s %zu\n", s.c_str(), s.size());
  return 0;
}
'

# --- Summary ---
echo ""
echo "================================"
echo "  SEMANTICS TEST RESULTS"
echo "================================"
echo "  PASS: $PASS / $TOTAL"
echo "  FAIL: $FAIL / $TOTAL"
echo "================================"

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
