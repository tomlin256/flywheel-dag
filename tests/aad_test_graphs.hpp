// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// aad_test_graphs.hpp — small graphs with known derivatives, shared by the
// algorithmic-differentiation suites.

#include <gtest/gtest.h>
#include "flywheel/dag.hpp"
#include "flywheel/dag_ops.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace aad_test {

/// Equal to 1e-13, relative to the larger of 1 and the expected value.
inline void expectClose(double actual, double expected) {
    EXPECT_NEAR(actual, expected, 1e-13 * std::max(1.0, std::abs(expected)));
}

/// x·y + exp(x)/y
struct SumOfTerms {
    dag::InputPtr<double> x = dag::Input<double>::make("x", 1.3);
    dag::InputPtr<double> y = dag::Input<double>::make("y", 0.7);
    dag::NodePtr root = dag::ops::SumNode<>::make("root", {
        dag::ops::ProductNode<>::make("xy", {x, y}),
        dag::ops::DivideNode<>::make("exp/y", dag::ops::ExpNode<>::make("exp", x), y)});

    std::vector<dag::InputPtr<double>> inputs() const { return {x, y}; }
    std::vector<double> gradient() const {
        const double a = x->get(), b = y->get();
        return {b + std::exp(a) / b, a - std::exp(a) / (b * b)};
    }
};

/// ln(x)·√y
struct LogTimesRoot {
    dag::InputPtr<double> x = dag::Input<double>::make("x", 2.5);
    dag::InputPtr<double> y = dag::Input<double>::make("y", 3.0);
    dag::NodePtr root = dag::ops::ProductNode<>::make("root", {
        dag::ops::LnNode<>::make("ln", x), dag::ops::SqrtNode<>::make("sqrt", y)});

    std::vector<dag::InputPtr<double>> inputs() const { return {x, y}; }
    std::vector<double> gradient() const {
        const double a = x->get(), b = y->get();
        return {std::sqrt(b) / a, std::log(a) / (2.0 * std::sqrt(b))};
    }
};

/// (x − y)^z
struct PowerOfDifference {
    dag::InputPtr<double> x = dag::Input<double>::make("x", 3.5);
    dag::InputPtr<double> y = dag::Input<double>::make("y", 1.25);
    dag::InputPtr<double> z = dag::Input<double>::make("z", 1.8);
    dag::NodePtr root = dag::ops::PowerNode<>::make(
        "root", dag::ops::DiffNode<>::make("x-y", x, y), z);

    std::vector<dag::InputPtr<double>> inputs() const { return {x, y, z}; }
    std::vector<double> gradient() const {
        const double d = x->get() - y->get(), e = z->get();
        const double da = e * std::pow(d, e - 1.0);
        return {da, -da, std::pow(d, e) * std::log(d)};
    }
};

/// sin(x)·cos(y) + tan(x/y)
struct TrigOfRatio {
    dag::InputPtr<double> x = dag::Input<double>::make("x", 0.7);
    dag::InputPtr<double> y = dag::Input<double>::make("y", 1.9);
    dag::NodePtr root = dag::ops::SumNode<>::make("root", {
        dag::ops::ProductNode<>::make("sin*cos", {dag::ops::SinNode<>::make("sin", x),
                                                  dag::ops::CosNode<>::make("cos", y)}),
        dag::ops::TanNode<>::make("tan", dag::ops::DivideNode<>::make("x/y", x, y))});

    std::vector<dag::InputPtr<double>> inputs() const { return {x, y}; }
    std::vector<double> gradient() const {
        const double a = x->get(), b = y->get();
        const double c = std::cos(a / b);
        const double sec2 = 1.0 / (c * c);
        return {std::cos(a) * std::cos(b) + sec2 / b,
                -std::sin(a) * std::sin(b) - a * sec2 / (b * b)};
    }
};

/// asin(x·y) + acos(x) + atan2(y, x)·atan(y)
struct InverseTrig {
    dag::InputPtr<double> x = dag::Input<double>::make("x", 0.35);
    dag::InputPtr<double> y = dag::Input<double>::make("y", 0.8);
    dag::NodePtr root = dag::ops::SumNode<>::make("root", {
        dag::ops::AsinNode<>::make("asin", dag::ops::ProductNode<>::make("xy", {x, y})),
        dag::ops::AcosNode<>::make("acos", x),
        dag::ops::ProductNode<>::make("atan2*atan", {dag::ops::Atan2Node<>::make("atan2", y, x),
                                                     dag::ops::AtanNode<>::make("atan", y)})});

    std::vector<dag::InputPtr<double>> inputs() const { return {x, y}; }
    std::vector<double> gradient() const {
        const double a = x->get(), b = y->get();
        const double rootOfXy = std::sqrt(1.0 - a * a * b * b);
        const double r2 = a * a + b * b;
        return {b / rootOfXy - 1.0 / std::sqrt(1.0 - a * a) - b * std::atan(b) / r2,
                a / rootOfXy + a * std::atan(b) / r2 + std::atan2(b, a) / (1.0 + b * b)};
    }
};

/// atan2(r·sin φ, r·cos φ), which is φ. Its ∂/∂r is 0 only because the two
/// paths from r cancel.
struct PolarRoundTrip {
    dag::InputPtr<double> r   = dag::Input<double>::make("r", 1.5);
    dag::InputPtr<double> phi = dag::Input<double>::make("phi", 2.0);
    dag::NodePtr root = dag::ops::Atan2Node<>::make("root",
        dag::ops::ProductNode<>::make("y", {r, dag::ops::SinNode<>::make("sin", phi)}),
        dag::ops::ProductNode<>::make("x", {r, dag::ops::CosNode<>::make("cos", phi)}));

    std::vector<dag::InputPtr<double>> inputs() const { return {r, phi}; }
    std::vector<double> gradient() const { return {0.0, 1.0}; }
};

/// The nodes of `inputs` as NodePtrs, for a wrt list.
inline std::vector<dag::NodePtr> asNodes(const std::vector<dag::InputPtr<double>>& inputs) {
    return {inputs.begin(), inputs.end()};
}

} // namespace aad_test
