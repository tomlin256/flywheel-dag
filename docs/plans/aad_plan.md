# Algorithmic Differentiation: Forward and Reverse Mode

**Status: Approved (2026-09-29).**

Closes [flywheel-dag#10](https://github.com/tomlin256/flywheel-dag/issues/10).

## Problem

`dag::ops` gives each arithmetic op its own type "so that inputs() plus the op's identity are enough
for a future pass (e.g. reverse-mode AAD) to attach a closed-form local derivative". No such pass
exists. The issue asks for one, in both modes:

- **Reverse (adjoint) mode.** One sweep backwards from an output gives its derivative with respect
  to every input it depends on. It costs one sweep per output.
- **Forward (tangent) mode.** One sweep forwards gives the derivative of every output in one
  direction of the inputs. It costs one sweep per direction.

Both take the derivative at the values the graph holds now. A pass must leave the graph as it found
it: it evaluates nothing the graph has not already evaluated, advances no stateful node, and never
reads the branch a `ConditionNode` did not take. The engine's evaluation path does not change, so
`bench_hot_path`'s exact columns stay as committed.

**Done when** a test differentiates a closed-form formula built from ops and differentiable compute
nodes, and both modes match its analytic derivatives.

## Design

### The graph is the tape

An AAD library records every arithmetic operation on a tape as the program runs, through an active
number type, and then sweeps the tape. Here the program is already a graph, and the engine already
knows every edge through `inputs()`. So a pass records one entry per node instead of one per
operation: the node's local partial derivatives at the values it holds now.

Nothing is recorded during evaluation. A pass costs something only when it is asked for, and a graph
that never asks pays nothing.

Rejected: an active number type inside the nodes. Every node would record on every cycle, whether
or not anything asked for a derivative. That puts allocation and work on the hot path, and it
changes every functor's types.

### The contract: `aad::IDifferentiable`

```cpp
namespace dag::aad {

// What partials() writes: one entry per input the node's value depends on.
class Partials {
public:
    void add(std::size_t input, double d);   // d = ∂value/∂inputs()[input]
    // ...
};

class IDifferentiable {
public:
    virtual ~IDifferentiable() = default;
    /// The node's local partial derivatives at its inputs' current values.
    /// Pulls its inputs with eval(ctx), as eval() does. Returns false if the
    /// node cannot say.
    virtual bool partials(EvalContext& ctx, Partials& out) = 0;
};

} // namespace dag::aad
```

- **It is a mixin, like `ITweakable` and `IStatefulNode`.** A pass finds it with `dynamic_cast`, as
  `Engine::discoverStatefulNodes()` finds `IStatefulNode`. `INode` and `NodeBase` do not change, so
  no application node has to.
- **A pass calls it only on a clean node.** Every input it pulls is then clean too, so `eval()`
  returns the cached value, and nothing is evaluated.
- **Partials come from the inputs alone,** never from the node's own published value. Under a
  tolerance policy the two can differ, because the published value is the last one the policy let
  through.
- **`false` makes the node a barrier** (see the tape, below).

The declarations go in `dag.hpp`, beside `ITweakable`, because `ConditionNode` and
`TweakableComputeNode` implement them.

| Node | Partials |
|---|---|
| `SumNode` | 1 for each input |
| `ProductNode` | For each input, the product of the others, from prefix and suffix products |
| `DiffNode` | 1, −1 |
| `DivideNode` | 1/b, −a/b² |
| `NegateNode` | −1 |
| `ExpNode` | exp(a) |
| `LnNode` | 1/a |
| `PowerNode` | b·a^(b−1), or 0 when b = 0. a^b·ln a, or 0 at a = 0 with b > 0 |
| `SqrtNode` | 1/(2·√a) |
| `ConditionNode` | 1 for the branch it took. Nothing for the condition or for the other branch |
| `TweakableComputeNode` | While tweaked, none: its value is a constant. Otherwise `false`: its functor is opaque |
| `aad::DifferentiableNode<N>` (new) | From its functor, run once on dual numbers |

Three points differ from what `CLAUDE.md` says today:

- **`ProductNode`.** `product / x_i` is 0/0 when `x_i` is 0. The product of the other factors is
  exact.
- **`PowerNode`.** At a = 0 with b > 0, a^b·ln a is 0·(−∞), which is NaN. But a^b is flat in b
  there, so the partial is 0. *As built:* the same holds for ∂/∂a when b is 0. There b·a^(b−1) is
  0·∞ at a = 0, but a^0 is 1 for every a, so that partial is 0 too.
- **Only `T = double` has partials.** An op over another type returns `false`. A pass needs a
  `double` root, so such an op could reach one only through a conversion node, which is a barrier
  anyway.

The ops' partials come from a trait specialised per `Op`, `ops::Derivative<Op>`, as `CLAUDE.md`
anticipates. An application that builds a `UnaryOpNode<double, MyOp>` makes it differentiable by
specialising the trait.

### The tape

`aad::Tape` records the nodes that some roots depend on at the values they hold now. It walks up
from the roots once, and follows only the inputs that each node's partials name:

- A node with no inputs, such as `Input`, `AsyncInput` or `ReplayInput`, is a **leaf**.
- A node with inputs but no partials is a **barrier**, and its inputs are not followed. Every
  `dag::ts` node is one. So is a `ComputeNode`, `InPlaceComputeNode` or `MemoizedComputeNode`,
  because its functor is opaque.
- The branch a `ConditionNode` did not take, and the inputs of a tweaked node, are never named, so
  the walk never reaches them.

The tape keeps the nodes in topological order, with each node's partials as a flat edge list. Two
sweeps read it:

```cpp
namespace dag::aad {

struct Seed { NodePtr node; double tangent; };

class Tape {
public:
    /// Throws std::invalid_argument unless every root is clean and holds a double.
    explicit Tape(std::vector<NodePtr> roots);

    /// Reverse: ∂root/∂w for each w in wrt, in one sweep. root is one of the roots.
    std::vector<double> adjoints(const NodePtr& root, const std::vector<NodePtr>& wrt) const;

    /// Forward: each root's derivative in the direction the seeds give,
    /// Σ ∂root/∂s.node · s.tangent, in one sweep.
    std::vector<double> tangents(const std::vector<Seed>& seeds) const;

    /// The number of nodes recorded.
    std::size_t size() const noexcept;
};

// Record, then sweep once.
std::vector<double> adjoints(const NodePtr& root, const std::vector<NodePtr>& wrt);
std::vector<double> tangents(const std::vector<NodePtr>& roots, const std::vector<Seed>& seeds);

} // namespace dag::aad
```

In use:

```cpp
EvalContext ctx;
price->eval(ctx);   // a pass reads a clean root

// Reverse: every sensitivity of one output, in one sweep.
const std::vector<double> g = aad::adjoints(price, {spot, vol, rate});

// Forward: the sensitivity of several outputs to one input, in one sweep.
const std::vector<double> dVol = aad::tangents({price, hedge}, {{vol, 1.0}});
```

A tape can be swept many times. A full Jacobian is one reverse sweep per root, or one forward sweep
per input, over the same tape. A tape holds partials, not values, so it describes the point it was
recorded at. Once an input moves, record a new one.

The rules:

- **Every root must be clean,** or the tape throws. A pass reads what the graph holds and evaluates
  nothing. Evaluating a dirty root for the caller would be friendlier, but in an engine it is wrong.
  `Engine::cycle()` snapshots its outputs' dirty flags before it evaluates them. If a pass evaluates
  a registered output between cycles, that output reads as clean at the next snapshot, and its
  callback misses the change. So a pass runs in an output callback, where the engine has just
  evaluated the root, or after the caller has evaluated the root itself.
- **Any node can be a `wrt` or seed node,** not only a leaf. Its adjoint is the derivative with
  respect to a change in its own value. A seed on an intermediate node adds to the tangent that
  reaches it. That keeps the two modes exact duals: a root's tangent is Σ seed · adjoint.
- **A node the root does not depend on at this point has derivative 0.** That includes a node on
  the branch not taken.
- **A barrier with a `wrt` or seed node upstream throws `std::domain_error`,** and the message
  names both. The derivative through a barrier is unknown, and 0 would be a silently wrong answer. A
  barrier with none upstream is a constant. A barrier can itself be a `wrt` node, so the derivative
  with respect to a stateful node's output is available, though not with respect to its input. In
  reverse mode, only the barriers upstream of the swept root count.
- **A zero adjoint or tangent propagates nothing.** So a constant's NaN or infinite partial never
  reaches a result. Take `PowerNode` at a = −2 with a constant n = 3. Its ∂/∂n is NaN, and ∂/∂a is
  still 12 in both modes. With 0·NaN taken literally, ∂/∂a would be NaN too. A varied input whose
  partial is NaN still gives NaN, which is the true answer.
- **A `wrt` or seed node on the tape must hold a `double`,** or the sweep throws
  `std::invalid_argument`. A node that is not on the tape has derivative 0, whatever it holds.
- **Eval thread only,** like `eval()`.

### `aad::DifferentiableNode<N>` and `aad::Dual<N>`

A `ComputeNode`'s functor is a `std::function<Out(const Ins&...)>`, so it is opaque. The new node
takes its functor once, as a generic callable, and instantiates it at two types: at `double` to
evaluate, and at `aad::Dual<N>` to take all N partials in one call.

```cpp
auto d1 = aad::DifferentiableNode<3>::make(
    "d1", {logMoneyness, vol, expiry},
    [](const auto& m, const auto& v, const auto& t) {
        using std::sqrt;
        return (m + 0.5 * v * v * t) / (v * sqrt(t));
    },
    InvalidationMode::Lazy);
```

- **Everything else is `ComputeNode`.** It has the same three `make()` overloads for the policy and
  the mode, a `ValueSlot`, and an allocation-free steady state. Its inputs and its output are
  `double`.
- **`Dual<N>` is a value and N partials, on the stack.** It has `+ − × ÷` with a `Dual` or a
  `double` on either side, unary minus, and comparisons on the value, so a functor can branch. It
  has `exp`, `log`, `sqrt`, `pow`, `abs`, `min`, `max`, `sin`, `cos`, `tanh`, `erf` and `erfc`,
  found by argument-dependent lookup. A functor calls them unqualified after `using std::exp;`, so
  the same body compiles at `double` and at `Dual<N>`. `aad::chain(x, f(x.value), f′(x.value))`
  lifts any other function whose derivative is known.
- **The functor must have no side effects.** It runs again, on duals, each time a tape records the
  node. A functor that mutates captured state would mutate it twice.
- **Why a new node, and not another `ComputeNode::make()`.** `std::function` erases a generic
  callable to one signature, so `ComputeNode` would need a second callable member. That is an extra
  member on the hottest class in the engine, for a handful of nodes. `InPlaceComputeNode`'s note
  rejects the same thing for the same reason.

### What does not change

No `eval()` path changes, so `allocs/cycle`, `callbacks` and `checksum` stay as committed. `INode`,
`NodeBase` and every existing `make()` are untouched. The ops, `ConditionNode` and
`TweakableComputeNode` gain a base class, which is one more vtable pointer per node.

### Prototype

A scratch prototype on `086efa0`, built with Apple Clang 21 and `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`,
added the contract, the ops' partials, `ConditionNode`'s and `TweakableComputeNode`'s partials, the
tape with both sweeps, a minimal `Dual<N>` and a minimal `DifferentiableNode<N>`. It compiled without
a warning, and 11 tests passed:

- A Black–Scholes call, built from ops and differentiable nodes, records a 17-node tape. Reverse
  mode matches the closed-form delta and strike sensitivity to 1e-12, and vega, theta and rho to
  1e-11. Forward mode matches reverse to 1e-14 relative. A weighted seed matches the same weighted
  sum of adjoints to 1e-13.
- After a pass, every node was still clean and its `ValuePtr` kept its identity. The `d1` node's
  functor did not run on `double`s, and ran once on duals per recording.
- The untaken branch of a `ConditionNode` stayed dirty, and its functor never ran.
- As a `wrt` node, an `EWMANode` got the right adjoint, and the pass did not advance it: it matched
  a control fed the same values. With its input as `wrt`, the pass threw `std::domain_error`.
- A product with a zero factor gave exact partials, (12, 0, 0) at (0, 3, 4). `PowerNode` at a = −2
  with a constant exponent of 3 gave ∂/∂a = 12 in both modes.
- A pass inside an `addOutput` callback, driven by `Engine::step()`, gave 2, 4 and 10 for ∂(s·s)/∂s
  at s = 1, 2 and 5.

With the prototype's suite added, ctest was green at 28 of 28, and `--invariants` matched the
committed file. Interleaved on the same machine, `chain` read 246.7–251.5 ns/cycle before and
248.3–253.5 after, which is within noise.

On this Mac (Apple M4 Pro), recording the call's tape and sweeping it in reverse took 3.4 µs. A
reverse sweep of a recorded tape took 0.18 µs and a forward sweep 0.10 µs. For scale, re-evaluating
the graph after an input moves took 0.17 µs. So recording costs about 20 evaluations and a sweep
about one.

### Not in this plan

- **Derivatives through `dag::ts` nodes.** They are barriers. A derivative through a stateful node
  can mean two things: at the current state (∂output/∂input, holding the state) or through the
  state, over time. That choice is a design of its own, and each node needs its own partials and
  tests. A follow-up issue.
- **Sensitivities as nodes.** A node whose value is a gradient would let `addOutput` deliver
  sensitivities. Until then, a pass runs in the root's output callback. That callback fires only
  when the root's value changes, and a gradient can change while the value does not: x·y is 6 at
  (2, 3) and at (3, 2), with gradients (3, 2) and (2, 3). A follow-up issue.
- **An allocation-free pass.** A tape allocates, and so does each `inputs()` call. A pass is not on
  the cycle path unless an application puts it there. A follow-up issue.
- **Higher-order derivatives, and nodes whose value is a vector.**
- **A benchmark row** for the cost of a pass.

### Release

**v0.1.6.** The change adds a header, an interface and base classes, and changes nothing a consumer
already calls, as v0.1.5 added the install rules.

## Steps

Every commit subject is scoped to flywheel-dag#10 in the repo's `type(#N): …` form. The subjects
below leave the scope out. Every checkpoint runs a full build, including `bench_hot_path`, and a
full ctest run, and both must pass. `bench_hot_path --invariants` must match the committed file.
Each step is pushed, and CI must be green on both legs before the next step starts.

Each test below that guards a specific mistake must be seen to fail before it is trusted: the step
lists the change to make by hand, then revert.

### Step 1 — The contract, and partials for the ops, `ConditionNode` and `TweakableComputeNode`

- `dag.hpp` and `dag.inl`: `aad::Partials` and `aad::IDifferentiable`. `ConditionNode` and
  `TweakableComputeNode` implement it.
- `dag_ops.hpp` and `dag_ops.inl`: `ops::Derivative<Op>` for the nine ops. `OpNodeImpl` derives
  from `aad::IDifferentiable`, and each arity template implements `partials()`.
- Time `chain` before and after, interleaved on one machine.

Tests: a new suite, `tests/test_aad_partials.cpp`.

| Test | Asserts |
|---|---|
| `AadPartials.OpsMatchTheirClosedForms` | Each of the nine ops reports the table's partials at two points |
| `AadPartials.OpsMatchCentralDifferences` | Each op's partials are within 1e-6 relative of a central difference |
| `AadPartials.ProductIsExactWithZeroFactors` | (0, 3, 4) gives (12, 0, 0), and (0, 0, 4) gives (0, 0, 0) |
| `AadPartials.PowerIsFlatInTheExponentAtZero` | a = 0 and b = 2 give ∂/∂b = 0 |
| `AadPartials.PowerIsFlatInTheBaseWhenTheExponentIsZero` | *Added as built.* b = 0 gives ∂/∂a = 0, at a = 0 and at a = −2 |
| `AadPartials.PowerKeepsATrueNaN` | *Added as built.* a = −2 and b = 3 give ∂/∂a = 12, and a NaN for ∂/∂b |
| `AadPartials.OnlyDoubleOpsHavePartials` | `SumNode<int>` and `ExpNode<float>` return `false` |
| `AadPartials.AnApplicationOpCanSpecialiseDerivative` | A test-local `Op` with a `Derivative` specialisation reports it. One without returns `false` |
| `AadPartials.AnInputNamedTwiceGetsTwoEntries` | *Added as built.* A `DiffNode` of x and x names x twice, and the two entries add up to 0 |
| `AadPartials.ConditionNamesOnlyTheTakenBranch` | Input 1 when the condition is true and input 2 when it is false, with partial 1. Never input 0 |
| `AadPartials.ConditionNeverReadsTheOtherBranch` | A counting `ComputeNode` on the untaken branch stays dirty, and its count does not move |
| `AadPartials.ATweakedNodeIsAConstant` | Tweaked: `true` with no entries. Cleared: `false` |
| `AadPartials.ReadingPartialsEvaluatesNothing` | On an evaluated graph, every node's `ValuePtr` keeps its identity across `partials()` |

| Change made by hand, then reverted | Must fail |
|---|---|
| `ProductNode`'s partial as `product / x_i` | `ProductIsExactWithZeroFactors` |
| Drop both flat cases from `PowerNode` | `PowerIsFlatInTheExponentAtZero` and `PowerIsFlatInTheBaseWhenTheExponentIsZero` |
| `ConditionNode::partials()` pulls both branches | `ConditionNeverReadsTheOtherBranch` |

**Done when:** ctest is green (28 of 28), `--invariants` matches, each hand-made change fails as
stated, `chain` is within noise of before, and CI is green on both legs.

Commit: `feat: local partial derivatives for the ops, ConditionNode and TweakableComputeNode`.

### Step 2 — The tape and the reverse sweep

- New `include/flywheel/dag_aad.hpp` and `dag_aad.inl`: `aad::Tape`, its recording and
  `adjoints()`, and the free `aad::adjoints()`.

Tests: a new suite, `tests/test_aad_reverse.cpp`.

| Test | Asserts |
|---|---|
| `AadReverse.MatchesAnalyticGradients` | On graphs such as x·y + exp(x)/y and ln(x)·√y, every adjoint equals the analytic derivative |
| `AadReverse.MatchesCentralDifferences` | The same graphs, bumped through `Input::set()` and re-evaluated |
| `AadReverse.SharedNodesAccumulate` | x·eˣ·eˣ, where eˣ feeds one product twice and x feeds both |
| `AadReverse.RecordsEachNodeOnce` | A diamond records four nodes |
| `AadReverse.AnIntermediateNodeCanBeWrt` | ∂root/∂(eˣ) on the graph above |
| `AadReverse.AnUnreachedNodeHasDerivativeZero` | A node of another graph, and a node on the branch not taken, both give 0 |
| `AadReverse.ARootCanBeALeaf` | ∂x/∂x is 1 |
| `AadReverse.RejectsADirtyRoot` | `std::invalid_argument` after `Input::set()` and before `eval()` |
| `AadReverse.RejectsARootOrWrtThatIsNotADouble` | An `Input<int>` root. An `Input<float>` as `wrt`, put on the tape by a test-local node whose partials name it |
| `AadReverse.ABarrierWithAWrtUpstreamThrows` | A `ComputeNode` between x and the root: `std::domain_error`, with both names in the message |
| `AadReverse.ABarrierIsAConstantOrAWrt` | With no `wrt` upstream it is a constant. As a `wrt` it has its adjoint |
| `AadReverse.OnlyBarriersUpstreamOfTheRootCount` | A tape with two roots and a barrier under one: the other root's sweep does not throw |
| `AadReverse.AZeroAdjointPropagatesNothing` | `PowerNode` at a = −2 with a constant exponent: ∂/∂a = 12, and ∂/∂n is NaN |
| `AadReverse.APassEvaluatesNothing` | Every node's `ValuePtr` identity and every functor's count are unchanged across a pass |
| `AadReverse.APassDoesNotAdvanceAStatefulNode` | An `EWMANode` as `wrt` gets its adjoint and matches a control fed the same values |
| `AadReverse.InsideAnEngineCallback` | An `AsyncInput` driven by `Engine::step()`: the callback's pass gives ∂(s·s)/∂s = 2s on each cycle |
| `AadReverse.ATapeSweepsManyTimes` | One tape with two roots, swept once for each, equals two single-root passes |

| Change made by hand, then reverted | Must fail |
|---|---|
| Remove the zero-adjoint skip | `AZeroAdjointPropagatesNothing` |
| Drop the clean-root check | `RejectsADirtyRoot` |
| Drop the barrier check | `ABarrierWithAWrtUpstreamThrows` |
| Check every barrier on the tape, not only those upstream of the root | `OnlyBarriersUpstreamOfTheRootCount` |

**Done when:** ctest is green (29 of 29), `--invariants` matches, each hand-made change fails as
stated, and CI is green on both legs.

Commit: `feat: record a tape and sweep it in reverse`.

### Step 3 — The forward sweep

- `Tape::tangents()` and the free `aad::tangents()`.

Tests: a new suite, `tests/test_aad_forward.cpp`.

| Test | Asserts |
|---|---|
| `AadForward.MatchesAnalyticDerivatives` | Step 2's graphs, seeded one input at a time |
| `AadForward.OneSweepServesEveryRoot` | Three roots and one seed: one `tangents()` call returns all three derivatives |
| `AadForward.IsTheDualOfReverse` | On every Step 2 graph, a weighted seed's tangent equals the weighted sum of adjoints, to 1e-13 relative |
| `AadForward.JacobianBothWays` | A graph with 3 inputs and 2 roots: 3 forward sweeps and 2 reverse sweeps of one tape give the same Jacobian |
| `AadForward.ASeedOnAnIntermediateNodeAdds` | A seed on eˣ alone gives ∂root/∂(eˣ). Seeded with x as well, it adds to the tangent from x |
| `AadForward.ABarrierWithASeedUpstreamThrows` | `std::domain_error`, as in reverse mode |
| `AadForward.AZeroTangentPropagatesNothing` | `PowerNode` at a = −2 with a constant exponent, seeded on a: 12, not NaN |

| Change made by hand, then reverted | Must fail |
|---|---|
| Remove the zero-tangent skip | `AZeroTangentPropagatesNothing` |
| Let a seed replace the tangent that reaches its node, instead of adding to it | `ASeedOnAnIntermediateNodeAdds` |

**Done when:** ctest is green (30 of 30), `--invariants` matches, each hand-made change fails as
stated, and CI is green on both legs.

Commit: `feat: sweep a tape forward`.

### Step 4 — Dual numbers

- `aad::Dual<N>`, its operators and functions, and `aad::chain()`, in `dag_aad.hpp` and
  `dag_aad.inl`.

Tests: a new suite, `tests/test_aad_dual.cpp`.

| Test | Asserts |
|---|---|
| `AadDual.ArithmeticFollowsTheSumProductAndQuotientRules` | `+ − × ÷` and unary minus, between duals and with a `double` on either side |
| `AadDual.FunctionsMatchTheirDerivatives` | At three points, each function's value equals the `std::` one, and its partials equal the analytic derivative |
| `AadDual.PowCoversEachMix` | `pow(Dual, Dual)`, `pow(Dual, double)` and `pow(double, Dual)`, with ∂/∂b = 0 at a = 0 and b > 0 |
| `AadDual.ComparisonsReadTheValue` | A functor that branches on `x < y` takes the same branch at `double` and at `Dual` |
| `AadDual.ConventionsAtKinks` | abs′(0) is 0. `min` and `max` follow the argument they return, and the first one on a tie |
| `AadDual.OneBodyCompilesAtBothTypes` | A generic lambda with `using std::exp;` gives the same value at `double` and at `Dual<3>`, and three partials that match central differences |
| `AadDual.ChainLiftsAFunction` | The standard normal CDF, built with `chain()`, has the density as its derivative |

| Change made by hand, then reverted | Must fail |
|---|---|
| The quotient rule without the square in its denominator | `ArithmeticFollowsTheSumProductAndQuotientRules` |
| Drop the a = 0 case from `pow` | `PowCoversEachMix` |

**Done when:** ctest is green (31 of 31), each hand-made change fails as stated, and CI is green on
both legs.

Commit: `feat: dual numbers`.

### Step 5 — `aad::DifferentiableNode<N>`

- `aad::DifferentiableNode<N>` in `dag_aad.hpp` and `dag_aad.inl`, with `ComputeNode`'s three
  `make()` overloads.

Tests: a new suite, `tests/test_aad_node.cpp`.

| Test | Asserts |
|---|---|
| `AadNode.EvaluatesLikeAComputeNode` | Its value; it recomputes when an input moves; its equality policy gates as a `ComputeNode`'s does |
| `AadNode.LazySkipsAndEagerReruns` | Both modes on one graph, as `test_lazy_invalidation.cpp` checks them for `ComputeNode` |
| `AadNode.SteadyStateAllocatesNothing` | Counted `operator new`, as in `test_value_slot.cpp`: 0 per evaluation once warm |
| `AadNode.PartialsComeFromOneDualCall` | Its partials equal the analytic ones. Recording ran the functor once on duals, and never on `double`s |
| `AadNode.ACallPriceMatchesItsClosedFormGreeks` | The prototype's Black–Scholes graph: both modes match delta, ∂/∂K, vega, theta and rho to 1e-11. This is the issue's criterion |

The counting `operator delete` needs the same one-site silencing for GCC bug 103993 as
`test_value_slot.cpp`.

| Change made by hand, then reverted | Must fail |
|---|---|
| Seed every dual argument with the same unit vector | `PartialsComeFromOneDualCall` |
| Evaluate through the dual functor | `PartialsComeFromOneDualCall` |

**Done when:** ctest is green (32 of 32), `ACallPriceMatchesItsClosedFormGreeks` passes on both CI
legs, each hand-made change fails as stated, and CI is green on both legs.

Commit: `feat: a compute node differentiated through dual numbers`.

### Step 6 — Example, docs and release

- `examples/aad.cpp`: prices a call from ops and `DifferentiableNode`s, and prints its
  sensitivities three ways: reverse mode, forward mode and central differences. Its last line says
  whether all three agree, and `example_aad` matches it, as `example_quickstart` matches its own.
- `README.md`: a Features entry, a row in the headers table, and a short section with the two
  calls, the barrier rule and the callback caveat from "Not in this plan".
- `CLAUDE.md`: a section on algorithmic differentiation, with the rules above: clean roots,
  barriers, zeros that propagate nothing, partials from the inputs alone, and a
  `DifferentiableNode` functor with no side effects. The ops section drops "No tape or backward pass
  exists yet", and gives the `ProductNode` and `PowerNode` partials as built. `NodeBase`'s list of
  derived classes gains `DifferentiableNode`.
- `dag_ops.hpp`'s header comment no longer says "forward evaluation only". *As built:* Step 1
  changed it, when the ops gained their partials.
- Release v0.1.6 as v0.1.5 was released: the project version and the FetchContent snippets in
  `CMakeLists.txt` and the README, the tag, and a GitHub release with notes.
- Open the follow-up issues from "Not in this plan", mark this plan done, and close flywheel-dag#10
  with a summary comment.

**Done when:** ctest is green (33 of 33), CI is green on the release commit, the release is
published, the follow-up issues are open and flywheel-dag#10 is closed.

Commits: `docs: add an example that checks a call's sensitivities both ways`,
`docs: describe algorithmic differentiation`, `build: release v0.1.6` and `docs: mark the plan done`.

## Self-review — risks and assumptions

- **A pass reads through `eval()`.** It relies on one invariant: once a root is clean, every node
  its partials name is clean too, so pulling one returns its cached value. The invalidation protocol
  keeps that true. Its two known exceptions are never on a tape: the branch not taken and a tweaked
  node's inputs. A third is an application's clock-driven node, whose `dirty()` is always true. A
  differentiable node that reads one re-evaluates it, as any consumer's pull does, and it can then
  disagree with the value the root was computed from. Such a node can never be a root either,
  because it is never clean. The engine has no such node, and the docs will say so.
- **The derivative sits at the inputs' current values.** Under a tolerance policy a node's published
  value can lag its inputs, so the chain rule mixes points that differ by less than the tolerance.
  Under the default policy they are the same point.
- **Zero propagates nothing, which departs from IEEE on purpose.** 0·∞ and 0·NaN count as 0 when the
  zero is an adjoint or a tangent. A NaN partial on a path the result depends on still gives NaN.
  Tests pin both halves.
- **The barrier check is conservative.** It throws when a `wrt` node is anywhere upstream of a
  barrier the root reaches, even if the path through the barrier contributes 0. Each sweep walks up
  from its barriers with one shared visited set, so the check is linear in the size of the graph.
- **A clean root is required, not evaluated.** Outside an engine, the caller writes one `eval()`
  first. That is what keeps a pass from swallowing an engine callback.
- **`DifferentiableNode`'s functor runs again at each recording.** Nothing can enforce that it has no
  side effects. The class comment and the docs say so, as they do for `InPlaceComputeNode`'s
  contract.
- **Argument-dependent lookup.** A functor that writes `std::exp(x)` does not compile at `Dual<N>`.
  That is a compile error, not a wrong answer, and the docs give the `using std::exp;` form.
- **GCC is unverified.** The prototype was built with Apple Clang alone. CI's GCC 13 leg sees the
  code first at Step 1, and the dual numbers' overloads at Step 4.
- **Cost.** A pass allocates, and makes one `dynamic_cast`, one `inputs()` call and one `partials()`
  call per node. Recording costs about 20 evaluations of the graph, as measured above. That is fine
  for sensitivities on demand. A pass on every cycle of a large graph would want the allocation-free
  pass listed under "Not in this plan".
- **Base-class cost.** The ops, `ConditionNode` and `TweakableComputeNode` grow by one vtable
  pointer. The prototype's `chain` timing stayed within noise.
- **A tape goes stale.** It holds the partials of the point it was recorded at. A sweep after an
  input has moved gives the old point's derivatives, and a tape cannot cheaply detect that. The docs
  say so.
- **Assumptions:** the engine stays header-only C++17, so the new code goes in `.hpp` and `.inl`
  files. `std::erf` and `std::erfc` have been in `<cmath>` since C++11. CI stays on `ubuntu-latest`
  and `macos-latest`. flywheel-dag#7's move to Ubuntu 26 on 2026-10-19 may land during this plan and
  bring a newer GCC.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — The contract, and partials | Done locally | ctest 28 / 28, and `--invariants` is unchanged. The build prints no warnings with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`. `test_aad_partials` has 13 tests: the plan's 10 and three added as built. Writing `Derivative<PowOp<double>>` found a second flat case: at b = 0, b·a^(b−1) is NaN at a = 0, where a^0 is flat. The three hand-made changes each failed as stated. `product / x_i` gave NaN against 12 at (0, 3, 4). Without their flat cases, both `PowerNode` tests read NaN against 0. Pulling both branches ran the other branch's functor once and left it clean. Interleaved on this Mac, `chain` read 250.2–254.3 ns/cycle before and 250.4–254.3 after |
| 2 — The tape and the reverse sweep | Not started | |
| 3 — The forward sweep | Not started | |
| 4 — Dual numbers | Not started | |
| 5 — `aad::DifferentiableNode<N>` | Not started | |
| 6 — Example, docs and release | Not started | |
