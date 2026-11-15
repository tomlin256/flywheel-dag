// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// reduce_test_nodes.hpp — the nodes of the dag::Reduction suites' own, the
// Black–Scholes call they share, and two helpers for the messages they read.

#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_reduce.hpp"
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace reduce_test {

using namespace dag;

// ─────────────────────────────────────────────────────────────────────────────
// Three nodes of the suites' own. Two override NodeBase's reduction hooks, so
// that what bind() does with what a node reads is held without any engine node.
// The third derives INode alone, and so has no hooks.
// ─────────────────────────────────────────────────────────────────────────────

/// What a Counted gives a reduction: no step, a step, or a step whose partials
/// name an input the node does not have.
enum class Gives : std::uint8_t { NoStep, AStep, AStepThatMisnamesItsInput };

/// A pure function of one input, 2·a + 1, that counts each time it is computed:
/// by its own eval() and by a step alike. In is the type its input holds. Its
/// step has the one partial, 2.
template<typename In = double>
class Counted
    : public NodeBase
    , public std::enable_shared_from_this<Counted<In>>
{
public:
    static std::shared_ptr<Counted> make(std::string name, NodePtr in,
                                         Gives gives = Gives::AStep) {
        auto self = std::shared_ptr<Counted>(new Counted(std::move(name), std::move(in), gives));
        wire(self, self->inputs());
        return self;
    }

    int calls() const noexcept { return calls_; }

    ValuePtr eval(EvalContext& ctx) override {
        if (!dirty() && !ctx.forceRecompute) return cached_;
        beginEval();
        const ValuePtr v = in_->eval(ctx);
        if (skipRecompute(ctx)) { endEval(); return cached_; }
        cached_ = slot_.emit(compute(get_value<In>(v)));
        notifyDownstream();
        endEval();
        return cached_;
    }
    std::string name() const override { return name_; }
    std::vector<NodePtr> inputs() const override { return {in_}; }
    NodeKind kind() const override { return NodeKind::Compute; }

    std::unique_ptr<IReduceStep> reduceStep() override {
        if (gives_ == Gives::NoStep) return nullptr;
        return std::make_unique<Step>(this->shared_from_this());
    }

private:
    Counted(std::string name, NodePtr in, Gives gives)
        : NodeBase(InvalidationMode::Lazy), name_(std::move(name)), in_(std::move(in))
        , gives_(gives) {}

    double compute(const In& a) {
        ++calls_;
        return 2.0 * static_cast<double>(a) + 1.0;
    }

    class Step final : public IReduceStep {
    public:
        explicit Step(std::shared_ptr<Counted> node) : node_(std::move(node)) {}
        ValuePtr apply(const ReduceInputs& in) override {
            return slot_.emit(node_->compute(get_value<In>(in[0])));
        }
        bool partials(const ReduceInputs&, aad::Partials& out) override {
            out.add(node_->gives_ == Gives::AStepThatMisnamesItsInput ? 1 : 0, 2.0);
            return true;
        }
    private:
        std::shared_ptr<Counted> node_;
        ValueSlot<double>        slot_;
    };

    std::string       name_;
    NodePtr           in_;
    Gives             gives_;
    ValuePtr          cached_;
    ValueSlot<double> slot_;
    int               calls_ = 0;
};

/// Selects one of two inputs by a third: `first` while `which` holds 0, and
/// `second` otherwise. Like a ConditionNode, it pulls and reads only the input
/// it selects, and `which` decides which that is. hold(v) freezes it at v, and
/// it then reads no input at all, as a tweaked node does.
class Chooser
    : public NodeBase
    , public std::enable_shared_from_this<Chooser>
{
public:
    static std::shared_ptr<Chooser> make(std::string name, NodePtr which, NodePtr first,
                                         NodePtr second) {
        auto self = std::shared_ptr<Chooser>(
            new Chooser(std::move(name), std::move(which), std::move(first), std::move(second)));
        wire(self, self->inputs());
        return self;
    }

    void hold(double v) {
        held_   = true;
        cached_ = make_value(v);
        markDirty();
        notifyDownstream();
    }

    ValuePtr eval(EvalContext& ctx) override {
        if (held_) { markClean(); return cached_; }
        if (!dirty() && !ctx.forceRecompute) return cached_;
        beginEval();
        const ValuePtr which = which_->eval(ctx);
        cached_ = (get_value<double>(which) == 0.0 ? first_ : second_)->eval(ctx);
        notifyDownstream();
        endEval();
        return cached_;
    }
    std::string name() const override { return name_; }
    std::vector<NodePtr> inputs() const override { return {which_, first_, second_}; }
    NodeKind kind() const override { return NodeKind::Compute; }

    std::unique_ptr<IReduceStep> reduceStep() override { return std::make_unique<Step>(); }

    void reduceReads(EvalContext& ctx, ReduceReads& out) override {
        if (held_) return;
        const ValuePtr which = which_->eval(ctx);
        out.decides(0);
        out.reads(get_value<double>(which) == 0.0 ? 1 : 2);
    }

private:
    Chooser(std::string name, NodePtr which, NodePtr first, NodePtr second)
        : name_(std::move(name)), which_(std::move(which)), first_(std::move(first))
        , second_(std::move(second)) {}

    class Step final : public IReduceStep {
    public:
        ValuePtr apply(const ReduceInputs& in) override {
            return in[get_value<double>(in[0]) == 0.0 ? 1 : 2];
        }
    };

    std::string name_;
    NodePtr     which_, first_, second_;
    ValuePtr    cached_;
    bool        held_ = false;
};

/// −a, as a node that derives INode alone and keeps the dirty protocol by hand.
/// The engine's nodes all derive NodeBase, and a node of your own should: this
/// one is here because a reduction can meet a node that does not.
class Bare : public INode, public std::enable_shared_from_this<Bare> {
public:
    static std::shared_ptr<Bare> make(std::string name, NodePtr in) {
        auto self = std::shared_ptr<Bare>(new Bare(std::move(name), std::move(in)));
        self->in_->addDownstream(self);
        return self;
    }

    ValuePtr eval(EvalContext& ctx) override {
        if (!dirty_ && !ctx.forceRecompute) return cached_;
        const ValuePtr v = in_->eval(ctx);
        cached_ = make_value(-get_value<double>(v));
        dirty_  = false;
        for (auto& w : downstream_) if (auto n = w.lock()) n->invalidate();
        return cached_;
    }
    std::string name() const override { return name_; }
    std::vector<NodePtr> inputs() const override { return {in_}; }
    void invalidate() override { mark(); }
    void invalidateMaybe() override { mark(); }
    bool dirty() const override { return dirty_; }
    void addDownstream(std::weak_ptr<INode> n) override { downstream_.push_back(std::move(n)); }
    NodeKind kind() const override { return NodeKind::Compute; }

private:
    Bare(std::string name, NodePtr in) : name_(std::move(name)), in_(std::move(in)) {}

    void mark() {
        if (dirty_) return;
        dirty_ = true;
        for (auto& w : downstream_) if (auto n = w.lock()) n->invalidateMaybe();
    }

    std::string                       name_;
    NodePtr                           in_;
    ValuePtr                          cached_;
    bool                              dirty_ = true;
    std::vector<std::weak_ptr<INode>> downstream_;
};

/// A Black–Scholes call, from ops and differentiable nodes: d1 is one
/// differentiable node over all five inputs, d2 is ops over d1, and each normal
/// CDF is a differentiable node over one input. Seventeen nodes, five of them
/// inputs, and eight of the other twelve below vol. The spot is an Input of 100
/// unless the test brings a node of its own, such as an engine's source.
struct CallPrice {
    NodePtr          spot;
    InputPtr<double> strike = Input<double>::make("strike", 95.0);
    InputPtr<double> vol    = Input<double>::make("vol", 0.25);
    InputPtr<double> expiry = Input<double>::make("expiry", 0.75);
    InputPtr<double> rate   = Input<double>::make("rate", 0.03);
    NodePtr call;

    explicit CallPrice(NodePtr spotNode = Input<double>::make("spot", 100.0))
        : spot(std::move(spotNode))
    {
        auto d1 = aad::DifferentiableNode<5>::make("d1", {spot, strike, vol, expiry, rate},
            [](const auto& s, const auto& k, const auto& v, const auto& t, const auto& r) {
                using std::log; using std::sqrt;
                return (log(s / k) + (r + 0.5 * v * v) * t) / (v * sqrt(t));
            },
            InvalidationMode::Lazy);
        auto volRootT = ops::ProductNode<>::make("vol.sqrt(expiry)",
            {vol, ops::SqrtNode<>::make("sqrt(expiry)", expiry)});
        auto d2 = ops::DiffNode<>::make("d2", d1, volRootT);
        const auto cdf = [](const auto& x) {
            using std::erfc; using std::sqrt;
            return 0.5 * erfc(-x / sqrt(2.0));
        };
        auto nd1 = aad::DifferentiableNode<1>::make("N(d1)", {d1}, cdf, InvalidationMode::Lazy);
        auto nd2 = aad::DifferentiableNode<1>::make("N(d2)", {d2}, cdf, InvalidationMode::Lazy);
        auto discount = ops::ExpNode<>::make("discount", ops::NegateNode<>::make("-rate.expiry",
            ops::ProductNode<>::make("rate.expiry", {rate, expiry})));
        call = ops::DiffNode<>::make("call",
            ops::ProductNode<>::make("spot.N(d1)", {spot, nd1}),
            ops::ProductNode<>::make("strike.discount.N(d2)", {strike, discount, nd2}));
    }
};

/// The message of the E that fn() throws. An exception of another type leaves
/// the test, and fails it.
template<typename E, typename Fn>
std::string messageOf(Fn&& fn) {
    try {
        fn();
    } catch (const E& e) {
        return e.what();
    }
    return "nothing was thrown";
}

inline bool mentions(const std::string& message, const std::string& words) {
    return message.find(words) != std::string::npos;
}

} // namespace reduce_test
