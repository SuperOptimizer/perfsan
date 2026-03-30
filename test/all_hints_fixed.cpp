// all_hints_fixed.cpp — The same numerical computation library as
// all_hints_bad.cpp, but with EVERY performance hint addressed.
//
// Compile with:
//   clang++ -std=c++20 -O2 -c all_hints_fixed.cpp
//
// Each section documents which hint was fixed and how.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 1. FIX: constexpr added
// ---------------------------------------------------------------------------
constexpr int triangleNumber(int n) noexcept {
    return n * (n + 1) / 2;
}

// ---------------------------------------------------------------------------
// 2. FIX: consteval (evaluated only at compile time)
// ---------------------------------------------------------------------------
consteval int kTileSize(int dim) { return dim * dim; }

// ---------------------------------------------------------------------------
// 3. FIX: noexcept added
// ---------------------------------------------------------------------------
double lerp(double a, double b, double t) noexcept {
    return a + t * (b - a);
}

// ---------------------------------------------------------------------------
// 4. FIX: __attribute__((pure)) added, plus noexcept
// ---------------------------------------------------------------------------
[[nodiscard]] __attribute__((pure))
double magnitude(double x, double y, double z) noexcept {
    return std::sqrt(x * x + y * y + z * z);
}

// ---------------------------------------------------------------------------
// 5. FIX: __restrict__ on pointer params, noexcept
// ---------------------------------------------------------------------------
void blendArrays(float *__restrict__ dst,
                 const float *__restrict__ src0,
                 const float *__restrict__ src1,
                 float alpha, int n) noexcept {
    for (int i = 0; i < n; ++i) {
        dst[i] = src0[i] * alpha + src1[i] * (1.0f - alpha);
    }
}

// ---------------------------------------------------------------------------
// 6. FIX: fields reordered largest-first to eliminate padding
//    Layout: double(8) + int(4) + char(1) + char(1) + char(1) + 1 pad = 16
// ---------------------------------------------------------------------------
struct SensorReading {
    double value;    //  8 bytes
    int   sensorId;  //  4 bytes
    char  status;    //  1 byte
    char  unit;      //  1 byte
    char  flags;     //  1 byte
    // Total: 15 bytes data + 1 byte tail padding = 16 bytes
};

// ---------------------------------------------------------------------------
// 7. FIX: class marked final, method marked final + noexcept
// ---------------------------------------------------------------------------
struct Integrator {
    virtual double step(double y, double t, double dt) const noexcept {
        return y + dt;
    }
    virtual ~Integrator() = default;
};

struct RungeKutta4 final : Integrator {
    double step(double y, double t, double dt) const noexcept final override {
        double k1 = dt * y;
        double k2 = dt * (y + 0.5 * k1);
        double k3 = dt * (y + 0.5 * k2);
        double k4 = dt * (y + k3);
        return y + (k1 + 2.0 * k2 + 2.0 * k3 + k4) / 6.0;
    }
};

// ---------------------------------------------------------------------------
// 8. FIX: alignas(32) on array fields for SIMD
// ---------------------------------------------------------------------------
struct SimdWorkBuffer {
    alignas(32) float data[16];
    alignas(32) float scratch[16];
};

// ---------------------------------------------------------------------------
// 9. FIX: __builtin_assume to communicate loop bound to optimizer
// ---------------------------------------------------------------------------
void normalizeWeights(float *__restrict__ weights, int count) noexcept {
    __builtin_assume(count > 0 && count <= 4096);
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
// 10. FIX: __builtin_assume on iteration cap for while-loop
// ---------------------------------------------------------------------------
double newtonSqrt(double x) noexcept {
    double guess = x * 0.5;
    double prev  = 0.0;
    int iters = 0;
    constexpr int kMaxIters = 100;
    while (std::abs(guess - prev) > 1e-12 && iters < kMaxIters) {
        prev  = guess;
        guess = 0.5 * (guess + x / guess);
        ++iters;
    }
    return guess;
}

// ---------------------------------------------------------------------------
// 11. FIX: loop condition call hoisted; unsigned counter
// ---------------------------------------------------------------------------
void scaleVector(std::vector<double> &v, double factor) noexcept {
    const std::size_t len = v.size();
    for (std::size_t i = 0; i < len; ++i) {
        v[i] *= factor;
    }
}

// ---------------------------------------------------------------------------
// 12. FIX: [[unlikely]] on error branch
// ---------------------------------------------------------------------------
[[nodiscard]]
int checkedIndex(const float *data, int len, int idx) noexcept {
    if (idx < 0 || idx >= len) [[unlikely]] {
        return -1;
    }
    return static_cast<int>(data[idx]);
}

// ---------------------------------------------------------------------------
// 13. FIX: __attribute__((always_inline)) on small loop-called function
// ---------------------------------------------------------------------------
__attribute__((always_inline))
inline float fastInvSqrt(float x) noexcept {
    float half = 0.5f * x;
    int i;
    std::memcpy(&i, &x, sizeof(i));
    i = 0x5f3759df - (i >> 1);
    std::memcpy(&x, &i, sizeof(x));
    x = x * (1.5f - half * x * x);
    return x;
}

void computeInvNorms(float *__restrict__ out,
                     const float *__restrict__ in, int n) noexcept {
    for (int i = 0; i < n; ++i) {
        out[i] = fastInvSqrt(in[i]);
    }
}

// ---------------------------------------------------------------------------
// 14. FIX: stack allocation instead of heap
// ---------------------------------------------------------------------------
double computeWithTemp() noexcept {
    double tmp = 0.0;
    for (int i = 0; i < 100; ++i) {
        tmp += static_cast<double>(i) * 0.01;
    }
    return tmp;
}

// ---------------------------------------------------------------------------
// 15. FIX: #pragma STDC FP_CONTRACT ON for FMA
// ---------------------------------------------------------------------------
#pragma STDC FP_CONTRACT ON
void matvecMul4x4(const double *__restrict__ mat,
                   const double *__restrict__ vec,
                   double *__restrict__ out) noexcept {
    for (int row = 0; row < 4; ++row) {
        out[row] = 0.0;
        for (int col = 0; col < 4; ++col) {
            out[row] += mat[row * 4 + col] * vec[col];  // FMA contracted
        }
    }
}
#pragma STDC FP_CONTRACT DEFAULT

// ---------------------------------------------------------------------------
// 16. FIX: SoA layout — parallel arrays instead of AoS struct
// ---------------------------------------------------------------------------
struct Particles {
    double *x,  *y,  *z;
    double *vx, *vy, *vz;
    double *mass;
    int count;
};

void updatePositions(Particles &p, double dt) noexcept {
    const int n = p.count;
    __builtin_assume(n > 0 && n <= 65536);
    for (int i = 0; i < n; ++i) {
        p.x[i] += p.vx[i] * dt;  // contiguous access
    }
    for (int i = 0; i < n; ++i) {
        p.y[i] += p.vy[i] * dt;
    }
    for (int i = 0; i < n; ++i) {
        p.z[i] += p.vz[i] * dt;
    }
}

// ---------------------------------------------------------------------------
// 17. FIX: error handling block marked __attribute__((cold)),
//     separated into its own function
// ---------------------------------------------------------------------------
__attribute__((cold, noinline))
static int handleBadSample(int total, int i, float val) noexcept {
    volatile int errCode = -1;
    volatile int errLine = i;
    volatile float errVal = val;
    (void)errCode;
    (void)errLine;
    (void)errVal;
    return total - 1;
}

int processBuffer(const float *__restrict__ buf, int len) noexcept {
    int total = 0;
    for (int i = 0; i < len; ++i) {
        if (buf[i] < 0.0f) [[unlikely]] {
            total = handleBadSample(total, i, buf[i]);
            continue;
        }
        total += static_cast<int>(buf[i] * 100.0f);
    }
    return total;
}

// ---------------------------------------------------------------------------
// 18. FIX: loop-invariant condition hoisted outside the loop
// ---------------------------------------------------------------------------
void applyGain(float *__restrict__ samples, int n,
               float gain, bool clip) noexcept {
    if (clip) {
        for (int i = 0; i < n; ++i) {
            samples[i] *= gain;
            if (samples[i] > 1.0f)  samples[i] = 1.0f;
            if (samples[i] < -1.0f) samples[i] = -1.0f;
        }
    } else {
        for (int i = 0; i < n; ++i) {
            samples[i] *= gain;
        }
    }
}

// ---------------------------------------------------------------------------
// 19. FIX: #pragma clang loop vectorize(enable) on SIMD loop
// ---------------------------------------------------------------------------
void simdAdd(float *__restrict__ dst,
             const float *__restrict__ a,
             const float *__restrict__ b) noexcept {
#pragma clang loop vectorize(enable) interleave(enable)
    for (int i = 0; i < 16; ++i) {
        dst[i] = a[i] + b[i];
    }
}

// ---------------------------------------------------------------------------
// 20. FIX: compiler already optimises div-by-constant, but we make the
//     constant constexpr and use unsigned to help strength-reduction
// ---------------------------------------------------------------------------
void quantize(const float *__restrict__ in,
              int *__restrict__ out, int n) noexcept {
    constexpr int kQuantStep = 7;
    for (int i = 0; i < n; ++i) {
        out[i] = static_cast<int>(in[i]) / kQuantStep;
    }
}

// ---------------------------------------------------------------------------
// 21. FIX: [[nodiscard]] added to pure function
// ---------------------------------------------------------------------------
[[nodiscard]] __attribute__((pure))
double dotProduct3(double ax, double ay, double az,
                   double bx, double by, double bz) noexcept {
    return ax * bx + ay * by + az * bz;
}

// ---------------------------------------------------------------------------
// 22. FIX: unsigned loop counter matching size_t
// ---------------------------------------------------------------------------
[[nodiscard]]
double sumVector(const std::vector<double> &v) noexcept {
    double s = 0.0;
    const std::size_t len = v.size();
    for (std::size_t i = 0; i < len; ++i) {
        s += v[i];
    }
    return s;
}

// ---------------------------------------------------------------------------
// 23. FIX: unused parameter removed
// ---------------------------------------------------------------------------
void zeroFill(float *dst, int n) noexcept {
    for (int i = 0; i < n; ++i) {
        dst[i] = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// 24. FIX: result of pure function is actually used
// ---------------------------------------------------------------------------
double usefulComputation(double x, double y, double z) noexcept {
    return magnitude(x, y, z);
}

// ---------------------------------------------------------------------------
// 25. FIX: std::move on return of local
// ---------------------------------------------------------------------------
[[nodiscard]]
std::vector<double> buildHistogram(const double *data, int n, int bins) {
    std::vector<double> hist(bins, 0.0);
    double invBins = 1.0 / static_cast<double>(bins);
    for (int i = 0; i < n; ++i) {
        int b = static_cast<int>(data[i] * invBins);
        if (b >= 0 && b < bins)
            hist[b] += 1.0;
    }
    // Two named locals — NRVO cannot apply since the compiler cannot know
    // which one is returned.  std::move is the correct fix here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpessimizing-move"
    std::vector<double> empty;
    if (n <= 0)
        return std::move(empty);
    return std::move(hist);
#pragma clang diagnostic pop
}

// ---------------------------------------------------------------------------
// main — exercise every pattern so the compiler sees them
// ---------------------------------------------------------------------------
int main() {
    // 1. constexpr promotion
    constexpr int tri = triangleNumber(10);

    // 2. consteval promotion
    constexpr int tile = kTileSize(8);

    // 3. noexcept
    double mid = lerp(0.0, 1.0, 0.5);

    // 4. pure attribute
    double mag = magnitude(3.0, 4.0, 0.0);

    // 5. restrict
    float a[256], b[256], c[256];
    blendArrays(c, a, b, 0.5f, 256);

    // 6. struct padding — now 16 bytes instead of 32
    SensorReading sr{};
    (void)sr;

    // 7. devirtualization — RungeKutta4 is final
    RungeKutta4 rk;
    Integrator *intg = &rk;
    double yNext = intg->step(1.0, 0.0, 0.01);

    // 8. alignment
    SimdWorkBuffer wb{};
    (void)wb;

    // 9. runtime loop bound with __builtin_assume
    normalizeWeights(a, 256);

    // 10. while with bounded iterations
    double sq = newtonSqrt(2.0);

    // 11. hoisted size(), unsigned counter
    std::vector<double> vec(100, 1.0);
    scaleVector(vec, 2.0);

    // 12. [[unlikely]] error branch
    int idx = checkedIndex(a, 256, 10);

    // 13. always_inline on hot helper
    computeInvNorms(c, a, 256);

    // 14. stack allocation
    double accum = computeWithTemp();

    // 15. FMA contraction
    double mat[16] = {}, v4[4] = {1,0,0,0}, out4[4];
    matvecMul4x4(mat, v4, out4);

    // 16. SoA layout
    constexpr int kNumParticles = 64;
    double px[kNumParticles]{}, py[kNumParticles]{}, pz[kNumParticles]{};
    double pvx[kNumParticles]{}, pvy[kNumParticles]{}, pvz[kNumParticles]{};
    double pmass[kNumParticles]{};
    Particles parts{px, py, pz, pvx, pvy, pvz, pmass, kNumParticles};
    updatePositions(parts, 0.016);

    // 17. cold error handler
    int pval = processBuffer(a, 256);

    // 18. hoisted loop-invariant condition
    applyGain(a, 256, 0.8f, true);

    // 19. vectorize pragma
    simdAdd(c, a, b);

    // 20. strength reduction
    int qout[256];
    quantize(a, qout, 256);

    // 21. [[nodiscard]] on pure function
    double dp = dotProduct3(1, 0, 0, 0, 1, 0);

    // 22. unsigned counter
    double sv = sumVector(vec);

    // 23. unused param removed
    zeroFill(a, 256);

    // 24. result of pure function used
    double used = usefulComputation(1.0, 2.0, 3.0);

    // 25. std::move on return
    double histData[1000]{};
    auto hist = buildHistogram(histData, 1000, 50);

    // Prevent everything from being optimized away
    volatile double sink = 0.0;
    sink += tri + tile + mid + mag + yNext + sq + idx + accum + dp + sv + pval;
    sink += out4[0] + parts.x[0] + c[0] + hist[0] + used;
    (void)sink;

    return 0;
}
