# DTESSL Roadmap

Versions follow `vMilestone.MajorFeature.MinorFeature`. Each row is an
acceptance slice, not merely a calendar label.

Released foundation: `v0.0.1`, `v0.0.2`, `v0.0.3`, `v0.1.0`, `v0.1.1`,
`v0.1.2`, `v0.1.3`, `v0.2.0`, `v0.2.1`. Current release gate: `v0.3.0`.

## Permanent architecture invariant — 2026-08-28

This is a long-term product boundary, independent of the current milestone:

- DTESSL consumes only a DTESSL `Program` and native typed `EventTrace`, and
  produces deterministic logical results and typed `ActionPlan` proposals.
- DTESSL does not ingest, normalize or replay chenRT/runtime journals, syslog,
  audit logs, Provider receipts, snapshots, arbitrary JSON or free text.
- An existing system must actively export a lossy DTESSL projection artifact
  with provenance, coverage and classified gaps. DTESSL does not infer it.
- A generated operational mirror is never represented as an independently
  authored assurance model.
- An ActionPlan can mention only declared typed ports. Physical authority,
  execution, receipts, runtime replay and reconciliation are outside DTESSL.

No v1/v2 feature, adapter or solver may weaken this invariant.

## Milestone 0 — semantic foundation

| Version | Feature slice | Required acceptance evidence |
| --- | --- | --- |
| `v0.0.1` | Minimal executable loop | Parser/type verifier, typed state, parallel event round, invariant, finite set predicate, action DAG, bounded scratch pool, CLI/replay tests |
| `v0.0.2` | Causal-round hardening | Explicit event bags, read/write sets, confluent merge hook, causal predecessors and deterministic parallel scheduling corpus |
| `v0.0.3` | Backend seam | Canonical-module placeholder contract, typed backend feature negotiation and interpreter/VM/provider rejection tests without freezing the private AST |
| `v0.1.0` | Canonical value codec | Bounded canonical codec for foundation values, minimal encodings, golden bytes and hostile-input corpus |
| `v0.1.1` | Generic finite collections | `list/set/map/bag<T>`, recursive typed values, canonical ordering and collection-budget tests |
| `v0.1.2` | Algebraic/nominal values | Records, variants, enums, newtypes, option/result, exhaustive matching and schema-aware canonical field/constructor IDs |
| `v0.1.3` | Exact numeric profile | Arbitrary/exact integer policy, normalized rational, deterministic arithmetic and cross-backend golden corpus |
| `v0.2.0` | Relation and deterministic search | Relation/join/project/compose/closure, `E/A`, `select ... by lex`, static and runtime search plans, ambiguity/budget adversarial tests |
| `v0.2.1` | Declared semantic projection | Canonical SemanticDescriptor, typed action ports, descriptor-to-DTESSL generator, source map/digests, coverage/gaps and native EventTrace replay without runtime-log adapters |
| `v0.3.0` | Full state theory | `data/derive/invariant`, state sets, explicit shared inputs, trace/history relations, transition occurrence claims |
| `v0.4.0` | Whole-system transition relations | Multi-component `from/to`, typed before/after patterns, independent/dependent targets, lifecycle/completion library axes, atomic commit tests |
| `v0.5.0` | Typed action and scheduler model | Canonical call DAG, typed port registry, `requires` relation, deterministic scheduler queries and backend-neutral ActionPlan API without physical receipt ingestion |
| `v0.6.0` | Context and Binding | Package/namespace, immutable shared values, first-class Binding skeleton and typed derived bindings, stale generation/fencing tests |
| `v0.7.0` | Claims and monitors | `claim`, implication, trace projection/redaction, monitor automata, unknown/failed/proved status and evidence metadata |
| `v0.8.0` | Native trace, replay and migration | Versioned DTESSL EventTrace, logical replay, state/schema migration, executable/source digests and compatibility corpus |
| `v0.9.0` | Foundation hardening | Parser/search limits, fuzzing, differential determinism, failure recovery, benchmarks and release candidate documentation |

Milestone 0 exits only when the complete language design is represented in the
canonical AST, even if advanced domain theories remain libraries or exporters.

## Milestone 1 — stable executable modeling system

| Version | Feature slice | Required acceptance evidence |
| --- | --- | --- |
| `v1.0.0` | Stable core language/runtime | Frozen grammar/core AST v1, compatibility policy, packaged CLI/library, end-to-end host simulation and migration guide |
| `v1.1.0` | Workflow algebra | Task, serial/parallel/race, compensation, deadline and cancellation lowered to events/transitions/action DAG |
| `v1.2.0` | Matrix and optimization library | Dense/sparse typed matrices, exact profiles, argmin/lex scoring and performance/reference equivalence tests |
| `v1.3.0` | Distributed/resource dialect | Message/fault schedules, leases/credits/conservation/backpressure and deterministic provider facts |
| `v1.4.0` | Security and memory dialect | actsFor/rights/labels/declassification, RegionRef/ownership/HB/consistency claims and adversarial corpus |
| `v1.5.0` | Tooling | Formatter, incremental compiler cache, stable diagnostics, LSP navigation/completion and syntax migration tool |
| `v1.6.0` | chenRT/ChenVM and external adapters | Typed host interface injection, package embedding, trace exchange and reference/fallback parity for ChenVM and out-of-tree VM/provider implementations without shared bytecode |

## Milestone 2 — four projections from one typed AST

| Version | Feature slice | Required acceptance evidence |
| --- | --- | --- |
| `v2.0.0` | Runtime monitor projection | Invariant/claim-to-monitor lowering and trace-driven counterexample tests |
| `v2.1.0` | Bounded model exploration | Finite-domain state search, spec `choose`, fault schedule exploration, counterexample replay into runtime |
| `v2.2.0` | TLA+ export | Refinement mapping, fairness/spec-only validation and generated-model conformance corpus |
| `v2.3.0` | Timed automata export | Logical clocks, guards/resets/deadlines/invariants and bounded timed equivalence tests |
| `v2.4.0` | Memory/security logic export | Separation/HB/consistency and noninterference claim export with evidence status import |
| `v2.5.0` | Compositional automata | Product/synchronization/projection/hiding/renaming/assume-guarantee and refinement checks |

## Milestone 3 — distributed assurance ecosystem

| Version | Feature slice | Required acceptance evidence |
| --- | --- | --- |
| `v3.0.0` | Stable assurance metadata | Claim/evidence/provenance schemas, redaction policy and reproducible assurance bundles |
| `v3.1.0` | Solver/plugin protocol | Sandboxed external solver interface, resource limits, result verification and untrusted-output handling |
| `v3.2.0` | Provider/fabric modeling kit | Binding-derived identity/capability/realm/provider/state/channel/artifact models and search benchmarks |

## Release discipline

For every version:

1. Update compiler version, changelog, compatibility table and current QC facts.
2. Provide static, unit, integration and user-path evidence proportional to the
   slice.
3. Run sanitizer/fuzzer/performance gates where affected.
4. Tag only the verified commit.
5. Update the ChenVM submodule only after the DTESSL tag exists.
6. Never mark a future roadmap row complete because its syntax appears in a
   design document.
