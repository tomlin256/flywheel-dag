// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_ops.inl — implementation of all dag_ops.hpp declarations
// Included at the bottom of dag_ops.hpp; never include this file directly.

#pragma once

namespace dag::ops {

// ─────────────────────────────────────────────────────────────────────────────
// OpNodeImpl<Derived>
// ─────────────────────────────────────────────────────────────────────────────

template<typename Derived>
void OpNodeImpl<Derived>::notifyDownstream(const ValuePtr& newV, const EqualityPolicyPtr& eq) {
    if (!eq->equal(cached_, newV)) {
        cached_ = newV;
        this->NodeBase::notifyDownstream();
    }
    this->endEval();
}

// ─────────────────────────────────────────────────────────────────────────────
// UnaryOpNode<T, Op>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T, typename Op>
std::shared_ptr<UnaryOpNode<T,Op>> UnaryOpNode<T,Op>::make(
    std::string name, NodePtr a, EqualityPolicyPtr eq)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<T>>();
    auto self = std::shared_ptr<UnaryOpNode<T,Op>>(
        new UnaryOpNode<T,Op>(std::move(name), std::move(a), std::move(eq)));
    wire(self, self->inputs());
    return self;
}

template<typename T, typename Op>
UnaryOpNode<T,Op>::UnaryOpNode(std::string name, NodePtr a, EqualityPolicyPtr eq)
    : name_(std::move(name)), a_(std::move(a)), eq_(std::move(eq)) {}

template<typename T, typename Op>
ValuePtr UnaryOpNode<T,Op>::eval(EvalContext& ctx) {
    if (!this->dirty() && !ctx.forceRecompute) return this->cached_;
    this->beginEval();
    // Hold the pulled value: the resolve check has to happen after the pull, and
    // the ValuePtr is what keeps the producer's ValueSlot buffer from being
    // recycled underneath the get_value() below.
    const ValuePtr av = a_->eval(ctx);
    if (this->skipRecompute(ctx)) { this->endEval(); return this->cached_; }
    T result = Op{}(get_value<T>(av));
    this->notifyDownstream(slot_.emit(std::move(result)), eq_);
    return this->cached_;
}

template<typename T, typename Op>
std::string UnaryOpNode<T,Op>::name() const { return name_; }

template<typename T, typename Op>
std::vector<NodePtr> UnaryOpNode<T,Op>::inputs() const { return {a_}; }

// A pass asks only a clean node, so the pull returns a's cached value.
template<typename T, typename Op>
bool UnaryOpNode<T,Op>::partials([[maybe_unused]] EvalContext& ctx,
                                 [[maybe_unused]] aad::Partials& out) {
    if constexpr (std::is_same_v<T, double> && Derivative<Op>::defined) {
        const ValuePtr av = a_->eval(ctx);
        out.add(0, Derivative<Op>::d(get_value<double>(av)));
        return true;
    } else {
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BinaryOpNode<T, Op>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T, typename Op>
std::shared_ptr<BinaryOpNode<T,Op>> BinaryOpNode<T,Op>::make(
    std::string name, NodePtr a, NodePtr b, EqualityPolicyPtr eq)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<T>>();
    auto self = std::shared_ptr<BinaryOpNode<T,Op>>(
        new BinaryOpNode<T,Op>(std::move(name), std::move(a), std::move(b),
                               std::move(eq)));
    wire(self, self->inputs());
    return self;
}

template<typename T, typename Op>
BinaryOpNode<T,Op>::BinaryOpNode(std::string name, NodePtr a, NodePtr b,
                                 EqualityPolicyPtr eq)
    : name_(std::move(name)), a_(std::move(a)), b_(std::move(b)), eq_(std::move(eq)) {}

template<typename T, typename Op>
ValuePtr BinaryOpNode<T,Op>::eval(EvalContext& ctx) {
    if (!this->dirty() && !ctx.forceRecompute) return this->cached_;
    this->beginEval();
    const ValuePtr av = a_->eval(ctx);
    const ValuePtr bv = b_->eval(ctx);
    if (this->skipRecompute(ctx)) { this->endEval(); return this->cached_; }
    T result = Op{}(get_value<T>(av), get_value<T>(bv));
    this->notifyDownstream(slot_.emit(std::move(result)), eq_);
    return this->cached_;
}

template<typename T, typename Op>
std::string BinaryOpNode<T,Op>::name() const { return name_; }

template<typename T, typename Op>
std::vector<NodePtr> BinaryOpNode<T,Op>::inputs() const { return {a_, b_}; }

// When a and b are one node, as in x − x, the two entries name the same node,
// and a pass adds them up.
template<typename T, typename Op>
bool BinaryOpNode<T,Op>::partials([[maybe_unused]] EvalContext& ctx,
                                  [[maybe_unused]] aad::Partials& out) {
    if constexpr (std::is_same_v<T, double> && Derivative<Op>::defined) {
        const ValuePtr av = a_->eval(ctx);
        const ValuePtr bv = b_->eval(ctx);
        const auto [da, db] = Derivative<Op>::d(get_value<double>(av), get_value<double>(bv));
        out.add(0, da);
        out.add(1, db);
        return true;
    } else {
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// NAryOpNode<T, Op>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T, typename Op>
std::shared_ptr<NAryOpNode<T,Op>> NAryOpNode<T,Op>::make(
    std::string name, std::vector<NodePtr> ins, EqualityPolicyPtr eq)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<T>>();
    auto self = std::shared_ptr<NAryOpNode<T,Op>>(
        new NAryOpNode<T,Op>(std::move(name), std::move(ins), std::move(eq)));
    wire(self, self->inputs());
    return self;
}

template<typename T, typename Op>
NAryOpNode<T,Op>::NAryOpNode(std::string name, std::vector<NodePtr> ins,
                             EqualityPolicyPtr eq)
    : name_(std::move(name)), inputs_(std::move(ins)), eq_(std::move(eq)) {}

// The n-ary node is the one place the resolve check costs something, and only
// when it is Lazy. Its pulls and its fold are the same loop, so there is no
// point between "every input pulled" and "the functor has run" to check at —
// hence the separate pull pass. Two things keep the cost where it belongs:
//
//   • an Eager node never enters it, so it pays one enum compare, the same as
//     every other node;
//   • a Lazy node that does NOT skip re-pulls each input, but every one of them
//     is Clean by then, so the second pull is a virtual call returning cached_.
//
// The alternative — retaining the pulled ValuePtrs in a member vector — is
// ruled out: it pins a ValueSlot buffer and pushes the producer back into
// allocating.
template<typename T, typename Op>
ValuePtr NAryOpNode<T,Op>::eval(EvalContext& ctx) {
    if (!this->dirty() && !ctx.forceRecompute) return this->cached_;
    this->beginEval();
    if (!ctx.forceRecompute) {
        for (auto& in : inputs_) in->eval(ctx);
        if (this->skipRecompute(ctx)) { this->endEval(); return this->cached_; }
    }
    T total = Op::identity();
    Op op{};
    for (auto& in : inputs_) total = op(total, get_value<T>(in->eval(ctx)));
    this->notifyDownstream(slot_.emit(std::move(total)), eq_);
    return this->cached_;
}

template<typename T, typename Op>
std::string NAryOpNode<T,Op>::name() const { return name_; }

template<typename T, typename Op>
std::vector<NodePtr> NAryOpNode<T,Op>::inputs() const { return inputs_; }

// The values are copied out, because a product's partials need all of them at
// once. Allocating here is fine: a pass is not on the cycle path.
template<typename T, typename Op>
bool NAryOpNode<T,Op>::partials([[maybe_unused]] EvalContext& ctx,
                                [[maybe_unused]] aad::Partials& out) {
    if constexpr (std::is_same_v<T, double> && Derivative<Op>::defined) {
        std::vector<double> x;
        x.reserve(inputs_.size());
        for (auto& in : inputs_) x.push_back(get_value<double>(in->eval(ctx)));
        std::vector<double> dx(x.size());
        Derivative<Op>::d(x, dx);
        for (std::size_t i = 0; i < dx.size(); ++i) out.add(i, dx[i]);
        return true;
    } else {
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Derivative<Op>
//
// Full specialisations are ordinary classes, so their members defined here are
// inline, like any non-template function in a header.
// ─────────────────────────────────────────────────────────────────────────────

inline void Derivative<PlusOp<double>>::d(const std::vector<double>&,
                                          std::vector<double>& dx) {
    for (double& v : dx) v = 1.0;
}

// dx[i] is first the product of the factors before i, then times the product
// of those after it. No factor is ever divided out, so a zero factor is exact:
// (0, 3, 4) gives (12, 0, 0).
inline void Derivative<MultipliesOp<double>>::d(const std::vector<double>& x,
                                                std::vector<double>& dx) {
    double before = 1.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        dx[i] = before;
        before *= x[i];
    }
    double after = 1.0;
    for (std::size_t i = x.size(); i-- > 0;) {
        dx[i] *= after;
        after *= x[i];
    }
}

inline std::pair<double, double> Derivative<std::minus<double>>::d(double, double) {
    return {1.0, -1.0};
}

// −a/b² as −(a/b)/b, which does not overflow or underflow in b² first.
inline std::pair<double, double> Derivative<std::divides<double>>::d(double a, double b) {
    const double q = a / b;
    return {1.0 / b, -q / b};
}

inline double Derivative<std::negate<double>>::d(double) { return -1.0; }

inline double Derivative<ExpOp<double>>::d(double a) { return std::exp(a); }

inline double Derivative<LnOp<double>>::d(double a) { return 1.0 / a; }

// a^b is flat in a when b is 0: a^0 is 1 for every a. It is flat in b at a = 0
// with b > 0: 0^b is 0 there. At a = 0 the formulas give 0·∞, a NaN, for both.
// Elsewhere a NaN is the true answer, as for ∂/∂b at a < 0, and is kept.
inline std::pair<double, double> Derivative<PowOp<double>>::d(double a, double b) {
    const double da = (b == 0.0) ? 0.0 : b * std::pow(a, b - 1.0);
    const double db = (a == 0.0 && b > 0.0) ? 0.0 : std::pow(a, b) * std::log(a);
    return {da, db};
}

inline double Derivative<SqrtOp<double>>::d(double a) { return 0.5 / std::sqrt(a); }

inline double Derivative<SinOp<double>>::d(double a) { return std::cos(a); }

inline double Derivative<CosOp<double>>::d(double a) { return -std::sin(a); }

// 1 + tan² a rather than 1/cos² a: both terms are non-negative, so nothing
// cancels.
inline double Derivative<TanOp<double>>::d(double a) {
    const double t = std::tan(a);
    return 1.0 + t * t;
}

// 1 − a² as (1 − a)(1 + a). Next to |a| = 1, a² is rounded before the
// subtraction exposes it: at a = 1 − 2⁻²⁷, 1 − a² reads 2⁻²⁶ where the exact
// value is 2⁻²⁶ − 2⁻⁵⁴. Next to ±1 one factor is exact and the other is near 2,
// so the product is good to about an ulp. Where a compiler fuses 1 − a·a into
// one FMA, as Apple Clang does on arm64, the textbook form is exact as well, so
// AadPartials.AsinAndAcosAreAccurateNextToTheirEnds catches it only where it
// does not. At |a| = 1 the partial is +∞, the one-sided slope, as √'s is at 0.
inline double Derivative<AsinOp<double>>::d(double a) {
    return 1.0 / std::sqrt((1.0 - a) * (1.0 + a));
}

inline double Derivative<AcosOp<double>>::d(double a) {
    return -Derivative<AsinOp<double>>::d(a);
}

// 1 + a² is at least 1, so it cannot underflow. It overflows past
// |a| ≈ 1.3e154, where the partial reads 0 and the exact value is below
// 5.6e-309, a subnormal.
inline double Derivative<AtanOp<double>>::d(double a) { return 1.0 / (1.0 + a * a); }

// b/(a² + b²) and −a/(a² + b²) as (b/h)/h and −(a/h)/h, with h = hypot(a, b),
// for the reason −a/b² is −(a/b)/b above. a² + b² overflows once a or b passes
// about 1.3e154, and underflows once both fall below about 1.5e-154. hypot does
// neither, and b/h is cos θ. At the origin h is 0 and both partials are NaN,
// from 0/0: the angle jumps there, and has no derivative.
inline std::pair<double, double> Derivative<Atan2Op<double>>::d(double a, double b) {
    const double h = std::hypot(a, b);
    return {(b / h) / h, -(a / h) / h};
}

} // namespace dag::ops
