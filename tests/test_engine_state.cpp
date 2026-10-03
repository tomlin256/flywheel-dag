// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_engine_state.cpp — unit tests for Engine::setStateStore/saveState/restoreState
//
// Tests:
//   SaveRestoreRoundtrip        — save mid-run, restore into fresh engine; outputs match
//   NoStoreSaveIsNoop           — saveState() with no store set completes silently
//   NoStoreRestoreReturnsFalse  — restoreState() with no store returns false
//   RestoreReturnsFalseOnCold   — store has no saved state → restoreState() false
//   RestoreReturnsTrueAfterSave — store has saved state → restoreState() true

#include <gtest/gtest.h>

#include "flywheel/dag_compute_module.hpp"
#include "flywheel/dag.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_state_store.hpp"
#include "flywheel/dag_timeseries.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace dag;
using namespace dag::async;
using namespace dag::ts;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: a simple module that owns one EWMA node over a shared Input<double>
// ─────────────────────────────────────────────────────────────────────────────
struct EwmaModule : public IComputeModule {
    std::string                        name_;
    std::shared_ptr<Input<double>>     inp_;
    std::shared_ptr<EWMANode>          ewma_;
    double                             alpha_;

    EwmaModule(std::string name,
               std::shared_ptr<Input<double>> inp,
               double alpha)
        : name_(std::move(name)), inp_(std::move(inp)), alpha_(alpha) {}

    std::string name() const override { return name_; }

    void wire(Engine& engine) override {
        ewma_ = EWMANode::make(name_ + ".ewma", inp_, alpha_);
        // Registered as an output so Engine::discoverStatefulNodes() reaches it.
        // The callback is a no-op: this test drives eval() itself.
        engine.addOutput<double>(ewma_, [](const double&) {});
    }
};

// Warm an EWMA by feeding values into inp and evaluating ewma manually
static void warmEwma(std::shared_ptr<Input<double>> inp,
                     std::shared_ptr<EWMANode> ewma,
                     const std::vector<double>& values)
{
    EvalContext ctx;
    for (double v : values) { inp->set(v); ewma->eval(ctx); }
}

// ─────────────────────────────────────────────────────────────────────────────
// Strategy: warm an EWMA through N values and save its state.  Build a fresh
// EWMA (cold), restore into it.  Feed the same M values to both original and
// restored; outputs must match.
// ─────────────────────────────────────────────────────────────────────────────
TEST(EngineStateTests, SaveRestoreRoundtrip) {
    const double alpha = 0.3;
    const std::vector<double> warmValues = {10.0, 15.0, 12.0, 18.0, 11.0};
    const std::vector<double> extraValues = {9.0, 14.0, 16.0};

    // Original engine — warm then save
    auto inp1 = Input<double>::make("x", 0.0);
    auto mod1 = std::make_shared<EwmaModule>("ewma", inp1, alpha);

    Engine e1;
    e1.install(mod1);

    auto store = std::make_shared<InMemoryStateStore>();
    e1.setStateStore(store);

    warmEwma(inp1, mod1->ewma_, warmValues);
    e1.saveState();

    // Restored engine
    auto inp2 = Input<double>::make("x2", 0.0);
    auto mod2 = std::make_shared<EwmaModule>("ewma", inp2, alpha);

    Engine e2;
    e2.install(mod2);
    e2.setStateStore(store);
    EXPECT_TRUE(e2.restoreState());

    // Feed extra values to both
    EvalContext ctx;
    for (double v : extraValues) {
        inp1->set(v); mod1->ewma_->eval(ctx);
        inp2->set(v); mod2->ewma_->eval(ctx);
    }

    EXPECT_NEAR(get_value<double>(mod1->ewma_->eval(ctx)),
                get_value<double>(mod2->ewma_->eval(ctx)), 1e-9);
}

TEST(EngineStateTests, NoStoreSaveIsNoop) {
    auto inp = Input<double>::make("x", 0.0);
    auto mod = std::make_shared<EwmaModule>("ewma", inp, 0.5);

    Engine engine;
    engine.install(mod);
    // No store set — must not throw
    EXPECT_NO_THROW(engine.saveState());
}

TEST(EngineStateTests, NoStoreRestoreReturnsFalse) {
    auto inp = Input<double>::make("x", 0.0);
    auto mod = std::make_shared<EwmaModule>("ewma", inp, 0.5);

    Engine engine;
    engine.install(mod);
    EXPECT_FALSE(engine.restoreState());
}

// Store set but no prior save: the store's restore() returns false.
TEST(EngineStateTests, RestoreReturnsFalseOnCold) {
    auto inp = Input<double>::make("x", 0.0);
    auto mod = std::make_shared<EwmaModule>("ewma", inp, 0.5);

    Engine engine;
    engine.install(mod);
    engine.setStateStore(std::make_shared<InMemoryStateStore>());
    EXPECT_FALSE(engine.restoreState());
}

TEST(EngineStateTests, RestoreReturnsTrueAfterSave) {
    auto inp = Input<double>::make("x", 0.0);
    auto mod = std::make_shared<EwmaModule>("ewma", inp, 0.5);

    Engine engine;
    engine.install(mod);
    auto store = std::make_shared<InMemoryStateStore>();
    engine.setStateStore(store);

    engine.saveState();
    EXPECT_TRUE(engine.restoreState());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
