// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_aad.inl — implementation of dag_aad.hpp declarations.
// Included at the bottom of dag_aad.hpp; never include this file directly.

#pragma once

namespace dag::aad {

// ─────────────────────────────────────────────────────────────────────────────
// Tape
// ─────────────────────────────────────────────────────────────────────────────

// Recording evaluates nothing, so checking each root as add() reaches it is
// checking all of them at once.
inline Tape::Tape(std::vector<NodePtr> roots) {
    roots_.reserve(roots.size());
    for (const auto& root : roots) add(root);
}

inline std::size_t Tape::size() const noexcept { return entries_.size(); }

// A depth-first walk up from the root, without recursion. It finishes a node
// only after every input its partials name, so each node lands on the tape
// after all of them. A node the tape already holds is not walked again: an
// earlier add() recorded it, with everything it names.
//
// The walk reads each node twice when it first reaches it: eval() to learn what
// the node holds, and partials(). Both read a clean node, so both return cached
// values. add() checks the root clean, and a clean node's named inputs are clean
// too: an input that moves invalidates its consumers, and a node does not end
// an evaluation clean over an input that went dirty again (flywheel-dag#18).
inline void Tape::add(const NodePtr& root) {
    if (!root) throw std::invalid_argument("aad::Tape: a root is null");
    if (root->dirty())
        throw std::invalid_argument("aad::Tape: root " + root->name()
            + " is dirty. A tape reads only evaluated values: evaluate it first");
    record(root);
}

// No clean check: a sensitivity node records the root it has just pulled. A
// root that an always-dirty node reaches by two paths stays dirty after its own
// pull, and the walk's eval() and partials() then evaluate what went dirty
// again, as they evaluate any always-dirty node they meet.
inline void Tape::record(const NodePtr& root) {
    EvalContext ctx;
    const ValuePtr rootValue = root->eval(ctx);
    if (!rootValue || rootValue->type() != std::type_index(typeid(double)))
        throw std::invalid_argument("aad::Tape: root " + root->name()
            + " does not hold a double");

    Partials partials;

    struct Frame {
        NodePtr node;
        std::vector<std::pair<NodePtr, double>> named;   ///< the inputs its partials name
        std::size_t next = 0;                            ///< the next of them to walk
        bool barrier = false;
        bool holdsDouble = false;
    };
    std::vector<Frame> stack;
    std::unordered_set<const INode*> walking;   ///< reached by this call, not yet on the tape

    const auto visit = [&](const NodePtr& n) {
        if (position_.count(n.get()) != 0 || !walking.insert(n.get()).second) return;
        Frame f;
        f.node = n;
        const ValuePtr v = n->eval(ctx);
        f.holdsDouble = v && v->type() == std::type_index(typeid(double));
        const std::vector<NodePtr> ins = n->inputs();
        if (!ins.empty()) {
            auto* d = dynamic_cast<IDifferentiable*>(n.get());
            partials.clear();
            if (d != nullptr && d->partials(ctx, partials)) {
                for (const auto& e : partials.entries()) {
                    if (e.input >= ins.size())
                        throw std::logic_error("aad::Tape: " + n->name() + " names input "
                            + std::to_string(e.input) + " of its "
                            + std::to_string(ins.size()));
                    f.named.emplace_back(ins[e.input], e.d);
                }
            } else {
                f.barrier = true;
            }
        }
        stack.push_back(std::move(f));
    };

    visit(root);
    while (!stack.empty()) {
        Frame& top = stack.back();
        if (top.next < top.named.size()) {
            // A copy, not a reference: visit() can grow the stack and move top.
            const NodePtr in = top.named[top.next++].first;
            visit(in);
            continue;
        }
        Entry e{top.node, edges_.size(), 0, top.barrier, top.holdsDouble};
        for (const auto& [in, d] : top.named)
            edges_.push_back({position_.at(in.get()), d});
        e.lastEdge = edges_.size();
        position_.emplace(top.node.get(), entries_.size());
        entries_.push_back(std::move(e));
        stack.pop_back();
    }
    roots_.push_back(root);
}

inline std::size_t Tape::rootPosition(const NodePtr& root) const {
    for (const auto& r : roots_)
        if (r == root) return position_.at(r.get());
    throw std::invalid_argument("aad::Tape: " + (root ? root->name() : std::string("null"))
                                + " is not one of the tape's roots");
}

inline void Tape::checkWrt(const std::vector<NodePtr>& wrt) const {
    for (const auto& w : wrt) {
        if (!w) throw std::invalid_argument("aad::Tape: a wrt node is null");
        const auto it = position_.find(w.get());
        if (it != position_.end() && !entries_[it->second].holdsDouble)
            throw std::invalid_argument("aad::Tape: wrt " + w->name()
                                        + " does not hold a double");
    }
}

// The root's adjoint is 1. Each node the root reaches, from the root down,
// passes its adjoint times each partial to the input the partial names. Every
// input sits earlier on the tape, so a node's adjoint is complete before it is
// passed on.
//
// Only the root's own positions are swept: every node it reaches is recorded
// before it. A node after the root belongs to a later root, and a node before
// it that the root does not reach keeps an adjoint of 0.
inline std::vector<double> Tape::adjoints(const NodePtr& root,
                                          const std::vector<NodePtr>& wrt) const {
    const std::size_t top = rootPosition(root);
    checkWrt(wrt);

    std::vector<bool>   reached(top + 1, false);
    std::vector<double> adj(top + 1, 0.0);
    reached[top] = true;
    adj[top]     = 1.0;
    for (std::size_t i = top + 1; i-- > 0;) {
        if (!reached[i]) continue;
        const Entry& e = entries_[i];
        const double a = adj[i];
        for (std::size_t k = e.firstEdge; k < e.lastEdge; ++k) {
            reached[edges_[k].input] = true;
            // A partial times an adjoint is 0 when either is 0, so neither 0·∞
            // nor 0·NaN makes a NaN. A NaN that is not multiplied by 0 does
            // propagate.
            const double d = edges_[k].d;
            if (a != 0.0 && d != 0.0) adj[edges_[k].input] += d * a;
        }
    }
    checkBarriers("root " + root->name(), reached, wrt);

    std::vector<double> out;
    out.reserve(wrt.size());
    for (const auto& w : wrt) {
        const auto it = position_.find(w.get());
        const bool reachable = it != position_.end() && it->second <= top;
        out.push_back(reachable ? adj[it->second] : 0.0);
    }
    return out;
}

// Each seed adds its tangent to its node. Then each node, from the leaves up,
// adds each partial times the tangent of the input it names to its own. Every
// input sits earlier on the tape, so its tangent is complete first.
//
// A seed on an intermediate node adds to the tangent that reaches it, rather
// than replacing it. That keeps this sweep the exact dual of adjoints(): a
// root's tangent is Σ seed · adjoint.
inline std::vector<double> Tape::tangents(const std::vector<Seed>& seeds) const {
    std::vector<NodePtr> seeded;
    seeded.reserve(seeds.size());
    for (const auto& s : seeds) seeded.push_back(s.node);
    checkWrt(seeded);
    // One forward sweep serves every root, so every barrier on the tape counts.
    checkBarriers("a root", std::vector<bool>(entries_.size(), true), seeded);

    std::vector<double> t(entries_.size(), 0.0);
    for (const auto& s : seeds) {
        const auto it = position_.find(s.node.get());
        if (it != position_.end()) t[it->second] += s.tangent;
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        double sum = t[i];
        for (std::size_t k = e.firstEdge; k < e.lastEdge; ++k) {
            // As in adjoints(): a partial times a tangent is 0 when either is 0.
            const double tin = t[edges_[k].input];
            const double d   = edges_[k].d;
            if (tin != 0.0 && d != 0.0) sum += d * tin;
        }
        t[i] = sum;
    }

    std::vector<double> out;
    out.reserve(roots_.size());
    for (const auto& r : roots_) out.push_back(t[position_.at(r.get())]);
    return out;
}

// A wrt node upstream of a barrier that the sweep reaches has a derivative
// through that barrier that nothing can say, so the answer would be silently
// incomplete. The walk follows inputs(), which reads no value. One visited set
// serves every barrier: a node already walked from one barrier has no wrt node
// above it, or the walk would have thrown there.
inline void Tape::checkBarriers(const std::string& target, const std::vector<bool>& reached,
                                const std::vector<NodePtr>& wrt) const {
    if (wrt.empty()) return;
    std::unordered_set<const INode*> wanted;
    for (const auto& w : wrt) wanted.insert(w.get());

    std::unordered_set<const INode*> visited;
    std::vector<NodePtr> frontier;
    for (std::size_t i = 0; i < reached.size(); ++i) {
        const Entry& e = entries_[i];
        if (!reached[i] || !e.barrier) continue;
        frontier = e.node->inputs();
        while (!frontier.empty()) {
            const NodePtr n = std::move(frontier.back());
            frontier.pop_back();
            if (!visited.insert(n.get()).second) continue;
            if (wanted.count(n.get()) != 0)
                throw std::domain_error("aad::Tape: " + n->name() + " reaches " + target
                    + " through " + e.node->name() + ", which has no partials");
            for (auto& m : n->inputs()) frontier.push_back(std::move(m));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Free functions
// ─────────────────────────────────────────────────────────────────────────────

inline std::vector<double> adjoints(const NodePtr& root, const std::vector<NodePtr>& wrt) {
    return Tape({root}).adjoints(root, wrt);
}

inline std::vector<double> tangents(const std::vector<NodePtr>& roots,
                                    const std::vector<Seed>& seeds) {
    return Tape(roots).tangents(seeds);
}

// ─────────────────────────────────────────────────────────────────────────────
// Dual<N>
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// A partial times a derivative, as 0 when either one is 0. A tape's sweeps
/// keep the same rule.
inline double times(double partial, double d) {
    return (partial == 0.0 || d == 0.0) ? 0.0 : partial * d;
}

/// The value v of f(a, b), whose partials are pa and pb.
template<std::size_t N>
Dual<N> combine(double v, double pa, const Dual<N>& a, double pb, const Dual<N>& b) {
    Dual<N> r(v);
    for (std::size_t i = 0; i < N; ++i) r.d[i] = times(pa, a.d[i]) + times(pb, b.d[i]);
    return r;
}

/// 2/√π, the constant in erf′ and erfc′.
inline constexpr double kTwoOverRootPi = 1.1283791670955126;

} // namespace detail

template<std::size_t N>
Dual<N>::Dual(double v) : value(v) {}

template<std::size_t N>
Dual<N>& Dual<N>::operator+=(const Dual& b) { return *this = *this + b; }

template<std::size_t N>
Dual<N>& Dual<N>::operator-=(const Dual& b) { return *this = *this - b; }

template<std::size_t N>
Dual<N>& Dual<N>::operator*=(const Dual& b) { return *this = *this * b; }

template<std::size_t N>
Dual<N>& Dual<N>::operator/=(const Dual& b) { return *this = *this / b; }

template<std::size_t N>
Dual<N> chain(const Dual<N>& x, double fx, double dfx) {
    Dual<N> r(fx);
    for (std::size_t i = 0; i < N; ++i) r.d[i] = detail::times(dfx, x.d[i]);
    return r;
}

// ── Arithmetic ──────────────────────────────────────────────────────────────
//
// A double operand is a constant: it converts to a Dual whose d is 0, which
// the zero rule then passes over.

template<std::size_t N> Dual<N> operator+(const Dual<N>& a) { return a; }
template<std::size_t N> Dual<N> operator-(const Dual<N>& a) { return chain(a, -a.value, -1.0); }

template<std::size_t N> Dual<N> operator+(const Dual<N>& a, const Dual<N>& b) {
    return detail::combine(a.value + b.value, 1.0, a, 1.0, b);
}
template<std::size_t N> Dual<N> operator+(const Dual<N>& a, double b) { return a + Dual<N>(b); }
template<std::size_t N> Dual<N> operator+(double a, const Dual<N>& b) { return Dual<N>(a) + b; }

template<std::size_t N> Dual<N> operator-(const Dual<N>& a, const Dual<N>& b) {
    return detail::combine(a.value - b.value, 1.0, a, -1.0, b);
}
template<std::size_t N> Dual<N> operator-(const Dual<N>& a, double b) { return a - Dual<N>(b); }
template<std::size_t N> Dual<N> operator-(double a, const Dual<N>& b) { return Dual<N>(a) - b; }

template<std::size_t N> Dual<N> operator*(const Dual<N>& a, const Dual<N>& b) {
    return detail::combine(a.value * b.value, b.value, a, a.value, b);
}
template<std::size_t N> Dual<N> operator*(const Dual<N>& a, double b) { return a * Dual<N>(b); }
template<std::size_t N> Dual<N> operator*(double a, const Dual<N>& b) { return Dual<N>(a) * b; }

// The quotient's partials are DivideNode's: 1/b and −a/b², the second as
// −(a/b)/b.
template<std::size_t N> Dual<N> operator/(const Dual<N>& a, const Dual<N>& b) {
    const auto [pa, pb] = ops::Derivative<std::divides<double>>::d(a.value, b.value);
    return detail::combine(a.value / b.value, pa, a, pb, b);
}
template<std::size_t N> Dual<N> operator/(const Dual<N>& a, double b) { return a / Dual<N>(b); }
template<std::size_t N> Dual<N> operator/(double a, const Dual<N>& b) { return Dual<N>(a) / b; }

// ── Comparisons, on the value alone ─────────────────────────────────────────

template<std::size_t N> bool operator==(const Dual<N>& a, const Dual<N>& b) { return a.value == b.value; }
template<std::size_t N> bool operator==(const Dual<N>& a, double b) { return a.value == b; }
template<std::size_t N> bool operator==(double a, const Dual<N>& b) { return a == b.value; }
template<std::size_t N> bool operator!=(const Dual<N>& a, const Dual<N>& b) { return a.value != b.value; }
template<std::size_t N> bool operator!=(const Dual<N>& a, double b) { return a.value != b; }
template<std::size_t N> bool operator!=(double a, const Dual<N>& b) { return a != b.value; }
template<std::size_t N> bool operator<(const Dual<N>& a, const Dual<N>& b) { return a.value < b.value; }
template<std::size_t N> bool operator<(const Dual<N>& a, double b) { return a.value < b; }
template<std::size_t N> bool operator<(double a, const Dual<N>& b) { return a < b.value; }
template<std::size_t N> bool operator<=(const Dual<N>& a, const Dual<N>& b) { return a.value <= b.value; }
template<std::size_t N> bool operator<=(const Dual<N>& a, double b) { return a.value <= b; }
template<std::size_t N> bool operator<=(double a, const Dual<N>& b) { return a <= b.value; }
template<std::size_t N> bool operator>(const Dual<N>& a, const Dual<N>& b) { return a.value > b.value; }
template<std::size_t N> bool operator>(const Dual<N>& a, double b) { return a.value > b; }
template<std::size_t N> bool operator>(double a, const Dual<N>& b) { return a > b.value; }
template<std::size_t N> bool operator>=(const Dual<N>& a, const Dual<N>& b) { return a.value >= b.value; }
template<std::size_t N> bool operator>=(const Dual<N>& a, double b) { return a.value >= b; }
template<std::size_t N> bool operator>=(double a, const Dual<N>& b) { return a >= b.value; }

// ── Functions ───────────────────────────────────────────────────────────────
//
// exp, log, sqrt and the trigonometric functions take their partials from the
// ops' Derivative<Op>, so a Dual and an op node agree to the bit.

template<std::size_t N> Dual<N> exp(const Dual<N>& x) {
    return chain(x, std::exp(x.value), ops::Derivative<ops::ExpOp<double>>::d(x.value));
}
template<std::size_t N> Dual<N> log(const Dual<N>& x) {
    return chain(x, std::log(x.value), ops::Derivative<ops::LnOp<double>>::d(x.value));
}
template<std::size_t N> Dual<N> sqrt(const Dual<N>& x) {
    return chain(x, std::sqrt(x.value), ops::Derivative<ops::SqrtOp<double>>::d(x.value));
}
template<std::size_t N> Dual<N> sin(const Dual<N>& x) {
    return chain(x, std::sin(x.value), ops::Derivative<ops::SinOp<double>>::d(x.value));
}
template<std::size_t N> Dual<N> cos(const Dual<N>& x) {
    return chain(x, std::cos(x.value), ops::Derivative<ops::CosOp<double>>::d(x.value));
}
template<std::size_t N> Dual<N> tan(const Dual<N>& x) {
    return chain(x, std::tan(x.value), ops::Derivative<ops::TanOp<double>>::d(x.value));
}
template<std::size_t N> Dual<N> asin(const Dual<N>& x) {
    return chain(x, std::asin(x.value), ops::Derivative<ops::AsinOp<double>>::d(x.value));
}
template<std::size_t N> Dual<N> acos(const Dual<N>& x) {
    return chain(x, std::acos(x.value), ops::Derivative<ops::AcosOp<double>>::d(x.value));
}
template<std::size_t N> Dual<N> atan(const Dual<N>& x) {
    return chain(x, std::atan(x.value), ops::Derivative<ops::AtanOp<double>>::d(x.value));
}
template<std::size_t N> Dual<N> tanh(const Dual<N>& x) {
    const double t = std::tanh(x.value);
    return chain(x, t, 1.0 - t * t);
}
template<std::size_t N> Dual<N> erf(const Dual<N>& x) {
    return chain(x, std::erf(x.value), detail::kTwoOverRootPi * std::exp(-x.value * x.value));
}
template<std::size_t N> Dual<N> erfc(const Dual<N>& x) {
    return chain(x, std::erfc(x.value), -detail::kTwoOverRootPi * std::exp(-x.value * x.value));
}
// abs′(0) is 0: the midpoint of the two one-sided slopes.
template<std::size_t N> Dual<N> abs(const Dual<N>& x) {
    const double slope = x.value > 0.0 ? 1.0 : (x.value < 0.0 ? -1.0 : 0.0);
    return chain(x, std::abs(x.value), slope);
}

template<std::size_t N> Dual<N> pow(const Dual<N>& a, const Dual<N>& b) {
    const auto [pa, pb] = ops::Derivative<ops::PowOp<double>>::d(a.value, b.value);
    return detail::combine(std::pow(a.value, b.value), pa, a, pb, b);
}
template<std::size_t N> Dual<N> pow(const Dual<N>& a, double b) { return pow(a, Dual<N>(b)); }
template<std::size_t N> Dual<N> pow(double a, const Dual<N>& b) { return pow(Dual<N>(a), b); }

template<std::size_t N> Dual<N> atan2(const Dual<N>& a, const Dual<N>& b) {
    const auto [pa, pb] = ops::Derivative<ops::Atan2Op<double>>::d(a.value, b.value);
    return detail::combine(std::atan2(a.value, b.value), pa, a, pb, b);
}
template<std::size_t N> Dual<N> atan2(const Dual<N>& a, double b) { return atan2(a, Dual<N>(b)); }
template<std::size_t N> Dual<N> atan2(double a, const Dual<N>& b) { return atan2(Dual<N>(a), b); }

// As std::min and std::max: the first argument on a tie.
template<std::size_t N> Dual<N> min(const Dual<N>& a, const Dual<N>& b) {
    return b.value < a.value ? b : a;
}
template<std::size_t N> Dual<N> min(const Dual<N>& a, double b) { return min(a, Dual<N>(b)); }
template<std::size_t N> Dual<N> min(double a, const Dual<N>& b) { return min(Dual<N>(a), b); }
template<std::size_t N> Dual<N> max(const Dual<N>& a, const Dual<N>& b) {
    return a.value < b.value ? b : a;
}
template<std::size_t N> Dual<N> max(const Dual<N>& a, double b) { return max(a, Dual<N>(b)); }
template<std::size_t N> Dual<N> max(double a, const Dual<N>& b) { return max(Dual<N>(a), b); }

// ─────────────────────────────────────────────────────────────────────────────
// DifferentiableNode<N>
// ─────────────────────────────────────────────────────────────────────────────

template<std::size_t N>
template<typename F>
DifferentiableNodePtr<N> DifferentiableNode<N>::make(
    std::string name, Inputs inNodes, F fn, EqualityPolicyPtr eq)
{
    return make(std::move(name), std::move(inNodes), std::move(fn), std::move(eq),
                InvalidationMode::Eager);
}

template<std::size_t N>
template<typename F>
DifferentiableNodePtr<N> DifferentiableNode<N>::make(
    std::string name, Inputs inNodes, F fn, InvalidationMode mode)
{
    return make(std::move(name), std::move(inNodes), std::move(fn), nullptr, mode);
}

// The one functor becomes two std::functions, one for each type it runs at, and
// each holds its own copy of it.
template<std::size_t N>
template<typename F>
DifferentiableNodePtr<N> DifferentiableNode<N>::make(
    std::string name, Inputs inNodes, F fn, EqualityPolicyPtr eq, InvalidationMode mode)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<double>>();
    Fn     evaluate(fn);
    DualFn differentiate(std::move(fn));
    auto self = std::shared_ptr<DifferentiableNode>(new DifferentiableNode(
        std::move(name), std::move(inNodes), std::move(evaluate), std::move(differentiate),
        std::move(eq), mode));
    wire(self, self->inputs());
    return self;
}

template<std::size_t N>
DifferentiableNode<N>::DifferentiableNode(std::string name, Inputs ins, Fn fn, DualFn dualFn,
                                          EqualityPolicyPtr eq, InvalidationMode mode)
    : NodeBase(mode), name_(std::move(name)), inputs_(std::move(ins)), fn_(std::move(fn))
    , dualFn_(std::move(dualFn)), eq_(std::move(eq)) {}

template<std::size_t N>
ValuePtr DifferentiableNode<N>::eval(EvalContext& ctx) {
    if (!dirty() && !ctx.forceRecompute) return cached_;
    beginEval();
    return applyInputs(ctx, std::make_index_sequence<N>{});
}

// ComputeNode's path for inputs that are cheap to copy: every input is pulled
// and copied out, left to right, before the resolve check. See
// ComputeNode::applyInputs in dag.inl.
template<std::size_t N>
template<std::size_t... Is>
ValuePtr DifferentiableNode<N>::applyInputs(EvalContext& ctx, std::index_sequence<Is...>) {
    const std::array<double, N> x{ get_value<double>(inputs_[Is]->eval(ctx))... };
    if (skipRecompute(ctx)) { endEval(); return cached_; }
    const ValuePtr newV = slot_.emit(fn_(x[Is]...));
    if (!eq_->equal(cached_, newV)) {
        cached_ = newV;
        notifyDownstream();
    }
    endEval();
    return cached_;
}

template<std::size_t N>
std::string DifferentiableNode<N>::name() const { return name_; }

template<std::size_t N>
std::vector<NodePtr> DifferentiableNode<N>::inputs() const {
    return {inputs_.begin(), inputs_.end()};
}

template<std::size_t N>
bool DifferentiableNode<N>::partials(EvalContext& ctx, Partials& out) {
    dualPartials(ctx, out, std::make_index_sequence<N>{});
    return true;
}

// Argument i arrives as its input's value with d = eᵢ, so the result's d holds
// every partial. A tape asks only a clean node, so each pull returns a cached
// value.
template<std::size_t N>
template<std::size_t... Is>
void DifferentiableNode<N>::dualPartials(EvalContext& ctx, Partials& out,
                                         std::index_sequence<Is...>) {
    std::array<Dual<N>, N> x{ Dual<N>(get_value<double>(inputs_[Is]->eval(ctx)))... };
    for (std::size_t i = 0; i < N; ++i) x[i].d[i] = 1.0;
    const Dual<N> y = dualFn_(x[Is]...);
    for (std::size_t i = 0; i < N; ++i) out.add(i, y.d[i]);
}

// ─────────────────────────────────────────────────────────────────────────────
// GradientNode
// ─────────────────────────────────────────────────────────────────────────────

inline GradientNodePtr GradientNode::make(std::string name, NodePtr root,
                                          std::vector<NodePtr> wrt, EqualityPolicyPtr eq)
{
    if (!root) throw std::invalid_argument("aad::GradientNode: " + name + " has a null root");
    if (wrt.empty())
        throw std::invalid_argument("aad::GradientNode: " + name + " has no wrt nodes");
    for (const auto& w : wrt)
        if (!w) throw std::invalid_argument("aad::GradientNode: " + name + " has a null wrt node");
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<std::vector<double>>>();
    auto self = std::shared_ptr<GradientNode>(new GradientNode(
        std::move(name), std::move(root), std::move(wrt), std::move(eq)));
    wire(self, self->inputs());
    return self;
}

// Eager, fixed here, with no mode to take: see the class comment.
inline GradientNode::GradientNode(std::string name, NodePtr root, std::vector<NodePtr> wrt,
                                  EqualityPolicyPtr eq)
    : NodeBase(InvalidationMode::Eager), name_(std::move(name)), root_(std::move(root))
    , wrt_(std::move(wrt)), eq_(std::move(eq)) {}

// Pulling the root leaves it clean, so the tape reads it and evaluates nothing
// more. A throw from the tape leaves this node dirty, and cached_ as it was.
//
// A node that is always dirty is the exception. Reaching the root by two paths,
// it leaves the root dirty after its own pull, which record() allows. And the
// tape's pull evaluates it again, which can mark the root dirty again
// (flywheel-dag#19). This node then hears a "maybe", and endEval() keeps it
// dirty and tells its consumers: marked clean, it would never see the next
// change, which stops at the root because the root is already dirty.
inline ValuePtr GradientNode::eval(EvalContext& ctx) {
    if (!dirty() && !ctx.forceRecompute) return cached_;
    beginEval();
    root_->eval(ctx);
    Tape tape;
    tape.record(root_);
    const ValuePtr newV = slot_.emit(tape.adjoints(root_, wrt_));
    if (!eq_->equal(cached_, newV)) {
        cached_ = newV;
        notifyDownstream();
    }
    endEval();
    return cached_;
}

inline std::string GradientNode::name() const { return name_; }

inline std::vector<NodePtr> GradientNode::inputs() const { return {root_}; }

// ─────────────────────────────────────────────────────────────────────────────
// TangentNode
// ─────────────────────────────────────────────────────────────────────────────

inline TangentNodePtr TangentNode::make(std::string name, std::vector<NodePtr> roots,
                                        std::vector<Seed> seeds, EqualityPolicyPtr eq)
{
    if (roots.empty())
        throw std::invalid_argument("aad::TangentNode: " + name + " has no roots");
    for (const auto& r : roots)
        if (!r) throw std::invalid_argument("aad::TangentNode: " + name + " has a null root");
    if (seeds.empty())
        throw std::invalid_argument("aad::TangentNode: " + name + " has no seeds");
    for (const auto& s : seeds)
        if (!s.node)
            throw std::invalid_argument("aad::TangentNode: " + name + " has a seed on a null node");
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<std::vector<double>>>();
    auto self = std::shared_ptr<TangentNode>(new TangentNode(
        std::move(name), std::move(roots), std::move(seeds), std::move(eq)));
    wire(self, self->inputs());
    return self;
}

// Eager, fixed here, with no mode to take: see the class comment.
inline TangentNode::TangentNode(std::string name, std::vector<NodePtr> roots,
                                std::vector<Seed> seeds, EqualityPolicyPtr eq)
    : NodeBase(InvalidationMode::Eager), name_(std::move(name)), roots_(std::move(roots))
    , seeds_(std::move(seeds)), eq_(std::move(eq)) {}

// Each root is recorded right after its pull. A later root's pull evaluates
// only nodes the tape does not hold yet, an always-dirty node apart, so it moves
// nothing else the tape recorded. It can leave an earlier root dirty again,
// through an always-dirty node that reaches both. The node then hears a
// "maybe", and endEval() keeps it dirty, as for a GradientNode.
inline ValuePtr TangentNode::eval(EvalContext& ctx) {
    if (!dirty() && !ctx.forceRecompute) return cached_;
    beginEval();
    Tape tape;
    for (const auto& root : roots_) {
        root->eval(ctx);
        tape.record(root);
    }
    const ValuePtr newV = slot_.emit(tape.tangents(seeds_));
    if (!eq_->equal(cached_, newV)) {
        cached_ = newV;
        notifyDownstream();
    }
    endEval();
    return cached_;
}

inline std::string TangentNode::name() const { return name_; }

inline std::vector<NodePtr> TangentNode::inputs() const { return roots_; }

} // namespace dag::aad
