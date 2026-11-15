// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_reduce.hpp — a subgraph as a function
//
// dag::reduce(root, args) turns the nodes between some argument nodes and a
// root into a dag::Reduction: a function of the arguments' values, which a
// solver can call at any point without moving the graph and, once warm,
// without allocating. Everything no argument reaches is a constant, read from
// the graph when the reduction is bound.
//
//   EvalContext ctx;
//   price->eval(ctx);                               // a reduction reads a clean root
//
//   dag::Reduction f = dag::reduce(price, {vol});   // price as a function of vol
//   f({0.30});                                      // the price, had vol been 0.30
//   double vega = 0.0;
//   f.gradient({0.30}, {&vega, 1});                 // the same, and ∂price/∂vol there
//
//   spot->set(101.0);                               // the point moves,
//   price->eval(ctx);
//   f.bind();                                       // so read it again
//
// Why not move the graph
// ──────────────────────
// "What would this output be, had that input been x?" can be asked by setting
// the input, evaluating the output and putting the input back. That evaluates
// the live nodes, so a stateful node between the two ticks once per trial, and
// it invalidates every consumer of the input, in the subgraph or not. A
// reduction applies each node's function to values of its own instead, so no
// node's cached value, slot, state or dirty flag moves.
//
// Three phases, each done as rarely as it can be
// ──────────────────────────────────────────────
//   1. Construct, once. One walk up from each root finds every node an
//      argument reaches, in topological order, and asks each for a step
//      (NodeBase::reduceStep(), dag.hpp). A graph is not rewired once it is
//      built, so the walk is never repeated. It reads no value, so a reduction
//      can be built while a graph is being wired, before anything is evaluated.
//   2. bind(), once per point. It follows what the roots depend on at the
//      values the graph holds now, decides which of those nodes an argument
//      moves here, and reads the value of every node they read that no
//      argument moves, and what a stateful node's step needs of its node
//      (IReduceStep::bind()).
//   3. Call, many times. The arguments' values go into the reduction's own
//      buffers, each step runs in order, and the roots' values come out.
//      Nothing in the graph is read or written.
//
// A bound reduction describes one point. Once a value it read at bind() moves,
// it still answers for the old point, and cannot tell: bind it again.
//
// Several roots
// ─────────────
// A reduction of several roots is a function ℝⁿ → ℝᵐ: the residuals of a fit,
// say, which share most of their nodes. A node that several roots depend on
// has one step, which a call runs once.
//
//   dag::Reduction r = dag::reduce(std::vector<NodePtr>{r0, r1, r2}, {a, b});
//   std::array<double, 3> y{};
//   std::array<double, 6> j{};              // one row per root: j[i·2 + k] = ∂rᵢ/∂argₖ
//   r({1.0, 2.0}, y);
//   r.jacobian({1.0, 2.0}, y, j);
//
// The one-root calls, operator()(x) and gradient(), are for a reduction of one
// root, and throw std::logic_error on any other.
//
// Which nodes can be called
// ─────────────────────────
// A node that an argument moves is called through its step, so it must give
// one. A reduction calls a functor only where a contract its author has already
// accepted makes that safe:
//
//   • Every dag::ops node. They are structurally pure: Op{} is built at each
//     evaluation.
//   • aad::DifferentiableNode. Its contract is a functor with no side effects,
//     which a tape runs again too.
//   • MemoizedComputeNode. Its contract is a functor that is a pure function.
//     The step calls the functor and not the cache, which trial values would
//     fill.
//   • ComputeNode, InPlaceComputeNode and TweakableComputeNode, when Lazy.
//     Lazy is the author's statement that the functor is a pure function of
//     its inputs. Eager says nothing, and is what a functor with captured
//     state must be: called at trial values, such a functor would advance on
//     every one.
//   • ConditionNode. It selects, and has no functor.
//   • EWMANode, RollingStats, RollingSumNode, ZScoreNode, OutlierGateNode,
//     and a DelayNode of no ticks. The step applies the node's last update to
//     another sample, from what it read of the node at bind(), so a call
//     advances nothing (dag_timeseries.hpp).
//   • A node of your own that overrides NodeBase::reduceStep().
//
// A node gives no step unless it says otherwise, and a node that gives none,
// with an argument above it, is a BARRIER: every other dag::ts node, an Eager
// compute node, and a node that does not derive NodeBase.
//
// The rules
// ─────────
//   • Every root must be clean and hold a double, or bind() throws
//     std::invalid_argument. A reduction reads what the graph holds and
//     evaluates nothing. Evaluating a dirty root for the caller would break an
//     engine: Engine::cycle() snapshots its outputs' dirty flags before it
//     evaluates them, so an output evaluated between cycles reads as clean,
//     and its callback misses the change. A clean root's inputs are clean too,
//     so every value bind() pulls is a cached one. Bind in the root's output
//     callback, in a node that has just pulled the root, or after evaluating
//     the root. A ReductionNode (below) is the node that binds.
//   • bind() follows only the inputs each node reads at this point
//     (NodeBase::reduceReads()). A ConditionNode reads its condition and the
//     branch the condition selects, never the other, which can be stale. A
//     tweaked node reads nothing. Every other node of the engine reads every
//     input. An input a node does not read is never pulled, and a barrier
//     above it is not one.
//   • A node that reads no input an argument moves is a constant here, and so
//     is every node no argument reaches: a stateful node, a source. The
//     reduction holds the value each one published, until the next bind().
//   • A node that an argument does move must give a step, or bind() throws
//     std::domain_error, naming the argument, the root and the node. The value
//     through a barrier is unknown, and the value the graph holds would be a
//     silently wrong answer. A node that does not say what it reads is taken
//     to read every input, so it is refused even when the input the argument
//     moves contributes nothing.
//   • A node whose deciding input an argument moves (ReduceReads::decides()),
//     which is a ConditionNode whose condition an argument moves, CHOOSES at
//     each call, by the trial values. So it reads every input it has, and
//     bind() follows the inputs the graph did not read as well. Each of those
//     must be clean, or bind() throws std::domain_error, naming it: a
//     reduction evaluates nothing, and the value above an input the graph did
//     not read is the graph's only where something else has evaluated it at
//     this point. Give such an input to the node that binds as an input of its
//     own (ReductionNode), or evaluate it. A call then runs the steps of every
//     branch, as the graph evaluates every branch at its own point, and the
//     choosing node's step selects. A barrier on a branch the graph did not
//     take, with an argument above it, is a barrier here.
//   • Any node can be an argument, not only a leaf. The function is then what
//     the root would hold had that node's value been x, and the node's own
//     inputs are not read. An argument the roots do not depend on at this
//     point is one the function ignores: a node of another graph, or one above
//     an input that is not read.
//   • An argument that a step reads must hold a double, or bind() throws
//     std::invalid_argument. The arguments and the roots are doubles. The nodes
//     between them hold any type.
//   • A bind() that throws leaves the reduction unbound, and a call on an
//     unbound reduction throws std::logic_error.
//   • A step applies its node's function and no equality policy. Under the
//     default policy the function at the arguments' own values is the root's
//     value, to the bit. Under a tolerance policy a published value can lag
//     its inputs, and the two can differ by less than the tolerance.
//   • A reduction holds the values it bound until the next bind(), which pins
//     one of each producer's two buffers (ValueSlot). Bound in each cycle, that
//     costs nothing. Left bound while the graph moves, a producer allocates
//     once or twice and then has two buffers again.
//   • Eval thread only, like eval().
//
// The derivatives
// ───────────────
// gradient() and jacobian() run the call, ask the steps for their local
// partials at the values the call left (IReduceStep::partials()), and sweep
// them. The partials are the ones a tape uses (dag_aad.hpp), at values a tape
// cannot reach: an op's from ops::Derivative<Op>, a DifferentiableNode's from
// one run of its functor on duals, and 1 for the branch a ConditionNode
// selects.
//
//   • A step is asked only where the derivative passes: a root's step, and the
//     step of each input that the partials of such a step name. That is the
//     rule a tape records by. So the node that computes a condition is never
//     asked, and neither are the steps of a branch that a choosing node did not
//     select at this call.
//   • A partial times an adjoint or a tangent counts as 0 when either one is
//     0, as on a tape. So neither an infinite partial under an adjoint of 0
//     nor a NaN that nothing depends on reaches the result: z·√x at z = 0 and
//     x = 0 has ∂/∂x = 0. A NaN on a path the result depends on is kept.
//   • A step that is asked and has no partials makes the call throw
//     std::domain_error, naming the argument, the root and the node. The step
//     of a ComputeNode, an InPlaceComputeNode, a TweakableComputeNode or a
//     MemoizedComputeNode has none, because its functor is opaque, and neither
//     has an op over a type other than double. Calls still work, so such a
//     reduction serves a solver that needs no derivative.
//   • The derivative is with respect to the arguments' own values. An argument
//     a root does not depend on at the bound point has derivative 0, and so
//     has one above a tweaked node, through that node.
//   • jacobian() asks each step once, keeps the partials, and sweeps them once
//     per root in reverse, or once per argument forward when there are fewer
//     arguments than roots. The two agree, except where a product of partials
//     overflows or underflows taken from one end and not from the other.
//   • Under the default equality policy the gradient at the arguments' own
//     values is a tape's, to a few units in the last place: the two take their
//     partials from the same code, inlined at two sites.
//   • Once warm, a gradient and a Jacobian allocate nothing.
//
// Never include dag_reduce.inl directly — always include this file.

#include "dag.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dag {

// ─────────────────────────────────────────────────────────────────────────────
// Reduction — some roots as a function of args, at the point the graph held
// when it was last bound. The rules are at the top of this file.
//
// Move-only: it owns its steps and their buffers. A Reduction that has been
// moved from is good only for assignment and destruction.
// ─────────────────────────────────────────────────────────────────────────────
class Reduction {
public:
    /// The structure: walks up from root once, and reads no value. Throws
    /// std::invalid_argument for a null root, no arguments, or a null or
    /// repeated argument. The reduction is unbound until bind().
    Reduction(NodePtr root, std::vector<NodePtr> args);

    /// The same, for several roots: a function ℝⁿ → ℝᵐ. Throws
    /// std::invalid_argument for no roots or a null one as well. A root may be
    /// named twice, and is then two equal values.
    Reduction(std::vector<NodePtr> roots, std::vector<NodePtr> args);

    Reduction(Reduction&&) noexcept = default;
    Reduction& operator=(Reduction&&) noexcept = default;
    Reduction(const Reduction&) = delete;
    Reduction& operator=(const Reduction&) = delete;
    ~Reduction() = default;

    /// Binds to the point the graph holds: reads every value that no argument
    /// moves. Throws std::invalid_argument unless every root is clean and
    /// holds a double and every argument a step reads holds one, and
    /// std::domain_error for a barrier, and for an input the graph did not
    /// read, of a node that an argument makes choose, that is dirty. A bind
    /// that throws leaves the reduction unbound.
    void bind();

    /// The root's value, had the arguments held x, one value per argument in
    /// their order. Throws std::logic_error on an unbound reduction and on a
    /// reduction of several roots, and std::invalid_argument unless x has
    /// arity() values.
    double operator()(std::span<const double> x);
    double operator()(std::initializer_list<double> x);

    /// Every root's value, had the arguments held x, written to y in the
    /// roots' order. Throws std::logic_error on an unbound reduction, and
    /// std::invalid_argument unless x has arity() values and y roots() slots.
    void operator()(std::span<const double> x, std::span<double> y);
    void operator()(std::initializer_list<double> x, std::span<double> y);

    /// The root's value, with its derivative with respect to argument i
    /// written to g[i]: 0 for an argument the root does not depend on at the
    /// bound point. Throws what the one-root call throws,
    /// std::invalid_argument unless g has arity() slots, and std::domain_error
    /// when a step the derivative passes through has no partials.
    double gradient(std::span<const double> x, std::span<double> g);
    double gradient(std::initializer_list<double> x, std::span<double> g);

    /// Every root's value, written to y, and the Jacobian, written to j one
    /// row per root: j[i·arity() + k] is the derivative of root i with respect
    /// to argument k, and 0 where root i does not depend on argument k at the
    /// bound point. Throws what the call throws, std::invalid_argument unless
    /// j has roots()·arity() slots, and std::domain_error when a step a
    /// derivative passes through has no partials.
    void jacobian(std::span<const double> x, std::span<double> y, std::span<double> j);
    void jacobian(std::initializer_list<double> x, std::span<double> y, std::span<double> j);

    /// The value argument i itself holds at the bound point: where a solver
    /// starts. Throws std::logic_error on an unbound reduction,
    /// std::out_of_range for an argument the reduction does not have, and
    /// std::domain_error for one the roots do not depend on at the bound
    /// point, whose value bind() does not read.
    double argument(std::size_t i) const;

    /// The number of arguments.
    std::size_t arity() const noexcept;

    /// The number of roots.
    std::size_t roots() const noexcept;

    /// The steps a call runs at the bound point. 0 while unbound.
    std::size_t size() const noexcept;

private:
    // A ReductionNode binds the roots it has just pulled, through rebind().
    template<typename Out, typename... Ins>
    friend class ReductionNode;

    /// What a node is to the function. An Argument's value comes from the
    /// caller. A Candidate has an argument above it, so at some point it may be
    /// a step. A Constant has none: it is read from the graph, and only as an
    /// input of a candidate or as a root.
    enum class Role : std::uint8_t { Argument, Candidate, Constant };

    struct Entry {
        NodePtr                      node;
        Role                         role;
        std::size_t                  argument;     ///< an Argument's position in args_
        std::size_t                  firstInput;   ///< a Candidate's inputs are
        std::size_t                  lastInput;    ///<   inputs_[firstInput, lastInput)
        std::shared_ptr<NodeBase>    hooks;        ///< a Candidate, as the NodeBase it is, or null
        std::unique_ptr<IReduceStep> step;         ///< a Candidate's step, or null
    };

    /// One partial of one step, as a sweep reads it.
    struct Edge {
        std::size_t input;   ///< the named input's position in entries_
        double      d;       ///< ∂step/∂input
    };

    /// No position: an argument the roots do not reach, a node no argument moves.
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    static bool holdsDouble(const ValuePtr& v) noexcept;
    /// Why bind() cannot call entries_[barrier], which argument `by` moves.
    std::string barrierMessage(std::size_t barrier, std::size_t by) const;
    /// Why bind() cannot follow entries_[chooser], whose input inputs_[stale]
    /// the graph did not read and nothing has evaluated.
    std::string staleMessage(std::size_t chooser, std::size_t stale) const;
    /// Why `binder` cannot bind a point that follows entries_[unheard], which
    /// it was not given as an input.
    std::string unheardMessage(std::size_t unheard, const std::string& binder) const;
    /// "root r", or "roots r, s and t".
    std::string rootNames() const;

    /// Forgets the bound point, and lets go of every value it held.
    [[gnu::always_inline]] void release() noexcept;
    /// bind() without the clean check, for roots the caller has just pulled.
    /// An always-dirty node that reaches a root by two paths leaves it dirty
    /// after its pull, and the pulls below then evaluate what went dirty
    /// again, as they evaluate any always-dirty node they meet.
    [[gnu::always_inline]] void rebind();
    /// Throws std::domain_error unless every input the bound point follows
    /// although the graph did not read it (followed_) is an argument or one of
    /// `heard`, the inputs of the node `binder` that binds this reduction. The
    /// function reads such an input, and the root does not hear it, so a node
    /// that is not given it would keep a value the function has moved from.
    void requireHeard(std::span<const NodePtr> heard, const std::string& binder) const;
    /// One reading of the point: what the roots depend on, with every node of
    /// choosing_ reading all its inputs, and which of that an argument moves.
    /// True when it found a node that an argument makes choose and choosing_
    /// did not hold: the reading is then incomplete, and must be taken again.
    /// Following is false for the first reading, which is the only one unless
    /// it meets such a node: it then reads as a point with no choosing node is
    /// read, and stops at the node.
    template<bool Following>
    [[gnu::always_inline]] bool readPoint(EvalContext& ctx);
    /// The readings that follow a choice, kept out of line: a point with no
    /// choosing node never comes here.
    [[gnu::noinline, gnu::cold]] void readFollowing(EvalContext& ctx) {
        do {
            for (auto& v : values_) v.reset();
            run_.clear();
            live_.clear();
        } while (readPoint<true>(ctx));
    }
    /// The checks every call makes, and the steps.
    [[gnu::always_inline]] void run(std::span<const double> x);
    /// Starts a derivative pass: nothing is wanted yet.
    void nextPass();
    /// Asks each step the derivative passes through for its partials, and
    /// keeps them in edges_.
    void gatherPartials();
    /// One reverse sweep of edges_ from entries_[root]: adjoints_.
    void sweepReverse(std::size_t root);
    /// One forward sweep of edges_ from entries_[seed]: adjoints_, as tangents.
    void sweepForward(std::size_t seed);
    /// gradient()'s sweep with the marks: it asks only the steps the
    /// derivative passes through.
    void sweepMarked();
    /// gradient() by the marked sweep, from here on at this bound point.
    [[gnu::noinline, gnu::cold]] double gradientMarked(double value, std::span<double> g) {
        precise_ = true;
        sweepMarked();
        for (std::size_t k = 0; k < args_.size(); ++k)
            g[k] = argPosition_[k] == kNone ? 0.0 : adjoints_[argPosition_[k]];
        return value;
    }

    std::vector<NodePtr> roots_;
    std::vector<NodePtr> args_;

    // ── The structure, fixed at construction ─────────────────────────────────
    /// The arguments the roots reach, the candidates, and the constants a
    /// candidate names, each after every input it names.
    std::vector<Entry>       entries_;
    /// Every candidate's inputs, in inputs() order, as positions in entries_.
    std::vector<std::size_t> inputs_;
    /// Each root's position in entries_.
    std::vector<std::size_t> rootPosition_;
    /// Each argument's position in entries_, or kNone.
    std::vector<std::size_t> argPosition_;

    // ── The point, rewritten by each bind() and sized at construction ────────
    /// One per entry: what it holds at the bound point, for the steps to read.
    /// An argument's and a step's are rewritten by each call. A constant's is
    /// read at bind(). The rest stay null.
    std::vector<ValuePtr>         values_;
    std::vector<ReduceReads::Use> uses_;      ///< one per inputs_: what the candidate reads
    std::vector<std::uint8_t>     reached_;   ///< one per entry: a root depends on it here
    std::vector<std::size_t>      movedBy_;   ///< one per entry: an argument that moves it, or kNone
    /// One per entry: the argument that moves its deciding input, which makes
    /// it choose at each call, or kNone.
    std::vector<std::size_t>      choosing_;
    bool                          chose_ = false;   ///< choosing_ holds a mark
    /// One per entry: its step reads its node at bind() (IReduceStep::binds()).
    std::vector<std::uint32_t>    binds_;
    bool                          anyBinds_ = false;
    /// The inputs of the nodes of choosing_ that the graph did not read, and
    /// the bound point follows, as positions in entries_.
    std::vector<std::size_t>      followed_;
    std::vector<std::size_t>      run_;       ///< the candidates a call runs, in order
    std::vector<std::size_t>      live_;      ///< the arguments a call writes
    std::vector<double>           own_;       ///< one per argument: the value it held at bind()
    std::vector<ValuePtr>         rootValue_; ///< one per root, while bind() reads the point
    bool                          bound_ = false;
    bool                          single_ = true;    ///< one root
    std::size_t                   root0_ = kNone;    ///< the first root's position in entries_
    NodePtr                       root0Node_;         ///< the first root

    /// One per argument: where a call publishes its value.
    std::vector<ValueSlot<double>> argValues_;

    // ── A derivative's working storage, kept for its capacity ────────────────
    std::vector<double>       adjoints_;   ///< one per entry: an adjoint, or a tangent
    aad::Partials             partials_;   ///< the step being asked
    /// One per entry: the derivative pass that last passed through it. A stamp
    /// and not a flag, so that nothing is cleared between passes, and not a
    /// byte, a store through which would make the compiler reload everything
    /// else it holds.
    std::vector<std::uint32_t> wanted_;
    std::uint32_t              pass_ = 0;   ///< the current derivative pass
    /// The bound point has a step the derivative may not pass through, so a
    /// gradient marks what it passes through and asks only that.
    bool                       precise_ = false;
    std::vector<Edge>         edges_;      ///< the partials of the steps asked
    /// One per step of run_: its partials are edges_[firstEdge_[r], lastEdge_[r]).
    std::vector<std::size_t>  firstEdge_;
    std::vector<std::size_t>  lastEdge_;
};

/// A Reduction of root over args, bound to the point the graph holds. Throws
/// what the constructor and bind() throw.
Reduction reduce(NodePtr root, std::vector<NodePtr> args);

/// The same, for several roots.
Reduction reduce(std::vector<NodePtr> roots, std::vector<NodePtr> args);

// ─────────────────────────────────────────────────────────────────────────────
// ReductionNode<Out, Ins...> — a node that owns a Reduction, binds it each time
// it recomputes, and hands it to a functor that returns the node's value: a
// solver as a node.
//
//   auto implied = ReductionNode<double, double>::make(
//       "implied vol", price, {vol}, {quote},
//       [](Reduction& f, const double& target) {
//           double v = f.argument(0), vega = 0.0;       // from the vol the graph holds
//           for (int i = 0; i < 20; ++i) {
//               const double miss = f.gradient({v}, {&vega, 1}) - target;
//               if (std::abs(miss) < 1e-12) break;
//               v -= miss / vega;
//           }
//           return v;
//       });
//
// Its inputs are its roots, in order, and then the inputs given as `ins`, whose
// values the functor is handed, as a ComputeNode's functor is. The arguments
// are not inputs: one the roots depend on is upstream of a root, which carries
// its changes, and pulling one they do not depend on would evaluate a node the
// roots do not read.
//
// It is Eager, fixed here. The function it is handed depends on every value
// the reduction reads, and it declares none of them as inputs. A change
// upstream of a root marks it at least Maybe, and an Eager node recomputes
// then. A Lazy node would skip whenever the roots' values stood still, and the
// point can move while they do: a·x + b is 2 at x = 2 for (a, b) = (2, −2) and
// for (4, −6), and its zero is not where it was.
//
// Each time it recomputes, it pulls its roots and its other inputs, binds the
// reduction, and runs the functor. It binds the roots it has just pulled,
// clean or not (Reduction::rebind()). An always-dirty node that reaches a root
// by two paths leaves the root dirty after its own pull, and requiring it clean
// would make this node throw on every evaluation. It stays dirty when a root
// goes dirty again during its evaluation, as any node does when an input it
// read does (NodeBase::endEval()).
//
// eval() throws what the bind throws and what the functor throws, and the node
// then stays dirty and keeps the value it had, so the next eval() tries again.
// What a solver that does not converge publishes is the functor's to say: it
// returns the value it wants published, an Out that can say "none" among them,
// or it throws.
//
// To follow a ConditionNode whose condition an argument moves, give the node
// both of its branches as inputs: it then pulls the one the graph did not take,
// which a reduction needs evaluated and never evaluates itself, and it hears
// that branch move, which its root does not. The node insists on it: where the
// bound point follows an input the graph did not read, and the node was not
// given that input, eval() throws std::domain_error, even if something else
// had evaluated it. An argument is the exception: the function takes its value
// from the caller.
//
// TWO FUNCTOR SHAPES. make() takes a functor that returns Out. makeInPlace()
// takes one that writes into `out`, under InPlaceComputeNode's contract: `out`
// arrives holding the previous evaluation's value, and the functor overwrites
// everything it owns on every path. An Out that owns heap, such as the
// std::vector<double> of a solution in several unknowns, then allocates
// nothing once warm.
//
// It gives a Reduction no step and a tape no partials: its value is not a
// function of its inputs' values alone.
// ─────────────────────────────────────────────────────────────────────────────
template<typename Out, typename... Ins>
class ReductionNode
    : public NodeBase
    , public std::enable_shared_from_this<ReductionNode<Out, Ins...>>
{
public:
    using Fn         = std::function<Out(Reduction&, const Ins&...)>;
    using InPlaceFn  = std::function<void(Out& out, Reduction&, const Ins&...)>;
    using InputTuple = std::tuple<std::conditional_t<true, NodePtr, Ins>...>;

    /// A node over one root. Throws what Reduction's constructor throws. The
    /// default policy is TypedEqualityPolicy<Out>.
    static std::shared_ptr<ReductionNode> make(
        std::string name, NodePtr root, std::vector<NodePtr> args, InputTuple ins, Fn fn,
        EqualityPolicyPtr eq = nullptr);
    /// A node over several roots.
    static std::shared_ptr<ReductionNode> make(
        std::string name, std::vector<NodePtr> roots, std::vector<NodePtr> args, InputTuple ins,
        Fn fn, EqualityPolicyPtr eq = nullptr);

    /// The same two, for a functor that writes into `out`.
    static std::shared_ptr<ReductionNode> makeInPlace(
        std::string name, NodePtr root, std::vector<NodePtr> args, InputTuple ins, InPlaceFn fn,
        EqualityPolicyPtr eq = nullptr);
    static std::shared_ptr<ReductionNode> makeInPlace(
        std::string name, std::vector<NodePtr> roots, std::vector<NodePtr> args, InputTuple ins,
        InPlaceFn fn, EqualityPolicyPtr eq = nullptr);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    /// The roots, in order, and then the inputs given as `ins`.
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::Compute; }

private:
    ReductionNode(std::string name, std::vector<NodePtr> roots, std::vector<NodePtr> args,
                  InputTuple ins, InPlaceFn fn, EqualityPolicyPtr eq);

    /// Pull the other inputs, bind, invoke fn_ into scratch_, publish. The same
    /// two paths as ComputeNode::applyInputs, chosen the same way.
    template<std::size_t... Is>
    ValuePtr applyInputs(EvalContext& ctx, std::index_sequence<Is...>);
    ValuePtr publish();
    template<std::size_t... Is>
    void collectInputs(std::vector<NodePtr>& out, std::index_sequence<Is...>) const;

    std::string          name_;
    std::vector<NodePtr> roots_;
    /// The roots and then the other inputs: what inputs() returns, and what the
    /// node hears.
    std::vector<NodePtr> heard_;
    InputTuple           inputs_;
    InPlaceFn            fn_;
    EqualityPolicyPtr    eq_;
    Reduction            reduction_;
    ValuePtr             cached_;
    /// The retained output buffer, as InPlaceComputeNode's.
    Out                  scratch_{};
    ValueSlot<Out>       slot_;
};

template<typename Out, typename... Ins>
using ReductionNodePtr = std::shared_ptr<ReductionNode<Out, Ins...>>;

} // namespace dag

#include "dag_reduce.inl"
