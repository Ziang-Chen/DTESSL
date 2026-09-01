# DTESSL Project QC Control

Updated: 2026-09-01, Asia/Shanghai

## Control metadata

- Owner: Ziang-Chen
- Target repository: `Ziang-Chen/DTESSL`
- Integration repository: `Ziang-Chen/chenVirtualMachine`,
  `external/DTESSL` submodule (explicitly outside this integration slice)
- Current target: validate the `v0.4.2` typed/anonymous relation, lazy
  comprehension and structural compact-pattern slice
- Stop orders: none

## Current state

**Baseline complete.** The `v0.3.2` implementation permits multiple enabled candidates
only under an explicit exact-numeric after-state optimizer, selects the unique
greatest score, rejects ties atomically, and retains the exactly-one rule for
unannotated transitions. Standard, `-Werror` and ASan/UBSan suites pass 25/25;
implementation commit `83eb39e` is pushed on
`codex/optimized-transition-v0-3-2`; this closeout records that exact identity.

The local merge also brings in direct REPL access to `RuntimeContext`: procedure
startup, typed injection, runtime inspection, closed capture and artifact
replay. Its gates will be rerun against the merged TransitionInput semantics.

The `v0.3.3` compact AST candidate is now implemented on that merged baseline.
Standard, `-Werror`, and ASan/UBSan builds each pass 31/31 tests,
including compact check/trace integration, production highlighting and
same-line rejection.

The `v0.3.4` candidate separates static state declarations from canonical
dynamic Embeddings, gives the reachable transition graph the stable name
`EmbeddingExpand`, adds dense-ID/reference parity benchmarks and searches bounded
safety/eventuality counterexamples through the independent Solver layer.
Standard, `-Werror`, and ASan/UBSan builds each pass 35/35 tests.

The `v0.3.5` candidate adds one temporal AST and ClaimMonitor compiler shared by
finite Trace evaluation, procedure EmbeddingExpand Product search and transition
obligations. It also adds explicit Claim targets, cross-context scopes and
procedure-local anonymous automaton edges. Standard, `-Werror`, and
ASan/UBSan builds each pass 38/38 tests.

The `v0.4.0` candidate preserves recursive StateSchema nodes through parsing
and validation, lowers stable semantic paths into Embedding control/value slots,
maps those paths to raw vector offsets with `RawKeyMap`, and recursively lowers
`subject ~ R1, (R2 | R3)`. General and compact examples, ancestor rejection,
false relation constraints and exact-raw node identity are covered. Standard,
`-Werror`, and ASan/UBSan builds each pass 42/42 tests.

The occurrence-owned correction removes procedure as a closure boundary,
adds active-state-axis causality, `eventually` capture intervals and generic
typed `TraceArtifact` replay for free and procedure-labelled occurrences.
The REPL now exposes exact free `:step`, occurrence/causal/temporal trace detail
and generic `:replay-artifact` through those same public runtime APIs. Standard,
`-Werror`, and ASan/UBSan builds each pass 45/45 tests; real PTY history and the
declared/live occurrence demo pass.

The state-space refinement adds named/anonymous recursive `case` axes without
inferring transition edges, enumerable `typetrait`/enumeration/range static
constraints, finite parameter generation in Solver, and authenticated
`EmbeddingDelta` witness edges. General/compact syntax, nested axis rewrites,
domain rejection and finite Claim closure are covered. Standard, `-Werror`, and
ASan/UBSan builds each pass 47/47 tests.

The `v0.4.2` slice adds typed rule relations, lazy independent-branch set
union, explicit Cartesian tuple projection, canonical `<...>` products,
unordered `{...}` anonymous relation patterns and compact structural
Transition updates. The frontend now has separate private AST and relation
semantic layers. Standard, `-Werror`, and ASan/UBSan builds each pass 51/51
tests, including branch-permutation and parenthesized-filter checks.

## Current goal contract

- North star: see `GOAL_CONTRACT.md`.
- Allowed surface: this DTESSL repository's parser, verifier, simulator,
  language service, CLI/REPL, examples, tests and documentation.
- Non-goals: tags, ChenVM/chenRT, other repositories or task threads,
  LSP transport, ambient authority and runtime nondeterminism.
- Current exit gate: pass parser/verifier/runtime and CLI trace/claim tests plus
  standard/`-Werror`/ASan+UBSan, then commit, push and leave the worktree clean.

## Evidence matrix

| Requirement | Verification evidence | Validation evidence | Status |
| --- | --- | --- | --- |
| Independent C++ build | Standalone CMake build passes | Built from standalone clone layout | pass |
| Typed state/transition loop | Unit test covers parse, verify and two serial steps | Scheduler example produces inspectable state | pass |
| Parallel transition semantics | Same-snapshot disjoint merge and conflict rejection tests | No multi-component user model yet | candidate |
| Canonical event bags | Permutation-equivalence unit test and batch CLI integration | Scheduler batch exercised | pass |
| Typed merge relation | Same-round set-union and default-conflict tests | Grow-only set scenario exercised | pass |
| Causal predecessor trace | Prior-writer and merged-multiwriter assertions | Output is inspectable; full trace file absent | candidate |
| Backend feature discovery | Program-derived feature set and CLI inspection tests | Example program inspected | pass |
| Backend rejection | Missing feature and unsupported projection tests | Real VM adapter absent | candidate |
| Canonical value codec | Roundtrip/re-encode, golden bytes and malformed-input tests | Backend consumption not yet implemented | candidate |
| Generic finite collections | Nested source values, generic set execution/merge and codec roundtrips | One scheduler fixture; no large search benchmark | candidate |
| Algebraic/nominal values | Record/variant/enum/newtype/option/result execution and codec roundtrips | Algebraic CLI replay fixture passes | pass |
| Exhaustive matching | Positive option/variant payload matches plus non-exhaustive rejection | Both branches exercised over consecutive events | pass |
| Exact numeric profile | Big-int identities, signed small-domain differential test, normalized rational arithmetic and golden bytes | Exact numeric CLI replay passes | pass |
| Deterministic action DAG | Dependency assertions and replay equality | CLI output shows serial fan-out | pass |
| Relation predicate baseline | `exists`, membership and set updates tested | Unknown worker disables transition | pass |
| Canonical n-ary relation | Tuple/relation roundtrip, golden bytes, sort/dedup and malformed-order rejection | Graph fixture renders canonical rows | pass |
| Relation algebra | Project/join/compose/inverse/closure/union/intersection/difference assertions | Graph CLI replay exercises join and closure | pass |
| Deterministic search | E/A empty/non-empty semantics, typed select, ambiguity/no-candidate tests | Graph chooses `b` only with explicit unique lex score | pass |
| Search budgets/plans | Static plan metadata and million-work rollback test | Budget failure leaves round zero | pass |
| No ambient effects | Engine only returns `ActionPlan` | Host is not invoked by CLI | pass |
| Runtime memory safety | ASan/UBSan baseline plus bounded scratch-pool unit test | Pool not yet used by parser/search hot paths | candidate |
| Version identity | CMake/generated header target `0.4.2` | Built CLI reports `v0.4.2` | pass |
| Solver semantic seam | Public Solver owns Embedding expansion and Claim search; runtime keeps procedure/replay execution | CLI emits verified/counterexample/inconclusive status without performing ActionPlans | pass |
| EmbeddingExpand graph | Canonical Embedding codec/store plus explicit directed adjacency; witness parent is not graph topology | Safety path and eventuality lasso fixtures emit reproducible Embedding digests | pass |
| Compact automaton AST | Single-line parsing, direct typed-AST lowering, deterministic trace and multiline rejection tests | `examples/compact_automaton.dtessl` selects `ctodo.a_to_b` | pass |
| Logical names | Distinct public Value kind, nominal type check, canonical codec roundtrip and invalid-atom rejection | Scheduler renders `WorkerId(a)` without string quotes | pass |
| Recursive relation expression | Tuple/name/value subjects plus explicit `and`/`or` parser/type/evaluator tests; false conjunction rejects initial embedding | `<a,b> ~ R1 and (<a,b> ~ R2 or <a,b> ~ R3)` runs in the recursive scheduler fixture | pass |
| Typed relation rules | General and compact declarations share typed parameters, direct membership and bounded lazy enumeration | `typed_relations.dtessl` composes declared relations through closure/union | pass |
| Anonymous relation containers | `<...>` preserves product order; `{...}` canonicalizes unordered union branches; nested compact Transition patterns retain structural AST | Independent set union, explicit Cartesian product, grouped filters and nested Scatter transition run | pass |
| Unified RelationMatch path | Equality/order/membership and recursive `~` lower to one typed AST/evaluator; runtime and ClaimMonitor atoms call it without private predicate evaluation | Existing comparison corpus plus nested tuple relation and canonical-render tests pass | pass |
| Trace relation syntax | `(a,b) ~ happens_before` has a distinct trace-domain AST case; compatibility `before(a,b)` shares it; finite nesting executes and unsupported Solver nesting is inconclusive | Procedure Product verifies strict ordering; closed trace satisfies nested `eventually(RelationMatch)` | pass |
| Recursive StateSchema | General/compact recursive declaration, duplicate/path/depth checks, local typed fields/invariants and structural transition matching | Nested scheduler and switch examples execute and Claim search reaches three Embeddings | pass |
| State case axes | Named/anonymous cases lower to recursive Choice nodes; declaration order creates no edge and transitions rewrite only explicit axes | `state_case_finite.dtessl` changes Phase then one nested Step slot | pass |
| Finite static constraints | `typetrait<T>`, `T{...}`, and inclusive integer ranges share declaration/runtime admission and bounded Solver generation | Invalid stride member rejects; finite Retry Claim closes over 9 Embeddings and 32 edges | pass |
| Authenticated Embedding delta | Runtime decisions and Solver witnesses carry ordered control/value patches plus parent/child digests; insertion reapplies and checks each patch | Nested axis delta contains exactly one control path and reconstructs the target Embedding | pass |
| RawKeyMap lowering | Stable control/value semantic paths map to raw embedding-vector offsets; exact raw content owns graph identity | Solver API exposes Phase/Stage/value offsets and digest remains evidence-only | pass |
| Bracket options | `[T]`, `[]`, `[value]`, empty-context rejection and exhaustive option-pattern test | Choose/reset two-step model exercises present/absent states | pass |
| Explicit list literals | `list[...]` parse and canonical rendering | Scheduler fixture contains typed logical-name list | pass |
| Full relation/search design | Profile plus implementation and adversarial tests | Standard, `-Werror` and ASan/UBSan suites pass 17/17; installed graph replay/plans pass | pass |
| Explicit model projection | Canonical descriptor parse/print, origin digest, provenance and classified-gap checks | Scheduler projection generates checked DTESSL | pass |
| Generated source evidence | Stable descriptor/source SHA-256 and complete nonblank-line source map | CLI check/source-map/manifest exercised | pass |
| Typed action ports | Declared port lookup plus exact argument-type verification | Three-port serial/parallel ActionPlan fixture passes; mismatch rejects | pass |
| Lifecycle mappings | Unique field/phase/literal checks and generated literal typecheck | string lifecycle fixture passes; int literal rejects | pass |
| Native EventTrace replay | Bounded typed batches, empty-batch rejection and fresh-engine equality | Two-round logical state/action replay passes | pass |
| Runtime-log exclusion | No log, receipt, snapshot or journal types/adapters in public API | Descriptor contains explicit external-runtime gap | pass |
| Shared language pipeline | `analyze_source` runs the production lexer/parser/verifier; valid core fixture asserts `name`, `~`, option and `list` spans | CLI highlight and REPL check agree with `dtessl check` fixtures | pass |
| Editor document model | Monotonic versions, validated UTF-8 byte ranges and non-overlapping snapshot edits | Replace and stale-version tests pass | pass |
| Located diagnostics | Lex/parse/semantic categories retain source ranges and stable codes | Parser and wrong-type assignment locations asserted | pass |
| REPL workbench | Multi-line insert/replace/delete, undo/redo, load/save and both Engine/RuntimeContext invalidation implemented; exact `:step` and `:replay-artifact` call production APIs | Real PTY passes history recall, free occurrence decisions, pending/witnessed intervals, excluded post-witness occurrence and generic replay | pass |
| Multi-state transition model | Conjunctive source/target sets, alternative case paths, atomic invariant gate and ambiguity tests | Composite scheduler example and CLI trace pass | pass |
| Native static/dynamic trace | Source EventTrace plus closed/projected Engine capture scoped by `@context` | Static run and projected capture tests pass | pass |
| Temporal Claim AST | always/eventually/until/within/since plus derived never/before/weak_until share typed predicate leaves | Finite trace and procedure examples exercise all primitives | pass |
| Unified Property disposition | `where`, invariant, ensure and Claim share typed evaluation/monitor compilation; false maps explicitly to disable/reject/violation/counterexample | Existing guard selection, invariant rollback, ensure violation and Claim counterexample suites pass | pass |
| ClaimMonitor Product | Embedding and monitor state have separate keys; deadlock/lasso/finite counterexamples are reconstructed | Positive and negative procedure claims plus transition obligation cycle tests pass | pass |
| Product snapshot ownership | Product nodes reference one Engine snapshot per unique base Embedding; monitor histories do not copy Engine | `StartedSinceDone` reaches more Product nodes than Embeddings while `snapshots == embeddings` | pass |
| Finite-prefix claims | logical Round 0, temporal evaluation, count plus three statuses | Closed temporal trace CLI and projection-gap tests pass | pass |
| Pure helper functions | Typed parameter/result checking, state/round isolation and recursion rejection | Composite guard calls a verified helper | pass |
| Procedure entry boundary | Parser accepts initial context/state plus anonymous local automaton edges; ordered steps/admission scripts remain forbidden | Solver expands local edges only for the owning procedure | pass |
| Procedure RuntimeContext | Two procedure instances retain isolated typed state across an interleaved replay; same-layer decisions share RoundId and same-procedure dependencies retain qualified predecessor IDs | Idle frames plus REPL multi-injection/runtime inspection remain visible | pass |
| Round input evidence | Every decision directly owns value-copied Event/Transition admission identity, typed fields, procedure/context and before/after state; procedure frames repeat their per-round inputs | Open Event, exact TransitionId, interleaved closed capture, replay equality and exact RoundId lookup tests pass | pass |
| Capture filter | Typed state/transition/procedure selector sets validate references and procedure capture emits immutable per-RoundId frames | Composite example captures one procedure and dual-procedure test retains both histories | pass |
| Distributed capture relation | Postfix `[]` extensions aggregate typed State/Transition/Procedure seeds by Trace/Session; state entry+exit and family cases use OR matching, and only explicit closed templates emit replayable closure | Parser, Engine projected capture and RuntimeContext closed-closure regression tests pass | pass |
| Golden procedure replay | Replay consumes declared initial embedding plus typed transition-occurrence history; cases are recomputed | Public API and REPL replay report matching rounds, decisions and procedure histories | pass |
| Search replay | A named transition path may be asserted only as derived evidence, never forced | `Transition.case(...) @ Procedure` and explicit `SearchExpectation` both re-enter ordinary search | pass |
| Transition occurrence injection | Exact TransitionId plus typed fields are appended to a started procedure; no direct state mutation or case selection | Same Event family is ambiguous through legacy dispatch but exact TransitionId injection selects only its own family | pass |
| Indexed transition search | Exact TransitionId/event index followed by active-state-signature path index; only indexed candidates evaluate `where` | Index-plan evidence and shared-Event ambiguity/bypass test pass | pass |
| Explicit optimized choice | Typed exact after-state score, greatest unique winner, tie rollback and unchanged unannotated ambiguity rule | Runnable `optimized_transition.dtessl`, feature/plan/output evidence and positive/tie/type-error tests; all three suites pass 25/25 | pass |
| Semantic source boundaries | Volatile parsing remains in `frontend.cpp`; instantaneous, trace and monitor semantics, runtime orchestration, Solver exploration and backend negotiation have explicit files | One public parser and one semantic route remain; standard, warning-clean and sanitizer suites pass | pass |
| Occurrence/temporal capture closure | State/transition/procedure seeds select actual occurrences; closed capture follows explicit predecessor IDs and optional eventually intervals without requiring procedure ownership | CLI plus REPL show a transition seed excluding the later `After` edge; free Engine capture retains anchor-through-witness and generic artifact replay matches | pass |
| Physical receipt isolation | Public API contains no receipt/journal ingestion | Physical completion remains outside DTESSL | pass |
| Four AST projections | Architecture specified | Implementations absent | missing |

## Gaps and blockers

- P0 gap: source AST is currently internal and has no canonical serialized form.
- P0 gap: backend/provider contracts are designed but wait on canonical AST.
- P1 gap: no external user dogfood model beyond the bundled fixtures.

- No blocker remains in the v0.3.2 implementation. Canonical AST serialization remains a
  later P0 milestone dependency, not a blocker for this private-AST split.
## Risks

- Scope: implementing all domain dialects inside the core would bloat the
  language. The P0/P1/P2 split is mandatory.
- Verification: frontend and runtime intentionally share one private translation
  unit while the typed AST remains internal; a future canonical AST can make
  them independently compiled without exposing a second IR.
- Validation: syntax remains candidate until exercised on at least one
  non-trivial user model.
- Release: DTESSL tags and ChenVM submodule updates must remain ordered.

## Next corrective focus

1. Resume typed derived data and explicit shared inputs as the next slice.
2. Preserve the permanent native Program/EventTrace-only validation boundary.
3. Keep external projection producers separate from DTESSL verification.
4. Reject any claim that the full language is complete until all roadmap gates
   have evidence.

## Decision log

- 2026-08-28: DTESSL split into an independent repository; ChenVM consumes it as
  a pinned external submodule.
- 2026-08-28: version interpretation fixed as
  `vMilestone.MajorFeature.MinorFeature`; first baseline is `v0.0.1`.
- 2026-08-28: complete design partitioned into core, standard dialect and
  external integration layers.
- 2026-08-28: `v0.0.1` released; per-transition tick replaced by same-snapshot
  parallel rounds before stabilizing the next slice.
- 2026-08-28: `v0.0.2` released with canonical event bags, typed merge and
  causal predecessor evidence.
- 2026-08-28: `v0.0.3` released with typed feature/projection negotiation and no
  private AST exposure.
- 2026-08-28: `v0.1.0` released Canonical Value Format v1 for all foundation
  values with hostile-input limits.
- 2026-08-28: `v0.1.1` released recursive generic collections.
- 2026-08-28: `v0.1.2` released algebraic/nominal values, exhaustive matching
  and additive Canonical Value Format v1 tags; standard and ASan/UBSan suites
  pass with four tests each.
- 2026-08-28: `v0.1.3` released arbitrary exact integers, normalized rationals,
  frozen numeric/canonical profiles and deterministic budget failures; standard,
  `-Werror` and ASan/UBSan suites pass with five tests each.
- 2026-08-28: `v0.2.0` released first-class finite relations, algebra, compact
  quantifiers, explicit lex selection and bounded plan metadata; standard,
  `-Werror` and ASan/UBSan suites pass with seven tests and installed CLI replay
  and plan inspection pass.
- 2026-08-28: fixed the permanent boundary that DTESSL consumes only native
  Program/typed EventTrace and emits deterministic logical result/ActionPlan;
  existing systems must actively provide an explicit projection artifact.
- 2026-08-28: `v0.2.1` released SemanticDescriptor verification, canonical
  generation/evidence, typed ports and native multi-round replay, with no
  runtime-log, receipt, snapshot or journal adapter; standard, `-Werror` and
  ASan/UBSan suites pass 10/10 and installed CLI evidence passes.
- 2026-08-28: `v0.2.3` integrates the reusable language-service/document and
  REPL/editor/highlighting surface with logical names, compact relations,
  bracket options and explicit lists. LSP transport, symbol indexing and
  incremental parsing remain later tooling gates, not parallel parsers.
- 2026-08-28: merged standard, `-Werror` and ASan/UBSan suites pass 17/17;
  installed check/replay, non-interactive check/highlight/run/replay, legacy
  compatibility and real PTY history/cursor/source-edit/fancy-run gates pass.
- 2026-08-28: `v0.3.0` candidate replaces the redundant `when` wrapper with
  repeated path-local `case (source-set)->(target-set):`, adds exact/set/wildcard
  topology matching, atomic orthogonal state commit, native trace scopes and
  three-valued finite-prefix claims.
- 2026-08-28: fixed procedure as an entry embedding only: initial context
  plus initial state combination. It is neither a trace nor a transition
  container; only explicit Engine startup applies it.
- 2026-08-28: replay occurrences now explicitly bind
  `Transition.case(...) @ Procedure`; the language runtime preserves isolated
  procedure state across rounds and qualifies causal decision IDs by procedure.
- 2026-08-28: RoundId is the shared dynamic-DAG layer, not a per-procedure
  counter. Procedure revision is separate, and captured procedures retain an
  immutable frame at every global RoundId, including idle frames.
- 2026-08-28: owner golden correction: procedure is the automaton instance;
  replay replays complete procedures and context injections, never transition
  commands. Capture filters must close automatically into complete replayable
  procedures. Quiescent procedures resume only through typed `when`/`where`
  context injection.
- 2026-08-28: implemented the then-current procedure-owned interpretation
  without removing search replay. This capture detail was superseded by the
  2026-08-31 occurrence-owned correction below.
  `Event(...) @ Procedure` is typed replay input; `Transition.case(...) @
  Procedure` is a derived-path assertion. RuntimeContext owns persistent
  instances and complete initial-state artifacts; projected traces remain
  non-replayable.
- 2026-08-31: corrected capture ownership. State/transition/procedure are seed
  relations over occurrences; closed closure follows explicit causal edges and
  optional temporal intervals. Procedure is only a grouping label. Generic
  `TraceArtifact` replay now covers both free Engine and procedure runtimes.
- 2026-09-01: refreshed the REPL around occurrence ownership. Exact free
  transition admission uses `Engine::step_transition`; declared/live trace
  views expose occurrence causality, temporal status and typed prefix rounds;
  `:replay-artifact` invokes the generic core replay API. All three suites pass
  45/45 and a real PTY smoke passes.
- 2026-08-28: `v0.3.1` corrects injection to an extra typed transition
  occurrence addressed by TransitionId. A procedure contains only initial
  context/state, starts DTESSL's own search loop and is quiescent when no
  occurrence is pending; there is no external search-library call or
  procedure-local admission rule.
- 2026-08-28: backend search now resolves TransitionId/event family and the
  current active-state signature through indexes before evaluating dynamic
  `where`. Replay's optional case name is an assertion, never a forced path.
- 2026-08-28: `v0.3.2` relaxes exactly-one selection only under explicit
  `transition @ scope [optimized_score=...]`. Scores observe candidate
  after-state, the unique greatest exact score wins, and ties reject without
  committing state or RoundId.
- 2026-08-28: exposed Golden procedure semantics in the REPL with atomic
  multi-procedure `:inject`, `:runtime`, `:capture` and
  `:replay-procedures`. Declared `:trace`/`:claims` are separated from explicit
  `:trace-live`/`:claims-live`; merged conformance is revalidated on v0.3.2.
