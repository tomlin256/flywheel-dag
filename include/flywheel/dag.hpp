// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
#include <memory>
#include <vector>
#include <functional>
#include <stdexcept>
#include <typeindex>
#include <tuple>
#include <utility>
#include <optional>
#include <type_traits>
#include <cstdint>

namespace dag {

class INode;
using NodePtr = std::shared_ptr<INode>;

// ─────────────────────────────────────────────────────────────────────────────
// IValue — type-erased value container
// ─────────────────────────────────────────────────────────────────────────────
class IValue {
public:
    virtual ~IValue() = default;

    /// The stored type. Non-virtual by design: the type is fixed at
    /// construction, so this is a plain load rather than a vtable dispatch on a
    /// path taken once per input per node per cycle.
    std::type_index type() const noexcept { return type_; }

protected:
    explicit IValue(std::type_index t) noexcept : type_(t) {}

private:
    std::type_index type_;
};

template<typename T>
class ValueSlot;

// final: TypedValue<T> is the only IValue implementation and nothing derives
// from it. Saying so lets get_value() replace a dynamic_cast with an exact
// type_index compare — the two are equivalent only while this holds.
template<typename T>
class TypedValue final : public IValue {
public:
    explicit TypedValue(T v);
    const T& get() const;
private:
    // Only ValueSlot may rewrite a value in place, and only after proving it is
    // the sole owner of the buffer. A TypedValue is immutable to everyone else.
    friend class ValueSlot<T>;
    // Copy-assign from an lvalue rather than move-assign through a by-value
    // parameter: for a T with heap members, assigning into value_ reuses the
    // storage it already owns, where a move would free exactly the buffer this
    // class exists to recycle. See ValueSlot's "Copy or move" note below.
    void set(const T& v) { value_ = v; }
    void set(T&& v)      { value_ = std::move(v); }

    T value_;
};

using ValuePtr = std::shared_ptr<const IValue>;

template<typename T>
ValuePtr make_value(T v);

template<typename T>
const T& get_value(const ValuePtr& v);

// ─────────────────────────────────────────────────────────────────────────────
// ValueSlot<T> — recycles two TypedValue<T> buffers so a node's steady-state
// eval allocates nothing.
//
// Before this, every recomputing node called make_value() on every cycle: a
// heap allocation to carry, typically, one double. Measured at 16 allocations
// per cycle on a 14-node graph, growing linearly with the graph.
//
// How it stays safe
// ─────────────────
// A buffer is rewritten ONLY when this slot holds the last reference to it
// (use_count() == 1). That single check carries two guarantees:
//
//   1. Nobody can observe a value changing under them. Every other holder —
//      the node's own cached_, the engine's outputs_[i].lastSeen, a caller
//      holding the ValuePtr eval() returned — bumps the count and takes the
//      buffer out of the running.
//
//   2. The returned pointer is never the one cached_ already holds. cached_
//      itself is a reference, so its buffer always reads use_count() >= 2. The
//      equality check (cached_ vs new) and the engine's pointer-identity change
//      detection both depend on those being distinct objects.
//
// Two buffers is exactly enough: cached_ pins at most one, leaving the other
// free. When neither is free — a downstream consumer is holding both — it
// falls back to allocating, which is always correct, just not free.
//
// Copy or move
// ────────────
// Recycling the TypedValue is only half the job for a T that owns heap of its
// own. A move-assign into the buffer frees the very vectors being recycled and
// steals fresh ones, so a heap-holding T allocated on every emit no matter how
// warm the slot was (two allocations per update for a value holding two vectors).
//
// So there are two overloads, and which one a caller gets is the right answer
// for what that caller has:
//
//   emit(const T&) — the caller keeps its value (AsyncInput::flush stages into
//     a member it reuses next cycle). Copy-assigning reuses the buffer's
//     capacity, and the source keeps its own: nothing allocates.
//
//   emit(T&&) — the caller built the value for this emit alone (every compute,
//     op and time-series node). Its storage is already paid for, so there is
//     nothing to recycle and a copy would be pure added work.
//
// Eval-thread only
// ────────────────
// use_count() is a sound sole-ownership test only when every copy of the
// pointer is made on one thread. This is fine for compute/time-series/op nodes
// and for AsyncInput::flush(), all of which the engine drives on the eval
// thread. It is NOT fine for Input<T>::set(), which Engine::makeInput documents
// as callable from application code — so Input<T>::set() deliberately keeps
// allocating. Do not add a slot to anything reachable off the eval thread.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class ValueSlot {
public:
    /// Emit a value the caller keeps. Copy-assigns into the recycled buffer, so
    /// a T with heap members keeps that buffer's capacity.
    ValuePtr emit(const T& v) { return emitImpl(v); }

    /// Emit a value the caller is done with. Move-assigns, as before.
    ValuePtr emit(T&& v) { return emitImpl(std::move(v)); }

private:
    template<typename U>
    ValuePtr emitImpl(U&& v) {
        for (auto& slot : buf_) {
            if (slot && slot.use_count() == 1) {
                slot->set(std::forward<U>(v));
                return slot;
            }
        }
        // Both buffers are still referenced elsewhere. Replacing our handle on
        // one is safe — the other owners keep it alive through their own
        // shared_ptr — and gives this slot something to recycle next time.
        buf_[next_] = std::make_shared<TypedValue<T>>(std::forward<U>(v));
        ValuePtr fresh = buf_[next_];
        next_ ^= 1u;
        return fresh;
    }

    std::shared_ptr<TypedValue<T>> buf_[2];
    unsigned next_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// IEqualityPolicy — controls what "change" means for a node's output.
//
// WHERE IT GATES WORK. A policy's verdict decides whether a node calls
// notifyDownstream(), and what that buys depends on where the node sits and on
// its InvalidationMode:
//
//   • at a SOURCE (Input::set, AsyncInput::flush) — it gates the cascade itself.
//   • at a REGISTERED ENGINE OUTPUT — it gates the addOutput callback, because
//     Engine::cycle fires on ValuePtr identity and an "equal" verdict keeps
//     cached_ pointing at the same buffer.
//   • at an INTERMEDIATE node with LAZY consumers — it gates their recomputation.
//     A consumer resolves by pulling its inputs and seeing whether any of them
//     said "changed"; a policy that says "equal" is what makes it skip.
//   • at an INTERMEDIATE node whose consumers are all EAGER — it costs a
//     comparison and suppresses nothing, because an Eager node recomputes
//     whenever anything upstream fired. Free for a double, real work for a
//     container; such a node should take AlwaysChangedPolicy.
//
// The last two are the same node with different consumers, which is why this is
// a per-node judgement and not a rule. Held by
// DAGTests.EqualityPolicyOnIntermediateNodeDoesNotSuppressDownstreamEval and its
// ...DoesSuppress... sibling, which assert opposite outcomes on the same graph
// and are both correct.
//
// WHAT IT COMPARES. The new value against the last one the node PUBLISHED, which
// is cached_, not against the last one it computed. Only an "unequal" verdict
// rebinds cached_. Under a tolerance policy this is the difference between
// publishing a slow drift once it has moved the tolerance in total and never
// publishing it at all. Held for stateful nodes by
// StatefulNodeBase.EqualityPolicyComparesAgainstTheLastPublishedValue.
// ─────────────────────────────────────────────────────────────────────────────
class IEqualityPolicy {
public:
    virtual ~IEqualityPolicy() = default;
    /// Return true if old == new (no change). Returning false rebinds the node's
    /// cached_ and re-notifies downstream — which is NOT the same as being what
    /// dirties them; see the note above for what that buys and where.
    virtual bool equal(const ValuePtr& oldVal, const ValuePtr& newVal) const = 0;
};

class AlwaysChangedPolicy : public IEqualityPolicy {
public:
    bool equal(const ValuePtr&, const ValuePtr&) const override;
};

template<typename T>
class TypedEqualityPolicy : public IEqualityPolicy {
public:
    bool equal(const ValuePtr& a, const ValuePtr& b) const override;
};

class PredicateEqualityPolicy : public IEqualityPolicy {
public:
    using Pred = std::function<bool(const ValuePtr&, const ValuePtr&)>;
    explicit PredicateEqualityPolicy(Pred p);
    bool equal(const ValuePtr& a, const ValuePtr& b) const override;
private:
    Pred pred_;
};

template<typename T,
         typename = std::enable_if_t<std::is_arithmetic_v<T>>>
class EpsilonPolicy : public IEqualityPolicy {
public:
    explicit EpsilonPolicy(T e);
    bool equal(const ValuePtr& a, const ValuePtr& b) const override;
private:
    T eps_;
};

using EqualityPolicyPtr = std::shared_ptr<IEqualityPolicy>;

// ─────────────────────────────────────────────────────────────────────────────
// EvalContext — carries per-evaluation settings
// ─────────────────────────────────────────────────────────────────────────────
class EvalContext {
public:
    bool forceRecompute = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// NodeKind — discriminator for graph export and visualisation.
// ─────────────────────────────────────────────────────────────────────────────
enum class NodeKind { Input, AsyncInput, AsyncQueue, Compute, TimeSeries };

// ─────────────────────────────────────────────────────────────────────────────
// Dirtiness — three states, where there used to be two.
//
// The old protocol had one bit: dirty or not. invalidate() was transitive, so a
// source that moved marked its ENTIRE reachable subgraph dirty in one walk, at
// flush time, before any value existed to compare. An equality policy on an
// intermediate node therefore could not gate anything below it — everything
// below it was already committed to re-evaluating.
//
// The third state splits "dirty" into the two things it was conflating:
//
//   Dirty  an INPUT OF MINE definitely changed its value. Set by the one hop
//          from a node whose eval() saw its own value change.
//   Maybe  an ANCESTOR of mine may have changed. Set by the transitive cascade.
//          Whether this node actually has to recompute is not yet known, and is
//          resolved when it is pulled (NodeBase's resolve step).
//   Clean  up to date.
//
// dirty() is `state_ != Clean`, so everything that reads it — the engine's
// pre-eval snapshot above all — keeps working unchanged.
// ─────────────────────────────────────────────────────────────────────────────
enum class Dirtiness : std::uint8_t { Clean, Maybe, Dirty };

// ─────────────────────────────────────────────────────────────────────────────
// InvalidationMode — per-node, set at construction, never changed.
//
// Eager  recompute whenever anything upstream fired. The original behaviour, and
//        the DEFAULT everywhere. It is also the correct and PERMANENT answer for
//        any node whose output is not a pure function of its declared inputs'
//        VALUES — every dag::ts stateful node is one (an EWMA fed a value equal
//        to the last one still has to tick, because its output depends on how
//        many times it was evaluated), as is any functor that reads state it did
//        not declare as an input.
//
// Lazy   recompute only when an input's value actually changed. Requires the
//        functor to be a pure function of its declared inputs.
//
// Not a legacy escape hatch: which of the two a node wants is a property of its
// functor, and only the functor's author knows it. That is why the mode is a
// constructor parameter set in the factory that WRITES the functor, and why
// there is no setter: a post-construction modifier would let a graph site vouch
// for the purity of a functor it cannot see.
// ─────────────────────────────────────────────────────────────────────────────
enum class InvalidationMode : std::uint8_t { Eager, Lazy };

// ─────────────────────────────────────────────────────────────────────────────
// INode — the core interface.
// ─────────────────────────────────────────────────────────────────────────────
class INode {
public:
    virtual ~INode() = default;
    virtual ValuePtr eval(EvalContext& ctx) = 0;
    virtual std::string name() const = 0;
    virtual std::vector<NodePtr> inputs() const = 0;
    /// "An input of yours definitely changed." One hop; the transitive part of
    /// it is the maybe-cascade below.
    virtual void invalidate() = 0;
    /// "An ancestor of yours may have changed." The transitive cascade.
    virtual void invalidateMaybe() = 0;
    virtual bool dirty() const = 0;
    virtual void addDownstream(std::weak_ptr<INode>) = 0;
    virtual NodeKind kind() const = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// NodeBase — the one copy of the dirty/downstream protocol.
//
// Every node in the engine kept its own identical `downstream_` vector, dirty
// flag, dirty(), invalidate(), addDownstream() and notifyDownstream() — twelve
// copies of the same eight lines, across dag.hpp, dag_async.hpp, dag_ops.hpp,
// dag_timeseries.hpp, dag_memoize.hpp, dag_replay.hpp and two nodes defined
// outside the engine. They now live here once.
//
// `downstream_` is PRIVATE on purpose. Several of those copies walked the
// vector inline rather than calling their own notifyDownstream() — which is how
// twelve copies of one idea drifted into four spellings of it. Everything that
// wants to reach downstream goes through notifyDownstream().
//
// What a derived class may still override, and the only reasons known today:
//
//   • TweakableComputeNode::invalidate() — absorbs while frozen; the tweaked
//     value does not depend on inputs.
//   • RateLimiterNode::invalidate()      — absorbs, and re-notifies from inside
//     eval() only when the change clears its threshold.
//   • A clock-driven node (its output is a function of time, not only of its
//     inputs) — dirty() is always true, so the state_ guards below would swallow
//     every invalidation; it forwards unconditionally instead.
//
// Anything else overriding these is a smell: it is re-implementing the protocol
// rather than using it.
// ─────────────────────────────────────────────────────────────────────────────
class NodeBase : public INode {
public:
    /// True when this node may need re-evaluating — Maybe or Dirty. Cleared by
    /// eval(). The engine's pre-eval dirty snapshot reads this and is unaffected
    /// by the split.
    bool dirty() const override { return state_ != Dirtiness::Clean; }

    /// Which of the two invalidation behaviours this node was built with.
    InvalidationMode invalidationMode() const noexcept { return mode_; }

    // invalidate() and invalidateMaybe() are FINAL. A node that needs different
    // behaviour overrides propagate() — see the note below for why.
    void invalidate()      final { propagate(Dirtiness::Dirty); }
    void invalidateMaybe() final { propagate(Dirtiness::Maybe); }

    void addDownstream(std::weak_ptr<INode> n) override {
        downstream_.push_back(std::move(n));
    }

protected:
    explicit NodeBase(InvalidationMode mode = InvalidationMode::Eager) noexcept
        : mode_(mode) {}

    // ── The whole invalidation protocol, in one virtual ──────────────────────
    //
    // THE ONE OVERRIDE POINT, and the reason it is one rather than two: a node
    // that overrode invalidate() alone would keep its behaviour on the direct
    // hop and silently lose it on the transitive cascade, which is the path that
    // carries almost every invalidation in a real graph. That failure compiles,
    // breaks no test, and is invisible. Overriding propagate() cannot do it,
    // because both entry points come through here.
    //
    // The three kinds of override, and what each does with `incoming`:
    //   TweakableComputeNode  — ignores it entirely while frozen (a tweaked
    //                           value does not depend on its inputs).
    //   ts::RateLimiterNode   — marks self, never cascades; it re-notifies from
    //                           inside eval() only when the change clears its
    //                           threshold.
    //   a clock-driven node   — always cascades; its output is a function of a
    //                           clock, so it has no clean state for the guards
    //                           below to key off.
    virtual void propagate(Dirtiness incoming) {
        if (incoming == Dirtiness::Maybe) {
            // Same visited-guard as the old two-state cascade: a diamond's lower
            // half is walked once, not once per path.
            if (state_ != Dirtiness::Clean) return;
            state_ = Dirtiness::Maybe;
        } else {
            if (state_ == Dirtiness::Dirty) return;
            const bool wasClean = (state_ == Dirtiness::Clean);
            state_ = Dirtiness::Dirty;
            // Already Maybe: the cascade below has been through, and walking it
            // again would only re-set what it already set.
            if (!wasClean) return;
        }
        for (auto& w : downstream_) if (auto n = w.lock()) n->invalidateMaybe();
    }

    /// Invalidate every downstream node — "my value changed". Expired weak_ptrs
    /// are skipped, not erased: a node's downstream set is built once at wire()
    /// time and the engine's graphs are not rewired at runtime.
    void notifyDownstream() {
        for (auto& w : downstream_) if (auto n = w.lock()) n->invalidate();
    }

    // ── The resolve step ─────────────────────────────────────────────────────
    //
    // Called by eval() AFTER it has pulled every input and BEFORE it invokes the
    // functor. That order is the whole mechanism: pulling an input whose value
    // really changed makes that input call OUR invalidate() from inside its own
    // eval(), which upgrades us from Maybe to Dirty. So if we are still merely
    // Maybe once every input has been pulled, nothing we depend on moved, and a
    // pure functor would return exactly what cached_ already holds.
    //
    // Nothing is stored to make this work — no per-input ValuePtr is retained.
    // Retaining one would pin a ValueSlot buffer and push the producer back into
    // allocating, and a raw const IValue* is unsound because
    // ValueSlot rewrites buffers in place, so a raw address can compare equal
    // across a genuine change.
    //
    // For an Eager node this is one enum compare that always fails: the mechanism
    // costs nothing for nodes that stay Eager.
    bool skipRecompute(const EvalContext& ctx) const noexcept {
        return mode_ == InvalidationMode::Lazy
            && state_ != Dirtiness::Dirty
            && !ctx.forceRecompute;
    }

    /// eval() calls this when it is up to date again.
    void markClean() noexcept { state_ = Dirtiness::Clean; }
    /// Mark self dirty WITHOUT cascading — for sources staging a new value
    /// (they cascade separately via notifyDownstream()) and for the absorbing
    /// overrides above.
    void markDirty() noexcept { state_ = Dirtiness::Dirty; }

    Dirtiness state() const noexcept { return state_; }

private:
    Dirtiness state_ = Dirtiness::Dirty;
    /// const: chosen once, at construction, by the code that wrote the functor.
    const InvalidationMode mode_;
    std::vector<std::weak_ptr<INode>> downstream_;
};
// ─────────────────────────────────────────────────────────────────────────────
// wire() — called inside every make() after the shared_ptr is constructed.
//
// The reason this can't happen in the constructor is that shared_from_this()
// isn't valid until the object is owned by a shared_ptr. make() constructs
// the shared_ptr first, then immediately calls wire() before returning it,
// so callers never need to think about this.
// ─────────────────────────────────────────────────────────────────────────────
void wire(const NodePtr& self, const std::vector<NodePtr>& ins);

// ─────────────────────────────────────────────────────────────────────────────
// Input<T> — leaf node.
//
// An optional wake hook can be installed via setWakeHook(). When set, it is
// called at the end of set() whenever the value actually changes — the engine
// uses this to wake its event loop without polling.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class Input : public NodeBase, public std::enable_shared_from_this<Input<T>> {
public:
    static std::shared_ptr<Input<T>> make(
        std::string name, T initial = T{},
        EqualityPolicyPtr eq = nullptr);

    void set(T newVal);
    const T& get() const;

    /// Install the engine's wake hook. Called once at registration time.
    void setWakeHook(std::function<void()> hook);

    ValuePtr eval(EvalContext&) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::Input; }

private:
    Input(std::string n, T initial, EqualityPolicyPtr eq);

    std::string name_;
    ValuePtr value_;
    EqualityPolicyPtr eq_;
    std::function<void()> wakeHook_;
};

template<typename T>
using InputPtr = std::shared_ptr<Input<T>>;

// ─────────────────────────────────────────────────────────────────────────────
// ComputeNode<Out, Ins...> — typed node with a functor over typed inputs.
// ─────────────────────────────────────────────────────────────────────────────
template<typename Out, typename... Ins>
class ComputeNode : public NodeBase, public std::enable_shared_from_this<ComputeNode<Out, Ins...>> {
public:
    using Fn = std::function<Out(const Ins&...)>;
    using InputTuple = std::tuple<std::conditional_t<true, NodePtr, Ins>...>;

    // Three make() overloads rather than one with a fifth defaulted parameter.
    // A fifth parameter would force `nullptr` into the equality-policy slot at
    // every opt-in site — make(name, ins, fn, nullptr, InvalidationMode::Lazy) —
    // which reads as noise at exactly the place the reader most wants to see the
    // choice. The overloads are unambiguous because EqualityPolicyPtr is a
    // shared_ptr and InvalidationMode is an enum class.
    //
    // The mode is set HERE, at construction, and nowhere else: whether the
    // functor is a pure function of its declared inputs is a property of the
    // functor, so it belongs next to the code that writes it. There is no
    // post-construction setter for the same reason: a graph site that could mark
    // a node Lazy would be vouching for a functor it cannot see.
    static std::shared_ptr<ComputeNode> make(
        std::string name, InputTuple inNodes, Fn fn,
        EqualityPolicyPtr eq = nullptr);
    static std::shared_ptr<ComputeNode> make(
        std::string name, InputTuple inNodes, Fn fn, InvalidationMode mode);
    static std::shared_ptr<ComputeNode> make(
        std::string name, InputTuple inNodes, Fn fn,
        EqualityPolicyPtr eq, InvalidationMode mode);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::Compute; }

private:
    ComputeNode(std::string name, InputTuple ins, Fn fn, EqualityPolicyPtr eq,
                InvalidationMode mode);

    /// Pull every input, resolve, and — unless the resolve says to skip — invoke
    /// fn_ and publish the result. Returns what eval() returns. See the
    /// definition in dag.inl for why cheap and expensive input types take
    /// different paths, and why the resolve check has to live in here rather
    /// than in eval().
    template<std::size_t... Is>
    ValuePtr applyInputs(EvalContext& ctx, std::index_sequence<Is...>);
    /// Emit, compare, notify, mark clean — the tail of every evaluation.
    ValuePtr publish(Out&& result);
    template<std::size_t... Is>
    std::vector<NodePtr> collectInputs(std::index_sequence<Is...>) const;

    std::string name_;
    InputTuple inputs_;
    Fn fn_;
    EqualityPolicyPtr eq_;
    ValuePtr cached_;
    ValueSlot<Out> slot_;
};

template<typename Out, typename... Ins>
using ComputeNodePtr = std::shared_ptr<ComputeNode<Out, Ins...>>;

// ─────────────────────────────────────────────────────────────────────────────
// InPlaceComputeNode<Out, Ins...> — a ComputeNode for an Out that owns heap.
//
// The problem it solves. ComputeNode's Fn returns Out BY VALUE, so
// every evaluation builds a fresh one and hands it over as an rvalue:
//
//     Out result = applyInputs(...);          // allocates, for a heap-owning Out
//     auto newV  = slot_.emit(std::move(result));   // move-assign FREES the
//                                                   // buffer ValueSlot was
//                                                   // recycling, and steals this
//                                                   // one instead
//
// So a node whose output is a std::vector or a struct of them allocates on
// every cycle no matter how warm its slot is, and the recycler's whole purpose
// is defeated at the last step. That is the pathology AsyncInput::post/flush
// avoids on the feed thread, reappearing on the eval thread, and it is
// unreachable from ComputeNode because ValueSlot's
// copy-assigning emit(const T&) overload needs an lvalue the caller keeps.
//
// This class supplies that lvalue: a retained `scratch_` the functor writes
// into. Capacity then cycles between scratch_ and the two slot buffers exactly
// as it cycles between pending_, staged_ and the slot buffers on the ingest
// path, and the steady state allocates nothing.
//
// THE CONTRACT, AND IT IS SHARP: `out` arrives holding the PREVIOUS
// evaluation's value, not a default-constructed one. The functor must overwrite
// everything it owns — clear() before push_back, assign every field — on EVERY
// path including early returns. A functor that appends without clearing
// produces a monotonically growing container: plausible-looking output, no
// crash, and nothing catches it but a test that evaluates twice. That is the
// price of recycling capacity; a node that reset the scratch for you could not
// recycle anything.
//
// Everything else is ComputeNode: same lazy dirty/cached protocol, same
// equality policy, same NodeKind::Compute, same reference-binding rule for
// expensive input types. It is a sibling rather than a second make() on
// ComputeNode because the alternative puts a runtime branch and an extra Out
// member on the hottest class in the engine — every instantiation of it — to
// serve the handful of nodes whose output owns heap.
// ─────────────────────────────────────────────────────────────────────────────
template<typename Out, typename... Ins>
class InPlaceComputeNode
    : public NodeBase,
      public std::enable_shared_from_this<InPlaceComputeNode<Out, Ins...>> {
public:
    /// Writes this cycle's value into `out` — see the contract above: `out`
    /// still holds the previous cycle's value on entry.
    using Fn = std::function<void(Out& out, const Ins&...)>;
    using InputTuple = std::tuple<std::conditional_t<true, NodePtr, Ins>...>;

    static std::shared_ptr<InPlaceComputeNode> make(
        std::string name, InputTuple inNodes, Fn fn,
        EqualityPolicyPtr eq = nullptr);
    static std::shared_ptr<InPlaceComputeNode> make(
        std::string name, InputTuple inNodes, Fn fn, InvalidationMode mode);
    static std::shared_ptr<InPlaceComputeNode> make(
        std::string name, InputTuple inNodes, Fn fn,
        EqualityPolicyPtr eq, InvalidationMode mode);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::Compute; }

private:
    InPlaceComputeNode(std::string name, InputTuple ins, Fn fn,
                       EqualityPolicyPtr eq, InvalidationMode mode);

    /// Pull, resolve, invoke fn_ into scratch_, publish. Same two paths as
    /// ComputeNode::applyInputs, chosen the same way and for the same reasons.
    template<std::size_t... Is>
    ValuePtr applyInputs(EvalContext& ctx, std::index_sequence<Is...>);
    ValuePtr publish();
    template<std::size_t... Is>
    std::vector<NodePtr> collectInputs(std::index_sequence<Is...>) const;

    std::string name_;
    InputTuple inputs_;
    Fn fn_;
    EqualityPolicyPtr eq_;
    ValuePtr cached_;
    /// The retained output buffer. Its capacity is the whole point of the class.
    Out scratch_{};
    ValueSlot<Out> slot_;
};

template<typename Out, typename... Ins>
using InPlaceComputeNodePtr = std::shared_ptr<InPlaceComputeNode<Out, Ins...>>;

// ─────────────────────────────────────────────────────────────────────────────
// ITweakable — interface for nodes that support value freezing.
//
// A tweaked node:
//   • Returns a fixed value from eval() — its functor is never called.
//   • Silently absorbs invalidate() from upstream — inputs are irrelevant.
//   • Immediately notifies downstream when the tweak value changes.
//   • Delivers a changed tweak value to its own engine output, once, on the
//     engine's next cycle. tweak() leaves the node dirty until it is evaluated.
//   • Treats an equal tweak as no change. The published value keeps its
//     identity, so nothing downstream or at the output sees anything.
//
// Clearing a tweak:
//   • Marks the node dirty and its consumers Maybe. Whether its value changes
//     is known only once it recomputes: its eval() then tells them Dirty if the
//     value moved, and a Lazy consumer skips if it did not.
//   • On the next eval() the functor runs normally from current inputs.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class ITweakable {
public:
    virtual ~ITweakable() = default;

    /// Freeze this node at `val`. Downstream is notified immediately (respecting
    /// the node's equality policy). Upstream invalidations are absorbed until
    /// clearTweak() is called. A `val` that differs from the published value
    /// also leaves the node dirty, so that an engine evaluates it on its next
    /// cycle and delivers `val` to this node's own output. An equal `val` only
    /// freezes.
    virtual void tweak(T val) = 0;

    /// Remove the freeze. Marks the node dirty and its consumers Maybe, so they
    /// re-pull on the next eval pass and learn there whether the value changed.
    /// Normal computation resumes.
    virtual void clearTweak() = 0;

    /// True while a tweak is active.
    virtual bool isTweaked() const = 0;

    /// The current tweak value, or std::nullopt if not tweaked.
    virtual std::optional<T> tweakValue() const = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// TweakableComputeNode<Out, Ins...>
//
// Identical to ComputeNode but also implements ITweakable<Out>.
//
// State machine:
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │  NORMAL                        TWEAKED                       │
//   │  ──────                        ───────                       │
//   │  state_ tracks upstream        Clean, or Dirty from a new    │
//   │                                tweak until eval() runs       │
//   │  eval() runs functor           eval() returns frozen value   │
//   │  invalidate() propagates       invalidate() is absorbed      │
//   │                                                              │
//   │  ─── tweak(v) ──►  freeze; if v is new, markDirty() and      │
//   │                    notify downstream                         │
//   │  ◄── clearTweak() ─  self Dirty, downstream Maybe            │
//   └──────────────────────────────────────────────────────────────┘
// ─────────────────────────────────────────────────────────────────────────────
template<typename Out, typename... Ins>
class TweakableComputeNode
    : public NodeBase
    , public ITweakable<Out>
    , public std::enable_shared_from_this<TweakableComputeNode<Out, Ins...>>
{
public:
    using Fn = std::function<Out(const Ins&...)>;
    using InputTuple = std::tuple<std::conditional_t<true, NodePtr, Ins>...>;

    static std::shared_ptr<TweakableComputeNode> make(
        std::string name, InputTuple inNodes, Fn fn,
        EqualityPolicyPtr eq = nullptr);
    static std::shared_ptr<TweakableComputeNode> make(
        std::string name, InputTuple inNodes, Fn fn, InvalidationMode mode);
    static std::shared_ptr<TweakableComputeNode> make(
        std::string name, InputTuple inNodes, Fn fn,
        EqualityPolicyPtr eq, InvalidationMode mode);

    // ── INode ────────────────────────────────────────────────────────────────
    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::Compute; }

    // ── ITweakable<Out> ──────────────────────────────────────────────────────
    void tweak(Out val) override;
    void clearTweak() override;
    bool isTweaked() const override;
    std::optional<Out> tweakValue() const override;

private:
    TweakableComputeNode(std::string name, InputTuple ins, Fn fn,
                         EqualityPolicyPtr eq, InvalidationMode mode);

    /// When tweaked, upstream invalidation is silently absorbed — the frozen
    /// value is unaffected and downstream sees no change. Overriding propagate()
    /// rather than invalidate() is what makes that hold on the transitive
    /// cascade as well as the direct hop.
    void propagate(Dirtiness incoming) override;

    template<std::size_t... Is>
    ValuePtr applyInputs(EvalContext& ctx, std::index_sequence<Is...>);
    ValuePtr publish(Out&& result);
    template<std::size_t... Is>
    std::vector<NodePtr> collectInputs(std::index_sequence<Is...>) const;

    std::string name_;
    InputTuple inputs_;
    Fn fn_;
    EqualityPolicyPtr eq_;
    ValuePtr cached_;
    bool tweaked_ = false;
    ValueSlot<Out> slot_;
};

// ─────────────────────────────────────────────────────────────────────────────
// ConditionNode — selects between two branches at runtime.
// ─────────────────────────────────────────────────────────────────────────────
class ConditionNode : public NodeBase, public std::enable_shared_from_this<ConditionNode> {
public:
    static std::shared_ptr<ConditionNode> make(
        std::string name, NodePtr condition, NodePtr trueBranch, NodePtr falseBranch,
        EqualityPolicyPtr eq = nullptr);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::Compute; }

private:
    ConditionNode(std::string name, NodePtr cond, NodePtr tb, NodePtr fb,
                  EqualityPolicyPtr eq);

    std::string name_;
    NodePtr condition_, trueBranch_, falseBranch_;
    EqualityPolicyPtr eq_;
    ValuePtr cached_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Graph — keeps nodes alive; provides a unified eval entry point.
// ─────────────────────────────────────────────────────────────────────────────
class Graph {
public:
    void addNode(NodePtr node);
    ValuePtr eval(const NodePtr& root);
    ValuePtr eval(const NodePtr& root, EvalContext ctx);
private:
    std::vector<NodePtr> nodes_;
};

} // namespace dag

#include "dag.inl"
