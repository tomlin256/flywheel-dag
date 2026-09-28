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
only when not `Clean`. `Input::set()` propagates downstream.

**`InvalidationMode` — per node, set at construction, never changed:**

| Mode | Meaning | Who gets it |
|---|---|---|
| `Eager` | Recompute whenever anything upstream fired. **The default.** | Anything whose output is not a pure function of its declared inputs' *values* — every `dag::ts` stateful node (its output depends on *how often* it ran), and any functor reading state it did not declare as an input. |
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
`ConditionNode` (`Lazy` — pure selection, no functor) and `dag::ts` (`Eager`).
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

**Tweakable nodes** — `TweakableComputeNode::tweak(v)` freezes output
mid-graph. A changed tweak reaches the node's own engine output once, on the
engine's next cycle, because `tweak()` leaves the node dirty until it is
evaluated. An equal tweak only freezes, and `cached_` keeps its identity
(flywheel-dag#5). `clearTweak()` resumes: the node goes Dirty and its consumers
Maybe, so a Lazy consumer skips when the recomputed value equals the frozen one
(flywheel-dag#8).

**Custom node design** — Compose `ComputeNode` (+ captured mutable state), or a
`dag::ts::StatefulNodeBase` for incremental time-series state, or a
`dag::ops::OpNodeImpl<Derived>`-based op (see below) for a stateless arithmetic
primitive that needs its own identifiable type. If you genuinely need a node of
your own, derive from **`dag::NodeBase`**, never from `dag::INode` — `INode` is
the interface the engine calls through, not a base to build on.

**`dag::NodeBase` — the one copy of the dirty/downstream protocol.** It owns
`downstream_`, the dirty flag, and `dirty()` / `invalidate()` / `addDownstream()`
/ `notifyDownstream()`. Every node in the engine derives from it: `Input`,
`ComputeNode`, `InPlaceComputeNode`, `TweakableComputeNode`, `ConditionNode`,
`AsyncInput`, `AsyncQueue`, `MemoizedComputeNode`, `ReplayInput`, `ReplayQueue`,
`ts::NodeImpl` and `ops::OpNodeImpl` — and so should any node an application
defines.

`downstream_` is **private**. Reaching downstream goes through
`notifyDownstream()` — several of the twelve copies this replaced walked the
vector inline instead of calling their own helper, which is how one idea drifted
into four spellings of it.

Overriding `dirty()` or `propagate()` — the one override point for invalidation,
since `invalidate()` and `invalidateMaybe()` are `final` — needs a reason, and
only two are known:

| Node | Override | Why |
|---|---|---|
| `TweakableComputeNode` | `propagate()` absorbs while frozen | A tweaked value does not depend on its inputs. |
| A clock-driven node (application-defined) | `dirty()` is always `true`; `propagate()` forwards unconditionally | Its output is a function of a clock, so it is never clean, and the inherited `state_` guards in `propagate()` would swallow every invalidation after the first. |

Anything else overriding these is re-implementing the protocol rather than using
it.

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

## Basic Arithmetic Ops (`dag_ops.hpp`)

All arithmetic op primitives live in `dag::ops::` and inherit `OpNodeImpl<Derived>`
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

`T` defaults to `double`. `SumNode`/`ProductNode`/`DiffNode`/`DivideNode` constrain to
`std::is_arithmetic_v<T>`; `NegateNode` tightens to `std::is_signed_v<T>` (negating
unsigned silently wraps); `ExpNode`/`LnNode`/`PowerNode`/`SqrtNode` tighten to
`std::is_floating_point_v<T>` (`<cmath>`'s integral-promoting overload would silently
truncate an integral `T`).

**Why dedicated types, not `ComputeNode<T, ...>` + a lambda:** each op is its own
concrete C++ type so that `inputs()` plus the op's identity are enough for a future
pass (e.g. reverse-mode AAD) to attach a closed-form local derivative
(`SumNode`: all partials `1`; `ProductNode`: partial w.r.t. `x_i` is `product / x_i`;
`DiffNode`: `+1`/`-1`; `DivideNode`: `1/b`, `-a/b²`; `NegateNode`: `-1`; `ExpNode`:
`exp(a)` itself; `LnNode`: `1/a`; `PowerNode`: `b·a^(b-1)` w.r.t. `a`, `a^b·ln(a)`
w.r.t. `b`; `SqrtNode`: `1/(2·sqrt(a))`) without redesigning these primitives — a
`ComputeNode` wrapping an arbitrary lambda can't supply that, since the lambda body
is opaque to the graph. No tape or backward pass exists yet — this is forward
evaluation only.

**Implementation note:** all nine ops above are `using` aliases over three
arity-generic templates — `UnaryOpNode<T,Op>`, `BinaryOpNode<T,Op>`,
`NAryOpNode<T,Op>` — parameterized by a small `Op` functor: `std::negate`/`minus`/
`divides` from `<functional>` where no identity element is needed (`NegateNode`/
`DiffNode`/`DivideNode`); a custom `PlusOp<T>`/`MultipliesOp<T>` where one is
(`SumNode`/`ProductNode`) — `std::plus`/`std::multiplies` model the combining rule
only, not the empty-input case, so they can't supply `identity()` themselves; and
`ExpOp<T>`/`LnOp<T>`/`PowOp<T>`/`SqrtOp<T>` wrapping `<cmath>` where no `<functional>`
equivalent exists at all. Each alias still names its own concrete type
(`SumNode<double>` ≡
`NAryOpNode<double,PlusOp<double>>`, distinct from `ProductNode<double>` ≡
`NAryOpNode<double,MultipliesOp<double>>`), so the AAD-dispatch property above is
unaffected — a future derivative trait would specialize per `Op` rather than per
node class name.

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
