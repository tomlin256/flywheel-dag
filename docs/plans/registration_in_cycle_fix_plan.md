# Refuse a Registration Made Inside a Cycle

**Status: Approved (2026-10-11).** `Engine::addOutput()`, `addFeedback()` and `install()` throw
`std::runtime_error` while a cycle is in flight, from an output callback or a node's `eval()`, and
register nothing. Today such a call can free the callback that is running. The check reads two counters
that `Engine` keeps already, so `Engine::cycle()` and `step()` compile to the same instructions as
v0.2.3's, and `bench_hot_path` reads within the noise of an identical build. This is the first of the
issue's three designs. The other two make a registration inside a cycle work, and "The decision" gives
the reasons this plan does not take them. The plan was approved as written, with the registration
refused rather than made to work by a deferral, a `std::deque` or a callback behind a `shared_ptr`.

Closes [flywheel-dag#24](https://github.com/tomlin256/flywheel-dag/issues/24).

## Problem

`Engine::cycle()` calls each output's callback where it is stored, as `outputs_[i].callback(val)`. A
callback that registers an output, through `addOutput()`, `addFeedback()` or `install()`, pushes onto
`outputs_`. When the push reallocates the vector, it moves the running callback's `std::function` into
the new storage and frees the old. A functor small enough for the `std::function`'s own buffer lives
inside that object, so the rest of its call reads freed memory.

On `2384c8a`, which has v0.2.3's code, the issue's program reports a heap-use-after-free under
`-fsanitize=address` with Apple Clang 21, at `-O0` and at `-O2`. Its callback holds two references and
registers eight outputs. The read is the callback's own capture, and the free is the `push_back` inside
`addOutput()`. Without the sanitizer, in a build like the tests', the same program as test 1 below dies
with SIGSEGV.

Whether a registration reads freed memory depends on the size of the callback. The typed
`addOutput<T>()` and `addFeedback()` wrap the callback in a lambda that holds a `std::function`, which is
too big for the small buffer of libc++ or libstdc++, so the functor that runs is on the heap and
survives the move. They register without error, and `cycle()`'s loop, which reads `outputs_.size()` on
every pass, visits the new output in the same cycle. A registration inside a cycle therefore either
crashes or works by accident, and the size of a callback decides which.

The rule is already written. `addOutput()`'s doc has said since v0.1.14
([flywheel-dag#23](https://github.com/tomlin256/flywheel-dag/issues/23)) to register between cycles and
never from inside one, and `CLAUDE.md` repeats it. Nothing enforces it.

## The decision

The issue names three fixes: refuse a registration inside a cycle, defer it to the end of the cycle, or
keep the entries where they do not move. The second and third make it work. The new output then fires
in the same cycle for a `std::deque`, or for a callback behind a `shared_ptr`, because the loop reads
`outputs_.size()` on every pass, and in the next cycle for a deferral. Each gives an output callback
the power to change the set of outputs while the engine runs, the consequence the issue's comment points
to. The issue asks that `bench_hot_path` time them. Each cell is the change against v0.2.3 in the same
run, as the median over 16 alignments of `Engine::cycle()` and then the lowest to the highest (see
"Prototype" for the method; the rows come from three runs, each with its own v0.2.3 build as the base):

| Design | A registration inside a cycle | What `cycle()` pays | chain | idle-queues | ingest |
|---|---|---|---|---|---|
| Refuse (this plan) | throws, registers nothing | nothing: the same instructions | -0.38% (-0.68% to +0.11%) | +0.00% (-0.25% to +2.80%) | +0.00% (-2.61% to +2.77%) |
| A control: v0.2.3's own headers, built again | the same as v0.2.3 | nothing | -0.28% (-0.71% to +0.60%) | -0.04% (-0.17% to +0.30%) | +0.62% (-2.69% to +2.78%) |
| `outputs_` in a `std::deque` | works, fires in the same cycle | `outputs_[i]` indexes two levels | +0.72% (+0.30% to +1.36%) | -0.02% (-0.21% to +10.44%) | +3.22% (+0.65% to +4.38%) |
| Each callback behind a `shared_ptr` | works, fires in the same cycle | one more load before a callback | +0.09% (-0.23% to +0.76%) | +0.32% (+0.17% to +0.55%) | +2.84% (+0.24% to +3.88%) |
| Defer to the end of the cycle | works, fires in the next cycle | a branch in the cycle's guard | +0.04% (-0.45% to +0.76%) | +0.15% (-0.38% to +0.72%) | +0.04% (-2.69% to +2.84%) |

**The plan refuses,** for these reasons:

- **It enforces the rule that is already written.** A caller inside the rule is not affected. One
  outside it gets an exception that names the call, where today it gets a crash or a registration that
  works by accident.
- **Making it work is a design, and nothing asks for the feature.** When does the new output fire: in
  the same cycle, which is an accident of the loop, or in the next, which is what "the first cycle after
  it is registered" says? What do `outputNodes()` and `discoverStatefulNodes()` return inside the cycle?
  What does `install()` do when its `wire()` registers several outputs and reads the list between them?
  How does an idle `run()` learn of an output registered in its last cycle? This repository's suite and
  examples never register inside a cycle (ctest passes with the refusal, 36 of 36), and neither does
  the benchmark (its invariants match).
- **It keeps the set of outputs fixed while `run()` goes.** `outputNodes()` and
  `discoverStatefulNodes()` read `outputs_` with no lock. During `run()` that is race-free today
  because nothing changes `outputs_` there, since registering from another thread is documented as
  unsupported. A registration inside a cycle, deferred or not, would change it mid-run, and a thread
  that calls `saveState()` while the engine runs would race it.
- **Cost does not decide it.** The `std::deque` and the `shared_ptr` read +3.2% and +2.8% on `ingest`,
  over the repository's 1% bar. The deferral reads within the noise of the control, so it is the design
  to build if the answer to "should a callback grow the graph" is yes, and this plan's check is what it
  would replace.

To add an output in response to a value, a caller records the request in the callback and registers
once the cycle has ended: after `step()` returns, or after `run()` returns, which can be called again.

## The fix

```cpp
// Engine, private. The first line of install(), of the untyped addOutput() and of addFeedback().
void refuseInCycle(const char* call) const;

inline void Engine::refuseInCycle(const char* call) const {
    if (cycles_.load(std::memory_order_relaxed) != timedCycles_)
        throw std::runtime_error(std::string("Engine::") + call + "() called during a cycle");
}
```

- **The counters already say a cycle is in flight.** `cycles_` counts a cycle as it begins and
  `timedCycles_` as its guard ends it
  ([flywheel-dag#34](https://github.com/tomlin256/flywheel-dag/issues/34)), so they differ from the
  `cycles_.fetch_add()` at the top of `cycle()` to the count in the guard's destructor, whether the cycle
  returns or throws. A callback and a node's `eval()` both run in that span. `refuseInCycle()` reads
  them and adds nothing to `cycle()`.
- **A cycle inside a cycle does not clear it.** `step()` called from a callback begins a second cycle
  (a defect of its own: [flywheel-dag#39](https://github.com/tomlin256/flywheel-dag/issues/39)). When it
  ends, the counters are still apart, because the cycle it is in has not ended. A record of the cycle's
  thread would be cleared by the nested cycle's end, and a registration after it would read freed
  memory again. The first prototype read the thread that `CycleSeqLock` keeps, and it failed on exactly
  that (test 9; see "Prototype").
- **Per engine.** The counters belong to the engine, so a callback of one engine may register an output
  on another that has no cycle in flight (test 8).
- **`install()` checks first,** before `modules_.push_back()`. A refusal then leaves the engine as it
  was: the module is neither kept nor wired. Checked later, `wire()` would run and its first
  `addOutput()` would throw, with the module already kept (test 4).
- **`addFeedback()` checks for itself,** so the message names the call the caller made, not the
  `addOutput()` underneath (test 3).
- **One exception type and one form of message.** `std::runtime_error` and
  `"Engine::addOutput() called during a cycle"`, as `run()` throws
  `std::runtime_error("Engine::run() called while already running")` for a call that cannot be made now.
- **A refusal is a throw out of a callback like any other.** The cycle ends, the outputs it did not reach
  stay due and get their value next cycle, and under `run()` the exception leaves through `run()`,
  which can be called again (test 6). A callback that wants to carry on catches it (test 7).
- **Another thread is not covered,** and nothing changes for it: a registration from a thread other
  than the one that runs the cycles, while `run()` is going, races the cycle, as the doc says. It reads
  the counters like any other call, so it may be refused when it lands inside a cycle, and nothing
  promises that.
- **Comments carry the contract.** `addOutput()`'s doc says when an output may be registered, that
  inside a cycle it throws and registers nothing, and what to do instead; `install()`'s and
  `addFeedback()`'s docs say the same for themselves; the overview's cycle paragraph gains a sentence;
  the comment over `timedCycles_` says that `refuseInCycle()` reads it too, on the thread that runs
  cycles; the comment over `refuseInCycle()` gives the mechanism of the defect and why it reads the
  counters; the coverage list at the head of `test_dag_async.cpp` gains the new section. `CLAUDE.md`'s
  rule drops the citation of this issue and says that the three calls throw inside a cycle.
- **Source-compatible.** One private member function, no data member and no signature changes, so this
  ships as v0.2.4, as v0.2.3 did. A caller that registered from inside a cycle and got away with it now
  gets an exception, which the release notes say under **Changed**.

## Tests

In `test_dag_async.cpp`, in a new section after the `EngineNewOutput` tests, with two helpers:
`stepAndCatch()`, which returns what `step()` threw as `runAndCatch()` does for `run()`, and
`WireCountingModule`, a module that counts its `wire()` calls and registers one output there. Every
test is deterministic: `Engine::step()` drives the cycle, and nothing sleeps. Test 1 is the issue's
program. Nine tests, in the suite `EngineRegistrationInCycle`. "Fails under" names the changes of
"Prototype" (the mutations) that fail it:

| # | Test | Asserts | Fails under |
|---|---|---|---|
| 1 | `ACallbackThatAddsOutputsIsRefused` | the issue's callback, two references and eight registrations, makes `step()` throw `Engine::addOutput() called during a cycle`, and registers nothing | b, e, i; a crash on v0.2.3's headers |
| 2 | `ATypedCallbackThatAddsAnOutputIsRefused` | the typed overload, which never read freed memory, is refused too | b, e, i |
| 3 | `ACallbackThatAddsAFeedbackIsRefused` | `addFeedback()` throws with its own name, registers nothing, and the input it would have set stays at 0 | d, e |
| 4 | `ACallbackThatInstallsAModuleIsRefusedBeforeItWires` | `install()` throws, `wire()` never runs, and the module's use count shows that the engine did not keep it | a, c, e |
| 5 | `ANodeThatAddsAnOutputInEvalIsRefused` | a registration from a node's `eval()` throws; the node stays dirty, and the next cycle delivers its value | b, e, i |
| 6 | `ARefusedCycleLeavesTheEngineUsable` | the output the refused cycle did not reach gets its value next cycle, and an output registered once the cycle has ended is accepted and fires | b, e, f, g, i |
| 7 | `ACallbackThatCatchesTheRefusalLetsTheCycleFinish` | the callback sees the message, the next output still fires, and nothing is registered | b, e, i |
| 8 | `AnotherEnginesOutputMayBeAddedFromACallback` | a callback of one engine registers an output on another, which fires | h |
| 9 | `ARegistrationAfterANestedStepIsStillRefused` | after a `step()` inside the callback, whatever it does, eight registrations are refused | b, e, i, j; a crash on v0.2.3's headers |

Test 8 passes on v0.2.3's headers. It pins that the refusal is per engine, and fails only under a
refusal that is not. No existing test is modified, and the 119 that exist pass untouched.

## Prototype

Built on scratch copies of the repository (`git archive` of `2384c8a`, and the same with each change),
with the flags of `build/compile_commands.json` and `-Werror`, under Apple Clang 21 on arm64. The
repository tree is untouched. The code is the sketch above, and the tests are the table.

- **Tests.** A full build of the change with `-Werror=dev -Werror=deprecated
  -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` prints no warnings, and ctest passes 36 of 36. `test_dag_async`
  passes 128 of 128: the 119 it has and the 9 new. Against `2384c8a`'s headers the 119 pass and, run one
  at a time, tests 1 and 9 die with SIGSEGV, tests 2 to 7 fail, and test 8 passes.
- **Mutations.** One change per scratch copy of `include/`, against the new test file, with a control
  copy that passes all 128:

  | | Change to the fix | Fails |
  |---|---|---|
  | a | `install()` does not refuse | 4 |
  | b | the untyped `addOutput()` does not refuse (the defect) | 1, 2, 5, 6, 7, 9 (1 and 9 crash) |
  | c | `install()` refuses after it keeps the module | 4 |
  | d | `addFeedback()` does not refuse for itself | 3 |
  | e | the check never fires | all but 8 (1 and 9 crash) |
  | f | the check fires once any cycle has begun | 6, and the four `EngineNewOutput` tests that register after a cycle |
  | g | a throw skips the count: it is bumped at the end of `cycle()`, outside the guard | 6, and the four tests of [flywheel-dag#21](https://github.com/tomlin256/flywheel-dag/issues/21) |
  | h | the check reads a depth that every engine on the thread shares | 8 |
  | i | `addOutput()` refuses after it registers | 1, 2, 5, 6, 7, 9 |
  | j | the check reads a flag that `cycle()` sets and its guard clears | 9 (a crash) |

  The four tests of flywheel-dag#21 are the three `ACycleThatThrowsFrom...IsTimed` tests and
  `StatsCoverACycleThatThrewAndOneThatDidNot`. No other test fails under any change.
- **Sanitizers.** `test_dag_async` passes under `-fsanitize=address,undefined` with no report. The issue's
  program, built with the change, ends in the refusal at `-O0` and at `-O2`, with no report. The 44 tests
  of `CycleSeqLock`, `EngineCycleSeqLock`, `EngineRegistrationInCycle`, `EngineCycleStats`, `EngineRun`,
  `EngineNewOutput` and `EngineAbortedCycle` pass under `-fsanitize=thread`. CI runs neither sanitizer.
- **The first prototype.** It asked `CycleSeqLock`, which stores the cycle's thread, whether the calling
  thread had a cycle open. It passed its own tests. A program that calls `step()` in the callback
  before it registers still reported the heap-use-after-free under AddressSanitizer: the nested cycle's
  scope clears the record when it ends, while the outer cycle runs on. Test 9 is that program. Refusing
  the nested `step()` would have closed it too, at the price of a check in `step()`, which
  flywheel-dag#39 costs at +0.88% on `ingest`; reading the counters closes it for nothing.
- **Hot path.** `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt` for every build
  here. `llvm-objdump` of `Engine::cycle()` and of its guard's destructor in the benchmark, built from
  `2384c8a`'s headers and from the change, shows the same 179 and 75 instructions. They differ in the
  function's own address, in the call stubs and in the address of one constant. `step()` is a tail
  call of `cycle()` in both. The change adds no instruction to the hot path, so the timing checks the
  build, not the code. A padding function of 0 to 15 nops ahead of the benchmark's other code moves
  `Engine::cycle()` through sixteen alignments mod 64, 4 bytes apart. A build's binaries are paired with
  the other builds' by where `cycle()` lands (read with `nm`), not by the padding, since the builds differ
  in size. 15 shuffled rounds, a core spun for 300 ms before each run, the median per binary, then the
  change per alignment against v0.2.3, then the median and range over the alignments. The control reads
  +0.62% on `ingest` for the same code, and an earlier run of the change alone read -0.23%, +0.00% and
  +0.66%, so `ingest` moves by about 0.6% between builds, and only the median means anything. The
  v0.2.3 times, in ns per cycle, are 265 (chain), 236 (idle-queues) and 120 (ingest).

## Steps

Every commit subject is scoped to flywheel-dag#24 in the repository's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — The tests and the fix

Written: tests 1 to 9 and their two helpers. Changed: `dag_engine.hpp` (the docs, the overview, the
comment over `timedCycles_`, and `refuseInCycle()`), `dag_engine.inl` (`refuseInCycle()` and its three
calls, with the comment over it) and `CLAUDE.md` (the rule).

**Done when:**

- the 9 tests fail on `2384c8a`'s headers as the table says; each mutation above fails the tests it
  lists and no others, on scratch copies of `include/`, since the repository's own tree is never
  mutated, with a control copy that passes them all;
- the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (36 of 36);
- `test_dag_async` passes under `-fsanitize=address,undefined`, the issue's program ends in the refusal
  with no report at `-O0` and `-O2`, and the 44 tests above pass under `-fsanitize=thread`;
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`, `Engine::cycle()` and
  `step()` compile to `2384c8a`'s instructions, and no row is more than 1% slower than `2384c8a` at the
  median over 16 alignments, with a control built from `2384c8a`'s headers in the same rounds.

Commit: `fix: refuse a registration made inside a cycle`.

### Step 2 — Release and close out

- Release v0.2.4 as v0.2.3 was released: the project version and the FetchContent tag in
  `CMakeLists.txt` and the README move to 0.2.4 and v0.2.4. Then the tag, and a GitHub release with
  notes. Under **Changed** they say that a registration made inside a cycle, which crashed or worked by
  accident, now throws `std::runtime_error`. There is no **Breaking** section: no signature changes.
- Mark this plan done, with the mutations and the timings rerun on the committed tree.
- Push `main`, watch CI on both legs, and close flywheel-dag#24 with a summary comment.
- Stop before the push, the tag and the release for a go-ahead.

**Done when:** the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` and ctest is
green (36 of 36), CI is green on the release commit on both legs, the installed version file reports
0.2.4, the release is published, and flywheel-dag#24 is closed.

Commits: `build: release v0.2.4` and `docs: mark the plan done`.

## Not in this plan

- **Making a registration inside a cycle work.** "The decision" has the three designs and their cost.
  The `std::deque` and the `shared_ptr` change what `cycle()` runs and read over the bar. The deferral
  reads within the noise of the control, and needs the questions in "The decision" answered: when the output fires,
  what the list shows inside the cycle, what `install()` does, and how an idle `run()` wakes. It is a
  plan of its own, and it replaces `refuseInCycle()`.
- **A nested `step()` or `run()`.** [flywheel-dag#39](https://github.com/tomlin256/flywheel-dag/issues/39),
  found while planning this: a cycle opened inside a cycle flushes every source a second time, which
  drops an `AsyncQueue`'s batch, and leaves `CycleSeqLock` out of step. It is another defect, and the
  refusal here holds through it (test 9).
- **Registering from another thread while `run()` is going.** Unchanged, and documented as unsupported
  since v0.1.14. Making it safe means a lock or a hand-off that the cycle drains, with the questions of
  the deferral, and `outputNodes()` and `discoverStatefulNodes()` guarded as well.
- **`addSource()` inside a cycle.** It reads no freed memory: it appends to `sources_` and marks the
  flush list stale, and the list is rebuilt at the start of the next cycle, never during one. Its doc
  says between cycles, and nothing here changes that.

## Self-review — risks and assumptions

- **A caller that got away with it now gets an exception.** The typed `addOutput<T>()` from a typed
  callback registers without error today, and so does `addFeedback()`, though both have been against the
  doc since v0.1.14. The exception names the call. Under `run()` it ends the run like any throw out of a
  callback, and `run()` can be called again. The release notes say so under **Changed**.
- **The counters are the check, so what they mean is a contract.** `timedCycles_` equals `cycles_`
  whenever no cycle is in flight, because `cycles_` is bumped and the guard is built on the next line, and
  nothing between can throw (flywheel-dag#34's plan). A later change that broke that would make
  `refuseInCycle()` refuse for ever or never, and mutations e, f and g show that the tests catch both.
- **A plain field is read.** The thread that runs cycles writes `timedCycles_`, and `refuseInCycle()`
  reads it on that thread, like every other call that touches `outputs_`. A call from another thread
  while a cycle runs reads it with no ordering, on top of racing `outputs_`. That call is unsupported
  already, and nothing promises that it is refused.
- **A refused `install()` has nothing to undo.** It throws before the module is kept. An exception from
  a `wire()` that fails after it registered some outputs is another matter, and it is unchanged.
- **A callback that swallows exceptions swallows this one too.** A callback with a blanket `catch (...)`
  that registers an output carries on without it, silently, as it would for any exception. Test 7 shows
  the refusal can be caught, and that is the price of an exception.
- **Tests 1 and 9 crash the test binary when the fix is absent.** They are the defect, so a regression
  fails `test_dag_async` as a whole with SIGSEGV, not one test with a message. ctest reports it.
- **GCC is unverified locally.** The prototype was built with Apple Clang alone. The change is a
  comparison and a throw, with nothing compiler-specific. CI's GCC leg builds the new tests, and neither
  GCC-only pattern `CLAUDE.md` lists, a reference into a temporary or an unbraced `if` around a gtest
  macro, appears in them.
- **The timings are one machine and one compiler,** Apple Clang 21 on arm64. The change adds no
  instruction to the hot path, which holds on any compiler that does not reorder across the new calls;
  the registration calls are not on it.
- **Assumptions:** the engine stays header-only C++17. One thread runs cycles. Outputs are registered
  between cycles. Pushing and releasing are assumed, as in the earlier plans; if either is to be done by
  hand, step 2 stops before it.
