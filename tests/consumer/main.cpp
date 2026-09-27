// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// main.cpp — the smallest useful program against flywheel::dag. Exits non-zero
// if the graph computes the wrong values, so the consumer test checks behaviour,
// not just that it links.

#include <flywheel/dag.hpp>

#include <cstdio>

int main() {
    using namespace dag;

    auto a   = Input<double>::make("a", 2.0);
    auto b   = Input<double>::make("b", 3.0);
    auto sum = ComputeNode<double, double, double>::make(
        "sum", {a, b}, [](const double& x, const double& y) { return x + y; });

    EvalContext ctx;
    const double first = get_value<double>(sum->eval(ctx));
    a->set(4.0);
    const double second = get_value<double>(sum->eval(ctx));

    std::printf("sum=%g then %g\n", first, second);
    return (first == 5.0 && second == 7.0) ? 0 : 1;
}
