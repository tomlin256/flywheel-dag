# `FeedbackCycleTerminates` Must Not Leak Its Cycle

**Status: Approved (2026-11-02).** The plan was approved as written: D1 to D3 stand, so the back edge is
held weakly, the two checks live in `FeedbackCycleTerminates`, and there is no release and no `CLAUDE.md`
line.

Closes [flywheel-dag#48](https://github.com/tomlin256/flywheel-dag/issues/48).

## Problem

`DagTraversal.FeedbackCycleTerminates` closes a cycle on purpose, to show that `bfs_upstream()` visits
each node once and ends: it makes `b` with `a` as an input, then calls `a->appendInput(b)`. The fixture's
`inputs_` holds `shared_ptr`s, so `a` and `b` keep each other alive and nothing in the test breaks the
loop. The pair outlives the test, and LeakSanitizer reports it when the binary exits.

### What I measured

At `0e4e9ce` (the commit after v0.3.1), in the `ubuntu-dev` container (GCC 13.3, aarch64), tree
`build/asan-linux-arm64` (`RelWithDebInfo`, `-fsanitize=address,undefined -fno-omit-frame-pointer`),
rebuilt from the current sources:

- `test_dag_traversal` with `ASAN_OPTIONS=detect_leaks=1` exits 1 with
  `SUMMARY: AddressSanitizer: 352 byte(s) leaked in 7 allocation(s).` With `detect_leaks=0` it exits 0 and
  passes 14 of 14, so the leak is the only fault.
- The seven are the two `TestNode`s (128 bytes each), their two control blocks (24 each) and three
  16-byte vector buffers: `b`'s `inputs_` (line 119), `a`'s `inputs_` (the `appendInput()` on line 120) and
  `a`'s `downstream_`, which `wire()` fills.
- **The whole suite**, `ctest` in that tree with `detect_leaks=1`: 37 of 40 pass. The three failures are
  this leak and the two `alloc-dealloc-mismatch` aborts of
  [flywheel-dag#47](https://github.com/tomlin256/flywheel-dag/issues/47) (`test_aad_node`,
  `test_value_slot`). Those two binaries abort in their first `TestSuite::Run`, so I ran them with
  `ASAN_OPTIONS=detect_leaks=1:alloc_dealloc_mismatch=0`: both pass (7 and 21 tests) and leak nothing.
  UndefinedBehaviorSanitizer printed nothing anywhere. So this is the only leak in the suite.
- **The Mac cannot see it.** Apple Clang's AddressSanitizer here answers `detect_leaks=1` with
  `detect_leaks is not supported on this platform` and exits 134. Of the two platforms, only the container
  has a leak checker.

The leak is in the fixture, not in the engine. An engine graph builds a node after its inputs, and the
links back to a consumer (`downstream_`, filled by `wire()`) are `weak_ptr`s, so ownership runs one way.
It matters because flywheel-dag#45 asks for an AddressSanitizer job and LeakSanitizer is on by default on
Linux: that job would fail on this test the day it lands.

## The fix

Hold the back edge weakly, as `wire()` holds the links back to a consumer. Only
`tests/test_dag_traversal.cpp` changes. `TestNode::appendInput()`, which nothing else calls, becomes:

```cpp
/// Splice in an input this node does not own, to close a cycle in the inputs()
/// graph. inputs_ owns what it lists, so a back edge held there would leave the
/// two nodes keeping each other alive after the test (flywheel-dag#48); a weak
/// one is listed by inputs() for as long as its target lives, which is all a
/// walk needs.
void appendWeakInput(const NodePtr& extra) { weakInputs_.push_back(extra); }

std::vector<NodePtr> inputs() const override {
    std::vector<NodePtr> all = inputs_;
    for (const auto& weak : weakInputs_) {
        NodePtr strong = weak.lock();
        if (strong) all.push_back(std::move(strong));
    }
    return all;
}
// ...
std::vector<std::weak_ptr<INode>> weakInputs_;
```

`a->inputs()` still lists `b` while `b` lives, which is the whole test, so the walk still meets the cycle
`b → a → b`. Nothing leaks by construction, including when an assertion fails or the walk throws.

The test also gains two checks, so that the fix shows in a build with no leak checker (the Mac's, and
CI's until flywheel-dag#45 lands):

```cpp
TEST(DagTraversal, FeedbackCycleTerminates) {
    std::weak_ptr<TestNode> weakA;
    std::weak_ptr<TestNode> weakB;
    {
        auto a = TestNode::make("a");
        auto b = TestNode::make("b", {a});
        a->appendWeakInput(b);          // a now lists b as an input → cycle
        weakA = a;
        weakB = b;

        EXPECT_EQ(a->inputs(), (std::vector<NodePtr>{b}));      // check 1

        std::vector<std::string> seen;
        for (const auto& n : traversal::bfs_upstream({b})) seen.push_back(n->name());
        // ... the assertions it has today ...
    }
    EXPECT_TRUE(weakA.expired());                                // check 2
    EXPECT_TRUE(weakB.expired());
}
```

1. **Before the walk, `a->inputs()` is exactly `{b}`.** The walk's assertions (`seen == {a, b}`) hold for
   the acyclic pair too, so the test would pass if the cycle never formed, and a weak edge is one `lock()`
   from not forming it.
2. **After the block, a `weak_ptr` to each node is `expired()`.** A strong back edge leaves both alive.

## Decisions to confirm

My defaults, each one a place to edit before approving.

- **D1. A weak back edge.** The issue left three ways open, and there is a fourth:
  - *Break the cycle at the end of the test.* Needs a `clearInputs()` on the fixture, and a scope guard to
    survive an early return or a throw. Without the guard, the first `ASSERT_` added above the clean-up
    leaks again, and so does the next cyclic test whose author forgets it.
  - *`__lsan_ignore_object`.* Hides the leak instead of removing it, needs `<sanitizer/lsan_interface.h>`
    behind a compiler-specific feature test, and leaves it for Valgrind or Apple's `leaks`.
  - *A `NeighborsFn` that closes the cycle, with no node owning anything.* Leaks nothing, but the test then
    walks `BfsRange` and not `bfs_upstream()`, which is the `inputs()` path production code uses.
- **D2. The two checks live in `FeedbackCycleTerminates`**, not in a test of their own. They guard its
  graph, and a second test would build the same pair to say the same thing.
- **D3. No release and no `CLAUDE.md` line.** Nothing under `include/` changes, and the comment over the
  fixture carries the rule.

## Steps

Every commit subject is scoped to flywheel-dag#48 in the repo's `type(#N): …` form. The subjects below
omit the scope. Each step's commit goes to `main` and is pushed when the step is done; approving the plan
approves that.

### Step 0 — Approval

The plan is committed as Proposed. Nothing else lands until it is approved. A comment on flywheel-dag#48
links the plan.

**Done when:** `git status` is clean on that commit, flywheel-dag#48 carries the link, and the plan is
approved.

Commit: `docs: add plan for a cycle test that does not leak`.

### Step 1 — Start from green

No commit. On the Mac:

```bash
cmake --preset relwithdebinfo-macos-arm64 -Werror=dev -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON
cmake --build --preset relwithdebinfo-macos-arm64
ctest --preset relwithdebinfo-macos-arm64
```

and the same ctest in the container on `relwithdebinfo-linux-arm64`, never at the same time as the Mac's
build. The AddressSanitizer baseline is the one measured above.

**Done when:** the configure and the build print no warning and ctest passes in full on both (the count is
recorded; 40 at v0.3.1). Anything else: stop and ask.

### Step 2 — The fix, with its checks

`tests/test_dag_traversal.cpp` as above. The test written is `DagTraversal.FeedbackCycleTerminates`,
changed in place; no other test changes.

**Mutants.** Each is a scratch copy of the test (or of `include/`) in the scratchpad, compiled by hand
against the tree's `libgtest.a`, run alone under a 2 s alarm (the third grows without bound until the
alarm), with the repo tree untouched. The copy with no change must pass.

| Change | Must fail | How |
|---|---|---|
| The back edge held strongly again (the fixture as it is now) | the expiry checks; on Linux with `-fsanitize=address`, LeakSanitizer too | assertion |
| `inputs()` drops the weak inputs, so the cycle never forms | check 1, and nothing else | assertion |
| `BfsRange` does not dedupe (`visited_` unused in `dag_traversal.inl`) | the test, by hang | alarm (exit 142) |

**Done when:**

- the three mutants fail as stated and the unchanged copy passes;
- `test_dag_traversal` in the container's AddressSanitizer tree with `detect_leaks=1` exits 0 with no
  report (it exits 1 with 352 bytes in 7 allocations today);
- `ctest` in that tree passes 38 of 40, the two failures being `test_aad_node` and `test_value_slot` with
  flywheel-dag#47's abort and nothing else (37 of 40 today), and 40 of 40 with
  `ASAN_OPTIONS=detect_leaks=1:alloc_dealloc_mismatch=0`;
- the build prints no warning with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` on Apple Clang here and on GCC in
  the container, and ctest passes in full on both;
- CI is green on both legs on the pushed commit, the GCC 15 leg being the only evidence for warnings that
  GCC 15 alone reports (flywheel-dag#44).

Commit: `test: FeedbackCycleTerminates holds its back edge weakly, so it leaks nothing`.

### Step 3 — Close

The plan is marked done with an Outcome section. flywheel-dag#48 is closed with a summary comment, and
flywheel-dag#45 gets a comment that this blocker is cleared and flywheel-dag#47 remains.

**Done when:** the plan is committed as Done and flywheel-dag#48 is closed.

Commit: `docs: mark the plan done`.

## Not in this plan

- **The two `alloc-dealloc-mismatch` aborts** (`test_aad_node`, `test_value_slot`) are
  [flywheel-dag#47](https://github.com/tomlin256/flywheel-dag/issues/47), found by the same probe and a
  different cause: a counting `operator new`/`delete` that leaves out the nothrow overload. A green
  AddressSanitizer job needs that one as well as this.
- **The AddressSanitizer job and its presets** are
  [flywheel-dag#45](https://github.com/tomlin256/flywheel-dag/issues/45). This plan clears one finding for
  it and adds none of its machinery.

## Self-review — risks and assumptions

- **The expiry check proves this pair is freed, not that nothing leaks.** LeakSanitizer is the arbiter for
  the rest of the suite, and flywheel-dag#45 is what will run it on every push. Until then the check is the
  only thing in the plain build that sees this leak return, and on the Mac it is the only thing at all.
- **A weak edge forms the cycle only while its target lives.** In the test `b` is a local that outlives the
  walk, and check 1 asserts that the cycle is there, so the test cannot go vacuous without saying so.
- **The expiry check needs every other reference gone.** `seen` holds names, not nodes, and the
  `BfsRange` dies with its loop, inside the block. A later edit that keeps a `NodePtr` from the walk past
  the block would fail the check for a reason that is not the back edge, at that line.
- **The container is the only LeakSanitizer evidence for this plan,** as it was for the issue. Its GCC is
  13.3 and CI's is 15.2, so a GCC 15-only warning is CI's to find.
- **GCC 15 warnings.** The change adds a `weak_ptr` vector and a loop; no `std::function`, no reference
  into a temporary, no unbraced `if` over a gtest macro (the rules in `CLAUDE.md`, and flywheel-dag#44).
- **Assumptions:** no other test leaks (measured above, including the two binaries that abort);
  `appendInput()` has no caller outside this file (`grep` over the tree finds none).

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 — Approval | Done | Approved as written on 2026-11-02. |
| 1 — Start from green | Pending | |
| 2 — The fix, with its checks | Pending | |
| 3 — Close | Pending | |
