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

// One walk up from the root, depth first and without recursion. It leaves a
// node only after every input, so a candidate lands in entries_ after
// everything it names, and the root lands last.
//
// An argument is a leaf of the walk. The function is of the argument's own
// value, so nothing above it is read, another argument included.
//
// A node with no argument above it gets no entry of its own. It gets one, as a
// Constant, only when a candidate names it, or when it is the root.
inline Reduction::Reduction(NodePtr root, std::vector<NodePtr> args)
    : root_(std::move(root)), args_(std::move(args))
{
    if (!root_) throw std::invalid_argument("dag::Reduction: the root is null");
    if (args_.empty())
        throw std::invalid_argument("dag::Reduction: root " + root_->name() + " has no arguments");
    std::unordered_map<const INode*, std::size_t> argument;   // a node's position in args_
    for (std::size_t i = 0; i < args_.size(); ++i) {
        if (!args_[i])
            throw std::invalid_argument("dag::Reduction: root " + root_->name()
                                        + " has a null argument");
        if (!argument.emplace(args_[i].get(), i).second)
            throw std::invalid_argument("dag::Reduction: " + args_[i]->name()
                + " is an argument of root " + root_->name() + " twice");
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

    visit(root_);
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
            Entry e{top.node, Role::Candidate, kNone, first, inputs_.size(),
                    std::dynamic_pointer_cast<IReducible>(top.node), nullptr};
            if (e.reducible) e.step = e.reducible->reduceStep();
            position.emplace(top.node.get(), entries_.size());
            entries_.push_back(std::move(e));
        }
        stack.pop_back();
    }
    rootPosition_ = entryOf(root_, Role::Constant, kNone);

    // Everything bind() and a call write to is sized here, so neither allocates.
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
    argValues_.resize(args_.size());
}

inline bool Reduction::holdsDouble(const ValuePtr& v) noexcept {
    return v && v->type() == std::type_index(typeid(double));
}

inline std::string Reduction::barrierMessage(std::size_t barrier, std::size_t by) const {
    const Entry& e = entries_[barrier];
    const std::string why = e.reducible
        ? "gives no step"
        : "is not an IReducible, so a reduction has no function of it to call";
    return "dag::Reduction: " + args_[by]->name() + " reaches root " + root_->name()
         + " through " + e.node->name() + ", which " + why;
}

// Two passes over entries_, which holds every node after the inputs it names.
//
// From the root up: what the root depends on at this point, through the inputs
// each node reads. Every consumer of an entry sits after it, so by the time the
// loop comes to an entry, it is reached or it is not.
//
// From the leaves down: which of those an argument moves. An argument does. A
// candidate does if an input it reads does, and it then runs as a step, reading
// from the graph the value of each input that no argument moves. A candidate
// that reads no moved input is a constant here, and nothing above it is read on
// its account.
//
// The root is checked clean first, and a clean node's inputs are clean too: an
// input that moves invalidates its consumers, and a node does not end an
// evaluation clean over an input that went dirty again. So every eval() below
// returns a cached value, an always-dirty node's apart.
inline void Reduction::bind() {
    bound_ = false;
    // The last point's values go first: a bind holds only what its own point
    // reads, and one that throws holds nothing.
    for (auto& v : values_) v.reset();
    run_.clear();
    live_.clear();

    if (root_->dirty())
        throw std::invalid_argument("dag::Reduction: root " + root_->name()
            + " is dirty. A reduction reads only evaluated values: evaluate it first");
    EvalContext ctx;
    const ValuePtr rootValue = root_->eval(ctx);
    if (!holdsDouble(rootValue))
        throw std::invalid_argument("dag::Reduction: root " + root_->name()
                                    + " does not hold a double");

    std::fill(reached_.begin(), reached_.end(), std::uint8_t{0});
    std::fill(uses_.begin(), uses_.end(), ReduceReads::Use::Unread);
    reached_[rootPosition_] = 1;
    for (std::size_t i = entries_.size(); i-- > 0;) {
        const Entry& e = entries_[i];
        if (reached_[i] == 0 || e.role != Role::Candidate) continue;
        const std::span<ReduceReads::Use> uses =
            std::span(uses_).subspan(e.firstInput, e.lastInput - e.firstInput);
        if (e.reducible) {
            ReduceReads reads(uses);
            e.reducible->reduceReads(ctx, reads);
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
            // Reached, so a step reads it, or it is the root. A call hands it
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
                throw std::domain_error("dag::Reduction: root " + root_->name() + ": "
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
            if (movedBy_[in] == kNone && !values_[in]) values_[in] = entries_[in].node->eval(ctx);
        }
        run_.push_back(i);
    }

    // A root that no argument moves is the whole function.
    if (movedBy_[rootPosition_] == kNone) values_[rootPosition_] = rootValue;
    bound_ = true;
}

// An argument's value is published through a slot of the reduction's, as a
// step's is through the step's, so a warm call allocates nothing. Each value
// stays in values_ until the next call replaces it, which is what lets a slot
// hand out its other buffer.
inline double Reduction::operator()(std::span<const double> x) {
    if (!bound_)
        throw std::logic_error("dag::Reduction: root " + root_->name()
                               + " is not bound: bind() it first");
    if (x.size() != args_.size())
        throw std::invalid_argument("dag::Reduction: root " + root_->name() + " takes "
            + std::to_string(args_.size()) + " arguments, and was given "
            + std::to_string(x.size()));

    for (const std::size_t a : live_) values_[argPosition_[a]] = argValues_[a].emit(x[a]);
    const std::span<const std::size_t> inputs(inputs_);
    for (const std::size_t i : run_) {
        const Entry& e = entries_[i];
        values_[i] = e.step->apply(
            ReduceInputs(values_, inputs.subspan(e.firstInput, e.lastInput - e.firstInput)));
    }
    return get_value<double>(values_[rootPosition_]);
}

inline double Reduction::operator()(std::initializer_list<double> x) {
    return (*this)(std::span<const double>(x.begin(), x.size()));
}

inline std::size_t Reduction::arity() const noexcept { return args_.size(); }

inline std::size_t Reduction::size() const noexcept { return bound_ ? run_.size() : 0; }

// ─────────────────────────────────────────────────────────────────────────────
// Free functions
// ─────────────────────────────────────────────────────────────────────────────

inline Reduction reduce(NodePtr root, std::vector<NodePtr> args) {
    Reduction f(std::move(root), std::move(args));
    f.bind();
    return f;
}

} // namespace dag
