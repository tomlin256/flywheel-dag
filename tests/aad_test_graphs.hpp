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
// algorithmic-differentiation suites (flywheel-dag#10).

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

/// The nodes of `inputs` as NodePtrs, for a wrt list.
inline std::vector<dag::NodePtr> asNodes(const std::vector<dag::InputPtr<double>>& inputs) {
    return {inputs.begin(), inputs.end()};
}

} // namespace aad_test
