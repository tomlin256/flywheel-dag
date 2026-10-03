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

    /// The stored type. Non-virtual: it is fixed at construction, so reading it
    /// is a plain load, not a vtable dispatch, on a path taken once per input per
    /// node per cycle.
    std::type_index type() const noexcept { return type_; }

protected:
    explicit IValue(std::type_index t) noexcept : type_(t) {}

private:
    std::type_index type_;
};

template<typename T>
class ValueSlot;

// final: TypedValue<T> is the only IValue implementation and nothing derives
// from it, so get_value() can use an exact type_index compare in place of a
// dynamic_cast. The two are equivalent only while this holds.
template<typename T>
class TypedValue final : public IValue {
public:
    explicit TypedValue(T v);
    const T& get() const;
private:
    // Only ValueSlot may rewrite a value in place, and only once it is the sole
    // owner of the buffer. A TypedValue is immutable to everyone else.
    friend class ValueSlot<T>;
    // Two overloads, not one by-value parameter, which would end in a
    // move-assign: that frees the storage value_ owns, the very buffer this
    // class exists to recycle, where copy-assigning from a const T& reuses it.
    // See ValueSlot's "Copy or move" note.
    void set(const T& v) { value_ = v; }
    void set(T&& v)      { value_ = std::move(v); }

    T value_;
};

using ValuePtr = std::shared_ptr<const IValue>;

template<typename T>
ValuePtr make_value(T v);

/// The value as a T. Throws std::runtime_error for a null ValuePtr and
/// std::bad_cast for a value of any other type.
template<typename T>
const T& get_value(const ValuePtr& v);

// ─────────────────────────────────────────────────────────────────────────────
// ValueSlot<T> — recycles two TypedValue<T> buffers so a node's steady-state
// eval allocates nothing.
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
//      holds a reference to the buffer it points at, so that buffer always
//      reads use_count() >= 2. The equality check (cached_ vs new) and the
//      engine's pointer-identity change detection both depend on those being
//      distinct objects.
//
// Two buffers is exactly enough: cached_ pins at most one, leaving the other
// free. When neither is free, the slot allocates a fresh buffer: always
// correct, just not free.
//
// Copy or move
// ────────────
// Recycling the TypedValue is only half the job for a T that owns heap. A
// move-assign into the buffer frees the very storage being recycled and takes
// the source's, so such a T allocates on every emit however warm the slot is.
//
// So there are two overloads, and a caller gets the one that fits what it has:
//
//   emit(const T&) — the caller keeps its value (AsyncInput::flush's staged_,
//     InPlaceComputeNode's scratch_). Copy-assigning reuses the buffer's
//     capacity, and the source keeps its own: nothing allocates.
//
//   emit(T&&) — the caller built the value for this emit alone (a ComputeNode's
//     result). Its storage is already paid for, so there is nothing to recycle
//     and a copy would be added work.
//
// Eval-thread only
// ────────────────
// use_count() is a sound sole-ownership test only when every copy of the
// pointer is made on one thread. That holds for compute, time-series and op
// nodes and for AsyncInput::flush(), which the engine drives on the eval
// thread. It does not hold for Input<T>::set(), which Engine::makeInput
// documents as callable from application code, so Input<T>::set() allocates
// (flywheel-dag#31 asks which thread may call it). Do not add a slot to
// anything reachable off the eval thread.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class ValueSlot {
public:
    /// Emit a value the caller keeps. Copy-assigns into the recycled buffer, so
    /// a T with heap members keeps that buffer's capacity.
    ValuePtr emit(const T& v) { return emitImpl(v); }

    /// Emit a value the caller is done with. Move-assigns.
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
        // Neither buffer is free. Replacing our handle on one is safe, since its
        // other owners keep it alive, and gives the slot a buffer to recycle
        // next time.
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
// The last two are the same node with different consumers, so this is a
// per-node judgement, not a rule. Held by
// DAGTests.EqualityPolicyOnIntermediateNodeDoesNotSuppressDownstreamEval and
// DAGTests.EqualityPolicyOnALazyIntermediateNodeDoesSuppressDownstreamEval,
// which run the same graph, Eager and Lazy, and assert opposite outcomes.
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
    /// True when the values are equal (no change). False publishes the new value
    /// and calls notifyDownstream(); the note above says what that gates.
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
// Dirtiness — what a node knows about its own freshness.
//
//   Dirty  an INPUT OF MINE definitely changed its value. Set by the one hop
//          from a node whose eval() saw its own value change.
//   Maybe  an ANCESTOR of mine may have changed. Set by the transitive cascade.
//          Whether this node actually has to recompute is not yet known, and is
//          resolved when it is pulled (NodeBase's resolve step).
//   Clean  up to date.
//
// The cascade marks only Maybe, so nothing past the one hop is committed to
// re-evaluating before a value exists to compare. That is what lets an equality
// policy on an intermediate node gate the work below it.
// ─────────────────────────────────────────────────────────────────────────────
enum class Dirtiness : std::uint8_t { Clean, Maybe, Dirty };

// ─────────────────────────────────────────────────────────────────────────────
// InvalidationMode — per-node, set at construction, never changed.
//
// Eager  recompute whenever anything upstream fired. The default wherever a
//        factory takes a mode. It is also the PERMANENT answer for any node whose
//        output is not a pure function of its declared inputs' VALUES — every
//        dag::ts stateful node is one (an EWMA fed a value equal to the last one
//        still has to tick, because its output depends on how many times it was
//        evaluated), as is any functor that reads state it did not declare as an
//        input.
//
// Lazy   recompute only when an input's value actually changed. Requires the
//        functor to be a pure function of its declared inputs.
//
// Which of the two a node wants is a property of its functor, and only the
// functor's author knows it. So the mode is set in the factory that WRITES the
// functor, with no setter: a setter would let a graph site vouch for the purity
// of a functor it cannot see.
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
    /// "An input of yours definitely changed." One hop; the transitive part is
    /// invalidateMaybe().
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
// `downstream_` is private: a derived class reaches it through
// notifyDownstream().
//
// A node is clean only when every input it read still is. The cascade's guard
// relies on it, and a node that pulls keeps it by bracketing each evaluation
// with beginEval() and endEval() — see "Clean only over clean inputs" below.
//
// A derived class overrides propagate() or dirty() only for these three reasons:
//
//   • TweakableComputeNode::propagate() ignores every invalidation while
//     frozen: a tweaked value does not depend on its inputs.
//   • ConditionNode's branch listeners' propagate() passes an invalidation on to
//     the node, as the same kind, only while the node takes that branch, and
//     drops it otherwise: the node's value does not depend on the other branch.
//   • A clock-driven node (its output is a function of time, not only of its
//     inputs) has dirty() always true, so the state_ guards below would swallow
//     every invalidation; it forwards unconditionally instead.
//
// Anything else overriding these is re-implementing the protocol rather than
// using it.
// ─────────────────────────────────────────────────────────────────────────────
class NodeBase : public INode {
public:
    /// True when this node may need re-evaluating: Maybe or Dirty. Cleared by
    /// eval(), unless an input it read went dirty again during it (endEval()).
    /// The engine's pre-eval dirty snapshot reads it.
    bool dirty() const override { return state_ != Dirtiness::Clean; }

    /// Which of the two invalidation behaviours this node was built with.
    InvalidationMode invalidationMode() const noexcept { return mode_; }

    // invalidate() and invalidateMaybe() are final: a node that needs different
    // behaviour overrides propagate().
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
    // hop and silently lose it on the transitive cascade, which carries almost
    // every invalidation in a real graph. Overriding propagate() cannot do that,
    // because both entry points come through here. The overrides that exist are
    // listed in the class comment.
    virtual void propagate(Dirtiness incoming) {
        if (incoming == Dirtiness::Maybe) {
            // Every "maybe" counts, the one the guard below drops included: see
            // "Clean only over clean inputs".
            heardMaybe_ = true;
            // Visited guard: a diamond's lower half is walked once, not once per
            // path.
            if (state_ != Dirtiness::Clean) return;
            state_ = Dirtiness::Maybe;
        } else {
            if (state_ == Dirtiness::Dirty) return;
            const bool wasClean = (state_ == Dirtiness::Clean);
            state_ = Dirtiness::Dirty;
            // Was Maybe, so its cascade has already run.
            if (!wasClean) return;
        }
        cascadeMaybe();
    }

    /// Invalidate every downstream node — "my value changed". Expired weak_ptrs
    /// are skipped, not erased: a node's downstream set is built once, by
    /// wire(), and graphs are not rewired at runtime.
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
    // allocating, and a raw const IValue* is unsound because ValueSlot rewrites
    // buffers in place, so a raw address can compare equal across a genuine
    // change.
    //
    // For an Eager node this is one enum compare that always fails, so the
    // mechanism costs nothing for nodes that stay Eager.
    bool skipRecompute(const EvalContext& ctx) const noexcept {
        return mode_ == InvalidationMode::Lazy
            && state_ != Dirtiness::Dirty
            && !ctx.forceRecompute;
    }

    // ── Clean only over clean inputs ─────────────────────────────────────────
    //
    // The cascade's guard stops at a node that is already dirty. That is safe
    // only while a clean node's inputs are clean, and an evaluation breaks it
    // when an input it has already read goes dirty again before it ends: an
    // always-dirty node, such as an application's clock-driven node, pulled
    // again through a later input, tells the earlier input's nodes. Marked
    // clean then, the node would never see a later change, which stops at the
    // dirty input.
    //
    // Such an input sends a "maybe" cascade, which reaches this node while it
    // is still evaluating. In an evaluation where nothing goes dirty again, the
    // inputs only go from dirty to clean, and one whose value moved says
    // "changed". So a "maybe" is the signal, and propagate() records every one.
    // beginEval() forgets those that came before the evaluation began, which a
    // diamond sends while the node waits to be pulled. A false alarm, from an
    // input not read yet or from one a later pull repaired, costs one more
    // evaluation and never a stale value.
    //
    // A node that pulls calls beginEval() before its first pull and endEval()
    // in place of markClean() wherever its evaluation ends, skips included. A
    // node of your own that pulls should do the same: one that calls
    // markClean() keeps working, and keeps the exposure.

    /// eval() calls this before its first pull.
    void beginEval() noexcept { heardMaybe_ = false; }

    /// eval() calls this once it is up to date with what it read. The node is
    /// clean, unless a "maybe" reached it since beginEval(). Then it keeps the
    /// state its evaluation left it in, and tells each consumer "maybe".
    ///
    /// It keeps its state rather than going Maybe because an input it read may
    /// have been evaluated again since, by a later input's pull, with a new
    /// value. That input's "changed" left this node Dirty, and a Lazy node sent
    /// to Maybe would skip its next evaluation and keep the old value. It tells
    /// its consumers for the one evaluating now, which has read this node and
    /// must not end clean over it either. The rest are dirty already.
    void endEval() {
        if (!heardMaybe_) { state_ = Dirtiness::Clean; return; }
        stayDirty();
    }

    /// A node that pulls nothing calls this when it is up to date again: a
    /// source, or a tweaked node returning its frozen value. A node that pulls
    /// calls endEval() instead.
    void markClean() noexcept { state_ = Dirtiness::Clean; }
    /// Mark self dirty WITHOUT cascading — for sources staging a new value and
    /// for tweak() publishing a frozen one. Both cascade separately via
    /// notifyDownstream().
    void markDirty() noexcept { state_ = Dirtiness::Dirty; }

    Dirtiness state() const noexcept { return state_; }

private:
    /// "An ancestor of yours may have changed", to every downstream node.
    void cascadeMaybe() {
        for (auto& w : downstream_) if (auto n = w.lock()) n->invalidateMaybe();
    }

    /// endEval()'s rare path, kept out of line and cold: it runs only when a
    /// "maybe" reached the node during its evaluation. GCC and Clang, the
    /// compilers CI builds with, both take the attributes.
    [[gnu::cold, gnu::noinline]] void stayDirty() { cascadeMaybe(); }

    Dirtiness state_ = Dirtiness::Dirty;
    /// const: chosen once, at construction, by the code that wrote the functor.
    const InvalidationMode mode_;
    /// A "maybe" reached this node since its last beginEval().
    bool heardMaybe_ = false;
    std::vector<std::weak_ptr<INode>> downstream_;
};
// ─────────────────────────────────────────────────────────────────────────────
// wire() — registers `self` as a downstream of each of `ins`.
//
// A make() with inputs calls it once the node is owned by a shared_ptr, which
// the constructor cannot supply. ConditionNode::make wires its branches by
// hand, through listeners.
// ─────────────────────────────────────────────────────────────────────────────
void wire(const NodePtr& self, const std::vector<NodePtr>& ins);

// ─────────────────────────────────────────────────────────────────────────────
// Input<T> — leaf node.
//
// An optional wake hook, installed with setWakeHook(), is called at the end of
// set() whenever the value changes. The engine uses it to wake its event loop
// without polling.
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
    // shared_ptr and InvalidationMode is an enum class. The mode is fixed at
    // construction: see InvalidationMode.
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
    /// definition in dag.inl for why trivially copyable and other input types
    /// take different paths, and why the resolve check lives here rather than in
    /// eval().
    template<std::size_t... Is>
    ValuePtr applyInputs(EvalContext& ctx, std::index_sequence<Is...>);
    /// Emit, compare, notify, endEval() — the tail of every evaluation.
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
// ComputeNode's Fn returns Out BY VALUE, and publish() emits it with
// slot_.emit(std::move(result)). For a heap-owning Out that move-assign FREES the
// buffer ValueSlot was recycling and takes the temporary's instead, so a node
// whose output is a std::vector or a struct of them allocates on every cycle
// however warm its slot is. ValueSlot's copy-assigning emit(const T&) avoids
// that, but needs an lvalue the caller keeps, which ComputeNode, building a
// fresh Out each time, does not have.
//
// This class supplies that lvalue: a retained `scratch_` the functor writes
// into. Capacity then cycles between scratch_ and the two slot buffers as it
// does between pending_, staged_ and the slot buffers on AsyncInput's ingest
// path, and the steady state allocates nothing.
//
// THE CONTRACT: `out` arrives holding the PREVIOUS evaluation's value, not a
// default-constructed one. The functor must overwrite everything it owns —
// clear() before push_back, assign every field — on EVERY path including early
// returns. A functor that appends without clearing grows without bound: the
// output looks plausible, nothing crashes, and only a test that evaluates twice
// catches it. That is the price of recycling capacity; a node that reset the
// scratch for you could not recycle anything.
//
// Everything else is ComputeNode: the same invalidation protocol, equality
// policy and NodeKind::Compute, and the same reference-binding rule for input
// types that are not trivially copyable. It is a sibling rather than a second
// make() on ComputeNode because the alternative puts a runtime branch and an
// extra Out member on the hottest class in the engine, in every instantiation,
// to serve the few nodes whose output owns heap.
// ─────────────────────────────────────────────────────────────────────────────
template<typename Out, typename... Ins>
class InPlaceComputeNode
    : public NodeBase,
      public std::enable_shared_from_this<InPlaceComputeNode<Out, Ins...>> {
public:
    /// Writes this cycle's value into `out`. See the contract above: `out` holds
    /// the previous cycle's value on entry.
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

    /// Pull, resolve, invoke fn_ into scratch_, publish. The same two paths as
    /// ComputeNode::applyInputs, chosen the same way.
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

namespace aad {

// ─────────────────────────────────────────────────────────────────────────────
// Partials — what IDifferentiable::partials() writes.
//
// One entry per input that the node's value depends on at the current point:
// the input's position in inputs(), and ∂value/∂input. An input the value does
// not depend on here gets no entry. That absence is what keeps a pass away from
// the branch a ConditionNode did not take, and from the inputs of a tweaked
// node: a pass follows only the inputs a node names.
//
// A node may name one input twice, as a DiffNode of x and x does. The entries
// add up.
// ─────────────────────────────────────────────────────────────────────────────
class Partials {
public:
    struct Entry {
        std::size_t input;   ///< position in the node's inputs()
        double      d;       ///< ∂value/∂inputs()[input]
    };

    void add(std::size_t input, double d);
    const std::vector<Entry>& entries() const noexcept;
    /// Empties the list and keeps its capacity, so a pass reuses one Partials
    /// for every node it asks.
    void clear() noexcept;

private:
    std::vector<Entry> entries_;
};

// ─────────────────────────────────────────────────────────────────────────────
// IDifferentiable — a node that can say how its value moves with its inputs.
//
// A mixin, like ITweakable and IStatefulNode. A pass finds it with
// dynamic_cast, as Engine::discoverStatefulNodes() finds IStatefulNode, so
// INode and NodeBase do not change, and neither does any node that does not
// implement it. A node with inputs that does not implement it is a barrier: a
// pass cannot follow its inputs.
//
// partials() writes the node's local partial derivatives at its inputs'
// CURRENT values. It pulls its inputs with eval(ctx), as eval() does. A pass
// calls it only on a clean node, whose inputs are then clean too, so every pull
// returns a cached value and nothing is evaluated.
//
// The partials come from the inputs alone, never from the node's own published
// value. Under a tolerance policy the two can differ, because the published
// value is the last one the policy let through.
//
// Returns false when the node cannot say, which makes it a barrier. An op over
// a type other than double returns false, and so does an untweaked
// TweakableComputeNode, whose functor is opaque.
// ─────────────────────────────────────────────────────────────────────────────
class IDifferentiable {
public:
    virtual ~IDifferentiable() = default;
    virtual bool partials(EvalContext& ctx, Partials& out) = 0;
};

} // namespace aad

// ─────────────────────────────────────────────────────────────────────────────
// ITweakable — interface for nodes that support value freezing.
//
// A tweaked node:
//   • Returns a fixed value from eval() — its functor is never called.
//   • Absorbs every upstream invalidation — inputs are irrelevant.
//   • Notifies downstream immediately when the tweak value changes.
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

    /// Freeze this node at `val`. Upstream invalidations are absorbed until
    /// clearTweak(). A `val` that differs from the published value, by the
    /// node's equality policy, is published as described above; an equal one
    /// only freezes.
    virtual void tweak(T val) = 0;

    /// Remove the freeze, as described above. Normal computation resumes.
    virtual void clearTweak() = 0;

    /// True while a tweak is active.
    virtual bool isTweaked() const = 0;

    /// The current tweak value, or std::nullopt if not tweaked.
    virtual std::optional<T> tweakValue() const = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// TweakableComputeNode<Out, Ins...>
//
// A ComputeNode that also implements ITweakable<Out> and aad::IDifferentiable.
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
    , public aad::IDifferentiable
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

    // ── aad::IDifferentiable ─────────────────────────────────────────────────
    /// While tweaked, true with no partials: the frozen value is a constant.
    /// Otherwise false: the functor is opaque.
    bool partials(EvalContext& ctx, aad::Partials& out) override;

private:
    TweakableComputeNode(std::string name, InputTuple ins, Fn fn,
                         EqualityPolicyPtr eq, InvalidationMode mode);

    /// While tweaked, absorbs every upstream invalidation: the frozen value does
    /// not depend on the inputs, so downstream sees no change.
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
//
// It pulls the condition and the branch the condition selects, never the other
// branch, so a node on the other branch can be stale. A switch to it never
// serves that staleness, because the branch is pulled before its value is used.
// Held by LazyInvalidation.Case6_ConditionNodeNeverServesAStaleUntakenBranch.
//
// WHAT IT HEARS. The condition, and the branch its last eval() took. Each
// branch is wired to a listener of its own rather than to the node, and the
// listener passes an invalidation on only while its branch is the one taken.
// The node's value does not depend on the other branch, and a switch to it
// arrives through the condition, which the node always hears.
//
// Hearing the branch not taken would let a move there make this node and its
// consumers dirty again after they were evaluated: they would recompute for
// nothing, and a root left dirty cannot be recorded on a tape. Held by
// UntakenBranch.ItsMoveReachesNoConsumer.
// ─────────────────────────────────────────────────────────────────────────────
class ConditionNode
    : public NodeBase
    , public aad::IDifferentiable
    , public std::enable_shared_from_this<ConditionNode>
{
public:
    static std::shared_ptr<ConditionNode> make(
        std::string name, NodePtr condition, NodePtr trueBranch, NodePtr falseBranch,
        EqualityPolicyPtr eq = nullptr);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::Compute; }

    /// 1 for the branch the condition selects. Never names the condition or the
    /// other branch, so a pass never reads the branch eval() did not take.
    bool partials(EvalContext& ctx, aad::Partials& out) override;

private:
    ConditionNode(std::string name, NodePtr cond, NodePtr tb, NodePtr fb,
                  EqualityPolicyPtr eq);

    // Hears one branch for the node. Not a node: nothing names a listener as
    // an input, so nothing pulls one, and its eval() throws.
    class BranchListener : public NodeBase {
    public:
        BranchListener(std::weak_ptr<ConditionNode> owner, bool branch);

        ValuePtr eval(EvalContext&) override;
        std::string name() const override;
        std::vector<NodePtr> inputs() const override;
        NodeKind kind() const override { return NodeKind::Compute; }

    private:
        /// Passes the invalidation on to the node, as the same kind, while the
        /// node takes this listener's branch. Drops it otherwise.
        void propagate(Dirtiness incoming) override;

        /// Weak: the node owns its listeners.
        std::weak_ptr<ConditionNode> owner_;
        const bool branch_;
    };

    std::string name_;
    NodePtr condition_, trueBranch_, falseBranch_;
    EqualityPolicyPtr eq_;
    ValuePtr cached_;
    /// The branch eval() last pulled. Empty until the first eval(), so the
    /// listeners pass nothing on before it: the node is dirty until then anyway.
    std::optional<bool> taken_;
    std::shared_ptr<BranchListener> onTrue_, onFalse_;
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
