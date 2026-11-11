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
//   1. Construct, once. One walk up from the root finds every node an argument
//      reaches, in topological order, and asks each for a step
//      (IReducible::reduceStep(), dag.hpp). A graph is not rewired once it is
//      built, so the walk is never repeated. It reads no value, so a reduction
//      can be built while a graph is being wired, before anything is evaluated.
//   2. bind(), once per point. It follows what the root depends on at the
//      values the graph holds now, decides which of those nodes an argument
//      moves here, and reads the value of every node they read that no
//      argument moves.
//   3. Call, many times. The arguments' values go into the reduction's own
//      buffers, each step runs in order, and the root's value comes out.
//      Nothing in the graph is read or written.
//
// A bound reduction describes one point. Once a value it read at bind() moves,
// it still answers for the old point, and cannot tell: bind it again.
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
//   • A node of your own that implements IReducible.
//
// Any other node with an argument above it is a BARRIER, every dag::ts node
// and an Eager compute node among them.
//
// The rules
// ─────────
//   • The root must be clean and hold a double, or bind() throws
//     std::invalid_argument. A reduction reads what the graph holds and
//     evaluates nothing. Evaluating a dirty root for the caller would break an
//     engine: Engine::cycle() snapshots its outputs' dirty flags before it
//     evaluates them, so an output evaluated between cycles reads as clean,
//     and its callback misses the change. A clean root's inputs are clean too,
//     so every value bind() pulls is a cached one. Bind in the root's output
//     callback, in a node that has just pulled the root, or after evaluating
//     the root.
//   • bind() follows only the inputs each node reads at this point
//     (IReducible::reduceReads()). A ConditionNode reads its condition and the
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
//     silently wrong answer. A node that is not an IReducible reads every
//     input, for all a reduction can tell, so it is refused even when the
//     input the argument moves contributes nothing.
//   • A node whose deciding input an argument moves (ReduceReads::decides()),
//     which is a ConditionNode whose condition an argument moves, throws
//     std::domain_error too. Following the choice would need the values above
//     the branch the graph did not take, which the graph does not hold. A
//     piecewise function of an argument belongs inside one functor, where it
//     can compare.
//   • Any node can be an argument, not only a leaf. The function is then what
//     the root would hold had that node's value been x, and the node's own
//     inputs are not read. An argument the root does not depend on at this
//     point is one the function ignores: a node of another graph, or one above
//     an input that is not read.
//   • An argument that a step reads must hold a double, or bind() throws
//     std::invalid_argument. The arguments and the root are doubles. The nodes
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
// The gradient
// ────────────
// gradient() runs the call, asks each step for its local partials at the values
// the call left (IReduceStep::partials()), and sweeps them once in reverse. The
// partials are the ones a tape uses (dag_aad.hpp), at values a tape cannot
// reach: an op's from ops::Derivative<Op>, a DifferentiableNode's from one run
// of its functor on duals, and 1 for the branch a ConditionNode selects.
//
//   • A partial times an adjoint counts as 0 when either one is 0, as on a
//     tape. So neither an infinite partial under an adjoint of 0 nor a NaN
//     that nothing depends on reaches the result: z·√x at z = 0 and x = 0 has
//     ∂/∂x = 0. A NaN on a path the result depends on is kept.
//   • A step with no partials makes gradient() throw std::domain_error, naming
//     the argument, the root and the node. The step of a ComputeNode, an
//     InPlaceComputeNode, a TweakableComputeNode or a MemoizedComputeNode has
//     none, because its functor is opaque, and neither has an op over a type
//     other than double. Calls still work, so such a reduction serves a solver
//     that needs no derivative.
//   • The derivative is with respect to the arguments' own values. An argument
//     the root does not depend on at the bound point has derivative 0, and so
//     has one above a tweaked node, through that node.
//   • Under the default equality policy the gradient at the arguments' own
//     values is a tape's, to a few units in the last place: the two take their
//     partials from the same code, inlined at two sites.
//   • Once warm, a gradient allocates nothing.
//
// Never include dag_reduce.inl directly — always include this file.

#include "dag.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dag {

// ─────────────────────────────────────────────────────────────────────────────
// Reduction — root as a function of args, at the point the graph held when it
// was last bound. The rules are at the top of this file.
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

    Reduction(Reduction&&) noexcept = default;
    Reduction& operator=(Reduction&&) noexcept = default;
    Reduction(const Reduction&) = delete;
    Reduction& operator=(const Reduction&) = delete;
    ~Reduction() = default;

    /// Binds to the point the graph holds: reads every value that no argument
    /// moves. Throws std::invalid_argument unless the root is clean and holds a
    /// double and every argument a step reads holds one, and std::domain_error
    /// for a barrier. A bind that throws leaves the reduction unbound.
    void bind();

    /// The root's value, had the arguments held x, one value per argument in
    /// their order. Throws std::logic_error on an unbound reduction, and
    /// std::invalid_argument unless x has arity() values.
    double operator()(std::span<const double> x);
    double operator()(std::initializer_list<double> x);

    /// The same value, with its derivative with respect to argument i written
    /// to g[i]: 0 for an argument the root does not depend on at the bound
    /// point. Throws what a call throws, std::invalid_argument unless g has
    /// arity() slots, and std::domain_error when a step has no partials.
    double gradient(std::span<const double> x, std::span<double> g);
    double gradient(std::initializer_list<double> x, std::span<double> g);

    /// The number of arguments.
    std::size_t arity() const noexcept;

    /// The steps a call runs at the bound point. 0 while unbound.
    std::size_t size() const noexcept;

private:
    /// What a node is to the function. An Argument's value comes from the
    /// caller. A Candidate has an argument above it, so at some point it may be
    /// a step. A Constant has none: it is read from the graph, and only as an
    /// input of a candidate or as the root.
    enum class Role : std::uint8_t { Argument, Candidate, Constant };

    struct Entry {
        NodePtr                      node;
        Role                         role;
        std::size_t                  argument;     ///< an Argument's position in args_
        std::size_t                  firstInput;   ///< a Candidate's inputs are
        std::size_t                  lastInput;    ///<   inputs_[firstInput, lastInput)
        std::shared_ptr<IReducible>  reducible;    ///< a Candidate's mixin, or null
        std::unique_ptr<IReduceStep> step;         ///< a Candidate's step, or null
    };

    /// No position: an argument the root does not reach, a node no argument moves.
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    static bool holdsDouble(const ValuePtr& v) noexcept;
    /// Why bind() cannot call entries_[barrier], which argument `by` moves.
    std::string barrierMessage(std::size_t barrier, std::size_t by) const;

    NodePtr              root_;
    std::vector<NodePtr> args_;

    // ── The structure, fixed at construction ─────────────────────────────────
    /// The arguments the root reaches, the candidates, and the constants a
    /// candidate names, each after every input it names.
    std::vector<Entry>       entries_;
    /// Every candidate's inputs, in inputs() order, as positions in entries_.
    std::vector<std::size_t> inputs_;
    std::size_t              rootPosition_ = kNone;
    /// Each argument's position in entries_, or kNone.
    std::vector<std::size_t> argPosition_;

    // ── The point, rewritten by each bind() and sized at construction ────────
    /// One per entry: what it holds at the bound point, for the steps to read.
    /// An argument's and a step's are rewritten by each call. A constant's is
    /// read at bind(). The rest stay null.
    std::vector<ValuePtr>         values_;
    std::vector<ReduceReads::Use> uses_;      ///< one per inputs_: what the candidate reads
    std::vector<std::uint8_t>     reached_;   ///< one per entry: the root depends on it here
    std::vector<std::size_t>      movedBy_;   ///< one per entry: an argument that moves it, or kNone
    std::vector<std::size_t>      run_;       ///< the candidates a call runs, in order
    std::vector<std::size_t>      live_;      ///< the arguments a call writes
    bool                          bound_ = false;

    /// One per argument: where a call publishes its value.
    std::vector<ValueSlot<double>> argValues_;

    // ── A gradient's working storage, kept for its capacity ──────────────────
    std::vector<double> adjoints_;   ///< one per entry: ∂root/∂(its value)
    aad::Partials       partials_;   ///< the step being swept
};

/// A Reduction of root over args, bound to the point the graph holds. Throws
/// what the constructor and bind() throw.
Reduction reduce(NodePtr root, std::vector<NodePtr> args);

} // namespace dag

#include "dag_reduce.inl"
