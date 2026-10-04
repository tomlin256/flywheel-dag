# AAD Example Functions and Polynomial Comparison

**Status: Done (2026-10-04).** Both examples are standalone functions. Ops and generic-lambda results and derivatives agree at `x = 2`; the warning-as-error build and all 36 CTest tests pass.

Closes [flywheel-dag#35](https://github.com/tomlin256/flywheel-dag/issues/35).

## Goal

Make each demonstration in `examples/aad.cpp` a standalone function. Keep the existing Black–Scholes comparison and add a compact `2*x^2 + x + 10` example implemented both as an ops graph and as a generic-lambda `aad::DifferentiableNode`, then compare their values and derivatives.

## Steps

1. Extract the existing Black–Scholes body into a named function and keep `main()` as the example runner. Done when the existing Black–Scholes reverse, forward, and bumped sensitivities still agree and `example_aad` retains its success signal.
2. Add a standalone polynomial comparison function. Build one expression from `dag::ops` nodes and another with a generic lambda in `aad::DifferentiableNode<1>`. Evaluate both at the same input, compute derivatives, and report success only when both values equal the polynomial and both derivatives equal `4*x + 1`. Done when the output shows matching results and a mismatch returns nonzero.
3. Build and run the example test plus the full test suite. Done when the warning-as-error build succeeds and CTest passes, including `example_aad`.

## Tests

- Keep `examples/CMakeLists.txt`'s `example_aad` test, updating its pass expression only if the success output must change.
- Run `cmake --build build --target aad` and `ctest --test-dir build -R example_aad --output-on-failure` after implementation.
- Run `cmake -B build -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, `cmake --build build`, and `ctest --test-dir build --output-on-failure` as the final gate.

## Self-review

- Assumption: “with a generic lambda” means comparing an ops-composed polynomial against `aad::DifferentiableNode<1>` using a generic lambda.
- Risk: changing stdout could invalidate the existing CTest pass regex; preserve its agreement line or update the regex with the new stable success marker.
- Risk: the functor must remain valid for both `double` and `aad::Dual<1>`; use generic math-compatible operations and verify by building/running the example.
- Scope: only `examples/aad.cpp` and, if needed, its existing CTest declaration should change; engine behavior and APIs stay untouched.