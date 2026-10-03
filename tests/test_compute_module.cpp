// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_compute_module.cpp — unit tests for IComputeModule and Engine::install()
//
// Tests:
//   1. install() calls wire() exactly once
//   2. install() keeps the module alive (shared_ptr stored in engine)
//   3. wire() receives the correct engine reference
//   4. Multiple modules — all wire() methods called
//   5. wire() can register engine outputs (integration smoke test)

#include <gtest/gtest.h>
#include "flywheel/dag_compute_module.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag.hpp"
#include <atomic>
#include <memory>
#include <string>

using namespace dag;
using namespace dag::async;

// ─────────────────────────────────────────────────────────────────────────────
// Test helper: records wire() calls
// ─────────────────────────────────────────────────────────────────────────────
struct SpyModule : public IComputeModule {
    std::string name_;
    int         wireCalls  = 0;
    Engine*     wiredTo    = nullptr;

    explicit SpyModule(std::string n) : name_(std::move(n)) {}

    std::string name() const override { return name_; }

    void wire(Engine& e) override {
        ++wireCalls;
        wiredTo = &e;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. install() calls wire() exactly once
// ─────────────────────────────────────────────────────────────────────────────
TEST(ComputeModule, InstallCallsWireExactlyOnce)
{
    Engine engine;
    auto mod = std::make_shared<SpyModule>("once");
    engine.install(mod);
    EXPECT_EQ(mod->wireCalls, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. install() keeps the module alive past the caller's scope
// ─────────────────────────────────────────────────────────────────────────────
TEST(ComputeModule, InstallKeepsModuleAlive)
{
    Engine engine;
    std::weak_ptr<SpyModule> weak;
    {
        auto mod = std::make_shared<SpyModule>("alive");
        weak = mod;
        engine.install(mod);
    }  // local shared_ptr drops here
    EXPECT_FALSE(weak.expired());  // engine still holds it
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. wire() receives the correct Engine reference
// ─────────────────────────────────────────────────────────────────────────────
TEST(ComputeModule, WireReceivesCorrectEngineReference)
{
    Engine engine;
    auto mod = std::make_shared<SpyModule>("ref");
    engine.install(mod);
    EXPECT_EQ(mod->wiredTo, &engine);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Multiple modules — all wire() methods called
// ─────────────────────────────────────────────────────────────────────────────
TEST(ComputeModule, MultipleModulesAllWired)
{
    Engine engine;
    auto m1 = std::make_shared<SpyModule>("m1");
    auto m2 = std::make_shared<SpyModule>("m2");
    auto m3 = std::make_shared<SpyModule>("m3");
    engine.install(m1);
    engine.install(m2);
    engine.install(m3);
    EXPECT_EQ(m1->wireCalls, 1);
    EXPECT_EQ(m2->wireCalls, 1);
    EXPECT_EQ(m3->wireCalls, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. wire() can register engine outputs (integration smoke)
//    Module uses engine.makeInput<double>() inside wire() and registers an
//    output callback.  Changing the input wakes the engine; callback fires.
// ─────────────────────────────────────────────────────────────────────────────
struct OutputRegistrationModule : public IComputeModule {
    dag::InputPtr<double> input;
    std::atomic<int> callbackCount{0};
    double           lastValue = 0.0;

    std::string name() const override { return "output-reg"; }

    void wire(Engine& e) override {
        input = e.makeInput<double>("x", 0.0);
        e.addOutput<double>(input, [this](const double& v) {
            lastValue = v;
            ++callbackCount;
        });
    }
};

TEST(ComputeModule, WireCanRegisterEngineOutputs)
{
    Engine engine;
    auto mod = std::make_shared<OutputRegistrationModule>();
    engine.install(mod);

    mod->input->set(42.0);
    engine.step();

    EXPECT_GT(mod->callbackCount.load(), 0);
    EXPECT_DOUBLE_EQ(mod->lastValue, 42.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// addFeedback tests
// ─────────────────────────────────────────────────────────────────────────────

// addFeedback propagates a node's output to an Input on the next engine step.
TEST(ComputeModule, AddFeedbackPropagatesValueToInput)
{
    Engine engine;

    // source → double_node → via addFeedback → feedback_input
    auto source         = engine.makeInput<double>("source", 0.0);
    auto double_node    = ComputeNode<double, double>::make(
        "double", std::make_tuple(std::static_pointer_cast<INode>(source)),
        [](const double& x) { return x * 2.0; });
    auto feedback_input = engine.makeInput<double>("feedback", -1.0);

    engine.addFeedback<double>(
        std::static_pointer_cast<INode>(double_node), feedback_input);

    double captured = -1.0;
    engine.addOutput<double>(feedback_input,
        [&](const double& v) { captured = v; });

    // Seed cycle: source = 5.0 → double_node fires 10.0 → feedback_input set to 10.0
    source->set(5.0);
    engine.step();
    // After this step: feedback_input was set to 10.0 (via addFeedback callback).
    // One more step to see feedback_input's callback fire.
    engine.step();

    EXPECT_DOUBLE_EQ(captured, 10.0);
}

// addFeedback respects the equality policy: no spurious wakeup when value unchanged.
TEST(ComputeModule, AddFeedbackNoWakeupWhenValueUnchanged)
{
    Engine engine;

    auto source      = engine.makeInput<double>("src", 3.0);
    auto double_node = ComputeNode<double, double>::make(
        "double", std::make_tuple(std::static_pointer_cast<INode>(source)),
        [](const double& x) { return x * 2.0; });
    auto feedback    = engine.makeInput<double>("fb", 0.0);

    engine.addFeedback<double>(std::static_pointer_cast<INode>(double_node), feedback);

    int callCount = 0;
    engine.addOutput<double>(feedback, [&](const double&) { ++callCount; });

    // Cycle 1: source fires 3.0 → double = 6.0 → feedback set to 6.0
    source->set(3.0);
    engine.step();
    engine.step();   // feedback callback fires once here

    int countAfterFirstChange = callCount;

    // Cycle 3: source unchanged → double unchanged → feedback not set again
    engine.step();
    EXPECT_EQ(callCount, countAfterFirstChange);  // no extra callback
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
