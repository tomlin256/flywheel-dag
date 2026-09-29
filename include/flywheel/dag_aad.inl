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

inline Tape::Tape(std::vector<NodePtr> roots) : roots_(std::move(roots)) {
    record();
}

inline std::size_t Tape::size() const noexcept { return entries_.size(); }

// A depth-first walk up from each root, without recursion. It finishes a node
// only after every input its partials name, so each node lands on the tape
// after all of them.
//
// The walk reads each node twice when it first reaches it: eval() to learn what
// the node holds, and partials(). Both read a clean node, so both return cached
// values. The roots are checked clean, and a clean node's named inputs are clean
// too, because an input that moves invalidates its consumers.
inline void Tape::record() {
    EvalContext ctx;
    Partials partials;

    struct Frame {
        NodePtr node;
        std::vector<std::pair<NodePtr, double>> named;   ///< the inputs its partials name
        std::size_t next = 0;                            ///< the next of them to walk
        bool barrier = false;
        bool holdsDouble = false;
    };
    std::vector<Frame> stack;
    std::unordered_set<const INode*> reached;

    const auto visit = [&](const NodePtr& n) {
        if (!reached.insert(n.get()).second) return;
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

    for (const auto& root : roots_) {
        if (!root) throw std::invalid_argument("aad::Tape: a root is null");
        if (root->dirty())
            throw std::invalid_argument("aad::Tape: root " + root->name()
                + " is dirty. A tape reads only evaluated values: evaluate it first");
        const ValuePtr v = root->eval(ctx);
        if (!v || v->type() != std::type_index(typeid(double)))
            throw std::invalid_argument("aad::Tape: root " + root->name()
                + " does not hold a double");

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
    }
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
// exp, log and sqrt take their partials from the ops' Derivative<Op>, so a
// Dual and an op node agree to the bit.

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
    return chain(x, std::sin(x.value), std::cos(x.value));
}
template<std::size_t N> Dual<N> cos(const Dual<N>& x) {
    return chain(x, std::cos(x.value), -std::sin(x.value));
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

} // namespace dag::aad
