# Sensitivities as Graph Nodes

**Status: Done (2026-09-30).** Approved 2026-09-30. Both steps landed, and v0.1.8 is released.
`aad::GradientNode` holds a root's gradient with respect to a list of wrt nodes, so an engine
delivers sensitivities through `addOutput`. Its one input is its root, and it is `Eager`, so it
delivers the gradient of x·y at (2, 3) and at (3, 2), where the root's own callback fires once.
It evaluates what its root's `eval()` evaluates, and throws what a tape throws. The forward-mode
node is flywheel-dag#17. No evaluation path changed.

Closes [flywheel-dag#12](https://github.com/tomlin256/flywheel-dag/issues/12).

## Problem

A tape is recorded and swept on demand (flywheel-dag#10). Today an application gets sensitivities
by running a pass in the root's output callback. That callback fires only when the root's value
changes, and a gradient can change while the value does not: x·y is 6 at (2, 3) and at (3, 2),
with gradients (3, 2) and (2, 3). The callback misses the second gradient.

Add a node whose value is a gradient, so that `addOutput` delivers sensitivities like any other
output.

No evaluation path changes, so `bench_hot_path`'s exact columns stay as committed.

**Done when** an engine delivers the issue's example through `addOutput`: the root's callback fires
once, with 6, and the gradient node's fires twice, with (3, 2) and then (2, 3).

## Design

### `aad::GradientNode`

```cpp
namespace dag::aad {

class GradientNode : public NodeBase, public std::enable_shared_from_this<GradientNode> {
public:
    /// ∂root/∂w for each w in wrt, in wrt's order. Throws std::invalid_argument
    /// if root or a wrt node is null, or if wrt is empty.
    static std::shared_ptr<GradientNode> make(
        std::string name, NodePtr root, std::vector<NodePtr> wrt,
        EqualityPolicyPtr eq = nullptr);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;   ///< the root alone
    NodeKind kind() const override { return NodeKind::Compute; }
};

} // namespace dag::aad
```

In use:

```cpp
auto grad = aad::GradientNode::make("dprice", price, {spot, vol, rate});
engine.addOutput<std::vector<double>>(grad, [](const std::vector<double>& g) {
    // g[0] = ∂price/∂spot, g[1] = ∂price/∂vol, g[2] = ∂price/∂rate
});
```

Its value is a `std::vector<double>` with one entry per wrt node, as `aad::adjoints()` returns.
`eval()` pulls the root, which leaves it clean, then records a tape on it and sweeps it in reverse:
`Tape({root}).adjoints(root, wrt)`. It publishes the result through a `ValueSlot` and its equality
policy, as `ComputeNode` does. The default policy is `TypedEqualityPolicy<std::vector<double>>`.
The node is not a template, so its bodies are `inline` in `dag_aad.inl`, as `Tape`'s are.

- **Its one input is its root.** A wrt node the root depends on sits upstream of the root, so the
  root already carries its changes. A wrt node the root does not depend on has derivative 0. So
  `inputs()` returns the root alone. `Engine::discoverStatefulNodes()` walks through the node to
  the root, and `GraphExporter` draws one edge.
- **It is `Eager`, fixed in the class.** Its value depends on the partials of every node the tape
  records, and it declares none of them. `CLAUDE.md`'s rule applies: a node that reads state it did
  not declare as an input must be `Eager`. Any change upstream of the root marks the node at least
  Maybe, and an `Eager` node recomputes then. A `Lazy` node would skip whenever the root's value
  stood still, which is the issue's case. Its `make()` takes no mode, like every `dag::ts` node's.
- **It evaluates what its root's `eval()` evaluates, and nothing more.** The tape reads only clean
  nodes and never the branch a `ConditionNode` did not take. Pulling the root is what any consumer
  does to its input, so it is not the evaluation the tape's rule forbids. The engine's dirty
  snapshot still covers a root that is also a registered output, even when the node is registered
  first.
- **It throws what its tape throws, from `eval()`, and stays dirty.** That is `std::domain_error`
  for a barrier with a wrt node upstream, and `std::invalid_argument` for a root, or a wrt node on
  the tape, that does not hold a `double`. `make()` cannot check either, because both depend on
  values. A later `eval()` retries, so a graph whose `ConditionNode` switches back off a barrier's
  path recovers. Answering 0 would be silently wrong, as it would for a pass.
- **It is not an `aad::IDifferentiable`.** Its value is a vector, so no tape can take it as a root
  or a wrt node, and a node that reads one of its entries is a barrier.
- **Each recompute records a new tape.** It recomputes whenever anything upstream of the root
  fires, including a change the gradient does not depend on. See the prototype for the cost.

Rejected:

- **The wrt nodes as inputs.** `eval()` would pull them, and so evaluate a node the root does not
  read, such as one on the branch a `ConditionNode` did not take.
- **A `std::array<double, N>` value,** with the number of wrt nodes as a template parameter. It
  would own no heap, but the tape allocates on every recompute anyway, and a runtime list matches
  `aad::adjoints()`.
- **One node per sensitivity, holding a `double`.** That is N tapes for N sensitivities, where one
  reverse sweep gives them all.

### What does not change

No `eval()` path changes. `INode`, `NodeBase`, the tape and every existing node are untouched.
`bench_hot_path` uses no AAD, so `--invariants` is unchanged and there is nothing to time.

### Prototype

A scratch prototype on `6c0ade2`, built with Apple Clang 21 and `-Wall -Wextra -Wpedantic -Werror`,
defined the node in a test file, with an extra `InvalidationMode` parameter to try `Lazy`. It
compiled without a warning, and 11 tests passed:

- The issue's example, posted through two `AsyncInput`s and driven by `Engine::step()`: the root's
  callback fired once, with 6, and the node's fired twice, with (3, 2) and (2, 3). Built `Lazy`,
  the node delivered (3, 2) alone.
- Over a `DifferentiableNode`, a clean `eval()` returned the same `ValuePtr` and ran no dual call.
  After an input moved, it ran one, and matched `aad::adjoints()` bit for bit.
- The gradient of x + k recomputed on each of three cycles and was delivered once.
- A move on the branch a `ConditionNode` did not take marked the node Maybe, and it recomputed
  without running the branch's functor. The branch stayed dirty.
- With an `EWMANode` as a wrt node, the `EWMANode` matched a control fed the same values.
- Through an opaque branch, `eval()` threw `std::domain_error`, and the node stayed dirty. Once the
  condition switched, it gave the right gradient. An `Input<int>` root threw
  `std::invalid_argument`.
- An engine with only the node registered discovered the `EWMANode` above the root.
- Registered before its root, the node still let the root's callback fire on each of three cycles.
- On the Black–Scholes call from `test_aad_node.cpp`, the node matched `aad::adjoints()` bit for
  bit. Setting an input and evaluating the call took 0.18 µs on this Mac, and setting it and
  evaluating the node took 3.9 µs, best of seven runs. So a recompute costs about 20 evaluations of
  the root, as flywheel-dag#10 measured for a pass.
- For a forward-mode node (see "Not in this plan"), pulling a second root left the first dirty, and
  a tape over both threw.

The prototype was a standalone file, not the test suite, and no GCC build saw it. It also found that
a throw out of a cycle leaves `Engine::run()` running, which is now flywheel-dag#16.

### Not in this plan

- **A forward-mode node,** holding each of several roots' derivatives in one direction. A tape
  needs every root clean at once, and pulling one root can leave an earlier one dirty: a stale node
  on the branch a `ConditionNode` did not take moves when another root pulls it, and tells the
  `ConditionNode`. So such a node must pull its roots until they settle, and an application's
  clock-driven node would never let them. That is a design of its own, and Step 2 opens it as a
  follow-up issue. *As built:* flywheel-dag#17.
- **An allocation-free recompute.** flywheel-dag#13.
- **Higher-order derivatives.**
- **`Engine::run()` after a throw.** flywheel-dag#16.

### Release

**v0.1.8.** The change adds a node, and changes nothing a consumer calls today, as v0.1.7 added
the trigonometric ops.

## Steps

Every commit subject is scoped to flywheel-dag#12 in the repo's `type(#N): …` form. The subjects
below leave the scope out. Every checkpoint runs a full build with
`FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, including `bench_hot_path`, and a full ctest run, and both
must pass. `bench_hot_path --invariants` must match the committed file. Each step is pushed, and CI
must be green on both legs before the next step starts.

Each test below that guards a specific mistake must be seen to fail before it is trusted: the step
lists the change to make by hand, then revert.

### Step 1 — `aad::GradientNode`

- `dag_aad.hpp` and `dag_aad.inl`: the node, and a "Sensitivities as nodes" section in the
  header comment.
- `tests/CMakeLists.txt`: a new suite, `test_aad_gradient_node`, so ctest goes from 33 to 34.

Tests: `tests/test_aad_gradient_node.cpp`.

| Test | Asserts |
|---|---|
| `AadGradientNode.MatchesTheTape` | On each graph in `aad_test_graphs.hpp`, the node's value equals `aad::adjoints()` bit for bit, and the analytic gradient to 1e-13 |
| `AadGradientNode.DeliversAGradientThatMovesWhileTheValueStandsStill` | The issue's example in an engine: the root's callback fires once, with 6. The node's fires twice, with (3, 2) and then (2, 3). This is the issue's criterion |
| `AadGradientNode.RecordsOnlyWhenSomethingUpstreamFired` | Over a `DifferentiableNode` that counts its dual calls: a clean `eval()` returns the same `ValuePtr` and runs none. After an input moves, it runs one |
| `AadGradientNode.AnUnchangedGradientIsNotDelivered` | The gradient of x + k: three cycles that move x deliver it once |
| `AadGradientNode.AnEqualityPolicyGatesDelivery` | With a `PredicateEqualityPolicy` tolerance, a move inside it keeps the published `ValuePtr`, and a move outside it does not |
| `AadGradientNode.EvaluatesOnlyWhatItsRootDoes` | A move on the branch a `ConditionNode` did not take: the node recomputes, the branch's counted functor does not run, and the branch stays dirty. The branch's node is a wrt node, with derivative 0 |
| `AadGradientNode.DoesNotAdvanceAStatefulNode` | An `EWMANode` as a wrt node gets its adjoint, and matches a control fed the same values |
| `AadGradientNode.ThrowsWhatItsTapeThrows` | Through an opaque branch: `std::domain_error`, and the node stays dirty. Once the condition switches: the right gradient. An `Input<int>` root: `std::invalid_argument` |
| `AadGradientNode.RejectsANullRootOrAnEmptyOrNullWrt` | `make()` throws `std::invalid_argument` |
| `AadGradientNode.ItsOneInputIsItsRoot` | `inputs()` is the root alone, and an engine with only the node registered discovers the stateful node above the root |
| `AadGradientNode.RegisteredBeforeItsRoot` | The engine evaluates the node first. The root's callback still fires on each of three cycles, and each gradient is right |

| Change made by hand, then reverted | Must fail |
|---|---|
| Make the node `Lazy` | `DeliversAGradientThatMovesWhileTheValueStandsStill` |
| Pull the wrt nodes in `eval()` as well | `EvaluatesOnlyWhatItsRootDoes` |
| Rebind `cached_` on every recompute, ignoring the policy | `AnUnchangedGradientIsNotDelivered` and `AnEqualityPolicyGatesDelivery` |

**Done when:** ctest is green (34 of 34), `--invariants` matches, each hand-made change fails as
stated, and CI is green on both legs.

Commit: `feat: a node whose value is a gradient`.

### Step 2 — Docs and release

- `README.md`: the Derivatives section's last bullet, on a callback missing a gradient, gains the
  node and its `addOutput` snippet. The headers table's `dag_aad.hpp` row gains `GradientNode`.
- `CLAUDE.md`: the algorithmic differentiation section gains the node: its one input, why it is
  `Eager`, why pulling its root is not the evaluation the tape's rule forbids, what it throws, and
  its cost. The `InvalidationMode` table's `Eager` row and the list of modes fixed in a class gain
  it, and so does `NodeBase`'s list of derived classes.
- Release v0.1.8 as v0.1.7 was released: the project version and the FetchContent snippets in
  `CMakeLists.txt` and the README, the tag, and a GitHub release with notes.
- Open the forward-mode node as a follow-up issue, mark this plan done, and close flywheel-dag#12
  with a summary comment.

**Done when:** ctest is green (34 of 34), CI is green on the release commit, the release is
published, the follow-up issue is open and flywheel-dag#12 is closed.

Commits: `docs: describe the gradient node`, `build: release v0.1.8` and
`docs: mark the plan done`.

## Self-review — risks and assumptions

- **Cost.** A recompute costs about 20 evaluations of the root, and an `Eager` node recomputes on
  every change upstream of its root, including one on a branch not taken or above a barrier. A
  graph that needs sensitivities rarely should keep running a pass on demand. flywheel-dag#13 would
  make a recompute cheaper. The docs say so.
- **A throw aborts the cycle.** A graph whose `ConditionNode` can switch onto a path through a
  barrier, with a wrt node above the barrier, throws out of that cycle, and every later cycle until
  it switches back. That is the contract a pass already has. Out of `Engine::run()`, it also leaves
  the engine running, which is flywheel-dag#16.
- **NaN.** The default policy compares with `==`, and NaN is unequal to itself. A gradient holding
  a true NaN, such as ∂/∂n of aⁿ at a < 0, is delivered on every recompute. A `double` output
  behaves the same under its default policy. A `PredicateEqualityPolicy` can treat NaNs as equal.
- **A clock-driven node upstream.** A tape's pull re-evaluates it, as any pull does, so the tape can
  record partials at a later point than the root's value. A pass has this caveat already, and the
  node adds nothing to it.
- **The node holds its wrt nodes.** Each exists before the node, and the engine never rewires a
  graph, so none can be downstream of the node, and no reference cycle can form.
- **GCC is unverified.** The prototype was built with Apple Clang alone. CI's GCC 13 leg sees the
  code first at Step 1. flywheel-dag#7 moves CI to Ubuntu 26 on 2026-10-19, which may land during
  this plan and bring a newer GCC.
- **Assumptions:** the engine stays header-only C++17. CI stays on `ubuntu-latest` and
  `macos-latest`.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — `aad::GradientNode` | Done | ctest 34 / 34, and `--invariants` is unchanged. The build prints no warnings with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`. `test_aad_gradient_node` has the plan's 11 tests. `EvaluatesOnlyWhatItsRootDoes` counts the dual calls of a `DifferentiableNode` on the taken branch, so it shows that the node recorded a tape without reading the other branch. The three hand-made changes each failed as stated. Built `Lazy`, the node also failed `EvaluatesOnlyWhatItsRootDoes`, because it skipped the recording that test counts. Rebinding on every recompute also failed it, through its check that an unchanged gradient keeps its `ValuePtr`. CI run 36686447518 is green on both legs, at 34 / 34 with no compiler warnings |
| 2 — Docs and release | Done | The README's Derivatives section shows the node registered as an engine output, and its features list and headers table name it. `CLAUDE.md`'s algorithmic differentiation section gives the node's one input, why it is `Eager`, why pulling its root is not the evaluation the tape rule forbids, what it throws and what it costs. The `InvalidationMode` table, the list of modes fixed in a class and `NodeBase`'s list of derived classes gain it. ctest 34 / 34. The docs and release commits were pushed together, and CI run 36687096320 on the release commit is green on both legs, at 34 / 34 with no compiler warnings. The installed version file reports 0.1.8. `v0.1.8` is tagged and released. The forward-mode node is flywheel-dag#17, and flywheel-dag#12 is closed |
