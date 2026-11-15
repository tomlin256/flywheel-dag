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
    choosing_.assign(entries_.size(), kNone);
    binds_.assign(entries_.size(), 0);
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].step && entries_[i].step->binds()) { binds_[i] = 1; anyBinds_ = true; }
    followed_.reserve(inputs_.size());
    run_.reserve(candidates);
    live_.reserve(args_.size());
    own_.assign(args_.size(), 0.0);
    rootValue_.resize(roots_.size());
    argValues_.resize(args_.size());
    adjoints_.assign(entries_.size(), 0.0);
    wanted_.assign(entries_.size(), 0);
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
        why = "is a time-series node that gives no step: it cannot apply its last update to"
              " another sample";
    else if (e.hooks && e.hooks->invalidationMode() == InvalidationMode::Eager)
        why = "is Eager, and gives no step. A compute node gives one when it is built with"
              " InvalidationMode::Lazy, which says that its functor is a pure function of its"
              " inputs";
    else
        why = "gives no step: nothing says that its function may be called at other values";
    return "dag::Reduction: " + args_[by]->name() + " reaches " + rootNames() + " through "
         + e.node->name() + ", which " + why;
}

inline std::string Reduction::staleMessage(std::size_t chooser, std::size_t stale) const {
    const Entry& e = entries_[chooser];
    std::string decider = "an input";
    for (std::size_t k = e.firstInput; k < e.lastInput; ++k)
        if (uses_[k] == ReduceReads::Use::Decides) decider = entries_[inputs_[k]].node->name();
    const std::string& other = entries_[inputs_[stale]].node->name();
    return "dag::Reduction: " + rootNames() + ": " + args_[choosing_[chooser]]->name() + " moves "
         + decider + ", which decides what " + e.node->name() + " reads, and " + other
         + ", which the graph did not read at this point, is dirty. A reduction evaluates"
           " nothing, so it follows a choice that an argument moves only where every input of"
           " the choosing node has been evaluated: evaluate " + other
         + ", or give it as an input to the node that binds the reduction";
}

inline void Reduction::release() noexcept {
    bound_ = false;
    for (auto& v : values_) v.reset();
    run_.clear();
    live_.clear();
}

// The roots are checked clean first, and a clean node's inputs are clean too:
// an input that moves invalidates its consumers, and a node does not end an
// evaluation clean over an input that went dirty again. So every eval() that
// rebind() makes returns a cached value, an always-dirty node's apart.
//
// A bind that throws here holds nothing of the last point.
inline void Reduction::bind() {
    if (single_) {
        // One root: no loop over a list of one.
        if (root0Node_->dirty()) {
            release();
            throw std::invalid_argument("dag::Reduction: root " + root0Node_->name()
                + " is dirty. A reduction reads only evaluated values: evaluate it first");
        }
    } else {
        for (const auto& root : roots_) {
            if (!root->dirty()) continue;
            release();
            throw std::invalid_argument("dag::Reduction: root " + root->name()
                + " is dirty. A reduction reads only evaluated values: evaluate it first");
        }
    }
    rebind();
}

// The last point's values go first: a bind holds only what its own point
// reads. Then the point is read, and read again for as long as a reading finds
// a node that an argument makes choose: such a node reads every input it has,
// so the reading that found it stopped short of the inputs the graph did not
// read. Each reading marks at least one more node, and no mark is taken back,
// so the readings end. With no such node there is one reading.
inline void Reduction::rebind() {
    release();
    EvalContext ctx;

    // One root, held in a local as its position is held in a member: a
    // reduction of one root takes no loop over a list of one.
    if (single_) {
        const ValuePtr rootValue = root0Node_->eval(ctx);
        if (!holdsDouble(rootValue))
            throw std::invalid_argument("dag::Reduction: root " + root0Node_->name()
                                        + " does not hold a double");
        if (chose_) {
            std::fill(choosing_.begin(), choosing_.end(), kNone);
            followed_.clear();
            chose_ = false;
        }
        if (readPoint<false>(ctx)) readFollowing(ctx);
        precise_ = chose_;
        if (anyBinds_)
            for (const std::size_t i : run_)
                if (binds_[i] != 0) entries_[i].step->bind(ctx);
        if (movedBy_[root0_] == kNone) values_[root0_] = rootValue;
        bound_ = true;
        return;
    }

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

    if (chose_) {
        std::fill(choosing_.begin(), choosing_.end(), kNone);
        followed_.clear();
        chose_ = false;
    }
    if (readPoint<false>(ctx)) readFollowing(ctx);
    precise_ = chose_;

    // Each step that runs keeps what it needs of its node at this point: a
    // stateful node's state, and the sample its last update took. Once, after
    // the last reading of the point, which is the one that stands.
    if (anyBinds_)
        for (const std::size_t i : run_)
            if (binds_[i] != 0) entries_[i].step->bind(ctx);

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
// time the loop comes to an entry, it is reached or it is not. A node of
// choosing_ reads every input. One that the graph did not read must be clean:
// then something has evaluated it at this point, everything it read is clean
// too, and reduceReads() can be asked of the nodes above it.
//
// From the leaves down: which of those an argument moves. An argument does. A
// candidate does if an input it reads does, and it then runs as a step, reading
// from the graph the value of each input that no argument moves. A candidate
// that reads no moved input is a constant here, and nothing above it is read on
// its account. A candidate whose deciding input an argument moves joins
// choosing_, and the reading is then taken again: what this one goes on to
// find is a subset of what the next one finds, so a throw from here is one the
// next would make as well.
template<bool Following>
inline bool Reduction::readPoint(EvalContext& ctx) {
    std::fill(reached_.begin(), reached_.end(), std::uint8_t{0});
    std::fill(uses_.begin(), uses_.end(), ReduceReads::Use::Unread);
    if (Following) followed_.clear();
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
        if (Following && choosing_[i] != kNone) {
            for (std::size_t k = e.firstInput; k < e.lastInput; ++k) {
                if (uses_[k] != ReduceReads::Use::Unread) continue;
                if (entries_[inputs_[k]].node->dirty())
                    throw std::domain_error(staleMessage(i, k));
                uses_[k] = ReduceReads::Use::Read;
                followed_.push_back(inputs_[k]);
            }
        }
        for (std::size_t k = e.firstInput; k < e.lastInput; ++k)
            if (uses_[k] != ReduceReads::Use::Unread) reached_[inputs_[k]] = 1;
    }

    bool found = false;
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
            own_[e.argument] = static_cast<const TypedValue<double>*>(held.get())->get();
            movedBy_[i] = e.argument;
            live_.push_back(e.argument);
            continue;
        }

        std::size_t by = kNone;
        for (std::size_t k = e.firstInput; k < e.lastInput; ++k) {
            if (uses_[k] == ReduceReads::Use::Unread) continue;
            const std::size_t mover = movedBy_[inputs_[k]];
            if (mover == kNone) continue;
            if (uses_[k] == ReduceReads::Use::Decides) {
                // The first reading stops here: the readings that follow take
                // the point again.
                if (!Following) { chose_ = true; return true; }
                if (choosing_[i] == kNone) {
                    choosing_[i] = mover;
                    found = true;
                }
            }
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
    return found;
}

// The function reads every input in followed_, and the root that was bound
// does not hear them: a ConditionNode hears only the branch it took. A node
// that binds, and is not given such an input as one of its own, is not told
// when the input moves, and would keep a value the function has moved from. An
// argument needs no hearing: the function takes its value from the caller.
inline void Reduction::requireHeard(std::span<const NodePtr> heard,
                                    const std::string& binder) const {
    for (const std::size_t p : followed_) {
        const Entry& e = entries_[p];
        if (e.role == Role::Argument) continue;
        const bool given = std::any_of(heard.begin(), heard.end(),
            [&e](const NodePtr& h) { return h == e.node; });
        if (!given) throw std::domain_error(unheardMessage(p, binder));
    }
}

inline std::string Reduction::unheardMessage(std::size_t unheard, const std::string& binder) const {
    const std::string& input = entries_[unheard].node->name();
    return "dag::Reduction: " + rootNames() + ": the function reads " + input
         + ", which the graph did not read at this point, and " + binder
         + " is not given it as an input, so it would not hear it move. Give " + binder + " "
         + input + " as an input";
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

// A stamp that wraps would meet the stamps of 2³² passes ago, so the marks are
// cleared then.
inline void Reduction::nextPass() {
    if (++pass_ == 0) {
        std::fill(wanted_.begin(), wanted_.end(), std::uint32_t{0});
        pass_ = 1;
    }
}

// The call leaves every step's inputs in values_. A derivative passes through
// the roots, and through each input that the partials of a step it passes
// through name. A step runs after every step it reads, so from the last run to
// the first, each step is known to be wanted, or not, before it is asked.
//
// A step the derivative does not pass through is not asked: the node that
// computes a condition, and the steps of a branch that a choosing node did not
// select at this call. A step that is asked must have partials, whatever its
// adjoint turns out to be.
inline void Reduction::gatherPartials() {
    nextPass();
    for (const std::size_t p : rootPosition_) wanted_[p] = pass_;
    edges_.clear();
    const std::span<const std::size_t> inputs(inputs_);
    for (std::size_t r = run_.size(); r-- > 0;) {
        const std::size_t i = run_[r];
        firstEdge_[r] = edges_.size();
        lastEdge_[r]  = edges_.size();
        if (wanted_[i] != pass_) continue;
        const Entry& e = entries_[i];
        const std::span<const std::size_t> ins =
            inputs.subspan(e.firstInput, e.lastInput - e.firstInput);
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
            wanted_[ins[p.input]] = pass_;
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

// gatherPartials()'s marks and sweepReverse()'s adjoints in one pass, from the
// root: a reverse sweep needs a step's partials only when it comes to the
// step, so the marked sweep keeps none.
inline void Reduction::sweepMarked() {
    std::fill(adjoints_.begin(), adjoints_.end(), 0.0);
    nextPass();
    const std::size_t root = rootPosition_.front();
    adjoints_[root] = 1.0;
    wanted_[root]   = pass_;
    const std::span<const std::size_t> inputs(inputs_);
    for (std::size_t r = run_.size(); r-- > 0;) {
        const std::size_t i = run_[r];
        if (wanted_[i] != pass_) continue;
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
            wanted_[ins[p.input]] = pass_;
            // A partial times an adjoint is 0 when either is 0, so neither 0·∞
            // nor 0·NaN makes a NaN. A NaN that is not multiplied by 0 does
            // propagate.
            if (adjoint != 0.0 && p.d != 0.0) adjoints_[ins[p.input]] += p.d * adjoint;
        }
    }
}

// One pass, which asks each step and passes its adjoint on at once, as
// sweepMarked() does, and gives the gradient that gatherPartials()'s rule
// gives.
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
    if (precise_) [[unlikely]] return gradientMarked(value, g);

    // Every step is asked, in one pass, for as long as every step answers. A
    // step with no partials, or one that names an input it does not have, may
    // be one the derivative does not pass through, which only the marks can
    // say: the marked sweep then starts again. A step the derivative does not
    // pass through has an adjoint of 0, so asking it here passes nothing on.
    std::fill(adjoints_.begin(), adjoints_.end(), 0.0);
    adjoints_[root0_] = 1.0;
    const std::span<const std::size_t> inputs(inputs_);
    for (std::size_t r = run_.size(); r-- > 0;) {
        const std::size_t i = run_[r];
        const Entry& e = entries_[i];
        const std::span<const std::size_t> ins =
            inputs.subspan(e.firstInput, e.lastInput - e.firstInput);
        partials_.clear();
        if (!e.step->partials(ReduceInputs(values_, ins), partials_)) [[unlikely]]
            return gradientMarked(value, g);
        const double adjoint = adjoints_[i];
        for (const auto& p : partials_.entries()) {
            if (p.input >= ins.size()) [[unlikely]] return gradientMarked(value, g);
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

// bind() reads each argument a step reads, to see that it holds a double, and
// keeps what it held.
inline double Reduction::argument(std::size_t i) const {
    if (!bound_)
        throw std::logic_error("dag::Reduction: " + rootNames()
            + (roots_.size() == 1 ? " is" : " are") + " not bound: bind() it first");
    if (i >= args_.size())
        throw std::out_of_range("dag::Reduction: argument " + std::to_string(i) + " of "
            + rootNames() + ", which " + (roots_.size() == 1 ? "has " : "have ")
            + std::to_string(args_.size()));
    const std::size_t p = argPosition_[i];
    if (p == kNone || movedBy_[p] == kNone)
        throw std::domain_error("dag::Reduction: " + rootNames()
            + (roots_.size() == 1 ? " does" : " do") + " not depend on argument "
            + args_[i]->name() + " at the bound point, so its value was not read");
    return own_[i];
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

// ─────────────────────────────────────────────────────────────────────────────
// ReductionNode<Out, Ins...>
// ─────────────────────────────────────────────────────────────────────────────

template<typename Out, typename... Ins>
ReductionNodePtr<Out, Ins...> ReductionNode<Out, Ins...>::make(
    std::string name, NodePtr root, std::vector<NodePtr> args, InputTuple ins, Fn fn,
    EqualityPolicyPtr eq)
{
    return make(std::move(name), std::vector<NodePtr>{std::move(root)}, std::move(args),
                std::move(ins), std::move(fn), std::move(eq));
}

// A functor that returns its value is run as one that writes it: the value is
// assigned to `out`, which for an Out that owns heap gives up the storage `out`
// held. That is what a ComputeNode's functor costs too. makeInPlace() is the
// way to keep the storage.
template<typename Out, typename... Ins>
ReductionNodePtr<Out, Ins...> ReductionNode<Out, Ins...>::make(
    std::string name, std::vector<NodePtr> roots, std::vector<NodePtr> args, InputTuple ins,
    Fn fn, EqualityPolicyPtr eq)
{
    return makeInPlace(std::move(name), std::move(roots), std::move(args), std::move(ins),
        InPlaceFn([fn = std::move(fn)](Out& out, Reduction& f, const Ins&... in) {
            out = fn(f, in...);
        }),
        std::move(eq));
}

template<typename Out, typename... Ins>
ReductionNodePtr<Out, Ins...> ReductionNode<Out, Ins...>::makeInPlace(
    std::string name, NodePtr root, std::vector<NodePtr> args, InputTuple ins, InPlaceFn fn,
    EqualityPolicyPtr eq)
{
    return makeInPlace(std::move(name), std::vector<NodePtr>{std::move(root)}, std::move(args),
                       std::move(ins), std::move(fn), std::move(eq));
}

template<typename Out, typename... Ins>
ReductionNodePtr<Out, Ins...> ReductionNode<Out, Ins...>::makeInPlace(
    std::string name, std::vector<NodePtr> roots, std::vector<NodePtr> args, InputTuple ins,
    InPlaceFn fn, EqualityPolicyPtr eq)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<Out>>();
    auto self = std::shared_ptr<ReductionNode>(new ReductionNode(
        std::move(name), std::move(roots), std::move(args), std::move(ins), std::move(fn),
        std::move(eq)));
    wire(self, self->inputs());
    return self;
}

// Eager, fixed here, with no mode to take: see the class comment. The
// reduction is built here, so a null root or a repeated argument throws from
// make(), before the node is wired.
template<typename Out, typename... Ins>
ReductionNode<Out, Ins...>::ReductionNode(
    std::string name, std::vector<NodePtr> roots, std::vector<NodePtr> args, InputTuple ins,
    InPlaceFn fn, EqualityPolicyPtr eq)
    : NodeBase(InvalidationMode::Eager), name_(std::move(name)), roots_(roots)
    , inputs_(std::move(ins)), fn_(std::move(fn)), eq_(std::move(eq))
    , reduction_(std::move(roots), std::move(args))
{
    heard_ = roots_;
    collectInputs(heard_, std::index_sequence_for<Ins...>{});
}

// The roots are pulled first, in order, and then the other inputs: by the time
// the reduction is bound, everything the node was given has been evaluated, an
// input that is a branch its root did not take included.
//
// A throw from the bind or from the functor leaves this node dirty, and cached_
// as it was.
template<typename Out, typename... Ins>
ValuePtr ReductionNode<Out, Ins...>::eval(EvalContext& ctx) {
    if (!dirty() && !ctx.forceRecompute) return cached_;
    beginEval();
    for (const auto& root : roots_) root->eval(ctx);
    return applyInputs(ctx, std::index_sequence_for<Ins...>{});
}

// ComputeNode's two paths, for the inputs the functor is handed: copied out
// when every type is trivially copyable, and otherwise bound by reference into
// values that `held` keeps alive and unmodified for the call. See
// ComputeNode::applyInputs in dag.inl.
//
// The bind takes no clean root for granted: an always-dirty node that reaches a
// root by two paths leaves it dirty after its pull (Reduction::rebind()).
template<typename Out, typename... Ins>
template<std::size_t... Is>
ValuePtr ReductionNode<Out, Ins...>::applyInputs(
    [[maybe_unused]] EvalContext& ctx, std::index_sequence<Is...>)
{
    if constexpr ((std::is_trivially_copyable_v<Ins> && ...)) {
        [[maybe_unused]] const std::tuple<Ins...> vals{
            get_value<Ins>(std::get<Is>(inputs_)->eval(ctx))... };
        reduction_.rebind();
        reduction_.requireHeard(heard_, name_);
        fn_(scratch_, reduction_, std::get<Is>(vals)...);
    } else {
        const ValuePtr held[] = { std::get<Is>(inputs_)->eval(ctx)... };
        reduction_.rebind();
        reduction_.requireHeard(heard_, name_);
        fn_(scratch_, reduction_, get_value<Ins>(held[Is])...);
    }
    return publish();
}

// InPlaceComputeNode's publish: emit takes a const lvalue, so the slot
// copy-assigns scratch_ into a recycled buffer, and neither gives its storage
// back.
template<typename Out, typename... Ins>
ValuePtr ReductionNode<Out, Ins...>::publish() {
    auto newV = slot_.emit(std::as_const(scratch_));
    if (!eq_->equal(cached_, newV)) {
        cached_ = newV;
        notifyDownstream();
    }
    endEval();
    return cached_;
}

template<typename Out, typename... Ins>
std::string ReductionNode<Out, Ins...>::name() const { return name_; }

template<typename Out, typename... Ins>
std::vector<NodePtr> ReductionNode<Out, Ins...>::inputs() const { return heard_; }

template<typename Out, typename... Ins>
template<std::size_t... Is>
void ReductionNode<Out, Ins...>::collectInputs(
    [[maybe_unused]] std::vector<NodePtr>& out, std::index_sequence<Is...>) const
{
    (out.push_back(std::get<Is>(inputs_)), ...);
}

} // namespace dag
