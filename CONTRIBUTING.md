# Contributing

## Ground rules

1. **The boundary is the product.** Congestion Observatory observes; it never controls. Do not add
   rate limiting, rerouting, queue programming, marking, pacing or drop policy.
2. **Utilization is not congestion.** Never let a utilization, contention, pressure or saturation
   signal alone produce a congestion assertion.
3. **Evidence or silence.** Any causal claim must cite the observations that support it. An edge,
   a candidate or a verdict without citations is a bug.
4. **Absence of evidence is not evidence.** Missing, stale, conflicting, incomplete, unsupported and
   unknown evidence are distinct states and must be reported as such.
5. **Determinism.** Decisions must be a function of data and versioned policy only: no wall clock,
   no iteration order of unordered containers, no pointer values, no floating point accumulation
   order.
6. **Bounds first.** Every new collection needs a bound in `Limits`, enforcement at the point of
   growth, and a test that drives past it.

## Building and testing

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
cmake --preset debug   && cmake --build --preset debug   && ctest --preset debug
cmake --preset asan    && cmake --build --preset asan    && ctest --preset asan
```

Warnings are errors in every configuration. The test framework has no timeouts; do not add one.

## Pull requests

* Add or update tests for every behaviour change, including the failure modes.
* Update the relevant document in `docs/` when a guarantee, a bound or a format changes.
* Keep the public headers self-contained and dependency free.
* Do not add a dependency without a discussion: the library depends only on the C++20 standard
  library and the platform socket API.
