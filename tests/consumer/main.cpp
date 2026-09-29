// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// main.cpp — the smallest useful program against flywheel::dag. Exits non-zero
// if the graph computes the wrong values, so the consumer tests check behaviour,
// not just that it links.
//
// It also saves a node's state through JsonFileStateStore and restores it into
// a fresh node. That is the engine code that uses both of its dependencies:
// nlohmann/json writes the snapshot and spdlog logs the save. So the program
// cannot build, link or run unless the consumer found both (flywheel-dag#2).

#include <flywheel/dag.hpp>
#include <flywheel/dag_state_store.hpp>
#include <flywheel/dag_timeseries.hpp>

#include <cstdio>
#include <filesystem>

int main(int /*argc*/, char* argv[]) {
    using namespace dag;

    auto a   = Input<double>::make("a", 2.0);
    auto b   = Input<double>::make("b", 3.0);
    auto sum = ComputeNode<double, double, double>::make(
        "sum", {a, b}, [](const double& x, const double& y) { return x + y; });

    EvalContext ctx;
    const double first = get_value<double>(sum->eval(ctx));
    a->set(4.0);
    const double second = get_value<double>(sum->eval(ctx));

    // The snapshot goes beside the executable, so consumers built in different
    // directories never share one, whatever directory ctest runs them from.
    const std::filesystem::path snapshot =
        std::filesystem::absolute(argv[0]).parent_path() / "consumer_state.json";

    auto in   = Input<double>::make("in", 0.0);
    auto ewma = ts::EWMANode::make("ewma", in, 0.5);
    for (const double v : {10.0, 20.0}) {
        in->set(v);
        ewma->eval(ctx);
    }
    JsonFileStateStore(snapshot).save({ewma});

    auto in2      = Input<double>::make("in", 0.0);
    auto restored = ts::EWMANode::make("ewma", in2, 0.5);
    const bool found = JsonFileStateStore(snapshot).restore({restored});
    std::filesystem::remove(snapshot);

    // Fed the same value, a restored node computes what the original does. A
    // node that started cold would output the new value itself.
    in->set(30.0);
    in2->set(30.0);
    const ValuePtr original = ewma->eval(ctx);
    const ValuePtr copy     = restored->eval(ctx);
    const double   want     = get_value<double>(original);
    const double   got      = get_value<double>(copy);

    std::printf("sum=%g then %g; ewma=%g, restored ewma=%g\n", first, second, want, got);
    return (first == 5.0 && second == 7.0 && found && got == want) ? 0 : 1;
}
