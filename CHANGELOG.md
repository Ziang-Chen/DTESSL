# Changelog

DTESSL uses `vMilestone.MajorFeature.MinorFeature`, not compatibility-oriented
Semantic Versioning. See `docs/VERSIONING.md`.

## v0.0.1

- First independent C++20 implementation.
- Indentation-sensitive parser and static type verifier.
- Typed states, invariants, event transitions and discrete logical time.
- Same-snapshot parallel transition batches with conflict rejection; simulation
  rounds do not impose a per-transition causal order.
- Deterministic finite-set predicates and existential search.
- Pure set updates with `insert` and `erase`.
- External call plans with labelled serial/parallel DAG composition.
- Deterministic state/action rendering and single-event replay check.
- Standalone CMake build, CLI, example and tests.
- Bounded reusable C++ scratch pool for future parser/search hot paths.
