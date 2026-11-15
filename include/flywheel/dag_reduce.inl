// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_reduce.inl — implementation of dag_reduce.hpp declarations.
// Included at the bottom of dag_reduce.hpp; never include this file directly.

#pragma once

namespace dag {

// ─────────────────────────────────────────────────────────────────────────────
// Reduction
// ─────────────────────────────────────────────────────────────────────────────

inline Reduction::Reduction(NodePtr root, std::vector<NodePtr> args)
    : Reduction(std::vector<NodePtr>{std::move(root)}, std::move(args)) {}

// One walk up from each root, depth first and without recursion. It leaves a
// node only after every input, so a candidate lands in entries_ after
// everything it names, and a root lands after everything it depends on. The
// walks share what they have seen, so a node that several roots depend on has
// one entry, and one step.
//
// An argument is a leaf of the walk. The function is of the argument's own
// value, so nothing above it is read, another argument included.
//
// A node with no argument above it gets no entry of its own. It gets one, as a
// Constant, only when a candidate names it, or when it is a root.
inline Reduction::Reduction(std::vector<NodePtr> roots, std::vector<NodePtr> args)
    : roots_(std::move(roots)), args_(std::move(args))
{
    if (roots_.empty()) throw std::invalid_argument("dag::Reduction: there is no root");
    for (const auto& root : roots_)
        if (!root)
            throw std::invalid_argument(roots_.size() == 1 ? "dag::Reduction: the root is null"
                                                           : "dag::Reduction: a root is null");
    if (args_.empty())
        throw std::invalid_argument("dag::Reduction: " + rootNames()
            + (roots_.size() == 1 ? " has" : " have") + " no arguments");
    std::unordered_map<const INode*, std::size_t> argument;   // a node's position in args_
    for (std::size_t i = 0; i < args_.size(); ++i) {
        if (!args_[i])
            throw std::invalid_argument("dag::Reduction: " + rootNames()
                + (roots_.size() == 1 ? " has" : " have") + " a null argument");
        if (!argument.emplace(args_[i].get(), i).second)
            throw std::invalid_argument("dag::Reduction: " + args_[i]->name()
                + " is an argument of " + rootNames() + " twice");
    }

    // Whether an argument is the node or above it. Walking: not left yet.
    enum class Seen : std::uint8_t { Walking, Unreached, Reached };
    struct Frame {
        NodePtr              node;
        std::vector<NodePtr> ins;
        std::size_t          next = 0;   ///< the next of ins to walk
    };
    std::unordered_map<const INode*, Seen>        seen;
    std::unordered_map<const INode*, std::size_t> position;   // a node's place in entries_
    std::vector<Frame>                            stack;

    // The node's entry, made with this role if the node has none.
    const auto entryOf = [&](const NodePtr& n, Role role, std::size_t arg) {
        const auto [it, added] = position.emplace(n.get(), entries_.size());
        if (added) entries_.push_back(Entry{n, role, arg, 0, 0, nullptr, nullptr});
        return it->second;
    };

    const auto visit = [&](const NodePtr& n) {
        if (seen.count(n.get()) != 0) return;
        const auto arg = argument.find(n.get());
        if (arg != argument.end()) {
            seen.emplace(n.get(), Seen::Reached);
            entryOf(n, Role::Argument, arg->second);
            return;
        }
        seen.emplace(n.get(), Seen::Walking);
        stack.push_back(Frame{n, n->inputs(), 0});
    };

    rootPosition_.reserve(roots_.size());
    for (const auto& root : roots_) {
        visit(root);
        while (!stack.empty()) {
            Frame& top = stack.back();
            if (top.next < top.ins.size()) {
                // A copy, not a reference: visit() can grow the stack and move top.
                const NodePtr in = top.ins[top.next++];
                visit(in);
                continue;
            }
            bool reached = false;
            for (const auto& in : top.ins) reached = reached || seen.at(in.get()) == Seen::Reached;
            seen[top.node.get()] = reached ? Seen::Reached : Seen::Unreached;
            if (reached) {
                const std::size_t first = inputs_.size();
                for (const auto& in : top.ins) inputs_.push_back(entryOf(in, Role::Constant, kNone));
                // The hooks are NodeBase's. A node that is not one has none: it gives
                // no step, and bind() takes it to read every input.
                Entry e{top.node, Role::Candidate, kNone, first, inputs_.size(),
                        std::dynamic_pointer_cast<NodeBase>(top.node), nullptr};
                if (e.hooks) e.step = e.hooks->reduceStep();
                position.emplace(top.node.get(), entries_.size());
                entries_.push_back(std::move(e));
            }
            stack.pop_back();
        }
        rootPosition_.push_back(entryOf(root, Role::Constant, kNone));
    }

    // Everything bind() and a call write to is sized here, so neither allocates.
    single_ = roots_.size() == 1;
    root0_  = rootPosition_.front();
    root0Node_ = roots_.front();
    argPosition_.assign(args_.size(), kNone);
    std::size_t candidates = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].role == Role::Argument) argPosition_[entries_[i].argument] = i;
        if (entries_[i].role == Role::Candidate) ++candidates;
    }
    values_.resize(entries_.size());
    uses_.assign(inputs_.size(), ReduceReads::Use::Unread);
    reached_.assign(entries_.size(), 0);
    movedBy_.assign(entries_.size(), kNone);
    run_.reserve(candidates);
    live_.reserve(args_.size());
    rootValue_.resize(roots_.size());
    argValues_.resize(args_.size());
    adjoints_.assign(entries_.size(), 0.0);
    edges_.reserve(inputs_.size());
    firstEdge_.assign(candidates, 0);
    lastEdge_.assign(candidates, 0);
}

inline bool Reduction::holdsDouble(const ValuePtr& v) noexcept {
    return v && v->type() == std::type_index(typeid(double));
}

inline std::string Reduction::rootNames() const {
    if (roots_.size() == 1) return "root " + roots_.front()->name();
    std::string names = "roots ";
    for (std::size_t r = 0; r < roots_.size(); ++r) {
        if (r > 0) names += r + 1 == roots_.size() ? " and " : ", ";
        names += roots_[r]->name();
    }
    return names;
}

inline std::string Reduction::barrierMessage(std::size_t barrier, std::size_t by) const {
    const Entry& e = entries_[barrier];
    std::string why;
    if (e.node->kind() == NodeKind::TimeSeries)
        why = "is a time-series node, whose value is not a function of its inputs' values alone";
    else if (e.hooks && e.hooks->invalidationMode() == InvalidationMode::Eager)
        why = "is Eager, and gives no step. A compute node gives one when it is built with"
              " InvalidationMode::Lazy, which says that its functor is a pure function of its"
              " inputs";
    else
        why = "gives no step: nothing says that its function may be called at other values";
    return "dag::Reduction: " + args_[by]->name() + " reaches " + rootNames() + " through "
         + e.node->name() + ", which " + why;
}

// The roots are checked clean first, and a clean node's inputs are clean too:
// an input that moves invalidates its consumers, and a node does not end an
// evaluation clean over an input that went dirty again. So every eval() that
// bind() makes returns a cached value, an always-dirty node's apart.
inline void Reduction::bind() {
    bound_ = false;
    // The last point's values go first: a bind holds only what its own point
    // reads, and one that throws holds nothing of the last point.
    for (auto& v : values_) v.reset();
    run_.clear();
    live_.clear();
    EvalContext ctx;

    // One root, held in a local as its position is held in a member: a
    // reduction of one root takes no loop over a list of one.
    if (single_) {
        if (root0Node_->dirty())
            throw std::invalid_argument("dag::Reduction: root " + root0Node_->name()
                + " is dirty. A reduction reads only evaluated values: evaluate it first");
        const ValuePtr rootValue = root0Node_->eval(ctx);
        if (!holdsDouble(rootValue))
            throw std::invalid_argument("dag::Reduction: root " + root0Node_->name()
                                        + " does not hold a double");
        readPoint(ctx);
        if (movedBy_[root0_] == kNone) values_[root0_] = rootValue;
        bound_ = true;
        return;
    }

    for (const auto& root : roots_)
        if (root->dirty())
            throw std::invalid_argument("dag::Reduction: root " + root->name()
                + " is dirty. A reduction reads only evaluated values: evaluate it first");

    // The roots' values are held while the point is read, and let go on every
    // way out, a throw included.
    struct LetGo {
        std::vector<ValuePtr>& held;
        ~LetGo() { for (auto& v : held) v.reset(); }
    };
    const LetGo letGo{rootValue_};
    for (std::size_t r = 0; r < roots_.size(); ++r) {
        rootValue_[r] = roots_[r]->eval(ctx);
        if (!holdsDouble(rootValue_[r]))
            throw std::invalid_argument("dag::Reduction: root " + roots_[r]->name()
                                        + " does not hold a double");
    }

    readPoint(ctx);

    // A root that no argument moves is its own value at every call.
    for (std::size_t r = 0; r < roots_.size(); ++r) {
        const std::size_t p = rootPosition_[r];
        if (movedBy_[p] == kNone) values_[p] = rootValue_[r];
    }
    bound_ = true;
}

// Two passes over entries_, which holds every node after the inputs it names.
//
// From the roots up: what the roots depend on at this point, through the
// inputs each node reads. Every consumer of an entry sits after it, so by the
// time the loop comes to an entry, it is reached or it is not.
//
// From the leaves down: which of those an argument moves. An argument does. A
// candidate does if an input it reads does, and it then runs as a step, reading
// from the graph the value of each input that no argument moves. A candidate
// that reads no moved input is a constant here, and nothing above it is read on
// its account.
inline void Reduction::readPoint(EvalContext& ctx) {
    std::fill(reached_.begin(), reached_.end(), std::uint8_t{0});
    std::fill(uses_.begin(), uses_.end(), ReduceReads::Use::Unread);
    if (single_) reached_[root0_] = 1;
    else for (const std::size_t p : rootPosition_) reached_[p] = 1;
    for (std::size_t i = entries_.size(); i-- > 0;) {
        const Entry& e = entries_[i];
        if (reached_[i] == 0 || e.role != Role::Candidate) continue;
        const std::span<ReduceReads::Use> uses =
            std::span(uses_).subspan(e.firstInput, e.lastInput - e.firstInput);
        if (e.hooks) {
            ReduceReads reads(uses);
            e.hooks->reduceReads(ctx, reads);
        } else {
            // Nothing says what it reads, so it reads every input.
            std::fill(uses.begin(), uses.end(), ReduceReads::Use::Read);
        }
        for (std::size_t k = e.firstInput; k < e.lastInput; ++k)
            if (uses_[k] != ReduceReads::Use::Unread) reached_[inputs_[k]] = 1;
    }

    for (std::size_t i = 0; i < entries_.size(); ++i) {
        movedBy_[i] = kNone;
        const Entry& e = entries_[i];
        if (reached_[i] == 0 || e.role == Role::Constant) continue;

        if (e.role == Role::Argument) {
            // Reached, so a step reads it, or it is a root. A call hands it
            // over as a double.
            const ValuePtr held = e.node->eval(ctx);
            if (!holdsDouble(held))
                throw std::invalid_argument("dag::Reduction: argument " + e.node->name()
                                            + " does not hold a double");
            movedBy_[i] = e.argument;
            live_.push_back(e.argument);
            continue;
        }

        std::size_t by = kNone;
        for (std::size_t k = e.firstInput; k < e.lastInput; ++k) {
            if (uses_[k] == ReduceReads::Use::Unread) continue;
            const std::size_t mover = movedBy_[inputs_[k]];
            if (mover == kNone) continue;
            if (uses_[k] == ReduceReads::Use::Decides)
                throw std::domain_error("dag::Reduction: " + rootNames() + ": "
                    + args_[mover]->name() + " moves " + entries_[inputs_[k]].node->name()
                    + ", which decides what " + e.node->name() + " reads. A reduction is bound to"
                      " the inputs the graph read, so it cannot follow a choice that an argument"
                      " moves");
            if (by == kNone) by = mover;
        }
        if (by == kNone) continue;   // a constant at this point
        if (!e.step) throw std::domain_error(barrierMessage(i, by));

        movedBy_[i] = by;
        for (std::size_t k = e.firstInput; k < e.lastInput; ++k) {
            if (uses_[k] == ReduceReads::Use::Unread) continue;
            const std::size_t in = inputs_[k];
            if (movedBy_[in] == kNone) values_[in] = entries_[in].node->eval(ctx);
        }
        run_.push_back(i);
    }
}

// An argument's value is published through a slot of the reduction's, as a
// step's is through the step's, so a warm call allocates nothing. Each value
// stays in values_ until the next call replaces it, which is what lets a slot
// hand out its other buffer.
inline void Reduction::run(std::span<const double> x) {
    if (!bound_)
        throw std::logic_error("dag::Reduction: " + rootNames()
            + (roots_.size() == 1 ? " is" : " are") + " not bound: bind() it first");
    if (x.size() != args_.size())
        throw std::invalid_argument("dag::Reduction: " + rootNames()
            + (roots_.size() == 1 ? " takes " : " take ") + std::to_string(args_.size())
            + " arguments, and was given " + std::to_string(x.size()));

    for (const std::size_t a : live_) values_[argPosition_[a]] = argValues_[a].emit(x[a]);
    const std::span<const std::size_t> inputs(inputs_);
    for (const std::size_t i : run_) {
        const Entry& e = entries_[i];
        values_[i] = e.step->apply(
            ReduceInputs(values_, inputs.subspan(e.firstInput, e.lastInput - e.firstInput)));
    }
}

inline double Reduction::operator()(std::span<const double> x) {
    if (!single_)
        throw std::logic_error("dag::Reduction: " + rootNames()
            + " are several values: call with room for each");
    run(x);
    return get_value<double>(values_[root0_]);
}

inline double Reduction::operator()(std::initializer_list<double> x) {
    return (*this)(std::span<const double>(x.begin(), x.size()));
}

inline void Reduction::operator()(std::span<const double> x, std::span<double> y) {
    if (y.size() != roots_.size())
        throw std::invalid_argument("dag::Reduction: " + rootNames() + ": "
            + std::to_string(roots_.size()) + " values, and room for "
            + std::to_string(y.size()));
    run(x);
    for (std::size_t r = 0; r < roots_.size(); ++r)
        y[r] = get_value<double>(values_[rootPosition_[r]]);
}

inline void Reduction::operator()(std::initializer_list<double> x, std::span<double> y) {
    (*this)(std::span<const double>(x.begin(), x.size()), y);
}

// The call leaves every step's inputs in values_. Every step that runs is
// asked for its partials, the ones whose adjoint turns out to be 0 included:
// whether a reduction has a derivative is then a matter of its steps, and not
// of the point it is asked at. From the last run to the first, as a gradient
// asks them.
inline void Reduction::gatherPartials() {
    edges_.clear();
    const std::span<const std::size_t> inputs(inputs_);
    for (std::size_t r = run_.size(); r-- > 0;) {
        const std::size_t i = run_[r];
        const Entry& e = entries_[i];
        const std::span<const std::size_t> ins =
            inputs.subspan(e.firstInput, e.lastInput - e.firstInput);
        firstEdge_[r] = edges_.size();
        partials_.clear();
        if (!e.step->partials(ReduceInputs(values_, ins), partials_))
            throw std::domain_error("dag::Reduction: " + args_[movedBy_[i]]->name()
                + " reaches " + rootNames() + " through " + e.node->name()
                + ", whose step has no partials: the function can be called, and has no gradient");
        for (const auto& p : partials_.entries()) {
            if (p.input >= ins.size())
                throw std::logic_error("dag::Reduction: the step of " + e.node->name()
                    + " names input " + std::to_string(p.input) + " of its "
                    + std::to_string(ins.size()));
            edges_.push_back(Edge{ins[p.input], p.d});
        }
        lastEdge_[r] = edges_.size();
    }
}

// The root's adjoint is 1. Each step, from the last run to the first, passes
// its adjoint times each partial to the input the partial names. A step runs
// after every step it reads, so in this order its adjoint is complete before
// it is passed on.
//
// A partial times an adjoint is 0 when either is 0, so neither 0·∞ nor 0·NaN
// makes a NaN. A NaN that is not multiplied by 0 does propagate.
inline void Reduction::sweepReverse(std::size_t root) {
    std::fill(adjoints_.begin(), adjoints_.end(), 0.0);
    adjoints_[root] = 1.0;
    for (std::size_t r = run_.size(); r-- > 0;) {
        const double adjoint = adjoints_[run_[r]];
        if (adjoint == 0.0) continue;
        for (std::size_t k = firstEdge_[r]; k < lastEdge_[r]; ++k) {
            const Edge& e = edges_[k];
            if (e.d != 0.0) adjoints_[e.input] += e.d * adjoint;
        }
    }
}

// The seed's tangent is 1. Each step, from the first run to the last, takes
// each partial times the tangent of the input it names. Every input a step
// reads is an argument or ran earlier, so its tangent is complete first. The
// same rule for a product with 0 holds as in the reverse sweep.
inline void Reduction::sweepForward(std::size_t seed) {
    std::fill(adjoints_.begin(), adjoints_.end(), 0.0);
    adjoints_[seed] = 1.0;
    for (std::size_t r = 0; r < run_.size(); ++r) {
        double tangent = 0.0;
        for (std::size_t k = firstEdge_[r]; k < lastEdge_[r]; ++k) {
            const Edge& e = edges_[k];
            const double t = adjoints_[e.input];
            if (t != 0.0 && e.d != 0.0) tangent += e.d * t;
        }
        adjoints_[run_[r]] = tangent;
    }
}

// One pass, which asks each step and passes its adjoint on at once: a reverse
// sweep needs a step's partials only when it comes to the step, so a gradient
// keeps none. It asks what gatherPartials() asks, in the same order, and passes
// adjoints on as sweepReverse() does.
inline double Reduction::gradient(std::span<const double> x, std::span<double> g) {
    if (g.size() != args_.size())
        throw std::invalid_argument("dag::Reduction: " + rootNames() + " has "
            + std::to_string(args_.size()) + " arguments, and its gradient was given room for "
            + std::to_string(g.size()));
    // The call, in line: a gradient is the hot call of a solver.
    if (!single_)
        throw std::logic_error("dag::Reduction: " + rootNames()
            + " are several values: ask for their Jacobian");
    run(x);
    const double value = get_value<double>(values_[root0_]);

    std::fill(adjoints_.begin(), adjoints_.end(), 0.0);
    adjoints_[root0_] = 1.0;
    const std::span<const std::size_t> inputs(inputs_);
    for (std::size_t r = run_.size(); r-- > 0;) {
        const std::size_t i = run_[r];
        const Entry& e = entries_[i];
        const std::span<const std::size_t> ins =
            inputs.subspan(e.firstInput, e.lastInput - e.firstInput);
        partials_.clear();
        if (!e.step->partials(ReduceInputs(values_, ins), partials_))
            throw std::domain_error("dag::Reduction: " + args_[movedBy_[i]]->name()
                + " reaches " + rootNames() + " through " + e.node->name()
                + ", whose step has no partials: the function can be called, and has no gradient");
        const double adjoint = adjoints_[i];
        for (const auto& p : partials_.entries()) {
            if (p.input >= ins.size())
                throw std::logic_error("dag::Reduction: the step of " + e.node->name()
                    + " names input " + std::to_string(p.input) + " of its "
                    + std::to_string(ins.size()));
            if (adjoint != 0.0 && p.d != 0.0) adjoints_[ins[p.input]] += p.d * adjoint;
        }
    }
    for (std::size_t k = 0; k < args_.size(); ++k)
        g[k] = argPosition_[k] == kNone ? 0.0 : adjoints_[argPosition_[k]];
    return value;
}

inline double Reduction::gradient(std::initializer_list<double> x, std::span<double> g) {
    return gradient(std::span<const double>(x.begin(), x.size()), g);
}

// The steps are asked once, and their partials kept. A Jacobian is then one
// sweep per root in reverse, or one per argument forward, and it takes
// whichever is fewer: forward when there are fewer arguments than roots. A
// sweep costs the same either way, since both read every partial once.
inline void Reduction::jacobian(std::span<const double> x, std::span<double> y,
                                std::span<double> j) {
    const std::size_t n = args_.size();
    const std::size_t m = roots_.size();
    if (y.size() != m)
        throw std::invalid_argument("dag::Reduction: " + rootNames() + ": " + std::to_string(m)
            + " values, and room for " + std::to_string(y.size()));
    if (j.size() != m * n)
        throw std::invalid_argument("dag::Reduction: " + rootNames() + ": a Jacobian of "
            + std::to_string(m) + " by " + std::to_string(n) + ", and room for "
            + std::to_string(j.size()));
    run(x);
    for (std::size_t r = 0; r < m; ++r) y[r] = get_value<double>(values_[rootPosition_[r]]);

    gatherPartials();
    if (n < m) {
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t seed = argPosition_[k];
            // An argument no root depends on here moves nothing.
            const bool moves = seed != kNone && movedBy_[seed] != kNone;
            if (moves) sweepForward(seed);
            for (std::size_t r = 0; r < m; ++r)
                j[r * n + k] = moves ? adjoints_[rootPosition_[r]] : 0.0;
        }
    } else {
        for (std::size_t r = 0; r < m; ++r) {
            sweepReverse(rootPosition_[r]);
            for (std::size_t k = 0; k < n; ++k)
                j[r * n + k] = argPosition_[k] == kNone ? 0.0 : adjoints_[argPosition_[k]];
        }
    }
}

inline void Reduction::jacobian(std::initializer_list<double> x, std::span<double> y,
                                std::span<double> j) {
    jacobian(std::span<const double>(x.begin(), x.size()), y, j);
}

inline std::size_t Reduction::arity() const noexcept { return args_.size(); }

inline std::size_t Reduction::roots() const noexcept { return roots_.size(); }

inline std::size_t Reduction::size() const noexcept { return bound_ ? run_.size() : 0; }

// ─────────────────────────────────────────────────────────────────────────────
// Free functions
// ─────────────────────────────────────────────────────────────────────────────

inline Reduction reduce(NodePtr root, std::vector<NodePtr> args) {
    Reduction f(std::move(root), std::move(args));
    f.bind();
    return f;
}

inline Reduction reduce(std::vector<NodePtr> roots, std::vector<NodePtr> args) {
    Reduction f(std::move(roots), std::move(args));
    f.bind();
    return f;
}

} // namespace dag
