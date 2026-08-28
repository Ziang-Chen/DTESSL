# DTESSL Project QC Control

Updated: 2026-08-28, Asia/Shanghai

## Control metadata

- Owner: Ziang-Chen
- Target repository: `Ziang-Chen/DTESSL`
- Integration repository: `Ziang-Chen/chenVirtualMachine`,
  `external/DTESSL` submodule
- Current target: integrate and release `v0.2.3`, then resume `v0.3.0`
- Stop orders: none

## Current state

**Integration candidate.** The isolated `v0.2.3` branch adds the compact core
logical surface over the released `v0.2.1` baseline. Standard tests and the
new scheduler CLI path passes under standard, `-Werror`, ASan/UBSan and an
isolated installed CLI; merge remains required before release. This is not the complete DTESSL
design described in `LANGUAGE_DESIGN.md`.

## Current goal contract

- North star: see `GOAL_CONTRACT.md`.
- Allowed surface: language/compiler/simulator/tooling/docs/releases and pinned
  ChenVM integration.
- Non-goals: ambient authority, hidden JSON programs, runtime nondeterminism,
  universal bytecode and premature JIT.
- Current exit gate: implement `v0.3.0` full state theory after the independent
  validation slice remains stable.

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
| Version identity | Isolated branch CMake/generated header/test at `0.2.3` | Installed isolated CLI reports `v0.2.3`; merged CLI not yet tested | candidate |
| Logical names | Distinct public Value kind, nominal type check, canonical codec roundtrip and invalid-atom rejection | Scheduler renders `WorkerId(a)` without string quotes | pass |
| Compact relation binding | `~T`, `~(A,B)`, `~{}` and `~` parser/type/evaluator tests; legacy unary tuple behavior retained | Scheduler searches `worker.capacity` without `.0` | pass |
| Bracket options | `[T]`, `[]`, `[value]`, empty-context rejection and exhaustive option-pattern test | Choose/reset two-step model exercises present/absent states | pass |
| Explicit list literals | `list[...]` parse and canonical rendering | Scheduler fixture contains typed logical-name list | pass |
| Full relation/search design | Profile plus implementation and adversarial tests | Standard, `-Werror` and ASan/UBSan suites pass 7/7; installed graph replay/plans pass | pass |
| Explicit model projection | Canonical descriptor parse/print, origin digest, provenance and classified-gap checks | Scheduler projection generates checked DTESSL | pass |
| Generated source evidence | Stable descriptor/source SHA-256 and complete nonblank-line source map | CLI check/source-map/manifest exercised | pass |
| Typed action ports | Declared port lookup plus exact argument-type verification | Three-port serial/parallel ActionPlan fixture passes; mismatch rejects | pass |
| Lifecycle mappings | Unique field/phase/literal checks and generated literal typecheck | string lifecycle fixture passes; int literal rejects | pass |
| Native EventTrace replay | Bounded typed batches, empty-batch rejection and fresh-engine equality | Two-round logical state/action replay passes | pass |
| Runtime-log exclusion | No log, receipt, snapshot or journal types/adapters in public API | Descriptor contains explicit external-runtime gap | pass |
| Multi-state transition model | Specification exists | Implementation absent | missing |
| Physical receipt isolation | Public API contains no receipt/journal ingestion | Physical completion remains outside DTESSL | pass |
| Four AST projections | Architecture specified | Implementations absent | missing |

## Gaps and blockers

- P0 gap: source AST is currently internal and has no canonical serialized form.
- P0 gap: backend/provider contracts are designed but wait on canonical AST.
- P1 gap: no external user dogfood model beyond the bundled fixtures.

- Integration dependency: `v0.2.3` was developed in an isolated worktree to
  avoid overwriting unrelated uncommitted language-service work in the main
  worktree. It must be rebased/merged only after that owner has produced a
  stable commit.

## Risks

- Scope: implementing all domain dialects inside the core would bloat the
  language. The P0/P1/P2 split is mandatory.
- Verification: C++ implementation is still concentrated in one source file;
  refactoring must follow semantic tests, not precede them.
- Validation: syntax remains candidate until exercised on at least one
  non-trivial user model.
- Release: DTESSL tags and ChenVM submodule updates must remain ordered.

## Next corrective focus

1. Integrate the fully gated `v0.2.3` branch without overwriting the main
   worktree, then repeat the gates on the merged tree.
2. Resume `v0.3.0` typed derived data, state sets and explicit shared inputs.
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
- 2026-08-28: `v0.2.3` core logical surface implemented as an isolated
  integration candidate so concurrent uncommitted tooling work was preserved.
