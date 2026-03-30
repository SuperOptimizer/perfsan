// RUN: %clang_cc1 -load %perfsanitizer_plugin -plugin perf-sanitizer \
// RUN:   -plugin-arg-perf-sanitizer -std=c++17 %s 2>&1 | FileCheck %s

// Test: constexpr promotion
// CHECK: constexpr-promotion
// CHECK: could be declared constexpr
int add(int a, int b) { return a + b; }

// Test: noexcept suggestion
// CHECK: noexcept
// CHECK: not declared noexcept
int multiply(int a, int b) { return a * b; }

// Test: restrict annotation
// CHECK: restrict-annotation
// CHECK: not __restrict__ qualified
void copy(float *dst, const float *src, int n) {
  for (int i = 0; i < n; ++i)
    dst[i] = src[i];
}

// Test: loop bound
// CHECK: loop-bound
// CHECK: no compile-time-constant upper bound
void scale(float *data, int n, float factor) {
  for (int i = 0; i < n; ++i)
    data[i] *= factor;
}

// Test: branch prediction
// CHECK: branch-prediction
// CHECK: error/early-return path
int safeDivide(int a, int b) {
  if (b == 0) {
    return -1;
  }
  return a / b;
}

// Test: struct padding
// CHECK: data-layout
// CHECK: padding
struct Padded {
  char a;
  double b;
  char c;
  int d;
};

// Test: heap to stack
// CHECK: heap-to-stack
// CHECK: small heap allocation
void smallAlloc() {
  int *p = new int(42);
  delete p;
}
