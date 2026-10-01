# A Node Is Clean Only When Every Input It Read Still Is

**Status: Approved (2026-10-01).**

Closes [flywheel-dag#18](https://github.com/tomlin256/flywheel-dag/issues/18).

## Problem

`NodeBase::propagate()` stops a cascade at the first node that is already dirty. That is safe
only while a dirty node's consumers are dirty too, so that there is nothing left below it to
reach. Put the other way round: a clean node's inputs are clean. `Tape::add()` relies on the
same thing: "a clean node's named inputs are clean too".

A node's `eval()` breaks it when an input the node has already read goes dirty again before the
evaluation ends. The node marks itself clean over a dirty input, and every later change stops at
that input. Two things make an input go dirty again inside a consumer's evaluation:

1. **The branch a `ConditionNode` did not take.** The node hears both branches but pulls only the
   one it took, so a node on the other branch can be stale. When a later pull evaluates that node
   and its value moves, it tells the `ConditionNode`, which marks everything below it dirty again.
   This is flywheel-dag#18's graph.
2. **An always-dirty node reached by two paths,** such as an application's clock-driven node. The
   second path's pull evaluates it again, and it tells the first path's nodes. flywheel-dag#19
   fixed this in `aad::GradientNode` and `aad::TangentNode` alone, by checking their roots, and it
   left the nodes over them exposed.

A probe on v0.1.11:

| Graph | Consumer | v0.1.11 | Right |
|---|---|---|---|
| flywheel-dag#18's: y moves to 9, then x to 2 | `ComputeNode` (`Eager` and `Lazy`), `InPlaceComputeNode`, `TweakableComputeNode`, `MemoizedComputeNode`, `aad::DifferentiableNode<2>`, a binary op | 2e + 4 | 2e² + 4 |
| the same | an n-ary op | 2e² + 4 | 2e² + 4 |
| the same, with a node over c | a unary op, an `EWMANode`, a `ComputeNode`, a `ConditionNode` | stale, as c is | fresh |
| the same, through an engine | c registered as an output | 2 callbacks: x = 2 never arrives | 3 callbacks |
| an always-dirty k reaching c through −(k·x) and k + 1; x moves from 3 to 5, then 7 | `ComputeNode` | −3, −3 | −7, −11 |
| `AadGradientNode.StaysDirtyWhileItsRootIs`'s, with a node over the gradient node; x moves to 5 | `ComputeNode` | 12 | 20 |

The n-ary op is right only because its fold pulls every input a second time, which cleans the
first again. Through the always-dirty node, it is stale too.

`dag.inl` says the branch not taken "can cost a spurious recompute after a switch, never a stale
value". On v0.1.11 that is false.

**Done when** every graph above gives the right value, through an engine as well; a move of the
branch a `ConditionNode` did not take reaches none of its consumers; a node whose evaluation
leaves an input it read dirty again stays dirty until it evaluates again; a graph's behaviour
changes in the ways listed under "What changes for a graph" and no others; the chain row of
`bench_hot_path` moves by no more than noise; and `--invariants` is unchanged.

## The two directions

The issue offers two directions. This plan takes both, one step each, and changes the second
one's mechanism. Each was built and measured in a scratch prototype (see Prototype):

| | Stale, through a `ConditionNode` | Stale, through an always-dirty node | Needless evaluations | Existing tests |
|---|---|---|---|---|
| 1 — the `ConditionNode` ignores the branch it did not take | fixed | still stale | fewer than v0.1.11 | three AAD tests pin v0.1.11's behaviour |
| 2 as the issue words it — a node is clean only if each input's `dirty()` is false | fixed | fixed | more than v0.1.11 | two AAD tests throw |
| 2 reworded — a node is clean only if it heard no "maybe" while it evaluated | fixed | fixed | more than v0.1.11 | all pass |
| **1 and 2 reworded — this plan** | fixed | fixed | fewer than v0.1.11 | the same three AAD tests |

- **Direction 2 as worded** asks each input for `dirty()`. An always-dirty node's `dirty()` is
  always true, so none of its direct consumers could ever be clean, nor anything below them.
  `AadGradientNode.StaysDirtyWhileItsRootIs` and `AadTangentNode.AnAlwaysDirtyNodeKeepsItDirty`
  both threw "aad::Tape: root … is dirty", because their roots read the always-dirty node
  directly. It also costs a virtual call per input on every evaluation. Rejected.
- **Direction 2 reworded** lets the cascade tell the node instead. An input that goes dirty again
  after the node read it sends a "maybe" cascade, which reaches the node while the node is still
  evaluating, and which the guard used to drop. A node that heard one does not end clean. It costs
  a store and a load per evaluation. An always-dirty node's direct consumer is untouched: the
  always-dirty node says "changed", not "maybe", on every pull, as it does today.
- **Direction 2 alone** leaves the `ConditionNode` hearing its untaken branch. Each move of that
  branch during an evaluation then leaves the graph below it dirty, and it evaluates once more at
  the next pull or engine cycle, even when nothing it depends on moved. In the prototype, an
  `Eager` node below c ran on engine steps where nothing moved: {1, 1, 1, 1, 1, 0} runs per step
  where {1, 0, 1, 0, 1, 0} is right. An `EWMANode` below it ticked on steps that moved only an
  unrelated input: {1, 1, 1, 1, 1, 1} ticks where {1, 1, 0, 1, 0, 1} is right.
- **Direction 1 alone** fixes flywheel-dag#18's graph exactly. It also removes work v0.1.11 does
  for nothing: an `Eager` node below a `ConditionNode` recomputes, and a stateful one ticks, when
  the untaken branch moves (1 run in the prototype, where 0 is right). The always-dirty node's
  consumer stays stale.
- **Both:** direction 1 removes the engine's own source of an input going dirty again. Direction
  2 guards the sources that remain, an application's always-dirty node and
  `EvalContext::forceRecompute`.

## Design

### Step 1 — A `ConditionNode` hears only the branch it took

Each branch is wired to a listener of its own instead of to the node. A listener passes an
invalidation on to the node only when its branch is the one the node's last `eval()` took. The
condition is wired to the node directly, as now.

```cpp
class ConditionNode : public NodeBase, public aad::IDifferentiable,
                      public std::enable_shared_from_this<ConditionNode> {
    // ... public interface unchanged ...
private:
    // Hears one branch for the node, and passes on only what the branch the
    // node took says. Not a node: nothing names it as an input.
    class BranchListener : public NodeBase {
        // eval() throws std::logic_error; inputs() is empty.
        void propagate(Dirtiness incoming) override;   // forwards when taken
        std::weak_ptr<ConditionNode> owner_;
        const bool branch_;
    };

    std::optional<bool> taken_;   ///< the branch eval() last pulled
    std::shared_ptr<BranchListener> onTrue_, onFalse_;
};
```

- `eval()` records `taken_` once it has read the condition, before it pulls the branch.
- `taken_` can be out of date while the node is dirty, with a changed condition still to be read.
  That does no harm: a dirty node's guard drops what a listener passes on anyway, and its next
  `eval()` reads the condition again. Before the first `eval()`, `taken_` is empty, the listeners
  pass nothing on, and the node is dirty from construction.
- The node owns its listeners, and each listener holds a `weak_ptr` to the node, so neither keeps
  the other alive.
- `inputs()` and `partials()` do not change, so traversal, stateful-node discovery, graph export
  and a tape see the same graph as before.
- One node on both branches has two listeners, and the one for the branch taken passes it on.
- Rejected: **telling the untaken branch's invalidation apart by state,** by checking on its
  arrival whether the condition or the taken branch is dirty. It relies on every sender being
  dirty when it sends, and a forced evaluation breaks that: a clean node publishes. A miss there
  would drop a real change. Rejected: **rewiring the node on a switch.** A node's downstream list
  is built once, at `wire()`.

### Step 2 — A node is clean only when every input it read still is

`NodeBase` records whether a "maybe" reached the node during its evaluation:

```cpp
class NodeBase : public INode {
protected:
    /// eval() calls this before its first pull.
    void beginEval() noexcept { heardMaybe_ = false; }
    /// eval() calls this in place of markClean() once it has pulled its inputs.
    /// Clean, unless an input it read went dirty again: then the node keeps the
    /// state its evaluation left it in, and tells each consumer "maybe".
    void endEval();
private:
    [[gnu::cold, gnu::noinline]] void stayDirty();   ///< the loop over consumers
    bool heardMaybe_ = false;
};
```

- `propagate()` sets `heardMaybe_` on every "maybe" it receives. `beginEval()` forgets those that
  arrived before the evaluation began: a diamond sends several while the node waits to be pulled.
- **Why a "maybe" means an input the node read went dirty again.** In an evaluation where nothing
  goes dirty again, the node's inputs only go from dirty to clean, and one whose value moved tells
  the node "changed". A "maybe" comes only from a node that went from clean to dirty, and during
  this node's evaluation that happens only when something makes an input dirty again.
- **It can be a false alarm.** An input the node had not read yet can go dirty again first, or a
  later pull can repair an input the node read, as an n-ary op's fold does. The node then stays
  dirty although what it read is fresh. That costs an evaluation and never gives a stale value.
- **Why the node keeps its state, rather than going Maybe.** An input the node read can be
  evaluated again later in the same evaluation, by another input's pull, with a new value. Its
  "changed" made this node Dirty. A `Lazy` node sent to Maybe would skip its next evaluation and
  keep the value it computed from the old one.
- **Why it tells its consumers.** One of them may be evaluating right now, having pulled this
  node. It must not end clean over it either, and the "maybe" is how it learns. The others are
  already dirty, because this node was.
- **Why the stay is out of line.** It runs only when an input went dirty again. Inlined into every
  evaluation, it cost +2.4% on the chain row. Out of line, +1.0%, within the benchmark's noise.
  `cold` and `noinline` are GNU attributes, which GCC and Clang, the two compilers CI builds with,
  both take.
- The pair goes into every node that pulls: `ComputeNode`, `InPlaceComputeNode`,
  `TweakableComputeNode` (untweaked), `ConditionNode`, the unary, binary and n-ary ops (through
  `OpNodeImpl::notifyDownstream()`), `StatefulNodeBase`, `ZScoreNode` and `OutlierGateNode`
  (through `NodeImpl::notifyDownstream()`), `MemoizedComputeNode`, `aad::DifferentiableNode`,
  `aad::GradientNode` and `aad::TangentNode`, skip paths included: 14 `beginEval()` calls and 22
  `endEval()` calls. The sources and a tweaked `TweakableComputeNode` pull nothing, and keep
  `markClean()`.
- **The sensitivity nodes' root checks go.** `endEval()` covers them, and their consumers as well,
  which the checks flywheel-dag#19 added did not.
- **A sensitivity node records a root it has just pulled, clean or not.** A root that an
  always-dirty node reaches by two paths now stays dirty after its own pull, and `Tape::add()`
  would throw on it. On v0.1.11 the root read as clean and the node gave a gradient, and
  `AadTangentNode.AnAlwaysDirtyNodeKeepsItDirty` promises that such a graph "neither hangs nor
  throws". A new private `Tape::record()` walks from a root without checking it, `add()` checks and
  then records, and `GradientNode` and `TangentNode` call `record()` for the root they just
  pulled. `GradientNode` becomes a friend of `Tape`, as `TangentNode` is. The tape's walk then
  evaluates the nodes that went dirty again, as it did on v0.1.11 and as it evaluates any
  always-dirty node it meets. The public constructor still requires clean roots.
- **An application's own node** that pulls inputs and calls `markClean()` keeps v0.1.11's
  behaviour, exposure included. `CLAUDE.md` tells node authors to use the pair.
- Rejected: **setting the flag only for a "maybe" the guard drops.** The prototype found no graph
  that tells the two apart, and its mutation to that rule failed no test. The one evaluation that
  starts clean is a forced one, and there the pull in progress also carries a "maybe" to the node,
  so the guard drops the second. Every "maybe" is the simpler rule.

### What changes for a graph

1. **A move of the branch a `ConditionNode` did not take reaches none of its consumers.** An
   `Eager` node below it no longer recomputes for it, a stateful node no longer ticks for it, and a
   sensitivity node no longer records for it. A switch still reads the branch fresh.
2. **A node whose evaluation leaves an input it read dirty again stays dirty,** and so do its
   consumers, until it next evaluates. After Step 1, only an always-dirty node reached by two
   paths, or a forced evaluation, can do that. With an always-dirty node reached by two paths, the
   node where the paths meet and everything below it end every evaluation dirty, so each is
   evaluated again on every pull and every engine cycle. That is what an always-dirty node already
   asks of the node it feeds directly, and it is new for the nodes below that one.
3. **A node over a sensitivity node now sees a later change.**

Nothing else. `--invariants` is unchanged, and every existing test passes apart from the three
AAD tests Step 1 rewrites.

### Prototype

A scratch prototype on v0.1.11 (`211f1a0`), built with Apple Clang 21 at `-Wall -Wextra
-Wpedantic -Werror`: patched copies of `dag.hpp`, `dag.inl`, `dag_ops.inl`, `dag_timeseries.inl`,
`dag_memoize.inl`, `dag_aad.hpp` and `dag_aad.inl` with both steps, and each direction on its own.
It compiled without a warning, and:

- Every existing ctest entry passed, once the three AAD tests Step 1 rewrites were rewritten.
- A prototype of the new suite had 56 tests: the tests below, without the three kinds that take
  the reference path, and with the skip test run over all 12 pair kinds. On v0.1.11, the 37 that
  assert a later change arrives, or that the untaken branch's move reaches no consumer, failed.
  The 19 that assert what v0.1.11 already does passed. At Step 1 alone, the 34 of Step 2 failed.
  With both steps, all 56 passed.
- The rewritten AAD tests failed on v0.1.11, and passed with Step 1 and with both steps.
- Every hand-made change in Step 1's table, and in Step 2's, failed exactly the tests stated
  across every suite. For the per-node rows, that was checked for `ComputeNode`'s skip on its copy
  path, the ops' and the time-series nodes' shared publish, the `ConditionNode`'s last line and
  skip, the unary op's skip and `ZScoreNode`'s `beginEval()`.
- `bench_hot_path`, 15 interleaved runs each, chain row median: 257.9 ns/cycle on v0.1.11, 260.5
  with both steps (+1.0%), and 264.0 with the stay inlined (+2.4%). The idle-queues and ingest
  rows did not move beyond noise. `--invariants` matched.
- Direction 2 as worded failed `AadGradientNode.StaysDirtyWhileItsRootIs` and
  `AadTangentNode.AnAlwaysDirtyNodeKeepsItDirty` with "aad::Tape: root … is dirty".
- Without `Tape::record()`, a `GradientNode` and a `TangentNode` over k·x·x − (k + 1), with k
  always dirty, threw that error. With it, they gave 12 at x = 3 and 20 at x = 5.

No GCC build saw the prototype.

### Not in this plan

- **An always-dirty node that passes invalidations on as "changed".** `NodeBase` gives an override
  of `propagate()` two ways to pass one on: `NodeBase::propagate()`, whose guards such a node must
  avoid, and `notifyDownstream()`, which says "changed". If a node above an always-dirty node went
  dirty again during a consumer's evaluation, the consumer would hear "changed" and end clean.
  After Step 1, that needs a second always-dirty node, or a forced evaluation, above the first.
  `CLAUDE.md` names it as a limit.
- **`forceRecompute` re-evaluating a stateful node on every pull.** Unchanged, and documented
  already.

### Release

**v0.1.12.** It fixes a stale value, and it changes what reaches a `ConditionNode`'s consumers,
which the release notes say.

## Steps

Every commit subject is scoped to flywheel-dag#18 in the repo's `type(#N): …` form. The subjects
below leave the scope out.
Every checkpoint runs a full build with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, including
`bench_hot_path`, and a full ctest run, and both must pass. `bench_hot_path --invariants` must
match the committed file. Each step is pushed, and CI must be green on both legs before the next
step starts.

Each test below that guards a specific mistake must be seen to fail before it is trusted: the step
lists the change to make by hand, then revert. After reverting, `touch` the file before the
checkpoint build: the local Make compares timestamps to the second.

### Step 1 — A `ConditionNode` hears only the branch it took

- `dag.hpp`: the listeners and `taken_`. The class comment says what the node hears, and why.
- `dag.inl`: `make()` wires the condition to the node and each branch to its listener; `eval()`
  records `taken_`. The two comments that promise "never a stale value" say what is true now.
- `tests/CMakeLists.txt`: a new suite, `test_clean_inputs`, so ctest goes from 35 to 36.

Tests, in the new `tests/test_clean_inputs.cpp`:

| Test | Asserts | v0.1.11 |
|---|---|---|
| `UntakenBranch.TheIssuesSequenceSeesTheLaterChange` | The issue's code: after y = 9, c is 2e + 4 and root1 is clean. After x = 2, c is 2e² + 4 | 2e + 4 after x = 2 |
| `UntakenBranch.AnEngineDeliversTheLaterChange` | x and y as `AsyncInput`s, c an output: callbacks 2e + 3, 2e + 4 and 2e² + 4 | two callbacks |
| `UntakenBranch.ItsMoveReachesNoConsumer` | The untaken branch, evaluated once and clean, moves: the `ConditionNode` and an `Eager` node below it stay clean, and the `Eager` node's functor does not run. A switch then gives the branch's new value | the functor runs |
| `UntakenBranch.OneNodeOnBothBranchesIsHeardEitherWay` | With one node on both branches, its move arrives under either condition | passes |
| `UntakenBranch.ItsListenersDoNotKeepItAlive` | A released `ConditionNode` expires, and a later move of its branch does nothing | passes |

Three AAD tests pinned v0.1.11's behaviour on the path this step removes: a move of the branch not
taken reached the sensitivity node. Each keeps its purpose, with a trigger that still exists:

| Test | v0.1.11 | After |
|---|---|---|
| `AadGradientNode.EvaluatesOnlyWhatItsRootDoes` | a move of y, on the branch not taken, made the node record | y's move leaves the node clean. x moving away and back makes it record, with the old assertions: one dual call, the counted functor not run and still dirty, the same gradient pointer |
| `AadTangentNode.EvaluatesOnlyWhatItsRootsDo` | the same | the same |
| `AadTangentNode.ARootLeftDirtyByAnotherKeepsTheNodeDirty`, renamed `ABranchNotTakenLeavesNoRootDirty` | on flywheel-dag#18's graph, root2's pull left root1 dirty, the node stayed dirty, and a tape recorded after both pulls threw | root1 and the node end clean, x's move reaches the node, and a tape recorded after both pulls holds both roots |

Nothing is weakened: each now asserts the behaviour this step makes true, and each fails on
v0.1.11. What the last one guarded, a node staying dirty while a later root's pull leaves an
earlier root dirty, `AadTangentNode.AnAlwaysDirtyNodeKeepsItDirty` still guards: there, the pull
of k·x·x leaves k·x dirty.

| Change made by hand, then reverted | Must fail |
|---|---|
| A listener passes on its branch's invalidations whichever branch was taken | `TheIssuesSequenceSeesTheLaterChange`, `AnEngineDeliversTheLaterChange`, `ItsMoveReachesNoConsumer` and the three rewritten AAD tests |
| A listener holds a `shared_ptr` to its node | `ItsListenersDoNotKeepItAlive` |

**Done when:** the first three tests fail on v0.1.11 as stated, and every test passes after the
change; the rewritten AAD tests fail on v0.1.11; each hand-made change fails exactly the tests
stated; ctest is green (36 of 36) with no compiler warnings; `--invariants` matches; CI is green
on both legs.

Commit: `fix: a ConditionNode hears only the branch it took`.

### Step 2 — A node is clean only when every input it read still is

- `dag.hpp`: `beginEval()`, `endEval()`, the out-of-line stay and `heardMaybe_`, and
  `propagate()` sets the flag. `NodeBase`'s comment states the rule, why a "maybe" is the signal,
  and that a node of one's own that pulls uses the pair.
- `dag.inl`, `dag_ops.inl`, `dag_timeseries.inl`, `dag_memoize.inl`, `dag_aad.inl`: every node
  listed under Design brackets its evaluation, skip paths included.
- `dag_aad.hpp` and `dag_aad.inl`: the private `Tape::record()`; `GradientNode` and `TangentNode`
  record their roots through it, and their root checks give way to `endEval()`. The class comments
  say why.
- `tests/test_nodes.hpp`, new: `AlwaysFiring`, moved from `aad_test_graphs.hpp` into namespace
  `test_nodes`, and `Tripwire`, which is always dirty, holds a constant, and tells its consumers
  once, on its n-th pull after `arm(n)`. The two AAD suites that use `AlwaysFiring` include it.

The new tests, in `test_clean_inputs.cpp`, run on three graphs:

- **Two paths:** an `AlwaysFiring` k reaches c through j = −(k·x) and through k + 1. Every
  evaluation of c leaves j dirty again.
- **One shot:** the same shape over a `Tripwire`. Once c has settled, an input moves that changes
  no value c reads, and the tripwire fires on its next pull, which is the pull of k + 1, after c
  has read j. A `Lazy` node takes its skip path.
- **A diamond:** x feeds −x and eˣ, and both feed c.

The node kinds, as parameters:

- **Pair kinds (12),** each c over two inputs: `ComputeNode`, `InPlaceComputeNode`,
  `TweakableComputeNode`, `MemoizedComputeNode` and `aad::DifferentiableNode<2>`, each `Eager` and
  `Lazy`, a binary op and an n-ary op.
- **Lazy pair kinds (10):** the seven of those that can skip, and `ComputeNode`,
  `InPlaceComputeNode` and `TweakableComputeNode` again, `Lazy`, over an input that is not
  trivially copyable, which takes their other pull path.
- **Top kinds (5),** each over a `ComputeNode` c: a unary op, a `ConditionNode` that takes c, an
  `EWMANode` with α = 1, a `ZScoreNode` and an `OutlierGateNode` whose threshold passes c through.
- **Lazy top kinds (2):** the unary op and the `ConditionNode`. The other three are time-series
  nodes, which are `Eager` and never skip.

| Test | Asserts | At Step 1 |
|---|---|---|
| `CleanInputs/PairKind.StaysDirtyWhileAnInputItReadIs` ×12 | Two paths: c's value is right, and c and j are dirty. After x = 5, then 7, c is right | stale |
| `CleanInputs/LazyPairKind.StaysDirtyWhenItSkips` ×10 | One shot: c's value is right, and c and j are dirty. After x = 5, c is right | stale |
| `CleanInputs/PairKind.AnOrdinaryEvaluationEndsClean` ×12 | Diamond: after x moves, c is right and clean | passes |
| `CleanInputs/TopKind.StaysDirtyWithItsInput` ×5 | Two paths: the top node is dirty with c, and after x = 5 it is right. The z-score's value is not checked | stale |
| `CleanInputs/LazyTopKind.StaysDirtyWithItsInputWhenNothingMoved` ×2 | One shot: the top node skips and stays dirty with c. After x = 5 it is right | stale |
| `CleanInputs/TopKind.AnOrdinaryEvaluationEndsClean` ×5 | Diamond below c: the top node is clean after x moves | passes |
| `CleanInputs.AnEngineDeliversTheLaterChange` | Two paths with x an `AsyncInput`: callbacks −3, −7 and −11 | −3 only |
| `CleanInputs.ASensitivityNodesConsumerStaysDirtyWithIt` | `StaysDirtyWhileItsRootIs`'s graph with a `ComputeNode` over a `GradientNode` and one over a `TangentNode`: 12, both dirty, and 20 each after x = 5 | 12 |
| `CleanInputs.AForcedEvaluationStaysDirtyWhileAnInputItReadIs` | One shot, settled, then a forced evaluation with the tripwire set for the pull of k + 1: j and c are dirty, and after x = 5 an ordinary evaluation is right | stale |
| `AadGradientNode.RecordsARootThatStaysDirty` | Over k·x·x − (k + 1), with k an `AlwaysFiring`: {12}, and the root and the node dirty. After x = 5, {20} | passes |
| `AadTangentNode.RecordsARootThatStaysDirty` | The same root, seeded along x | passes |

| Change made by hand, then reverted | Must fail |
|---|---|
| `endEval()` marks the node clean whatever it heard | every `StaysDirty…` instance, `AnEngineDeliversTheLaterChange`, `ASensitivityNodesConsumerStaysDirtyWithIt`, `AForcedEvaluationStaysDirtyWhileAnInputItReadIs`, `AadGradientNode.StaysDirtyWhileItsRootIs`, `AadGradientNode.RecordsARootThatStaysDirty`, `AadTangentNode.AnAlwaysDirtyNodeKeepsItDirty` and `AadTangentNode.RecordsARootThatStaysDirty` |
| `beginEval()` forgets nothing | every `AnOrdinaryEvaluationEndsClean` instance, `UntakenBranch.ItsMoveReachesNoConsumer`, and ten tests in other suites: `AadGradientNode.EvaluatesOnlyWhatItsRootDoes` and `.ThrowsWhatItsTapeThrows`, `AadTangentNode.EvaluatesOnlyWhatItsRootsDo`, `.ABranchNotTakenLeavesNoRootDirty` and `.ThrowsWhatItsTapeThrows`, `AadReverse.APassDoesNotAdvanceAStatefulNode`, `LazyInvalidation.Case8_RateLimiterForwardsMaybeAndItsLazyConsumerSkips`, `NodeStateTests.RestoreInvalidatesDownstream`, `StatefulNodeBase.EqualityPolicyComparesAgainstTheLastPublishedValue` and `ValueSlot.SteadyStateEvalAllocatesNothing` |
| In each node that pulls, in turn, `markClean()` in place of one `endEval()` | that node's own instances in `test_clean_inputs`, and nothing else. For example: `ComputeNode`'s skip on its copy path fails `LazyPairKind.StaysDirtyWhenItSkips/ComputeLazy`; the ops' shared publish fails `PairKind.StaysDirtyWhileAnInputItReadIs/BinaryOp` and `/NAryOp` and `TopKind.StaysDirtyWithItsInput/UnaryOp` |
| In each node that pulls, in turn, no `beginEval()` | that node's `AnOrdinaryEvaluationEndsClean` instances, and nothing else. For example, `ZScoreNode`'s fails `TopKind.AnOrdinaryEvaluationEndsClean/ZScore` |
| `GradientNode` and `TangentNode` record their roots through `add()` | `AadGradientNode.RecordsARootThatStaysDirty` and `AadTangentNode.RecordsARootThatStaysDirty` |

A script applies the per-node changes one at a time: 14 `beginEval()` calls and 22 `endEval()`
calls. A per-node change that fails more than stated is recorded in the Progress notes, with why.

Time it: `bench_hot_path` at Step 1's commit and after this step, 15 interleaved runs of each.
The Progress notes record the chain row's medians.

**Done when:** every test in the first table that is stale at Step 1 fails at Step 1's commit, and
every test passes after the change; each hand-made change fails as stated; the chain row's median
is within noise of Step 1's; ctest is green (36 of 36) with no compiler warnings; `--invariants`
matches; CI is green on both legs.

Commit: `fix: a node is clean only when every input it read still is`.

### Step 3 — Docs and release

- `CLAUDE.md`:
  - Key Patterns: the rule, with `beginEval()` and `endEval()`, beside lazy evaluation and in the
    `NodeBase` section, with a node author's part in it.
  - The `ConditionNode` hears only the branch it took, and why.
  - The override table's clock-driven row: reached by two paths, it leaves the node where they meet
    dirty after every evaluation, and an override that passes invalidations on as "changed" is not
    covered by the rule.
  - Algorithmic Differentiation: the `TangentNode` bullet says an always-dirty node, not a branch
    not taken, is what can leave an earlier root dirty. "A sensitivity node is clean only when its
    roots are" becomes the general rule, with `Tape::record()`. The pinned tests are renamed, and
    `aad_test::AlwaysFiring` is now `test_nodes::AlwaysFiring`.
- `README.md`: the Concepts bullet on `Eager` and `Lazy` says a `ConditionNode` passes on only
  what the branch it took says.
- Release v0.1.12 as v0.1.11 was released: the project version and the FetchContent snippets in
  `CMakeLists.txt` and the README, the tag, and a GitHub release whose notes list the behaviour
  changes.
- Mark this plan done, and close flywheel-dag#18 with a summary comment.

**Done when:** ctest is green (36 of 36), CI is green on the release commit on both legs, the
installed version file reports 0.1.12, the release is published, and flywheel-dag#18 is closed.

Commits: `docs: describe the clean-inputs rule`, `build: release v0.1.12` and
`docs: mark the plan done`.

## Self-review — risks and assumptions

- **Behaviour change 1: the branch not taken.** A graph that counted on an `Eager` or stateful node
  below a `ConditionNode` recomputing when the untaken branch moves loses that. Such a node was
  ticking on moves of a value it did not read, so this is a fix, but it is visible. The release
  notes say so.
- **Behaviour change 2: an always-dirty node on two paths.** The node where the paths meet, and
  every node below it, are now evaluated on every engine cycle. That is more work per cycle, and a
  stateful node below ticks each cycle. It was stale before. An application with such a graph sees
  it at once.
- **The per-node pair is the author's to call.** An application's node that pulls and calls
  `markClean()` keeps working as before, with v0.1.11's exposure. Forgetting `beginEval()` costs
  evaluations, never a value: the node stays dirty after every evaluation that followed a "maybe".
- **False alarms** cost an evaluation, never a value. After Step 1, only an always-dirty node or a
  forced evaluation raises one.
- **A tape may evaluate.** `Tape::record()` lets a sensitivity node record a root that stayed
  dirty. Its walk then evaluates the nodes that went dirty again. That is v0.1.11's behaviour for
  those graphs, where the root read as clean, and it happens only with an always-dirty node.
- **A guard weakens.** `TangentNode` records each root right after its pull. With `record()`,
  recording them all after every pull would no longer throw, and no test would catch the change:
  after Step 1, only an always-dirty node leaves an earlier root dirty, and there the documented
  caveat already allows partials at slightly different points. `CLAUDE.md` keeps the reason for
  the order.
- **The listeners.** Two allocations per `ConditionNode`, at `make()`, and one more hop, a
  `weak_ptr` lock, per invalidation from a branch. No benchmark row has a `ConditionNode`.
- **Threads.** `heardMaybe_` is written where `state_` is, on the eval thread, so nothing new is
  shared. `Input::set()` from another thread during a cycle was outside the thread model already.
- **GCC is unverified.** The prototype was built with Apple Clang alone. CI's GCC 13 leg sees the
  attributes first at Step 2. flywheel-dag#7 moves CI to Ubuntu 26 on 2026-10-19, which may land
  during this plan and bring a newer GCC.
- **Assumptions:** the engine stays header-only C++17. CI stays on `ubuntu-latest` and
  `macos-latest`, which build with GCC and Clang, so the GNU attributes need no guard.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — A `ConditionNode` hears only the branch it took | Done | ctest 36 / 36, and `--invariants` is unchanged. The build prints no warnings with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`. `test_clean_inputs` has the five `UntakenBranch` tests. Against v0.1.11's headers the first three failed as stated: c stayed 2e + 4 with root1 dirty, the engine made two callbacks, and the `Eager` node's functor ran. The other two passed there. The three rewritten AAD tests failed against v0.1.11's headers too. Each hand-made change failed exactly the tests stated, across every suite |
| 2 — A node is clean only when every input it read still is | Not started | |
| 3 — Docs and release | Not started | |
