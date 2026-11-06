# Issue Standalone Fences Through `dag::threadFence()`, Which GCC's ThreadSanitizer Build Accepts

**Status: In progress (approved 2026-11-06).** Approved as written: D1, the decision the issue asked for before a plan
was written (silence the true warning in one helper), and D2, the new public name (so the next release is 1.1.0), are
both yes.

Closes [flywheel-dag#49](https://github.com/tomlin256/flywheel-dag/issues/49).

## Problem

`CycleSeqLock` (`dag_engine.hpp`; `dag_engine.inl` lines 53, 57, 81 and 83) is a seqlock built on four standalone
fences: `WriteScope` issues a release fence on entry and on exit, and `readConsistent()` an acquire fence on each side
of the read. ThreadSanitizer does not model a standalone fence, and GCC 11 and later says so, with `-Wtsan`, at each
one it compiles under `-fsanitize=thread`. The suite draws 68 of them (one per translation unit that includes
`dag_engine.hpp`), and `-Werror` makes each an error:

```text
atomic_base.h:144:26: error: 'atomic_thread_fence' is not supported with '-fsanitize=thread' [-Werror=tsan]
```

The warning is true, and here it costs nothing. The lock guards atomics (its header: it lets another thread "read
cycle-written atomics as one set"), and ThreadSanitizer never reports on an atomic access, whether the lock is right or
wrong. What holds the lock to its contract is the retry, wait-out and give-up tests in `tests/test_dag_async.cpp`, so a
clean ThreadSanitizer run says nothing about `readConsistent()`, with the fences visible to it or not. What does hurt:

- a consumer that builds with `-Werror` under GCC `-fsanitize=thread` cannot include the engine without
  `-Wno-error=tsan`, which relaxes the warning for every fence that consumer owns;
- a sanitizer job for this repo (flywheel-dag#45) cannot build with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`.

The issue's comment worked out why the fences stay, and this plan relies on it. A fence-free seqlock needs every payload
store to be a release store and the loads to be acquire, or a read-modify-write on `seq_` that a `const` lock cannot do,
and each changes what callers must do. ThreadSanitizer's `__tsan_release` and `__tsan_acquire` annotations leave
`-Wtsan` where it is and have no honest edge to annotate, because a seqlock reader publishes nothing back.

## What the probes say

The issue's probe used a stand-in file. These use the real headers: a scratch copy of `include/` with the change
applied, compile only (`-c`), at `-O0` and `-O2`, on a unit that steps an `Engine` and reads its `CycleSeqLock`, and on
a unit that is one bare `std::atomic_thread_fence`. Flags: `-std=c++23 -fsanitize=thread -Wall -Wextra -Wpedantic
-Werror` (and again with only `-Werror=tsan -Werror=pragmas` for GCC, `-Werror=unknown-warning-option
-Werror=unknown-pragmas` for Clang, which is what the test will use).

| Compiler | Engine unit, today | Engine unit, with the helper | One bare fence |
|---|---|---|---|
| GCC 13.3.0 (`ubuntu-dev`) | 4 errors at `-O0`, 5 at `-O2`, all `-Werror=tsan` at `atomic_base.h:144` | builds | 1 error, `-Werror=tsan` |
| GCC 15.2.0 (`ubuntu:26.04`) | the same, at `atomic_base.h:147` | builds | 1 error, `-Werror=tsan` |
| Clang 23.1.6 (`ubuntu-dev`) | builds | builds | builds (no `-Wtsan`) |
| Apple clang 21.0.0 | builds | builds | builds |

- **The guard is needed.** With the pragma unguarded, Clang 23 and Apple clang stop with `unknown warning group
  '-Wtsan'`. With the name misspelt, GCC stops with `unknown option after '#pragma GCC diagnostic' kind`. The narrow
  flags above catch both.
- **Object code is unchanged at `-O2`.** The disassembly of a unit that opens a `WriteScope` and calls
  `readConsistent()` is identical with and without the helper, with and without `-fsanitize=thread`: on aarch64 (GCC
  13.3.0, native; only the object's file name differs) and on x86-64 (GCC 13.3.0, an emulated container, which is CI's
  architecture; 31,646 lines plain and 45,666 under ThreadSanitizer, no difference). ThreadSanitizer sees what it sees
  today; only the diagnostic goes. At `-O0` the helper is a call of its own.
- **The order must be a template argument.** An out-of-line function that takes the order at run time compiles to a full
  fence (GCC 13.3.0, x86-64, `-O1`, `-O2` and `-Os`: `lock orq` or `mfence`, where a constant acquire fence compiles to
  nothing). A template argument is a constant inside the helper whether or not the caller inlines it. (At `-O0` even
  `std::atomic_thread_fence` itself loses the constant, so every fence is a full one there; that is not new.)
- **`-flto` is not covered.** With `-flto -fsanitize=thread -Werror=tsan`, GCC 13.3 runs the sanitizer passes at link
  time, where no pragma applies: today's headers and the helper's stop the link with the same error. Not a regression;
  such a build keeps `-Wno-error=tsan`, and the helper's comment says so.
- **Not probed:** GCC 11, 12 and 14 (no such compiler here). The pragma is honoured at an inlined call on GCC 13.3 and
  15.2. Where it is not, the new test fails there and says which unit.
- **Cost of the test:** the engine unit compiles in 2.0 s at `-O0` and 3.4 s at `-O2` under `-fsanitize=thread` (GCC
  13.3, `ubuntu-dev`); the others in under 0.1 s.

## The change

**1. `include/flywheel/dag_thread_fence.hpp`**, a header of its own that includes `<atomic>` and nothing of the
project's:

```cpp
namespace dag {

template<std::memory_order Order>
inline void threadFence() noexcept {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtsan"
#endif
    std::atomic_thread_fence(Order);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

}  // namespace dag
```

and, over it, the comment that carries the rule: what ThreadSanitizer cannot see; that the warning is true and why it
costs nothing for a fence that orders atomics only; that for a fence that orders plain data ThreadSanitizer reports races
that are not there, the warning is what points at the cause, and this helper would hide it (use a lock, or a
release/acquire operation it models); why the order is a template argument; why GCC only; and the `-flto` limit. The
header refuses to compile below C++23, like the others (`test_standard_guard` globs them).

**2. `CycleSeqLock`** calls `dag::threadFence<std::memory_order_release>()` and `<std::memory_order_acquire>()` at its
four sites, and `dag_engine.hpp` includes the header. The class comment gains the sentence the issue found missing: a
sanitizer cannot check this lock (it does not report on atomics and does not model the fences), so the deterministic
tests hold the protocol.

**3. Two tests:** `test_dag_thread_fence` (gtest, the helper's own) and `test_tsan_fences` (a compile probe and a scan,
the engine's).

**4. Docs:** `README.md`'s header table gains a row, and `CLAUDE.md`'s rule "fix a warning, never silence it" names its
second exception beside the compiler-false-positive one.

**Reusing it.** In this repo or any program that links `flywheel::dag`:

```cpp
#include <flywheel/dag_thread_fence.hpp>
dag::threadFence<std::memory_order_release>();   // in place of std::atomic_thread_fence(std::memory_order_release)
```

A project that does not depend on flywheel-dag copies the one header (it needs `<atomic>` only; drop the C++23 guard if
the project is older), and `tests/check_tsan_fences.cmake` is a template for the check that keeps a bare fence out.

## Decisions

- **D1. Silence a true warning, in one place. Needs your yes.** `CLAUDE.md` allows silencing only a confirmed compiler
  false positive. `-Wtsan` here is a true statement about ThreadSanitizer, harmless for a fence that orders atomics.
  Recommended: yes. The scope is one function and GCC only, its comment states when it is wrong to use, and a test keeps
  every other fence an error. The alternatives are in the issue: restructure to release/acquire operations (changes what
  callers must do), annotations (no honest edge, and `-Wtsan` still fires), `-Wno-tsan` or `-Wno-error=tsan` for a whole
  job (hides every use, not these four).
- **D2. A public header and a function template, not two `detail` functions. Needs your yes.** The issue sketched
  `detail::releaseFence()` and `detail::acquireFence()` inside `dag_engine.hpp`. You asked for something you can reuse, so
  the helper has a header of its own and serves every order. The cost is a new public name, so the next release is 1.1.0,
  not 1.0.3 (the release is not in this plan).
- **D3. The order is a template argument,** for the reason measured above: it stays a constant inside the helper, so the
  engine's release and acquire fences cannot turn into full fences if a compiler declines to inline it.
- **D4. The check is a compile probe and a scan, not a sanitizer run.** The property is a diagnostic, so it is a
  compile-time one. The probe needs no ThreadSanitizer runtime (it compiles, never links), runs in the ordinary `ctest` on
  every GCC and Clang host, and so is held by CI's two legs today without waiting for flywheel-dag#45. It passes only the
  flags that bear on the property (`-Werror=tsan -Werror=pragmas` on GCC; `-Werror=unknown-warning-option
  -Werror=unknown-pragmas` on Clang), not `-Wall -Wextra -Wpedantic -Werror`, so a newer compiler's unrelated warning
  cannot fail it: a contributor's build is warning-lenient by design (`FLYWHEEL_DAG_WARNINGS_AS_ERRORS` is off by default).
- **D5. The scan:** no header or `.inl` under `include/flywheel/` except `dag_thread_fence.hpp` may contain
  `atomic_thread_fence`. The probe compiles only what it instantiates; the scan covers the rest, on every compiler, so a
  bare fence added later is caught on the Mac too. A comment that needs to mention a fence writes `dag::threadFence` or
  "fence".
- **D6. The release is not part of this plan.** A consumer takes the fix with a tag, and I will do the version bump when
  you ask, as for v1.0.2.

## Steps

### Step 1 — The plan

Commit: `docs: add plan for issuing the engine's fences through dag::threadFence`. **Tests: none.** **Done when:**
the plan is committed, a comment on the issue gives its path, and D1 and D2 are answered (the approval is then recorded
in the plan, as for flywheel-dag#64).

### Step 2 — The helper and its tests

`include/flywheel/dag_thread_fence.hpp`, `tests/test_dag_thread_fence.cpp` (added to `FLYWHEEL_DAG_TESTS`), and the
README row. Nothing uses the helper yet, so the engine is untouched.

**Tests written**, in `tests/test_dag_thread_fence.cpp`:

| Test | Asserts |
|---|---|
| `ThreadFence.IsNoexceptForEveryOrder` | `noexcept(dag::threadFence<O>())` for relaxed, acquire, release, acq_rel and seq_cst, as `static_assert`s: a signature that lost `noexcept` stops compiling |
| `ThreadFence.RunsForEveryOrder` | the five calls return; the template instantiates and links for each order |
| `ThreadFence.ReleaseAndAcquireFencesPassARelaxedStoreToAnotherThread` | the pattern `CycleSeqLock` uses: a writer stores a relaxed payload, issues `threadFence<release>()`, sets a relaxed flag; the reader spins on the flag, issues `threadFence<acquire>()`, and reads the new payload, on each of 1000 rounds. Bounded by the round count; no sleep, timeout or condition variable |

**Done when:**

- the build prints no warning, and `ctest --preset relwithdebinfo-macos-arm64` passes in full (47 of 47);
- `test_standard_guard` passes with the new header (it globs `include/flywheel/`, so the guard is checked with no edit)
  and `test_install` passes (it globs the headers as well);
- the `clang_tidy_` tests pass on the Mac and in `ubuntu-dev` (one more than today: the new test file);
- in `ubuntu-dev` (GCC 13.3.0) the Linux tree builds at `--parallel 4` without a warning and `ctest` passes in full;
- hand-made changes on a scratch copy, run test by test under a watchdog: **M1** `noexcept` dropped from the helper fails
  `IsNoexceptForEveryOrder` (the build); **M2** the helper ignores its order and always issues seq_cst *survives*, because
  an ordering cannot be observed by a test. The call sites carry the order and review holds it; this is the limit of
  testing a one-line wrapper, and it is stated, not hidden.

Commit: `feat: add dag::threadFence, a standalone fence that a GCC ThreadSanitizer build accepts`.

### Step 3 — `CycleSeqLock` uses it, and the check that holds it

**Tests written first**, and run red before the engine changes: `tests/check_tsan_fences.cmake` and `test_tsan_fences`,
registered at the top level for GCC and Clang with the arguments of `test_crtp_access`
(`-DCXX -DINCLUDE_DIR -DSPDLOG_SOURCE_DIR -DNLOHMANN_JSON_SOURCE_DIR -DWORK_DIR`, plus the compiler's id). It holds:

1. **the scan** (D5): the file and the matching line of each bare fence; finding no header is an error, so a wrong path
   cannot pass by checking nothing;
2. **a precondition:** an empty unit compiles with `-fsanitize=thread -c`. If the compiler refuses the flag the compile
   cases are skipped and the status line says so (the scan still counts);
3. **the compile cases,** at `-O0` and `-O2`, with `-fsanitize=thread -c`: a unit that includes only the helper and calls
   it with every order, and an engine unit (steps an `Engine`, opens a `WriteScope`, calls `readConsistent()`). Both must
   compile;
4. **the control, on GCC:** one bare `std::atomic_thread_fence` must fail with `-Werror=tsan`. Without it a pass of 3
   proves nothing, and if GCC stops warning about fences the control fails with a message that says the pragma is then
   dead code.

Red before the change: in `ubuntu-dev` the engine unit stops with `-Werror=tsan` and the scan names the four sites; on the
Mac the scan names the four sites and the compile cases pass. Green after the engine change.

**Done when:**

- the build prints no warning; `ctest` passes in full on the Mac (48 of 48) and in `ubuntu-dev`;
- the probe test passes on Apple clang 21 (Mac), GCC 13.3 and Clang 23 (`ubuntu-dev`), and the script run by hand in a
  throwaway `ubuntu:26.04` container passes on GCC 15.2;
- **the whole suite under ThreadSanitizer,** `ubuntu-dev`, tree `tsan-linux-arm64`, GCC 13.3, configured with
  `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` and `-DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"`: the build
  prints no `-Wtsan` line (68 today) and no error, and `ctest` passes with no ThreadSanitizer report, the `CycleSeqLock`
  and `EngineCycleSeqLock` tests among them. On the Mac (Apple clang 21), `test_dag_async` built with
  `-fsanitize=thread` runs those tests with no report: the issue's open "Clang with the sanitizer running";
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt` (CI's check; the object code is identical,
  so expected);
- hand-made changes on a scratch copy, run test by test under a watchdog:

  | Change | Fails |
  |---|---|
  | M3. one of the four sites back to `std::atomic_thread_fence` | `test_tsan_fences`: the scan on every host, and the engine unit on GCC |
  | M4. the pragmas deleted from the helper | `test_tsan_fences` on GCC: the helper unit and the engine unit |
  | M5. the `!defined(__clang__)` guard deleted | `test_tsan_fences` on Clang and Apple clang: `unknown warning group '-Wtsan'`, in both units |
  | M6. `-Wtsan` misspelt in the pragma | `test_tsan_fences` on GCC: `unknown option after '#pragma GCC diagnostic'`, and `-Werror=tsan` again |
  | M7. the control's bare fence replaced with a plain statement | `test_tsan_fences` on GCC: the control compiles |

  M5 and M6 were run on the probe above, with the narrow flags, and fail as stated; the rest are run in this step.

Commit: `fix: issue CycleSeqLock's fences through dag::threadFence`.

### Step 4 — Docs, and close out

`CLAUDE.md`: the second exception to "fix a warning, never silence it". The plan's status to Done and an Outcome section.
The issue is closed with a comment that gives the commits, once CI on the Step 3 push is green. A docs commit; later runs
of docs commits are not waited for.

Commit: `docs: name dag::threadFence as the second exception to the warning rule`.

## Not in this plan

- **flywheel-dag#45,** the sanitizer job. With the engine clean it can build with `-Werror` and no exemption. This plan
  adds no job and no preset.
- **A consumer's own `-Wno-error=tsan`.** It goes when that consumer takes the release that carries this change.
- **The release.** D6.
- **`-flto` with GCC.** Covered by the helper's comment, not by code.
- **Modelling the seqlock for ThreadSanitizer,** which is the issue's second and third ways out and does not fit a lock
  whose readers publish nothing.

## Self-review — risks and assumptions

- **D1 silences a true statement.** The guards: one function, GCC only; the comment states the precondition (atomics
  only); `-Wtsan` stays an error for every other fence, which the control proves; and the scan keeps a bare fence out of
  `include/`. A reuser who points the helper at plain data hides the warning that would have led them to the cause. The
  comment says so; the compiler cannot.
- **Nothing checks the ordering,** before or after. M2 survives and is stated. The protocol is held by the deterministic
  `CycleSeqLock` tests, now said in the class comment.
- **A pragma at an inlined call is compiler behaviour, not a promise.** It holds on GCC 13.3 and 15.2 at `-O0` and
  `-O2`; GCC 11, 12 and 14 are not probed and `-flto` is known not to hold. If a release stops honouring it, the probe
  fails on that compiler and names the unit, and the fallback is the consumer's `-Wno-error=tsan`, which is today.
- **The control can go red for a good reason.** If ThreadSanitizer learns to model fences and GCC drops the warning, the
  control compiles and the test fails once, on purpose, with a message that says what to delete.
- **The scan is textual.** `atomic_thread_fence` anywhere in the headers but the helper's fails, comments included.
- **A new public name.** `dag::threadFence` is API, so the next release is 1.1.0 (D2). Nothing existing changes for a
  consumer, and nobody has to use it. A consumer with its own `threadFence` in scope and `using namespace dag` would
  see an ambiguity only if it called it unqualified with the same shape; the engine's own calls are qualified.
- **Compile cost of the test:** about 6 s of compilation in `ubuntu-dev`; measured again in Step 3.
- **The probe compiles with `-fsanitize=thread` and never links,** so it needs no runtime. A compiler that refuses the
  flag is skipped with a status line, not failed; the scan still runs.
- **Architectures.** The object code was compared on aarch64 and on x86-64 (emulated), and is identical on both at
  `-O2`. Step 3 builds and tests natively on aarch64 only (the Mac, and `ubuntu-dev`); x86-64 is CI's Ubuntu leg, which
  runs the new test on the push.
