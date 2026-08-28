# Changelog

DTESSL uses `vMilestone.MajorFeature.MinorFeature`, not compatibility-oriented
Semantic Versioning. See `docs/VERSIONING.md`.

## v0.3.5

- Replaced the two ad-hoc Claim cases with one typed temporal AST and
  deterministic `ClaimMonitor` compiler for `always`, `eventually`, `until`,
  `within` and `since`.
- Added standard derived predicates `never`, `before` and `weak_until`; they
  lower to the five core operators before Solver execution.
- Added explicit `Claim name @ trace|state|procedure Target` bindings and
  optional multi-context scope declarations while retaining legacy `@ Trace`
  input compatibility.
- Added finite-trace evaluation including logical Round 0 and on-demand
  `StateExpand × ClaimMonitor` exploration with immediate, deadlock and lasso
  counterexamples. `since` uses recurrence bits and `within`/count use bounded
  monitor counters instead of retaining trace prefixes.
- Added procedure-local anonymous `(source-set)->(target-set)` automaton edges.
  They lower into the same verified Transition AST and are explored only for
  their owning procedure.
- Added transition `ensure` temporal obligations. They activate after a
  selected edge and participate in the Product automaton; they never predict
  the future to enable runtime execution.
- Added Product/monitor metrics, temporal feature negotiation, runnable finite
  trace and procedure examples, positive and counterexample tests, and fixed
  `<` comparison parsing so it is not confused with constructor type arguments.

## v0.3.4

- Added `Solver` as the high-performance semantic layer between the verified
  frontend Program and execution/exploration backends.
- Separated static source `state` declarations from dynamic `Configuration`
  nodes, whose canonical binary encoding contains active locations and typed
  variable valuations.
- Added exact content-addressed Configuration deduplication and named the
  reachable directed transition graph `StateExpand`; witness parents remain
  paths through that graph rather than pretending it is a tree.
- Added dense context/state/transition/route IDs with the prior string/map
  matcher retained as a benchmark and semantic-parity baseline.
- Added bounded Claim counterexample search: `always` finds reachable violating
  Configurations and `eventually` finds nonmatching deadlocks or lassos.
  Parameterized domains and count-monitor products report `inconclusive`.
- Added staged encoding benchmarks, four runnable Claim models, public Solver
  result/status APIs and CLI `bench` / `verify-claim` commands.

## v0.3.3

- Added semicolon-terminated, single-physical-line compact declarations for
  quick automata: `state`, `trans`, `procedure` and `trace`.
- Kept compact declarations as explicit source AST nodes, then lowered them
  directly into the existing typed state/transition/procedure/trace AST. There
  is no second parser, verifier, search loop or replay engine.
- Derived orthogonal state axes from transition-graph connectivity. In a
  compact procedure, the first listed state on each axis is initial and later
  names on that same axis are non-active model hints.
- Added inferred bool/int/rational/string state fields, comma-as-conjunction in
  compact `when`, explicitly declared `trans Name:` families, checked
  TransitionId injection, generated path identities and one-round procedure
  replay/capture.
- Added a runnable compact example plus execution, highlighting and invalid
  multiline-form tests.

## v0.3.2

- Added explicit transition optimization syntax
  `transition Name @ scope [optimized_score = expression](...)`.
- Relaxed the exactly-one-enabled-candidate rule only for transitions carrying
  an optimizer. Unannotated ambiguous transitions still reject the round.
- Scores are evaluated over each candidate's proposed after-state and must be
  exact `int` or `rational`; the greatest unique score wins.
- Equal greatest scores reject without state or RoundId commit, preserving
  deterministic replay.
- Added replay-visible optimization scope/score evidence, feature discovery,
  search-plan metadata, a runnable example and positive/tie/type-error tests.

## v0.3.1

- Split the implementation into `src/frontend.cpp` for lexing, parsing,
  typing and verification, and `src/runtime.cpp` for execution, procedure
  persistence, round semantics, replay, capture and search.
- Reduced `procedure` to exactly its initial context and initial orthogonal
  state combination. Starting a procedure enters DTESSL's runtime loop; an
  empty pending set is quiescence.
- Made `inject Transition(fields...) @ Procedure` a typed occurrence addressed
  by TransitionId. It adds work to the target procedure and never invokes an
  external search library, mutates state directly or selects a case.
- Made `transition Name(fields...)` the primary declaration form and retained
  `transition Name @ EventName(fields...)` only for the legacy open Event API.
- Added exact TransitionId/event-family dispatch indexes and an
  active-state-signature path index. Dynamic `where` evaluation now runs only
  for statically eligible indexed paths.
- Added optional replay assertion `-> Transition.case`; the backend still
  derives the selected case and rejects mismatches.
- Preserved one shared causal-DAG RoundId for all simultaneous injections,
  isolated per-procedure revisions and atomic all-procedure round commit.

## v0.3.0

- Added orthogonal typed state axes: every `@context` has exactly one initial
  state and the Engine exposes the complete active-state map.
- Added primary transition syntax `case (source-set) -> (target-set):` with
  conjunctive state sets, repeated alternative paths, atomic multi-axis commit
  and path-local `where`, `set @ context` and `do`.
- Added optional case names with stable `Transition.caseName` identities for
  path-selective dynamic capture and exact transition-count claims.
- Added finite source patterns `{A, B} @ context` and `_ @ context`; patterns
  expand to bounded executable alternatives, exact targets remain mandatory,
  and overlapping enabled paths fail as deterministic ambiguity.
- Retained legacy single-state `from/to/where/do` as v0 compatibility input.
- Added source-declared static typed EventTrace and dynamic `closed` or
  `projected` native Engine capture scoped by explicit `@context`.
- Static trace input is spelled `replay:`; one trace may declare replay followed
  by capture, while the reverse order is rejected.
- Source replay accepts `Event(...) @ Procedure` typed context injections and
  independently supports `Transition.case(...) @ Procedure` search assertions.
  Both re-enter ordinary matching; an assertion can verify but never command a
  transition path.
- Added typed procedure-local `inject` admission with static `when` topology and
  dynamic `where` predicates.
- Added public persistent `RuntimeContext`, complete initial-state
  `ProcedureArtifact`, deterministic artifact replay and derived-path search
  expectations.
- Closed capture now treats state/path/procedure filters as seeds and retains a
  conservative whole-procedure causal/data closure. Projected capture is
  explicitly non-replayable and emits no procedure artifact.
- Added procedure-aware capture filters and immutable per-RoundId procedure
  frames, including idle rounds, with exact public lookup by procedure and
  RoundId.
- Separated causal RoundId, procedure revision and occurrence identity. All
  transitions in one dynamic-DAG layer share one RoundId regardless of runtime
  traversal order.
- Added compact multi-context updates with one `set:` block and
  `field = expression @ context` assignments; `set @ context:` remains accepted
  as v0 compatibility syntax.
- Added pure, total, non-recursive typed expression `function` declarations.
- Added `procedure` as a named automaton instance containing an initial context,
  initial orthogonal state set and context-admission rules. Global transitions
  still perform every state change.
- Added `Claim` with `always`, `eventually`, transition count bounds, logical
  implication and `satisfied/violated/pending` finite-prefix semantics.
- Added `traces`, `trace` and `claims` CLI commands plus REPL trace inspection
  and claim evaluation.
- Refreshed the REPL around Golden procedure semantics: persistent `:start` and
  typed `:inject`, same-RoundId multi-procedure injection, live runtime
  inspection, closed capture and complete artifact replay with re-derived
  decisions.
- Split declared `:trace`/`:claims` execution from legacy dynamic
  `:trace-live`/`:claims-live`, preventing an unrun Engine capture from
  masquerading as an empty procedure replay.
- Added `procedure_replay.dtessl`, demonstrating isolated persistent instances,
  atomic interleaved injection, complete capture, artifact replay and claims.
- Added composite scheduler/trace example, atomic state-set tests, pattern
  ambiguity rejection, projection-gap checks and CLI conformance tests.

## v0.2.3

- Added a public language-service API with UTF-8 byte ranges, syntax classes,
  stable diagnostic codes and versioned document snapshots.
- Syntax highlighting and diagnostics now run through the production lexer,
  parser and semantic verifier instead of an editor-only grammar.
- Added ordered, non-overlapping text edits plus an in-memory document store
  suitable for a future LSP transport adapter.
- Added `dtessl highlight` and an interactive `dtessl repl` workbench with
  multi-line insertion/replacement/deletion, undo/redo, load/save, history,
  verification, highlighting and persistent logical event execution.
- REPL event execution renders a colored decision card with state deltas,
  access sets, causal predecessors and the ActionPlan dependency graph.
- Added a dependency-free terminal line editor with arrow-key history and
  cursor navigation plus standard Ctrl-A/E/U/K/W/L/C/D editing controls.
- Added language-service and CLI highlighting acceptance tests.
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
