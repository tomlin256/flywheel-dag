// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_timeseries.hpp — time-series processing nodes built on dag.hpp
//
// Design principles
// ─────────────────
//  • Each node stores only the state needed for its O(1) incremental update.
//  • The DAG's lazy invalidation means un-observed branches never compute.
//  • IEqualityPolicy on each node's output controls downstream propagation.
//
// Node catalogue
// ──────────────
//  WindowNode<T>       — keeps the last N values as a std::deque<T>
//  RollingStats        — single node: incremental mean + stddev (Welford)
//                        exposes mean(), stddev(), variance() directly
//  RollingMinMaxNode   — sliding min/max via monotonic deque, O(1) amort.
//                        output: std::pair<double,double> {min, max}
//  EWMANode            — exponential weighted moving average (alpha)
//  DeltaNode<T>        — first difference (value − previous)
//  DelayNode<T>        — N-tick ring buffer; output is value from N ticks ago
//  ThresholdNode<T>    — bool: value crosses a level, optional hysteresis
//  ZScoreNode          — (x − μ) / σ  using a RollingStats internally
//  OutlierGateNode     — passes raw value if |z| < threshold, else mean
//  RateLimiterNode<T>  — suppresses downstream if |Δ| < minDelta
//  DebounceCountNode       — bool: only true after N consecutive true ticks
//  LatchedDebounceNode     — std::optional<bool>: edge-triggered onset/resolved with debounce

#include "dag.hpp"
#include "dag_ring_buffer.hpp"
#include "dag_state_store.hpp"
#include "dag_window_status.hpp"
#include <deque>
#include <optional>
#include <cmath>
#include <cassert>
#include <cstdint>

namespace dag::ts {

// ─────────────────────────────────────────────────────────────────────────────
// Internal CRTP helper — provides the dirty/downstream boilerplate.
// Concrete node: inherit NodeImpl<Self>, implement compute(EvalContext&).
// ─────────────────────────────────────────────────────────────────────────────
template<typename Derived>
class NodeImpl : public NodeBase, public std::enable_shared_from_this<Derived> {
public:
    NodeKind kind() const override { return NodeKind::TimeSeries; }

protected:
    /// Publish this evaluation's value: rebind cached_ and invalidate downstream
    /// if the policy says it changed, then endEval(). The no-argument
    /// NodeBase::notifyDownstream() it builds on stays reachable by that name.
    using NodeBase::notifyDownstream;
    void notifyDownstream(const ValuePtr& newV, EqualityPolicyPtr& eq);

    ValuePtr cached_;
};

// ─────────────────────────────────────────────────────────────────────────────
// StatefulNodeBase<Derived, Out, In, State>
//
// Eliminates per-node boilerplate: name, upstream, equality policy, dirty flag,
// cache, downstream wiring, and the IStatefulNode mixin.
//
// Derived must implement three CRTP methods (enforced via static_assert):
//   Out  doCompute(const In& input, State& state)
//   void doSaveState(INodeState& s, const State& state) const
//   void doRestoreState(const INodeState& s, State& state)
// ─────────────────────────────────────────────────────────────────────────────
template<typename Derived, typename Out, typename In, typename State>
class StatefulNodeBase : public NodeImpl<Derived>, public IStatefulNode {
public:
    std::string           name()   const override;
    std::vector<NodePtr>  inputs() const override;
    ValuePtr              eval(EvalContext& ctx) override;
    void saveState(INodeState& s)        const override;
    void restoreState(const INodeState& s)     override;

protected:
    StatefulNodeBase(std::string name, NodePtr upstream,
                     EqualityPolicyPtr eq = nullptr);

    State   state_{};
    NodePtr upstream_;
    EqualityPolicyPtr eq_;
    ValueSlot<Out> slot_;

private:
    std::string name_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Per-node State structs (defined before class bodies — required because the
// class template parameter list cannot reference an incomplete nested type).
// Each node's class body aliases its State with `using State = ...;`.
// ─────────────────────────────────────────────────────────────────────────────

struct EWMANodeState {
    double ewma        = 0.0;
    bool   initialized = false;
};

struct EWMATickRateNodeState {
    double rate = 0.0;
};

template<typename T>
struct DeltaNodeState {
    T    prev{};
    bool hasPrev = false;
};

template<typename T>
struct ThresholdNodeState {
    bool triggered = false;
};

struct DebounceCountNodeState {
    std::size_t count = 0;
};

struct LatchedDebounceNodeState {
    bool        latched = false;
    std::size_t count   = 0;
};

template<typename T>
struct RateLimiterNodeState {
    T    lastEmitted{};
    bool hasEmitted = false;
};

template<typename T>
struct DelayNodeState {
    RingBuffer<T> buf;
    bool          warm = false;
};

template<typename T>
struct WindowNodeState {
    std::deque<T> buf;
};

struct RollingMinMaxNodeEntry { double val; std::size_t idx; };

struct RollingMinMaxNodeState {
    RingBuffer<RollingMinMaxNodeEntry> minDeq, maxDeq;
    std::size_t tick = 0;
};

struct RollingSumNodeState {
    RingBuffer<double> buf;
    double      sum             = 0.0;
    std::size_t sinceRecompute  = 0;
};

struct RollingStatsState {
    RingBuffer<double> buf;
    double mean       = 0.0;
    double M2         = 0.0;
    double prevMean   = 0.0;
    double prevStddev = 0.0;
    std::size_t count = 0;
};

struct ZScoreNodeState {};

struct OutlierGateNodeState {
    bool lastWasOutlier = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// WindowNode<T>
// Keeps the last `capacity` values. Output: std::deque<T>.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class WindowNode
    : public StatefulNodeBase<WindowNode<T>, std::deque<T>, T, WindowNodeState<T>>,
      public IWindowed {
public:
    using State = WindowNodeState<T>;

    static std::shared_ptr<WindowNode<T>> make(
        std::string name, NodePtr upstream, std::size_t capacity);

    const std::deque<T>& window() const;
    bool full() const;

    // IWindowed
    std::size_t capacity() const noexcept override { return cap_; }
    std::size_t filled()   const noexcept override { return this->state_.buf.size(); }
    NodePtr windowStatusNode() const override;

    std::deque<T> doCompute(const T& x, State& s);
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    WindowNode(std::string n, NodePtr up, std::size_t cap);
    std::size_t cap_;
    mutable std::weak_ptr<INode> windowStatusNode_;
};

// ─────────────────────────────────────────────────────────────────────────────
// RollingStats  —  single node: incremental mean + variance (Welford online).
// Output: double (the mean — use stddev()/variance() for the rest).
// Sharing one node avoids duplicating the O(1) state for ZScore etc.
// ─────────────────────────────────────────────────────────────────────────────
class RollingStats
    : public StatefulNodeBase<RollingStats, double, double, RollingStatsState>,
      public IWindowed {
public:
    using State = RollingStatsState;

    static std::shared_ptr<RollingStats> make(
        std::string name, NodePtr upstream, std::size_t window,
        EqualityPolicyPtr eq = nullptr);

    // Accessors — valid after eval()
    double      mean()     const;
    double      variance() const;
    double      stddev()   const;
    std::size_t count()    const;
    /// Stats computed BEFORE the current sample was pushed — for outlier detection.
    double prevMean()   const;
    double prevStddev() const;

    // IWindowed
    std::size_t capacity() const noexcept override { return window_; }
    std::size_t filled()   const noexcept override { return state_.count; }
    NodePtr windowStatusNode() const override;

    double doCompute(const double& x, State& s);
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    RollingStats(std::string n, NodePtr up, std::size_t w,
                 EqualityPolicyPtr eq);
    void push(double x, State& s);

    std::size_t window_;
    /// Weak ref to avoid strong cycle: the companion node holds *this strongly
    /// via inputs_ AND the lambda capture. Downstream consumers keep it alive.
    mutable std::weak_ptr<INode> windowStatusNode_;
};

using RollingStatsPtr = std::shared_ptr<RollingStats>;

// ─────────────────────────────────────────────────────────────────────────────
// RollingSumNode — sum of the last `window` values, maintained incrementally.
// Output: double
//
// Replaces the WindowNode + fold idiom, which re-summed the whole window every
// tick after copying it twice (once out of the node, once into the functor's
// argument tuple). This holds one running total and adjusts it by the value
// entering and the value leaving: O(1) per tick, no copies.
//
// Drift control: an add/subtract running total accumulates floating-point error
// without bound, and this node runs for days. Every `window` pushes it re-sums
// the buffer exactly, which caps the error at `window` incremental steps and
// costs O(1) amortised — the re-sum is a contiguous scan, not a deque walk.
// ─────────────────────────────────────────────────────────────────────────────
class RollingSumNode
    : public StatefulNodeBase<RollingSumNode, double, double, RollingSumNodeState>,
      public IWindowed {
public:
    using State = RollingSumNodeState;

    static std::shared_ptr<RollingSumNode> make(
        std::string name, NodePtr upstream, std::size_t window,
        EqualityPolicyPtr eq = nullptr);

    double sum() const { return state_.sum; }

    // IWindowed
    std::size_t capacity() const noexcept override { return window_; }
    std::size_t filled()   const noexcept override { return state_.buf.size(); }
    NodePtr windowStatusNode() const override;

    double doCompute(const double& x, State& s);
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    RollingSumNode(std::string n, NodePtr up, std::size_t w, EqualityPolicyPtr eq);
    /// Re-sum the buffer exactly, discarding accumulated incremental error.
    static void recompute(State& s);

    std::size_t window_;
    mutable std::weak_ptr<INode> windowStatusNode_;
};

using RollingSumNodePtr = std::shared_ptr<RollingSumNode>;

// ─────────────────────────────────────────────────────────────────────────────
// RollingMinMaxNode
// Sliding min and max via two monotonic deques, O(1) amortized.
// Output: std::pair<double,double>  {min, max}
// ─────────────────────────────────────────────────────────────────────────────
class RollingMinMaxNode
    : public StatefulNodeBase<RollingMinMaxNode,
                               std::pair<double,double>, double,
                               RollingMinMaxNodeState>,
      public IWindowed {
public:
    using State = RollingMinMaxNodeState;
    using Entry = RollingMinMaxNodeEntry;

    static std::shared_ptr<RollingMinMaxNode> make(
        std::string name, NodePtr upstream, std::size_t window);

    // IWindowed — filled = min(tick, window) because each push advances tick_
    std::size_t capacity() const noexcept override { return window_; }
    std::size_t filled()   const noexcept override {
        return state_.tick < window_ ? state_.tick : window_;
    }
    NodePtr windowStatusNode() const override;

    std::pair<double,double> doCompute(const double& x, State& s);
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    RollingMinMaxNode(std::string n, NodePtr up, std::size_t w);
    void push(double x, State& s);

    std::size_t window_;
    mutable std::weak_ptr<INode> windowStatusNode_;
};

// ─────────────────────────────────────────────────────────────────────────────
// EWMANode — Exponential Weighted Moving Average
// alpha in (0,1]: weight of the newest sample (higher = more reactive).
// Output: double
// ─────────────────────────────────────────────────────────────────────────────
class EWMANode
    : public StatefulNodeBase<EWMANode, double, double, EWMANodeState> {
public:
    using State = EWMANodeState;

    static std::shared_ptr<EWMANode> make(
        std::string name, NodePtr upstream, double alpha,
        EqualityPolicyPtr eq = nullptr);

    bool isInitialized() const { return state_.initialized; }

    double doCompute(const double& x, State& s);
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    EWMANode(std::string n, NodePtr up, double a, EqualityPolicyPtr eq);
    double alpha_;
};

// ─────────────────────────────────────────────────────────────────────────────
// EWMATickRateNode — EWMA-smoothed tick occurrence rate
// Counts trigger firings (each eval = 1 occurrence) via exponential decay.
// Unlike EWMANode, the upstream value is ignored — only its firing matters.
// Output: double converging toward 1.0 as ticks keep arriving at rate ≥ alpha.
// ─────────────────────────────────────────────────────────────────────────────
class EWMATickRateNode
    : public StatefulNodeBase<EWMATickRateNode, double, double, EWMATickRateNodeState> {
public:
    using State = EWMATickRateNodeState;

    static std::shared_ptr<EWMATickRateNode> make(
        std::string name, NodePtr trigger, double alpha,
        EqualityPolicyPtr eq = nullptr);

    double doCompute(const double& /*trigger*/, State& s);
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    EWMATickRateNode(std::string n, NodePtr trigger, double alpha,
                     EqualityPolicyPtr eq);
    double alpha_;
};

// ─────────────────────────────────────────────────────────────────────────────
// DeltaNode<T>  —  value − previous value (first difference)
// Output: T  (zero on first tick)
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class DeltaNode
    : public StatefulNodeBase<DeltaNode<T>, T, T, DeltaNodeState<T>> {
public:
    using State = DeltaNodeState<T>;

    static std::shared_ptr<DeltaNode<T>> make(std::string name, NodePtr upstream);

    T    doCompute(const T& x, State& s);
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    DeltaNode(std::string n, NodePtr up);
};

// ─────────────────────────────────────────────────────────────────────────────
// ThresholdNode<T>
// Output: bool — true on the tick when the value crosses the level.
// Hysteresis: once triggered, value must move back by `hysteresis` before
// the node can trigger again.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class ThresholdNode
    : public StatefulNodeBase<ThresholdNode<T>, bool, T, ThresholdNodeState<T>> {
public:
    using State = ThresholdNodeState<T>;
    enum class Direction { Above, Below };

    static std::shared_ptr<ThresholdNode<T>> make(
        std::string name, NodePtr upstream,
        T level, Direction dir = Direction::Above, T hysteresis = T{});

    // ThresholdNode is a LEVEL DETECTOR:
    // • Output is true while the value is on the trigger side of the level.
    // • Hysteresis: once triggered, stays true until value crosses back by hys_.
    //   (prevents chatter near the threshold)
    // Pair with DebounceCountNode to require N consecutive true ticks.
    bool doCompute(const T& x, State& s);
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    ThresholdNode(std::string n, NodePtr up, T l, Direction d, T h);
    T level_, hys_;
    Direction dir_;
};

// ─────────────────────────────────────────────────────────────────────────────
// ZScoreNode
// (x − μ) / σ  using a single shared RollingStats node.
// Output: double
// Overrides eval() to also drive the inner RollingStats node each cycle.
// ─────────────────────────────────────────────────────────────────────────────
class ZScoreNode
    : public StatefulNodeBase<ZScoreNode, double, double, ZScoreNodeState> {
public:
    using State = ZScoreNodeState;

    /// stats must already have upstream wired; pass it in so it can be shared.
    static std::shared_ptr<ZScoreNode> make(
        std::string name, NodePtr upstream,
        RollingStatsPtr stats,
        EqualityPolicyPtr eq = nullptr);

    /// Convenience: creates its own RollingStats internally.
    static std::shared_ptr<ZScoreNode> make(
        std::string name, NodePtr upstream, std::size_t window,
        EqualityPolicyPtr eq = nullptr);

    ValuePtr eval(EvalContext& ctx) override;
    RollingStats& stats();

    // Trivial stub — eval() is overridden; doCompute is not called via the base scaffold.
    double doCompute(const double&, State&) { return 0.0; }
    void doSaveState(INodeState& s, const State&) const;
    void doRestoreState(const INodeState& s, State&);

private:
    ZScoreNode(std::string n, NodePtr up,
               RollingStatsPtr stats,
               EqualityPolicyPtr eq);

    RollingStatsPtr stats_;
};

// ─────────────────────────────────────────────────────────────────────────────
// OutlierGateNode
// Passes raw if |z| < threshold, else substitutes rolling mean.
// Shares one RollingStats between the z-score and mean lookups.
// Output: double
// Overrides eval() to also drive the inner ZScoreNode each cycle.
// ─────────────────────────────────────────────────────────────────────────────
class OutlierGateNode
    : public StatefulNodeBase<OutlierGateNode, double, double, OutlierGateNodeState> {
public:
    using State = OutlierGateNodeState;

    static std::shared_ptr<OutlierGateNode> make(
        std::string name, NodePtr upstream, std::size_t window, double zThreshold = 3.0,
        EqualityPolicyPtr eq = nullptr);

    ValuePtr eval(EvalContext& ctx) override;
    bool lastWasOutlier() const;

    // Trivial stub — eval() is overridden; doCompute is not called via the base scaffold.
    double doCompute(const double&, State&) { return 0.0; }
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    OutlierGateNode(std::string n, NodePtr up,
                    std::shared_ptr<ZScoreNode> z, double thr,
                    EqualityPolicyPtr eq);

    std::shared_ptr<ZScoreNode> zNode_;
    double threshold_;
};

// ─────────────────────────────────────────────────────────────────────────────
// RateLimiterNode<T>
// Only emits a new value downstream if |Δ from last emission| >= minDelta.
// Works on any numeric T that supports operator- and std::abs.
// Output: T (last emitted value)
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class RateLimiterNode
    : public StatefulNodeBase<RateLimiterNode<T>, T, T, RateLimiterNodeState<T>> {
public:
    using State = RateLimiterNodeState<T>;

    static std::shared_ptr<RateLimiterNode<T>> make(
        std::string name, NodePtr upstream, T minDelta);

    // NO invalidation override. It used to absorb upstream invalidation without
    // forwarding, and the tri-state protocol makes that both unnecessary and
    // wrong:
    //
    //   • Unnecessary, because NodeBase's default IS the behaviour the override
    //     was hand-writing — mark self Dirty, tell downstream Maybe. "Maybe" is
    //     exactly the suppression flag a limiter needs: a consumer is
    //     told something upstream moved, and finds out whether it matters by
    //     pulling. If this node does not emit, a Lazy consumer skips.
    //
    //   • Wrong, because absorbing meant a consumer was never dirtied, so in a
    //     pull-based graph it never pulled this node, so this node never reached
    //     the eval() that would have released — and a limiter wired MID-GRAPH
    //     never propagated its release at all. It only ever worked when
    //     registered directly as an engine output. See
    //     ValueSlot.RateLimiterWiredMidGraphPropagatesItsReleaseThroughTheEngine.
    //
    // The trade this makes, stated plainly: an EAGER consumer now recomputes on
    // every upstream change where it used to recompute on none. That is what
    // Eager means, and it gains the release it never used to get. Suppression is
    // now opt-in, by declaring the consumer Lazy.

    T    doCompute(const T& x, State& s);
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    RateLimiterNode(std::string n, NodePtr up, T d);
    T minDelta_;
};

// ─────────────────────────────────────────────────────────────────────────────
// DebounceCountNode
// Propagates true only after the upstream bool has been true for
// `required` consecutive ticks.  Resets immediately on false.
// Output: bool
// ─────────────────────────────────────────────────────────────────────────────
class DebounceCountNode
    : public StatefulNodeBase<DebounceCountNode, bool, bool, DebounceCountNodeState> {
public:
    using State = DebounceCountNodeState;

    static std::shared_ptr<DebounceCountNode> make(
        std::string name, NodePtr upstream, std::size_t required);

    bool doCompute(const bool& in, State& s);
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    DebounceCountNode(std::string n, NodePtr up, std::size_t req);
    std::size_t required_;
};

// ─────────────────────────────────────────────────────────────────────────────
// LatchedDebounceNode
// Input:  bool (typically from ThresholdNode)
// Output: std::optional<bool>
//           true    — onset:    N consecutive true ticks confirmed; latch was clear
//           false   — resolved: upstream went false; latch was set
//           nullopt — no transition. The first nullopt after a transition is a
//                     change of value, so it propagates and reaches an output
//                     callback. Later ones are equal, and are suppressed.
//
// Combines debounce counting and edge detection in a single stateful node.
// Resolved transitions are immediate (no debounce on the falling edge) — the
// upstream ThresholdNode's hysteresis already prevents chatter there.
// ─────────────────────────────────────────────────────────────────────────────
class LatchedDebounceNode
    : public StatefulNodeBase<LatchedDebounceNode,
                              std::optional<bool>, bool,
                              LatchedDebounceNodeState> {
public:
    using State = LatchedDebounceNodeState;

    static std::shared_ptr<LatchedDebounceNode> make(
        std::string name, NodePtr upstream, std::size_t required);

    std::optional<bool> doCompute(const bool& x, State& s);
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    LatchedDebounceNode(std::string n, NodePtr up, std::size_t req);
    std::size_t required_;
};

// ─────────────────────────────────────────────────────────────────────────────
// DelayNode<T>
// N-tick ring buffer.  Returns the value from exactly N ticks ago.
// On the first N−1 ticks the output is `initialValue` (default T{}).
// Output: T
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class DelayNode
    : public StatefulNodeBase<DelayNode<T>, T, T, DelayNodeState<T>>,
      public IWindowed {
public:
    using State = DelayNodeState<T>;

    static std::shared_ptr<DelayNode<T>> make(
        std::string name, NodePtr upstream, std::size_t delay,
        T initialValue = T{},
        EqualityPolicyPtr eq = nullptr);

    /// How many ticks have been buffered so far.
    std::size_t bufferedCount() const;
    /// True once the first real delayed value has been emitted (i.e., delay+1 ticks seen).
    bool isWarm() const;

    // IWindowed
    std::size_t capacity() const noexcept override { return delay_; }
    std::size_t filled()   const noexcept override { return this->state_.buf.size(); }
    NodePtr windowStatusNode() const override;

    T    doCompute(const T& x, State& s);
    void doSaveState(INodeState& s, const State& st) const;
    void doRestoreState(const INodeState& s, State& st);

private:
    DelayNode(std::string n, NodePtr up, std::size_t delay,
              T initial, EqualityPolicyPtr eq);
    std::size_t delay_;
    T initial_;
    mutable std::weak_ptr<INode> windowStatusNode_;
};

// ─────────────────────────────────────────────────────────────────────────────
// makeTimeDelayNode<T> — TIME-based delay (contrast DelayNode's N-*tick* delay).
//
// Output at each eval is the value that was current at (now − horizonUs): the
// most recent sample at or before that instant. Unlike a tick-count delay this is
// robust to irregular update cadence (a quiet stream vs a busy one) and is
// replay-correct — the horizon is measured in the SAME time base as the `timeUs`
// input, whatever that is.
//
// Inputs (the "time as a DAG input" design — pure, no wall-clock read):
//   value  : the observed series.
//   timeUs : a monotonic microsecond clock supplied as a DAG input by the app
//            (real steady_clock live · ReplayClock in replay · the app's own cycle
//            clock). Time flows as data, so the node depends only on its inputs and
//            is deterministic under replay.
//
// Output std::optional<T>: nullopt until at least horizonUs of history exists
//   (warm-up), so downstream gates on has_value(); the default
//   TypedEqualityPolicy<optional<T>> suppresses the nullopt==nullopt stretch.
//
// State is a captured (time,value) ring — the "ComputeNode + captured state"
// pattern (a two-input node cannot use single-input StatefulNodeBase). It is
// deliberately NOT persisted: timestamps are clock-epoch-relative (a restart
// resets steady_clock's epoch), so a restored buffer would be meaningless, and at
// seconds-scale horizons re-warming costs nothing.
template<typename T>
ComputeNodePtr<std::optional<T>, T, std::int64_t>
makeTimeDelayNode(std::string name, NodePtr value, NodePtr timeUs,
                  std::int64_t horizonUs, EqualityPolicyPtr eq = nullptr);

} // namespace dag::ts

#include "dag_timeseries.inl"
