# Close Out the Differentiable Subgraphs Milestone

**Status: Done (2026-11-15).** Approved as written, so D1 to D14 stood at their defaults, and D15 was put to you
with its figures before the tag. v1.4.0 is released, the five issues are closed and the milestone is closed. The
Outcome at the end records what each step found on the committed files and where the work differs from this text.
Everything under "Measured" was taken read-only at `ff19003`, in scratch copies and by hand-made builds outside the
checkout. The prototypes were kept outside the checkout, and the code was written again from this plan, test first.

Closes [flywheel-dag#11](https://github.com/tomlin256/flywheel-dag/issues/11),
[flywheel-dag#13](https://github.com/tomlin256/flywheel-dag/issues/13),
[flywheel-dag#74](https://github.com/tomlin256/flywheel-dag/issues/74),
[flywheel-dag#75](https://github.com/tomlin256/flywheel-dag/issues/75) and
[flywheel-dag#76](https://github.com/tomlin256/flywheel-dag/issues/76), the five open issues of the
[Differentiable subgraphs milestone](https://github.com/tomlin256/flywheel-dag/milestone/4): "a subgraph used as a
function that can be called, differentiated and run without allocating: a reduce API, derivatives through stateful
time-series nodes, an allocation-free AAD pass". One plan for the five, because they change two classes between them
(`aad::Tape` and `dag::Reduction`), each one's answer constrains the next, and they want one release.

| Issue | The change | What holds it afterwards |
|---|---|---|
| flywheel-dag#11 | Nine `dag::ts` nodes give a tape their partial at the current state, through a new virtual of `NodeBase`, and six of them give a `Reduction` a step: the node's last update, applied to another sample | `test_aad_timeseries` and `test_dag_reduce_timeseries`, new; no node class grows, which a `static_assert` and the size table hold |
| flywheel-dag#13 | `aad::Tape` keeps its storage and what it has read of the graph's structure: `record()` records again, and two sweeps write into a caller's span. `GradientNode` and `TangentNode` keep one tape | `test_aad_tape`, new; two benchmark rows, `gradient` and `tangent`, whose `allocs/cycle` is 0 |
| flywheel-dag#75 | `Reduction` takes several roots: one call runs each step once, and `jacobian()` sweeps whichever way is fewer | `test_dag_reduce_roots`, new |
| flywheel-dag#76 | A `ConditionNode` whose condition an argument moves is followed when the branch the graph did not take is clean, and refused, as now, when it is not: a reduction still evaluates nothing | `test_dag_reduce_choice`, new |
| flywheel-dag#74 | `dag::ReductionNode<Out, Ins...>` owns a reduction, binds it at each evaluation and hands it to a functor | `test_reduction_node`, new |

Six things turned out differently from how the issues read, and each is under "Measured":

- **A stateful node cannot join a tape the way an op does without growing.** `aad::IDifferentiable` is a mixin, and a
  mixin is 8 bytes on every node of the class. The partials come through a virtual of `NodeBase` instead, as the
  reduction's hooks do, and no node is larger (D3).
- **A node does not keep what its last update started from.** So a step works from the update's result, and is the
  node's value to the bit at the sample the node took and to rounding at any other. Two nodes, `DeltaNode` and
  `RateLimiterNode`, overwrite even that, and get a partial and no step (D4, D5).
- **`GradientNode` built on a kept reduction is not the answer to flywheel-dag#13.** A tape and a reduction differ in
  what a derivative with respect to an intermediate node means, in which nodes can join, and in what they refuse. A
  tape that keeps its storage has the tape's meaning, and passes the seven AAD suites unedited (D6).
- **A moved condition can be followed without evaluating anything**, which the issue did not list: where the other
  branch is clean, something else evaluated it at this point. A node that binds must be given that branch as an
  input, or it would not hear it move (D9).
- **Written the obvious way, the three reduction changes cost the calls that exist today 8% and 21%**: a
  `gradient()` and a `bind()` of a one-root reduction. Most of it comes out when today's code stays on today's
  path. What is left in the second prototype is 2 to 4% of a gradient and 1 ns an argument of a bind, and the
  benchmark's `solve` row shows none of it (D15).
- **Two things at `HEAD` that no test holds.** `TangentNode` records each root right after its pull, and its suite
  passes when it does not: Step 3 writes the test. A window of 0 is accepted by four `make()` functions, which then
  fail at an evaluation with a ring buffer's message: that is outside this plan, and is
  [flywheel-dag#80](https://github.com/tomlin256/flywheel-dag/issues/80).

## Measured

Apple M4 Pro, Apple clang 21.0.0, CMake 4.4.5, LLVM 23.1.4, tree `relwithdebinfo-macos-arm64`; `ubuntu-dev` (aarch64,
GCC 13.3.0, LLVM 23.1.6); and GCC 15.2.0, CI's compiler on Linux, in a throwaway `ubuntu:26.04` container. At
`ff19003` the Mac configures and builds with no warning, `ctest` passes 91 of 91, and `bench_hot_path --invariants` is
the committed file's. The 31 suites of `tests/` hold 751 tests.

Each prototype is a copy of the checkout with edited headers. Every suite of `tests/` was compiled against it by hand,
with the flags the tree's `compile_commands.json` gives a test, `-Werror` among them, beside new suites for the new
code. Four prototypes were written apart (the tape's side of flywheel-dag#11; flywheel-dag#13; flywheel-dag#75,
flywheel-dag#76 and flywheel-dag#74 together; the reduction's side of flywheel-dag#11) and then merged into one
copy, which is what "The whole change, once" builds under three compilers.

### flywheel-dag#11 — which derivative, and what it costs a node

**The reading.** The issue asks which of two derivatives is wanted, and the comment on it says why only one can serve
a `Reduction`: one call at a trial value has no history to differentiate through. So the prototype takes "at the
current state", and states it as one function for both uses:

> A stateful node's function is its last update, applied to another sample: what the node would hold now, had the
> sample it just took been x. The state that update started from is held fixed.

Its derivative at the sample the node took is the node's partial on a tape, and the function itself is the node's
step in a reduction. An EWMA's is α, and 1 for its first sample, which it takes whole. The other reading, through the
state over time, is a different feature, with a tangent carried in each node's state from cycle to cycle: it is not
built here (D2).

**How a node says so.** `aad::IDifferentiable` is how an op gives a tape its partials, and it is a mixin: a second
polymorphic base, 8 bytes on every node of a class that joins. The reduce plan measured that size, on three of the
eleven nodes of the benchmark's `chain` row, at 0.37% of the row, and it was taken out before the release. Six of
`chain`'s nodes are `dag::ts` nodes that would join here. So the prototype adds one virtual to `NodeBase`,

```cpp
/// This node's local partial derivatives at the values the graph holds now, written as
/// aad::IDifferentiable::partials() writes them, or false, the default, when it cannot say.
virtual bool partials(EvalContext& ctx, aad::Partials& out);
```

and a tape asks a node that derives `NodeBase` through it, and any other node through the mixin, as now. A class that
derives both, as every op does, overrides both with its one function, so nothing an op does changes.

**The partials.** Nine classes take a double and give one:

| Node | Its partial with respect to the sample it took | It names no input when |
|---|---|---|
| `EWMANode` | α, and 1 for the first sample | never |
| `RollingStats` (its value is the mean) | 1/count | never |
| `RollingSumNode` | 1 | never |
| `DeltaNode<double>` | 1 | the first sample, whose value is 0 whatever it is |
| `DelayNode<double>` | 1 for a delay of 0 | a delay of a tick or more: the value is an older sample |
| `EWMATickRateNode` | none | always: it counts evaluations and never reads the sample |
| `ZScoreNode` | 1/σ, with σ taken before the sample joined the window | σ below 1e-12, where the value is 0 |
| `OutlierGateNode` | 1 for a sample that passes, 1/count for one that is gated | never |
| `RateLimiterNode<double>` | 1 for a sample that passed | one it held back |

A node that names no input is a constant to a pass, as a tweaked node is, so a barrier above it is not one. The other
five classes (`WindowNode`, `RollingMinMaxNode`, `ThresholdNode`, `DebounceCountNode`, `LatchedDebounceNode`) hold no
double, as do the `int` instantiations of the templates, and stay barriers. `makeTimeDelayNode` is an `Eager`
`ComputeNode`, and stays one too.

Three of the nine need a fact their state does not hold: whether the last update was the first. Each gets a flag, in
bytes its state already had as padding (`EWMANodeState::blended`, `DeltaNodeState::differenced`,
`RateLimiterNodeState::passed`), written once per update and not persisted: a restored node is dirty, so it updates
before any pass can ask.

**What it does to the suite.** Against the prototype's headers, 750 of the 751 tests pass. The one that fails is
`AadReverse.APassDoesNotAdvanceAStatefulNode`, at its last line, which expects `std::domain_error` for a wrt node
above an `EWMANode`. A new suite of 14 tests passes: each partial against its closed form to 1e-13 and against a
central difference between two twin nodes that differ in their last sample alone; the chain rule through an EWMA and
a rolling mean in both sweeps; a node that names no input hiding a barrier above it; a pass that advances nothing;
and a `GradientNode` in an engine delivering 2·ewma·α through an `EWMANode`.

**What it costs the cycle.** Nothing grows: each of 29 node classes is the size it was (`EWMANode` 200 bytes,
`DeltaNode<double>` 192, `RollingStats` 288, `ComputeNode<double, double>` 200, `ops::ProductNode<>` 184), and the
three state structs are 16 bytes as before. `bench_hot_path --invariants` is the committed file's. The benchmark's
machine code, function by function, with a function that only moved counted as the same:

- Of the 909 functions that both binaries hold, 842 are the same. `Engine::cycle()`, its timing guard's destructor,
  `NodeBase::propagate()`, the four `equal()`, the six `flush()`, the four `emitImpl()` and 20 of the 22 `eval()` are
  among them.
- Of the 67 that differ, 63 differ in one immediate: where a second vtable sits in its group, which moves by 8 bytes
  because the first has one more slot. They are constructors, destructors and their thunks.
- The other four are the `eval()` of `EWMANode` and of `DeltaNode<double>`, one instruction longer each, the store
  of the flag, and their two constructors, which zero two flags where they zeroed one.
- Six functions are new: the `partials()` of the six `dag::ts` classes the benchmark holds.

The benchmark's rows, swept at the 16 alignments, are under "What the changes cost the calls that exist": with each
row run once `chain` reads 0.13% more, where a second build of `HEAD`'s own headers reads 0.02% more.

### flywheel-dag#11 — a reduction through a stateful node

**The steps.** A step needs what its node's last update started from, and a node keeps only what the update left. So
the prototype adds one hook to `IReduceStep`,

```cpp
/// Reads from the node whatever the step needs that is not an input's value: a stateful node's
/// state, and the sample its last update took. A Reduction calls it at each bind().
virtual void bind(EvalContext& ctx);
```

and a stateful step works from the result: an `EWMANode`'s is `held + α·(x − sample)`, where `held` is the average
the node holds and `sample` the one it took. Seven classes join, six of them with a step:

| Node | Its step, with `held`, `sample` and `count` read at `bind()` |
|---|---|
| `EWMANode` | `held + α·(x − sample)`, and `x` for a first sample |
| `RollingStats` | `held + (x − sample)/count` |
| `RollingSumNode` | `held + (x − sample)` |
| `ZScoreNode` | `(x − μ)/σ`, with μ and σ from before the sample: `eval()`'s own expression |
| `OutlierGateNode` | `x` where the z-score of `x` passes, and the rolling mean's step where it is gated |
| `DelayNode<T>`, delay 0 | the sample itself |
| `DelayNode<T>` with a delay, `EWMATickRateNode`, a `ZScoreNode` with no spread | no step: they read no input, so they are constants at that point |

**To the bit at the sample taken, to rounding elsewhere.** At the sample the node took, `x − sample` is exactly 0 and
the step returns what the node holds. At another sample it returns what the node would hold to within the rounding of
the numbers on the way, which are the value held and the two samples, and not to within the rounding of the answer.
The prototype pins the worst case: after samples 1 and then 1e17, an EWMA of α = 0.25 holds 2.5e16, and its step at
2.0 returns 0, where a twin node fed 1 and then 2.0 holds 1.25. A sample that is not finite leaves the step with no
finite value at any sample, until the node's next update. Keeping what the update started from closes both, and is
D5's alternative: 8 bytes and one store on each of `EWMANode`, `RollingStats` and `RollingSumNode`.

**Two nodes cannot do even that.** `DeltaNode` overwrites the previous sample with the one it took, and
`RateLimiterNode` the value it last passed, so neither holds anything a step could work from. They keep their tape
partials and stay barriers to a reduction (D4). The alternative was built for `DeltaNode<double>`: one more sample
in its state, 192 to 200 bytes, one more store per update, and a step that is the node's own subtraction, so its
value to the bit at every sample.

**Results.** A new suite of 17 tests passes: each step against its node at the sample taken, with `EXPECT_EQ`, and
against a twin node at other samples, to 1e-12; an outlier gate on both sides of its threshold in one bound
reduction; 100 calls that advance nothing; the gradient against a tape at the sample taken and against the closed
form elsewhere; a reduction that still answers for the point it was bound at after its node updates again; a chain
of an EWMA, a rolling mean and ops; a Newton solve, as a node in an engine, for the sample at which a smoothed signal
meets a target; and a warm call, gradient, bind and solving cycle that allocate nothing. Under AddressSanitizer and
UBSan on the Mac the suite passes with no report.

In the reduce suite, three tests fail, each because its barrier is an `EWMANode`
(`ANodeThatCannotBeCalledIsABarrier`, `IsUnboundUntilABindSucceeds`,
`ReduceContract.ANodeGivesNoStepUnlessItSaysOtherwise`), and three more pass while no longer holding what their
comments say, for the same reason. With a node that still is a barrier in those six, the suite passes 56 of 56.

Fourteen hand-made changes to a copy of the headers, each against a control: a step that reads its node's state at
the call and not at `bind()`; an EWMA step that blends a first sample; a mean's step that divides by the window; a
gate that keeps the bound point's side of its threshold; a delayed sample that is read; a `bind()` that does not call
the hook, or calls it for a step that does not run; and seven more. Each fails a test of the new suite, and none
survives.

### flywheel-dag#13 — a tape that is kept

**Why not a kept reduction.** The comment on the issue names three things between a `GradientNode` and a
`Reduction`. There are three more, and they are differences of meaning, not gaps to close:

- A tape's adjoint of a wrt node is the derivative with respect to a change in that node's own value, and the walk
  goes on above it. A reduction's argument is a leaf: nothing above it is read. For u = exp(x) and a root u·x, a tape
  over {u, x} gives x and u + x·exp(x); a reduction over them is the function u·v, and gives x and u.
- A tape needs a node's partials, and a reduction needs a step. A node of an application that implements
  `IDifferentiable` and no step is on a tape today and would be a barrier.
- A tape through a `ConditionNode` whose condition a wrt node moves gives the derivative of the branch taken, and a
  reduction refuses the bind unless the other branch is clean.

So the prototype keeps the tape, and its meaning.

**What is kept.** Today every pass builds a tape: its vectors, a position map, the walk's stack, a set, and the vector
that each `inputs()` call returns; then a sweep builds its own arrays and, for the barrier check, two more sets. The
prototype's tape keeps all of it:

- A cache of each node it has met, filled the first time and never again: its `inputs()` (a graph is not rewired once
  built), the node as a `NodeBase` or as the mixin, and three stamps. A stamp says which record put the node on the
  tape, or which sweep marked it, so nothing is cleared between passes.
- The walk is today's: it follows only the inputs each node's partials name at this point, so a branch switch or a
  tweak changes what a record holds. A record over nodes the tape has met allocates nothing.
- `NAryOpNode::partials()` builds two vectors at each call today. `aad::Partials` lends them (`work()`), and a pass
  keeps one `Partials`. `Derivative<Op>::d()`, which an application may specialise, keeps its signature.
- `GradientNode` and `TangentNode` keep one tape and a vector, record through a private path with no clean check, as
  they do now, sweep into the vector, and publish with the slot's copying `emit()`.

```cpp
class Tape {
public:
    explicit Tape(const std::vector<NodePtr>& roots);
    /// Records again: the same roots, in their order, at the values the graph holds now.
    void record();
    std::vector<double> adjoints(const NodePtr& root, const std::vector<NodePtr>& wrt) const;
    void adjoints(const NodePtr& root, std::span<const NodePtr> wrt, std::span<double> out);
    std::vector<double> tangents(const std::vector<Seed>& seeds) const;
    void tangents(std::span<const Seed> seeds, std::span<double> out);
    std::size_t size() const noexcept;
};
```

The two const sweeps are as they are, and allocate what they return. The two new ones use the tape's own storage, so
they are not const.

**Results.** The seven AAD suites pass with no edit to any of them, and a new suite of 23 tests passes:

- A `record()` gives, bit for bit, what a new tape gives, in both sweeps, on the six graphs of
  `tests/aad_test_graphs.hpp` and the 17-node Black–Scholes call, at four points each.
- A warm `record()` with both span sweeps allocates nothing over the call price, an n-ary product, a `ConditionNode`,
  a tweaked node, an `EWMANode`, and two roots with a barrier: 50 passes each. The first record after a switch to a
  branch the tape has not met allocates, and the next does not.
- A `GradientNode` and a `TangentNode` in an engine deliver on each of 200 cycles and allocate nothing, and each value
  delivered is a new tape's.
- A record that throws leaves no recording, and both kinds of sweep then throw `std::logic_error` until one succeeds.
  That state is new: today's tape is recorded once, in its constructor.

Under AddressSanitizer and UBSan on the Mac the eight suites pass with no report. clang-tidy 23 has no finding in the
headers.

| Apple M4 Pro, the 17-node call, all five inputs; ns, best of nine loops of 200,000, three runs | `HEAD` | The prototype |
|---|---|---|
| A tape built for the pass, and one reverse sweep (`aad::adjoints()`) | 3,129 to 3,131 | 2,524 to 2,551 |
| The kept tape: `record()` and one span sweep | | 590 to 611 |
| `record()` alone | | 475 to 496 |
| One further span sweep | | 81 to 93 |
| A `GradientNode`'s recompute | 3,126 to 3,180 | 588 to 590 |
| The graph: `vol->set(v)`, then `call->eval()` | 200 to 201 | 199 to 201 |

A `GradientNode` costs three evaluations of its graph where it cost sixteen.

**Two rows.** One graph, with a sensitivity node as its one output, recomputing on every cycle:

```text
a ── EWMANode ── level ─┬────────────┐
                        ├─ blend ─┬─ area ─┬─ area − √blend = root
b ──────────────────────┴─────────┴────────┘

  blend = level·b + √level / b     a DifferentiableNode, on dual numbers
  area  = blend · level · b        a ProductNode of three
```

`gradient` is a `GradientNode` of the root over a and b, and `tangent` a `TangentNode` of the root and `blend` along
a. The pass goes through a stateful node, a functor on duals and an n-ary product, and uses nothing but `+ − × ÷` and
`sqrt`, in the values and in the partials. On Apple clang:

```text
gradient allocs/cycle=0 callbacks=200000 checksum=2422144.5540557634
tangent allocs/cycle=0 callbacks=200000 checksum=785635.84108931082
```

The first eight lines of the file do not change. A plain loop with no engine, tape or duals gives the same two
figures to 17 digits, at `-O0` and `-O2`, and `test_bench_invariants` and `test_allocation_functions` pass on the
copy as they are. GCC 13.3 and GCC 15.2, on arm64, print the same nine lines.

Sixteen hand-made changes, each against a control, over the eight suites that use a tape. Fifteen fail a test:
among them an epoch that is not bumped, `inputs()` read again at each record, an n-ary node that builds its own
vectors, a node that emits an rvalue, a barrier check that does not look above a cached node, and adjoints that are
not zeroed. One fails nothing, and it is not this change's: a `TangentNode` that pulls every root and then records
them passes the 12 tests of `HEAD`'s own suite as well. Its class comment says each root is recorded right after its
pull, and the test that held it is gone. One was written for Step 3: two roots over one always-dirty node, with a
functor under the first that counts its runs. It passes at `HEAD` and on the prototype, and fails on that change to
either, with two runs where there should be one.

### flywheel-dag#75 — several roots

`Reduction` takes a list of roots, and stays one class, as `aad::Tape` is for one root or several:

```cpp
Reduction(std::vector<NodePtr> roots, std::vector<NodePtr> args);
std::size_t roots() const noexcept;
/// Each root's value, had the arguments held x.
void operator()(std::span<const double> x, std::span<double> y);
/// The same, and the Jacobian by rows: j[i·arity() + k] = ∂root i/∂argument k.
void jacobian(std::span<const double> x, std::span<double> y, std::span<double> j);
```

One walk per root at construction shares its maps, so a node two roots reach has one entry and one step, and a call
runs it once. `jacobian()` asks each step for its partials once, keeps them, and sweeps: forward, once per argument,
when there are fewer arguments than roots, and otherwise in reverse, once per root. The one-root calls,
`double operator()(x)` and `gradient()`, are what they were on a reduction of one root, and throw `std::logic_error`
on one of several.

Which way a Jacobian was swept shows in one place: where a product of partials overflows taken one way and not the
other. x·1e200·1e200·1e-300 has the derivative 1e100 swept in reverse and inf swept forward. The prototype's test uses
that to hold the direction for each shape.

A new suite of 17 tests passes: values to the bit against the graph, per root; a shared node run once per call; the
Jacobian against a tape's adjoints, per root, and its tangents, per argument, to 1e-13, and against closed forms, in
both shapes; a root that is an argument, one no argument reaches, and one named twice; and a warm call and Jacobian
that allocate nothing. The 56 tests of the reduce suite pass with no edit.

### flywheel-dag#76 — a condition an argument moves

**The rule.** The issue lists three ways to lift the refusal, and each of the two that lift it evaluates the branch
the graph did not take or reads its stale values. There is a fourth. Where that branch is clean, something else in
the graph evaluated it at this point, so everything it read is clean too, and a reduction can read it as it reads the
branch that was taken, without evaluating anything:

> A node whose deciding input an argument moves reads every input. An input it did not name must be clean at
> `bind()`, or `bind()` throws `std::domain_error`, as now, and the message says the way out.

A call then runs the steps of both branches, and the choosing node's step selects by the trial condition, as
`ConditionNode`'s step already does. The graph evaluates both branches at its own point, which is the condition for
following, and so does the function. A barrier on the other branch is a barrier again.

`bind()` reads the point as now. Where that reading meets a deciding input that an argument moves, the point is read
again with the node marked as choosing, and again for as long as a reading marks a new one: three nested choices take
three more readings. With no such node there is the one reading there is today.

**The gradient.** A comparison on the condition's path is a `Lazy` compute node that holds a bool. It has no
partials, and nothing can pass a derivative through it. Asked, as every step is today, it would make every gradient
across a condition throw. So a derivative pass asks only the steps it passes through: the roots, and each input that
the partials of a step it passes through name. That is the tape's rule. None of the 56 tests of the reduce suite can
tell it from today's: among the engine's nodes, with no choosing node, each step that runs is named by the step
that reads it. A node of an application's own can tell: one that reads two inputs and whose partials name the first
gives a reduction a gradient today only if the step under the second has partials too. A test written for Step 5
holds the new answer.

**A node that binds must hear the branch.** A `ConditionNode` does not hear the branch it did not take. So a solver
node over a root that is one would follow a clean branch, and then never be told when that branch moves. The
prototype's `ReductionNode` refuses a point that follows an input it was not given as one of its own, and the message
names the input. Given both branches as inputs, it pulls them, so they are clean, and hears them.

A new suite of 16 tests passes: the function across the flip against the graph, to the bit, on both sides, and bound
from either side; both branches' steps counted at each call; a branch nothing evaluated, and one that is always
dirty, refused; a barrier on the other branch; choices within choices; the suite's own choosing node; the gradient
as the selected branch's on each side; and a warm call, gradient and bind that allocate nothing.

Two tests of the reduce suite were expected to change and pass as they are,
`AConditionAnArgumentMovesIsABarrier` and `ADecidingInputAnArgumentMovesIsABarrier`: in each graph the other input
was never evaluated, so the bind is still refused, and the new message keeps the words each looks for. Their names no
longer say the rule.

### flywheel-dag#74 — a node that binds

```cpp
template<typename Out, typename... Ins>
class ReductionNode : public NodeBase, public std::enable_shared_from_this<ReductionNode<Out, Ins...>> {
public:
    using Fn         = std::function<Out(Reduction&, const Ins&...)>;
    using InPlaceFn  = std::function<void(Out& out, Reduction&, const Ins&...)>;
    using InputTuple = std::tuple<std::conditional_t<true, NodePtr, Ins>...>;

    static std::shared_ptr<ReductionNode> make(std::string name, NodePtr root,
        std::vector<NodePtr> args, InputTuple ins, Fn fn, EqualityPolicyPtr eq = nullptr);
    static std::shared_ptr<ReductionNode> makeInPlace(std::string name, NodePtr root,
        std::vector<NodePtr> args, InputTuple ins, InPlaceFn fn, EqualityPolicyPtr eq = nullptr);
    // and each with std::vector<NodePtr> roots
};
```

The README's solver becomes:

```cpp
auto implied = dag::ReductionNode<double, double>::make("implied vol", price, {vol}, {quote},
    [](dag::Reduction& f, const double& target) {
        double v = f.argument(0), vega = 0.0;      // start from the vol the graph holds
        for (int i = 0; i < 20; ++i) {
            const double miss = f.gradient({v}, {&vega, 1}) - target;
            if (std::abs(miss) < 1e-12) break;
            v -= miss / vega;
        }
        return v;
    });
```

It is `Eager`, fixed in the class. Its inputs are its roots and then `ins`. At each evaluation it pulls the roots
and the other inputs, binds without the clean check, and calls the functor. A throw from the bind or the functor
leaves it dirty and its value as it was. `Reduction::argument(i)` is the value argument i itself holds at the bound
point, which `bind()` already reads to check its type.

A new suite of 19 tests passes: the solver in an engine, solving again when the spot or the quote moves and not in a
quiet cycle; a point that moves while the root stands still; a root that an always-dirty node reaches by two paths,
which the public `bind()` refuses; two roots over an always-dirty node; a functor that throws; a Gauss–Newton fit
over three roots through `makeInPlace`, 200 cycles with no allocation; a solver across a `ConditionNode` whose
branches it is given, and the refusal when it is not; and `argument()` with its three throws.

Sixteen hand-made changes across the three, each against a control over the 108 tests of the four reduce suites: a
forward sweep, and a reverse one, for every shape; partials gathered once per sweep; the stale-branch check dropped;
the point read at most twice; partials asked of every step; a node that binds with the clean check, is built `Lazy`,
does not pull its roots, or follows a branch it was not given. Each fails a test, and none survives.

### What the changes cost the calls that exist

**A reduction of one root.** The three reduction changes were first written the obvious way: one path for every
shape, a mark for each step a derivative passes through, the hook called for every step. A probe of the calls v1.3.1
has, on the Black–Scholes call (17 nodes), built behind a pad at 8 alignments and run in shuffled order, each figure
the best of five loops of 100,000:

| ns, the mean over the 8 alignments | `HEAD` | Written the obvious way | With the changes below |
|---|---|---|---|
| `f(v)`: the call over vol, 8 steps | 72.5 | 72.9 | 71.5 |
| `gradient(v)` over vol | 245.8 | 261.8, +6.5% | 254.7, +3.6% |
| `f(x)`: the call over all five inputs, 12 steps | 107.3 | 108.6 | 106.7 |
| `gradient(x)` over all five | 327.0 | 355.7, +8.8% | 334.2, +2.2% |
| `bind()` over vol | 103.7 | 124.9, +20.5% | 104.3, +0.6% |
| `bind()` over all five | 124.5 | 152.7, +22.7% | 129.2, +3.8% |
| `bind()` and six gradients over vol | 1,654.8 | 1,756.3, +6.1% | 1,679.3, +1.5% |

The checksum of every value and gradient the probe takes is the same in all three: a one-root reduction's results
are `HEAD`'s to the bit. So the obvious way costs `gradient()` 7 to 9% and `bind()` over 20%, at every alignment.
What took most of it out is what Steps 4 to 7 build:

- **The marks are stamps, not bytes.** The mark for "a derivative passes through this step" was a
  `std::vector<std::uint8_t>`, written once per partial. A store through a byte type may alias anything, so the
  compiler reloaded every pointer the loop held after each one. A 32-bit stamp of the pass does not, and needs no
  clearing.
- **A gradient asks every step, in one pass, for as long as every step answers**, which is today's loop. A step with
  no partials, or a bound point with a choosing node, sends it out of line to the marked sweep, which says whether
  the derivative passes through that step at all. The answer is the same either way: a step the derivative does not
  pass through has an adjoint of 0, and passes nothing on.
- **`bind()` reads the point once, as today, and again only where that reading meets a deciding input that an
  argument moves.** The readings that follow a choice are out of line.
- **The hook is asked for once.** When a reduction is built, each step says whether it reads its node at `bind()`,
  and `bind()` calls only those.
- **The first root is held as it is today**, as a node and a position of its own, and the others beside it: a
  one-root call takes no loop over a list of one. The call's body is in line in `gradient()`, as it is today.

What is left is small and is not all found. A call costs what it cost. `gradient()` reads 2 to 4% more, 7 to 9 ns,
at every alignment. `bind()` reads the same over one argument (slower at 5 of the 8) and 4% more over five, about
1 ns an argument, of which a quarter is the store that keeps each argument's own value for `argument()`. The
benchmark's `solve` row, which binds and takes six gradients in each cycle on a reduction of two steps, reads less
than at `HEAD`: 2.65% less with each row run once, at every one of the 16 alignments.

Steps 4 to 7 hold both instruments to these figures, and what is left at Step 7 is found or put to you before the
release (D15).

**The cycle.** The benchmark at `HEAD` and with the merged prototype's headers, each built from one benchmark source
at the 16 alignments, with a control that is a second build of `HEAD`'s headers:

| Change against `HEAD`, over the 16 alignments | `chain` | `idle-queues` | `ingest` | `solve` | `ops-compute` | `ops-moving` | `ops-held` |
|---|---|---|---|---|---|---|---|
| **Each row once**, 15 runs. The control | +0.02% | 0.00% | 0.00% | +0.11% | +0.15% | +0.04% | −0.17% |
| The prototype | +0.13% | +0.01% | −0.23% | −2.65% | −0.55% | +0.01% | +0.29% |
| Slower at | 11 of 16, as the control | 6 of 16 | 2 of 16 | 0 of 16 | 2 of 16 | 8 of 16 | 10 of 16 |
| **In rotation**, 12 runs. The prototype | +1.01% | −1.66% | +0.65% | −2.02% | +0.45% | +3.42% | +8.48% |
| Slower at | 14 of 16 | 4 of 16 | 11 of 16 | 0 of 16 | 13 of 16 | 16 of 16 | 16 of 16 |
| In rotation with `solve` left out | −4.82% | −0.05% | −0.15% | | +0.09% | −0.66% | +1.94% |
| Slower at | 0 of 16 | 6 of 16 | 3 of 16 | | 9 of 16 | 4 of 16 | 16 of 16 |

Run as the benchmark runs them, no row is slower. The rotation was timed on an earlier build of the second
prototype, which differs from the final one only in code that no row but `solve` runs. In rotation `ops-held` reads
8% more at every alignment, and `ops-moving` 3%, and neither is the engine's code: of the 895 functions that the two
binaries both hold, 820 are the same, and every `eval()` the three `ops` rows run is among them, with
`Engine::cycle()`, `NodeBase::propagate()`, the policies' `equal()`, the sources' `flush()` and the slots'
`emitImpl()`. Of the 75 that differ, 64 differ in one immediate (a vtable's offset), and the other eleven are the two
`eval()` with their flag, their two constructors, the two `NAryOpNode::partials()`, and five of `Reduction`'s. With
`solve` left out of the rotation, `ops-moving` reads the same, `ops-held` 2% more at 16 of 16 and `chain` 5% less at
0 of 16, with the same instructions again. So what moves a row in rotation is where everything else lands and what
the rows before it leave on the heap, which is the benchmark plan's finding: a sweep averages only over where the code
lands in a cache line.

### The whole change, once

The four prototypes merged into one copy of the checkout, with the changes above:

- **Apple clang 21.0.0.** No warning in any of the 41 suites or the benchmark. Of the 751 tests of the 31 suites,
  747 pass, and the four that do not are the ones named above, each of which expects an `EWMANode` to be a barrier.
  The nine new suites pass, 109 tests, and so does the reduce suite with a `RollingMinMaxNode` in place of the
  `EWMANode` in six tests, 56 of 56.
- **GCC 13.3.0** (`ubuntu-dev`) and **GCC 15.2.0** (a throwaway `ubuntu:26.04`), both arm64: the same suites, with
  the same results. Each first refused something that Apple clang accepts, and every one was in the second
  prototype's own code: GCC 13.3, `noinline` on a function declared `inline` and `always_inline` on a template that is
  not (`-Werror=attributes`); GCC 15.2, a mark written into a byte vector right after its `assign()`
  (`-Wstringop-overflow`), and two braced lists in the arms of a `?:` in the new rows (`-Wdangling-pointer`), which it
  also draws against `HEAD`'s headers. The cold paths are now written in the class, as `NodeBase::stayDirty()` is,
  the marks are 32-bit words, and the rows build their node in an `if`. Neither compiler then prints a warning, and
  the only text in a log is GCC's ABI note on aarch64.
- **AddressSanitizer and UBSan**, on the Mac: the ten new suites and the eight whose headers change most, with no
  report.
- **`bench_hot_path --invariants`** prints the same nine rows from all three compilers, and the first seven are the
  committed file's.
- **clang-tidy 23.1.4**, with the repo's checks and `--warnings-as-errors=*`: no finding in 16 units, which between
  them include every header that changes. A canary unit with one use after a move draws its finding.
- **Sizes.** `aad::Tape` 112 to 416 bytes, `GradientNode` 192 to 632, `TangentNode` 200 to 640, `Reduction` 344 to
  680. Every node class a cycle runs is the size it was.
- **50 hand-made changes** to copies of the headers, each beside a control: 46 to three of the prototypes, and four
  to the merged one, at its two paths through `bind()` and `gradient()`. Each fails a test of the new suites, one of
  them only since the test written for it: the change to `TangentNode`, which `HEAD`'s own suite does not catch
  either.

## Decisions to confirm

Each has a default, which is what the steps below do. D2, D4, D5 and D9 are the ones the issues leave to you, and
D15 is a cost I would rather you saw now than at the release.

- **D1. One plan, and how its commits are scoped.** A commit that serves one issue cites that issue. The plan's own
  commits, and the release, cite flywheel-dag#11.
- **D2. flywheel-dag#11's reading is "at the current state": a node's last update, applied to another sample.** It
  is the only reading a `Reduction` can use, it gives a tape a derivative that is exact and local, and it costs no
  node a byte. It answers "had the latest sample been higher, how much would this move?". It does not answer for an
  input that also fed every earlier sample: that part of the derivative went into the state at earlier updates, and
  is not in it. The header says so. *Alternative:* "through the state, over time" as well, which is a feature of its
  own: each stateful node would carry a tangent per direction in its state, updated on every cycle, with the
  directions fixed when the graph is built. D13 opens an issue for it and builds nothing.
- **D3. A node gives a tape its partials through a virtual of `NodeBase`, `partials()`, and not through the
  `IDifferentiable` mixin.** No node grows, and the cycle's machine code is the same but for two stores. The mixin
  stays, for a node that does not derive `NodeBase` and for the engine's classes that carry it: taking it off them
  would be a break, since code that finds differentiable nodes by `dynamic_cast` would stop seeing ops (D13 opens an
  issue). *Alternative:* the mixin on the nine classes: 8 bytes each, on six of `chain`'s eleven nodes, where three
  cost the row 0.37% in the reduce plan.
- **D4. `DeltaNode` and `RateLimiterNode` get a partial and no step.** On a tape they have a derivative. A reduction
  through one is refused, with a message that says why. *Alternative:* each keeps what its last update started from,
  8 bytes and one store per update, and gives a step that is its value to the bit. Built for `DeltaNode<double>`
  (192 to 200 bytes), it moved `chain`, which holds one, by −0.05% with each row run once, slower at 7 of the 16
  alignments, and by −0.37% in rotation, slower at 4: nothing the sweep can see. No row holds a `RateLimiterNode`.
  Every design that gives the two a step needs what their last update started from, so each grows the node, which
  is the bar this plan holds the cycle to: so the choice is yours.
- **D5. A stateful step works from the node's result: to the bit at the sample the node took, to rounding at the
  scale of that sample elsewhere, and not finite after a sample that is not.** For samples of one scale that is the
  last place or two. *Alternative:* `EWMANode`, `RollingStats` and `RollingSumNode` keep what their last update
  started from: 8 bytes and one store each, and a step that is the update itself, to the bit at every sample,
  which no longer pulls the node's input at `bind()`. `chain` holds all three, and read 0.11% more with each row
  run once (0.3 ns in 265), slower at 12 of the 16 alignments, and 0.26% more in rotation, slower at 9. The
  control, a second build of the same headers, read −0.16% and −0.05%. So it is small, and it is probably there.
  Each design that is the update itself needs what the update started from, and so grows the node: D4's reason for
  putting it to you holds here too.
- **D6. flywheel-dag#13 is a tape that is kept, and `GradientNode` and `TangentNode` keep one.** `aad::adjoints()`
  and `aad::tangents()`, which build a tape for one pass, stay. *Alternative:* build the two nodes on a kept
  reduction, which changes what three kinds of graph give (above) and which nodes can join.
- **D7. Two benchmark rows, `gradient` and `tangent`,** on one graph, so that each sweep is priced and held apart.
  `benchmarks/expected_invariants.txt` gains two lines. *Alternative:* one row, a `GradientNode` alone.
- **D8. Several roots are the same class, `Reduction`, and the Jacobian goes by rows into the caller's span.** A
  forward sweep is part of it, taken when there are fewer arguments than roots. The one-root calls throw
  `std::logic_error` on a reduction of several roots. A braced list of one root, `reduce({r}, {x})`, does not
  compile, because it could be either constructor: write `reduce(r, {x})`. *Alternative:* a second class for
  several roots, which gives the shape a type and the API a second name for one thing.
- **D9. A condition an argument moves is followed where the other branch is clean, and refused where it is not.** A
  reduction still evaluates nothing. Whether a hand-bound reduction binds then depends on the point: `x > 0 ? x : −x`
  binds where the graph took `−x`, since the other branch is the argument, and is refused where it took `x` until
  something evaluates `−x`. A `ReductionNode` that is given both branches as inputs always binds. *Alternative A:*
  leave it refused, as you answered for the reduce plan, and close the issue with the way round that exists today: a
  selection written as an `aad::DifferentiableNode` over both branches, which evaluates both and which a reduction
  follows. *Alternative B:* evaluate the other branch at `bind()`, which the issue lists and which breaks the rule
  that a reduction evaluates nothing.
- **D10. A derivative pass asks only the steps it passes through**, where today every step that runs is asked. With
  no choosing node the two are the same for every node of the engine. With one, a step with no partials on a branch
  stops the gradient only at the calls that select the branch, so a solver can meet the refusal part of the way
  through. The header says so. *Alternative:* ask every step but those on a condition's path, so that a reduction
  either has a gradient at every point or at none: simpler to rely on, and it refuses a function whose opaque step is
  on a branch the solver never selects.
- **D11. `ReductionNode<Out, Ins...>`.** Its functor is `Out(Reduction&, const Ins&...)`, with the other inputs
  typed as a `ComputeNode`'s are, and `makeInPlace()` takes `void(Out&, Reduction&, const Ins&...)` for an `Out` that
  owns heap. What it publishes when a solver does not converge is the functor's: it returns what it wants published,
  an `Out` of `std::optional<double>` if it wants to say so, or throws, which leaves the node dirty. The arguments'
  own values are offered through `Reduction::argument(i)`, which throws for an argument the root does not depend on
  at the bound point, since `bind()` does not read it. Keeping them costs `bind()` one store an argument. An input
  the functor does not use, such as a branch it must hear, still takes a parameter. *Alternative:* a second list of
  inputs that are pulled and heard and not handed over.
- **D12. The release is v1.4.0, in Step 9.** New public names make it a minor: `NodeBase::partials()`,
  `IReduceStep::bind()`, `Tape::record()` and the two span sweeps, `aad::Partials::work()`, the several-root calls of
  `Reduction`, `Reduction::argument()`, `ReductionNode`, `ZScoreNode::score()`. Three things that threw now answer: a
  derivative through one of nine stateful nodes, a reduction through six, and a bind at a condition an argument
  moves where the other branch is clean. A program that relied on the throw sees a value. *Alternative:* leave the
  release until you ask.
- **D13. Two issues to open at approval**, each with the figures above, `enhancement` and `aad`, and no milestone
  (opened: [flywheel-dag#81](https://github.com/tomlin256/flywheel-dag/issues/81) and
  [flywheel-dag#82](https://github.com/tomlin256/flywheel-dag/issues/82)):
  neither is a step of this plan, and the other open milestone is about the engine's threads and lifecycle.
  - derivatives through the state, over time (D2);
  - the engine's own nodes giving their partials through `NodeBase::partials()` and dropping the `IDifferentiable`
    base, 8 bytes a node, in a major release (D3).

  The window of 0 is opened already, as a bug found outside the plan: flywheel-dag#80, `bug` and `nodes`, with no
  milestone, since the open one is about the engine's threads and lifecycle. None is opened for the test `HEAD`
  lacks on `TangentNode`: Step 3 writes it, since Step 3 rewrites the code it holds.
- **D14. Pushes, comments and closing.** Each step is one commit on `main`, pushed once its checkpoint is green and
  not held for CI. Approving the plan approves those pushes, a comment on each of the five issues that names this
  file, the issues of D13, the closing comment on each of the five and its close by hand, the milestone's close, and
  the tag and release page of Step 9.
- **D15. What the one-root calls may cost.** The bar in the steps is that `bind()`, a call and `gradient()` cost a
  one-root reduction what they cost at `HEAD`, on the `solve` row at 16 alignments and on the probe at 8. The second
  prototype meets it on the row and on a call, and misses it on the probe by 7 to 9 ns a gradient (2 to 4%) and
  1 ns an argument a bind (4% over five arguments), a quarter of the latter being what `argument()` needs. Step 7
  either finds the rest or brings you the figures before the release. *Alternative A:* accept what the second
  prototype reads, as the price of one class for every shape. *Alternative B:* several roots as a class of their own
  (D8's alternative), which leaves `Reduction`'s calls as they are by construction, and still wants the hook and the
  choice in `bind()`.

## Steps

Every commit subject is scoped to its issue in the repo's `type(#N): …` form, and the subjects below omit the scope.
No commit carries a closing keyword: each issue is closed by hand in Step 9, after its comment. Every step is test
first: the tests are written and seen red (for a new name, a build that fails on it), then the code.

**A checkpoint** is a configure with CI's flags and the lint gate (`-Werror=author -Werror=deprecated
-DFLYWHEEL_DAG_CLANG_TIDY=clang-tidy`), a full build with no warning and `ctest --preset relwithdebinfo-macos-arm64`
passing in full on the Mac; the same in `ubuntu-dev` (`relwithdebinfo-linux-arm64`, GCC 13.3); the new code and the
suites that use it built and run under GCC 15.2 in a throwaway container; `bench_hot_path --invariants` equal to the
committed file on all three; then the leak audit and a push. CI is read in passing and not waited for.

**The cycle's cost** is held three ways at each step that changes a header a cycle runs: the size of every node
class against the table of `ff19003`; the benchmark's machine code, function by function, against the build before
the step; and, where a function on a cycle's path changed, the benchmark's rows at the 16 alignments, each row once
and in rotation, against a control that is a second build of the same headers. **A reduction's cost** is held by a
probe of the calls a one-root reduction had at v1.3.1 (`bind()`, a call, `gradient()`, on the Black–Scholes call),
built behind a pad at 8 alignments and run in shuffled order, and by the `solve` row in the sweep. The bar is D15's:
what those calls cost at `HEAD`.

**Hand-made changes** are made to a scratch copy, never the tree, each after an assertion that its old text occurs
exactly once, each test run alone under a watchdog, beside an unchanged control. A change that is not caught as
stated is read before anything is edited.

The ctest count goes from 91 to 98, and the `clang_tidy_` tests from 45 to 52.

### Step 1 — The plan

Commit: `docs: add plan for closing out the Differentiable subgraphs milestone`. On approval it is pushed, a comment
on each of the five issues names this file, and the two issues of D13 are opened. **Tests: none.** **Done when:**
the plan is committed, you have approved it and answered D2, D4, D5 and D9, it is pushed, each issue names it, and I
have told you the numbers, labels and milestones of the issues opened.

### Step 2 — flywheel-dag#11: a tape through the stateful nodes

**Tests new:** `tests/test_aad_timeseries.cpp` (suite `AadTimeseries`), and one test in `tests/test_aad_reverse.cpp`.

| Test | Asserts |
|---|---|
| `EwmaIsAlphaOnceItHoldsAValue` | 1 for the first sample and α after it, a sample equal to the average held included: against the closed form to 1e-13, and against two twin nodes that differ in the last sample alone |
| `RollingMeanIsOneOverItsCount`, `RollingSumIsOne` | While the window fills, when it is full, and on the update that re-sums it |
| `DeltaIsOneOnceItHasAPreviousSample`, `ARateLimiterPassesOneOrHolds` | No input named for a first sample, or for one held back |
| `ADelayOfATickOrMoreDoesNotDependOnTheLatestSample`, `ATickRateDoesNotDependOnItsSample` | No input named; 1 for a delay of 0 |
| `ZScoreIsOneOverTheSpreadBeforeTheSample`, `AnOutlierGatePassesOneOrTheMeansShare` | 1/σ, and none while there is no spread; 1 for a sample that passes and 1/count for one that is gated |
| `ANodeThatHoldsNoDoubleGivesNone` | The five classes that hold no double and the `int` instantiations: `partials()` is false |
| `ATapeSweepsThroughStatefulNodes` | k · 1/count · α through an EWMA, a rolling mean and a product, in both sweeps |
| `ANodeThatNamesNoInputHidesABarrierAboveIt` | An opaque node above a delay of 1 is not a barrier, and above a delay of 0 it is |
| `APassThroughAStatefulNodeDoesNotAdvanceIt` | Three passes per update, over four updates: a control twin agrees |
| `AGradientNodeDeliversThroughAStatefulNode` | In an engine: 2·ewma·α on each of three cycles |
| `AadReverse.ANodeThatIsNotANodeBaseIsAskedThroughTheMixin` | A node that derives `INode` and `IDifferentiable` alone is on the tape with its partial |

The sizes are held where the flags are declared: a `static_assert` that each of the three state structs is two
doubles.

**Tests modified:** `AadReverse.APassDoesNotAdvanceAStatefulNode`, whose last line expects a throw for a wrt node
above an `EWMANode`, expects the derivative, 3·α.

**The change:** `dag.hpp` and `dag.inl`: `NodeBase::partials()`, and what the comments on `NodeBase` and
`IDifferentiable` say of the two ways to join. `dag_aad.inl`: the tape asks through the one or the other.
`dag_timeseries.hpp` and `.inl`: the three flags, the nine `partials()`, and a block at the top of the header that
states the reading of D2. `dag_aad.hpp`, the README's Derivatives section and `docs/engine_rules.md`: what they say
is a barrier.

| Change made by hand, then reverted | Must fail |
|---|---|
| `EWMANode::partials()` gives α for a first sample | `EwmaIsAlphaOnceItHoldsAValue` |
| `RollingStats::partials()` divides by the window | `RollingMeanIsOneOverItsCount` |
| `ZScoreNode::partials()` reads the spread with the sample in it | `ZScoreIsOneOverTheSpreadBeforeTheSample` |
| `OutlierGateNode::partials()` gives 1 for a gated sample | `AnOutlierGatePassesOneOrTheMeansShare` |
| A delay of a tick names its input | `ADelayOfATickOrMoreDoesNotDependOnTheLatestSample`, `ANodeThatNamesNoInputHidesABarrierAboveIt` |
| A flag is not written by its update | The test of its node |
| The tape asks only the mixin | Every test of `AadTimeseries` but `ANodeThatHoldsNoDoubleGivesNone` |
| The tape asks only `NodeBase` | `ANodeThatIsNotANodeBaseIsAskedThroughTheMixin` |

**Done when:** red then green; the checkpoint at 92 tests; each change as stated; the size table and the function
table of "Measured" taken again on the committed files; the sweep of `chain` and `ops-moving` against the control,
both ways, in the Outcome; pushed. Commit: `feat: a tape differentiates through nine dag::ts nodes, at the current
state`.

### Step 3 — flywheel-dag#13: a tape that is kept

**Tests new:** `tests/test_aad_tape.cpp` (suite `AadTape`, which links the allocation counter), and tests in the two
sensitivity-node suites, which join the counting list.

| Test | Asserts |
|---|---|
| `ARecordGivesWhatANewTapeGives` | Six graphs and the call price, four points each, both sweeps, bit for bit |
| `SeveralRootsAreRecordedAgainInTheirOrder` | Two roots, and the forward sweep's order |
| `ABranchSwitchIsFollowedByTheNextRecord`, `ATweakIsFollowedByTheNextRecord`, `AStatefulNodeIsRecordedAtItsCurrentState` | What a record holds is this point's |
| `ANodesInputsAreReadOnce` | A node of the suite's that counts its `inputs()` calls: one, over ten records |
| `ARecordThatThrowsLeavesNoRecording`, `ARecordThatThrowsPartWayIsForgotten`, `EveryRootMustBeCleanForARecord` | Sweeps throw `std::logic_error` until a record succeeds, and the next record is whole |
| `ASpanSweepWantsOneSlotPerResult`, `ASpanSweepRefusesWhatAVectorSweepRefuses`, `ABarrierCheckWalksEveryNodeAboveABarrier` | The span sweeps' refusals are the vector sweeps', with their messages |
| `AWarmRecordAndItsSweepsAllocateNothing` | Six graphs, 50 passes each: 0 |
| `TheFirstRecordThatMeetsNewNodesAllocatesAndTheNextDoesNot`, `AWrtNodeOffTheTapeIsMetOnce` | What "warm" means |
| `AnNAryNodesPartialsUseThePassesStorage` | A product of eight, asked 100 times through one `Partials`: 0 |
| `AadGradientNode.AllocatesNothingOnceWarm`, `AadTangentNode.AllocatesNothingOnceWarm` | 200 cycles in an engine, a delivery on each, each value a new tape's: 0 |
| `AadGradientNode.AThrowLeavesItsValueAsItWas` | And the node dirty, and the next evaluation whole |
| `AadTangentNode.RecordsEachRootRightAfterItsPull` | Two roots over one always-dirty node, a functor that counts its runs under the first: one run per evaluation |

**Tests modified:** none. The seven AAD suites pass as they are.

**The change:** `dag_aad.hpp` and `.inl`: the tape as under "Measured", and the two nodes. `dag.hpp`:
`aad::Partials::work()`. `dag_ops.inl`: `NAryOpNode::partials()`. `benchmarks/`: the two rows, their two lines, and
the count of workloads in the file's comment. The README: what a `GradientNode` costs, and the benchmark's table.

| Change made by hand, then reverted | Must fail |
|---|---|
| The record's epoch is not bumped | Every test that records twice |
| `inputs()` is read at each record | `ANodesInputsAreReadOnce`, and the allocation tests |
| A node's place on the tape ignores the epoch | `ABranchSwitchIsFollowedByTheNextRecord` |
| `NAryOpNode::partials()` builds its vectors | `AnNAryNodesPartialsUseThePassesStorage` |
| A node emits an rvalue | Its `AllocatesNothingOnceWarm` |
| The kept barrier check does not look above a node it has met | `ABarrierCheckWalksEveryNodeAboveABarrier` |
| The adjoints, or the tangents, are not zeroed | `ARecordGivesWhatANewTapeGives` |
| `record()` takes a dirty root | `EveryRootMustBeCleanForARecord` |
| A sweep takes a record that did not finish | `ARecordThatThrowsLeavesNoRecording` |
| `TangentNode` pulls every root, then records them (probed: nothing at `HEAD` fails) | `RecordsEachRootRightAfterItsPull` |

**Done when:** red then green; the checkpoint at 93 tests; each change as stated; `--invariants` is the new file's
nine lines on the Mac, in `ubuntu-dev` and under GCC 15.2, and the plain loop gives the two new figures; the timing
table of "Measured" taken again; the sweep of the nine rows, with the seven older ones against the build before the
step; pushed. CI's Ubuntu leg is the first x86-64 reading of the two checksums, and a difference there is fixed
forward. Commit: `feat: a tape that is kept, and two rows for the passes that keep one`.

### Step 4 — flywheel-dag#75: several roots

**Tests new:** `tests/reduce_test_nodes.hpp`, which takes the reduce suite's three nodes and its call price as they
are, and `tests/test_dag_reduce_roots.cpp` (suite `ReductionRoots`).

| Test | Asserts |
|---|---|
| `MatchesTheGraphAtEveryPoint` | Three roots over shared nodes: each value to the bit, at the point held and at a moved one |
| `ASharedNodeRunsOncePerCall` | A counted node under two roots: one run per call |
| `BindReadsThePointEveryRootHolds`, `RejectsADirtyRootOrOneThatHoldsNoDouble`, `RejectsBadRoots` | Every root clean and a double; a null root and no roots refused |
| `ARootCanBeAnArgumentOrAConstant`, `ARootNamedTwiceIsTwoValues`, `OneRootIsARowOfOne` | The edges of the shape |
| `TheOneRootCallsRefuseSeveralRoots`, `RejectsRoomOfTheWrongSize` | `std::logic_error`, and `std::invalid_argument` |
| `TheJacobianMatchesATape`, `TheJacobianMatchesAnalyticDerivatives` | Adjoints per root and tangents per argument, to 1e-13, in both shapes |
| `TheJacobianSweepsTheShorterWay` | The overflow that tells the directions apart: reverse for 1 by 2 and for a tie, forward for 2 by 1 |
| `TheJacobianAsksEachStepOnce` | A step that counts its `partials()` calls: one per Jacobian of three roots |
| `AZeroPartialOrTangentPropagatesNothing`, `AnOpaqueStepStopsTheJacobianOfTheRootsItIsUnder` | The tape's rules, forward as in reverse |
| `AWarmCallAndJacobianAllocateNothing` | 100 of each: 0 |

**Tests modified:** `tests/test_dag_reduce.cpp` includes the new header in place of its own copy of the four. Its 56
tests pass with no other edit.

| Change made by hand, then reverted | Must fail |
|---|---|
| A forward sweep for every shape, and a reverse one for every shape | `TheJacobianSweepsTheShorterWay` |
| Partials gathered once per sweep | `TheJacobianAsksEachStepOnce` |
| The forward sweep drops the zero rule | `AZeroPartialOrTangentPropagatesNothing` |
| Every root's value is the first root's | Nine tests of the suite |
| Every reduction is taken to have one root (probed) | Thirteen tests of the suite |
| A root no argument moves is not held | `ARootCanBeAnArgumentOrAConstant` |

**Done when:** red then green; the checkpoint at 94 tests; each change as stated; the probe and the `solve` row
against the build before the step; pushed. Commit: `feat: a reduction of several roots, and its Jacobian`.

### Step 5 — flywheel-dag#76: a condition an argument moves

**Tests new:** `tests/test_dag_reduce_choice.cpp` (suite `ReductionChoice`).

| Test | Asserts |
|---|---|
| `FollowsAConditionTheArgumentMoves`, `IsTheSameFunctionFromEitherSide` | Across the flip, to the bit against the graph; bound from each side |
| `BothBranchesRunAtEachCall`, `ANaNOnTheBranchNotSelectedIsNotTheValue` | Each branch's counted node runs once per call, and only the selected one is the value |
| `ABranchNothingEvaluatedIsRefused`, `AnAlwaysDirtyBranchIsRefused` | `std::domain_error`, naming the root, the argument, the choosing node and the stale input; bound once it is evaluated |
| `ABarrierOnTheOtherBranchIsOne`, `TheOtherBranchCanBeTheArgument` | What following a branch brings with it |
| `FollowsChoicesWithinChoices`, `TwoChoicesOnOnePathAreFollowed`, `ANodeOfYourOwnThatDecidesIsFollowed` | Three nested choices; two in a row; the suite's `Chooser` |
| `TheGradientIsTheSelectedBranchs`, `AnOpaqueBranchStopsTheGradientOnlyWhereItIsSelected`, `TheJacobianFollowsTheChoice` | D10, with a bool comparison on the condition's path |
| `AStepTheDerivativeDoesNotPassThroughIsNotAsked` | D10 with no choosing node: a node of the suite's that reads two inputs and names one, over an opaque step. The gradient is 1, twice, and a root that is the opaque step itself still has none |
| `AWarmCallGradientAndBindAllocateNothing` | With a choosing node: 0 |

**Tests modified:** `Reduction.AConditionAnArgumentMovesIsABarrier` and
`Reduction.ADecidingInputAnArgumentMovesIsABarrier` are renamed for the rule (`…IsRefusedWhileItsOtherBranchIsStale`),
and each gains its second half: with the other input evaluated, the bind succeeds.

| Change made by hand, then reverted | Must fail |
|---|---|
| The stale-branch check is dropped | `ABranchNothingEvaluatedIsRefused`, `AnAlwaysDirtyBranchIsRefused`, and the two renamed tests |
| The point is read at most twice | `FollowsChoicesWithinChoices` |
| Partials are asked of every step | `TheGradientIsTheSelectedBranchs`, `AnOpaqueBranchStopsTheGradientOnlyWhereItIsSelected` |
| The one-pass gradient throws where a step has no partials, and does not take the marked sweep (probed) | `AStepTheDerivativeDoesNotPassThroughIsNotAsked` |
| The first reading of the point does not stop at a deciding input that an argument moves (probed) | Nine tests of the suite and of Step 7's |
| A choosing node reads only what it named | `FollowsAConditionTheArgumentMoves` |

**Done when:** red then green; the checkpoint at 95 tests; each change as stated; the probe and the `solve` row
against the build before Step 4; pushed. Commit: `feat: a reduction follows a condition an argument moves, where the
other branch is clean`.

### Step 6 — flywheel-dag#11: a reduction through the stateful nodes

**Tests new:** `tests/test_dag_reduce_timeseries.cpp` (suite `ReductionTimeseries`).

| Test | Asserts |
|---|---|
| `AnEwmaAppliesItsLastUpdateToAnotherSample`, and the same for a rolling mean, a rolling sum, a z-score and an outlier gate | At the sample the node took, the node's value with `EXPECT_EQ`; at others a twin's, to 1e-12; the gate on both sides of its threshold in one bound reduction |
| `ADelayOfNoTicksPassesItsSampleThrough` | For a double and for an int |
| `AStepWorksFromTheValueItsNodeHolds` | The case of "Measured": 0 where a twin holds 1.25, and NaN after a sample that is not finite |
| `AStepAppliesNoEqualityPolicy` | Under a tolerance policy the step is the update, not the value published |
| `AStepAnswersForThePointItWasBoundAt`, `AStepTheBoundPointDoesNotRunIsNotBound`, `WhatANodeReadsIsAskedAtEachBind` | After the node updates again: the old point until `bind()`, the new one after |
| `ANodeThatDoesNotReadItsSampleIsAConstant` | A delay, a tick rate and a z-score with no spread: no step, and a barrier above is not one |
| `ANodeThatGivesNoStepIsABarrier` | `DeltaNode`, `RateLimiterNode`, and a `RollingMinMaxNode` under a compute node: the message |
| `AChainOfStatefulNodesAndOps`, `ASolverFindsTheSampleAtWhichASmoothedSignalMeetsATarget` | The gradient against a tape; the solution, fed to a twin, gives the target |
| `AWarmCallGradientAndBindAllocateNothing`, `ASolvingCycleThroughAStatefulNodeAllocatesNothing` | 0 |

**Tests modified:** six tests of `tests/test_dag_reduce.cpp` use an `EWMANode` as a node that cannot be called. Three
fail (`ANodeThatCannotBeCalledIsABarrier`, `IsUnboundUntilABindSucceeds`,
`ReduceContract.ANodeGivesNoStepUnlessItSaysOtherwise`) and three pass without holding what they say
(`ATweakedNodeIsAConstant`, `FollowsOnlyTheInputsANodeReads`, `ANodeThatReadsNothingIsAConstant`). Each takes a
`RollingMinMaxNode` under a compute node, which is a barrier under either answer to D4.

**The change:** `dag.hpp`: `IReduceStep::bind()`. `dag_reduce.inl`: `bind()` calls it, once the point is read, for
the steps that asked for it when the reduction was built, so that a reduction with no stateful step pays nothing;
the barrier's message. `dag_timeseries.hpp` and `.inl`: the six steps, three `reduceReads()`, and
`ZScoreNode::score()`, which `eval()` and two steps share. The header comments, the README's table of what is
called, and `docs/engine_rules.md`.

| Change made by hand, then reverted | Must fail |
|---|---|
| A step reads its node's state at the call | `AStepAnswersForThePointItWasBoundAt` |
| The EWMA's step blends a first sample | `AnEwmaAppliesItsLastUpdateToAnotherSample` |
| The mean's step divides by the window | `ARollingMeanAppliesItsLastUpdateToAnotherSample` |
| The gate's step keeps the bound point's side of the threshold | `AnOutlierGateAppliesItsLastUpdateToAnotherSample` |
| A delayed sample is read; a tick rate reads its sample; a flat z-score reads its sample | `ANodeThatDoesNotReadItsSampleIsAConstant` |
| `bind()` does not call the hook; it calls it for a step that does not run; no step is asked whether it reads its node (probed) | Eleven tests of the suite; `AStepTheBoundPointDoesNotRunIsNotBound`; eleven tests of the suite |
| The z-score's step reads the stats' current mean | `AZScoreAppliesItsLastUpdateToAnotherSample` |

**Done when:** red then green; the checkpoint at 96 tests; each change as stated; the size table; `ZScoreNode::eval()`
and `OutlierGateNode::eval()` compared in the benchmark's machine code, and `chain` swept if either differs; the probe
against the build before Step 4; pushed. Commit: `feat: a reduction through six dag::ts nodes`.

### Step 7 — flywheel-dag#74: `ReductionNode`

**Tests new:** `tests/test_reduction_node.cpp` (suites `ReductionNode` and `ReductionArgument`).

| Test | Asserts |
|---|---|
| `ASolverInAnEngine` | The implied vol of the snippet above: delivered; solved again when the spot moves and when the quote moves; not in a cycle where nothing moves |
| `SeesAPointThatMovesWhileTheRootStandsStill` | a·x + b with (a, b) moved so that the root keeps its value: the new zero |
| `RegisteredBeforeItsRoot`, `AnEqualityPolicyGatesDelivery`, `ItsInputsAreItsRootsThenItsOtherInputs` | As an engine output |
| `AnInputThatOwnsHeapIsHandedByReference`, `AForcedEvaluationReachesItsInputs` | The two paths of `ComputeNode`; `forceRecompute` |
| `StaysDirtyWhileItsRootIs`, `BindsARootThatStaysDirty`, `BindsTwoRootsOverAnAlwaysDirtyNode` | Over `test_nodes.hpp`'s always-dirty nodes, as `GradientNode`'s tests |
| `AThrowLeavesItDirtyAndItsValueAsItWas`, `RejectsWhatItsReductionRejects` | From the functor, from the bind, and from `make()` |
| `AFitInPlaceAllocatesNothing`, `ASolvingCycleAllocatesNothing` | A Gauss–Newton fit of two parameters to three roots through `makeInPlace`, and the solver: 200 cycles, 0 |
| `SolvesAcrossASelectionWhoseBranchesItIsGiven`, `MustBeGivenTheBranchesItFollows`, `AnArgumentItFollowsNeedsNoHearing` | D9, in an engine |
| `ReductionArgument.IsTheValueTheGraphHolds`, `RefusesWhatItDidNotRead` | At the bound point, whatever is called; `std::logic_error`, `std::out_of_range`, `std::domain_error` |

**Tests modified:** none. `Reduction.ASolverNodeInAnEngine` and its two neighbours keep holding the hand-bound
pattern, which stays valid, and the `solve` row keeps its graph.

| Change made by hand, then reverted | Must fail |
|---|---|
| The node binds with the clean check | `BindsARootThatStaysDirty`, `BindsTwoRootsOverAnAlwaysDirtyNode` |
| The node is built `Lazy` | `SeesAPointThatMovesWhileTheRootStandsStill` |
| The in-place path emits an rvalue | `AFitInPlaceAllocatesNothing` |
| `argument(i)` follows the trial value | `ReductionArgument.IsTheValueTheGraphHolds` |
| The node does not pull its roots | `AForcedEvaluationReachesItsInputs` |
| The other inputs are pulled after the bind | `SolvesAcrossASelectionWhoseBranchesItIsGiven` |
| The node follows a branch it was not given | `MustBeGivenTheBranchesItFollows` |

**Done when:** red then green; the checkpoint at 97 tests; each change as stated; every hand-made change of Steps 4
to 6 made again on this code; the probe, and the sweep of the nine rows against the build before Step 4, both ways,
in the Outcome; pushed. Commit: `feat: ReductionNode, a node that owns a reduction and binds it at each evaluation`.

### Step 8 — Example and docs

- `examples/fit.cpp`: a line fitted to three points that move, as a `ReductionNode` over three residuals and their
  Jacobian, checked against the closed form of the least-squares line. Its last line says whether they agree, and
  `example_fit` matches that line (98 tests).
- `README.md`: the Features entries; in Derivatives, the stateful nodes and the kept tape; in "A subgraph as a
  function", several roots, the condition and the node, whose snippet replaces the compute node's; the headers
  table.
- The header comments carry the rules and were written with the code, step by step, so this step adds none.
- `docs/engine_rules.md`, under "Writing nodes": that a node of your own joins a pass by overriding
  `NodeBase::partials()`, and that a node which solves is a `ReductionNode`, given every branch it may cross as an
  input. About 250 of the 350 bytes the budget has left. `CLAUDE.md` has 31 bytes left and does not change.

**Done when:** the checkpoint at 98 tests; `test_claude_md_budget` and `test_comment_refs` pass. Commit: `docs:
describe the stateful derivatives, the kept tape and ReductionNode, with an example`.

### Step 9 — Release v1.4.0, and close out

As `docs/releasing.md`: the scope and the access of each changed signature, the three version strings, the gate on
the Mac and in `ubuntu-dev`, the commit, the tag and the release page with its notes, then the runs read afterwards.
Then the Outcome below, a closing comment on each of the five issues and each closed by hand, and the milestone
closed. Commits: `build: release v1.4.0`, and `docs: record the outcome and close out the Differentiable subgraphs
milestone`.

## Not in this plan

- **Derivatives through the state, over time** (D2), and **the mixin off the engine's own nodes** (D3): the two
  issues of D13.
- **A step for a stateful node that holds no double.** `ThresholdNode`, the two debounce nodes, `WindowNode` and
  `RollingMinMaxNode` have no derivative, and no issue asks for a reduction through them. A `ThresholdNode`'s would
  cost it one flag. A `RollingMinMaxNode`'s cannot be had from its state: the samples its last one pushed out of
  the two deques are gone.
- **A lazy call.** A reduction that follows a condition runs both branches at each call. Running only the selected
  one would need the steps to be pulled, not run in order.
- **`GradientNode` and `TangentNode` on a reduction** (D6).
- **A derivative of a `ReductionNode`'s own value**, by the implicit function theorem: it is a barrier to a tape and
  to a reduction, as a `GradientNode` is.
- **Products with a vector**, J·v and Jᵀ·w, without the whole Jacobian.
- **A solver.** A root finder or an optimiser is the application's.
- **The window of 0**, flywheel-dag#80, which is a fix to four `make()` functions and wants its own tests.

## Self-review — risks and assumptions

- **Three refusals become answers.** A program that caught `std::domain_error` to learn that a derivative crosses a
  stateful node, or that a reduction crosses a condition, now gets a number. The release notes say which, and D12
  is where to say if that makes the release a major.
- **A stateful step is only as good as the sample the node took.** After a sample far out of scale, or one that is
  not finite, the function through that node is poor or NaN until the node's next update, and nothing says so at the
  call. The header states it, with the case of "Measured", and D5 is the cure.
- **"At the current state" can be misread as the whole derivative.** For an input that also fed earlier samples it
  is the latest sample's share. The header and the README say what it answers and what it does not.
- **A `ZScoreNode` built over a `RollingStats` of another upstream** can be clean while its stats update, and its
  partial and its step then read a mean and a spread newer than the ones it scored against. The two-argument
  `make()` exists to share one stats node over one upstream, and the comment there will say that the derivative
  assumes it.
- **An always-dirty node directly above a stateful node** is pulled again when a step binds, and a step then reads
  the sample of that second pull. For a node whose value is the cycle's time the two are the same. The caveat on
  always-dirty nodes in `dag_aad.hpp` covers the tape's side of this already.
- **A reduction that follows a choice can be refused from one cycle to the next**, when the other branch goes stale.
  A `ReductionNode` given its branches cannot. A hand-bound one can, and the message says what to do.
- **The gradient's rule changes one sentence of v1.3's contract** ("every step is asked"). No test of the suite can
  tell, and an application's own step that names fewer inputs than it reads could.
- **A kept tape holds every node it has met**, the ones above a barrier and on a branch no longer taken included,
  for as long as it lives. In a graph they are upstream nodes that the root keeps alive anyway.
- **The const sweeps keep their own barrier walk**, because a const function cannot write the stamps. Two walks must
  agree, and `ASpanSweepRefusesWhatAVectorSweepRefuses` is what holds them to it.
- **The fast paths are a second way through `bind()` and `gradient()`.** A point with no choosing node, and a
  gradient whose every step answers, run today's code, and the rest runs the new code. Each new suite has cases on
  both sides, and the 56 old tests are the fast side's.
- **New names.** `NodeBase::partials()` is a virtual with the mixin's name and signature. A node of an application
  that derives `NodeBase` and has an unrelated `partials` of another signature gets GCC's `-Woverloaded-virtual`,
  which `-Wall` turns on. `Reduction`, `aad::Tape`, `GradientNode` and `TangentNode` change size, which is no break in
  a header-only engine.
- **The two new checksums are measured on arm64 only**, under three compilers. The x86-64 reading is CI's.
- **The sweep is not proof.** It averages over where the code lands in a cache line. What shows that the cycle is
  the same is the machine code, function by function.
- **`docs/engine_rules.md` has 350 bytes left, and `CLAUDE.md` 31.** The two lines of Step 8 are sized against the
  first, and nothing is added to the second.
- **A push cancels the `CI` run in flight**, so a mid-plan checkpoint may get no macOS result; the next commit's run
  covers it, and the Outcome will say which runs were read.
- **This repo is public.** The plan, the commits and the issue comments name the engine only, and the leak audit
  runs over each before its push.
- **Assumptions:** graphs are not rewired after they are built; CI stays on `ubuntu-latest` and `macos-latest`; LLVM
  23 stays the lint gate's major.

## Outcome

**Done 2026-11-15.** v1.4.0 is tagged on the release commit and its release page is up, the five issues are closed,
each with a closing comment, the two issues of D13 are open, and the milestone is closed. D1 to D14 stood at their
defaults. D15 was put to you before the tag, with the figures under Step 7 below, and you chose to release.

| Step | Status | Commit |
|---|---|---|
| 1 — The plan | Done | `489c895`, and `df84034` for the approval; each issue names the plan in a comment |
| 2 — flywheel-dag#11: a tape through the stateful nodes | Done | `ae5ce40` |
| 3 — flywheel-dag#13: a tape that is kept | Done | `3bd9102` |
| 4 — flywheel-dag#75: several roots | Done | `97ab83e` |
| 5 — flywheel-dag#76: a condition an argument moves | Done | `10aebe7` |
| 6 — flywheel-dag#11: a reduction through the stateful nodes | Done | `d6cac06` |
| 7 — flywheel-dag#74: `ReductionNode` | Done | `b79cad8`, and `1f31326` for D15 |
| 8 — Example and docs | Done | `c3daa51` |
| 9 — Release v1.4.0, and close out | Done | `8d6db44`, tagged `v1.4.0`, and the commit that adds this section |

What each step's "done when" found, on the files as committed. Apple M4 Pro, Apple clang 21.0.0, CMake 4.4.5 and LLVM
23.1.4 on the Mac; GCC 13.3.0, CMake 4.4.5 and LLVM 23.1.6 in `ubuntu-dev`; GCC 15.2.0 in a throwaway `ubuntu:26.04`.
Every checkpoint passed as "Steps" defines it: no warning from any of the three compilers, `ctest` in full with the
lint gate on the Mac and in `ubuntu-dev`, the new suites under GCC 15.2, and `bench_hot_path --invariants` equal to
the committed file on all three. Every hand-made change failed a test written to hold it, beside a control that
passed. The probe is the one of "What the changes cost the calls that exist", against Step 3's headers, whose
reduction is v1.3.1's: 8 alignments, 8 rounds.

- **Step 2.** Red: `test_aad_timeseries` did not compile, and `AadReverse.APassDoesNotAdvanceAStatefulNode` threw
  "x reaches root root through ewma, which has no partials". `ANodeThatIsNotANodeBaseIsAskedThroughTheMixin` passed
  at the old headers: it holds the new split. The checkpoint passed at 92 tests, 138 with the gate. Ten hand-made
  changes. Every class is the size it was at `ff19003`. The benchmark's machine code against `ff19003`'s headers: of
  909 functions, 842 are the same, `Engine::cycle()`, `NodeBase::propagate()`, the four `equal()`, the six
  `flush()`, the four `emitImpl()` and 20 of the 22 `eval()` among them; 63 of the 67 that differ differ in one
  immediate, and the other four are the `eval()` of `EWMANode` and of `DeltaNode<double>`, one store longer each, and
  their constructors. The sweep against a second build of `ff19003`'s headers: each row once, `chain` +0.01% (slower
  at 9 of 16) and `ops-moving` +0.30% (9 of 16), where the control read +0.01% and +0.17%; in rotation, `chain`
  +0.79% (12 of 16) and `ops-moving` −0.19%, where the control read −0.13% on `chain`.
  - Departures: each `static_assert` says `sizeof == sizeof(double) + alignof(state)`, not "two doubles", so that an
    ABI with 4-byte alignment does not fail it; the suite also checks the two debounce nodes; `OPEN_ISSUES` gains
    81, for the header's citation.
- **Step 3.** Red at Step 2's headers: `test_aad_tape` did not compile, and both `AllocatesNothingOnceWarm` failed;
  the other new tests of the two sensitivity-node suites passed there, as the guards they are. There are six of them,
  not the seven an earlier note of this plan said. The checkpoint passed at 93, 140 with the gate. Twelve hand-made
  changes. The two new rows' lines came from the three compilers and from a plain loop, at `-O0` and `-O2`, to 17
  digits, and CI's Ubuntu leg read the same on x86-64. On the 17-node call, best of nine loops of 200,000, Step 2's
  headers against Step 3's: a tape built for the pass and swept once, 2,710 to 2,808 ns against 2,569 to 2,613; a
  `GradientNode`'s recompute, 2,738 to 2,815 against 582 to 584; a kept tape's `record()` and span sweep, 601 to 608,
  of which the record is 491 to 496 and a further sweep 84 to 88; the graph's evaluation, 201 to 203 in both. The
  nine rows swept against a control, each row once: every older row within −0.05% and +0.35%, slower at no more than
  10 of 16, and `gradient` −74% and `tangent` −70% at 0 of 16; in rotation, every older row within −1.32% and +1.49%,
  where the control read +0.01% to +1.35%. The +1.49% is `ingest`, slower at 15 of 16 in rotation and at 5 of 16
  once, where it read −0.05%.
- **Step 4.** Red: the new suite did not build at Step 3's headers. The checkpoint passed at 94, 142 with the gate.
  Seven hand-made changes; "every root's value is the first root's" fails nine tests only when it is made at both
  places a root's value is read. The probe: `gradient()` +3.0% over vol and +1.1% over five arguments, `bind()` +0.2%
  and +0.4%. The `solve` row, each row once: +0.24%, slower at 11 of 16, where the control read −0.03%.
  - Departures: `tests/reduce_test_nodes.hpp` also holds the suite's two message helpers; the comment on `bind()`
    says that a bind that throws holds nothing of the last point, which `HEAD`'s did not say of one that throws part
    of the way through.
- **Step 5.** Red: the 16 tests and the two renamed ones failed at Step 4's headers. The checkpoint passed at 95, 144
  with the gate. Six hand-made changes; the first reading that does not stop fails six tests here and eight at Step 7,
  where the table said nine. The probe: `gradient()` +1.5% and +1.4%, `bind()` +0.8% and +1.2%. The `solve` row, each
  row once: −1.21%, slower at 1 of 16.
  - Departures: the comments on `ReduceReads::decides()`, `ConditionNode` and its step are written for the rule; the
    README's sentence on a moved condition was made true here, ahead of Step 8.
- **Step 6.** Red: the 17 tests failed at Step 5's headers. The checkpoint passed at 96, 146 with the gate. Nine
  hand-made changes. Every node class is the size it was at `ff19003`. The benchmark's machine code against Step 5's
  headers: of 1,027 functions, 1,019 are the same, every `eval()`, every function of `ZScoreNode` and
  `Engine::cycle()` among them, and no row holds an `OutlierGateNode`, so `chain` was not swept. The probe:
  `gradient()` +2.7% and +0.8%, `bind()` +1.1% and +1.1%.
  - Departures: six reduce tests take a `Lazy` compute node over a `RollingMinMaxNode` as their barrier; the header
    lists the six callable nodes, and `dag_timeseries.hpp` gains a "Reductions" paragraph.
- **Step 7.** Red: the new suite did not build at Step 6's headers. The checkpoint passed at 97, 148 with the gate.
  The seven hand-made changes of the table, and the 24 of Steps 4 to 6 made again on this code. The probe: a call
  the same (+0.7% and −0.5%), `gradient()` +2.5% over vol and +1.4% over five, slower at 8 of 8, and `bind()` +3.8%
  over vol (102.8 to 106.9 ns against 99.2 to 102.2) and +3.9% over five. `bind()` had grown from 831 instructions to
  1,399, from 1,335 at Step 6.
  - Two builds of these headers took `bind()`'s part apart. Without the store that keeps each argument's own value,
    it read +2.1% and +1.2%. With `bind()` and `rebind()` as one body, which lets go of the point first and checks
    each root clean where it reads it, as v1.3.1's `bind()` did, it read +0.1% over vol (100.9 to 103.7 ns against
    100.2 to 103.2) and +1.7% over five, at 1,346 instructions; a `bind()` and six gradients −0.5%. `gradient()`'s
    code is the same in the three builds, and read +0.2% to +2.6% between them: that is what layout gives it.
  - The second build is `1f31326`, a commit of its own, its `bind()` the same instructions. Two tests check that a
    refused bind lets go of its point, and pass at the headers before it. Six hand-made changes: `rebind()` checks
    one root, or several; `bind()` skips the check of one root, or of several; a refused bind keeps its point, for one
    root or for several. Its checkpoint passed at 98, 150 with the gate, on the Mac and in `ubuntu-dev`, and the 37
    suites passed under GCC 15.2. The `solve` row alone, each row once, against Step 3's headers: −0.55%, slower at 2
    of 16, where the control read +0.10%.
  - The nine rows swept against Step 3's headers, with a control: each row once, every older row within −0.45% and
    +0.04% but `solve`, −2.24% at 0 of 16, and `gradient` and `tangent` +0.30% and +0.32%, slower at 11 of 16; in
    rotation, `idle-queues` +1.18%, `ops-moving` +1.08% and `ops-held` +0.80%, slower at 13 or 14 of 16, `solve`
    −2.23%, and the rest within −0.57% and +0.29%. The control read −0.27% to +0.23% once and −0.20% to +0.50% in
    rotation. Of the 1,010 functions both binaries hold, 1,001 are the same, `Engine::cycle()` and every `eval()`
    among them; the nine that differ are four of `Reduction`'s, its `construct_at`, the `solve` row, in an
    immediate, two of `Tape`'s, at the same lengths, and the stubs. So what moves three rows in rotation is the layout
    and the heap behind a `Reduction` of 680 bytes, which "Measured" found too.
  - D15, put to you with these figures before the tag: `bind()` over one argument and a call cost what they cost at
    v1.3.1; `bind()` over five arguments 1.7% more, the store D11 accepted; `gradient()` 0.2% to 2.6% more, within
    what layout gives it. You chose to release.
  - Departures: the code is the second prototype's but for a guard that measured the store `argument()` needs, which
    is dropped, the order of the declarations and the comments; the suite takes the call price of
    `tests/reduce_test_nodes.hpp`.
- **Step 8.** The checkpoint passed at 98, 150 with the gate, on the Mac and in `ubuntu-dev`, and the four examples
  build with no warning and agree under GCC 15.2. The README's three new snippets were compiled with `-Wall -Wextra
  -Wpedantic -Werror` and run outside the tree. `docs/engine_rules.md` is 5,887 of its 6,000 bytes, 166 more, where
  the plan said about 250; `CLAUDE.md` did not change, and `test_claude_md_budget` and `test_comment_refs` pass.
- **Step 9.** Eight commits change what a consumer sees. Every signature they change is public and new; `aad::Tape`'s
  default constructor, `add()` and `record(root)`, which it lost, were private. So the number is v1.4.0, as D12 said.
  The gate on the tree as committed: the Mac and `ubuntu-dev` configure with CI's flags and the lint gate, build with
  no warning and pass 150 of 150, `test_package_version` among them, and `--invariants` is the committed file; under
  GCC 15.2 the four examples build with no warning and agree. Sizes against v1.3.1: `aad::Tape` 416 bytes, from 112,
  `GradientNode` 632, from 192, `TangentNode` 640, from 200, `Reduction` 680, from 344, and the other 28 lines of the
  size table as they were. The benchmark built from one source against v1.3.1's headers and against v1.4.0's: of the
  966 functions both hold, 880 are the same and 63 differ in one immediate, and `Engine::cycle()` and every `eval()`
  of the seven older rows but `EWMANode`'s and `DeltaNode<double>`'s, one store longer each, are among the 880. The
  release page is up, and `v1.4.0` is the latest release.
- **CI.** Every run on this plan's commits finished, and none was cancelled by a push. `CI` passed on `ae5ce40`
  (43715169832), `3bd9102` (43717774866), `97ab83e` (43723178687), `10aebe7` (43724534651), `d6cac06`
  (43725892077), `b79cad8` (43726683237), `c3daa51` (43730732156), `1f31326` (43731399874) and the release commit
  (43735357847), and `clang-tidy` on each of them (43715169833, 43717774862, 43723178612, 43724534589, 43725892114,
  43726683304, 43730732122, 43731399916 and 43735357821). The nightly `Sanitizers` run before them, 43707700589 on
  `ff19003`, passed; the first on these commits is the next night's.

Where the work differs from the plan:

- **`1f31326`**, which is not a step: Step 7's code, made again so that `bind()` costs what it did (above).
- **Steps 4 to 7 were staged** each in a scratch copy, from the prototypes and test first, and verified there with
  the same checks before they were applied to the checkout.
- **The six tests of Step 3** in the two sensitivity-node suites, where a note said seven.
- **`docs/engine_rules.md` took 166 bytes**, not about 250.

Left for later: nothing of this plan. Open: flywheel-dag#80, flywheel-dag#81 and flywheel-dag#82.
