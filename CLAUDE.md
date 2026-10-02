# CLAUDE.md — flywheel-dag

The DAG engine is **genuinely header-only**. Every class and function is a
template (or depends on templates), so all method bodies must be visible at
the point of instantiation. The `.hpp`/`.inl` split is **required** here —
nothing in the engine can move to a `.cpp`.

---

## File Layout Rule

```
dag.hpp          — class declarations (templates only)
dag.inl          — all template method bodies
dag.hpp          — ends with: #include "dag.inl"
```

- `.hpp` = declarations only; no method bodies.
- `.inl` = method bodies for the class(es) declared in the paired `.hpp`.
- `.hpp` `#include`s its `.inl` at the bottom so consumers get both in one include.
- **Never include a `.inl` directly** — always include the `.hpp`.
- There are **no `.cpp` files** in the engine — they are never needed.

---

## Build & Test

```bash
cmake -B build && cmake --build build
ctest --test-dir build
```

The default build type is `RelWithDebInfo` (`-O2 -g -DNDEBUG`). **`NDEBUG` is
defined in the default build, so `assert()` does not run.** Any validation that
must survive has to throw — see the `alpha` range checks in `EWMANode::make` /
`EWMATickRateNode::make`.

**The build is warning-free, and CI keeps it so** (flywheel-dag#4). The tests,
the example and the benchmark compile with `-Wall -Wextra -Wpedantic` on GCC and
Clang, and CI configures with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, so any
warning fails it. Turn the option on locally to see what CI will see.

- **Fix a warning; do not silence it.** The one exception is a confirmed
  compiler false positive. Silence that at the one site, for the one compiler,
  with a comment naming the compiler bug. The counting `operator delete` in
  `test_value_slot.cpp` and `bench_hot_path.cpp` does this for GCC bug 103993.
- **Hold an evaluated `ValuePtr` in a local before binding a reference into it:**
  `const ValuePtr v = node->eval(ctx); const T& x = get_value<T>(v);`. A
  reference bound to `get_value()` of the temporary relies on the producer's
  `cached_` alone, and GCC's `-Wdangling-reference` rejects it.
- **Brace an `if` whose body is a gtest check.** `EXPECT_…` and `ASSERT_…` expand
  to an `if` with an `else` of their own, and GCC's `-Wdangling-else` rejects one
  under an unbraced `if`. Apple Clang does not warn, so only CI's GCC leg sees it
  (flywheel-dag#18).
- **The flags are the top level's alone.** Never put a flag on `flywheel_dag`'s
  `INTERFACE` or in the cache: a consumer's flags are its own.
  `test_warning_flags` checks that every translation unit of this project gets
  the flags and no dependency's does. `test_consumer_subproject` checks that a
  consumer gets none.

**The benchmark** — `benchmarks/bench_hot_path` is `EXCLUDE_FROM_ALL` and not a
ctest test. Time a change by running it before and after, on the same machine:

```bash
cmake --build build --target bench_hot_path && ./build/bin/bench_hot_path
```

Its exact columns — `allocs/cycle`, `callbacks` and `checksum` — show that a
speed-up did not come from doing less work. CI diffs them (`--invariants`)
against `benchmarks/expected_invariants.txt`:

- A non-zero `allocs/cycle` is an engine regression. Fix the engine; never
  regenerate the file to make CI pass.
- A changed `callbacks` or `checksum` means the graph did different work.
  Regenerate the file (`./build/bin/bench_hot_path --invariants >
  benchmarks/expected_invariants.txt`) only when that is the point of the
  change, and say why in the commit.
- The checksum is identical on every platform only because the target builds
  with `-ffp-contract=off` and the rows use nothing but `+ − × ÷` and `sqrt`.
  Keep them that way: `exp`, `log` and `pow` differ between math libraries.

**The install is the engine alone** (flywheel-dag#2): the headers, a
`find_package(flywheel_dag)` config and the LICENSE file. `FLYWHEEL_DAG_INSTALL`
turns the rules on, and it is on at the top level only.

- **The export names no dependency.** The copies of spdlog and nlohmann/json
  that the build fetches are its own targets and in no export set, so
  `install(EXPORT)` refuses to export a link to them. The build therefore links
  them through `$<BUILD_INTERFACE:…>`. `cmake/flywheel_dagConfig.cmake.in` links
  them after finding them, and looks for a dependency only when the consumer
  has no target for it yet. Never make the export name them, and never install
  them: an install into `/usr/local` would overwrite any copies already there.
- **A new dependency goes in three places:** the root's FetchContent block and
  its `$<BUILD_INTERFACE:…>` link; the config's guarded `find_dependency()` and
  its `set_property` link; and the tests, in `tests/install_dependencies.cmake`
  and the consumer's `CONSUMER_FETCHES_DEPENDENCIES` block.
- **A new header needs nothing more,** as long as it is a `.hpp` or an `.inl`.
  `test_install` fails on any header that is not installed.
- **The version file uses `SameMinorVersion`** until 1.0, and
  `SameMajorVersion` after that.
- **The package consumers search `CMAKE_PREFIX_PATH` alone,** so a package
  installed on the machine cannot stand in for one a test installed.

---

## Key Patterns

**Node factory** — Always use static `make()` — never construct directly
(required for `shared_from_this()` wiring):

```cpp
auto node = ComputeNode<double, double, double>::make(
    "name", { input_a, input_b },
    [](const double& a, const double& b) { return a / b; }
);
```

**Equality policies** — Passed as optional arg to `make()`:

| Policy | Use |
|---|---|
| `TypedEqualityPolicy<T>` | Default (`operator==`) |
| `EpsilonPolicy<T>` | Suppress tiny float changes |
| `AlwaysChangedPolicy` | Stateful nodes that always need re-evaluation |
| `PredicateEqualityPolicy` | Custom comparison logic |

**Lazy evaluation** — Nodes cache a value and a `Dirtiness` state. `eval()` runs
only when not `Clean`. `Input::set()` propagates downstream. A node is clean only
when every input it read still is: an evaluation that a "maybe" reaches before it
ends leaves the node dirty, and tells its consumers (flywheel-dag#18; see
`dag::NodeBase` below).

**`InvalidationMode` — per node, set at construction, never changed:**

| Mode | Meaning | Who gets it |
|---|---|---|
| `Eager` | Recompute whenever anything upstream fired. **The default.** | Anything whose output is not a pure function of its declared inputs' *values* — every `dag::ts` stateful node (its output depends on *how often* it ran), `aad::GradientNode` and `aad::TangentNode` (their values depend on the partials of every node above their roots), and any functor reading state it did not declare as an input. |
| `Lazy` | Recompute only when an input's value actually changed. | A functor that is a pure function of its declared inputs. |

`Dirtiness` has three states, not two: `Dirty` means an input of mine definitely
changed (one hop, from a node whose `eval()` saw its own value move); `Maybe`
means an ancestor may have; `Clean` is up to date. `dirty()` is `state != Clean`.
A `Lazy` node pulls every input and, if it is still only `Maybe` afterwards,
skips its functor — because an input that really changed would have called its
`invalidate()` on the way past.

The mode is a `make()` argument on the four opaque-functor templates
(`ComputeNode`, `InPlaceComputeNode`, `TweakableComputeNode`,
`MemoizedComputeNode`); it is **fixed in the base** for `dag::ops` (`Lazy` —
structurally pure, since `eval()` default-constructs the functor every time),
`ConditionNode` (`Lazy` — pure selection, no functor), `dag::ts` (`Eager`) and
`aad::GradientNode` and `aad::TangentNode` (`Eager` — see Algorithmic
Differentiation).
Set it in the factory that **writes** the functor, not at the graph site: whether
a functor is pure is a property of the functor.

> **A node that reads state it did not declare as an input must be `Eager`, and
> the marking is documentation rather than redundancy.** Typical examples: a
> functor that mutates captured state; a functor that ignores its declared input
> and reads `mean()` *and* `stddev()` off a captured `RollingStats`; and every
> `IWindowed::windowStatusNode()`, which returns `capacity()`/`filled()` off its
> upstream, so it changes while its declared input stands still. The last was
> found by instrumenting a recorded replay, not by reading the code.

**What an equality policy can and cannot gate** — a policy's verdict decides
whether a node calls `notifyDownstream()`. What that buys depends on where the
node sits **and on its consumers' `InvalidationMode`**:

| Position | What its policy gates |
|---|---|
| A **source** — `Input::set`, `AsyncInput::flush` | The invalidate cascade itself. |
| A **registered engine output** | The `addOutput` callback, via `ValuePtr` identity in `Engine::cycle`. |
| An **intermediate** node with **`Lazy`** consumers | Their recomputation — a consumer resolves by pulling its inputs and seeing whether any said "changed", so an "equal" verdict is what makes it skip. |
| An **intermediate** node whose consumers are all **`Eager`** | Nothing. An `Eager` node recomputes whenever anything upstream fired, so the comparison is pure cost — harmless for a `double`, real work for a container. Such a node should take `AlwaysChangedPolicy`. |

The last two rows are the same node with different consumers, which is why this
is a per-node judgement rather than a rule.

A policy compares the new value against the last one the node **published**,
not the last one it computed. Only an "unequal" verdict rebinds `cached_`, and
`eval()` returns what `cached_` holds. Under a tolerance policy, a slow drift
therefore publishes once it has moved the tolerance in total. Comparing against
the previous evaluation instead would never publish it at all.
`StatefulNodeBase.EqualityPolicyComparesAgainstTheLastPublishedValue` holds this
for stateful nodes (flywheel-dag#1).

Both halves are pinned, on the same graph, by tests that assert opposite outcomes
and are both correct:
`DAGTests.EqualityPolicyOnIntermediateNodeDoesNotSuppressDownstreamEval` (Eager,
`downEvals == 10`) and
`DAGTests.EqualityPolicyOnALazyIntermediateNodeDoesSuppressDownstreamEval`
(`Lazy`, `downEvals == 1`).

**`InPlaceComputeNode<Out, Ins...>`** — same contract as `ComputeNode`, but its
functor is `void(Out& out, const Ins&...)` and writes into a **retained** scratch
buffer, which `ValueSlot::emit(const T&)` then copy-assigns into the recycled
`TypedValue`. Use it when `Out` owns heap: the steady state allocates nothing
where `ComputeNode` costs one allocation per heap member per eval.
**`out` arrives holding the previous evaluation's value** — the functor must
overwrite everything it owns on every path, early returns included. A functor
that appends without clearing grows without bound, and nothing catches it but a
test that evaluates twice.

**Thread model** — Single-threaded eval; no locks during DAG traversal. Feed
threads call `AsyncInput::post()` (mutex-guarded); `flush()` drains on the
eval thread. Engine wakes via `std::condition_variable` + atomic bool.

**Cross-thread reads of callback-written state** — `Engine::cycle()` holds a
`CycleSeqLock` (a seqlock, `dag_engine.hpp`) open for its whole body. A thread
that needs several callback-written atomics as one consistent set takes
`engine.cycleSeqLock()` (in `wire()`) and reads inside `readConsistent(fn)`: it
waits out an open cycle and retries a read that a new cycle interrupts, and the
eval thread never blocks. Two rules: on the eval thread inside a cycle it
returns `fn()` at once (the replay exhaustion callback relies on this), and it
must not be called while holding a lock an output callback needs — it would
wait on a cycle that cannot finish.

> **Staging storage is recycled, not reallocated.**
> `AsyncInput<T>::post()` takes `const T&` and copy-**assigns** into a live
> `pending_` member; `flush()` **swaps** `pending_` with `staged_` and emits
> through `ValueSlot::emit(const T&)`, which copy-assigns into the recycled
> `TypedValue` buffer. Capacity cycles between the four buffers instead of being
> freed and reallocated, so a `T` that owns heap — a snapshot posted on nearly
> every cycle, say — costs no allocation once warm. Three consequences worth
> knowing: `T` must be default-constructible and copy-assignable; `post()` never
> consumes the caller's value (a feed that merges deltas keeps its value to merge
> the next delta into, which is why the copy is not removable); and the copy
> happens **under** the staging lock. `ValueSlot::emit` keeps an rvalue overload
> that still moves, and every compute/op/time-series node returning a scalar
> takes it: it built the value for this emit alone, so there is nothing to
> recycle. **That reasoning does not hold for an `Out` that owns heap** — there,
> building it fresh IS an allocation, and the move-assign then frees the very
> buffer the slot was keeping. Such a node uses `InPlaceComputeNode` (above)
> instead.

> **The engine owns flushing.** Only the engine (via `IFlushable*` through its
> `FeedRegistry`) may call `flush()` on a live source. Never call
> `AsyncQueue::flush()` directly on a concrete queue outside a self-contained
> unit test that owns the queue: `flush()` always refreshes the cached batch,
> so a redundant flush after the queue has drained overwrites it with `[]` and
> **silently discards the batch** (`post(t); flush(); engine.step()` loses `t`).

> **That refresh must stay, and it must not allocate.** A downstream node
> dirtied by a *different* input still calls `eval()` on the queue and has to
> read `[]` rather than the batch it already consumed — so moving `flush()`'s
> `n == 0` return above the refresh is a double-counting bug, not an
> optimisation. The empty batch is a constant: the constructor's initial value is
> kept as `emptyValue_` and the empty path rebinds `value_` to it, one allocation
> for the node's lifetime. Before that, every queue paid a fresh allocation on
> every cycle, including the great majority where nothing arrives.
> `AsyncQueue.EmptyFlushRefreshesToEmptyNotStaleBatch` is the guard; it goes red
> on the shorter wrong fix.

> **`flush()` must not construct a `std::deque`, on any path.** libstdc++
> allocates a deque's map *and* its first block in the **default constructor**
> (libc++ waits for the first `push_back`), so the drain deque `flush()` used to
> declare as a local cost two allocations per queue per cycle on Linux — on the
> idle path too, where there is nothing to drain. That is why the idle-queue
> guard passed on macOS and failed in CI. `drain_` is a member, swapped with
> `queue_` and cleared, so both deques keep their blocks;
> `ValueSlot.QueueArrivalAllocatesOnlyItsBatch` pins the arrival path at 2
> allocations (the batch vector and its `TypedValue`) and goes red if a local
> comes back.

**A cycle that throws** (flywheel-dag#20) — `Engine::cycle()` marks each output
due when its node is dirty, before it evaluates any of them, and clears the mark
when it reaches the output. A node's `eval()` or an output callback that throws
ends the cycle, and the outputs after it stay due, so the next cycle delivers the
value each holds then. The marks used to be a snapshot that each cycle
overwrote, so an output that an earlier one had pulled clean read clean at the
next cycle, and its callback missed the value its node held until that value
moved again.

- **An output whose node throws stays due.** The mark is cleared only once
  `eval()` returns, whatever state the throw left the node in.
- **A callback that throws has had its value.** The mark is cleared, and
  `lastSeen` set, before the callback runs, so the engine does not offer that
  value again, only the next one. Offering it again would call a callback that
  throws on a value once more on every cycle, and hold back every output after
  it.

`EngineAbortedCycle` in `test_dag_async.cpp` pins both. A new output starts
unmarked, so one registered on a node that is already clean waits for the node
to move (flywheel-dag#23).

**Tweakable nodes** — `TweakableComputeNode::tweak(v)` freezes output
mid-graph. A changed tweak reaches the node's own engine output once, on the
engine's next cycle, because `tweak()` leaves the node dirty until it is
evaluated. An equal tweak only freezes, and `cached_` keeps its identity
(flywheel-dag#5). `clearTweak()` resumes: the node goes Dirty and its consumers
Maybe, so a Lazy consumer skips when the recomputed value equals the frozen one
(flywheel-dag#8).

**`ConditionNode` hears only the branch it took** (flywheel-dag#18). It pulls the
condition and the branch the condition selects, never the other, and each branch
reaches it through a listener of its own, which passes an invalidation on only
while the node takes that branch. A move of the other branch reaches none of its
consumers: an `Eager` node below it does not recompute for it, a stateful one does
not tick, and a sensitivity node does not record. A switch arrives through the
condition, which the node always hears, and reads the branch fresh. Hearing both
branches gave a stale value: a consumer's later pull could evaluate the stale
branch after the consumer had read the `ConditionNode`, and the consumer ended
clean over it. The `UntakenBranch` tests in `test_clean_inputs.cpp` pin it.

**Custom node design** — Compose `ComputeNode` (+ captured mutable state), or a
`dag::ts::StatefulNodeBase` for incremental time-series state, or a
`dag::ops::OpNodeImpl<Derived>`-based op (see below) for a stateless arithmetic
primitive that needs its own identifiable type, or an
`aad::DifferentiableNode<N>` for a functor a tape must differentiate (see
Algorithmic Differentiation). If you genuinely need a node of your own, derive
from **`dag::NodeBase`**, never from `dag::INode` — `INode` is the interface the
engine calls through, not a base to build on.

**`dag::NodeBase` — the one copy of the dirty/downstream protocol.** It owns
`downstream_`, the dirty flag, and `dirty()` / `invalidate()` / `addDownstream()`
/ `notifyDownstream()`, and the bracket around an evaluation that pulls,
`beginEval()` / `endEval()`. Every node in the engine derives from it: `Input`,
`ComputeNode`, `InPlaceComputeNode`, `TweakableComputeNode`, `ConditionNode` (and
its branch listeners), `AsyncInput`, `AsyncQueue`, `MemoizedComputeNode`,
`ReplayInput`, `ReplayQueue`, `ts::NodeImpl`, `ops::OpNodeImpl`,
`aad::DifferentiableNode`, `aad::GradientNode` and `aad::TangentNode` — and so
should any node an application defines.

`downstream_` is **private**. Reaching downstream goes through
`notifyDownstream()` — several of the twelve copies this replaced walked the
vector inline instead of calling their own helper, which is how one idea drifted
into four spellings of it.

**A node is clean only when every input it read still is** (flywheel-dag#18). The
cascade's guard stops at a node that is already dirty, which is safe only while a
clean node's inputs are clean. An evaluation breaks that when an input it has
already read goes dirty again before it ends, as when an always-dirty node, pulled
again through a later input, tells the earlier input's nodes. That input sends a
"maybe" cascade, which reaches the node while it is still evaluating, so a "maybe"
is the signal: `propagate()` records every one.

- A node that pulls calls `beginEval()` before its first pull, which forgets the
  "maybe"s that came before the evaluation, and `endEval()` in place of
  `markClean()` wherever its evaluation ends, skip paths included.
- `endEval()` marks the node clean, unless a "maybe" reached it during the
  evaluation. Then the node keeps the state its evaluation left it in, so a Lazy
  node that read an input since evaluated again still recomputes, and tells its
  consumers "maybe", so the one evaluating it now stays dirty too.
- Sources, and a tweaked node returning its frozen value, pull nothing and call
  `markClean()`. A node of your own that pulls should use the pair: one that calls
  `markClean()` works as before, and keeps the exposure.
- Asking each input for `dirty()` instead would leave every consumer of an
  always-dirty node dirty for good, and cost a virtual call per input.
- The stay is `[[gnu::cold, gnu::noinline]]`. Inlined into every evaluation, the
  rule cost `bench_hot_path`'s chain row 2.4%; out of line, 1 to 2%.
- `test_clean_inputs.cpp` pins the rule for every node kind that pulls, with the
  always-dirty test nodes in `tests/test_nodes.hpp`.

Overriding `dirty()` or `propagate()` — the one override point for invalidation,
since `invalidate()` and `invalidateMaybe()` are `final` — needs a reason, and
only three are known:

| Node | Override | Why |
|---|---|---|
| `TweakableComputeNode` | `propagate()` absorbs while frozen | A tweaked value does not depend on its inputs. |
| A `ConditionNode`'s branch listener | `propagate()` passes the invalidation on to the node, as the same kind, while the node takes its branch | The node's value does not depend on the other branch (flywheel-dag#18). |
| A clock-driven node (application-defined) | `dirty()` is always `true`; `propagate()` forwards unconditionally | Its output is a function of a clock, so it is never clean, and the inherited `state_` guards in `propagate()` would swallow every invalidation after the first. |

Anything else overriding these is re-implementing the protocol rather than using
it.

A clock-driven node reached by two paths leaves the node where they meet dirty
after every evaluation, so that node, and everything below it, evaluates again on
every pull and every engine cycle. And the clean-inputs rule cannot see through a
clock-driven node that forwards with `notifyDownstream()`, which says "changed":
if a node above it went dirty again during a consumer's evaluation, the consumer
would hear "changed" and end clean. That needs a second always-dirty node, or a
forced evaluation, above the first.

---

## State Persistence — Discovery, Not Registration

`Engine::saveState()` and `Engine::restoreState()` find the stateful-node
set by walking the DAG from every registered output via `INode::inputs()`,
deduped, BFS order. Any `IStatefulNode` reachable that way is persisted —
no hand-maintained list anywhere.

The traversal primitive is `dag::traversal::bfs_upstream(roots)` (see
[`dag_traversal.hpp`](include/flywheel/dag_traversal.hpp)); the engine
helper is `Engine::discoverStatefulNodes()`.

**Implications for module authors:**

- A stateful node is persisted iff some `addOutput` registered on the
  engine reaches it through its inputs chain. If you build a stateful
  node and forget to wire it through, it is not persisted. Wire it.
- Two modules sharing an upstream stateful node persist it once
  (BFS dedup by pointer identity), regardless of which module owns the
  `shared_ptr`.
- To **exclude** a stateful node from snapshots, override
  `IStatefulNode::persistState()` on the node itself to return `false`.
  Default is `true`. Useful for transient / diagnostic nodes whose
  accumulated state has no cross-restart meaning.
- The on-disk JSON format keys by `INode::name()`; node names must be
  unique across the whole DAG and stable across restarts (no embedded
  timestamps or addresses).

The forget-to-register-stateful bug class — adding a stateful node and
forgetting to extend a manual list — does not exist by construction.

---

## Time-Series Nodes (`dag_timeseries.hpp`)

All time-series nodes live in `dag::ts::` and inherit `StatefulNodeBase<Derived, Out, In, State>`.
Each stores only the state needed for its O(1) incremental update.

| Node | Output | Purpose |
|---|---|---|
| `WindowNode<T>` | `std::deque<T>` | Last N values |
| `RollingStats` | — | Incremental mean + stddev (Welford); read via `mean()`, `stddev()` |
| `RollingMinMaxNode` | `std::pair<double,double>` | Sliding min/max, O(1) amortised |
| `EWMANode` | `double` | Exponential weighted moving average |
| `DeltaNode<T>` | `T` | First difference (value − previous) |
| `DelayNode<T>` | `T` | N-tick ring buffer |
| `makeTimeDelayNode<T>` | `std::optional<T>` | **Time**-based delay: value as of `now − horizonUs` (see below) |
| `ThresholdNode<T>` | `bool` | Level detector with optional hysteresis — **stays true while above threshold** |
| `ZScoreNode` | `double` | (x − μ) / σ using a shared `RollingStats` |
| `OutlierGateNode` | `double` | Passes raw value if \|z\| < threshold, else mean |
| `RateLimiterNode<T>` | `T` | Suppresses downstream if \|Δ\| < minDelta |
| `DebounceCountNode` | `bool` | True only after N consecutive true ticks — **level output, stays true** |
| `LatchedDebounceNode` | `std::optional<bool>` | Edge-triggered onset/resolved with debounce — see below |

### `LatchedDebounceNode`

Combines debounce counting and an active/resolved latch into a single stateful node.
Use this instead of `DebounceCountNode` when you need exactly one event at onset and
one at resolution — not a continuous `true` signal for every tick the condition holds.

**Output semantics:**

| Output | Meaning |
|---|---|
| `true` | Onset: N consecutive `true` ticks confirmed; latch was previously clear |
| `false` | Resolved: upstream went `false`; latch was previously set |
| `nullopt` | No transition |

The default `TypedEqualityPolicy<std::optional<bool>>` handles suppression:
`nullopt == nullopt`, so a run of non-transitions neither notifies downstream nor
fires an `addOutput` callback. The exception is the tick straight after a
transition. Going from `true` or `false` back to `nullopt` is a change of value,
so a callback runs once per transition and then once more with `nullopt`. It
must ignore the `nullopt`. Pinned by
`LatchedDebounceNodeTests.EngineCallbacksFireOnTransitionsAndOnTheReturnToNullopt`.

**State** (`LatchedDebounceNodeState`):

```cpp
struct LatchedDebounceNodeState {
    bool        latched = false;
    std::size_t count   = 0;
};
```

Both fields are saved and restored by `IStatefulNode` — the latched alert state survives
a restart without replaying history.

**Resolved transitions are immediate** (no debounce on the falling edge). The upstream
`ThresholdNode`'s hysteresis is the right place to prevent chatter on recovery.

**Typical chain:**

```cpp
// ThresholdNode → LatchedDebounceNode
auto alert = ThresholdNode<double>::make(name, upstream, level, Direction::Above, hys);
auto latch  = LatchedDebounceNode::make(name + "_latch", alert, debounce_ticks);
engine.addOutput(latch, [](const std::optional<bool>& t) {
    if (!t) return;           // nullopt — the tick after a transition
    const char* state = *t ? "active" : "resolved";
    // ...
});
```

### `makeTimeDelayNode<T>` — time-based delay

`DelayNode<T>` delays by a fixed number of *ticks*; `makeTimeDelayNode<T>` delays by *elapsed
time* — its output is the value that was current at `now − horizonUs`. Use it when the horizon is
a duration (e.g. 1s/5s/30s) rather than a tick count: it is robust to irregular update cadence
(a quiet stream vs a busy one) and replay-correct.

**Time is a DAG input, not a wall-clock read.** The node takes two pure inputs — the `value`
series and a monotonic microsecond clock `timeUs` — so it depends only on its inputs and is
deterministic under replay. The app supplies `timeUs` from its own clock (real `steady_clock`
live · `ReplayClock` in replay · the app's own cycle clock), set once per cycle. No node in the
engine reads a wall clock; this keeps the engine's purity contract intact.

**Output `std::optional<T>`:** `nullopt` until at least `horizonUs` of history exists (warm-up), so
downstream gates on `has_value()`; the default `TypedEqualityPolicy<std::optional<T>>` suppresses
the `nullopt == nullopt` stretch.

**Realization:** a factory returning a `ComputeNode` with a captured `(time,value)` ring — the
"ComputeNode + captured state" pattern (§Key Patterns), because a *two-input* node cannot use the
single-input `StatefulNodeBase`. It is deliberately **not** an `IStatefulNode`: the timestamps are
clock-epoch-relative (a restart resets `steady_clock`'s epoch), so a restored buffer would be
meaningless, and at seconds-scale horizons re-warming costs nothing.

---

## Arithmetic and Trigonometric Ops (`dag_ops.hpp`)

All op primitives live in `dag::ops::` and inherit `OpNodeImpl<Derived>`
(dirty/downstream/`kind()` boilerplate, parallel to `dag::ts::NodeImpl<Derived>`
but reports `NodeKind::Compute` — these are stateless, not incremental).

| Node | Arity | Output | Purpose |
|---|---|---|---|
| `SumNode<T>` | n-ary (`vector<NodePtr>`) | `T` | `ins[0] + ins[1] + ...` (0 inputs → `T{}`) |
| `ProductNode<T>` | n-ary (`vector<NodePtr>`) | `T` | `ins[0] * ins[1] * ...` (0 inputs → `T{1}`) |
| `DiffNode<T>` | binary | `T` | `a - b` |
| `DivideNode<T>` | binary | `T` | `a / b` — unguarded; `b == 0` propagates IEEE inf/nan |
| `NegateNode<T>` | unary | `T` | `-a` — signed `T` only |
| `ExpNode<T>` | unary | `T` | `exp(a)` — floating-point `T` only |
| `LnNode<T>` | unary | `T` | `ln(a)` — floating-point `T` only; unguarded, `a <= 0` propagates IEEE inf/nan |
| `PowerNode<T>` | binary | `T` | `a ^ b` — floating-point `T` only; unguarded, negative base + non-integer exponent propagates NaN |
| `SqrtNode<T>` | unary | `T` | `sqrt(a)` — floating-point `T` only; unguarded, `a < 0` propagates NaN |
| `SinNode<T>` | unary | `T` | `sin(a)`, in radians — floating-point `T` only; unguarded, infinite `a` propagates NaN |
| `CosNode<T>` | unary | `T` | `cos(a)` — as `SinNode` |
| `TanNode<T>` | unary | `T` | `tan(a)` — as `SinNode`. No double is a pole: π/2 is not one |
| `AsinNode<T>` | unary | `T` | `asin(a)` — floating-point `T` only; unguarded, `\|a\| > 1` propagates NaN |
| `AcosNode<T>` | unary | `T` | `acos(a)` — as `AsinNode` |
| `AtanNode<T>` | unary | `T` | `atan(a)` — floating-point `T` only |
| `Atan2Node<T>` | binary | `T` | `atan2(a, b)`, the angle of the point `(b, a)` — **y first**, in `std::atan2`'s order; floating-point `T` only |

`T` defaults to `double`. `SumNode`/`ProductNode`/`DiffNode`/`DivideNode` constrain to
`std::is_arithmetic_v<T>`; `NegateNode` tightens to `std::is_signed_v<T>` (negating
unsigned silently wraps); `ExpNode`/`LnNode`/`PowerNode`/`SqrtNode` and the seven
trigonometric nodes tighten to `std::is_floating_point_v<T>` (`<cmath>`'s
integral-promoting overload would silently truncate an integral `T`).

**Why dedicated types, not `ComputeNode<T, ...>` + a lambda:** each op is its own
concrete C++ type so that `inputs()` plus the op's identity are enough for a tape
(see Algorithmic Differentiation) to attach a closed-form local derivative — a
`ComputeNode` wrapping an arbitrary lambda can't supply that, since the lambda body
is opaque to the graph. Each op reports its partials through `aad::IDifferentiable`,
from `ops::Derivative<Op>`:

| Op | Partials |
|---|---|
| `SumNode` | `1` for each input |
| `ProductNode` | The product of the other factors, from prefix and suffix products — exact at a zero factor, where `product / x_i` is 0/0 |
| `DiffNode` | `+1`, `-1` |
| `DivideNode` | `1/b`, `-a/b²` (computed as `-(a/b)/b`) |
| `NegateNode` | `-1` |
| `ExpNode` | `exp(a)` |
| `LnNode` | `1/a` |
| `PowerNode` | `b·a^(b-1)`, or `0` when `b` is 0; `a^b·ln(a)`, or `0` at `a = 0` with `b > 0` |
| `SqrtNode` | `1/(2·sqrt(a))` |
| `SinNode` | `cos(a)` |
| `CosNode` | `-sin(a)` |
| `TanNode` | `1 + tan²(a)` |
| `AsinNode` | `1/sqrt(1 - a²)` (computed as `1/sqrt((1 - a)(1 + a))`) |
| `AcosNode` | `-1/sqrt(1 - a²)`, the negative of `AsinNode`'s |
| `AtanNode` | `1/(1 + a²)` |
| `Atan2Node` | `b/(a² + b²)`, `-a/(a² + b²)` (computed as `(b/h)/h` and `-(a/h)/h`, with `h = hypot(a, b)`) |

`PowerNode`'s two exceptions are points where the formula gives 0·∞, a NaN, but the
function is flat. A NaN that is the true answer, ∂/∂b at `a < 0`, is kept. Only
`T = double` has partials: an op over another type returns `false` and is a barrier.

Three forms depart from the textbook to stay accurate. `DivideNode`'s `-(a/b)/b`
does not overflow or underflow in `b²` first. `AsinNode`'s and `AcosNode`'s
`(1 - a)(1 + a)` does not cancel next to `|a| = 1`, where the textbook partial is
1.9e-9 off at `a = 1 - 2⁻²⁷`. `Atan2Node`'s `hypot` neither overflows past about
1.3e154 nor underflows below about 1.5e-154, where `a² + b²` would make the partials
0 or ±∞ (flywheel-dag#14). The asin guard,
`AadPartials.AsinAndAcosAreAccurateNextToTheirEnds`, bites only where the compiler
does not fuse `1 - a·a` into one FMA: Apple Clang on arm64 does, which makes the
textbook form exact there, so CI's Linux leg is what guards it. Check a change to it
locally in a build configured with `-DCMAKE_CXX_FLAGS=-ffp-contract=off`. The true
edges are kept: asin′ and acos′ are ±∞ at `|a| = 1`, and atan2 has no derivative at
the origin (both partials NaN).

**Implementation note:** all sixteen ops above are `using` aliases over three
arity-generic templates — `UnaryOpNode<T,Op>`, `BinaryOpNode<T,Op>`,
`NAryOpNode<T,Op>` — parameterized by a small `Op` functor: `std::negate`/`minus`/
`divides` from `<functional>` where no identity element is needed (`NegateNode`/
`DiffNode`/`DivideNode`); a custom `PlusOp<T>`/`MultipliesOp<T>` where one is
(`SumNode`/`ProductNode`) — `std::plus`/`std::multiplies` model the combining rule
only, not the empty-input case, so they can't supply `identity()` themselves; and
`ExpOp<T>`/`LnOp<T>`/`PowOp<T>`/`SqrtOp<T>` and the seven trigonometric `Op`s
(`SinOp<T>` … `Atan2Op<T>`) wrapping `<cmath>` where no `<functional>` equivalent
exists at all. Each alias still names its own concrete type
(`SumNode<double>` ≡
`NAryOpNode<double,PlusOp<double>>`, distinct from `ProductNode<double>` ≡
`NAryOpNode<double,MultipliesOp<double>>`), so the AAD-dispatch property above is
unaffected — the derivative trait, `Derivative<Op>`, specialises per `Op` rather
than per node class name, and an application's own `Op` becomes differentiable by
specialising it.

---

## Algorithmic Differentiation (`dag_aad.hpp`)

**The graph is the tape** (flywheel-dag#10). An `aad::Tape` records the nodes some
roots depend on at the values they hold now, each with its local partials, in
topological order. `adjoints(root, wrt)` sweeps it in reverse: the root's
derivative with respect to any number of nodes, in one sweep. `tangents(seeds)`
sweeps it forward: every root's derivative in one direction, in one sweep. Nothing
is recorded while the graph evaluates, so no `eval()` path pays for it.

**Where partials come from** — `aad::IDifferentiable`, declared in `dag.hpp`: a
mixin a tape finds with `dynamic_cast`, as `discoverStatefulNodes()` finds
`IStatefulNode`, so `INode` and `NodeBase` are untouched.

| Node | Partials |
|---|---|
| `dag::ops` over `double` | Closed form, from `ops::Derivative<Op>` (see Arithmetic and Trigonometric Ops) |
| `ConditionNode` | `1` for the branch it took. It never names the condition or the other branch |
| `TweakableComputeNode` | Tweaked: none, a constant. Untweaked: `false`, a barrier |
| `aad::DifferentiableNode<N>` | Its generic functor, run once on `aad::Dual<N>` |

A node with inputs and no partials is a **barrier**: every `dag::ts` node,
`ComputeNode`, `InPlaceComputeNode`, `MemoizedComputeNode`, and any application
node that does not implement the mixin. A tape does not follow a barrier's inputs.

**The rules:**

- **A tape evaluates nothing.** Every root must be clean, or the tape throws
  `std::invalid_argument`. `partials()` pulls inputs with `eval()`, and a clean
  node's named inputs are clean too (flywheel-dag#18), so every pull returns a
  cached value. An always-dirty node is the exception, and so is the root a
  sensitivity node records right after its pull (below). Never
  make a tape evaluate a dirty root for its caller: `Engine::cycle()` snapshots its
  outputs' dirty flags before evaluating them, so a registered output evaluated
  between cycles reads as clean, and its callback misses the change. A pass runs in
  the root's output callback, or after the caller has evaluated the root.
- **Partials come from the inputs alone,** never from the node's own published
  value, which a tolerance policy can hold back.
- **A partial times an adjoint or a tangent is 0 when either is 0.** So a
  constant's ∞ or NaN partial never reaches a result, and the two sweeps agree at
  0·∞: z·√x at z = 0 and x = 0 has ∂/∂x = 0 both ways. A NaN that is not multiplied
  by 0 propagates, because it is the true answer. `Dual<N>` keeps the same rule.
- **A barrier with a `wrt` or seed node upstream throws `std::domain_error`,**
  naming both, rather than answer 0. A reverse sweep counts only the barriers its
  root reaches. A forward sweep serves every root, so it counts every barrier on
  the tape. A barrier can itself be a `wrt` node.
- **Any node can be a `wrt` or seed node.** A seed on an intermediate node adds to
  the tangent that reaches it, which keeps the sweeps exact duals: a tangent is
  Σ seed · adjoint.
- **A tape holds partials, not values,** so it goes stale when an input moves.
  Record another.
- **Eval thread only.** An application's clock-driven node, whose `dirty()` is
  always true, is re-evaluated when a differentiable consumer's `partials()` pulls
  it, as any consumer's pull does, and it can never be a root.

**Sensitivities as nodes — `aad::GradientNode` and `aad::TangentNode`**
(flywheel-dag#12, flywheel-dag#17). A pass in the root's output callback misses a
gradient that moves while the root's value stands still: x·y is 6 at (2, 3) and at
(3, 2), with gradients (3, 2) and (2, 3). A `GradientNode` holds ∂root/∂w for each w
in its `wrt` list. A `TangentNode` holds each of several roots' derivatives in the
direction its seeds give. Both values are a `std::vector<double>`, so an engine
delivers them through `addOutput`. Each recompute pulls the roots, records a tape
and sweeps it, in reverse or forward.

- **Their inputs are their roots.** A `wrt` or seed node a root depends on is
  upstream of it, and one it does not depend on has derivative 0. Pulling those
  nodes too would evaluate what the roots do not read, such as the branch a
  `ConditionNode` did not take.
- **They are `Eager`, fixed in the class.** Their values depend on the partials of
  every node the tape records, and they declare none of them. A `Lazy` node would
  skip whenever the roots' values stood still, which is the case these nodes exist
  for. `AadGradientNode.DeliversAGradientThatMovesWhileTheValueStandsStill` and
  `AadTangentNode.DeliversTangentsThatMoveWhileTheValuesStandStill` go red if their
  node is made `Lazy`.
- **Pulling a root is not the evaluation the tape rule above forbids.** It is what
  any consumer does to its input, inside the engine's cycle, so the dirty snapshot
  still covers a root that is also a registered output, registered before or after
  the node.
- **A `TangentNode` records each root right after its pull,** into one tape, through
  the private `Tape::record()`. A later root's pull can leave an earlier root dirty
  again, through an always-dirty node that reaches both. Recorded right after its
  pull, each root is recorded at the values its pull left, and a later pull
  evaluates only nodes the tape does not hold yet, an always-dirty node apart, so
  one forward sweep serves every root. Until flywheel-dag#18's fix, a stale node on a
  branch not taken did the same, and a tape recorded after every pull threw.
  `record()` is private because between two calls nothing may move the graph but
  the next root's pull. `AadTangentNode.OneTapeServesEveryRoot` goes red on a tape
  per root.
- **A sensitivity node records the root it has just pulled, clean or not,**
  through `Tape::record()`, which skips `add()`'s clean check. A root that an
  always-dirty node reaches by two paths stays dirty after its own pull, and
  requiring it clean would make the node throw on every evaluation. The tape's walk
  then evaluates what went dirty again, as it evaluates any always-dirty node it
  meets. The public `Tape` constructor still requires clean roots.
  `AadGradientNode.RecordsARootThatStaysDirty` and
  `AadTangentNode.RecordsARootThatStaysDirty` go red through `add()`.
- **A sensitivity node stays dirty when a root goes dirty again during its
  evaluation,** as every node now does (`NodeBase::endEval()`): a later root's pull,
  or the tape's pull of an always-dirty node such as an application's clock-driven
  node, can leave one so. Marked clean then, the node would miss the next change,
  which stops at the dirty root (flywheel-dag#19), and `endEval()` tells the nodes
  over it too (flywheel-dag#18). `AadGradientNode.StaysDirtyWhileItsRootIs`,
  `AadTangentNode.AnAlwaysDirtyNodeKeepsItDirty` and
  `CleanInputs.ASensitivityNodesConsumerStaysDirtyWithIt` pin it, with the
  always-dirty test node `test_nodes::AlwaysFiring`.
- **`eval()` throws what the tape throws, and leaves the node dirty,** so the next
  `eval()` retries: `std::domain_error` for a barrier with a `wrt` or seed node
  upstream, `std::invalid_argument` for a root, `wrt` or seed node on the tape that
  does not hold a `double`. The throw ends `Engine::run()`, which can then be called
  again (flywheel-dag#16), and the next cycle delivers the outputs the aborted one did
  not reach (flywheel-dag#20).
- **Cost.** Each recompute records a new tape, about 20 evaluations of the roots'
  nodes, and an `Eager` node recomputes on every change upstream of any root,
  including one its value does not depend on. Where sensitivities are wanted only
  now and then, run a pass on demand. The allocation-free pass is flywheel-dag#13.

**`DifferentiableNode<N>`'s functor must have no side effects:** it runs again, on
duals, each time a tape records the node. Its body calls math functions
unqualified, after `using std::exp;`, so that one body compiles at `double` and at
`Dual<N>`; `std::exp(x)` does not compile at `Dual<N>`. `aad::chain(x,
f(x.value), f′(x.value))` lifts any other function. At a kink, `abs′(0)` is 0, and
`min`/`max` follow the argument they return, the first one on a tie. `÷`, `exp`,
`log`, `sqrt`, `pow` and the seven trigonometric functions (`sin`, `cos`, `tan`,
`asin`, `acos`, `atan`, and `atan2` with y first) on duals take their partials from
`ops::Derivative<Op>`, so a `Dual` and an op node cannot drift apart.
`AadDual.TrigSharesTheOpsPartials` holds the trigonometric ones to the bit.

**Test the path you mean to test.** The Black–Scholes price's derivative through
d1 is 0, because S·φ(d1) = K·e^(−rT)·φ(d2), so the price alone cannot see d1's
partials: a wrong partial passed that test.
`AadNode.ItsDeltaMatchesItsClosedFormDerivatives` differentiates N(d1), which can
see them.

---

## Testing Patterns

- Create inputs → build DAG → `eval(ctx)` → verify with `get_value<T>(result)`.
- Use `EvalContext` as the eval state container (passed by reference through the graph).
- Custom nodes: prefer a `ComputeNode` lambda + captured state (see Key Patterns §Custom
  node design); subclass `INode` directly only as a last resort.
- Stateful time-series nodes: inherit `StatefulNodeBase<Derived, ...>`, implement `doEval()`.
- **Batch aggregation:** `AsyncQueue<T>` exposes `std::vector<T>` per cycle.
  Wire a downstream `ComputeNode<R, std::vector<T>>` for aggregation.
- **Deterministic tests:** drive the engine with `Engine::step()` and bounded
  cycle counts — never `sleep_for` or wall-clock timeouts.
- **Always-dirty test nodes** stand in for an application's clock-driven node, in
  `tests/test_nodes.hpp`: `AlwaysFiring` tells its consumers on every pull, and
  `Tripwire` once, on the pull a test arms it for.
- **`LatchedDebounceNode` test checklist:** onset fires exactly once after N ticks;
  `nullopt` on every subsequent tick while latched; resolved fires exactly once on first
  `false`; count resets if upstream goes `false` before N ticks; state save/restore
  mid-active produces no spurious onset.

---

## Repo Conventions

- **Banner.** Every source file starts with the project banner — copy it from any
  existing header: `flywheel-dag — A header-only C++17 reactive DAG computation engine`,
  `Copyright (c) 2026 Rob Tomlin`, MIT.
- **Plans and issues live here.** A non-trivial change gets a GitHub issue in this repo
  and a plan in `docs/plans/`, linked both ways.
- **Cite issues as `flywheel-dag#N`** or by full URL in code and docs, never as a bare
  `#N`: a reader arriving from another repo cannot tell whose issue a bare number is.
  Commit subjects keep the conventional `type(#N): …` form.
- **This repo stands alone.** Code, comments, tests, docs and commit messages describe
  the engine only. Never name or describe a downstream application — its modules,
  types, figures, issues or plans — even as an example; write a generic one instead.
- **Tests.** Every change is unit-tested, and tests are deterministic: drive the engine
  with `Engine::step()` and bounded cycle counts, never sleeps or wall-clock timeouts.
