// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// test_nodes.hpp — always-dirty test nodes, which stand in for an
// application's clock-driven node: dirty() is always true, so every pull
// evaluates them again.

#include "flywheel/dag.hpp"
#include <memory>
#include <string>
#include <vector>

namespace test_nodes {

/// Always dirty, and tells its consumers each time it is pulled. Its value is a
/// constant, so a test can name the exact value or derivative through it.
class AlwaysFiring : public dag::NodeBase, public std::enable_shared_from_this<AlwaysFiring> {
public:
    static std::shared_ptr<AlwaysFiring> make(double value) {
        return std::shared_ptr<AlwaysFiring>(new AlwaysFiring(value));
    }
    bool dirty() const override { return true; }
    dag::ValuePtr eval(dag::EvalContext&) override {
        notifyDownstream();
        return value_;
    }
    std::string name() const override { return "always-firing"; }
    std::vector<dag::NodePtr> inputs() const override { return {}; }
    dag::NodeKind kind() const override { return dag::NodeKind::Input; }

private:
    explicit AlwaysFiring(double value) : value_(dag::make_value(value)) {}
    dag::ValuePtr value_;
};

/// Always dirty, with a constant value, and tells its consumers once: on its
/// n-th pull after arm(n). A clock-driven node whose value moves at one pull,
/// at a pull the test chooses.
class Tripwire : public dag::NodeBase, public std::enable_shared_from_this<Tripwire> {
public:
    static std::shared_ptr<Tripwire> make(double value) {
        return std::shared_ptr<Tripwire>(new Tripwire(value));
    }
    void arm(int pulls) { pullsLeft_ = pulls; }
    bool dirty() const override { return true; }
    dag::ValuePtr eval(dag::EvalContext&) override {
        if (pullsLeft_ > 0) {
            --pullsLeft_;
            if (pullsLeft_ == 0) notifyDownstream();
        }
        return value_;
    }
    std::string name() const override { return "tripwire"; }
    std::vector<dag::NodePtr> inputs() const override { return {}; }
    dag::NodeKind kind() const override { return dag::NodeKind::Input; }

private:
    explicit Tripwire(double value) : value_(dag::make_value(value)) {}
    dag::ValuePtr value_;
    int pullsLeft_ = 0;
};

} // namespace test_nodes
