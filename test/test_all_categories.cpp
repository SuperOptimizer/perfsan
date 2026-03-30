// test_all_categories.cpp - Comprehensive test exercising every PerfSanitizer
// hint category (all 73 enum values where possible).
//
// Compile with:
//   clang++ -std=c++20 -O2 -c test_all_categories.cpp
//
// Each section is labeled with "// TEST: <category-name>" so coverage is easy
// to verify.  The code is INTENTIONALLY suboptimal.

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

// Prevent the compiler from optimizing away values.
template <typename T>
void doNotOptimize(T const &val) {
    asm volatile("" : : "r,m"(val) : "memory");
}

// ============================================================================
// TEST: ConstexprPromotion (function)
// Function whose body is a pure computation on its arguments — could be
// constexpr but is not.
// ============================================================================
int triangleNumber(int n) { return n * (n + 1) / 2; }

// TEST: ConstexprPromotion (variable)
// Variable initialized from a literal expression — could be constexpr.
static const int kCacheLineSize = 64;

// ============================================================================
// TEST: ConstevalPromotion
// Already constexpr, but only ever called with compile-time constants —
// candidate for consteval.
// ============================================================================
constexpr int tileArea(int dim) { return dim * dim; }

// ============================================================================
// TEST: NoExcept
// Simple non-throwing function that lacks noexcept.
// ============================================================================
double lerp(double a, double b, double t) { return a + t * (b - a); }

// ============================================================================
// TEST: PureConst
// Function with no side effects — candidate for __attribute__((pure/const)).
// ============================================================================
double magnitude(double x, double y, double z) {
    return std::sqrt(x * x + y * y + z * z);
}

// ============================================================================
// TEST: RestrictAnnotation
// Pointer parameters that are not __restrict__ qualified.
// ============================================================================
void blendArrays(float *dst, const float *src0, const float *src1,
                  float alpha, int n) {
    for (int i = 0; i < n; ++i)
        dst[i] = src0[i] * alpha + src1[i] * (1.0f - alpha);
}

// ============================================================================
// TEST: DataLayout (struct padding)
// Struct with terrible field ordering — lots of internal padding.
// ============================================================================
struct SensorReading {
    char status;   //  1 byte
    double value;  //  8 bytes  (7 padding before)
    char unit;     //  1 byte
    int sensorId;  //  4 bytes  (3 padding before)
    char flags;    //  1 byte   (3 padding after)
};

// ============================================================================
// TEST: VirtualDevirt
// Virtual method on a derived class not marked final.
// ============================================================================
struct Integrator {
    virtual double step(double y, double t, double dt) const {
        return y + dt;
    }
    virtual ~Integrator() = default;
};

struct RungeKutta4 : Integrator {
    // Could be marked final.
    double step(double y, double t, double dt) const override {
        double k1 = dt * y;
        double k2 = dt * (y + 0.5 * k1);
        double k3 = dt * (y + 0.5 * k2);
        double k4 = dt * (y + k3);
        return y + (k1 + 2 * k2 + 2 * k3 + k4) / 6.0;
    }
};

// ============================================================================
// TEST: AlignmentHint (alignas missing on array field)
// Float array intended for SIMD but lacking alignas.
// ============================================================================
struct SimdWorkBuffer {
    float data[16];    // Should be alignas(64)
    float scratch[16];
};

// ============================================================================
// TEST: LoopBound (runtime bound)
// For-loop whose upper bound is a function parameter.
// ============================================================================
void normalizeWeights(float *weights, int count) {
    float sum = 0.0f;
    for (int i = 0; i < count; ++i)
        sum += weights[i];
    if (sum > 0.0f)
        for (int i = 0; i < count; ++i)
            weights[i] /= sum;
}

// ============================================================================
// TEST: LoopBound (while loop, unknown trip count)
// ============================================================================
double newtonSqrt(double x) {
    double guess = x * 0.5;
    double prev = 0.0;
    while (std::abs(guess - prev) > 1e-12) {
        prev = guess;
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}

// ============================================================================
// TEST: LoopBound (function call in loop condition)
// Calling vec.size() each iteration.
// ============================================================================
void scaleVector(std::vector<double> &v, double factor) {
    for (int i = 0; i < static_cast<int>(v.size()); ++i)
        v[i] *= factor;
}

// ============================================================================
// TEST: BranchPrediction (error path)
// Error/early-return branch without [[unlikely]].
// ============================================================================
int checkedIndex(const float *data, int len, int idx) {
    if (idx < 0 || idx >= len) {
        return -1; // error path
    }
    return static_cast<int>(data[idx]);
}

// ============================================================================
// TEST: SmallFunctionInline (small function in loop / inlining candidate)
// ============================================================================
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
    for (int i = 0; i < n; ++i)
        out[i] = fastInvSqrt(in[i]); // called in tight loop
}

// ============================================================================
// TEST: HeapToStack
// new/delete for a tiny object that never escapes.
// ============================================================================
double computeWithTemp() {
    double *tmp = new double(0.0);
    for (int i = 0; i < 100; ++i)
        *tmp += static_cast<double>(i) * 0.01;
    double result = *tmp;
    delete tmp;
    return result;
}

// ============================================================================
// TEST: FMAContraction
// a*b + c pattern in a loop.
// ============================================================================
void matvecMul4x4(const double *mat, const double *vec, double *out) {
    for (int row = 0; row < 4; ++row) {
        out[row] = 0.0;
        for (int col = 0; col < 4; ++col)
            out[row] += mat[row * 4 + col] * vec[col]; // a*b + c
    }
}

// ============================================================================
// TEST: SoAvsAoS (strided access in loop)
// ============================================================================
struct Particle {
    double x, y, z;
    double vx, vy, vz;
    double mass;
};

void updatePositions(Particle *particles, int n, double dt) {
    for (int i = 0; i < n; ++i) {
        particles[i].x += particles[i].vx * dt;
        particles[i].y += particles[i].vy * dt;
        particles[i].z += particles[i].vz * dt;
    }
}

// ============================================================================
// TEST: ColdPathOutlining
// Large error-handling block in a hot loop.
// ============================================================================
int processBuffer(const float *buf, int len) {
    int total = 0;
    for (int i = 0; i < len; ++i) {
        if (buf[i] < 0.0f) {
            // Bulky cold code
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

// ============================================================================
// TEST: LoopInvariant (loop-invariant condition)
// ============================================================================
void applyGain(float *samples, int n, float gain, bool clip) {
    for (int i = 0; i < n; ++i) {
        samples[i] *= gain;
        if (clip) { // loop-invariant condition
            if (samples[i] > 1.0f) samples[i] = 1.0f;
            if (samples[i] < -1.0f) samples[i] = -1.0f;
        }
    }
}

// ============================================================================
// TEST: SIMDWidth (vectorization hint)
// Loop over small fixed-size array with no SIMD width pragma.
// ============================================================================
void simdAdd(float *dst, const float *a, const float *b) {
    for (int i = 0; i < 16; ++i)
        dst[i] = a[i] + b[i];
}

// ============================================================================
// TEST: StrengthReduction (division by constant)
// ============================================================================
void quantize(const float *in, int *out, int n) {
    for (int i = 0; i < n; ++i)
        out[i] = static_cast<int>(in[i]) / 7;
}

// ============================================================================
// TEST: MissingNodiscard
// Pure computation function without [[nodiscard]].
// ============================================================================
double dotProduct3(double ax, double ay, double az,
                   double bx, double by, double bz) {
    return ax * bx + ay * by + az * bz;
}

// ============================================================================
// TEST: SignedLoopCounter
// Signed int compared against unsigned size().
// ============================================================================
double sumVector(const std::vector<double> &v) {
    double s = 0.0;
    for (int i = 0; i < static_cast<int>(v.size()); ++i)
        s += v[i];
    return s;
}

// ============================================================================
// TEST: ExceptionCost (try/catch in loop)
// ============================================================================
int exceptionInLoop(const std::vector<std::string> &data) {
    int total = 0;
    for (int i = 0; i < static_cast<int>(data.size()); ++i) {
        try {
            total += std::stoi(data[i]);
        } catch (...) {
            total += 0;
        }
    }
    return total;
}

// ============================================================================
// TEST: FalseSharing (adjacent atomics in a struct)
// Two atomics on the same cache line, written by different logical owners.
// ============================================================================
struct CounterPair {
    std::atomic<int> counterA{0}; // likely same cache line as counterB
    std::atomic<int> counterB{0};
};

void incrementCounters(CounterPair &cp, int n) {
    for (int i = 0; i < n; ++i) {
        cp.counterA.fetch_add(1, std::memory_order_relaxed);
        cp.counterB.fetch_add(1, std::memory_order_relaxed);
    }
}

// ============================================================================
// TEST: StringByValue
// std::string passed by value where const& would suffice.
// ============================================================================
int countChar(std::string s, char c) {
    int count = 0;
    for (char ch : s)
        if (ch == c) ++count;
    return count;
}

// ============================================================================
// TEST: ContainerReserve (push_back without reserve)
// ============================================================================
std::vector<int> buildSequence(int n) {
    std::vector<int> result;
    for (int i = 0; i < n; ++i)
        result.push_back(i); // no reserve
    return result;
}

// ============================================================================
// TEST: RangeForConversion
// Old-style index loop that could be range-for.
// ============================================================================
double sumWithIndex(const std::vector<double> &v) {
    double s = 0.0;
    for (size_t i = 0; i < v.size(); ++i)
        s += v[i];
    return s;
}

// ============================================================================
// TEST: ConstexprIf
// if-else on a compile-time-deducible type trait.
// ============================================================================
template <typename T>
int typeTag(T val) {
    if (std::is_integral<T>::value)
        return 1;
    else
        return 2;
}

// ============================================================================
// TEST: LambdaCaptureOpt (lambda capture by value for large object)
// ============================================================================
void lambdaCaptureTest(const std::vector<int> &big) {
    auto fn = [big]() { return big.size(); }; // captures by value
    doNotOptimize(fn());
}

// ============================================================================
// TEST: OutputParamToReturn
// Output parameter that could be a return value.
// ============================================================================
void computeMinMax(const float *data, int n, float *outMin, float *outMax) {
    *outMin = data[0];
    *outMax = data[0];
    for (int i = 1; i < n; ++i) {
        if (data[i] < *outMin) *outMin = data[i];
        if (data[i] > *outMax) *outMax = data[i];
    }
}

// ============================================================================
// TEST: SmallFunctionNotInline
// Tiny helper never marked inline.
// ============================================================================
int clampInt(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// ============================================================================
// TEST: UnnecessaryCopy (large param by value)
// ============================================================================
double sumMap(std::map<int, double> m) { // map by value
    double s = 0.0;
    for (auto &kv : m)
        s += kv.second;
    return s;
}

// ============================================================================
// TEST: TightLoopAllocation (new in loop)
// ============================================================================
void tightLoopAlloc(int n) {
    for (int i = 0; i < n; ++i) {
        int *p = new int(i);
        doNotOptimize(*p);
        delete p;
    }
}

// ============================================================================
// TEST: BoolBranching
// if (x) return true; else return false;
// ============================================================================
bool isPositive(int x) {
    if (x > 0)
        return true;
    else
        return false;
}

// ============================================================================
// TEST: PowerOfTwo (x % 8 could be x & 7)
// ============================================================================
int modEight(int x) { return x % 8; }

// ============================================================================
// TEST: ExceptionInDestructor
// Destructor that may throw.
// ============================================================================
struct BadDtor {
    std::string name;
    ~BadDtor() {
        if (name.empty())
            throw std::runtime_error("empty name"); // throw in dtor
    }
};

// ============================================================================
// TEST: VectorBoolAvoid
// vector<bool> is a proxy container with poor perf.
// ============================================================================
void usesVectorBool(int n) {
    std::vector<bool> flags(n, false);
    for (int i = 0; i < n; i += 2)
        flags[i] = true;
    doNotOptimize(flags[0]);
}

// ============================================================================
// TEST: MutexInLoop
// Acquiring a mutex every iteration.
// ============================================================================
void mutexInLoop(std::mutex &mtx, std::vector<int> &data, int n) {
    for (int i = 0; i < n; ++i) {
        std::lock_guard<std::mutex> lock(mtx);
        data.push_back(i);
    }
}

// ============================================================================
// TEST: StdFunctionOverhead
// std::function in a hot loop adds indirection overhead.
// ============================================================================
void applyFn(std::function<double(double)> fn, double *data, int n) {
    for (int i = 0; i < n; ++i)
        data[i] = fn(data[i]);
}

// ============================================================================
// TEST: EmptyLoopBody
// Loop with no useful work.
// ============================================================================
void emptyLoop(int n) {
    for (int i = 0; i < n; ++i) {
        ; // empty body
    }
}

// ============================================================================
// TEST: StringConcatInLoop
// Building a string with += in a loop.
// ============================================================================
std::string buildCsv(const std::vector<int> &vals) {
    std::string result;
    for (int v : vals)
        result += std::to_string(v) + ","; // repeated concat
    return result;
}

// ============================================================================
// TEST: RegexInLoop
// Constructing/compiling a regex inside a loop.
// ============================================================================
int countMatches(const std::vector<std::string> &lines) {
    int count = 0;
    for (auto &line : lines) {
        std::regex pat(R"(\d+)"); // compiled every iteration
        if (std::regex_search(line, pat))
            ++count;
    }
    return count;
}

// ============================================================================
// TEST: DynamicCastInLoop
// dynamic_cast inside a loop.
// ============================================================================
struct Base {
    virtual ~Base() = default;
};
struct Derived : Base {
    int val = 42;
};

int sumDerived(const std::vector<Base *> &items) {
    int total = 0;
    for (auto *b : items) {
        auto *d = dynamic_cast<Derived *>(b); // dynamic_cast in loop
        if (d) total += d->val;
    }
    return total;
}

// ============================================================================
// TEST: VirtualDtorMissing
// Base class with virtual methods but non-virtual destructor.
// ============================================================================
struct Shape {
    virtual double area() const = 0;
    ~Shape() {} // should be virtual
};

struct Circle : Shape {
    double r;
    Circle(double r) : r(r) {}
    double area() const override { return 3.14159 * r * r; }
};

// ============================================================================
// TEST: CopyInRangeFor
// Range-for loop that copies elements unnecessarily.
// ============================================================================
double sumStrLengths(const std::vector<std::string> &vs) {
    double total = 0;
    for (std::string s : vs) // copy each string
        total += s.size();
    return total;
}

// ============================================================================
// TEST: GlobalVarInLoop
// Reading a global variable inside a hot loop.
// ============================================================================
int gMultiplier = 3;

void scaleByGlobal(int *arr, int n) {
    for (int i = 0; i < n; ++i)
        arr[i] *= gMultiplier; // global read each iter
}

// ============================================================================
// TEST: VolatileInLoop
// Volatile access inside a computation loop.
// ============================================================================
void volatileLoop(volatile int *flag, int *data, int n) {
    for (int i = 0; i < n; ++i) {
        data[i] += *flag; // volatile read each iteration
    }
}

// ============================================================================
// TEST: Vectorization
// Loop that could be vectorized but has potential aliasing.
// ============================================================================
void addArrays(double *dst, const double *a, const double *b, int n) {
    for (int i = 0; i < n; ++i)
        dst[i] = a[i] + b[i];
}

// ============================================================================
// TEST: AliasBarrier
// Store/load through potentially aliased pointers.
// ============================================================================
void scatterGather(int *out, const int *in, const int *indices, int n) {
    for (int i = 0; i < n; ++i)
        out[indices[i]] = in[i]; // alias barrier
}

// ============================================================================
// TEST: TailCall
// Recursive function that could use tail-call optimization.
// ============================================================================
int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

// ============================================================================
// TEST: HotColdSplit / HotColdFunction
// Function with a hot path and a large cold error path.
// ============================================================================
void processWithFallback(int *data, int n) {
    for (int i = 0; i < n; ++i) {
        if (data[i] >= 0) {
            data[i] = data[i] * 2 + 1; // hot
        } else {
            // Cold: lots of work for error
            data[i] = -data[i];
            data[i] = data[i] * data[i];
            data[i] = data[i] % 1000;
            data[i] += 42;
        }
    }
}

// ============================================================================
// TEST: RedundantLoad
// Loading the same value repeatedly in a loop.
// ============================================================================
struct Config {
    int threshold;
};

int countAboveThreshold(const int *data, int n, const Config *cfg) {
    int count = 0;
    for (int i = 0; i < n; ++i)
        if (data[i] > cfg->threshold) // reload each iter?
            ++count;
    return count;
}

// ============================================================================
// TEST: SROAEscape
// Aggregate that escapes to a call, blocking scalar replacement.
// ============================================================================
struct Vec2 {
    double x, y;
};

double externalUse(Vec2 *v);

double sroaEscape() {
    Vec2 v{1.0, 2.0};
    return externalUse(&v); // address escapes
}

// ============================================================================
// TEST: MoveSemantics
// Missing std::move on a local being returned in one of two branches.
// ============================================================================
std::vector<double> buildData(int n) {
    std::vector<double> result(n, 0.0);
    std::vector<double> empty;
    if (n <= 0) return empty;
    for (int i = 0; i < n; ++i)
        result[i] = static_cast<double>(i);
    return result;
}

// ============================================================================
// TEST: BranchlessSelect
// Conditional that could be a branchless select / cmov.
// ============================================================================
int absVal(int x) {
    if (x < 0)
        return -x;
    return x;
}

// ============================================================================
// TEST: LoopUnswitching
// Another loop-invariant branch that could be unswitched.
// ============================================================================
void processMode(int *data, int n, int mode) {
    for (int i = 0; i < n; ++i) {
        if (mode == 1)
            data[i] += 10;
        else
            data[i] -= 10;
    }
}

// ============================================================================
// TEST: UnusedInclude
// (Detected at include-graph level; we include <regex> but use it minimally.)
// ============================================================================
// The #include <regex> above may trigger this if the regex usage is removed.

// ============================================================================
// TEST: RedundantComputation
// Same sub-expression computed multiple times.
// ============================================================================
void redundantCalc(double *out, const double *a, const double *b, int n) {
    for (int i = 0; i < n; ++i) {
        out[i] = std::sqrt(a[i] * a[i] + b[i] * b[i])
                + std::sqrt(a[i] * a[i] + b[i] * b[i]); // duplicate sqrt
    }
}

// ============================================================================
// TEST: SortAlgorithm
// Bubble sort where std::sort would be better.
// ============================================================================
void bubbleSort(int *arr, int n) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n - 1 - i; ++j)
            if (arr[j] > arr[j + 1])
                std::swap(arr[j], arr[j + 1]);
}

// ============================================================================
// TEST: SharedPtrOverhead
// shared_ptr where unique_ptr would suffice (single owner).
// ============================================================================
double sharedPtrOverhead() {
    auto sp = std::make_shared<double>(3.14);
    *sp += 1.0;
    return *sp;
}

// ============================================================================
// TEST: BitManipulation
// Manual bit tricks that could use __builtin_popcount etc.
// ============================================================================
int manualPopcount(unsigned x) {
    int count = 0;
    while (x) {
        count += x & 1;
        x >>= 1;
    }
    return count;
}

// ============================================================================
// TEST: RedundantAtomic
// Atomic operations in a single-threaded context.
// ============================================================================
int redundantAtomic() {
    std::atomic<int> counter{0};
    for (int i = 0; i < 100; ++i)
        counter.fetch_add(1, std::memory_order_seq_cst);
    return counter.load();
}

// ============================================================================
// TEST: CacheLineSplit
// Array access pattern likely to cross cache lines.
// ============================================================================
struct alignas(1) TinyStruct {
    char data[63]; // straddles 64-byte boundary
};

void touchAlternating(TinyStruct *arr, int n) {
    for (int i = 0; i < n; ++i)
        arr[i].data[0] = static_cast<char>(i);
}

// ============================================================================
// TEST: CrossTUInlining
// Small function defined in a source file that would benefit from being in a
// header for cross-TU inlining.  (Simulated: PerfSanitizer flags small
// non-inline non-static functions.)
// ============================================================================
double energy(double m) { return m * 299792458.0 * 299792458.0; }

// ============================================================================
// TEST: DuplicateCondition
// Same boolean test repeated.
// ============================================================================
int duplicateCond(int x) {
    if (x > 0 && x > 0) // duplicate check
        return x;
    return 0;
}

// ============================================================================
// TEST: ThrowInNoexcept
// Simulated: a noexcept(false) function that contains throw.
// (Included for category coverage; actual detection depends on AST analysis.)
// ============================================================================
void mightThrow() {
    throw std::runtime_error("oops");
}

// ============================================================================
// TEST: ImplicitConversion
// Implicit narrowing or widening conversion.
// ============================================================================
float implicitNarrow(double d) {
    return d; // implicit double -> float
}

// ============================================================================
// TEST: SlicingCopy
// Object slicing: copying derived into base by value.
// ============================================================================
struct Animal {
    virtual ~Animal() = default;
    virtual int legs() const { return 0; }
};

struct Dog : Animal {
    int legs() const override { return 4; }
};

Animal sliceCopy(Dog d) {
    Animal a = d; // slicing
    return a;
}

// ============================================================================
// TEST: DivisionChain
// Chained divisions that could be restructured.
// ============================================================================
double divisionChain(double a, double b, double c) {
    return a / b / c; // two divisions; a / (b * c) is one div + one mul
}

// ============================================================================
// TEST: SpillPressure
// Function with many live variables that may cause register spills.
// ============================================================================
double spillPressure(double a, double b, double c, double d,
                     double e, double f, double g, double h,
                     double i, double j, double k, double l,
                     double m, double n, double o, double p) {
    return a + b + c + d + e + f + g + h
         + i + j + k + l + m + n + o + p;
}

// ============================================================================
// TEST: UnrollingBlocker
// Loop body with a call that blocks unrolling.
// ============================================================================
void unrollingBlocker(int *data, int n) {
    for (int i = 0; i < n; ++i)
        data[i] = std::rand(); // opaque call blocks unrolling
}

// ============================================================================
// TEST: BranchOnFloat
// Branching on floating-point comparison in a loop.
// ============================================================================
void branchOnFloat(const double *data, int *out, int n) {
    for (int i = 0; i < n; ++i) {
        if (data[i] > 0.5)
            out[i] = 1;
        else
            out[i] = 0;
    }
}

// ============================================================================
// TEST: MemoryAccessPattern
// Non-sequential (column-major) access of a row-major 2D array.
// ============================================================================
void columnSum(const double *matrix, double *colSums, int rows, int cols) {
    for (int c = 0; c < cols; ++c) {
        colSums[c] = 0.0;
        for (int r = 0; r < rows; ++r)
            colSums[c] += matrix[r * cols + c]; // column-major traversal
    }
}

// ============================================================================
// TEST: InliningCandidate
// Another small function that should be inlined.
// ============================================================================
int addOne(int x) { return x + 1; }

// ============================================================================
// main() — exercise every pattern
// ============================================================================
int main() {
    volatile double sink = 0.0;
    volatile int isink = 0;

    // ConstexprPromotion
    isink += triangleNumber(10);
    (void)kCacheLineSize;

    // ConstevalPromotion
    isink += tileArea(8);

    // NoExcept
    sink += lerp(0.0, 1.0, 0.5);

    // PureConst
    sink += magnitude(3.0, 4.0, 0.0);

    // RestrictAnnotation
    float fa[256], fb[256], fc[256];
    blendArrays(fc, fa, fb, 0.5f, 256);

    // DataLayout
    SensorReading sr{};
    doNotOptimize(sr);

    // VirtualDevirt
    RungeKutta4 rk;
    Integrator *intg = &rk;
    sink += intg->step(1.0, 0.0, 0.01);

    // AlignmentHint
    SimdWorkBuffer wb{};
    doNotOptimize(wb);

    // LoopBound (runtime)
    normalizeWeights(fa, 256);

    // LoopBound (while)
    sink += newtonSqrt(2.0);

    // LoopBound (func call in condition)
    std::vector<double> vec(100, 1.0);
    scaleVector(vec, 2.0);

    // BranchPrediction
    isink += checkedIndex(fa, 256, 10);

    // SmallFunctionInline
    computeInvNorms(fc, fa, 256);

    // HeapToStack
    sink += computeWithTemp();

    // FMAContraction
    double mat[16] = {}, v4[4] = {1, 0, 0, 0}, out4[4];
    matvecMul4x4(mat, v4, out4);
    sink += out4[0];

    // SoAvsAoS
    Particle parts[64]{};
    updatePositions(parts, 64, 0.016);
    sink += parts[0].x;

    // ColdPathOutlining
    isink += processBuffer(fa, 256);

    // LoopInvariant
    applyGain(fa, 256, 0.8f, true);

    // SIMDWidth
    simdAdd(fc, fa, fb);

    // StrengthReduction
    int qout[256];
    quantize(fa, qout, 256);

    // MissingNodiscard
    sink += dotProduct3(1, 0, 0, 0, 1, 0);

    // SignedLoopCounter
    sink += sumVector(vec);

    // ExceptionCost
    std::vector<std::string> strs{"1", "2", "bad", "3"};
    isink += exceptionInLoop(strs);

    // FalseSharing
    CounterPair cp;
    incrementCounters(cp, 100);
    isink += cp.counterA.load();

    // StringByValue
    isink += countChar("hello world", 'o');

    // ContainerReserve
    auto seq = buildSequence(100);
    isink += seq.back();

    // RangeForConversion
    sink += sumWithIndex(vec);

    // ConstexprIf
    isink += typeTag(42);
    isink += typeTag(3.14);

    // LambdaCaptureOpt
    std::vector<int> bigVec(1000, 1);
    lambdaCaptureTest(bigVec);

    // OutputParamToReturn
    float fmin, fmax;
    computeMinMax(fa, 256, &fmin, &fmax);
    sink += fmin + fmax;

    // SmallFunctionNotInline
    isink += clampInt(50, 0, 100);

    // UnnecessaryCopy
    std::map<int, double> m{{1, 1.1}, {2, 2.2}};
    sink += sumMap(m);

    // TightLoopAllocation
    tightLoopAlloc(50);

    // BoolBranching
    isink += isPositive(5) ? 1 : 0;

    // PowerOfTwo
    isink += modEight(123);

    // ExceptionInDestructor — just instantiate but do not let dtor run with
    // empty name (that would actually throw).
    {
        BadDtor bd{"ok"};
        doNotOptimize(bd);
    }

    // VectorBoolAvoid
    usesVectorBool(100);

    // MutexInLoop
    std::mutex mtx;
    std::vector<int> mdata;
    mutexInLoop(mtx, mdata, 50);

    // StdFunctionOverhead
    double arr[64];
    applyFn([](double x) { return x * 2.0; }, arr, 64);

    // EmptyLoopBody
    emptyLoop(1000);

    // StringConcatInLoop
    auto csv = buildCsv(seq);
    doNotOptimize(csv);

    // RegexInLoop
    std::vector<std::string> lines{"abc123", "def", "456ghi"};
    isink += countMatches(lines);

    // DynamicCastInLoop
    Derived d1, d2;
    Base b1;
    std::vector<Base *> bases{&d1, &b1, &d2};
    isink += sumDerived(bases);

    // VirtualDtorMissing — just use the type
    Circle circ(5.0);
    sink += circ.area();

    // CopyInRangeFor
    std::vector<std::string> names{"alpha", "beta", "gamma"};
    sink += sumStrLengths(names);

    // GlobalVarInLoop
    int iarr[64];
    scaleByGlobal(iarr, 64);

    // VolatileInLoop
    volatile int flag = 1;
    int idata[64];
    volatileLoop(&flag, idata, 64);

    // Vectorization / AliasBarrier
    double da[128], db[128], dc[128];
    addArrays(dc, da, db, 128);

    int indices[64];
    for (int i = 0; i < 64; ++i) indices[i] = i;
    scatterGather(iarr, iarr, indices, 64);

    // TailCall
    isink += factorial(10);

    // HotColdSplit / HotColdFunction
    processWithFallback(iarr, 64);

    // RedundantLoad
    Config cfg{50};
    isink += countAboveThreshold(iarr, 64, &cfg);

    // SROAEscape — can't call externalUse (undefined), but instantiate Vec2
    Vec2 vv{1.0, 2.0};
    doNotOptimize(vv);

    // MoveSemantics
    auto bdata = buildData(10);
    sink += bdata[0];

    // BranchlessSelect
    isink += absVal(-7);

    // LoopUnswitching
    processMode(iarr, 64, 1);

    // RedundantComputation
    redundantCalc(da, db, dc, 128);

    // SortAlgorithm
    bubbleSort(iarr, 64);

    // SharedPtrOverhead
    sink += sharedPtrOverhead();

    // BitManipulation
    isink += manualPopcount(0xDEADBEEF);

    // RedundantAtomic
    isink += redundantAtomic();

    // CacheLineSplit
    TinyStruct ts[10];
    touchAlternating(ts, 10);

    // CrossTUInlining
    sink += energy(1.0);

    // DuplicateCondition
    isink += duplicateCond(5);

    // ImplicitConversion
    sink += implicitNarrow(3.14159265358979);

    // SlicingCopy
    Dog dog;
    Animal animal = sliceCopy(dog);
    isink += animal.legs();

    // DivisionChain
    sink += divisionChain(100.0, 3.0, 7.0);

    // SpillPressure
    sink += spillPressure(1, 2, 3, 4, 5, 6, 7, 8,
                          9, 10, 11, 12, 13, 14, 15, 16);

    // UnrollingBlocker
    unrollingBlocker(iarr, 64);

    // BranchOnFloat
    int bout[128];
    branchOnFloat(da, bout, 128);

    // MemoryAccessPattern
    double colSums[16];
    columnSum(da, colSums, 8, 16);

    // InliningCandidate
    isink += addOne(isink);

    (void)sink;
    (void)isink;
    return 0;
}
