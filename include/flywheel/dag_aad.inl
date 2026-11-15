// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
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

// Recording evaluates nothing, so checking each root as record() reaches it is
// checking all of them at once.
inline Tape::Tape(const std::vector<NodePtr>& roots) : roots_(roots) {
    rootPosition_.reserve(roots_.size());
    record();
}

inline Tape::Tape(std::vector<NodePtr> roots, Unrecorded) : roots_(std::move(roots)) {
    rootPosition_.reserve(roots_.size());
}

inline std::size_t Tape::size() const noexcept { return entries_.size(); }

// Each root is checked clean before it is recorded, and a clean node's named
// inputs are clean too: an input that moves invalidates its consumers, and a
// node does not end an evaluation clean over an input that went dirty again.
// So recordRoot()'s walk reads clean nodes alone.
inline void Tape::record() {
    restart();
    for (std::size_t i = 0; i < roots_.size(); ++i) {
        const NodePtr& root = roots_[i];
        if (!root) throw std::invalid_argument("aad::Tape: a root is null");
        if (root->dirty())
            throw std::invalid_argument("aad::Tape: root " + root->name()
                + " is dirty. A tape reads only evaluated values: evaluate it first");
        recordRoot(i);
    }
}

// A new epoch takes every node off the tape at once: a node is on it only while
// its stamp is the current one. The vectors keep their capacity. The stacks are
// emptied too, because a record that threw part of the way left them as they
// were.
inline void Tape::restart() {
    ++epoch_;
    rootPosition_.clear();
    entries_.clear();
    edges_.clear();
    stack_.clear();
    named_.clear();
}

inline std::size_t Tape::meet(const NodePtr& node) {
    const auto found = index_.find(node.get());
    if (found != index_.end()) return found->second;
    Known k;
    k.node = node;
    // The two ways a node gives its partials: one that derives NodeBase through
    // NodeBase::partials(), and any other through the IDifferentiable mixin.
    k.base = std::dynamic_pointer_cast<NodeBase>(node);
    if (!k.base) k.mixin = std::dynamic_pointer_cast<IDifferentiable>(node);
    known_.push_back(std::move(k));
    index_.emplace(node.get(), known_.size() - 1);
    return known_.size() - 1;
}

// meet() can grow known_, so the node is reached by its place and not through a
// reference held across the loop.
inline void Tape::expand(std::size_t node) {
    if (known_[node].expanded) return;
    const std::vector<NodePtr> ins = known_[node].node->inputs();
    const std::size_t first = inputs_.size();
    for (const auto& in : ins) {
        const std::size_t place = meet(in);
        inputs_.push_back(place);
    }
    known_[node].firstInput = first;
    known_[node].lastInput  = inputs_.size();
    known_[node].expanded   = true;
}

// The walk reads each node twice when it first reaches it: eval() to learn what
// the node holds, and partials(). Both read a clean node, so both return cached
// values. The inputs its partials name go on named_, above those of every frame
// below it.
inline void Tape::visit(std::size_t node, EvalContext& ctx) {
    if (known_[node].recorded == epoch_) return;   // on the tape, or on the stack
    known_[node].recorded = epoch_;
    known_[node].position = kWalking;

    const ValuePtr v = known_[node].node->eval(ctx);
    Frame f{node, named_.size(), named_.size(), named_.size(), false,
            v && v->type() == std::type_index(typeid(double))};
    expand(node);
    const Known& k = known_[node];   // nothing below grows known_
    const std::size_t inputs = k.lastInput - k.firstInput;
    if (inputs != 0) {
        partials_.clear();
        const bool said = k.base ? k.base->partials(ctx, partials_)
                                 : (k.mixin && k.mixin->partials(ctx, partials_));
        if (said) {
            for (const auto& e : partials_.entries()) {
                if (e.input >= inputs)
                    throw std::logic_error("aad::Tape: " + k.node->name() + " names input "
                        + std::to_string(e.input) + " of its " + std::to_string(inputs));
                named_.push_back({inputs_[k.firstInput + e.input], e.d});
            }
        } else {
            f.barrier = true;
        }
    }
    f.lastNamed = named_.size();
    stack_.push_back(f);
}

// A depth-first walk up from the root, without recursion. It finishes a node
// only after every input its partials name, so each node lands on the tape
// after all of them. A node the tape already holds is not walked again: an
// earlier root recorded it, with everything it names.
//
// No clean check: see the declaration.
inline void Tape::recordRoot(std::size_t i) {
    const NodePtr& root = roots_[i];
    EvalContext ctx;
    const ValuePtr rootValue = root->eval(ctx);
    if (!rootValue || rootValue->type() != std::type_index(typeid(double)))
        throw std::invalid_argument("aad::Tape: root " + root->name()
            + " does not hold a double");

    const std::size_t rootPlace = meet(root);
    visit(rootPlace, ctx);
    while (!stack_.empty()) {
        // A copy, not a reference: visit() can grow the stack and move the frame.
        const Frame top = stack_.back();
        if (top.next < top.lastNamed) {
            ++stack_.back().next;
            visit(named_[top.next].node, ctx);
            continue;
        }
        Entry e{top.node, edges_.size(), 0, top.barrier, top.holdsDouble};
        for (std::size_t n = top.firstNamed; n < top.lastNamed; ++n) {
            const Known& in = known_[named_[n].node];
            // Every named input has been walked, so it is on the tape, unless
            // it is still on the stack: a node that names itself, directly or
            // through others.
            if (in.position == kWalking)
                throw std::logic_error("aad::Tape: " + known_[top.node].node->name()
                    + " depends on itself, through " + in.node->name());
            edges_.push_back({in.position, named_[n].d});
        }
        e.lastEdge = edges_.size();
        known_[top.node].position = entries_.size();
        entries_.push_back(e);
        named_.resize(top.firstNamed);   // its named inputs were the last on named_
        stack_.pop_back();
    }
    rootPosition_.push_back(known_[rootPlace].position);
}

inline void Tape::requireRecorded() const {
    if (rootPosition_.size() != roots_.size())
        throw std::logic_error("aad::Tape: the last record() did not finish, so the tape"
                               " holds no recording: record() it again");
}

inline std::size_t Tape::rootPosition(const NodePtr& root) const {
    for (std::size_t i = 0; i < roots_.size(); ++i)
        if (roots_[i] == root) return rootPosition_[i];
    throw std::invalid_argument("aad::Tape: " + (root ? root->name() : std::string("null"))
                                + " is not one of the tape's roots");
}

// A node is on the tape while its stamp is the current record's. A finished
// record leaves none walking.
inline std::size_t Tape::positionOf(const NodePtr& node) const {
    const auto found = index_.find(node.get());
    if (found == index_.end()) return kNone;
    const Known& k = known_[found->second];
    return k.recorded == epoch_ ? k.position : kNone;
}

inline void Tape::checkWrt(const NodePtr& wrt) const {
    if (!wrt) throw std::invalid_argument("aad::Tape: a wrt node is null");
    const std::size_t at = positionOf(wrt);
    if (at != kNone && !entries_[at].holdsDouble)
        throw std::invalid_argument("aad::Tape: wrt " + wrt->name()
                                    + " does not hold a double");
}

// The root's adjoint is 1. Each node the root reaches, from the root down,
// passes its adjoint times each partial to the input the partial names. Every
// input sits earlier on the tape, so a node's adjoint is complete before it is
// passed on.
//
// Only the root's own positions are swept: every node it reaches is recorded
// before it. A node after the root belongs to a later root, and a node before
// it that the root does not reach keeps an adjoint of 0.
inline void Tape::sweepReverse(std::size_t top, std::vector<std::uint32_t>& reached,
                               std::vector<double>& adjoint) const {
    reached.assign(top + 1, std::uint32_t{0});
    adjoint.assign(top + 1, 0.0);
    reached[top] = 1;
    adjoint[top] = 1.0;
    for (std::size_t i = top + 1; i-- > 0;) {
        if (reached[i] == 0) continue;
        const Entry& e = entries_[i];
        const double a = adjoint[i];
        for (std::size_t k = e.firstEdge; k < e.lastEdge; ++k) {
            reached[edges_[k].input] = 1;
            // A partial times an adjoint is 0 when either is 0, so neither 0·∞
            // nor 0·NaN makes a NaN. A NaN that is not multiplied by 0 does
            // propagate.
            const double d = edges_[k].d;
            if (a != 0.0 && d != 0.0) adjoint[edges_[k].input] += d * a;
        }
    }
}

// Each node, from the leaves up, adds each partial times the tangent of the
// input it names to its own, which holds its seed on entry. Every input sits
// earlier on the tape, so its tangent is complete first.
inline void Tape::sweepForward(std::vector<double>& tangent) const {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        double sum = tangent[i];
        for (std::size_t k = e.firstEdge; k < e.lastEdge; ++k) {
            // As in sweepReverse(): a partial times a tangent is 0 when either is 0.
            const double tin = tangent[edges_[k].input];
            const double d   = edges_[k].d;
            if (tin != 0.0 && d != 0.0) sum += d * tin;
        }
        tangent[i] = sum;
    }
}

inline std::vector<double> Tape::adjoints(const NodePtr& root,
                                          const std::vector<NodePtr>& wrt) const {
    requireRecorded();
    const std::size_t top = rootPosition(root);
    for (const auto& w : wrt) checkWrt(w);

    std::vector<std::uint32_t> reached;
    std::vector<double>       adjoint;
    sweepReverse(top, reached, adjoint);
    checkBarriers("root " + root->name(), top + 1,
                  [&reached](std::size_t i) { return reached[i] != 0; }, wrt);

    std::vector<double> out;
    out.reserve(wrt.size());
    for (const auto& w : wrt) {
        const std::size_t at = positionOf(w);
        out.push_back(at != kNone && at <= top ? adjoint[at] : 0.0);
    }
    return out;
}

// The sweep of the function above, in the tape's own storage. A wrt node the
// tape has not met is given a place in known_, so that the barrier check can
// tell it from the nodes it walks: that allocates once for such a node, and not
// again.
inline void Tape::adjoints(const NodePtr& root, std::span<const NodePtr> wrt,
                           std::span<double> out) {
    if (out.size() != wrt.size())
        throw std::invalid_argument("aad::Tape: " + std::to_string(wrt.size())
            + " wrt nodes, and room for " + std::to_string(out.size()) + " adjoints");
    requireRecorded();
    const std::size_t top = rootPosition(root);
    for (const auto& w : wrt) checkWrt(w);

    sweepReverse(top, reached_, adjoint_);
    if (!wrt.empty()) {
        ++sweep_;
        for (const auto& w : wrt) {
            const std::size_t place = meet(w);
            known_[place].wanted = sweep_;
        }
        checkKeptBarriers(&root, top + 1, [this](std::size_t i) { return reached_[i] != 0; });
    }

    for (std::size_t k = 0; k < wrt.size(); ++k) {
        const std::size_t at = positionOf(wrt[k]);
        out[k] = at != kNone && at <= top ? adjoint_[at] : 0.0;
    }
}

// Each seed adds its tangent to its node, and sweepForward() carries them up.
//
// A seed on an intermediate node adds to the tangent that reaches it, rather
// than replacing it. That keeps this sweep the exact dual of adjoints(): a
// root's tangent is Σ seed · adjoint.
inline std::vector<double> Tape::tangents(const std::vector<Seed>& seeds) const {
    requireRecorded();
    std::vector<NodePtr> seeded;
    seeded.reserve(seeds.size());
    for (const auto& s : seeds) seeded.push_back(s.node);
    for (const auto& w : seeded) checkWrt(w);
    // One forward sweep serves every root, so every barrier on the tape counts.
    checkBarriers("a root", entries_.size(), [](std::size_t) { return true; }, seeded);

    std::vector<double> t(entries_.size(), 0.0);
    for (const auto& s : seeds) {
        const std::size_t at = positionOf(s.node);
        if (at != kNone) t[at] += s.tangent;
    }
    sweepForward(t);

    std::vector<double> out;
    out.reserve(roots_.size());
    for (const std::size_t at : rootPosition_) out.push_back(t[at]);
    return out;
}

// The sweep of the function above, in the tape's own storage.
inline void Tape::tangents(std::span<const Seed> seeds, std::span<double> out) {
    if (out.size() != roots_.size())
        throw std::invalid_argument("aad::Tape: " + std::to_string(roots_.size())
            + " roots, and room for " + std::to_string(out.size()) + " tangents");
    requireRecorded();
    for (const auto& s : seeds) checkWrt(s.node);
    if (!seeds.empty()) {
        ++sweep_;
        for (const auto& s : seeds) {
            const std::size_t place = meet(s.node);
            known_[place].wanted = sweep_;
        }
        // One forward sweep serves every root, so every barrier on the tape counts.
        checkKeptBarriers(nullptr, entries_.size(), [](std::size_t) { return true; });
    }

    tangent_.assign(entries_.size(), 0.0);
    for (const auto& s : seeds) {
        const std::size_t at = positionOf(s.node);
        if (at != kNone) tangent_[at] += s.tangent;
    }
    sweepForward(tangent_);
    for (std::size_t i = 0; i < roots_.size(); ++i) out[i] = tangent_[rootPosition_[i]];
}

// A wrt node upstream of a barrier that the sweep reaches has a derivative
// through that barrier that nothing can say, so the answer would be silently
// incomplete. The walk follows inputs(), which reads no value. One visited set
// serves every barrier: a node already walked from one barrier has no wrt node
// above it, or the walk would have thrown there.
template<typename Reached>
void Tape::checkBarriers(const std::string& target, std::size_t count, Reached&& reached,
                         const std::vector<NodePtr>& wrt) const {
    if (wrt.empty()) return;
    std::unordered_set<const INode*> wanted;
    for (const auto& w : wrt) wanted.insert(w.get());

    std::unordered_set<const INode*> visited;
    std::vector<NodePtr> frontier;
    for (std::size_t i = 0; i < count; ++i) {
        const Entry& e = entries_[i];
        if (!reached(i) || !e.barrier) continue;
        const NodePtr& barrier = known_[e.node].node;
        frontier = barrier->inputs();
        while (!frontier.empty()) {
            const NodePtr n = std::move(frontier.back());
            frontier.pop_back();
            if (!visited.insert(n.get()).second) continue;
            if (wanted.count(n.get()) != 0)
                throw std::domain_error("aad::Tape: " + n->name() + " reaches " + target
                    + " through " + barrier->name() + ", which has no partials");
            for (auto& m : n->inputs()) frontier.push_back(std::move(m));
        }
    }
}

// The walk of the function above, in the same order, over the inputs the tape
// keeps: a node is expanded the first time a walk comes to it. The sweep's stamp
// stands for the two sets: a node is wanted, or visited, when it carries it.
template<typename Reached>
void Tape::checkKeptBarriers(const NodePtr* root, std::size_t count, Reached&& reached) {
    frontier_.clear();
    for (std::size_t i = 0; i < count; ++i) {
        if (!reached(i) || !entries_[i].barrier) continue;
        const std::size_t barrier = entries_[i].node;
        // A barrier has inputs, so the record expanded it.
        for (std::size_t k = known_[barrier].firstInput; k < known_[barrier].lastInput; ++k)
            frontier_.push_back(inputs_[k]);
        while (!frontier_.empty()) {
            const std::size_t n = frontier_.back();
            frontier_.pop_back();
            if (known_[n].visited == sweep_) continue;
            known_[n].visited = sweep_;
            if (known_[n].wanted == sweep_)
                throw std::domain_error("aad::Tape: " + known_[n].node->name() + " reaches "
                    + (root ? "root " + (*root)->name() : std::string("a root"))
                    + " through " + known_[barrier].node->name() + ", which has no partials");
            expand(n);   // can grow known_ and inputs_: no reference is held across it
            for (std::size_t k = known_[n].firstInput; k < known_[n].lastInput; ++k)
                frontier_.push_back(inputs_[k]);
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
// tanh′ is sech², taken as 1/cosh/cosh. Not 1 − tanh²: once tanh rounds to ±1,
// from |x| ≈ 19, that reads 0, and no rearrangement of it helps, because
// rounding tanh has already lost the answer. Not 1/(cosh·cosh) either: cosh²
// overflows past |x| ≈ 355, where sech² is still a subnormal number, though
// cosh itself overflows only past 710.
template<std::size_t N> Dual<N> tanh(const Dual<N>& x) {
    const double c = std::cosh(x.value);
    return chain(x, std::tanh(x.value), 1.0 / c / c);
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

// A tape asks only a clean node, so each pull, left to right as in eval(),
// returns a cached value.
template<std::size_t N>
bool DifferentiableNode<N>::partials(EvalContext& ctx, Partials& out) {
    std::array<double, N> at{};
    for (std::size_t i = 0; i < N; ++i) at[i] = get_value<double>(inputs_[i]->eval(ctx));
    dualPartials(at, out);
    return true;
}

// Argument i arrives as its value with d = eᵢ, so the result's d holds every
// partial.
template<std::size_t N>
void DifferentiableNode<N>::dualPartials(const std::array<double, N>& at, Partials& out) const {
    std::array<Dual<N>, N> x{};
    for (std::size_t i = 0; i < N; ++i) {
        x[i] = Dual<N>(at[i]);
        x[i].d[i] = 1.0;
    }
    const Dual<N> y = std::apply(dualFn_, x);
    for (std::size_t i = 0; i < N; ++i) out.add(i, y.d[i]);
}

// The functor at double, as eval() runs it, at the values a reduction hands
// it, published through a slot of the step's own. Its partials are the node's:
// one run of the functor on duals, at the same values.
template<std::size_t N>
class DifferentiableNode<N>::Step final : public IReduceStep {
public:
    explicit Step(std::shared_ptr<const DifferentiableNode> node) : node_(std::move(node)) {}

    ValuePtr apply(const ReduceInputs& in) override {
        return call(in, std::make_index_sequence<N>{});
    }

    bool partials(const ReduceInputs& in, Partials& out) override {
        std::array<double, N> at{};
        for (std::size_t i = 0; i < N; ++i) at[i] = get_value<double>(in[i]);
        node_->dualPartials(at, out);
        return true;
    }

private:
    template<std::size_t... Is>
    ValuePtr call(const ReduceInputs& in, std::index_sequence<Is...>) {
        return slot_.emit(node_->fn_(get_value<double>(in[Is])...));
    }

    std::shared_ptr<const DifferentiableNode> node_;
    ValueSlot<double>                         slot_;
};

template<std::size_t N>
std::unique_ptr<IReduceStep> DifferentiableNode<N>::reduceStep() {
    return std::make_unique<Step>(this->shared_from_this());
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
    , wrt_(std::move(wrt)), eq_(std::move(eq))
    , tape_(std::vector<NodePtr>{root_}, Tape::Unrecorded{})
    , gradient_(wrt_.size(), 0.0) {}

// Pulling the root leaves it clean, so the tape reads it and evaluates nothing
// more. A throw from the tape leaves this node dirty, and cached_ as it was.
//
// A node that is always dirty is the exception. Reaching the root by two paths,
// it leaves the root dirty after its own pull, which recordRoot() allows. And
// the tape's pull evaluates it again, which can mark the root dirty again. This
// node then hears a "maybe", and endEval() keeps it dirty and tells its
// consumers: marked clean, it would never see the next change, which stops at
// the root because the root is already dirty.
//
// The gradient is emitted by copy: copy-assigning into the slot's buffer reuses
// its storage, where a moved-in vector would replace it.
inline ValuePtr GradientNode::eval(EvalContext& ctx) {
    if (!dirty() && !ctx.forceRecompute) return cached_;
    beginEval();
    root_->eval(ctx);
    tape_.restart();
    tape_.recordRoot(0);
    tape_.adjoints(root_, wrt_, gradient_);
    const ValuePtr newV = slot_.emit(std::as_const(gradient_));
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
    , seeds_(std::move(seeds)), eq_(std::move(eq))
    , tape_(roots_, Tape::Unrecorded{})
    , tangents_(roots_.size(), 0.0) {}

// Each root is recorded right after its pull. A later root's pull evaluates
// only nodes the tape does not hold yet, an always-dirty node apart, so it moves
// nothing else the tape recorded. It can leave an earlier root dirty again,
// through an always-dirty node that reaches both. The node then hears a
// "maybe", and endEval() keeps it dirty, as for a GradientNode.
//
// The tangents are emitted by copy, for GradientNode's reason.
inline ValuePtr TangentNode::eval(EvalContext& ctx) {
    if (!dirty() && !ctx.forceRecompute) return cached_;
    beginEval();
    tape_.restart();
    for (std::size_t i = 0; i < roots_.size(); ++i) {
        roots_[i]->eval(ctx);
        tape_.recordRoot(i);
    }
    tape_.tangents(seeds_, tangents_);
    const ValuePtr newV = slot_.emit(std::as_const(tangents_));
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
