// all_hints_bad.cpp — Comprehensive test with INTENTIONALLY SUBOPTIMAL code
// that triggers every PerfSanitizer hint category.
//
// Compile with:
//   clang++ -std=c++20 -O2 -c all_hints_bad.cpp
//
// This file models a small numerical computation library with deliberately
// poor performance patterns throughout.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// 1. Function that could be constexpr but is not
// HINT: constexpr-promotion
// ---------------------------------------------------------------------------
int triangleNumber(int n) {
    return n * (n + 1) / 2;
}

// ---------------------------------------------------------------------------
// 2. Small constexpr function that could be consteval
// HINT: consteval-promotion
// ---------------------------------------------------------------------------
constexpr int kTileSize(int dim) { return dim * dim; }

// ---------------------------------------------------------------------------
// 3. Function without noexcept that does not throw
// HINT: noexcept
// ---------------------------------------------------------------------------
double lerp(double a, double b, double t) {
    return a + t * (b - a);
}

// ---------------------------------------------------------------------------
// 4. Pure function without __attribute__((pure))
// HINT: pure-attribute
// ---------------------------------------------------------------------------
double magnitude(double x, double y, double z) {
    return std::sqrt(x * x + y * y + z * z);
}

// ---------------------------------------------------------------------------
// 5. Pointer params without __restrict__
// HINT: restrict-annotation
// ---------------------------------------------------------------------------
void blendArrays(float *dst, const float *src0, const float *src1,
                  float alpha, int n) {
    for (int i = 0; i < n; ++i) {
        dst[i] = src0[i] * alpha + src1[i] * (1.0f - alpha);
    }
}

// ---------------------------------------------------------------------------
// 6. Struct with terrible padding (char, double, char, int, char)
// HINT: data-layout / struct-padding
// ---------------------------------------------------------------------------
struct SensorReading {
    char  status;    //  1 byte
    double value;    //  8 bytes  (7 bytes padding before)
    char  unit;      //  1 byte
    int   sensorId;  //  4 bytes  (3 bytes padding before)
    char  flags;     //  1 byte   (3 bytes padding after)
    // Total data: 15 bytes, padding: 13 bytes => 32 bytes with alignment
};

// ---------------------------------------------------------------------------
// 7. Virtual methods not marked final
// HINT: devirtualization
// ---------------------------------------------------------------------------
struct Integrator {
    virtual double step(double y, double t, double dt) const {
        return y + dt;  // Euler fallback
    }
    virtual ~Integrator() = default;
};

struct RungeKutta4 : Integrator {
    // HINT: could be marked final
    double step(double y, double t, double dt) const override {
        double k1 = dt * y;
        double k2 = dt * (y + 0.5 * k1);
        double k3 = dt * (y + 0.5 * k2);
        double k4 = dt * (y + k3);
        return y + (k1 + 2.0 * k2 + 2.0 * k3 + k4) / 6.0;
    }
};

// ---------------------------------------------------------------------------
// 8. Array field without alignas
// HINT: alignment
// ---------------------------------------------------------------------------
struct SimdWorkBuffer {
    float data[16];   // Should be aligned for SIMD (e.g., alignas(64))
    float scratch[16];
};

// ---------------------------------------------------------------------------
// 9. For-loop with runtime bound (no constant)
// HINT: loop-bound
// ---------------------------------------------------------------------------
void normalizeWeights(float *weights, int count) {
    float sum = 0.0f;
    for (int i = 0; i < count; ++i) {
        sum += weights[i];
    }
    if (sum > 0.0f) {
        for (int i = 0; i < count; ++i) {
            weights[i] /= sum;
        }
    }
}

// ---------------------------------------------------------------------------
// 10. While-loop with unknown trip count
// HINT: loop-bound (while variant)
// ---------------------------------------------------------------------------
double newtonSqrt(double x) {
    double guess = x * 0.5;
    double prev  = 0.0;
    while (std::abs(guess - prev) > 1e-12) {
        prev  = guess;
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}

// ---------------------------------------------------------------------------
// 11. Function call in loop condition (vec.size())
// HINT: loop-condition-call
// ---------------------------------------------------------------------------
void scaleVector(std::vector<double> &v, double factor) {
    for (int i = 0; i < v.size(); ++i) {
        v[i] *= factor;
    }
}

// ---------------------------------------------------------------------------
// 12. If/else where one branch is clearly an error path
// HINT: branch-prediction / [[unlikely]]
// ---------------------------------------------------------------------------
int checkedIndex(const float *data, int len, int idx) {
    if (idx < 0 || idx >= len) {
        return -1;  // error path
    }
    return static_cast<int>(data[idx]);
}

// ---------------------------------------------------------------------------
// 13. Small function called in a loop without always_inline
// HINT: inline-candidate
// ---------------------------------------------------------------------------
float fastInvSqrt(float x) {
    float half = 0.5f * x;
    int i;
    std::memcpy(&i, &x, sizeof(i));
    i = 0x5f3759df - (i >> 1);
    std::memcpy(&x, &i, sizeof(x));
    x = x * (1.5f - half * x * x);
    return x;
}

void computeInvNorms(float *out, const float *in, int n) {
    for (int i = 0; i < n; ++i) {
        out[i] = fastInvSqrt(in[i]);  // called in tight loop
    }
}

// ---------------------------------------------------------------------------
// 14. new int(42) that could be stack-allocated
// HINT: heap-to-stack
// ---------------------------------------------------------------------------
double computeWithTemp() {
    double *tmp = new double(0.0);
    for (int i = 0; i < 100; ++i) {
        *tmp += static_cast<double>(i) * 0.01;
    }
    double result = *tmp;
    delete tmp;
    return result;
}

// ---------------------------------------------------------------------------
// 15. a*b + c floating point pattern in a loop (FMA candidate)
// HINT: fma-contraction
// ---------------------------------------------------------------------------
void matvecMul4x4(const double *mat, const double *vec, double *out) {
    for (int row = 0; row < 4; ++row) {
        out[row] = 0.0;
        for (int col = 0; col < 4; ++col) {
            out[row] += mat[row * 4 + col] * vec[col];  // a*b + c
        }
    }
}

// ---------------------------------------------------------------------------
// 16. Struct array accessed with strided pattern — AoS
// HINT: data-layout / AoS-to-SoA
// ---------------------------------------------------------------------------
struct Particle {
    double x, y, z;
    double vx, vy, vz;
    double mass;
};

void updatePositions(Particle *particles, int n, double dt) {
    for (int i = 0; i < n; ++i) {
        particles[i].x += particles[i].vx * dt;  // strided access
        particles[i].y += particles[i].vy * dt;
        particles[i].z += particles[i].vz * dt;
    }
}

// ---------------------------------------------------------------------------
// 17. Large error handling block inside a hot path
// HINT: cold-code-separation
// ---------------------------------------------------------------------------
int processBuffer(const float *buf, int len) {
    int total = 0;
    for (int i = 0; i < len; ++i) {
        if (buf[i] < 0.0f) {
            // Large error handling block in hot path
            volatile int errCode = -1;
            volatile int errLine = i;
            volatile float errVal = buf[i];
            (void)errCode;
            (void)errLine;
            (void)errVal;
            total -= 1;
            continue;
        }
        total += static_cast<int>(buf[i] * 100.0f);
    }
    return total;
}

// ---------------------------------------------------------------------------
// 18. If-statement inside loop where condition is loop-invariant
// HINT: loop-invariant-condition
// ---------------------------------------------------------------------------
void applyGain(float *samples, int n, float gain, bool clip) {
    for (int i = 0; i < n; ++i) {
        samples[i] *= gain;
        if (clip) {  // loop-invariant condition
            if (samples[i] > 1.0f)  samples[i] = 1.0f;
            if (samples[i] < -1.0f) samples[i] = -1.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// 19. Loop over float[16] array without vectorization hint
// HINT: vectorization-hint
// ---------------------------------------------------------------------------
void simdAdd(float *dst, const float *a, const float *b) {
    for (int i = 0; i < 16; ++i) {
        dst[i] = a[i] + b[i];
    }
}

// ---------------------------------------------------------------------------
// 20. Division by constant inside a loop
// HINT: strength-reduction (div-by-constant)
// ---------------------------------------------------------------------------
void quantize(const float *in, int *out, int n) {
    for (int i = 0; i < n; ++i) {
        out[i] = static_cast<int>(in[i]) / 7;
    }
}

// ---------------------------------------------------------------------------
// 21. Pure function without [[nodiscard]]
// HINT: nodiscard
// ---------------------------------------------------------------------------
double dotProduct3(double ax, double ay, double az,
                   double bx, double by, double bz) {
    return ax * bx + ay * by + az * bz;
}

// ---------------------------------------------------------------------------
// 22. for(int i = 0; i < vec.size(); i++) — signed vs unsigned
// HINT: sign-compare
// ---------------------------------------------------------------------------
double sumVector(const std::vector<double> &v) {
    double s = 0.0;
    for (int i = 0; i < v.size(); ++i) {
        s += v[i];
    }
    return s;
}

// ---------------------------------------------------------------------------
// 23. Function with unused parameter
// HINT: unused-parameter
// ---------------------------------------------------------------------------
void zeroFill(float *dst, int n, int alignment) {
    for (int i = 0; i < n; ++i) {
        dst[i] = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// 24. Return value of pure function discarded
// HINT: discarded-result
// ---------------------------------------------------------------------------
void wastedComputation(double x, double y, double z) {
    magnitude(x, y, z);  // result discarded
}

// ---------------------------------------------------------------------------
// 25. Missing std::move on return of local
// HINT: move-on-return
// ---------------------------------------------------------------------------
std::vector<double> buildHistogram(const double *data, int n, int bins) {
    std::vector<double> hist(bins, 0.0);
    double invBins = 1.0 / static_cast<double>(bins);
    for (int i = 0; i < n; ++i) {
        int b = static_cast<int>(data[i] * invBins);
        if (b >= 0 && b < bins)
            hist[b] += 1.0;
    }
    // Two named locals — NRVO cannot apply.  Missing std::move.
    std::vector<double> empty;
    if (n <= 0)
        return empty;
    return hist;
}

// ---------------------------------------------------------------------------
// main — exercise every pattern so the compiler sees them
// ---------------------------------------------------------------------------
int main() {
    // 1. constexpr promotion
    int tri = triangleNumber(10);

    // 2. consteval promotion
    int tile = kTileSize(8);

    // 3. noexcept
    double mid = lerp(0.0, 1.0, 0.5);

    // 4. pure attribute
    double mag = magnitude(3.0, 4.0, 0.0);

    // 5. restrict
    float a[256], b[256], c[256];
    blendArrays(c, a, b, 0.5f, 256);

    // 6. struct padding
    SensorReading sr{};
    (void)sr;

    // 7. devirtualization
    RungeKutta4 rk;
    Integrator *intg = &rk;
    double yNext = intg->step(1.0, 0.0, 0.01);

    // 8. alignment
    SimdWorkBuffer wb{};
    (void)wb;

    // 9. runtime loop bound
    normalizeWeights(a, 256);

    // 10. while unknown trip
    double sq = newtonSqrt(2.0);

    // 11. function call in condition
    std::vector<double> vec(100, 1.0);
    scaleVector(vec, 2.0);

    // 12. error branch
    int idx = checkedIndex(a, 256, 10);

    // 13. inline candidate
    computeInvNorms(c, a, 256);

    // 14. heap to stack
    double accum = computeWithTemp();

    // 15. FMA
    double mat[16] = {}, v4[4] = {1,0,0,0}, out4[4];
    matvecMul4x4(mat, v4, out4);

    // 16. AoS
    Particle parts[64]{};
    updatePositions(parts, 64, 0.016);

    // 17. cold block
    int pval = processBuffer(a, 256);

    // 18. loop-invariant condition
    applyGain(a, 256, 0.8f, true);

    // 19. vectorization
    simdAdd(c, a, b);

    // 20. div by constant
    int qout[256];
    quantize(a, qout, 256);

    // 21. nodiscard
    double dp = dotProduct3(1, 0, 0, 0, 1, 0);

    // 22. signed vs unsigned
    double sv = sumVector(vec);

    // 23. unused param
    zeroFill(a, 256, 16);

    // 24. discarded result
    wastedComputation(1.0, 2.0, 3.0);

    // 25. missing move
    double histData[1000]{};
    auto hist = buildHistogram(histData, 1000, 50);

    // Prevent everything from being optimized away
    volatile double sink = 0.0;
    sink += tri + tile + mid + mag + yNext + sq + idx + accum + dp + sv + pval;
    sink += out4[0] + parts[0].x + c[0] + hist[0];
    (void)sink;

    return 0;
}
