# DTESSL Project QC Control

Updated: 2026-08-28, Asia/Shanghai

## Control metadata

- Owner: Ziang-Chen
- Target repository: `Ziang-Chen/DTESSL`
- Integration repository: `Ziang-Chen/chenVirtualMachine`,
  `external/DTESSL` submodule (explicitly outside this integration slice)
- Current target: gate `v0.3.0` composite state and native trace on a DTESSL-only branch
- Stop orders: none

## Current state

**Complete.** The `v0.3.0` implementation now has two explicit, non-confused
replay capabilities: typed context replay from complete initial-state procedure
artifacts, and derived-path search assertions. Procedure-local `inject`
admission and conservative closed-capture closure are implemented. Standard,
`-Werror` and ASan/UBSan suites pass 22/22. Implementation commit `d18845b` is
pushed on `codex/composite-trace-claims-v0-3-0`; this QC closeout commit records
the final handoff and clean-tree evidence.

## Current goal contract

- North star: see `GOAL_CONTRACT.md`.
- Allowed surface: this DTESSL repository's parser, verifier, simulator,
  language service, CLI/REPL, examples, tests and documentation.
- Non-goals: `main`, tags, ChenVM/chenRT, other repositories or task threads,
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
| Version identity | CMake/generated header target `0.3.0` | Built CLI reports `v0.3.0` | pass |
| Logical names | Distinct public Value kind, nominal type check, canonical codec roundtrip and invalid-atom rejection | Scheduler renders `WorkerId(a)` without string quotes | pass |
| Compact relation binding | `~T`, `~(A,B)`, `~{}` and `~` parser/type/evaluator tests; legacy unary tuple behavior retained | Scheduler searches `worker.capacity` without `.0` | pass |
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
| REPL workbench | Multi-line insert/replace/delete, undo/redo, load/save and engine invalidation implemented | Real PTY passes history recall, cursor/Delete repair, replace, undo, check and two-round fancy run | pass |
| Multi-state transition model | Conjunctive source/target sets, alternative case paths, atomic invariant gate and ambiguity tests | Composite scheduler example and CLI trace pass | pass |
| Native static/dynamic trace | Source EventTrace plus closed/projected Engine capture scoped by `@context` | Static run and projected capture tests pass | pass |
| Finite-prefix claims | always/eventually/count plus implication and three statuses | Closed trace claim CLI and projection-gap tests pass | pass |
| Pure helper functions | Typed parameter/result checking, state/round isolation and recursion rejection | Composite guard calls a verified helper | pass |
| Procedure entry boundary | Parser accepts initial context/state plus typed admission rules; no transition or ordered steps may be nested | Explicit Engine startup and RuntimeContext dispatch both use global transitions | pass |
| Procedure RuntimeContext | Two procedure instances retain isolated typed state across an interleaved replay; same-layer decisions share RoundId and same-procedure dependencies retain qualified predecessor IDs | Idle procedure frame remains queryable at the next global RoundId | pass |
| Capture filter | Typed state/transition/procedure selector sets validate references and procedure capture emits immutable per-RoundId frames | Composite example captures one procedure and dual-procedure test retains both histories | pass |
| Golden procedure replay | Replay consumes declared initial configuration plus typed context-injection history; transitions are recomputed | Public `ProcedureArtifact` replay is run twice and compared; wrong path expectation rejects | pass |
| Search replay | A named transition path may be asserted only as derived evidence, never forced | `Transition.case(...) @ Procedure` and explicit `SearchExpectation` both re-enter ordinary search | pass |
| Quiescent context injection | Procedure-local typed injection with static `when` and dynamic `where`, without direct state mutation | Parser/verifier/runtime exercise event-schema equality and dynamic admission | pass |
| Filter-to-procedure closure | State/transition/procedure filter seeds automatic causal/data closure into a replayable procedure artifact | Closed path/state seed retains whole matching procedure; projected capture emits no artifact | pass |
| Physical receipt isolation | Public API contains no receipt/journal ingestion | Physical completion remains outside DTESSL | pass |
| Four AST projections | Architecture specified | Implementations absent | missing |

## Gaps and blockers

- P0 gap: source AST is currently internal and has no canonical serialized form.
- P0 gap: backend/provider contracts are designed but wait on canonical AST.
- P1 gap: no external user dogfood model beyond the bundled fixtures.

- P0 handoff gate: record the pushed integration-branch SHA and clean status.

## Risks

- Scope: implementing all domain dialects inside the core would bloat the
  language. The P0/P1/P2 split is mandatory.
- Verification: C++ implementation is still concentrated in one source file;
  refactoring must follow semantic tests, not precede them.
- Validation: syntax remains candidate until exercised on at least one
  non-trivial user model.
- Release: DTESSL tags and ChenVM submodule updates must remain ordered.

## Next corrective focus

1. Finish standard, `-Werror` and ASan/UBSan gates for `v0.3.0`, then push the
   scoped branch and verify its exact SHA.
2. Resume typed derived data and explicit shared inputs as the next slice.
3. Preserve the permanent native Program/EventTrace-only validation boundary.
4. Keep external projection producers separate from DTESSL verification.
5. Reject any claim that the full language is complete until all roadmap gates
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
- 2026-08-28: fixed procedure as an entry configuration only: initial context
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
- 2026-08-28: implemented the correction without removing search replay.
  `Event(...) @ Procedure` is typed replay input; `Transition.case(...) @
  Procedure` is a derived-path assertion. RuntimeContext owns persistent
  instances and complete initial-state artifacts; closed filters conservatively
  retain whole-procedure closure, while projected traces are non-replayable.
