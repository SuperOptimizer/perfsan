// demo.cpp — Deliberately suboptimal code that triggers every PerfSanitizer
// hint category. Compile with:
//   clang++ -fplugin=./libPerfSanitizer.so -O2 -c demo.cpp
//
// Expected hints are annotated inline.

#include <cmath>
#include <cstdlib>
#include <vector>

// --- constexpr promotion ---
// HINT: could be constexpr
int factorial(int n) {
  if (n <= 1)
    return 1;
  return n * factorial(n - 1);
}

// HINT: const variable could be constexpr
const int kBufferSize = 1024;

// --- consteval promotion ---
// HINT: small constexpr function could be consteval
constexpr int square(int x) { return x * x; }

// --- noexcept ---
// HINT: function doesn't throw but not noexcept
int add(int a, int b) { return a + b; }

// --- pure/const attribute ---
// HINT: appears to have no side effects
double computeDistance(double x1, double y1, double x2, double y2) {
  double dx = x2 - x1;
  double dy = y2 - y1;
  return std::sqrt(dx * dx + dy * dy);
}

// --- restrict annotation ---
// HINT: pointer parameters not __restrict__
void vectorAdd(float *dst, const float *a, const float *b, int n) {
  // HINT: alias barrier — dst may alias a or b
  for (int i = 0; i < n; ++i) {
    dst[i] = a[i] + b[i];
  }
}

// --- loop bound / trip count ---
// HINT: no compile-time-constant upper bound
void processData(float *data, int n) {
  for (int i = 0; i < n; ++i) {
    data[i] = data[i] * 2.0f + 1.0f;
  }
}

// --- loop-invariant call in condition ---
int getSize();
void processVector(std::vector<int> &v) {
  // HINT: getSize() called in loop condition
  for (int i = 0; i < getSize(); ++i) {
    v.push_back(i);
  }
}

// --- branch prediction ---
// HINT: error path without [[unlikely]]
int safeDivide(int a, int b) {
  if (b == 0) {
    return -1; // error path
  }
  return a / b;
}

// --- FMA contraction ---
// HINT: multiply-add pattern in loop
void dotProduct(const double *a, const double *b, int n, double *result) {
  *result = 0.0;
  for (int i = 0; i < n; ++i) {
    *result += a[i] * b[i]; // a[i]*b[i] + running sum
  }
}

// --- heap to stack ---
// HINT: small heap allocation
void createSmallObject() {
  int *p = new int(42);
  *p += 1;
  delete p;
}

// --- struct padding / data layout ---
// HINT: significant padding due to field ordering
struct BadLayout {
  char a;     // 1 byte
  double b;   // 8 bytes (7 bytes padding before)
  char c;     // 1 byte
  int d;      // 4 bytes (3 bytes padding before)
  char e;     // 1 byte (3 bytes padding after)
  // Total: 15 bytes data, 13 bytes padding = 28 bytes? Actually 32 with
  // alignment
};

// --- virtual devirtualization ---
// HINT: virtual method not marked final
struct Shape {
  virtual double area() const { return 0.0; }
  virtual ~Shape() = default;
};

struct Circle : Shape {
  double radius;
  // HINT: could be final
  double area() const override { return 3.14159 * radius * radius; }
};

// --- inlining candidate in loop ---
// HINT: small function called in loop, not always_inline
int transform(int x) { return x * 2 + 1; }

void applyTransform(int *data, int n) {
  for (int i = 0; i < n; ++i) {
    data[i] = transform(data[i]);
  }
}

// --- tail call ---
// HINT: call in tail position not marked musttail
int fibonacci(int n) {
  if (n <= 1)
    return n;
  return fibonacci(n - 1) + fibonacci(n - 2);
}

// --- branchless select opportunity ---
int absVal(int x) {
  if (x < 0)
    return -x;
  else
    return x;
}

// --- alignment ---
struct SimdData {
  float values[16]; // HINT: array field without explicit alignment
};

// --- move semantics ---
std::vector<int> createVector(int n) {
  std::vector<int> v(n);
  for (int i = 0; i < n; ++i)
    v[i] = i;
  return v; // NRVO should handle this, but in complex cases...
}

// --- math in loop ---
void computeSines(double *out, const double *in, int n) {
  for (int i = 0; i < n; ++i) {
    out[i] = std::sin(in[i]); // HINT: math function in loop
  }
}

// --- small trip count ---
void processQuad(float data[4]) {
  for (int i = 0; i < 3; ++i) {
    data[i] += data[i + 1];
  }
}

int main() {
  float a[1024], b[1024], c[1024];
  vectorAdd(c, a, b, 1024);

  const int n = 100;
  int data[100];
  applyTransform(data, n);

  BadLayout bl;
  (void)bl;

  Circle circle;
  circle.radius = 5.0;
  Shape *s = &circle;
  (void)s->area();

  return 0;
}
