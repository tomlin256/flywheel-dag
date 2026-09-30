# A Forward-Mode Sensitivity Node

**Status: Approved (2026-09-30).**

Closes [flywheel-dag#17](https://github.com/tomlin256/flywheel-dag/issues/17) and
[flywheel-dag#19](https://github.com/tomlin256/flywheel-dag/issues/19).

## Problem

`aad::GradientNode` (flywheel-dag#12) holds one root's gradient, from a reverse sweep, so an engine
delivers it through `addOutput`. flywheel-dag#17 asks for its forward-mode counterpart: a node
holding each of several roots' derivatives in one direction, from one tape and one forward sweep,
as `aad::tangents(roots, seeds)` returns them.

A tape needs each root clean when it records it, and pulling one root can leave an earlier one
dirty. A node on the branch a `ConditionNode` did not take can be stale. When a later root pulls it
and its value moves, it tells the `ConditionNode`, which marks the earlier root dirty again. So
after both roots are pulled, `aad::Tape({root1, root2})` throws.

Prototyping found the same effect in the released `GradientNode` (flywheel-dag#19). Its tape can
pull a node that is always dirty, such as an application's clock-driven node, and leave the root
dirty. v0.1.8 marks the gradient node clean anyway, and the next change stops at the root, which is
already dirty. The node never sees it, and returns a stale gradient.

Both nodes need one rule: a sensitivity node is clean only when its roots are.

**Done when** an engine delivers a forward-mode node's tangents through `addOutput`, including
tangents that move while the roots' values stand still. On a graph where a later root's pull leaves
an earlier one dirty, the node must record both roots, and a change after that must still reach it.
A `GradientNode` whose tape leaves its root dirty must see a later change too.

## Design

### `aad::TangentNode`

```cpp
namespace dag::aad {

class TangentNode : public NodeBase, public std::enable_shared_from_this<TangentNode> {
public:
    /// Each root's derivative in the direction the seeds give,
    /// Σ ∂root/∂s.node · s.tangent, in the roots' order. Throws
    /// std::invalid_argument if roots or seeds is empty, or if a root or a
    /// seed's node is null.
    static std::shared_ptr<TangentNode> make(
        std::string name, std::vector<NodePtr> roots, std::vector<Seed> seeds,
        EqualityPolicyPtr eq = nullptr);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;   ///< the roots
    NodeKind kind() const override { return NodeKind::Compute; }
};

} // namespace dag::aad
```

In use:

```cpp
// ∂price/∂spot and ∂hedge/∂spot, from one tape and one sweep:
auto dspot = aad::TangentNode::make("dspot", {price, hedge}, {{spot, 1.0}});
engine.addOutput<std::vector<double>>(dspot, [](const std::vector<double>& t) {
    // t[0] = ∂price/∂spot, t[1] = ∂hedge/∂spot
});
```

Everything the two nodes share follows `GradientNode`. The value is a `std::vector<double>`,
published through a `ValueSlot` and the equality policy. The inputs are the roots, and a seed's
node is not one, as a wrt node is not. The mode is `Eager`, fixed in the class, for the same
reason. `eval()` throws what the tape throws. The seeds are constants, given at `make()`.

### One tape, each root recorded right after its pull

`eval()` pulls each root in turn, and the tape records each root right after its pull, not after
all of them. After a root's pull, every node the tape records for it is clean, and a later pull
does not evaluate a clean node again. So a later root's pull evaluates only nodes the tape does not
hold, such as a stale node on a branch not taken. When such a node tells a `ConditionNode` that it
changed, no value the tape recorded moves: the `ConditionNode` holds its taken branch's value. One
forward sweep of the one tape then serves every root.

The recording is a new private `Tape::add(root)`, and `Tape(roots)` becomes `add()` for each root
in turn. Its behaviour does not change: the constructor already checked each root as it reached
it, and recording evaluates nothing, so checking one root at a time is checking all of them at
once. `add()` is private because between two calls only the next root's pull may move the graph.
A caller that moved an input between two calls would get a tape that mixes two points, silently.
`TangentNode` is a friend of `Tape`.

The one exception is a node that is always dirty: any pull evaluates it again, including the
tape's own pulls. The tape then records partials at slightly different points. A pass already has
this caveat, and the node adds nothing to it.

Rejected:

- **Pull the roots until they settle, then record once,** as flywheel-dag#17 suggested. The loop
  needs a bound, and an answer for when it is reached, and an always-dirty node never lets the
  roots settle. Recording each root right after its pull needs neither.
- **A tape per root:** N recordings and N sweeps, where one of each serves. One `GradientNode` per
  root already gives that.

### A sensitivity node is clean only when its roots are

After recording, a root can be dirty again: a later root's pull left it so, or the tape pulled an
always-dirty node on its path. If the node marked itself clean then, the next change upstream
would stop at that root, which is already dirty, and never reach the node. So each node marks
itself clean only when every root is clean, and otherwise stays dirty. Its published value was
right when published. It recomputes on its next evaluation, from an engine's next cycle or from a
consumer's pull.

`GradientNode` takes the same rule, which fixes flywheel-dag#19.

It costs one more recording, at the node's next evaluation, after a later root's pull has left an
earlier one dirty. With an always-dirty node upstream, the node records on every evaluation, as
every consumer of such a node recomputes whenever it is pulled.

### What does not change

The only existing `eval()` path that changes is `GradientNode`'s: it stays dirty while its root is.
`bench_hot_path` uses no AAD, so `--invariants` is unchanged and there is nothing to time.

### Prototype

A scratch prototype on `be4339c`, built with Apple Clang 21 and `-Wall -Wextra -Wpedantic -Werror`,
patched copies of `dag_aad.hpp` and `dag_aad.inl` with `Tape::add()`, the node and the rule. It
compiled without a warning, and 8 tests passed:

- On each graph in `aad_test_graphs.hpp`, with seeds on two inputs, the node matched
  `aad::tangents()` bit for bit.
- Seeded along x, the roots x·y and x + y gave (3, 1) at (2, 3) and (2, 1) at (3, 2) through an
  engine, while x·y's own callback fired once.
- Two roots over one `DifferentiableNode` ran one dual call per recording.
- On flywheel-dag#18's graph, the node recorded both roots, and stayed dirty along with the root
  that the other root's pull had dirtied. After x moved, it gave the right tangent, 2e². A tape
  recorded after both pulls threw.
- With an always-dirty node above both roots, `eval()` returned the right tangents and the node
  stayed dirty. After x moved, it gave the new ones.
- Made to mark itself clean regardless of its roots, or to pull every root before recording, the
  node failed both of the last two tests.
- Built against the v0.1.8 headers, a `GradientNode` whose tape pulls an always-dirty node
  returned {12} after x moved, where {20} is right. With the rule, it returned {20}.

The prototype was a standalone file, not the test suite, and no GCC build saw it. It also found
that a node that pulls each input once can end clean while an input is dirty again, and miss every
later change. That is an engine bug of its own, flywheel-dag#18.

### Not in this plan

- **flywheel-dag#18,** the engine bug above. The rule here protects the two sensitivity nodes
  from it, and no other node.
- **A direction that comes from graph nodes.** The seeds' tangents are constants given at `make()`.
- **An allocation-free recompute.** flywheel-dag#13.
- **Higher-order derivatives.**

### Release

**v0.1.9.** It adds a node, and fixes `GradientNode` in a corner case, which changes nothing a
consumer calls.

## Steps

Every commit subject takes the `type(#N): …` form, scoped to the issue its step closes:
flywheel-dag#19 for Step 1 and flywheel-dag#17 after that. The subjects below leave the scope out.
Every checkpoint runs a full build with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, including
`bench_hot_path`, and a full ctest run, and both must pass. `bench_hot_path --invariants` must
match the committed file. Each step is pushed, and CI must be green on both legs before the next
step starts.

Each test below that guards a specific mistake must be seen to fail before it is trusted: the step
lists the change to make by hand, then revert.

### Step 1 — A gradient node stays dirty while its root is

- `dag_aad.inl`: `GradientNode::eval()` marks the node clean only when its root is clean.
  `dag_aad.hpp`: the class comment says so, and why.
- `tests/aad_test_graphs.hpp`: an always-dirty test node, `AlwaysFiring`. Its `dirty()` is always
  true, its `eval()` tells its consumers every time, and its value is a constant, so a test can
  name the exact gradient.

Tests, in `tests/test_aad_gradient_node.cpp`:

| Test | Asserts |
|---|---|
| `AadGradientNode.StaysDirtyWhileItsRootIs` | Over `AlwaysFiring`·x·x at x = 3: {12}, and the root and the node are dirty. After x moves to 5, the node is still dirty, and gives {20} |

| Change made by hand, then reverted | Must fail |
|---|---|
| Mark the node clean regardless of its root | `StaysDirtyWhileItsRootIs` |

**Done when:** ctest is green (34 of 34), `--invariants` matches, the hand-made change fails as
stated, and CI is green on both legs.

Commit: `fix: a gradient node stays dirty while its root is`.

### Step 2 — `aad::TangentNode`

- `dag_aad.hpp` and `dag_aad.inl`: the private `Tape::add()`, with `Tape(roots)` calling it; the
  node; and `TangentNode` in the header comment's "Sensitivities as nodes" section.
- `tests/CMakeLists.txt`: a new suite, `test_aad_tangent_node`, so ctest goes from 34 to 35.

Tests: `tests/test_aad_tangent_node.cpp`.

| Test | Asserts |
|---|---|
| `AadTangentNode.MatchesTheTape` | On each graph in `aad_test_graphs.hpp`, with seeds on two inputs, the node's value equals `aad::tangents()` bit for bit, and the analytic directional derivative to 1e-13 |
| `AadTangentNode.DeliversTangentsThatMoveWhileTheValuesStandStill` | In an engine, the roots x·y and x + y seeded along x: the node delivers (3, 1) at (2, 3) and then (2, 1) at (3, 2). x·y's own callback fires once, with 6. This is the issue's criterion |
| `AadTangentNode.OneTapeServesEveryRoot` | Two roots over one `DifferentiableNode` that counts its dual calls: one call per recording |
| `AadTangentNode.RecordsOnlyWhenSomethingUpstreamFired` | A clean `eval()` returns the same `ValuePtr` and runs no dual call. After an input moves, it runs one |
| `AadTangentNode.AnUnchangedTangentIsNotDelivered` | The roots x + k and x − k seeded along x hold (1, 1): three cycles that move x deliver it once |
| `AadTangentNode.ARootLeftDirtyByAnotherKeepsTheNodeDirty` | flywheel-dag#18's graph: after y moves, the node records both roots, and it and the earlier root are dirty. After x moves, it gives the right tangent. A tape recorded after both pulls throws `std::invalid_argument` |
| `AadTangentNode.AnAlwaysDirtyNodeKeepsItDirty` | `AlwaysFiring` above both roots: `eval()` returns, with the right tangents, and the node stays dirty. After x moves, it gives the new ones |
| `AadTangentNode.EvaluatesOnlyWhatItsRootsDo` | A seed on a counted node on the branch a `ConditionNode` did not take: the node records, as its dual calls show, the counted functor does not run, and that seed moves nothing |
| `AadTangentNode.ThrowsWhatItsTapeThrows` | A seed above a barrier: `std::domain_error`, and the node stays dirty. An `Input<int>` root: `std::invalid_argument` |
| `AadTangentNode.RejectsEmptyOrNullRootsOrSeeds` | `make()` throws `std::invalid_argument` |
| `AadTangentNode.ItsInputsAreItsRoots` | `inputs()` is the roots. The node is not an `IDifferentiable`, and an engine with only the node registered discovers a stateful node above a root |

| Change made by hand, then reverted | Must fail |
|---|---|
| Mark the node clean regardless of its roots | `ARootLeftDirtyByAnotherKeepsTheNodeDirty` and `AnAlwaysDirtyNodeKeepsItDirty` |
| Pull every root, then record one tape | `ARootLeftDirtyByAnotherKeepsTheNodeDirty` and `AnAlwaysDirtyNodeKeepsItDirty` |
| Record a tape per root, and sweep each | `OneTapeServesEveryRoot` |
| Make the node `Lazy` | `DeliversTangentsThatMoveWhileTheValuesStandStill` |

**Done when:** ctest is green (35 of 35), `--invariants` matches, each hand-made change fails as
stated, and CI is green on both legs.

Commit: `feat: a node whose value is a forward sweep's tangents`.

### Step 3 — Docs and release

- `README.md`: the Derivatives section gains `TangentNode` beside `GradientNode`. The headers
  table's `dag_aad.hpp` row gains it.
- `CLAUDE.md`: the sensitivity-node paragraph covers both nodes. It gains the one-tape recording,
  and the rule that a sensitivity node is clean only when its roots are, with why. Its bullet
  saying there is no forward-mode node goes. The `InvalidationMode` table's `Eager` row, the list
  of modes fixed in a class and `NodeBase`'s list of derived classes gain `TangentNode`.
- Release v0.1.9 as v0.1.8 was released: the project version and the FetchContent snippets in
  `CMakeLists.txt` and the README, the tag, and a GitHub release with notes.
- Mark this plan done, and close flywheel-dag#17 and flywheel-dag#19 with a summary comment each.

**Done when:** ctest is green (35 of 35), CI is green on the release commit, the release is
published, and flywheel-dag#17 and flywheel-dag#19 are closed.

Commits: `docs: describe the tangent node`, `build: release v0.1.9` and
`docs: mark the plan done`.

## Self-review — risks and assumptions

- **A node that stays dirty after `eval()` is new here.** Only an application's always-dirty node
  did so before. The engine reads `dirty()` only for its pre-cycle snapshot, so the node is
  evaluated again on the next cycle, finds the same value, and fires nothing. A consumer that pulls
  it recomputes it first. The cost is a recording, not a wrong value.
- **The one-tape argument depends on a clean node staying unevaluated.** Two things break that:
  an always-dirty node, covered above, and `EvalContext::forceRecompute`, which re-evaluates every
  node a pull reaches. Under `forceRecompute`, a stateful node ticks again at the later root's
  pull, and the tape then mixes its two values. `forceRecompute` already advances stateful nodes on
  every pull.
- **flywheel-dag#18 stays open.** Other nodes can still end clean with a dirty input. Only the two
  sensitivity nodes check their roots. Where that bug has left a node above a root stale, the
  root's value is stale too, and the tape records what the root holds.
- **Cost.** A recompute records one tape over every root's nodes, and costs about 20 evaluations of
  them, as for `GradientNode`. The node recomputes on every change upstream of any root.
- **A seed's tangent is fixed at `make()`.** A different direction needs another node.
- **GCC is unverified.** The prototype was built with Apple Clang alone. CI's GCC 13 leg sees the
  code first at Step 1. flywheel-dag#7 moves CI to Ubuntu 26 on 2026-10-19, which may land during
  this plan and bring a newer GCC.
- **Assumptions:** the engine stays header-only C++17. CI stays on `ubuntu-latest` and
  `macos-latest`.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — A gradient node stays dirty while its root is | Done | ctest 34 / 34, and `--invariants` is unchanged. The build prints no warnings with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`. `test_aad_gradient_node` has 12 tests: the 11 it had and `StaysDirtyWhileItsRootIs`. `AlwaysFiring` lives in `aad_test_graphs.hpp`, for Step 2's suite to share. Marking the node clean regardless of its root failed `StaysDirtyWhileItsRootIs` alone, as stated. CI run 36696404748 is green on both legs, at 34 / 34 with no compiler warnings |
| 2 — `aad::TangentNode` | Done | ctest 35 / 35, and `--invariants` is unchanged. The build prints no warnings. With `Tape(roots)` calling the new `add()`, and before the node existed, every AAD suite and `example_aad` passed unchanged. `test_aad_tangent_node` has the plan's 11 tests. The four hand-made changes each failed exactly the tests stated, and no others. CI run 36697477898 is green on both legs, at 35 / 35 with no compiler warnings |
| 3 — Docs and release | Not started | |
