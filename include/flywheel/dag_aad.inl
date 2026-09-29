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
            // A zero adjoint propagates nothing, so a constant's ∞ or NaN partial
            // cannot make a NaN out of it. A NaN adjoint is not zero, and does
            // propagate.
            if (a != 0.0) adj[edges_[k].input] += edges_[k].d * a;
        }
    }
    checkBarriers(root, reached, wrt);

    std::vector<double> out;
    out.reserve(wrt.size());
    for (const auto& w : wrt) {
        const auto it = position_.find(w.get());
        const bool reachable = it != position_.end() && it->second <= top;
        out.push_back(reachable ? adj[it->second] : 0.0);
    }
    return out;
}

// A wrt node upstream of a barrier that the root reaches has a derivative
// through that barrier that nothing can say, so the answer would be silently
// incomplete. The walk follows inputs(), which reads no value. One visited set
// serves every barrier: a node already walked from one barrier has no wrt node
// above it, or the walk would have thrown there.
inline void Tape::checkBarriers(const NodePtr& root, const std::vector<bool>& reached,
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
                throw std::domain_error("aad::Tape: " + n->name() + " reaches root "
                    + root->name() + " through " + e.node->name()
                    + ", which has no partials");
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

} // namespace dag::aad
