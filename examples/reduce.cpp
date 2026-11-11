// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// reduce.cpp — a subgraph as a function: an implied volatility by Newton's
// method.
//
// The graph prices a textbook Black–Scholes call from dag::ops nodes and
// aad::DifferentiableNodes. dag::reduce() turns the price into a function of
// the volatility, and Newton's method runs on that function. Each step is one
// call with its derivative, at a trial volatility the graph never holds: the
// graph does not move while the solver runs.
//
// Then the graph is asked. The volatility input is set to the answer and the
// price is evaluated. The last line says whether the function and the graph
// agree. The example doubles as a test, which matches that line.

#include <flywheel/dag.hpp>
#include <flywheel/dag_aad.hpp>
#include <flywheel/dag_ops.hpp>
#include <flywheel/dag_reduce.hpp>

#include <cmath>
#include <cstdio>
#include <exception>

int main() try {
    using namespace dag;

    auto spot   = Input<double>::make("spot", 100.0);
    auto strike = Input<double>::make("strike", 95.0);
    auto vol    = Input<double>::make("vol", 0.25);
    auto expiry = Input<double>::make("expiry", 0.75);
    auto rate   = Input<double>::make("rate", 0.03);

    // d1 = (ln(S/K) + (r + σ²/2)·T) / (σ·√T), one node over all five inputs.
    // Its functor is generic, so a reduction runs it at double for the value and
    // on dual numbers for the derivative.
    auto d1 = aad::DifferentiableNode<5>::make("d1", {spot, strike, vol, expiry, rate},
        [](const auto& s, const auto& k, const auto& v, const auto& t, const auto& r) {
            using std::log; using std::sqrt;
            return (log(s / k) + (r + 0.5 * v * v) * t) / (v * sqrt(t));
        },
        InvalidationMode::Lazy);

    // d2 = d1 − σ·√T, from ops.
    auto d2 = ops::DiffNode<>::make("d2", d1, ops::ProductNode<>::make("vol.sqrt(expiry)",
        {vol, ops::SqrtNode<>::make("sqrt(expiry)", expiry)}));

    // N(x), the standard normal distribution function.
    const auto normCdf = [](const auto& x) {
        using std::erfc; using std::sqrt;
        return 0.5 * erfc(-x / sqrt(2.0));
    };
    auto nd1 = aad::DifferentiableNode<1>::make("N(d1)", {d1}, normCdf, InvalidationMode::Lazy);
    auto nd2 = aad::DifferentiableNode<1>::make("N(d2)", {d2}, normCdf, InvalidationMode::Lazy);

    // S·N(d1) − K·e^(−r·T)·N(d2)
    auto discount = ops::ExpNode<>::make("discount", ops::NegateNode<>::make("-rate.expiry",
        ops::ProductNode<>::make("rate.expiry", {rate, expiry})));
    NodePtr call = ops::DiffNode<>::make("call",
        ops::ProductNode<>::make("spot.N(d1)", {spot, nd1}),
        ops::ProductNode<>::make("strike.discount.N(d2)", {strike, discount, nd2}));

    // A reduction reads only evaluated values, so evaluate first.
    EvalContext ctx;
    const double held = get_value<double>(call->eval(ctx));

    // The price as a function of vol. The nodes below vol are its steps, and
    // everything else is read from the graph once, here.
    Reduction price = reduce(call, {vol});
    std::printf("call %.10f at vol %.2f; price(vol) runs %zu of the graph's 17 nodes\n\n",
                held, vol->get(), price.size());

    // Newton's method on price(vol) − target, from 0.2.
    constexpr double kTarget   = 12.0;
    constexpr int    kMaxSteps = 20;
    double trial = 0.2;
    double vega  = 0.0;
    int    steps = 0;
    std::printf("%-5s %14s %16s %16s\n", "step", "vol", "price", "vega");
    for (; steps < kMaxSteps; ++steps) {
        const double value = price.gradient({trial}, {&vega, 1});
        std::printf("%-5d %14.10f %16.10f %16.10f\n", steps, trial, value, vega);
        if (std::abs(value - kTarget) < 1e-12) break;
        trial -= (value - kTarget) / vega;
    }
    const double fromFunction = price({trial});

    // The solver moved nothing: the graph holds the vol and the price it held.
    const bool untouched = vol->get() == 0.25 && !call->dirty()
                        && get_value<double>(call->eval(ctx)) == held;

    // Ask the graph: set the input to the answer and evaluate.
    vol->set(trial);
    const double fromGraph = get_value<double>(call->eval(ctx));

    std::printf("\nimplied vol %.10f after %d steps\n", trial, steps);
    std::printf("the price there: %.17g from the function, %.17g from the graph\n",
                fromFunction, fromGraph);

    const bool agree = steps < kMaxSteps && untouched && fromFunction == fromGraph
                    && std::abs(fromGraph - kTarget) < 1e-12;
    std::printf("\n%s\n", agree ? "the function and the graph agree"
                                : "the function and the graph DISAGREE");
    return agree ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "reduce: %s\n", e.what());
    return 1;
}
