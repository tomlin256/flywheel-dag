// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_timeseries.inl — implementation of all dag_timeseries.hpp declarations
// Included at the bottom of dag_timeseries.hpp; never include this file directly.

#pragma once

namespace dag::ts {

// ─────────────────────────────────────────────────────────────────────────────
// NodeImpl<Derived>
// ─────────────────────────────────────────────────────────────────────────────

template<typename Derived>
void NodeImpl<Derived>::notifyDownstream(
    const ValuePtr& newV, EqualityPolicyPtr& eq)
{
    if (!eq->equal(cached_, newV)) {
        cached_ = newV;
        this->NodeBase::notifyDownstream();
    }
    this->markClean();
}

// ─────────────────────────────────────────────────────────────────────────────
// StatefulNodeBase<Derived, Out, In, State>
// ─────────────────────────────────────────────────────────────────────────────

template<typename Derived, typename Out, typename In, typename State>
StatefulNodeBase<Derived, Out, In, State>::StatefulNodeBase(
    std::string name, NodePtr upstream, EqualityPolicyPtr eq)
    : name_(std::move(name))
    , upstream_(std::move(upstream))
    , eq_(eq ? std::move(eq) : std::make_shared<TypedEqualityPolicy<Out>>())
{}

template<typename Derived, typename Out, typename In, typename State>
std::string StatefulNodeBase<Derived, Out, In, State>::name() const { return name_; }

template<typename Derived, typename Out, typename In, typename State>
std::vector<NodePtr> StatefulNodeBase<Derived, Out, In, State>::inputs() const {
    return { upstream_ };
}

template<typename Derived, typename Out, typename In, typename State>
ValuePtr StatefulNodeBase<Derived, Out, In, State>::eval(EvalContext& ctx) {
    static_assert(std::is_invocable_r_v<Out,
        decltype(&Derived::doCompute), Derived*, const In&, State&>,
        "Derived must implement Out doCompute(const In&, State&)");
    if (!this->dirty() && !ctx.forceRecompute) return this->cached_;
    const In& inp = get_value<In>(upstream_->eval(ctx));
    Out result = static_cast<Derived*>(this)->doCompute(inp, state_);
    // notifyDownstream() owns cached_: it rebinds it only when the policy says
    // the value changed. Rebinding it here as well (flywheel-dag#1) gave cached_
    // a new identity on every evaluation, so Engine::cycle, which detects change
    // by pointer identity, fired this node's output callback on every dirty
    // cycle. It also made the policy compare against the previous evaluation
    // instead of the last published value, so a drift smaller than a tolerance
    // per step never published at all.
    this->notifyDownstream(slot_.emit(std::move(result)), eq_);
    return this->cached_;
}

template<typename Derived, typename Out, typename In, typename State>
void StatefulNodeBase<Derived, Out, In, State>::saveState(INodeState& s) const {
    static_assert(std::is_invocable_r_v<void,
        decltype(&Derived::doSaveState), const Derived*, INodeState&, const State&>,
        "Derived must implement void doSaveState(INodeState&, const State&) const");
    static_cast<const Derived*>(this)->doSaveState(s, state_);
}

template<typename Derived, typename Out, typename In, typename State>
void StatefulNodeBase<Derived, Out, In, State>::restoreState(const INodeState& s) {
    static_assert(std::is_invocable_r_v<void,
        decltype(&Derived::doRestoreState), Derived*, const INodeState&, State&>,
        "Derived must implement void doRestoreState(const INodeState&, State&)");
    static_cast<Derived*>(this)->doRestoreState(s, state_);
    this->invalidate();
}

// ─────────────────────────────────────────────────────────────────────────────
// WindowNode<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
std::shared_ptr<WindowNode<T>> WindowNode<T>::make(
    std::string name, NodePtr upstream, std::size_t capacity)
{
    auto self = std::shared_ptr<WindowNode<T>>(
        new WindowNode<T>(std::move(name), upstream, capacity));
    wire(self, self->inputs());
    return self;
}

template<typename T>
WindowNode<T>::WindowNode(std::string n, NodePtr up, std::size_t cap)
    : StatefulNodeBase<WindowNode<T>, std::deque<T>, T, WindowNodeState<T>>(
        std::move(n), up, std::make_shared<AlwaysChangedPolicy>())
    , cap_(cap)
{}

template<typename T>
const std::deque<T>& WindowNode<T>::window() const { return this->state_.buf; }

template<typename T>
bool WindowNode<T>::full() const { return this->state_.buf.size() == cap_; }

template<typename T>
std::deque<T> WindowNode<T>::doCompute(const T& x, State& s) {
    s.buf.push_back(x);
    if (s.buf.size() > cap_) s.buf.pop_front();
    return s.buf;
}

template<typename T>
void WindowNode<T>::doSaveState(INodeState& s, const State& st) const {
    std::vector<double> vec(st.buf.begin(), st.buf.end());
    s["buf"] = vec;
}

template<typename T>
void WindowNode<T>::doRestoreState(const INodeState& s, State& st) {
    auto vec = s["buf"].as<std::vector<double>>();
    st.buf.assign(vec.begin(), vec.end());
}

// ─────────────────────────────────────────────────────────────────────────────
// RollingStats
// ─────────────────────────────────────────────────────────────────────────────

inline std::shared_ptr<RollingStats> RollingStats::make(
    std::string name, NodePtr upstream, std::size_t window,
    EqualityPolicyPtr eq)
{
    auto self = std::shared_ptr<RollingStats>(
        new RollingStats(std::move(name), upstream, window, std::move(eq)));
    wire(self, self->inputs());
    return self;
}

inline RollingStats::RollingStats(std::string n, NodePtr up, std::size_t w,
                                   EqualityPolicyPtr eq)
    : StatefulNodeBase<RollingStats, double, double, RollingStatsState>(
        std::move(n), up, std::move(eq))
    , window_(w)
{
    // One spare slot above the window: sized for the transient peak rather than
    // the steady-state size, so no push can ever hit a full buffer. Storage is
    // allocated exactly here and never again.
    state_.buf.setCapacity(w + 1);
}

inline double RollingStats::mean()     const { return state_.count > 0 ? state_.mean : 0.0; }
inline double RollingStats::variance() const {
    if (state_.count < 2) return 0.0;
    return state_.M2 / static_cast<double>(state_.count - 1);
}
inline double RollingStats::stddev()   const { return std::sqrt(variance()); }
inline std::size_t RollingStats::count() const { return state_.count; }
inline double RollingStats::prevMean()   const { return state_.prevMean; }
inline double RollingStats::prevStddev() const { return state_.prevStddev; }

inline double RollingStats::doCompute(const double& x, State& s) {
    // Capture stats BEFORE including this sample — used by ZScoreNode for outlier detection
    s.prevMean   = s.count > 0 ? s.mean : 0.0;
    s.prevStddev = (s.count >= 2)
        ? std::sqrt(s.M2 / static_cast<double>(s.count - 1))
        : 0.0;
    push(x, s);
    return s.mean;
}

inline void RollingStats::doSaveState(INodeState& s, const State& st) const {
    std::vector<double> vec;
    vec.reserve(st.buf.size());
    st.buf.for_each_contiguous([&](const double* data, std::size_t n) {
        vec.insert(vec.end(), data, data + n);
    });
    s["buf"]        = vec;
    s["mean"]       = st.mean;
    s["M2"]         = st.M2;
    s["prevMean"]   = st.prevMean;
    s["prevStddev"] = st.prevStddev;
    s["count"]      = st.count;
}

inline void RollingStats::doRestoreState(const INodeState& s, State& st) {
    auto vec = s["buf"].as<std::vector<double>>();
    st.buf.clear();
    for (double v : vec) st.buf.push_back(v);
    st.mean       = s["mean"].as<double>();
    st.M2         = s["M2"].as<double>();
    st.prevMean   = s["prevMean"].as<double>();
    st.prevStddev = s["prevStddev"].as<double>();
    st.count      = s["count"].as<std::size_t>();
}

inline void RollingStats::push(double x, State& s) {
    if (s.buf.size() == window_) {
        // Remove oldest using Chan's parallel Welford downdate
        double old  = s.buf.front();
        s.buf.pop_front();
        double n    = static_cast<double>(s.buf.size() + 1); // before removal
        double newN = n - 1.0;
        if (newN <= 0) { s.mean = 0; s.M2 = 0; s.count = 0; }
        else {
            double delta  = old - s.mean;
            s.mean       -= delta / newN;
            double delta2 = old - s.mean;
            s.M2         -= delta * delta2;
            if (s.M2 < 0) s.M2 = 0; // numerical guard
            s.count = static_cast<std::size_t>(newN);
        }
    }
    s.buf.push_back(x);
    ++s.count;
    double delta  = x - s.mean;
    s.mean       += delta / static_cast<double>(s.count);
    double delta2 = x - s.mean;
    s.M2         += delta * delta2;
}

// ─────────────────────────────────────────────────────────────────────────────
// RollingSumNode
// ─────────────────────────────────────────────────────────────────────────────

inline std::shared_ptr<RollingSumNode> RollingSumNode::make(
    std::string name, NodePtr upstream, std::size_t window,
    EqualityPolicyPtr eq)
{
    auto self = std::shared_ptr<RollingSumNode>(
        new RollingSumNode(std::move(name), upstream, window, std::move(eq)));
    wire(self, self->inputs());
    return self;
}

inline RollingSumNode::RollingSumNode(std::string n, NodePtr up, std::size_t w,
                                       EqualityPolicyPtr eq)
    : StatefulNodeBase<RollingSumNode, double, double, RollingSumNodeState>(
        std::move(n), up, std::move(eq))
    , window_(w)
{
    state_.buf.setCapacity(w + 1);
}

inline double RollingSumNode::doCompute(const double& x, State& s) {
    if (s.buf.size() == window_ && window_ > 0) {
        s.sum -= s.buf.front();
        s.buf.pop_front();
    }
    s.buf.push_back(x);
    s.sum += x;

    // See the class comment: bound the drift an incremental total accumulates.
    if (++s.sinceRecompute >= window_) recompute(s);
    return s.sum;
}

inline void RollingSumNode::recompute(State& s) {
    double total = 0.0;
    s.buf.for_each_contiguous([&](const double* data, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) total += data[i];
    });
    s.sum            = total;
    s.sinceRecompute = 0;
}

inline void RollingSumNode::doSaveState(INodeState& s, const State& st) const {
    std::vector<double> vec;
    vec.reserve(st.buf.size());
    st.buf.for_each_contiguous([&](const double* data, std::size_t n) {
        vec.insert(vec.end(), data, data + n);
    });
    s["buf"]            = vec;
    s["sum"]            = st.sum;
    s["sinceRecompute"] = st.sinceRecompute;
}

inline void RollingSumNode::doRestoreState(const INodeState& s, State& st) {
    auto vec = s["buf"].as<std::vector<double>>();
    st.buf.clear();
    for (double v : vec) st.buf.push_back(v);
    st.sum            = s["sum"].as<double>();
    st.sinceRecompute = s["sinceRecompute"].as<std::size_t>();
}

// ─────────────────────────────────────────────────────────────────────────────
// RollingMinMaxNode
// ─────────────────────────────────────────────────────────────────────────────

inline std::shared_ptr<RollingMinMaxNode> RollingMinMaxNode::make(
    std::string name, NodePtr upstream, std::size_t window)
{
    auto self = std::shared_ptr<RollingMinMaxNode>(
        new RollingMinMaxNode(std::move(name), upstream, window));
    wire(self, self->inputs());
    return self;
}

inline RollingMinMaxNode::RollingMinMaxNode(std::string n, NodePtr up, std::size_t w)
    : StatefulNodeBase<RollingMinMaxNode, std::pair<double,double>, double,
                       RollingMinMaxNodeState>(std::move(n), up)
    , window_(w)
{
    // A monotonic deque holds at most `window_` entries after eviction plus the
    // one being pushed; the spare slot covers that peak and the window_ == 0 edge.
    state_.minDeq.setCapacity(w + 1);
    state_.maxDeq.setCapacity(w + 1);
}

inline std::pair<double,double> RollingMinMaxNode::doCompute(const double& x, State& s) {
    push(x, s);
    return { s.minDeq.front().val, s.maxDeq.front().val };
}

inline void RollingMinMaxNode::doSaveState(INodeState& s, const State& st) const {
    std::vector<double> minVals, minTicks, maxVals, maxTicks;
    for (std::size_t i = 0; i < st.minDeq.size(); ++i) {
        minVals.push_back(st.minDeq[i].val);
        minTicks.push_back(static_cast<double>(st.minDeq[i].idx));
    }
    for (std::size_t i = 0; i < st.maxDeq.size(); ++i) {
        maxVals.push_back(st.maxDeq[i].val);
        maxTicks.push_back(static_cast<double>(st.maxDeq[i].idx));
    }
    s["minDeq.values"] = minVals;
    s["minDeq.ticks"]  = minTicks;
    s["maxDeq.values"] = maxVals;
    s["maxDeq.ticks"]  = maxTicks;
    s["tick"]          = st.tick;
}

inline void RollingMinMaxNode::doRestoreState(const INodeState& s, State& st) {
    auto minVals  = s["minDeq.values"].as<std::vector<double>>();
    auto minTicks = s["minDeq.ticks"].as<std::vector<double>>();
    auto maxVals  = s["maxDeq.values"].as<std::vector<double>>();
    auto maxTicks = s["maxDeq.ticks"].as<std::vector<double>>();

    st.minDeq.clear();
    for (std::size_t i = 0; i < minVals.size(); ++i)
        st.minDeq.push_back({minVals[i], static_cast<std::size_t>(minTicks[i])});

    st.maxDeq.clear();
    for (std::size_t i = 0; i < maxVals.size(); ++i)
        st.maxDeq.push_back({maxVals[i], static_cast<std::size_t>(maxTicks[i])});

    st.tick = s["tick"].as<std::size_t>();
}

inline void RollingMinMaxNode::push(double x, State& s) {
    // Evict entries outside the window
    while (!s.minDeq.empty() && s.minDeq.front().idx + window_ <= s.tick) s.minDeq.pop_front();
    while (!s.maxDeq.empty() && s.maxDeq.front().idx + window_ <= s.tick) s.maxDeq.pop_front();
    // Maintain monotonic property
    while (!s.minDeq.empty() && s.minDeq.back().val >= x) s.minDeq.pop_back();
    while (!s.maxDeq.empty() && s.maxDeq.back().val <= x) s.maxDeq.pop_back();
    s.minDeq.push_back({x, s.tick});
    s.maxDeq.push_back({x, s.tick});
    ++s.tick;
}

// ─────────────────────────────────────────────────────────────────────────────
// EWMANode
// ─────────────────────────────────────────────────────────────────────────────

inline std::shared_ptr<EWMANode> EWMANode::make(
    std::string name, NodePtr upstream, double alpha,
    EqualityPolicyPtr eq)
{
    // Throw, not assert: the default build type defines NDEBUG, and an
    // out-of-range alpha silently producing a divergent EWMA is exactly the
    // class of failure that must surface rather than be assumed away.
    if (!(alpha > 0.0 && alpha <= 1.0))
        throw std::invalid_argument(
            "EWMANode '" + name + "': alpha must be in (0, 1]");
    auto self = std::shared_ptr<EWMANode>(
        new EWMANode(std::move(name), upstream, alpha, std::move(eq)));
    wire(self, self->inputs());
    return self;
}

inline EWMANode::EWMANode(std::string n, NodePtr up, double a,
                           EqualityPolicyPtr eq)
    : StatefulNodeBase<EWMANode, double, double, EWMANodeState>(
        std::move(n), up, std::move(eq))
    , alpha_(a)
{}

inline double EWMANode::doCompute(const double& x, State& s) {
    s.ewma = s.initialized ? (alpha_ * x + (1.0 - alpha_) * s.ewma) : x;
    s.initialized = true;
    return s.ewma;
}

inline void EWMANode::doSaveState(INodeState& s, const State& st) const {
    s["ewma"]        = st.ewma;
    s["initialized"] = st.initialized;
}

inline void EWMANode::doRestoreState(const INodeState& s, State& st) {
    st.ewma        = s["ewma"].as<double>();
    st.initialized = s["initialized"].as<bool>();
}

// ─────────────────────────────────────────────────────────────────────────────
// EWMATickRateNode
// ─────────────────────────────────────────────────────────────────────────────

inline std::shared_ptr<EWMATickRateNode> EWMATickRateNode::make(
    std::string name, NodePtr trigger, double alpha,
    EqualityPolicyPtr eq)
{
    // See EWMANode::make — throws rather than asserts for the same reason.
    if (!(alpha > 0.0 && alpha <= 1.0))
        throw std::invalid_argument(
            "EWMATickRateNode '" + name + "': alpha must be in (0, 1]");
    auto self = std::shared_ptr<EWMATickRateNode>(
        new EWMATickRateNode(std::move(name), trigger, alpha, std::move(eq)));
    wire(self, self->inputs());
    return self;
}

inline EWMATickRateNode::EWMATickRateNode(std::string n, NodePtr trigger, double alpha,
                                           EqualityPolicyPtr eq)
    : StatefulNodeBase<EWMATickRateNode, double, double, EWMATickRateNodeState>(
        std::move(n), trigger, std::move(eq))
    , alpha_(alpha)
{}

inline double EWMATickRateNode::doCompute(const double& /*trigger*/, State& s) {
    s.rate = alpha_ + (1.0 - alpha_) * s.rate;
    return s.rate;
}

inline void EWMATickRateNode::doSaveState(INodeState& s, const State& st) const {
    s["rate"] = st.rate;
}

inline void EWMATickRateNode::doRestoreState(const INodeState& s, State& st) {
    st.rate = s["rate"].as<double>();
}

// ─────────────────────────────────────────────────────────────────────────────
// DeltaNode<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
std::shared_ptr<DeltaNode<T>> DeltaNode<T>::make(std::string name, NodePtr upstream) {
    auto self = std::shared_ptr<DeltaNode<T>>(new DeltaNode<T>(std::move(name), upstream));
    wire(self, self->inputs());
    return self;
}

template<typename T>
DeltaNode<T>::DeltaNode(std::string n, NodePtr up)
    : StatefulNodeBase<DeltaNode<T>, T, T, DeltaNodeState<T>>(std::move(n), up)
{}

template<typename T>
T DeltaNode<T>::doCompute(const T& x, State& s) {
    T delta  = s.hasPrev ? (x - s.prev) : T{};
    s.prev   = x;
    s.hasPrev = true;
    return delta;
}

template<typename T>
void DeltaNode<T>::doSaveState(INodeState& s, const State& st) const {
    s["prev"]    = static_cast<double>(st.prev);
    s["hasPrev"] = st.hasPrev;
}

template<typename T>
void DeltaNode<T>::doRestoreState(const INodeState& s, State& st) {
    st.prev    = static_cast<T>(s["prev"].as<double>());
    st.hasPrev = s["hasPrev"].as<bool>();
}

// ─────────────────────────────────────────────────────────────────────────────
// ThresholdNode<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
std::shared_ptr<ThresholdNode<T>> ThresholdNode<T>::make(
    std::string name, NodePtr upstream,
    T level, Direction dir, T hysteresis)
{
    auto self = std::shared_ptr<ThresholdNode<T>>(
        new ThresholdNode<T>(std::move(name), upstream, level, dir, hysteresis));
    wire(self, self->inputs());
    return self;
}

template<typename T>
ThresholdNode<T>::ThresholdNode(std::string n, NodePtr up, T l, Direction d, T h)
    : StatefulNodeBase<ThresholdNode<T>, bool, T, ThresholdNodeState<T>>(
        std::move(n), up, std::make_shared<TypedEqualityPolicy<bool>>())
    , level_(l), hys_(h), dir_(d)
{}

template<typename T>
bool ThresholdNode<T>::doCompute(const T& x, State& s) {
    bool prevState = s.triggered;
    bool newState  = prevState;
    if (dir_ == Direction::Above) {
        if (!prevState && x > level_)            newState = true;
        else if (prevState && x < level_ - hys_) newState = false;
    } else {
        if (!prevState && x < level_)            newState = true;
        else if (prevState && x > level_ + hys_) newState = false;
    }
    s.triggered = newState;
    return newState;
}

template<typename T>
void ThresholdNode<T>::doSaveState(INodeState& s, const State& st) const {
    s["triggered"] = st.triggered;
}

template<typename T>
void ThresholdNode<T>::doRestoreState(const INodeState& s, State& st) {
    st.triggered = s["triggered"].as<bool>();
}

// ─────────────────────────────────────────────────────────────────────────────
// ZScoreNode
// ─────────────────────────────────────────────────────────────────────────────

inline std::shared_ptr<ZScoreNode> ZScoreNode::make(
    std::string name, NodePtr upstream,
    RollingStatsPtr stats,
    EqualityPolicyPtr eq)
{
    auto self = std::shared_ptr<ZScoreNode>(
        new ZScoreNode(std::move(name), upstream, std::move(stats), std::move(eq)));
    wire(self, self->inputs());
    return self;
}

inline std::shared_ptr<ZScoreNode> ZScoreNode::make(
    std::string name, NodePtr upstream, std::size_t window,
    EqualityPolicyPtr eq)
{
    auto stats = RollingStats::make(name + ".stats", upstream, window);
    auto self = std::shared_ptr<ZScoreNode>(
        new ZScoreNode(std::move(name), upstream, std::move(stats), std::move(eq)));
    wire(self, self->inputs());
    return self;
}

inline ZScoreNode::ZScoreNode(std::string n, NodePtr up,
                               RollingStatsPtr stats,
                               EqualityPolicyPtr eq)
    : StatefulNodeBase<ZScoreNode, double, double, ZScoreNodeState>(
        std::move(n), up, std::move(eq))
    , stats_(std::move(stats))
{}

inline ValuePtr ZScoreNode::eval(EvalContext& ctx) {
    if (!this->dirty() && !ctx.forceRecompute) return this->cached_;
    double x  = get_value<double>(upstream_->eval(ctx));
    stats_->eval(ctx);
    double mu = stats_->prevMean();
    double sd = stats_->prevStddev();
    double z  = (sd < 1e-12) ? 0.0 : (x - mu) / sd;
    // notifyDownstream() owns cached_ — see StatefulNodeBase::eval.
    this->notifyDownstream(slot_.emit(z), eq_);
    return this->cached_;
}

inline RollingStats& ZScoreNode::stats() { return *stats_; }

inline void ZScoreNode::doSaveState(INodeState& s, const State&) const {
    stats_->saveState(s.sub("stats"));
}

inline void ZScoreNode::doRestoreState(const INodeState& s, State&) {
    stats_->restoreState(s.sub("stats"));
}

// ─────────────────────────────────────────────────────────────────────────────
// OutlierGateNode
// ─────────────────────────────────────────────────────────────────────────────

inline std::shared_ptr<OutlierGateNode> OutlierGateNode::make(
    std::string name, NodePtr upstream, std::size_t window, double zThreshold,
    EqualityPolicyPtr eq)
{
    // One shared RollingStats feeds both ZScore and mean access
    auto stats  = RollingStats::make(name + ".stats", upstream, window);
    auto zscore = ZScoreNode::make(name + ".z", upstream, stats);
    auto self   = std::shared_ptr<OutlierGateNode>(
        new OutlierGateNode(std::move(name), upstream, zscore, zThreshold, std::move(eq)));
    wire(self, self->inputs());
    return self;
}

inline OutlierGateNode::OutlierGateNode(std::string n, NodePtr up,
                                         std::shared_ptr<ZScoreNode> z, double thr,
                                         EqualityPolicyPtr eq)
    : StatefulNodeBase<OutlierGateNode, double, double, OutlierGateNodeState>(
        std::move(n), up, std::move(eq))
    , zNode_(std::move(z))
    , threshold_(thr)
{}

inline ValuePtr OutlierGateNode::eval(EvalContext& ctx) {
    if (!this->dirty() && !ctx.forceRecompute) return this->cached_;
    double x  = get_value<double>(upstream_->eval(ctx));
    double z  = get_value<double>(zNode_->eval(ctx));
    double mu = zNode_->stats().mean();
    state_.lastWasOutlier = std::abs(z) >= threshold_;
    double out = state_.lastWasOutlier ? mu : x;
    // notifyDownstream() owns cached_ — see StatefulNodeBase::eval.
    this->notifyDownstream(slot_.emit(out), eq_);
    return this->cached_;
}

inline bool OutlierGateNode::lastWasOutlier() const { return state_.lastWasOutlier; }

inline void OutlierGateNode::doSaveState(INodeState& s, const State& st) const {
    s["lastWasOutlier"] = st.lastWasOutlier;
    zNode_->saveState(s.sub("zscore"));
}

inline void OutlierGateNode::doRestoreState(const INodeState& s, State& st) {
    st.lastWasOutlier = s["lastWasOutlier"].as<bool>();
    zNode_->restoreState(s.sub("zscore"));
}

// ─────────────────────────────────────────────────────────────────────────────
// RateLimiterNode<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
std::shared_ptr<RateLimiterNode<T>> RateLimiterNode<T>::make(
    std::string name, NodePtr upstream, T minDelta)
{
    auto self = std::shared_ptr<RateLimiterNode<T>>(
        new RateLimiterNode<T>(std::move(name), upstream, minDelta));
    wire(self, self->inputs());
    return self;
}

template<typename T>
RateLimiterNode<T>::RateLimiterNode(std::string n, NodePtr up, T d)
    : StatefulNodeBase<RateLimiterNode<T>, T, T, RateLimiterNodeState<T>>(std::move(n), up)
    , minDelta_(d)
{}

template<typename T>
T RateLimiterNode<T>::doCompute(const T& x, State& s) {
    bool doEmit = !s.hasEmitted || (std::abs(x - s.lastEmitted) >= minDelta_);
    if (doEmit) {
        s.lastEmitted = x;
        s.hasEmitted  = true;
    }
    return s.lastEmitted;
}

template<typename T>
void RateLimiterNode<T>::doSaveState(INodeState& s, const State& st) const {
    s["lastEmitted"] = static_cast<double>(st.lastEmitted);
    s["hasEmitted"]  = st.hasEmitted;
}

template<typename T>
void RateLimiterNode<T>::doRestoreState(const INodeState& s, State& st) {
    st.lastEmitted = static_cast<T>(s["lastEmitted"].as<double>());
    st.hasEmitted  = s["hasEmitted"].as<bool>();
    // Restore cached so the next non-emitting eval does not spuriously notify downstream
    if (st.hasEmitted) this->cached_ = make_value(st.lastEmitted);
}

// ─────────────────────────────────────────────────────────────────────────────
// DebounceCountNode
// ─────────────────────────────────────────────────────────────────────────────

inline std::shared_ptr<DebounceCountNode> DebounceCountNode::make(
    std::string name, NodePtr upstream, std::size_t required)
{
    auto self = std::shared_ptr<DebounceCountNode>(
        new DebounceCountNode(std::move(name), upstream, required));
    wire(self, self->inputs());
    return self;
}

inline DebounceCountNode::DebounceCountNode(std::string n, NodePtr up, std::size_t req)
    : StatefulNodeBase<DebounceCountNode, bool, bool, DebounceCountNodeState>(
        std::move(n), up, std::make_shared<TypedEqualityPolicy<bool>>())
    , required_(req)
{}

inline bool DebounceCountNode::doCompute(const bool& in, State& s) {
    s.count = in ? s.count + 1 : 0;
    return (s.count >= required_);
}

inline void DebounceCountNode::doSaveState(INodeState& s, const State& st) const {
    s["count"] = st.count;
}

inline void DebounceCountNode::doRestoreState(const INodeState& s, State& st) {
    st.count = s["count"].as<std::size_t>();
}

// ─────────────────────────────────────────────────────────────────────────────
// LatchedDebounceNode
// ─────────────────────────────────────────────────────────────────────────────

inline std::shared_ptr<LatchedDebounceNode> LatchedDebounceNode::make(
    std::string name, NodePtr upstream, std::size_t required)
{
    auto self = std::shared_ptr<LatchedDebounceNode>(
        new LatchedDebounceNode(std::move(name), upstream, required));
    wire(self, self->inputs());
    return self;
}

inline LatchedDebounceNode::LatchedDebounceNode(
    std::string n, NodePtr up, std::size_t req)
    : StatefulNodeBase<LatchedDebounceNode,
                       std::optional<bool>, bool,
                       LatchedDebounceNodeState>(std::move(n), up)
    , required_(req)
{}

inline std::optional<bool> LatchedDebounceNode::doCompute(const bool& x, State& s) {
    if (x) {
        if (!s.latched && ++s.count >= required_) {
            s.latched = true;
            s.count   = 0;
            return true;
        }
        return std::nullopt;
    } else {
        s.count = 0;
        if (s.latched) {
            s.latched = false;
            return false;
        }
        return std::nullopt;
    }
}

inline void LatchedDebounceNode::doSaveState(INodeState& s, const State& st) const {
    s["latched"] = st.latched;
    s["count"]   = st.count;
}

inline void LatchedDebounceNode::doRestoreState(const INodeState& s, State& st) {
    st.latched = s["latched"].as<bool>();
    st.count   = s["count"].as<std::size_t>();
}

// ─────────────────────────────────────────────────────────────────────────────
// DelayNode<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
std::shared_ptr<DelayNode<T>> DelayNode<T>::make(
    std::string name, NodePtr upstream, std::size_t delay,
    T initialValue,
    EqualityPolicyPtr eq)
{
    auto self = std::shared_ptr<DelayNode<T>>(
        new DelayNode<T>(std::move(name), upstream, delay,
                         std::move(initialValue), std::move(eq)));
    wire(self, self->inputs());
    return self;
}

template<typename T>
DelayNode<T>::DelayNode(std::string n, NodePtr up, std::size_t delay,
                         T initial, EqualityPolicyPtr eq)
    : StatefulNodeBase<DelayNode<T>, T, T, DelayNodeState<T>>(
        std::move(n), up, std::move(eq))
    , delay_(delay)
    , initial_(std::move(initial))
{
    // doCompute pushes before popping, so the transient peak is delay_ + 1.
    this->state_.buf.setCapacity(delay + 1);
}

template<typename T>
std::size_t DelayNode<T>::bufferedCount() const { return this->state_.buf.size(); }

template<typename T>
bool DelayNode<T>::isWarm() const { return this->state_.warm; }

template<typename T>
T DelayNode<T>::doCompute(const T& x, State& s) {
    s.buf.push_back(x);
    if (s.buf.size() > delay_) {
        T out = s.buf.front();
        s.buf.pop_front();
        s.warm = true;
        return out;
    }
    return initial_;
}

template<typename T>
void DelayNode<T>::doSaveState(INodeState& s, const State& st) const {
    std::vector<double> vec;
    vec.reserve(st.buf.size());
    for (std::size_t i = 0; i < st.buf.size(); ++i)
        vec.push_back(static_cast<double>(st.buf[i]));
    s["buf"]  = vec;
    s["warm"] = st.warm;
}

template<typename T>
void DelayNode<T>::doRestoreState(const INodeState& s, State& st) {
    auto vec = s["buf"].as<std::vector<double>>();
    st.buf.clear();
    for (double v : vec) st.buf.push_back(static_cast<T>(v));
    st.warm = s["warm"].as<bool>();
}

// ─────────────────────────────────────────────────────────────────────────────
// makeTimeDelayNode<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
ComputeNodePtr<std::optional<T>, T, std::int64_t>
makeTimeDelayNode(std::string name, NodePtr value, NodePtr timeUs,
                  std::int64_t horizonUs, EqualityPolicyPtr eq) {
    struct Sample { std::int64_t t; T v; };
    auto buf = std::make_shared<std::deque<Sample>>();
    return ComputeNode<std::optional<T>, T, std::int64_t>::make(
        std::move(name),
        std::make_tuple(std::move(value), std::move(timeUs)),
        [buf, horizonUs](const T& v, const std::int64_t& now) -> std::optional<T> {
            // Record a sample only when the value changes: consecutive duplicates
            // across time-only cycles add nothing to an at-or-before query and
            // would grow the ring without bound.
            if (buf->empty() || !(buf->back().v == v))
                buf->push_back(Sample{now, v});
            const std::int64_t asOf = now - horizonUs;
            // Slide the front to the newest sample at or before the query instant;
            // anything older is unreachable by any future (non-decreasing) query.
            while (buf->size() >= 2 && (*buf)[1].t <= asOf) buf->pop_front();
            if (!buf->empty() && buf->front().t <= asOf) return buf->front().v;
            return std::nullopt;   // < horizonUs of history yet (warm-up)
        },
        std::move(eq));
}

// ─────────────────────────────────────────────────────────────────────────────
// IWindowed::windowStatusNode() implementations
//
// Each node stores a weak_ptr to avoid a reference cycle: the companion
// ComputeNode holds *this strongly via inputs_ and the lambda capture, so
// holding the companion strongly here would keep both alive indefinitely.
// Downstream consumers keep the companion alive via their own inputs_ edges.
//
// EVERY ONE OF THESE IS IMPURE, AND MUST STAY InvalidationMode::Eager.
//
// Look at the functor: it IGNORES its declared input — the parameter is
// unnamed — and returns capacity() and filled() read off the captured upstream
// node. The declared input is that node's VALUE; the output is a function of its
// internal STATE. filled() advances while the value sits still, so these nodes
// change on cycles where their declared input does not, and a Lazy version
// would skip exactly then.
//
// This is the same shape as any functor that reads a captured node's state
// instead of its declared input, but it is generated by an engine factory
// rather than written at a call site, so it appears once per windowed node in
// every graph. It was caught by replaying a recorded session and flagging any
// node whose value moved in a cycle where none of its declared inputs did: this
// family accounted for most of the witnesses, and every one of its skips would
// have been unsafe. Pinned by
// TimeSeries.WindowStatusChangesWhileItsDeclaredInputDoesNot.
// ─────────────────────────────────────────────────────────────────────────────

inline NodePtr RollingStats::windowStatusNode() const {
    if (auto existing = windowStatusNode_.lock()) return existing;
    auto self = std::const_pointer_cast<RollingStats>(
        std::static_pointer_cast<const RollingStats>(shared_from_this()));
    auto node = ComputeNode<WindowStatus, double>::make(
        this->name() + ".windowStatus",
        std::make_tuple(std::static_pointer_cast<INode>(self)),
        [self](const double&) -> WindowStatus {
            return { self->capacity(), self->filled() };
        },
        // EAGER, permanently. See the note above windowStatusNode().
        InvalidationMode::Eager);
    windowStatusNode_ = node;
    return node;
}

inline NodePtr RollingSumNode::windowStatusNode() const {
    if (auto existing = windowStatusNode_.lock()) return existing;
    auto self = std::const_pointer_cast<RollingSumNode>(
        std::static_pointer_cast<const RollingSumNode>(shared_from_this()));
    auto node = ComputeNode<WindowStatus, double>::make(
        this->name() + ".windowStatus",
        std::make_tuple(std::static_pointer_cast<INode>(self)),
        [self](const double&) -> WindowStatus {
            return { self->capacity(), self->filled() };
        },
        // EAGER, permanently. See the note above windowStatusNode().
        InvalidationMode::Eager);
    windowStatusNode_ = node;
    return node;
}

inline NodePtr RollingMinMaxNode::windowStatusNode() const {
    if (auto existing = windowStatusNode_.lock()) return existing;
    auto self = std::const_pointer_cast<RollingMinMaxNode>(
        std::static_pointer_cast<const RollingMinMaxNode>(shared_from_this()));
    auto node = ComputeNode<WindowStatus, std::pair<double,double>>::make(
        this->name() + ".windowStatus",
        std::make_tuple(std::static_pointer_cast<INode>(self)),
        [self](const std::pair<double,double>&) -> WindowStatus {
            return { self->capacity(), self->filled() };
        },
        // EAGER, permanently. See the note above windowStatusNode().
        InvalidationMode::Eager);
    windowStatusNode_ = node;
    return node;
}

template<typename T>
NodePtr WindowNode<T>::windowStatusNode() const {
    if (auto existing = windowStatusNode_.lock()) return existing;
    auto self = std::const_pointer_cast<WindowNode<T>>(
        std::static_pointer_cast<const WindowNode<T>>(this->shared_from_this()));
    auto node = ComputeNode<WindowStatus, std::deque<T>>::make(
        this->name() + ".windowStatus",
        std::make_tuple(std::static_pointer_cast<INode>(self)),
        [self](const std::deque<T>&) -> WindowStatus {
            return { self->capacity(), self->filled() };
        },
        // EAGER, permanently. See the note above windowStatusNode().
        InvalidationMode::Eager);
    windowStatusNode_ = node;
    return node;
}

template<typename T>
NodePtr DelayNode<T>::windowStatusNode() const {
    if (auto existing = windowStatusNode_.lock()) return existing;
    auto self = std::const_pointer_cast<DelayNode<T>>(
        std::static_pointer_cast<const DelayNode<T>>(this->shared_from_this()));
    auto node = ComputeNode<WindowStatus, T>::make(
        this->name() + ".windowStatus",
        std::make_tuple(std::static_pointer_cast<INode>(self)),
        [self](const T&) -> WindowStatus {
            return { self->capacity(), self->filled() };
        },
        // EAGER, permanently. See the note above windowStatusNode().
        InvalidationMode::Eager);
    windowStatusNode_ = node;
    return node;
}

} // namespace dag::ts
