// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// aad.cpp — an output's sensitivities, taken both ways and checked by bumping.
//
// The graph prices a textbook Black–Scholes call from dag::ops nodes and
// aad::DifferentiableNodes. One reverse sweep gives the price's derivative with
// respect to all five inputs. Five forward sweeps give the same five, one input
// at a time. Both are checked against central differences: each input bumped
// up and down through set(), and the graph evaluated again.
//
// The last line says whether all three agree. The example doubles as a test,
// which matches that line.

#include <flywheel/dag.hpp>
#include <flywheel/dag_aad.hpp>
#include <flywheel/dag_ops.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

bool runBlackScholesExample() {
    using namespace dag;

    auto spot   = Input<double>::make("spot", 100.0);
    auto strike = Input<double>::make("strike", 95.0);
    auto vol    = Input<double>::make("vol", 0.25);
    auto expiry = Input<double>::make("expiry", 0.75);
    auto rate   = Input<double>::make("rate", 0.03);

    // d1 = (ln(S/K) + (r + σ²/2)·T) / (σ·√T): one differentiable node over all
    // five inputs. The functor is written once, generically, and a tape runs it
    // on dual numbers for its partials.
    auto d1 = aad::DifferentiableNode<5>::make("d1", {spot, strike, vol, expiry, rate},
        [](const auto& s, const auto& k, const auto& v, const auto& t, const auto& r) {
            using std::log; using std::sqrt;
            return (log(s / k) + (r + 0.5 * v * v) * t) / (v * sqrt(t));
        },
        InvalidationMode::Lazy);

    // d2 = d1 − σ·√T, from ops, whose partials are closed-form.
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

    // A tape reads only evaluated values, so evaluate first.
    EvalContext ctx;
    const double price = get_value<double>(call->eval(ctx));

    const std::vector<InputPtr<double>> inputs = {spot, strike, vol, expiry, rate};
    const std::vector<NodePtr> wrt(inputs.begin(), inputs.end());

    // Reverse: all five sensitivities in one sweep.
    const aad::Tape tape({call});
    const std::vector<double> reverse = tape.adjoints(call, wrt);

    std::printf("call %.10f, from a tape of %zu nodes\n\n", price, tape.size());
    std::printf("%-8s %16s %16s %16s\n", "input", "reverse", "forward", "bumped");

    bool agree = true;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        // Forward: one sweep for this input.
        const double forward = tape.tangents({{inputs[i], 1.0}})[0];

        // A central difference. Bumping moves the graph, so the tape, which
        // recorded the partials at the original point, is swept first.
        const double x  = inputs[i]->get();
        const double h  = 1e-5 * std::max(1.0, std::abs(x));
        inputs[i]->set(x + h);
        const double up = get_value<double>(call->eval(ctx));
        inputs[i]->set(x - h);
        const double down = get_value<double>(call->eval(ctx));
        inputs[i]->set(x);
        call->eval(ctx);
        const double bumped = (up - down) / (2.0 * h);

        std::printf("%-8s %16.10f %16.10f %16.10f\n",
                    inputs[i]->name().c_str(), reverse[i], forward, bumped);
        const double scale = std::max(1.0, std::abs(reverse[i]));
        agree = agree && std::abs(forward - reverse[i]) <= 1e-12 * scale
                      && std::abs(bumped - reverse[i]) <= 1e-6 * scale;
    }

    std::printf("\n%s\n", agree ? "reverse, forward and bumped agree"
                                : "reverse, forward and bumped DISAGREE");
    return agree;
}

bool runPolynomialExample() {
    using namespace dag;

    constexpr double xValue = 2.0;
    auto x = Input<double>::make("x", xValue);
    auto two = Input<double>::make("two", 2.0);
    auto ten = Input<double>::make("ten", 10.0);

    auto square = ops::ProductNode<>::make("x^2", {x, x});
    auto twiceSquare = ops::ProductNode<>::make("2*x^2", {two, square});
    auto opsResult = ops::SumNode<>::make("ops polynomial", {twiceSquare, x, ten});
    auto genericResult = aad::DifferentiableNode<1>::make(
        "generic polynomial", {x},
        [](const auto& value) { return 2.0 * value * value + value + 10.0; },
        InvalidationMode::Lazy);

    EvalContext ctx;
    const double opsValue = get_value<double>(opsResult->eval(ctx));
    const double genericValue = get_value<double>(genericResult->eval(ctx));
    const aad::Tape tape({opsResult, genericResult});
    const std::vector<NodePtr> wrt = {x};
    const double opsDerivative = tape.adjoints(opsResult, wrt)[0];
    const double genericDerivative = tape.adjoints(genericResult, wrt)[0];
    const double expectedValue = 2.0 * xValue * xValue + xValue + 10.0;
    const double expectedDerivative = 4.0 * xValue + 1.0;

    const bool agree = std::abs(opsValue - expectedValue) <= 1e-12
        && std::abs(genericValue - expectedValue) <= 1e-12
        && std::abs(opsDerivative - expectedDerivative) <= 1e-12
        && std::abs(genericDerivative - expectedDerivative) <= 1e-12;

    std::printf("polynomial at x=%.1f: ops=%.10f generic=%.10f\n",
                xValue, opsValue, genericValue);
    std::printf("derivative: ops=%.10f generic=%.10f expected=%.10f\n",
                opsDerivative, genericDerivative, expectedDerivative);
    std::printf("%s\n", agree ? "ops and generic polynomial values and derivatives agree"
                                : "ops and generic polynomial values and derivatives DISAGREE");
    return agree;
}

int main() {
    const bool blackScholesAgrees = runBlackScholesExample();
    const bool polynomialAgrees = runPolynomialExample();
    return blackScholesAgrees && polynomialAgrees ? 0 : 1;
}
