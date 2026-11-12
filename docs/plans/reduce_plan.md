# A Subgraph as a Function: `dag::reduce()`

**Status: Approved 2026-11-11, in progress.** D5 and D9 are answered (2026-11-11). The Progress table says which step
the work is at, and the Outcome at the end what each finished step measured and where it departed from this plan.

Closes [flywheel-dag#55](https://github.com/tomlin256/flywheel-dag/issues/55).

`dag::reduce(root, args)` turns the nodes between some argument nodes and a root into a `dag::Reduction`: a function of
the arguments that a solver can call at any point, with a gradient, without moving the graph and, once warm, without
allocating. Everything no argument reaches is a constant, read from the graph when the reduction is bound.

**Not here:** a solver (a root finder or an optimiser is the application's); a node that owns a reduction (D6); several
roots (D3); a `ConditionNode` whose condition an argument moves (D5); a reduction through a stateful `dag::ts` node
(flywheel-dag#11 decides what that means); a forward-mode sweep; rebuilding `GradientNode` on a reduction (flywheel-dag#13).

## Problem

The issue asks for "a dag reduce api that converts a dag from input to output as a callable function", so that a subgraph
can be used in a solver. A solver asks one question many times: what would this output be, had these inputs been x?

Today the only way to ask is to move the graph: `set()` each input, `eval()` the output, read it, and put the inputs
back. That is wrong inside an engine, for four reasons, and the probes below show the first two:

- Every trial evaluates the live nodes. A stateful node between the input and the output ticks once per trial, and every
  functor on the way runs its side effects.
- A node that moves an input above itself during its own evaluation hears the invalidation it caused, so it never ends
  clean, and an engine runs it again on every cycle.
- The arguments must be `Input`s. An `AsyncInput` or an intermediate node cannot be set.
- `Input::set()` allocates, wakes the engine and invalidates every consumer of the input, in the subgraph or not.

The milestone asks for more than a callable: "a subgraph used as a function that can be called, differentiated and run
without allocating". A tape (`dag_aad.hpp`) differentiates at the values the graph holds and nowhere else, so a
solver that wants a derivative at a trial point has none.

**Done when** a test reduces a graph of ops, differentiable nodes and `Lazy` compute nodes over one of its inputs, and:
the function's value at any x is, bit for bit, what the root holds once the input is set to x and the root evaluated; its
gradient there is what a tape gives; calling it leaves every node as it was; a warm call, gradient and bind allocate
nothing; and Newton's method on it finds a root inside an engine cycle.

## What I checked (2026-11-11, read-only; a scratch prototype outside the checkout)

A prototype on `6bf1442`: a copy of `include/` with the contract, the steps of every node class below, and
`dag_reduce.hpp` with `dag_reduce.inl`; a scratch test of 24 cases; and three harnesses. Built with the tree's own flags
(`-O2 -g -DNDEBUG -std=gnu++23 -Wall -Wextra -Wpedantic -Werror`).

- **It works, on three compilers.** Apple clang 21.0.0: no warning, 24 of 24. GCC 13.3.0 (`ubuntu-dev`, aarch64) and GCC
  15.2.0 (CI's compiler, a throwaway `ubuntu:26.04`, aarch64): no warning, 21 of 21 (the last three cases were written
  after those runs). `dag_reduce.hpp` compiles as a unit's only include on all three. That check caught a missing
  `<algorithm>`, which the test had hidden by including other headers first.
- **Values are the graph's, to the bit.** On each of the six graphs in `tests/aad_test_graphs.hpp`, reduced over all its
  inputs, f(x) equals the root's value at the point the graph holds and at a moved point, once the inputs are set there.
  The gradient equals `aad::adjoints()` at both points, also bit for bit on these three compilers (the tests will hold
  the gradient to 1e-13, for the reason in the self-review).
- **The existing suites do not notice.** All 30 of `tests/test_*.cpp` build and pass against the patched headers (Apple
  clang). Nine of them were also built and run under GCC 13.3 and pass, and seven compile under GCC 15.2 without a
  warning. `bench_hot_path`'s exact columns are those of `benchmarks/expected_invariants.txt` in all 240 runs below.
- **Two hand-made changes, to check two rows of the tables below before promising them.** A `bind()` that keeps the
  last point's values fails only the case that counts the references to a value the new point no longer reads; the
  engine-cycle allocation case stays green, so it is not that change's test. A dual helper that seeds every argument
  with one unit vector fails only a case whose gradient goes through a three-input `DifferentiableNode`; the comparison
  with a tape stays green, because the tape shares the helper, and so does the Black–Scholes price, whose derivative
  through d1 is 0.
- **clang-tidy 23.1.4**, with `.clang-tidy` and the gate's arguments: no finding in the prototype's headers (seven in the
  scratch test itself, which the real suite will not copy: a by-value parameter and a `push_back` in a loop).
- **What it costs to call.** Apple M4 Pro, the Black–Scholes call of `tests/test_aad_node.cpp` (17 nodes), best of nine
  loops of 200,000, three runs:

  | | ns |
  |---|---|
  | The formula as one plain function | 14 to 16 |
  | The graph: `vol->set(v)`, then `call->eval()` | 188 to 194 |
  | The reduction over vol (8 steps): `f(v)` | 64 to 70 |
  | The same, with its derivative: `f.gradient(v)` | 228 to 234 |
  | The reduction over all five inputs (12 steps): `f(x)` | 96 to 103 |
  | The same, with all five derivatives | 319 to 332 |
  | `bind()`, over vol and over all five | 130 to 132, 171 to 173 |
  | A tape recorded and swept once in reverse, for the same five | 3,094 to 3,276 |
  | Constructing the reduction over vol (once) | about 2,300 |

  A step costs about 8 ns. A warm call, gradient and bind allocate nothing (counted with `tests/allocation_counter.cpp`),
  and neither does the first bind of a reduction built without one. An engine cycle whose node binds a reduction and
  runs six Newton steps on it allocates nothing either, in the reduction or in the nodes whose values it holds.
- **What the mixin costs the hot path (D2).** Each class that joins grows by one vtable pointer: `ComputeNode<double,
  double>` 200 to 208 bytes, `ops::ProductNode<>` 184 to 192, `aad::DifferentiableNode<1>` 240 to 248, `ConditionNode` 208
  to 216. No `eval()` changes. `bench_hot_path`'s rows, built from both sets of headers behind a pad of 0, 2, … 14
  nops (eight alignments each), each binary run 15 times in shuffled order, each run the median of 7 timings in a warm
  process, paired by where `Engine::cycle()` lands (ns/cycle, patched / today's):

  | `cycle()` mod 64 | `chain` | `idle-queues` | `ingest` |
  |---|---|---|---|
  | 4 | 267.05 / 268.44 | 231.33 / 233.29 | 124.16 / 122.62 |
  | 12 | 268.33 / 268.25 | 232.82 / 232.39 | 122.50 / 123.45 |
  | 20 | 269.46 / 266.90 | 232.70 / 232.76 | 123.39 / 122.86 |
  | 28 | 267.53 / 266.93 | 232.90 / 231.07 | 122.40 / 122.78 |
  | 36 | 271.47 / 268.56 | 233.90 / 233.24 | 123.27 / 123.01 |
  | 44 | 268.84 / 266.18 | 232.89 / 230.76 | 123.88 / 122.47 |
  | 52 | 267.67 / 268.25 | 233.00 / 230.78 | 123.77 / 124.39 |
  | 60 | 268.43 / 268.51 | 231.43 / 232.10 | 122.52 / 122.80 |

  `chain` moves by −0.5% to +1.1% and `ingest` by −0.8% to +1.3%, with both signs. `idle-queues` holds no class that
  changed and moves by −0.8% to +1.0%, and today's headers alone spread 0.9% to 1.6% across the alignments. So the cost
  is inside the layout noise: unmeasured, not shown to be zero. Step 2 repeats it on the final code at all 16
  alignments.
- **Moving the graph instead** (the repo's own headers). Three trial values set on an `Input` and put back left an
  `EWMANode` below it at 2.0625, where a control holds 1. A compute node that sets an input above itself, evaluates and
  restores it was still dirty after its own evaluation, and ran on each of three engine cycles in which nothing moved.

## The design

### The function

```cpp
#include <flywheel/dag_reduce.hpp>

EvalContext ctx;
price->eval(ctx);                               // a reduction reads a clean root

dag::Reduction f = dag::reduce(price, {vol});   // price as a function of vol
f({0.30});                                      // the price, had vol been 0.30
double vega = 0.0;
f.gradient({0.30}, {&vega, 1});                 // the same, and ∂price/∂vol there

spot->set(101.0);                               // the point moves,
price->eval(ctx);
f.bind();                                       // so read it again
```

```cpp
namespace dag {

class Reduction {   // move-only
public:
    /// The structure: walks up from root once, and reads no value. Throws std::invalid_argument
    /// for a null root, no arguments, or a null or repeated argument.
    Reduction(NodePtr root, std::vector<NodePtr> args);

    /// Binds to the point the graph holds: every value that no argument moves.
    void bind();

    /// The root's value, had the arguments held x.
    double operator()(std::span<const double> x);
    double operator()(std::initializer_list<double> x);

    /// The same, with its derivative with respect to x[i] written to g[i].
    double gradient(std::span<const double> x, std::span<double> g);
    double gradient(std::initializer_list<double> x, std::span<double> g);

    std::size_t arity() const noexcept;   ///< the number of arguments
    std::size_t size() const noexcept;    ///< the steps a call runs at the bound point
};

/// A Reduction of root over args, bound to the point the graph holds.
Reduction reduce(NodePtr root, std::vector<NodePtr> args);

} // namespace dag
```

Three phases, each done as rarely as it can be:

1. **Construct, once.** One walk up from the root finds every node an argument reaches (a *candidate*), in topological
   order, and asks each for a *step*. A graph is not rewired once built (`INode::inputs()`), so this is never repeated.
   It reads no value, so a reduction can be built while a graph is being wired, before anything is evaluated.
2. **`bind()`, once per point.** It follows the root's dependence at the values the graph holds now, decides which
   candidates an argument moves at this point, and reads the value of every node they read that no argument moves.
3. **Call, many times.** The arguments' values go into the reduction's own buffers, each step runs in order, and the
   root's value comes out. Nothing in the graph is read or written.

A bound reduction describes one point, as a tape does. Once a value it bound moves, bind it again.

### A step, and the nodes that have one

A step is one node's function, applied to values the reduction hands it. It publishes through a `ValueSlot` of its own,
so the node's `cached_`, its slot and its dirty state are not touched, and a warm step allocates nothing. Values pass
between steps as `ValuePtr`s, so the nodes between the arguments and the root hold any type. Only the arguments and the
root are doubles.

```cpp
namespace dag {

class IReduceStep {
public:
    virtual ~IReduceStep() = default;
    /// The node's value at these input values, one per input in inputs() order.
    virtual ValuePtr apply(const ReduceInputs& in) = 0;
    /// The node's local partials at these values. False, the default: it cannot say.
    virtual bool partials(const ReduceInputs& in, aad::Partials& out);
};

class IReducible {   // a mixin, like ITweakable, IStatefulNode and aad::IDifferentiable
public:
    virtual ~IReducible() = default;
    /// A step that applies this node's function, or null when nothing says its functor may be
    /// called at other values: an Eager compute node.
    virtual std::unique_ptr<IReduceStep> reduceStep() = 0;
    /// The inputs this node's value depends on at the values the graph holds now. The default
    /// is every input.
    virtual void reduceReads(EvalContext& ctx, ReduceReads& out);
};

} // namespace dag
```

`ReduceInputs` is a view of the reduction's values, one per input. `ReduceReads` takes `reads(i)`, and `decides(i)` for
an input whose value chose which inputs are read. Both live in `dag.hpp` with the two interfaces, because the compute
classes implement them. An application's own node joins by implementing `IReducible`.

A reduction calls a functor only where a contract the author has already accepted makes that safe:

| Node | Has a step | Why its function may be called at other values |
|---|---|---|
| Every `dag::ops` node | always | Structurally pure: `Op{}` is default-constructed at each evaluation |
| `aad::DifferentiableNode<N>` | always | Its contract: the functor has no side effects. A tape already runs it again |
| `ConditionNode` | always | Selection, with no functor |
| `MemoizedComputeNode` | always | Its contract: the functor is a pure function. The step calls the functor, not the cache, which trial values would fill |
| `ComputeNode`, `InPlaceComputeNode`, `TweakableComputeNode` | when `Lazy` | `Lazy` is the author's statement that the functor is a pure function of its inputs. `Eager` says nothing, and is what a functor with captured state must be (D1) |
| Every `dag::ts` node, `GradientNode`, `TangentNode`, a node of your own without the mixin | never | A barrier |

### What `bind()` does

- **The root must be clean and hold a double**, or it throws `std::invalid_argument`. A reduction reads what the graph
  holds and evaluates nothing, for the tape's reason: an output evaluated between cycles reads as clean at the engine's
  next snapshot, and its callback misses the change. A clean root's inputs are clean too, so every pull below returns a
  cached value. Bind in the root's output callback, in a node that has just pulled the root, or after evaluating it.
- **It follows only what each node reads at this point.** A `ConditionNode` reads its condition and the branch the
  condition selects, never the other, which may be stale. A tweaked node reads nothing. Every other node reads every
  input.
- **A candidate that reads no input an argument moves is a constant here**, and so is every node no argument reaches: a
  stateful node, an opaque node, a source. The reduction holds the `ValuePtr` each published, until the next bind.
- **A candidate that an argument does move must have a step**, or `bind()` throws `std::domain_error`, naming the
  argument, the root, the node and the reason: no `IReducible`, or `Eager` (with the way out: `InvalidationMode::Lazy`).
  As on a tape, the test is whether an argument is anywhere above the node, so it can refuse a path that contributes
  nothing.
- **A node whose deciding input an argument moves** (a `ConditionNode` with such a condition) throws `std::domain_error`
  (D5).
- **An argument that a step reads must hold a double**, or `std::invalid_argument`.
- A bind that throws leaves the reduction unbound, and a call on an unbound one throws `std::logic_error`.

### Arguments

Any node can be an argument, not only a leaf, as any node can be a tape's wrt node. The function is then what the root
would hold had that node's value been x, and the node's own inputs are not read. An argument the root does not depend on
at this point is one the function ignores, with derivative 0: a node of another graph, or one on the branch not taken.

### The function and the equality policies

A step applies its node's function to its inputs and nothing else. It does not apply an equality policy, which compares
with the last value published and so has a history. Under the default policy the function at the arguments' own values
is the root's value, to the bit. Under a tolerance policy a published value can lag its inputs, and the two can differ by
less than the tolerance. A tape's partials follow the same rule.

### The gradient

`gradient()` runs the call, asks each step for its partials at the values the call left, and sweeps once in reverse. The
ops take their partials from `ops::Derivative<Op>`, a `DifferentiableNode` from one call of its functor on duals, and a
`ConditionNode` gives 1 for its branch: the functions a tape uses, at values the tape cannot reach. A partial times an
adjoint is 0 when either is 0, as on a tape. A step with no partials (a `Lazy` compute node, an op over another type)
makes `gradient()` throw `std::domain_error`, and calls still work: such a reduction serves a solver that needs no
derivative. The edges, the adjoints and an n-ary op's two vectors are kept, so a warm gradient allocates nothing.

### In an engine

A solver in a node is an `Eager` compute node over the root, which binds in its functor:

```cpp
auto f = std::make_shared<dag::Reduction>(price, std::vector<NodePtr>{vol});
auto implied = ComputeNode<double, double, double>::make("implied vol", {quote, price},
    [f](const double& target, const double&) {
        f->bind();            // price has just been pulled, so it is clean
        double v = 0.2, vega = 0.0;
        for (int i = 0; i < 20; ++i) {
            const double err = f->gradient({v}, {&vega, 1}) - target;
            if (std::abs(err) < 1e-12) break;
            v -= err / vega;
        }
        return v;
    });   // Eager, the default: its value depends on nodes it does not declare
```

The root is an input, so everything the function depends on invalidates the node, and the root is clean when the functor
runs. It must be `Eager`, for `GradientNode`'s reason: the point can move while the root's value stands still. Step 5
holds this pattern with tests, and D6 is the node that would make it a class.

### What does not change

No `eval()` path. `INode` and `NodeBase` are untouched. The classes in the table gain a base class, and
`DifferentiableNode`'s dual call moves into a helper that its `partials()` and its step share. `bench_hot_path`'s three
rows keep their exact columns.

*Since Step 6a:* `INode` is untouched, `NodeBase` gains two virtuals, and no class gains a base or a byte.

## Decisions to confirm

D5 and D9 are answered below (2026-11-11). The other defaults stand if you approve the plan as written.

- **D1 `Lazy` is what lets a compute node's functor be called.** `docs/engine_rules.md` recommends a `ComputeNode` lambda
  with captured state as the first way to write a node, and one that mutates that state must be `Eager`. Called at trial
  values it would advance on every one, silently, which is the defect the stateful barrier exists to stop. An `Eager` node in the way is refused with a
  message that names it and says how to fix it. *Alternative:* call every compute node and document that its functor
  must be pure. A pure functor built with the default mode would then need no edit, and a stateful one would be
  corrupted.
- **D2 A mixin on the compute classes, at 8 bytes a node.** It is how the engine adds an optional capability, it leaves
  `INode` and `NodeBase` alone, and its cost on the hot path is inside the noise (above). *Alternative:* two virtuals with
  defaults on `NodeBase`. No object grows and the hot path's machine code is provably the same, but `NodeBase` gains a
  concern that is not the dirty protocol. Step 2 switches to it if the full sweep shows a cost.
  *Measured in Step 2 (2026-11-11, the Outcome has the table):* the mixin costs `chain` 0.37% on average over the 16
  alignments, about 1 ns in 266, between nothing and 1.1%, and a control shows that the 8 bytes are the cause. `ingest`
  and `idle-queues` show nothing. Step 2's rule asks for a slowdown at every alignment that exceeds what `idle-queues`
  moves, and finds one at 13 of 16, so by that rule the mixin stayed through Step 6. The alternative closes the
  cost. **Answered: take the alternative before the release (2026-11-12).** Step 6a moves the two hooks to
  `NodeBase` and removes `IReducible`, which changes D8's names. Where the sections above say `IReducible`, read
  "the two hooks of `NodeBase`": Step 6a has the contract as it now is.
- **D3 One root, and doubles at the boundary.** A solver's function is ℝⁿ → ℝ. Several roots from one reduction (a
  least-squares fit shares most of its subgraph between residuals) is a follow-up issue: until then, one reduction per
  root.
- **D4 The gradient is in this plan** (Step 4). The milestone asks for a function that can be differentiated, and a tape
  cannot do it away from the graph's own point. *Alternative:* values only, and the gradient as a follow-up.
- **D5 A `ConditionNode` whose condition an argument moves is refused. Answered: yes, refuse (2026-11-11).**
  Following it needs the values below the branch the graph did not take, which the graph does not hold, and getting
  them means evaluating live nodes. Every design I can see breaks the bar this plan sets (a reduction evaluates nothing
  in the graph), so I asked rather than parking it. A piecewise function of an argument belongs inside a
  `DifferentiableNode`'s functor, where a dual compares and takes `min` and `max`. A follow-up issue records it.
- **D6 No node of its own yet.** A node that owns a reduction, takes the root as its input, is `Eager` by construction
  and binds at each evaluation would make the pattern above a class, as `GradientNode` followed the tape. It is also
  what could bind a root that an always-dirty node leaves dirty, which the public `bind()` refuses. A follow-up issue.
- **D7 A benchmark row, `solve`** (Step 5): a cycle that binds a reduction and runs six Newton steps on x·x − a. Its
  `allocs/cycle=0` holds the reduction, and the nodes whose values it holds, in CI on both platforms. Its arithmetic is
  nothing but + − × ÷, so its checksum is the same everywhere. `benchmarks/expected_invariants.txt` gains one line.
- **D8 Names** (`dag::IReducible` is not among them since Step 6a: its two functions are `NodeBase`'s)**:**
  `dag::reduce()`, `dag::Reduction`, `dag::IReducible`, `dag::IReduceStep`, `dag::ReduceInputs`,
  `dag::ReduceReads`, in `dag_reduce.hpp` and `dag.hpp`, with the root first, as in `aad::adjoints(root, wrt)`.
- **D9 The release is v1.3.0, in Step 7. Answered: v1.3.0 (2026-11-11).** New public names make it a minor, and nothing
  a consumer calls changes. The node classes in the table grow by 8 bytes, which is read as no break, as with the two
  fixes that grew `Engine`: the engine is header-only, so nothing compiled against the old layout is left to link, and
  a pin bump rebuilds. v2.0.0, the number if a layout change of a public type counted as a major, is not taken.
  *Alternative:* leave the release until you ask, as for flywheel-dag#49.
- **D10 Pushes, comments and issues.** Each step is one commit on `main`, pushed once its checkpoint is green and not
  held for CI. Approving the plan approves those pushes, the comment on flywheel-dag#55 that links this plan, the three
  follow-up issues and the two comments of Step 6, and the tag and release of Step 7.

## Steps

Commits are `type(#N): …` with this issue's number, and carry no closing keyword: flywheel-dag#55 is closed by hand in
Step 6, after its comment. Every step is test first: the tests are written and seen red (for a new name, a build that
fails on it), then the code. A checkpoint is a full build and `ctest --preset relwithdebinfo-macos-arm64` with CI's flags
and the lint gate (`-Werror=author -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON
-DFLYWHEEL_DAG_CLANG_TIDY=clang-tidy`), no warning; the same build and `ctest` in `ubuntu-dev` (GCC 13.3); the new code
compiled by GCC 15.2 in a throwaway container; and `bench_hot_path --invariants` equal to the committed file. The
default Mac tree has 83 tests today. Each hand-made change is made on a scratch copy, one at a time, each test run alone
under a watchdog, with an unchanged control; a change that is not caught as stated is read before anything is edited.
Before each push the leak audit passes on the tree.

### Step 0 — Approval

*Tests:* none. Commit this plan (`docs: add plan for a subgraph as a callable function`), push, and comment on
flywheel-dag#55 with the path. *Done when:* the issue and the plan link each other, D5 is answered, and you have said to
start.

### Step 1 — The contract, the ops, and a reduction's values, one commit

`dag.hpp` and `dag.inl`: `ReduceInputs`, `ReduceReads`, `IReduceStep`, `IReducible`. `dag_ops.hpp` and `dag_ops.inl`:
`OpNodeImpl` derives `IReducible`, and each arity template has a step that applies `Op{}`, the n-ary one folding in
`eval()`'s order. New `dag_reduce.hpp` and `dag_reduce.inl`: the constructor, `bind()`, the call, `arity()`, `size()`
and `reduce()`. New suite `tests/test_dag_reduce.cpp`, which links the allocation counter. Two nodes local to the suite
implement `IReducible`, so that everything `bind()` does with what a node reads is held here, before any engine node
uses it: `Chooser` (reads a deciding input and the input it selects) and `Counted` (a pure function that counts its
calls).

| Test (`Reduction.`) | Asserts |
|---|---|
| `MatchesTheGraphAtEveryPoint` | Each graph of `aad_test_graphs.hpp` over all its inputs: at the point the graph holds and at a moved one, f(x) is the root's value to the bit once the inputs are set to x |
| `AnNAryStepFoldsInItsNodesOrder` | A sum of 1e16, 1 and −1e16, whose value depends on the order: the function agrees with the node |
| `ACallLeavesTheGraphAsItWas` | After 100 calls every node is clean, every node's `ValuePtr` keeps its identity, and every input holds its value |
| `BindReadsThePointTheGraphHolds` | An input no argument reaches moves and the root is evaluated: the function is the old point's until `bind()`, and the new one's after |
| `AnyNodeCanBeAnArgument` | exp(x)·x over exp(x) is u·x and has one step; over both it is u·v; over the root itself it is the identity |
| `AnArgumentTheRootDoesNotDependOnIsIgnored` | A node of another graph as a second argument changes nothing; over it alone the reduction has no steps and returns the root's value |
| `ANodeNamedTwiceIsOneValue` | x·x and x − x over x |
| `RunsEachNodeOncePerCall` | A diamond over `Counted`: four steps, and one call of the shared node per call |
| `ANodeNoArgumentReachesIsAConstant` | An `EWMANode` over s, times x, over x: the function uses its value, and ten calls do not advance it (a control fed the same values agrees) |
| `ANodeThatCannotBeCalledIsABarrier` | An `EWMANode` over the argument: `std::domain_error`, and the message names the argument, the root and the node |
| `FollowsOnlyTheInputsANodeReads` | `Chooser`: the input it does not read stays dirty, is not a step, and a barrier on it is not one |
| `ADecidingInputAnArgumentMovesIsABarrier` | `Chooser` with a deciding input below the argument: `std::domain_error` |
| `ANodeThatReadsNothingIsAConstant` | `Chooser` reading no input: its value is read, and a barrier above it is never reached |
| `RejectsBadArguments` | A null root, no arguments, a null and a repeated argument: `std::invalid_argument` from the constructor. A call with the wrong number of values: `std::invalid_argument` |
| `RejectsADirtyRoot` | Never evaluated, and after an input moved: `std::invalid_argument`, and the root is still dirty |
| `RejectsARootOrAnArgumentThatDoesNotHoldADouble` | An `Input<int>` root, and an `Input<int>` argument that a step reads: `std::invalid_argument` from `bind()`, not `std::bad_cast` from a call |
| `IsUnboundUntilABindSucceeds` | Built on a graph never evaluated it evaluates nothing, and a call throws `std::logic_error`. A bind that throws leaves a bound reduction unbound |
| `AWarmCallAndBindAllocateNothing` | 100 calls and 100 binds of a warm reduction: 0 allocations. The first bind of one built without a bind: 0 |

| Change made by hand, then reverted | Must fail |
|---|---|
| `bind()` without the clean check | `RejectsADirtyRoot` |
| The walk follows every input, not only those read | `FollowsOnlyTheInputsANodeReads` |
| The deciding check dropped | `ADecidingInputAnArgumentMovesIsABarrier` |
| A candidate that reads nothing an argument moves is given a step all the same | `ANodeThatReadsNothingIsAConstant` |
| The walk goes on above an argument | `AnyNodeCanBeAnArgument` |
| A candidate with no `IReducible` is taken as a constant | `ANodeThatCannotBeCalledIsABarrier` |
| The n-ary step folds from the last input | `AnNAryStepFoldsInItsNodesOrder` |
| A call reads its constants from the graph, not from the bind | `BindReadsThePointTheGraphHolds` |
| A bind that throws leaves the reduction bound | `IsUnboundUntilABindSucceeds` |
| A repeated argument accepted | `RejectsBadArguments` |
| The argument's type not checked at `bind()` | `RejectsARootOrAnArgumentThatDoesNotHoldADouble` |
| An argument's value published with `make_value` | `AWarmCallAndBindAllocateNothing` |

*Done when:* red then green; the checkpoint passes at 84 tests; `dag_reduce.hpp` compiles as a unit's only include under
all three compilers; each change fails as stated; pushed. Commit: `feat: reduce a subgraph of ops to a function`.

*As built* (the Outcome has the results). Four tests more than the table, for code the table does not name:
`Reduction.MovesWithItsBinding`, for the move operations, and a suite for the two views a node and a step are handed,
`ReduceContract.InputsAreAViewOfTheReductionsValues`, `ReduceContract.ReadsRecordsWhatANodeNames` (a deciding input
stays one when the node then names it as read, and an input the node does not have is `std::out_of_range`) and
`ReduceContract.ANodeReadsEveryInputUnlessItSaysOtherwise`. `Counted` is a template over the type its input holds, so
that a step can read an int, and can be built to give no step, which is the second reason a node is a barrier.
`IsUnboundUntilABindSucceeds` makes its bind throw at a barrier, part of the way through, where the table said nothing
of how. `IReduceStep::partials()` arrives with the gradient in Step 4: until then nothing would call it, and no test
could hold it.

### Step 2 — Compute nodes, and which functors may be called, one commit

`dag.hpp` and `dag.inl`: `ComputeNode` and `InPlaceComputeNode` derive `IReducible`, with a step when `Lazy`; the
in-place step has a scratch of its own. `dag_aad.hpp` and `dag_aad.inl`: `DifferentiableNode`. `dag_memoize.hpp` and
`dag_memoize.inl`: `MemoizedComputeNode`. `InvalidationMode`'s comment says what `Lazy` now also allows.

| Test (`Reduction.`) | Asserts |
|---|---|
| `ALazyComputeNodeIsCalled` | Its value; its functor runs once per call; the node's own `ValuePtr` keeps its identity |
| `AnEagerComputeNodeIsABarrier` | `ComputeNode` and `InPlaceComputeNode`: `std::domain_error`, and the message says `Eager` and names `InvalidationMode::Lazy` |
| `NodesBetweenHoldAnyType` | A double argument, a `std::vector<double>` from an `InPlaceComputeNode`, a double root |
| `AnInPlaceStepHasAScratchOfItsOwn` | After calls, the node's own next evaluation gives its own value and allocates nothing; a warm call allocates nothing |
| `ACallLeavesANodesSlotAlone` | After calls, moving the node's input and evaluating it allocates nothing |
| `AMemoizedNodeIsCalledPastItsCache` | After 50 calls the cache's size and the node's hits and misses are what they were |
| `ADifferentiableNodeIsCalledAtDouble` | Calls run its functor on doubles and never on duals |
| `ACallPriceAsAFunctionOfVol` | The Black–Scholes graph over vol: 8 steps, and the graph's price at 0.30 to the bit |

| Change made by hand, then reverted | Must fail |
|---|---|
| `ComputeNode::reduceStep()` returns a step whatever the mode | `AnEagerComputeNodeIsABarrier` |
| `ComputeNode`'s step publishes through the node's own slot | `ACallLeavesANodesSlotAlone` |
| The in-place step writes into the node's `scratch_` | `AnInPlaceStepHasAScratchOfItsOwn` |
| The memoized step goes through the cache | `AMemoizedNodeIsCalledPastItsCache` |
| `DifferentiableNode`'s step evaluates through the dual functor | `ADifferentiableNodeIsCalledAtDouble` |

**The hot path.** The sweep above, on this step's headers against those before Step 1, at all 16 alignments, with
`idle-queues` as the row no change can reach. If `chain` or `ingest` is slower at every alignment by more than
`idle-queues` moves, the mixin goes and the two hooks become virtuals on `NodeBase` (D2), in this step, and the plan
says so. `Engine::cycle()`'s disassembly is compared before and after, as the proof that the engine's own code is the
same.

*Done when:* red then green; the checkpoint; each change as stated; the sweep's table is in the Outcome; pushed. Commit:
`feat: a reduction calls Lazy compute nodes, differentiable nodes and memoized nodes`.

*As built* (the Outcome has the results). The barrier's message gains its third reason here, with the nodes that have
it: an `IReducible` that is `Eager` and gives no step. The sweep was run twice, and the second time with a control the
plan did not ask for: the base headers with the 8 bytes and nothing else. The mixin stays, by the rule above, and it is
not free: `chain` reads 0.4% slower on average. The Outcome says what was measured, and D2 what closes it.

### Step 3 — Selection and tweaks, one commit

`dag.hpp` and `dag.inl`: `ConditionNode` derives `IReducible`: its step selects, and it reads its condition, which
decides, and the branch selected. `TweakableComputeNode` derives it too: a step when `Lazy`, and it reads nothing while
tweaked.

| Test (`Reduction.`) | Asserts |
|---|---|
| `AConditionTakesTheBranchTheGraphTook` | Its value and its steps; a counted functor on the other branch never runs, and that branch stays dirty |
| `ABranchSwitchIsSeenAtTheNextBind` | After the flag moves and the root is evaluated, `bind()` follows the other branch |
| `ABarrierOnTheBranchNotTakenIsNotOne` | An `Eager` node on the other branch: `bind()` succeeds. Once it is taken: `std::domain_error` |
| `AConditionAnArgumentMovesIsABarrier` | `std::domain_error`, and the message says why |
| `ABranchNoArgumentReachesIsAConstant` | One step while the constant branch is taken, three while the other is |
| `ATweakedNodeIsAConstant` | Its frozen value is used and its inputs are not read. Untweaked: called when `Lazy`, a barrier when `Eager` |
| `ATweakIsSeenAtTheNextBind` | Tweaked and cleared between binds |
| `ABindHoldsOnlyWhatItsPointReads` | After a branch switch and a bind, the value only the old branch read has the reference count it had before the reduction |

| Change made by hand, then reverted | Must fail |
|---|---|
| `ConditionNode` reads both branches | `AConditionTakesTheBranchTheGraphTook` and `ABarrierOnTheBranchNotTakenIsNotOne` |
| `ConditionNode` reads its condition without `decides` | `AConditionAnArgumentMovesIsABarrier` |
| `ConditionNode`'s step returns the other branch | `AConditionTakesTheBranchTheGraphTook` |
| `TweakableComputeNode` reads its inputs while tweaked | `ATweakedNodeIsAConstant` |
| `TweakableComputeNode::reduceStep()` returns a step whatever the mode | `ATweakedNodeIsAConstant` |
| `bind()` keeps the last point's values (probed: this test alone) | `ABindHoldsOnlyWhatItsPointReads` |

*Done when:* red then green; the checkpoint; each change as stated; pushed. Commit:
`feat: a reduction follows the branch and the tweaks the graph holds`.

*As built* (the Outcome has the results). `bind()` now reads a constant each time a step names it, where Step 1 skipped
one it already held. The values are the same. The difference is what makes the last row of the table fail one test
alone, as probed: with the skip, a `bind()` that kept the last point's values also kept a stale constant, and
`BindReadsThePointTheGraphHolds` failed with it.

### Step 4 — The gradient, one commit

`IReduceStep::partials()` for the ops (from `Derivative<Op>`), `DifferentiableNode` (the helper its `partials()` now
shares) and `ConditionNode`; `Reduction::gradient()`.

| Test (`Reduction.`) | Asserts |
|---|---|
| `GradientMatchesATape` | Each graph of `aad_test_graphs.hpp`, at the point the graph holds and at a moved one: `aad::adjoints()` there, to 1e-13 |
| `GradientMatchesAnalyticDerivatives` | The same graphs against their closed forms, and the Black–Scholes vega to 1e-11 |
| `AnOpaqueStepHasNoGradient` | A `Lazy` compute node: `gradient()` throws `std::domain_error` naming the argument, the root and the node, and a call still returns |
| `AZeroPartialOrAdjointPropagatesNothing` | z·√x at z = 0 and x = 0: both derivatives are 0 |
| `ATrueNaNIsKept` | `PowerNode` at a = −2 over both: the base's derivative is 12 and the exponent's is NaN |
| `TheGradientThroughAConditionIsItsBranchs` | And 0 for an argument on the other branch, and for one the root does not reach |
| `ATweakedNodeHasDerivativeZero` | The argument above it moves nothing through it |
| `ADifferentiableNodesPartialsAreItsFunctors` | a·eᵇ/c as one three-input node, over all three, away from the graph's point: its analytic partials |
| `OneDualCallPerDifferentiableNode` | A gradient runs each functor once on doubles and once on duals |
| `NewtonFindsAnImpliedVol` | From 0.2, the vol at which the call is worth 12, to 1e-12, in under 8 steps; the graph agrees once vol is set to it |
| `AWarmGradientAllocatesNothing` | 100 gradients over a graph with an n-ary product: 0 allocations |

| Change made by hand, then reverted | Must fail |
|---|---|
| A partial times an adjoint taken literally | `AZeroPartialOrAdjointPropagatesNothing` |
| The sweep runs the steps in call order | `GradientMatchesATape` |
| A step with no partials counts as 0 | `AnOpaqueStepHasNoGradient` |
| A binary op's step swaps its two partials | `GradientMatchesATape` |
| `ConditionNode`'s step names the other branch | `TheGradientThroughAConditionIsItsBranchs` |
| The n-ary step builds its vectors at each call | `AWarmGradientAllocatesNothing` |
| The dual helper seeds every argument with one unit vector (probed: not `GradientMatchesATape`, which shares it) | `ADifferentiableNodesPartialsAreItsFunctors`, and `AadNode.PartialsComeFromOneDualCall` |

*Done when:* red then green; the checkpoint; the seven AAD suites pass unchanged; each change as stated; pushed. Commit:
`feat: a reduction's gradient`.

*As built* (the Outcome has the results). `IReduceStep::partials()` and its default arrive here, as Step 1 said. Three
tests more than the table: `TheGradientIsInTheArgumentsOwnValues` (u·v over an intermediate argument and a leaf, and the
identity over the root), `ANodeOfYourOwnHasAGradientThroughItsStep`, and
`APartialOfAnInputTheNodeDoesNotHaveIsRefused`, which is `std::logic_error` by name, as on a tape. `RejectsBadArguments`
also holds a gradient to one slot per argument. `GradientMatchesAnalyticDerivatives` holds the closed forms to 1e-12,
and the tape to 1e-13 in its own test. `DifferentiableNode`'s helper takes the values as an array, so its `partials()`
pulls them and the step reads them, and both seed and run the functor in the one place.

### Step 5 — In an engine, and the benchmark row, one commit

Tests, in `tests/test_dag_reduce.cpp`, and `benchmarks/bench_hot_path.cpp`'s fourth row with its line in
`benchmarks/expected_invariants.txt` and its row in the README's table (D7). The first three lines of the file must not
change.

| Test (`Reduction.`) | Asserts |
|---|---|
| `ASolverNodeInAnEngine` | The node above delivers the vol at which the call meets the quote; it solves again when the spot moves and when the quote moves, and not in a cycle where nothing moves |
| `ASolverNodeSeesAPointThatMovesWhileTheRootStandsStill` | a·x + b at x₀, with (a, b) moved so that the root keeps its value: the node delivers the new solution |
| `ASolvingCycleAllocatesNothing` | 200 cycles of post, step, bind and six Newton steps: 0 allocations, in the reduction and in the nodes it reads |

These tests hold a pattern and a whole cycle, so their changes are to the pattern, and to code that earlier steps
already hold with a narrower test:

| Change made by hand, then reverted | Must fail |
|---|---|
| The test's solver node made `Lazy` | `ASolverNodeSeesAPointThatMovesWhileTheRootStandsStill` |
| The test's functor does not bind | `ASolverNodeInAnEngine`: the solution is the first point's |
| An argument's value published with `make_value` (Step 1's change) | `ASolvingCycleAllocatesNothing` as well |

*Done when:* red then green; the checkpoint; `--invariants` equals the new file on the Mac and in `ubuntu-dev`; each
change as stated; pushed. CI's Ubuntu leg is the first x86-64 reading of the new checksum, and a difference there is
fixed forward. Commit: `test: a reduction in an engine cycle, and a solve row for the benchmark`.

*As built* (the Outcome has the results). The tests hold a pattern and no new engine code, so they passed as written,
and the changes below are their red. The solver's reduction is built unbound, as in the snippet above, so a functor
that never binds fails at the first cycle on the unbound call. A functor that binds once is the change that gives "the
solution is the first point's", and it is in the Outcome too. The new line of `expected_invariants.txt` was not taken
from the benchmark alone: the same six Newton steps, written as a plain loop with no engine, graph or reduction, give
the same line.

### Step 6 — Example, docs, follow-ups, one commit

- `examples/reduce.cpp`: an implied volatility by Newton on a reduction of the Black–Scholes graph, checked by setting
  the input to it. Its last line says whether the function and the graph agree, and `example_reduce` matches that line,
  as `example_aad` does (85 tests).
- `README.md`: a Features entry; a section after Derivatives with the two snippets, the table of what can be called,
  and the rules for `bind()`; `dag_reduce.hpp` and `IReducible` in the headers table.
- `docs/engine_rules.md`, under "Writing nodes": a reduction calls a `Lazy` functor at values the graph does not hold and
  refuses an `Eager` one, and a node of your own joins through `IReducible` (about 300 of the 623 bytes the budget has
  left). `CLAUDE.md` is one byte under its budget and does not change.
- The header comment of `dag_reduce.hpp` carries the rules; the class comments in `dag.hpp` carry the contract.
- Three follow-up issues, labelled `enhancement` and `core`, in the milestone "Differentiable subgraphs": a node that
  owns a reduction (D6), several roots (D3), a condition an argument moves (D5). A comment on flywheel-dag#11 (a
  reduction through a stateful node needs the same decision as a derivative through one) and on flywheel-dag#13 (a kept
  reduction is a gradient at the graph's own point that allocates nothing, with the figures above).
- The Outcome here, a closing comment on flywheel-dag#55 with the commits, and the issue closed by hand.

*Done when:* the checkpoint at 85 tests; `test_claude_md_budget` and `test_comment_refs` pass; the issues exist and I
have told you their numbers, labels and milestone. Commit: `docs: describe dag::reduce, with an example`.

*As built* (the Outcome has the results). The header comments were written with the code, step by step, so this step
adds none. `docs/engine_rules.md` grew by 263 bytes, to 5,640 of its 6,000. The closing comment on flywheel-dag#55 and
its close wait with Step 7 for your answer to D2: if the mixin goes, the work goes on under this issue.

### Step 6a — The hooks move to `NodeBase`, one commit (added 2026-11-12)

D2 as answered: the mixin goes before the release, because after it the change is a break. `IReducible` is removed.
`NodeBase` gains the two hooks as virtuals, declared after `propagate()` so that no virtual a node already had moves in
its vtable:

```cpp
class NodeBase : public INode {
public:
    /// A step that applies this node's function, or null, the default: a barrier.
    virtual std::unique_ptr<IReduceStep> reduceStep();
    /// The inputs this node's value depends on at the values the graph holds now. The default is
    /// every input.
    virtual void reduceReads(EvalContext& ctx, ReduceReads& out);
};
```

The classes of Steps 1 to 4 keep their overrides and lose a base class, and so do the suite's two nodes. No node grows.
A `Reduction` finds the hooks with a cast to `NodeBase`. A node that is not one is a barrier, and reads every input,
as a node without the mixin did. A node of your own joins by overriding `reduceStep()`.

With no mixin to tell a compute node from a stateful one, the barrier's message goes by what the node says of itself:
a `NodeKind::TimeSeries` node is stateful, and the message does not tell the reader to build it `Lazy`; an `Eager` node
of another kind gives no step, and a compute node gives one when it is built `Lazy`; any other node gives no step.

| Test (`Reduction.`) | Asserts |
|---|---|
| The suite's `Counted` and `Chooser` | They derive `NodeBase` alone and override its two hooks, so the suite does not compile until `NodeBase` has them |
| `ANodeThatIsNotANodeBaseIsABarrier` | A node of the suite's own that derives `INode` alone. Below the argument: `std::domain_error`, by name. With no argument above it: a constant |
| `ANodeThatCannotBeCalledIsABarrier` (changed) | An `EWMANode`: the message names it as a time-series node and does not mention `Lazy`. A `Lazy` node of the suite's that gives no step: "gives no step" |
| `ANodeGivesNoStepUnlessItSaysOtherwise` | `NodeBase`'s defaults, on nodes that do not override them: no step from an `Input` or an `EWMANode`, and every input read |

| Change made by hand, then reverted | Must fail |
|---|---|
| A node that is not a `NodeBase` is taken as a constant | `ANodeThatIsNotANodeBaseIsABarrier` |
| The message does not look at the node's kind | `ANodeThatCannotBeCalledIsABarrier` |
| `NodeBase::reduceReads()` names no input | `ANodeGivesNoStepUnlessItSaysOtherwise`, and most of the suite |
| Every row of Steps 1 to 5, made again on this code | The tests it failed before |

**The hot path.** Every class is the size it was before Step 1. `Engine::cycle()`'s disassembly is compared with the
one before Step 1. The sweep of Step 2 is run again on this step's headers against those before Step 1, at all 16
alignments: `chain` must show no slowdown beyond what `idle-queues` moves.

*Done when:* red then green; the checkpoint at 85 tests on the three compilers; the sizes, the disassembly and the
sweep are in the Outcome; every hand-made change of Steps 1 to 5 still fails its test; the README, the engine rules,
the header comments and the example say `NodeBase` and not `IReducible`; pushed. Then the closing comment on
flywheel-dag#55 and its close, which Step 6 left. Commit: `refactor: the reduction hooks are NodeBase's`.

*As built* (the Outcome has the results). The comment over the hooks gives the reason they are virtuals, and not
where they sit in the class: the vtable order matters to the proof in the Outcome and to nothing a reader of the code
relies on. A fourth change was made by hand, a default step that is not null, beside the three of the table.

### Step 7 — Release v1.3.0

As `docs/releasing.md`: the scope and the access of each changed signature, the three version strings, the gate on the
Mac and in `ubuntu-dev`, the commit, the tag and the release page with its notes, then the runs read afterwards and
recorded here. Commit: `build: release v1.3.0`.

## Designs I rejected

- **Move the graph and put it back.** Unsound in an engine, as probed above.
- **A private copy of the subgraph.** Nodes have no clone, and a copy's `Input::set()` allocates on every call.
- **Doubles only, on the nodes a tape can see into.** Faster per step, and no new mixin on the compute classes, but a
  `ComputeNode` could never be called, and neither could a subgraph that carries a curve or a vector between a double
  argument and a double root. The issue is about a DAG, not about its differentiable part.
- **Read the constants at each call.** A virtual call and a reference count per constant per call, where a solver's
  calls all share one point. It would also put the clean-root rule on every call.
- **Evaluate a dirty root for the caller.** It would swallow an engine callback, as it would for a tape.
- **Return a `std::function`.** It could not be bound again or differentiated. A `Reduction` is callable, so a lambda
  that holds one is the `std::function` when one is wanted.
- **A step per call from the node (`make_value`).** Simpler than a step object, and it allocates on every call.

## Self-review — risks and assumptions

- **Purity is a contract, not a check.** A `Lazy` functor that has side effects has them at every call. `Lazy` already
  required a pure function, and the engine cannot verify that either.
- **A bound reduction goes stale.** After a constant moves it still answers for the old point, and it cannot tell. The
  header says so, as it does for a tape.
- **`bind()` needs a clean root.** Between cycles a dirty root throws rather than being evaluated. With an always-dirty
  node (an application's clock-driven node) above the root, `bind()`'s pull evaluates it again, as a tape's does, and one
  that reaches the root by two paths leaves the root dirty, so `bind()` always throws there. D6's node is the way out.
- **Barriers are conservative**, as a tape's are: an argument anywhere above a node that cannot be called is refused,
  even on a path that contributes nothing.
- **A functor that reads captured state reads it at call time**, not at bind time. Only a `DifferentiableNode` may (a
  `Lazy` functor may not read what it does not declare).
- **The bit-for-bit claim is about values.** A step and its node call one compiled functor on the same values. The
  gradient shares `Derivative<Op>` with the tape, inlined at two sites, and a compiler may contract them differently, so
  those tests hold 1e-13, as `ATrigFunctorAgreesWithItsOpGraph` does.
- **A reduction holds values.** It keeps each constant's `ValuePtr` until the next bind, which pins one of the
  producer's two buffers. Bound each cycle that costs nothing (Step 5 holds it). Left bound while the graph moves, the
  producer allocates once or twice and then has two buffers again.
- **The hot path.** The 8-alignment screen shows no cost; the 16-alignment sweep in Step 2 decides, with a design to
  fall back on.
- **Cost per step.** About 8 ns, most of it reference counts on `ValuePtr`s. A subgraph of doubles could run on a table
  of doubles, with no change to what a caller sees, if a solver ever needs it.
- **New names in `dag`.** An application with its own `dag::reduce` or `dag::Reduction` would collide. The classes in
  the table grow by 8 bytes, which is a layout change of public types and no source change.
- **Compilers.** Apple clang 21, GCC 13.3 and GCC 15.2 built the prototype without a warning, the last two on aarch64.
  x86-64 sees the code first in CI.
- **The eval thread only**, like `eval()`: steps use `ValueSlot`, and functors may read captured state.
- **Assumptions:** graphs are not rewired after they are built; CI stays on `ubuntu-latest` and `macos-latest`;
  LLVM 23 stays the lint gate's major.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 Approval | Done | Pushed and linked from flywheel-dag#55; D5 and D9 answered, and the plan approved, 2026-11-11. |
| 1 The contract, the ops, and a reduction's values | Done | 2026-11-11. 22 tests; the checkpoint at 84; 15 hand-made changes caught. See the Outcome. |
| 2 Compute nodes, and which functors may be called | Done | 2026-11-11. 30 tests; 9 hand-made changes caught; the mixin stays by Step 2's rule, at a measured 0.37% on `chain` (D2 is open for you). See the Outcome. |
| 3 Selection and tweaks | Done | 2026-11-11. 38 tests; the checkpoint at 84; 6 hand-made changes caught. See the Outcome. |
| 4 The gradient | Done | 2026-11-11. 52 tests; the checkpoint at 84; 10 hand-made changes caught. See the Outcome. |
| 5 In an engine, and the benchmark row | Done | 2026-11-11. 55 tests; the `solve` row, its checksum the same on three compilers and from a plain loop; 5 hand-made changes as predicted. See the Outcome. |
| 6 Example, docs, follow-ups | Done, but for the close | 2026-11-11. The checkpoint at 85; flywheel-dag#74, flywheel-dag#75 and flywheel-dag#76 raised. The closing comment and the close of flywheel-dag#55 follow Step 6a. |
| 6a The hooks move to `NodeBase` | Done | 2026-11-12. 56 tests; the checkpoint at 85; every class the size it was before Step 1, and `chain` no slower; 48 hand-made changes caught. See the Outcome. |
| 7 Release v1.3.0 | Not started | After Step 6a. |

## Outcome

The prototype of "What I checked" was not kept, so the code is written again from this plan, test first.

### Step 1 — the contract, the ops, and a reduction's values (2026-11-11)

- **Red, then green.** With the suite written and registered, the build stopped at `'flywheel/dag_reduce.hpp' file not
  found`. With the code, every test passed at the first build that compiled: 18 then, 22 with the four added since.
- **The checkpoint**, with CI's flags and the lint gate, and no warning in any build:

  | | Build | Tests | clang-tidy | `--invariants` |
  |---|---|---|---|---|
  | Mac, Apple clang 21.0.0 | every target | 84 of 84 | 43 of 43 (LLVM 23.1.4) | equal |
  | `ubuntu-dev`, GCC 13.3.0 | every target | 84 of 84 | 43 of 43 (LLVM 23.1.6) | equal |
  | `ubuntu:26.04`, GCC 15.2.0 | every target | `test_dag_reduce`, 22 of 22 | not run | equal |

  The same Mac run on the commit before gave 83 of 83 and 42 of 42. `dag_reduce.hpp` compiles as a unit's only include
  under all three compilers.
- **The hand-made changes**, each on a scratch copy of `include/` and `tests/`, with each test run alone under a
  watchdog and a control that passed 22 of 22. Each of the twelve fails the test its row names. Three fail other tests
  too, for reasons read from their logs:
  - *The walk follows every input* also fails `IsUnboundUntilABindSucceeds`, `ADecidingInputAnArgumentMovesIsABarrier`
    and `ANodeThatReadsNothingIsAConstant`: every test whose graph holds a `Chooser`, the one node that reads fewer
    inputs than it has.
  - *A candidate with no `IReducible` is taken as a constant* also fails `IsUnboundUntilABindSucceeds`, whose failing
    bind is a barrier.
  - *The n-ary step folds from the last input* also fails `MatchesTheGraphAtEveryPoint`, by one unit in the last place
    of a three-term sum.

  Two rows were made in a form that keeps the rest of the suite meaningful: *the walk goes on above an argument* as "an
  argument that has inputs is walked like any other node", and *a candidate that reads nothing an argument moves is
  given a step* as "it counts as moved by the first argument".
- **Three more changes**, for the tests the table did not name:

  | Change made by hand, then reverted | Fails |
  |---|---|
  | `ReduceReads::reads()` overwrites a deciding input | `ReduceContract.ReadsRecordsWhatANodeNames` alone |
  | `IReducible::reduceReads()` names no input by default | `ReduceContract.ANodeReadsEveryInputUnlessItSaysOtherwise`, and 16 of the `Reduction` tests |
  | `ReduceInputs` ignores the positions it is given | `ReduceContract.InputsAreAViewOfTheReductionsValues`, and 7 of the `Reduction` tests |

### Step 2 — compute nodes, and which functors may be called (2026-11-11)

- **Red, then green.** The eight tests were written against the headers of Step 1 and all eight failed, each on a
  compute node that was a barrier. With the code, 30 of 30.
- **The checkpoint**, with CI's flags and the lint gate, and no warning in any build:

  | | Build | Tests | clang-tidy | `--invariants` |
  |---|---|---|---|---|
  | Mac, Apple clang 21.0.0 | every target | 84 of 84 | 43 of 43 | equal |
  | `ubuntu-dev`, GCC 13.3.0 | every target | 84 of 84 | 43 of 43 | equal |
  | `ubuntu:26.04`, GCC 15.2.0 | every target | `test_dag_reduce`, 30 of 30 | not run | equal |

- **The hand-made changes.** Each of the five fails the test its row names, and no other, against a control that
  passed 30 of 30. Four more, for what the table left out:

  | Change made by hand, then reverted | Fails |
  |---|---|
  | `InPlaceComputeNode::reduceStep()` returns a step whatever the mode | `AnEagerComputeNodeIsABarrier` |
  | The barrier's message does not look at the mode | `AnEagerComputeNodeIsABarrier` |
  | `MemoizedComputeNode` gives a step only when `Lazy` | `AMemoizedNodeIsCalledPastItsCache` |
  | `DifferentiableNode` gives a step only when `Lazy` | `ADifferentiableNodeIsCalledAtDouble` |

- **The hot path: what did not change.** `Engine::cycle()` in `bench_hot_path` is the same 188 lines of machine code
  before Step 1 and after Step 2. The destructor of its guard differs in one `adrp`/`add` pair, the address of a string
  on its cold path: layout, not behaviour.
- **The hot path: what grew.** Each class that joins is 8 bytes larger: `ComputeNode<double, double>` 200 to 208,
  `InPlaceComputeNode<std::vector<double>, double>` 224 to 232, `ops::ProductNode<>` 184 to 192, `ops::DiffNode<>` 192
  to 200, `aad::DifferentiableNode<1>` 240 to 248, `MemoizedComputeNode<double, double>` 216 to 224.
- **The hot path: what it costs.** Apple M4 Pro. `bench_hot_path`'s three rows, built from the headers before Step 1
  and from this step's, behind a pad of 0 to 15 nops, so that `Engine::cycle()` lands at each of the 16 alignments.
  Each binary was run 30 times in shuffled order, over two sessions, each run the median of 7 timings in a warm
  process. Paired by where `cycle()` lands, ns/cycle, this step's / before:

  | `cycle()` mod 64 | `chain` | `idle-queues` | `ingest` |
  |---|---|---|---|
  | 0 | 266.10 / 266.05 (+0.0%) | 230.33 / 230.34 (-0.0%) | 122.36 / 123.76 (-1.1%) |
  | 4 | 266.12 / 265.24 (+0.3%) | 231.08 / 230.73 (+0.2%) | 122.23 / 122.60 (-0.3%) |
  | 8 | 267.24 / 264.27 (+1.1%) | 230.34 / 255.54 (-9.9%) | 123.08 / 122.39 (+0.6%) |
  | 12 | 267.06 / 265.18 (+0.7%) | 230.44 / 230.33 (+0.0%) | 121.69 / 123.32 (-1.3%) |
  | 16 | 267.55 / 265.19 (+0.9%) | 230.46 / 230.36 (+0.0%) | 123.61 / 122.10 (+1.2%) |
  | 20 | 266.79 / 265.95 (+0.3%) | 230.49 / 230.43 (+0.0%) | 123.37 / 123.43 (-0.1%) |
  | 24 | 267.01 / 266.41 (+0.2%) | 230.62 / 230.25 (+0.2%) | 122.23 / 122.80 (-0.5%) |
  | 28 | 267.06 / 264.94 (+0.8%) | 230.57 / 230.67 (-0.0%) | 121.93 / 122.05 (-0.1%) |
  | 32 | 267.04 / 265.87 (+0.4%) | 230.36 / 230.28 (+0.0%) | 121.96 / 123.15 (-1.0%) |
  | 36 | 266.62 / 266.07 (+0.2%) | 230.63 / 230.76 (-0.1%) | 121.67 / 123.78 (-1.7%) |
  | 40 | 266.17 / 266.34 (-0.1%) | 230.42 / 230.46 (-0.0%) | 122.84 / 121.81 (+0.8%) |
  | 44 | 266.60 / 265.85 (+0.3%) | 230.49 / 230.29 (+0.1%) | 122.56 / 122.75 (-0.1%) |
  | 48 | 266.66 / 265.55 (+0.4%) | 230.76 / 230.39 (+0.2%) | 123.32 / 123.53 (-0.2%) |
  | 52 | 266.52 / 266.37 (+0.1%) | 230.36 / 230.54 (-0.1%) | 123.04 / 121.77 (+1.0%) |
  | 56 | 266.12 / 265.89 (+0.1%) | 230.24 / 230.25 (-0.0%) | 122.69 / 122.78 (-0.1%) |
  | 60 | 266.30 / 266.07 (+0.1%) | 230.46 / 230.40 (+0.0%) | 122.72 / 122.29 (+0.4%) |

  | Over the 16 alignments | `chain` | `idle-queues` | `ingest` |
  |---|---|---|---|
  | Mean change | +0.37% | −0.58%, and +0.04% without alignment 8 | −0.15% |
  | Range | −0.1% to +1.1% | −9.9% to +0.2% | −1.7% to +1.2% |
  | Slower at | 15 of 16 | 9 of 16 | 5 of 16 |
  | Slower by more than `idle-queues` moved there | 13 of 16 | | 4 of 16 |
  | The headers before Step 1 alone, lowest to highest | 0.8% | 11.0% | 1.6% |

  `idle-queues` holds no class that changed. Its one large figure, at alignment 8, is the older binary reading 255 ns
  where every other binary reads 230: a slow alignment of that binary, the same in both sessions.

  The second session added a control that the plan did not ask for: the headers before Step 1 with a second
  polymorphic base on `ComputeNode`, `InPlaceComputeNode` and `OpNodeImpl`, and no other change. Against the headers
  before Step 1, over the 16 alignments of that session:

  | 15 runs per binary | `chain` | `idle-queues` | `ingest` |
  |---|---|---|---|
  | The control: the 8 bytes alone | +0.37%, slower at 14 | +0.01% at the median | −0.26%, slower at 7 |
  | This step's headers | +0.35%, slower at 16 | +0.03% at the median | −0.27%, slower at 5 |
  | This step's headers against the control | −0.02%, slower at 6 | 0.00% | −0.01%, slower at 6 |

  So the cost is real, it is small, and it is the 8 bytes: `chain`, which holds three of the grown nodes among its
  eleven, runs 0.37% slower on average, about 1 ns in 266, by nothing at some alignments and by 1.1% at the worst.
  `ingest` and `idle-queues` show none.
- **The rule.** The mixin goes if `chain` or `ingest` is slower at every alignment by more than `idle-queues` moves.
  `chain` is slower by more than `idle-queues` moved at 13 of the 16, and `ingest` at 4, so by the rule the mixin
  stays, and it stayed. That is the rule's answer and not a claim that the mixin is free: D2's alternative would close
  the cost, and D2 now asks which you want before the release.

### Step 3 — selection and tweaks (2026-11-11)

- **Red, then green.** The eight tests failed against the headers of Step 2, where a `ConditionNode` and a
  `TweakableComputeNode` were barriers. With the code, 38 of 38.
- **The checkpoint**, with CI's flags and the lint gate, and no warning in any build:

  | | Build | Tests | clang-tidy | `--invariants` |
  |---|---|---|---|---|
  | Mac, Apple clang 21.0.0 | every target | 84 of 84 | 43 of 43 | equal |
  | `ubuntu-dev`, GCC 13.3.0 | every target | 84 of 84 | 43 of 43 | equal |
  | `ubuntu:26.04`, GCC 15.2.0 | every target | `test_dag_reduce`, 38 of 38 | not run | equal |

  `ConditionNode` is 208 to 216 bytes and `TweakableComputeNode<double, double>` 224 to 232.
- **The hand-made changes**, against a control that passed 38 of 38. Each of the six fails the tests its row names.
  Three fail others too, for reasons read from their logs:
  - *`ConditionNode` reads both branches* also fails `ABranchNoArgumentReachesIsAConstant`, where the branch the
    argument moves is then read while the constant one is taken, and `ABindHoldsOnlyWhatItsPointReads`, where the old
    branch is still read after the switch.
  - *`ConditionNode`'s step returns the other branch* fails all five tests whose reduction runs a selection as a step.
  - *`TweakableComputeNode` reads its inputs while tweaked* also fails `ATweakIsSeenAtTheNextBind`, which then calls
    the functor where it should use the frozen value.

  *`bind()` keeps the last point's values* fails `ABindHoldsOnlyWhatItsPointReads` and no other test.
- **CI** on the commits of Steps 1 and 2: `CI` and `clang-tidy` green on both, which is the first x86-64 build of the
  new code.

### Step 4 — the gradient (2026-11-11)

- **Red, then green.** With the 14 tests written, the build stopped at `no member named 'gradient' in
  'dag::Reduction'`. With the code, 52 of 52.
- **The checkpoint**, with CI's flags and the lint gate, and no warning in any build:

  | | Build | Tests | clang-tidy | `--invariants` |
  |---|---|---|---|---|
  | Mac, Apple clang 21.0.0 | every target | 84 of 84 | 43 of 43 | equal |
  | `ubuntu-dev`, GCC 13.3.0 | every target | 84 of 84 | 43 of 43 | equal |
  | `ubuntu:26.04`, GCC 15.2.0 | every target | `test_dag_reduce`, 52 of 52, and the seven AAD suites, 94 of 94 | not run | equal |

  The seven AAD suites pass with no edit to any of them.
- **The hand-made changes**, against the reduce suite and `test_aad_node` together, with a control that passed 59 of
  59. Each of the seven fails the tests its row names. Four fail others too:
  - *The sweep runs the steps in call order* and *a binary op's step swaps its two partials* each also fail
    `GradientMatchesAnalyticDerivatives` and `NewtonFindsAnImpliedVol`, and one or two of the narrower tests.
  - *A step with no partials counts as 0* also fails `ATweakedNodeHasDerivativeZero`, whose last lines untweak the
    node and expect the refusal.
  - *The dual helper seeds every argument with one unit vector* also fails `OneDualCallPerDifferentiableNode` and two
    more of `test_aad_node`. As probed, it fails neither `GradientMatchesATape` nor the vega, nor the price's Greeks in
    `test_aad_node`: the tape shares the helper, and the price's derivative through d1 is 0.
- **Three more changes**, for what the table left out:

  | Change made by hand, then reverted | Fails |
  |---|---|
  | `gradient()` does not check the room it is given | `RejectsBadArguments` |
  | An argument the root does not reach keeps what its slot held | `TheGradientThroughAConditionIsItsBranchs` |
  | The adjoints are not zeroed between gradients | `GradientMatchesATape` and three more |
- **CI** on the commit of Step 3: `CI` and `clang-tidy` green.

### Step 5 — in an engine, and the benchmark row (2026-11-11)

- **The tests.** Three more, 55 in all. They hold a pattern, with no new engine code, so they passed as written.
- **The `solve` row.** `solve allocs/cycle=0 callbacks=200000 checksum=1341290.0460180121`. The first three lines of
  `benchmarks/expected_invariants.txt` did not change. The checksum was checked against the recurrence alone, as a
  plain loop of six steps of x − (x·x − a)/(x + x) from 1, with no engine, graph or reduction: the same 17 digits, at
  `-O0` and at `-O2`, with `-ffp-contract=off`.
- **The checkpoint**, with CI's flags and the lint gate, and no warning in any build:

  | | Build | Tests | clang-tidy | `--invariants`, four rows |
  |---|---|---|---|---|
  | Mac, Apple clang 21.0.0 | every target | 84 of 84 | 43 of 43 | equal |
  | `ubuntu-dev`, GCC 13.3.0 | every target | 84 of 84 | 43 of 43 | equal |
  | `ubuntu:26.04`, GCC 15.2.0 | every target | `test_dag_reduce`, 55 of 55 | not run | equal |

  All three are arm64. CI's Ubuntu leg, on the commit this step pushes, is the first x86-64 reading of the new line.
- **The hand-made changes**, against a control that passed 55 of 55:

  | Change made by hand, then reverted | Fails |
  |---|---|
  | The test's solver node made `Lazy` | `ASolverNodeSeesAPointThatMovesWhileTheRootStandsStill` alone: the zero stays at 1 |
  | The test's functor does not bind | `ASolverNodeInAnEngine`, at its first cycle: `root call is not bound` |
  | The test's functor binds at its first run only | `ASolverNodeInAnEngine`: the second solution is the first point's, so it is not delivered |
  | An argument's value published with `make_value` | `ASolvingCycleAllocatesNothing`, and the three other allocation tests |
  | `bind()` keeps the last point's values | `ABindHoldsOnlyWhatItsPointReads` alone, as probed: the solving cycle still allocates nothing |
- **CI** on the commit of Step 4: `CI` and `clang-tidy` green.

### Step 6 — example, docs, follow-ups (2026-11-11)

- **The example.** `examples/reduce.cpp` solves for the vol at which the call is worth 12, by Newton's method on
  `price(vol)`, from 0.2: three steps, to 0.2388677617. The graph then holds 0.25 and its price still, and with the
  input set to the answer it gives the function's price to the bit. `example_reduce` matches its last line.
- **The checkpoint**, with CI's flags and the lint gate, and no warning in any build:

  | | Build | Tests | clang-tidy | `--invariants` |
  |---|---|---|---|---|
  | Mac, Apple clang 21.0.0 | every target | 85 of 85 | 44 of 44 | equal |
  | `ubuntu-dev`, GCC 13.3.0 | every target | 85 of 85 | 44 of 44 | equal |
  | `ubuntu:26.04`, GCC 15.2.0 | every target | `test_dag_reduce`, 55 of 55; the example agrees | not run | equal |

  `test_claude_md_budget` and `test_comment_refs` pass. `CLAUDE.md` did not change.
- **What it costs to call**, on the code as it now is. Apple M4 Pro, the Black–Scholes call (17 nodes), best of nine
  loops of 200,000, three runs. The figures of "What I checked" were the prototype's:

  | | ns |
  |---|---|
  | The formula as one plain function | 19 to 21 |
  | The graph: `vol->set(v)`, then `call->eval()` | 194 to 198 |
  | The reduction over vol (8 steps): `f(v)` | 68 to 70 |
  | The same, with its derivative: `f.gradient(v)` | 241 to 243 |
  | The reduction over all five inputs (12 steps): `f(x)` | 102 to 104 |
  | The same, with all five derivatives | 328 to 330 |
  | `bind()`, over vol and over all five | 99 to 101, 122 to 126 |
  | A tape recorded and swept once in reverse, for the same five | 3,140 to 3,220 |
  | Constructing the reduction over vol (once) | 2,440 to 2,510 |
- **The follow-ups**, each labelled `enhancement` and `core`, in the milestone "Differentiable subgraphs":
  [flywheel-dag#74](https://github.com/tomlin256/flywheel-dag/issues/74), a node that owns a reduction and binds it at
  each evaluation (D6); [flywheel-dag#75](https://github.com/tomlin256/flywheel-dag/issues/75), several roots from one
  reduction (D3); [flywheel-dag#76](https://github.com/tomlin256/flywheel-dag/issues/76), a `ConditionNode` whose
  condition an argument moves (D5). A comment on flywheel-dag#11 says that a reduction through a stateful node needs
  the answer "at the current state", and one on flywheel-dag#13 gives the figures above for a kept reduction against a
  tape, and what still stands between a reduction and a `GradientNode` built on one.
- **Not done here:** the closing comment on flywheel-dag#55 and its close. They wait with Step 7 for D2.
- **CI** on the commit of Step 5: `CI` and `clang-tidy` green. The Ubuntu leg is x86-64, and its benchmark step
  compares four rows, so the `solve` checksum is the same there.

### Step 6a — the hooks move to `NodeBase` (2026-11-12)

- **Red, then green.** With the suite's two nodes deriving `NodeBase` alone, the build stopped at `only virtual member
  functions can be marked 'override'` and `no member named 'reduceStep' in 'dag::Input<double>'`. With the hooks on
  `NodeBase` and `IReducible` gone, 56 of 56.
- **The checkpoint**, with CI's flags and the lint gate, and no warning in any build:

  | | Build | Tests | clang-tidy | `--invariants` |
  |---|---|---|---|---|
  | Mac, Apple clang 21.0.0 | every target | 85 of 85 | 44 of 44 | equal |
  | `ubuntu-dev`, GCC 13.3.0 | every target | 85 of 85 | 44 of 44 | equal |
  | `ubuntu:26.04`, GCC 15.2.0 | every target | `test_dag_reduce`, 56 of 56, and twelve more suites, 282 of 282; both examples agree | not run | equal |

- **The hot path: nothing grew.** `ComputeNode<double, double>` 200 bytes, `InPlaceComputeNode<std::vector<double>,
  double>` 224, `ops::ProductNode<>` 184, `ops::DiffNode<>` 192, `aad::DifferentiableNode<1>` 240,
  `MemoizedComputeNode<double, double>` 216, `ConditionNode` 208, `TweakableComputeNode<double, double>` 224: each the
  size it was before Step 1.
- **The hot path: the same code.** `Engine::cycle()` is the same 188 lines of machine code as before Step 1. The
  destructor of its guard differs in one `adrp`/`add` pair, the address of a string on its cold path.
- **The hot path: no cost.** The sweep of Step 2 again, this step's headers against those before Step 1: 16
  alignments, each binary run 15 times in shuffled order. Paired by where `cycle()` lands, ns/cycle, this step's /
  before:

  | `cycle()` mod 64 | `chain` | `idle-queues` | `ingest` |
  |---|---|---|---|
  | 0 | 264.72 / 265.39 (-0.3%) | 229.99 / 230.20 (-0.1%) | 120.45 / 121.48 (-0.8%) |
  | 4 | 265.25 / 264.95 (+0.1%) | 230.26 / 230.59 (-0.1%) | 120.13 / 120.82 (-0.6%) |
  | 8 | 264.70 / 263.86 (+0.3%) | 230.06 / 254.66 (-9.7%) | 120.95 / 120.40 (+0.5%) |
  | 12 | 264.51 / 265.69 (-0.4%) | 230.33 / 230.10 (+0.1%) | 120.73 / 122.71 (-1.6%) |
  | 16 | 265.87 / 264.70 (+0.4%) | 230.05 / 230.19 (-0.1%) | 120.47 / 120.24 (+0.2%) |
  | 20 | 265.19 / 265.88 (-0.3%) | 230.06 / 230.10 (-0.0%) | 120.91 / 121.57 (-0.5%) |
  | 24 | 265.08 / 266.10 (-0.4%) | 230.15 / 230.02 (+0.1%) | 119.90 / 120.92 (-0.8%) |
  | 28 | 265.31 / 265.29 (+0.0%) | 230.32 / 230.30 (+0.0%) | 119.98 / 120.42 (-0.4%) |
  | 32 | 265.50 / 265.69 (-0.1%) | 230.17 / 229.99 (+0.1%) | 120.54 / 121.00 (-0.4%) |
  | 36 | 265.16 / 266.16 (-0.4%) | 230.48 / 230.39 (+0.0%) | 121.06 / 121.73 (-0.5%) |
  | 40 | 265.91 / 265.56 (+0.1%) | 230.04 / 230.14 (-0.0%) | 121.39 / 120.31 (+0.9%) |
  | 44 | 265.65 / 265.51 (+0.1%) | 230.15 / 229.96 (+0.1%) | 120.50 / 120.69 (-0.2%) |
  | 48 | 265.01 / 264.83 (+0.1%) | 230.32 / 230.17 (+0.1%) | 120.52 / 121.54 (-0.8%) |
  | 52 | 265.51 / 264.64 (+0.3%) | 230.09 / 230.28 (-0.1%) | 120.16 / 120.47 (-0.3%) |
  | 56 | 263.67 / 265.31 (-0.6%) | 230.19 / 230.13 (+0.0%) | 120.77 / 120.98 (-0.2%) |
  | 60 | 264.32 / 266.64 (-0.9%) | 230.06 / 230.25 (-0.1%) | 120.80 / 120.11 (+0.6%) |

  | Over the 16 alignments | `chain` | `idle-queues` | `ingest` |
  |---|---|---|---|
  | Mean change | −0.11% | −0.61%, and 0.00% without alignment 8 | −0.31% |
  | Range | −0.9% to +0.4% | −9.7% to +0.1% | −1.6% to +0.9% |
  | Slower at | 8 of 16 | 8 of 16 | 4 of 16 |
  | Slower by more than `idle-queues` moved there | 5 of 16 | | 3 of 16 |

  `chain` was 0.37% slower with the mixin, at 15 of 16 alignments. With the hooks on `NodeBase` it is slower at half
  of them and faster at half, by less either way than the headers before Step 1 spread on their own (1.1%).
- **The hand-made changes**, against the reduce suite and `test_aad_node` together, with a control that passed 63 of
  63 in every run. All 45 of Steps 1 to 5 were made again on this code, and each still fails the test its row names.
  Two rows needed a longer anchor, because the line they change now occurs twice, and one changed its form: a
  candidate *that gives no step* is taken as a constant, where it was one with no `IReducible`. Most fail more tests
  than they did, because later steps added tests that lean on the same code. Then this step's three, and with them
  the one of Step 1 that now changes `NodeBase`:

  | Change made by hand, then reverted | Fails |
  |---|---|
  | A node that is not a `NodeBase` is taken as a constant | `ANodeThatIsNotANodeBaseIsABarrier` alone |
  | The message does not look at the node's kind | `ANodeThatCannotBeCalledIsABarrier` alone |
  | `NodeBase::reduceReads()` names no input | `ANodeGivesNoStepUnlessItSaysOtherwise`, and 48 of the `Reduction` tests |
  | `NodeBase::reduceStep()` gives a step by default | `ANodeGivesNoStepUnlessItSaysOtherwise`, `ANodeThatCannotBeCalledIsABarrier` and `IsUnboundUntilABindSucceeds` |
- **CI** on the last commit of Step 6: `CI` and `clang-tidy` green. Its push cancelled the `CI` run of the commit
  before it, whose tree it contains.
