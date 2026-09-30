# Trigonometric Ops with Closed-Form Partials

**Status: Approved (2026-09-29).**

Closes [flywheel-dag#14](https://github.com/tomlin256/flywheel-dag/issues/14).

## Problem

`dag::ops` has nine ops: five arithmetic ones, and `exp`, `ln`, `pow` and `sqrt`. None is
trigonometric. A graph that needs sin(x) builds it from a `ComputeNode`, whose lambda is opaque, so
a tape stops there, or wraps it in an `aad::DifferentiableNode`.

Add `<cmath>`'s trigonometric functions as ops. Each is its own type, as the other ops are, and
reports its partials in closed form through `ops::Derivative<Op>`, so a tape differentiates through
it like any other op. `aad::Dual<N>` already has `sin` and `cos`, and computes their derivatives
itself. It gains the other five, and all seven take their partials from `Derivative<Op>`, as `÷`,
`exp`, `log`, `sqrt` and `pow` do. So a `Dual` and an op node agree to the bit.

No evaluation path changes, so `bench_hot_path`'s exact columns stay as committed.

**Done when** each op reports its closed-form partials, both sweeps of a tape over the ops match
analytic gradients, and the `Dual` functions agree with the ops bit for bit.

## Design

### Scope

The seven functions of `<cmath>`'s trigonometric group: sin, cos, tan, asin, acos, atan and atan2.
The hyperbolic functions are a separate group, and are not in this plan (see "Not in this plan").
Angles are in radians, as in `<cmath>`.

### The ops

| Node | Arity | Output | Op |
|---|---|---|---|
| `SinNode<T>` | unary | sin(a) | `SinOp<T>` |
| `CosNode<T>` | unary | cos(a) | `CosOp<T>` |
| `TanNode<T>` | unary | tan(a) | `TanOp<T>` |
| `AsinNode<T>` | unary | asin(a) | `AsinOp<T>` |
| `AcosNode<T>` | unary | acos(a) | `AcosOp<T>` |
| `AtanNode<T>` | unary | atan(a) | `AtanOp<T>` |
| `Atan2Node<T>` | binary | atan2(a, b): the angle of the point (b, a) | `Atan2Op<T>` |

- **Each follows `ExpNode`.** It is a `using` alias over `UnaryOpNode` or `BinaryOpNode`, with an
  `Op` that wraps its `<cmath>` function, as `ExpOp` does. `T` defaults to `double` and is
  constrained to `std::is_floating_point_v<T>`, for `ExpNode`'s reason: `<cmath>`'s integral
  overloads return `double`, which would be truncated back into an integral `T`. Every op is
  `Lazy`, from `OpNodeImpl`.
- **`Atan2Node` takes y first,** in `std::atan2`'s order: `Atan2Node<>::make(name, y, x)`. A
  swapped pair is the classic atan2 mistake, so the alias's comment says so, and a test pins it.
- **Unguarded,** as `LnNode` is. `AsinNode` and `AcosNode` propagate NaN for |a| > 1. `SinNode`,
  `CosNode` and `TanNode` propagate NaN for an infinite input. `TanNode` has no pole at a double,
  because π/2 is not one: at the double nearest it, tan is about 1.6e16.

### Their partials

`ops::Derivative<Op>` gets a specialisation for each `Op` at `double`:

| Op | Partials | Computed as |
|---|---|---|
| `SinOp` | cos a | |
| `CosOp` | −sin a | |
| `TanOp` | 1 + tan² a | |
| `AsinOp` | 1/√(1 − a²) | 1/√((1 − a)(1 + a)) |
| `AcosOp` | −1/√(1 − a²) | The negative of `AsinOp`'s |
| `AtanOp` | 1/(1 + a²) | |
| `Atan2Op` | b/(a² + b²), −a/(a² + b²) | (b/h)/h and −(a/h)/h, where h = hypot(a, b) |

Two forms depart from the textbook, for the reason `DivideNode` computes −a/b² as −(a/b)/b:

- **asin and acos: 1 − a² as (1 − a)(1 + a).** Next to |a| = 1, 1 − a² cancels: a² is rounded
  first, and the subtraction exposes the rounding. At a = 1 − 2⁻²⁷, 1 − a² reads 2⁻²⁶ where the
  exact value is 2⁻²⁶ − 2⁻⁵⁴, and the partial is 1.9e-9 too small. There 1 − a is exact, and so is
  the factored form. Apple Clang on arm64 fuses 1 − a·a into one FMA by default, which happens to be
  exact too. GCC on x86-64 has no FMA at its default `-march`, so it shows the error. The test that
  pins this therefore fails only on the Linux leg, or in a local build with `-ffp-contract=off`
  (see Step 2).
- **atan2: `hypot`, and divide twice.** a² + b² overflows once a or b passes about 1.3e154, and
  the partials read 0. It underflows once both fall below about 1.5e-154: it loses precision, and
  once it reaches 0 the partials read ±∞ or NaN. The true values are finite in both cases.
  `hypot` does neither, and b/h is cos θ, so (b/h)/h is well scaled. At (3, 4)·2⁻⁶⁰⁰ the exact
  partials are (4/25, −3/25)·2⁶⁰⁰, and the textbook form gives (+∞, −∞). At (3, 4)·2⁶⁰⁰ they are
  (4/25, −3/25)·2⁻⁶⁰⁰, and it gives (0, −0).

Two do not need it:

- **atan keeps the textbook form.** 1 + a² is at least 1, so it cannot underflow. It overflows
  past |a| ≈ 1.3e154, where the partial reads 0 and the exact value is below 5.6e-309, a subnormal.
- **tan uses 1 + tan² a,** not 1/cos² a. Both terms are non-negative, so nothing cancels. The two
  agree to an ulp, next to the pole too.

The true edges are kept, as `PowerNode` keeps its NaN ∂/∂b at a < 0:

- **asin′ and acos′ at |a| = 1 are +∞ and −∞,** the one-sided slopes, as `SqrtNode`'s partial at
  0 is +∞. For |a| > 1 they are NaN, as the value is. The sweeps' zero rule already keeps a
  constant's ∞ out of a result.
- **atan2 has no derivative at the origin,** where the angle jumps. Both partials are NaN there,
  from 0/0.

### Dual numbers

`aad::Dual<N>` gains `tan`, `asin`, `acos` and `atan`, and `atan2` in the three mixes `pow` has:
`(Dual, Dual)`, `(Dual, double)` and `(double, Dual)`. Each takes its partials from its op's
`Derivative<Op>`. `sin` and `cos`, which `Dual` already has, move to `Derivative<SinOp<double>>`
and `Derivative<CosOp<double>>`. Their forms are the same, so no value or derivative changes, but
the two can no longer drift apart.

A functor calls the new functions unqualified after `using std::atan2;` and the like, as it calls
`exp` today. A `DifferentiableNode` whose functor calls `atan2` then agrees with an `Atan2Node` bit
for bit.

### What does not change

No `eval()` path changes, and neither do `OpNodeImpl`, the arity templates or the existing ops.
`bench_hot_path` uses none of the new ops, so `--invariants` is unchanged and there is nothing to
time. `Dual`'s `sin` and `cos` give the same bits as before.

### Prototype

A scratch prototype on `cd5df17`, built with Apple Clang 21 and `-Wall -Wextra -Wpedantic -Werror`,
added the seven `Op`s, their aliases and `Derivative` specialisations, and the `Dual` functions. It
compiled without a warning, with and without `-ffp-contract=off`, and:

- At a = 0.3 and a = −0.8, each op's partials matched the textbook forms to an ulp or two.
- asin′ at 1 − 2⁻²⁷ was exact. The textbook form was 1.9e-9 too small without FMA contraction,
  and exact with it.
- atan2's partials at (3, 4)·2⁻⁶⁰⁰ and (3, 4)·2⁶⁰⁰ were exactly the values above, where the
  textbook form gave (+∞, −∞) and (0, −0). At the origin both partials were NaN.
- Both sweeps of sin(x)·cos(y) + tan(x/y), and of asin(x·y) + acos(x) + atan2(y, x)·atan(y),
  matched the analytic gradients to 1e-13. For atan2(r·sin φ, r·cos φ), which is φ, both sweeps
  gave ∂/∂φ = 1 to an ulp, and |∂/∂r| below 1e-17.
- A `DifferentiableNode<2>` whose generic lambda called all seven unqualified, including
  `atan2(v, 1.0)` and `atan2(2, u)`, compiled at `double` and at `Dual<2>`. Its adjoints matched
  central differences to 1e-8.
- `Dual`'s `asin` and `atan2` matched `Derivative<Op>` bit for bit at the points above.

The prototype was a standalone file, not the test suite, and no GCC build saw it.

### Not in this plan

- **Hyperbolic ops:** sinh, cosh and tanh, and their inverses. `Dual` has `tanh` already, and its
  derivative, 1 − tanh², loses precision for large |x|. That is
  [flywheel-dag#15](https://github.com/tomlin256/flywheel-dag/issues/15). A `TanhNode` would share
  its fix through `Derivative<TanhOp<double>>`.
- **Degrees, and a fused sincos node.** A node holds one value.

### Release

**v0.1.7.** The change adds ops and `Dual` functions, and changes nothing a consumer calls today,
as v0.1.6 added algorithmic differentiation.

## Steps

Every commit subject is scoped to flywheel-dag#14 in the repo's `type(#N): …` form. The subjects
below leave the scope out. Every checkpoint runs a full build with
`FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, including `bench_hot_path`, and a full ctest run, and both
must pass. `bench_hot_path --invariants` must match the committed file. Each step is pushed, and CI
must be green on both legs before the next step starts. No step adds a test executable, so ctest
stays at 33 tests.

Each test below that guards a specific mistake must be seen to fail before it is trusted: the step
lists the change to make by hand, then revert.

### Step 1 — The seven ops

- `dag_ops.hpp`: the seven `Op`s, beside `ExpOp`, and the seven aliases, each with a comment in
  `ExpNode`'s style. The catalogue at the top of the file gains seven lines.
- No `Derivative` specialisations yet, so the new ops are barriers until Step 2.

Tests, in `tests/test_dag_ops.cpp`:

| Test | Asserts |
|---|---|
| `DagOpsTests.TrigOpsMatchStd` | Each op equals its `std::` function at three points in its domain, to 4 ulp. Not exactly: GCC may fold a constant `std::sin(1.2)` at compile time, correctly rounded, where the node calls the library at run time |
| `DagOpsTests.TrigOpsRecomputeOnInputChange` | Each recomputes when its input moves, and `Atan2Node` when either input moves |
| `DagOpsTests.Atan2TakesYThenX` | (y, x) = (1, 1), (1, −1), (−1, −1) and (−1, 1) give π/4, 3π/4, −3π/4 and −π/4 |
| `DagOpsTests.AsinAndAcosOutsideTheirDomainAreNan` | 1.5 and −1.5 give NaN. At 1 and −1, asin gives π/2 and −π/2, and acos gives 0 and π |
| `DagOpsTests.SinCosTanOfInfinityAreNan` | +∞ and −∞ give NaN |
| `DagOpsTests.TanIsFiniteAtTheDoubleNearestHalfPi` | About 1.633e16, not ∞ |
| `DagOpsTests.AsinSinRoundTrip` | asin(sin(0.7)) is 0.7, to 1e-15 |
| `DagOpsTests.Atan2SinCosRoundTrip` | atan2(sin(2.5), cos(2.5)) is 2.5, to 1e-15: the second quadrant, which atan alone cannot reach |
| `DagOpsTests.TrigOpsWorkAtFloat` | `SinNode<float>` and `Atan2Node<float>` match `std::sin` and `std::atan2` at `float`, to 4 ulp |
| `DagOpsTests.AllOpsInputsReturnUpstreamNodes` | Extended to the seven |

| Change made by hand, then reverted | Must fail |
|---|---|
| `Atan2Op` calls `std::atan2(b, a)` | `Atan2TakesYThenX` and `TrigOpsMatchStd` |

**Done when:** ctest is green (33 of 33), `--invariants` matches, the build prints no warnings, the
hand-made change fails as stated, and CI is green on both legs.

Commit: `feat: trigonometric ops`.

### Step 2 — Their closed-form partials

- `dag_ops.hpp`: a `Derivative` specialisation for each of the seven `Op`s, each with a one-line
  comment giving its partials, as the existing ones have.
- `dag_ops.inl`: their definitions. The asin and atan2 forms each carry a comment saying why, as
  `DivideNode`'s does.
- `tests/aad_test_graphs.hpp`: three graphs built from the new ops.

Tests, in `tests/test_aad_partials.cpp`:

| Test | Asserts |
|---|---|
| `AadPartials.TrigOpsMatchTheirClosedForms` | At two points in each domain, each op reports the table's partials, to 4 ulp of the textbook forms. tan's is checked against 1/cos² a, so that the check is not the formula it checks |
| `AadPartials.TrigOpsMatchCentralDifferences` | Each op's partials are within 1e-6 relative of a central difference |
| `AadPartials.AsinAndAcosAreAccurateNextToTheirEnds` | At a = 1 − 2⁻²⁷ and at a = −(1 − 2⁻²⁷), asin′ is 2¹³/√(1 − 2⁻²⁸) to 4 ulp, and acos′ is its negative |
| `AadPartials.AsinAndAcosKeepTheirTrueInfinities` | At a = 1 and a = −1, asin′ is +∞ and acos′ is −∞. At a = 1.5, both are NaN |
| `AadPartials.Atan2NeitherOverflowsNorUnderflows` | At (3, 4)·2⁻⁶⁰⁰, (4/25, −3/25)·2⁶⁰⁰. At (3, 4)·2⁶⁰⁰, (4/25, −3/25)·2⁻⁶⁰⁰. Both to 4 ulp |
| `AadPartials.Atan2HasNoDerivativeAtTheOrigin` | At (0, 0), both partials are NaN |
| `AadPartials.OnlyDoubleOpsHavePartials` | Extended: `SinNode<float>` and `Atan2Node<float>` return `false` |

Each graph below joins `AadReverse.MatchesAnalyticGradients`, `AadReverse.MatchesCentralDifferences`,
`AadForward.MatchesAnalyticDerivatives` and `AadForward.IsTheDualOfReverse`:

| Graph | Root | Gradient |
|---|---|---|
| `TrigOfRatio` | sin(x)·cos(y) + tan(x/y) | (cos x·cos y + sec²(x/y)/y, −sin x·sin y − x·sec²(x/y)/y²) |
| `InverseTrig` | asin(x·y) + acos(x) + atan2(y, x)·atan(y) | (y/√(1 − x²y²) − 1/√(1 − x²) − y·atan(y)/(x² + y²), x/√(1 − x²y²) + x·atan(y)/(x² + y²) + atan2(y, x)/(1 + y²)) |
| `PolarRoundTrip` | atan2(r·sin φ, r·cos φ), which is φ | (0, 1) |

| Change made by hand, then reverted | Must fail |
|---|---|
| asin's partial as 1/√(1 − a²) | `AsinAndAcosAreAccurateNextToTheirEnds`, in a scratch build configured with `-DCMAKE_CXX_FLAGS=-ffp-contract=off`. The default build on this Mac fuses 1 − a·a into one FMA, which is exact, so it passes there. In CI, the Linux leg catches it |
| atan2's partials as b/(a² + b²) and −a/(a² + b²) | `Atan2NeitherOverflowsNorUnderflows` |
| atan2's two partials swapped | `TrigOpsMatchTheirClosedForms`, and the `InverseTrig` and `PolarRoundTrip` checks |

**Done when:** ctest is green (33 of 33), `--invariants` matches, the build prints no warnings, each
hand-made change fails as stated, and CI is green on both legs.

Commit: `feat: closed-form partials for the trigonometric ops`.

### Step 3 — Dual numbers

- `dag_aad.hpp` and `dag_aad.inl`: `tan`, `asin`, `acos`, `atan`, and `atan2` in its three mixes,
  each taking its partials from its op's `Derivative<Op>`. `sin` and `cos` take theirs from it too.
  The comment that names the functions which share the ops' partials names all of them.

Tests, in `tests/test_aad_dual.cpp` and `tests/test_aad_node.cpp`:

| Test | Asserts |
|---|---|
| `AadDual.FunctionsMatchTheirDerivatives` | Extended to `tan`, `asin`, `acos` and `atan`, at points inside each one's domain |
| `AadDual.Atan2CoversEachMix` | As `PowCoversEachMix`: `atan2(Dual, Dual)`, `atan2(Dual, double)` and `atan2(double, Dual)` give `std::atan2`'s value, and the partials b/(a² + b²) and −a/(a² + b²). A `double` side contributes nothing |
| `AadDual.TrigSharesTheOpsPartials` | Each of the seven gives its op's `Derivative<Op>` bit for bit: at ordinary points, at asin's 1 − 2⁻²⁷, and at atan2's (3, 4)·2⁻⁶⁰⁰ |
| `AadNode.ATrigFunctorAgreesWithItsOpGraph` | One formula over the seven functions, built as a `DifferentiableNode<2>` and as a graph of ops, gives the same value, adjoints and tangents to 1e-13. Not bit for bit: the compiler may fuse the lambda's arithmetic, which a chain of nodes cannot. The lambda calls the functions unqualified, with a `double` and an `int` on either side of `atan2` |

| Change made by hand, then reverted | Must fail |
|---|---|
| `Dual`'s `atan2` computes its own partials, as b/(a² + b²) and −a/(a² + b²) | `TrigSharesTheOpsPartials`, at 2⁻⁶⁰⁰ |
| `atan2(double, Dual)` passes its arguments on swapped | `Atan2CoversEachMix` |

**Done when:** ctest is green (33 of 33), `--invariants` matches, the build prints no warnings, each
hand-made change fails as stated, and CI is green on both legs.

Commit: `feat: trigonometric functions on dual numbers, with the ops' partials`.

### Step 4 — Docs and release

- `README.md`: the Features entry becomes "Arithmetic and trigonometric op nodes", and the headers
  table's `dag_ops.hpp` row gains the seven.
- `CLAUDE.md`: the ops section's heading becomes "Arithmetic and Trigonometric Ops". Its node
  table, its constraint paragraph and its partials table gain the seven, with the asin, acos and
  atan2 forms and why. "All nine ops" becomes sixteen. In the algorithmic differentiation section,
  the list of `Dual` functions that take their partials from `Derivative<Op>` gains the seven.
- Release v0.1.7 as v0.1.6 was released: the project version and the FetchContent snippets in
  `CMakeLists.txt` and the README, the tag, and a GitHub release with notes.
- Mark this plan done, and close flywheel-dag#14 with a summary comment.

**Done when:** ctest is green (33 of 33), CI is green on the release commit, the release is
published, and flywheel-dag#14 is closed.

Commits: `docs: describe the trigonometric ops`, `build: release v0.1.7` and
`docs: mark the plan done`.

## Self-review — risks and assumptions

- **Math libraries differ.** `std::sin` and the rest are not correctly rounded. glibc and Apple's
  library can differ in the last bit, and GCC folds a constant call at compile time, correctly
  rounded. So the tests compare with `std::` values to 4 ulp. Only `TrigSharesTheOpsPartials`
  asserts bits, and it compares one inline function with itself.
- **The asin guard bites only without FMA.** On this Mac, the default build makes the textbook form
  exact. So the local check needs `-ffp-contract=off`, and in CI the Linux leg is the one that
  guards it.
- **Bit-for-bit agreement depends on contraction too.** `TrigSharesTheOpsPartials` expects one
  inline function to round the same way at two call sites. Clang decides contraction per
  expression, before inlining, and x86-64 GCC has no FMA at its default `-march`, so both CI legs
  hold. A GCC build for an FMA target could fuse the two sites differently, and fail by an ulp. That
  would be a fragile test, not a wrong answer.
- **atan2's argument order** is the classic mistake. The alias's comment says y comes first, and
  `Atan2TakesYThenX` pins it.
- **An infinite input to atan2** makes one partial NaN, from ∞/∞, where its limit is 0. The textbook
  form gives NaN there too, and no op treats an infinite input specially.
- **New overloads in `dag::aad`.** An application that declared its own `tan`, `asin`, `acos`,
  `atan` or `atan2` for `Dual<N>` would find its calls ambiguous. Nothing else a consumer calls
  changes.
- **GCC is unverified.** The prototype was built with Apple Clang alone. CI's GCC 13 leg sees the
  code first at Step 1. flywheel-dag#7 moves CI to Ubuntu 26 on 2026-10-19, which may land during
  this plan and bring a newer GCC.
- **Assumptions:** the engine stays header-only C++17, so the new code goes in `.hpp` and `.inl`
  files. `std::hypot` has been in `<cmath>` since C++11, and both libraries' `hypot` are accurate
  to about an ulp: Apple's gave the exact values at the prototype's points. CI stays on
  `ubuntu-latest` and `macos-latest`.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — The seven ops | Done | ctest 33 / 33, and `--invariants` is unchanged. The build prints no warnings with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`. `test_dag_ops` has 54 tests: the 45 it had, nine new, and `AllOpsInputsReturnUpstreamNodes` extended to the seven. Swapping `Atan2Op`'s arguments broke `Atan2TakesYThenX` and `TrigOpsMatchStd`, as planned, and also `TrigOpsRecomputeOnInputChange`, `Atan2SinCosRoundTrip` and `TrigOpsWorkAtFloat`. CI run 36626502820 is green on both legs, at 33 / 33 with no compiler warnings |
| 2 — Their closed-form partials | Done | ctest 33 / 33, and `--invariants` is unchanged. The build prints no warnings. `test_aad_partials` has 19 tests: the 13 it had, six new, and `OnlyDoubleOpsHavePartials` extended. `TrigOfRatio`, `InverseTrig` and `PolarRoundTrip` joined the four gradient checks. The three hand-made changes each failed as stated. With asin's partial as 1/√(1 − a²), the default build on this Mac passed, and a build with `-ffp-contract=off` failed `AsinAndAcosAreAccurateNextToTheirEnds`: 8192 against 8192.0000152587891. The textbook atan2 form failed `Atan2NeitherOverflowsNorUnderflows` alone. Swapping atan2's partials failed `TrigOpsMatchTheirClosedForms`, `TrigOpsMatchCentralDifferences`, `Atan2NeitherOverflowsNorUnderflows` and the reverse and forward gradient checks, but not `IsTheDualOfReverse`: both sweeps read the same wrong partials. One restore landed in the same second as the broken build, and GNU Make 3.81 compares whole seconds, so `test_aad_forward` stayed stale until the full checkpoint caught it. A restored file is now touched before the rebuild. CI run 36627751774 is green on both legs, at 33 / 33 with no compiler warnings: the Linux leg ran the asin guard live |
| 3 — Dual numbers | Done | ctest 33 / 33, and `--invariants` is unchanged. The build prints no warnings. `test_aad_dual` has 11 tests: the 9 it had, two new, and `FunctionsMatchTheirDerivatives` extended to `tan`, `asin`, `acos` and `atan`. `test_aad_node` has 7: the 6 it had and `ATrigFunctorAgreesWithItsOpGraph`. The two hand-made changes each failed as stated. `Dual`'s `atan2` with its own textbook partials failed `TrigSharesTheOpsPartials` alone: at ordinary points the two forms agree within the node test's tolerance. Swapping the arguments of `atan2(double, Dual)` failed `Atan2CoversEachMix`, and also `ATrigFunctorAgreesWithItsOpGraph`, whose functor calls `atan2(2, x)`. The first run of that check read a stale `test_aad_node`, for the same whole-second reason as in Step 2, so each break now deletes the objects it affects before it builds |
| 4 — Docs and release | Not started | |
