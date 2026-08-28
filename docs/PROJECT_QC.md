# DTESSL Project QC Control

Updated: 2026-08-28, Asia/Shanghai

## Control metadata

- Owner: Ziang-Chen
- Target repository: `Ziang-Chen/DTESSL`
- Integration repository: `Ziang-Chen/chenVirtualMachine`,
  `external/DTESSL` submodule
- Current target: `v0.2.0`
- Stop orders: none

## Current state

**Released slice.** `v0.1.3` exact integers/rationals passed standard,
`-Werror`, ASan/UBSan, install, malformed-input and CLI replay gates. This is
not the complete DTESSL design described in `LANGUAGE_DESIGN.md`; the next gate
is `v0.2.0` relation and deterministic search.

## Current goal contract

- North star: see `GOAL_CONTRACT.md`.
- Allowed surface: language/compiler/simulator/tooling/docs/releases and pinned
  ChenVM integration.
- Non-goals: ambient authority, hidden JSON programs, runtime nondeterminism,
  universal bytecode and premature JIT.
- Current exit gate: publish `v0.2.0` finite relation algebra, `E/A`, explicit
  deterministic selection and bounded static/runtime search plans.

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
| No ambient effects | Engine only returns `ActionPlan` | Host is not invoked by CLI | pass |
| Runtime memory safety | ASan/UBSan baseline plus bounded scratch-pool unit test | Pool not yet used by parser/search hot paths | candidate |
| Version identity | CMake/generated header/CLI/test at `0.1.3` | Release tag and installed CLI smoke | pass |
| Full relation/search design | Specification exists | Implementation absent | missing |
| Multi-state transition model | Specification exists | Implementation absent | missing |
| Typed host receipts/replay | Call plan baseline exists | Receipt protocol absent | missing |
| Four AST projections | Architecture specified | Implementations absent | missing |

## Gaps and blockers

- P0 gap: source AST is currently internal and has no canonical serialized form.
- P0 gap: `exists` is boolean-only; no selected binding or relation algebra.
- P0 gap: replay is one event and recomputes rather than consuming recorded
  receipts.
- P0 gap: backend/provider contracts are designed but wait on canonical AST.
- P1 gap: no external user dogfood model beyond the bundled fixtures.

There is no external blocker for the `v0.2.0` gate.

## Risks

- Scope: implementing all domain dialects inside the core would bloat the
  language. The P0/P1/P2 split is mandatory.
- Verification: C++ implementation is still concentrated in one source file;
  refactoring must follow semantic tests, not precede them.
- Validation: syntax remains candidate until exercised on at least one
  non-trivial user model.
- Release: DTESSL tags and ChenVM submodule updates must remain ordered.

## Next corrective focus

1. Define a canonical n-ary relation value and typed relation schema.
2. Implement deterministic project/join/compose/closure and `E/A`.
3. Add explicit `select ... by lex` with ambiguity and budget rejection.
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
