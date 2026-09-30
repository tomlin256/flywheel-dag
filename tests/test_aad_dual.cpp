// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// Dual numbers: a value and its partials, for differentiating a functor in one
// call (flywheel-dag#10).

#include <gtest/gtest.h>
#include "flywheel/dag_aad.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

using dag::aad::Dual;

namespace {

// M_PI is POSIX, not standard C++.
const double kPi = std::acos(-1.0);

/// Argument i of N: its value, with d = eᵢ.
template<std::size_t N>
Dual<N> arg(double v, std::size_t i) {
    Dual<N> x(v);
    x.d[i] = 1.0;
    return x;
}

double normPdf(double x) { return std::exp(-0.5 * x * x) / std::sqrt(2.0 * kPi); }
double normCdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Arithmetic
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadDual, ArithmeticFollowsTheSumProductAndQuotientRules) {
    const Dual<2> x = arg<2>(3.0, 0);
    const Dual<2> y = arg<2>(-2.0, 1);

    Dual<2> r = x + y;
    EXPECT_EQ(r.value, 1.0);
    EXPECT_EQ(r.d[0], 1.0);
    EXPECT_EQ(r.d[1], 1.0);

    r = x - y;
    EXPECT_EQ(r.value, 5.0);
    EXPECT_EQ(r.d[0], 1.0);
    EXPECT_EQ(r.d[1], -1.0);

    r = x * y;
    EXPECT_EQ(r.value, -6.0);
    EXPECT_EQ(r.d[0], -2.0);
    EXPECT_EQ(r.d[1], 3.0);

    r = x / y;
    EXPECT_EQ(r.value, -1.5);
    EXPECT_EQ(r.d[0], -0.5);    // 1/y
    EXPECT_EQ(r.d[1], -0.75);   // −x/y²

    r = -x;
    EXPECT_EQ(r.value, -3.0);
    EXPECT_EQ(r.d[0], -1.0);
    EXPECT_EQ(r.d[1], 0.0);

    r = +y;
    EXPECT_EQ(r.value, -2.0);
    EXPECT_EQ(r.d[1], 1.0);
}

TEST(AadDual, ADoubleIsAConstantOnEitherSide) {
    const Dual<1> x = arg<1>(4.0, 0);

    EXPECT_EQ((x + 2.0).d[0], 1.0);
    EXPECT_EQ((2.0 + x).d[0], 1.0);
    EXPECT_EQ((x - 2.0).d[0], 1.0);
    EXPECT_EQ((2.0 - x).d[0], -1.0);
    EXPECT_EQ((x * 2.0).d[0], 2.0);
    EXPECT_EQ((2.0 * x).d[0], 2.0);
    EXPECT_EQ((x / 2.0).d[0], 0.5);
    EXPECT_EQ((2.0 / x).d[0], -0.125);    // −2/x²
    EXPECT_EQ((x * 3).value, 12.0);       // an int converts to double

    Dual<1> y = x;
    y += 1.0;
    y *= x;          // (x + 1)·x: 2x + 1
    y -= 2.0;
    y /= 2.0;        // ((x + 1)·x − 2)/2: x + 0.5
    EXPECT_EQ(y.value, 9.0);
    EXPECT_EQ(y.d[0], 4.5);

    const Dual<1> c = 7.0;
    EXPECT_EQ(c.value, 7.0);
    EXPECT_EQ(c.d[0], 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Functions
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadDual, FunctionsMatchTheirDerivatives) {
    struct Case {
        const char* name;
        Dual<1> (*f)(const Dual<1>&);
        double (*value)(double);
        double (*slope)(double);
    };
    const Case cases[] = {
        {"exp",  dag::aad::exp<1>,  [](double v) { return std::exp(v); },
                                    [](double v) { return std::exp(v); }},
        {"log",  dag::aad::log<1>,  [](double v) { return std::log(v); },
                                    [](double v) { return 1.0 / v; }},
        {"sqrt", dag::aad::sqrt<1>, [](double v) { return std::sqrt(v); },
                                    [](double v) { return 0.5 / std::sqrt(v); }},
        {"sin",  dag::aad::sin<1>,  [](double v) { return std::sin(v); },
                                    [](double v) { return std::cos(v); }},
        {"cos",  dag::aad::cos<1>,  [](double v) { return std::cos(v); },
                                    [](double v) { return -std::sin(v); }},
        {"tanh", dag::aad::tanh<1>, [](double v) { return std::tanh(v); },
                                    [](double v) { return 1.0 / (std::cosh(v) * std::cosh(v)); }},
        {"erf",  dag::aad::erf<1>,  [](double v) { return std::erf(v); },
                                    [](double v) { return 2.0 / std::sqrt(kPi) * std::exp(-v * v); }},
        {"erfc", dag::aad::erfc<1>, [](double v) { return std::erfc(v); },
                                    [](double v) { return -2.0 / std::sqrt(kPi) * std::exp(-v * v); }},
        {"abs",  dag::aad::abs<1>,  [](double v) { return std::abs(v); },
                                    [](double v) { return v > 0.0 ? 1.0 : -1.0; }},
        {"tan",  dag::aad::tan<1>,  [](double v) { return std::tan(v); },
                                    [](double v) { return 1.0 / (std::cos(v) * std::cos(v)); }},
        {"asin", dag::aad::asin<1>, [](double v) { return std::asin(v); },
                                    [](double v) { return 1.0 / std::sqrt(1.0 - v * v); }},
        {"acos", dag::aad::acos<1>, [](double v) { return std::acos(v); },
                                    [](double v) { return -1.0 / std::sqrt(1.0 - v * v); }},
        {"atan", dag::aad::atan<1>, [](double v) { return std::atan(v); },
                                    [](double v) { return 1.0 / (1.0 + v * v); }},
    };
    // The points, moved where a function needs them: abs to either side of its
    // kink, and asin and acos inside [−1, 1].
    const auto pointFor = [](const Case& c, double v) {
        const std::string name = c.name;
        if (name == "abs") return v - 2.0;
        if (name == "asin" || name == "acos") return v / 5.0;
        return v;
    };
    for (const Case& c : cases) {
        for (const double v : {0.3, 1.7, 4.2}) {
            const double at = pointFor(c, v);
            const Dual<1> r = c.f(arg<1>(at, 0));
            EXPECT_EQ(r.value, c.value(at)) << c.name << " at " << at;
            EXPECT_NEAR(r.d[0], c.slope(at), 1e-15 * std::max(1.0, std::abs(c.slope(at))))
                << c.name << " at " << at;
        }
    }
}

TEST(AadDual, PowCoversEachMix) {
    const Dual<2> a = arg<2>(2.0, 0);
    const Dual<2> b = arg<2>(3.0, 1);

    Dual<2> r = pow(a, b);
    EXPECT_EQ(r.value, 8.0);
    EXPECT_EQ(r.d[0], 12.0);                     // b·a^(b−1)
    EXPECT_DOUBLE_EQ(r.d[1], 8.0 * std::log(2.0));   // a^b·ln a

    r = pow(a, 3.0);
    EXPECT_EQ(r.d[0], 12.0);
    EXPECT_EQ(r.d[1], 0.0);

    r = pow(2.0, b);
    EXPECT_EQ(r.d[0], 0.0);
    EXPECT_DOUBLE_EQ(r.d[1], 8.0 * std::log(2.0));

    // Flat where the formulas give 0·∞: in b at a = 0 with b > 0, and in a
    // when b is 0.
    r = pow(arg<2>(0.0, 0), arg<2>(2.0, 1));
    EXPECT_EQ(r.d[0], 0.0);
    EXPECT_EQ(r.d[1], 0.0);
    r = pow(arg<2>(0.0, 0), arg<2>(0.0, 1));
    EXPECT_EQ(r.d[0], 0.0);
}

// y first, as std::atan2. At (3, −4), in the second quadrant, the partials are
// b/(a² + b²) = −4/25 and −a/(a² + b²) = −3/25.
TEST(AadDual, Atan2CoversEachMix) {
    const Dual<2> a = arg<2>(3.0, 0);
    const Dual<2> b = arg<2>(-4.0, 1);

    Dual<2> r = atan2(a, b);
    EXPECT_DOUBLE_EQ(r.value, std::atan2(3.0, -4.0));
    EXPECT_DOUBLE_EQ(r.d[0], -4.0 / 25.0);
    EXPECT_DOUBLE_EQ(r.d[1], -3.0 / 25.0);

    r = atan2(a, -4.0);
    EXPECT_DOUBLE_EQ(r.value, std::atan2(3.0, -4.0));
    EXPECT_DOUBLE_EQ(r.d[0], -4.0 / 25.0);
    EXPECT_EQ(r.d[1], 0.0);

    r = atan2(3.0, b);
    EXPECT_DOUBLE_EQ(r.value, std::atan2(3.0, -4.0));
    EXPECT_EQ(r.d[0], 0.0);
    EXPECT_DOUBLE_EQ(r.d[1], -3.0 / 25.0);
}

// A Dual takes its partials from the op's Derivative<Op>, so the two agree bit
// for bit, including where the textbook forms go wrong: asin next to 1, and
// atan2 where a² + b² underflows or overflows.
TEST(AadDual, TrigSharesTheOpsPartials) {
    namespace ops = dag::ops;
    const double nearOne = 1.0 - std::ldexp(1.0, -27);
    for (const double v : {-0.8, 0.3, nearOne}) {
        const Dual<1> x = arg<1>(v, 0);
        EXPECT_EQ(sin(x).d[0],  ops::Derivative<ops::SinOp<double>>::d(v)) << "at " << v;
        EXPECT_EQ(cos(x).d[0],  ops::Derivative<ops::CosOp<double>>::d(v)) << "at " << v;
        EXPECT_EQ(tan(x).d[0],  ops::Derivative<ops::TanOp<double>>::d(v)) << "at " << v;
        EXPECT_EQ(asin(x).d[0], ops::Derivative<ops::AsinOp<double>>::d(v)) << "at " << v;
        EXPECT_EQ(acos(x).d[0], ops::Derivative<ops::AcosOp<double>>::d(v)) << "at " << v;
        EXPECT_EQ(atan(x).d[0], ops::Derivative<ops::AtanOp<double>>::d(v)) << "at " << v;
    }
    for (const int e : {0, -600, 600}) {
        const double a = std::ldexp(3.0, e);
        const double b = std::ldexp(-4.0, e);
        const Dual<2> r = atan2(arg<2>(a, 0), arg<2>(b, 1));
        const auto [pa, pb] = ops::Derivative<ops::Atan2Op<double>>::d(a, b);
        EXPECT_EQ(r.d[0], pa) << "at (3, -4) * 2^" << e;
        EXPECT_EQ(r.d[1], pb) << "at (3, -4) * 2^" << e;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparisons and kinks
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadDual, ComparisonsReadTheValue) {
    const auto f = [](const auto& x, const auto& y) { return x < y ? x * y : x + y; };
    for (const auto& [x, y] : {std::pair{1.0, 2.0}, std::pair{3.0, 2.0}}) {
        const Dual<2> r = f(arg<2>(x, 0), arg<2>(y, 1));
        EXPECT_EQ(r.value, f(x, y));
        EXPECT_EQ(r.d[0], x < y ? y : 1.0);
        EXPECT_EQ(r.d[1], x < y ? x : 1.0);
    }
    const Dual<1> x = arg<1>(2.0, 0);
    EXPECT_TRUE(x == 2.0 && 2.0 == x && x != 3.0 && 3.0 != x);
    EXPECT_TRUE(x < 3.0 && 1.0 < x && x <= 2.0 && 2.0 <= x);
    EXPECT_TRUE(x > 1.0 && 3.0 > x && x >= 2.0 && 2.0 >= x);
}

TEST(AadDual, ConventionsAtKinks) {
    EXPECT_EQ(abs(arg<1>(0.0, 0)).d[0], 0.0);
    EXPECT_EQ(abs(arg<1>(-2.0, 0)).d[0], -1.0);

    const Dual<2> a = arg<2>(1.0, 0);
    const Dual<2> b = arg<2>(1.0, 1);
    EXPECT_EQ(min(a, b).d[0], 1.0);   // a tie returns the first
    EXPECT_EQ(max(a, b).d[0], 1.0);
    EXPECT_EQ(min(arg<2>(2.0, 0), b).d[1], 1.0);
    EXPECT_EQ(max(arg<2>(2.0, 0), b).d[0], 1.0);
    EXPECT_EQ(max(a, 0.0).d[0], 1.0);
    EXPECT_EQ(max(a, 5.0).d[0], 0.0);
    EXPECT_EQ(min(5.0, a).d[0], 1.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// One body, two types
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadDual, OneBodyCompilesAtBothTypes) {
    const auto f = [](const auto& s, const auto& v, const auto& t) {
        using std::exp; using std::log; using std::pow; using std::sqrt; using std::max;
        return max(log(s) * sqrt(t) + pow(v, 1.5) / exp(t), 0.0) - 2.0 * v;
    };
    const double s = 1.8, v = 0.6, t = 0.9;
    const Dual<3> r = f(arg<3>(s, 0), arg<3>(v, 1), arg<3>(t, 2));
    EXPECT_DOUBLE_EQ(r.value, f(s, v, t));

    const double h = 1e-6;
    const double fd[] = {
        (f(s + h, v, t) - f(s - h, v, t)) / (2 * h),
        (f(s, v + h, t) - f(s, v - h, t)) / (2 * h),
        (f(s, v, t + h) - f(s, v, t - h)) / (2 * h),
    };
    for (std::size_t i = 0; i < 3; ++i)
        EXPECT_NEAR(r.d[i], fd[i], 1e-7 * std::max(1.0, std::abs(fd[i]))) << "argument " << i;
}

TEST(AadDual, ChainLiftsAFunction) {
    const auto cdf = [](const Dual<1>& x) {
        return dag::aad::chain(x, normCdf(x.value), normPdf(x.value));
    };
    for (const double v : {-1.2, 0.0, 0.7}) {
        const Dual<1> r = cdf(arg<1>(v, 0));
        EXPECT_EQ(r.value, normCdf(v));
        EXPECT_EQ(r.d[0], normPdf(v));
        // The same function through erfc agrees.
        const Dual<1> viaErfc = 0.5 * erfc(-arg<1>(v, 0) / std::sqrt(2.0));
        EXPECT_NEAR(viaErfc.d[0], normPdf(v), 1e-15);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Zeros
// ─────────────────────────────────────────────────────────────────────────────

// A constant's derivative is 0, so its infinite or NaN partial reaches nothing.
TEST(AadDual, AZeroDerivativePropagatesNothing) {
    const Dual<1> x = arg<1>(-2.0, 0);
    EXPECT_EQ(pow(x, 3.0).d[0], 12.0);                // ln(−2) is NaN, times b's d of 0

    const Dual<1> zero = 0.0;
    EXPECT_EQ((x * sqrt(zero)).d[0], 0.0);            // √'s partial at 0 is ∞, times 0

    const Dual<1> inf = std::numeric_limits<double>::infinity();
    EXPECT_EQ((arg<1>(2.0, 0) * inf).d[0], std::numeric_limits<double>::infinity());
    EXPECT_EQ((inf * arg<1>(0.0, 0) + 1.0).d[0], std::numeric_limits<double>::infinity());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
