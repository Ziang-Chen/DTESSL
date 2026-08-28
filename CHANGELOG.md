# Changelog

DTESSL uses `vMilestone.MajorFeature.MinorFeature`, not compatibility-oriented
Semantic Versioning. See `docs/VERSIONING.md`.

## v0.2.3 (integration candidate)

- Added first-class `name T` declarations and canonical `T(atom)` values,
  distinct from human strings, newtypes and authority-bearing references.
- Added additive Canonical Value Format v1 tag `0f` for nominal logical names.
- Added compact relation types `~T`/`~(A,B)`, `~{...}` literals and `~`
  membership/binding in predicates, quantifiers and deterministic selection.
- Direct unary relations bind their element, enabling `~Worker` searches over
  named record fields without tuple `.0/.1`; legacy unary `relation<T>` keeps
  its tuple binding semantics during v0 migration.
- Added `[T]`, `[]` and `[value]` option syntax plus `[]`/`[binding]` exhaustive
  match patterns. Untyped empty options are rejected.
- Made list literals explicit as `list[...]` in canonical rendering while
  retaining the previous spelling as v0 input compatibility syntax.
- Made newline/indentation layout inert inside balanced delimiters, enabling
  readable multiline relation and record literals.
- Fixed self-referential tuple/type field projection assignments exposed by
  container-overflow sanitization.
- Added positive, negative, canonical-codec and CLI replay evidence in the
  `core_logic.dtessl` scheduler model.

## v0.2.1

- Added canonical, serializable `SemanticDescriptor v1` for model projections
  actively supplied by existing systems; no implementation or log inference.
- Added descriptor-to-DTESSL generation, line source maps, descriptor/source
  SHA-256 digests and structural coverage with classified gaps.
- Added generated-operational-mirror versus independent-assurance-model
  provenance without treating either self-assertion as proof.
- Added typed action-port declarations and static call argument verification.
- Added bounded native `DTESSL EventTrace` logical replay. It never consumes
  chenRT journals, snapshots or receipts and never re-executes physical effects.
- Added descriptor check/generate/source-map/manifest/run/replay CLI paths and
  conformance fixtures.

## v0.2.0

- First-class typed `tuple<T...>` and finite `relation<T...>` values with
  canonical row ordering, duplicate elimination, arity/row limits and additive
  canonical codec tags.
- Deterministic project, equijoin, binary composition, inverse, transitive
  closure, union, intersection and difference.
- Compact `E/A` quantifiers over sets and relations with standard empty-domain
  semantics.
- `select row in relation where predicate by lex(score...)` returning a typed
  option; equal complete scores for distinct rows are rejected as ambiguous.
- Typed static search-plan summaries expose operation, determinism, ambiguity
  policy, row bound and work bound without exposing the private AST.
- `dtessl plans` renders those summaries for backend/scheduler diagnostics.
- One-million-work and 4096-row deterministic runtime budgets; failure rejects
  the candidate round without logical state commit.
- Graph-model CLI replay, full algebra assertions, no-candidate/ambiguity/budget
  adversarial tests and canonical malformed-order corpus.

## v0.1.3

- Deterministic signed arbitrary-precision `int` with bounded source/runtime
  magnitude profiles and no host floating-point dependency.
- Normalized exact `rational` values with positive denominator, gcd reduction
  and canonical zero.
- Exact `+`, `-`, `*`, `/`, negation, comparison and mixed int/rational
  promotion; division by zero rejects the round before state commit.
- Additive canonical big-integer and rational tags while retaining every
  existing int64 golden byte.
- Public exact-numeric C++ API, backend feature negotiation and CLI support for
  integers beyond int64.
- Small-domain differential arithmetic, large-number identities, byte-golden,
  malformed/non-normalized codec and end-to-end replay tests.

## v0.1.2

- Nominal `newtype`, typed `record`, payload/no-payload `variant` and `enum`
  declarations with declaration-before-use verification.
- Structural `option<T>` and `result<T,E>` values with canonical full type
  identities.
- Record, variant, enum, newtype, option and result constructors in initial
  values and transition expressions.
- Record field projection and exhaustive variant pattern matching with static
  constructor, payload-binding and arm-result checks.
- Canonical value tags for records, variants and newtypes, including sorted
  record fields and bounded recursive decoding.
- Runtime, negative-verifier, canonical-roundtrip and sanitizer coverage.

## v0.1.1

- Recursive `list<T>`, `set<T>`, `map<K,V>` and `bag<T>` type syntax and initial
  values.
- Canonical generic collection values with deterministic set/map/bag ordering.
- Generic membership, existential search, count, set update and union merge.
- Recursive canonical codec tags with depth/cardinality/byte limits.
- Nested collection, execution, merge, roundtrip and malformed-order tests.

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
