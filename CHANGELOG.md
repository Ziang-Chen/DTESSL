# Changelog

DTESSL uses `vMilestone.MajorFeature.MinorFeature`, not compatibility-oriented
Semantic Versioning. See `docs/VERSIONING.md`.

## v0.1.0

- Canonical value format v1 for bool, int64, string and `set<string>`.
- Minimal unsigned length encoding, sorted-set enforcement and full-input
  consumption.
- Explicit decoder limits for total bytes, string bytes and set cardinality.
- Roundtrip, byte-golden and hostile-input tests.

## v0.0.3

- Typed execution/monitor/exploration/formal-export projection identifiers.
- Typed language feature discovery from verified programs.
- Backend descriptors and deterministic compatibility negotiation.
- Explicit rejection of unsupported projections and missing features without
  exposing the private parser AST.
- CLI feature inspection for VM and provider integration diagnostics.

## v0.0.2

- Canonical event-bag ordering for parallel rounds.
- Static transition read/write sets.
- Stable decision IDs and causal predecessor sets derived from prior field
  writers, without same-round artificial ordering.

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
