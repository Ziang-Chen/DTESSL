# DTESSL Goal Contract

Updated: 2026-08-28, Asia/Shanghai

## North star

Build DTESSL into a concise, deterministic and independently usable language
for describing and simulating discrete-time event systems. The same typed model
must eventually support deterministic execution, runtime monitoring, bounded
exploration and formal export without turning external effects or physical
completion into language-level fiction.

## Allowed surface

- DTESSL grammar, canonical typed AST and verifier.
- C++ reference compiler, simulator, search engine and host interface.
- Standard finite collection, relation, matrix and modeling libraries.
- Native typed EventTrace, logical replay, monitor and bounded exploration formats.
- Formal exporters and optional solver adapters.
- Formatter, diagnostics, test corpus and language tooling.
- DTESSL repository releases and the pinned external VM submodule reference.

## Non-goals

- A general-purpose application language or operating-system shell.
- A universal bytecode shared by external VM, Wasm, JVM and native runtimes.
- Ambient clock, randomness, filesystem, network, process or secret access.
- Allowing a model to mint authority, handles, leases, tokens or completion
  facts that only a host can attest.
- Hidden programs encoded in JSON/YAML or string-keyed attribute bags.
- Production nondeterminism without an explicit deterministic resolver.
- Making external VM a mandatory backend for the standalone simulator.

## Exit criteria

The project is complete only when all of the following are evidenced:

1. The stable grammar and canonical AST express the state, transition,
   relationship search, action DAG, binding and claim designs in
   `LANGUAGE_DESIGN.md`.
2. All executable programs are deterministic, total within declared budgets
   and reproducible across supported C++ platforms.
3. External calls are typed proposals only; DTESSL never mints or consumes
   physical completion and exposes no runtime receipt/journal ingestion API.
4. One typed AST has four verified projections: executable reducer, runtime
   monitor, bounded explorer and formal exporter.
5. Parser, formatter, type checker, verifier, simulator, trace/replay and host
   adapter have positive, negative, adversarial and end-to-end evidence.
6. Canonical forms and digests are invariant under formatting, aliases and
   declaration order where order is not semantic.
7. Versioned migration and compatibility rules cover source, AST, native trace
   and typed ActionPlan formats.
8. A tagged release can be built and tested standalone and through the external VM
   submodule without copied source.
9. Documentation lets another operator implement a model and diagnose a failed
   transition without reading compiler internals.

## Budget and termination conditions

- Scope is bounded by the milestone gates in `ROADMAP.md`; later-milestone work
  must not be pulled into an earlier release just because it is adjacent.
- Each version must have a finite resource/search budget and acceptance matrix.
- Stop and request an owner decision if syntax choices change the meaning of
  existing user-approved constructs, if a release would require moving a
  published tag, or if a host integration would grant new authority.
- A local build alone is candidate evidence, never final validation.

## Current v0.4.0 recursive Embedding boundary

- Work is restricted to the DTESSL repository and the
  `codex/repl-occurrence-artifact-demo-v0-4-0` branch.
- The built-in `Solver` is the sole high-performance semantic layer between the
  verified frontend Program and runtime/exploration backends. Static `state`
  declarations are not search nodes: dynamic `Embedding` nodes and
  transition edges form the `EmbeddingExpand` graph.
- Compact syntax is a source-level AST and direct lowering only. It cannot
  create a second semantic pipeline or bypass typing, deterministic search,
  procedure isolation, causal RoundId, capture or replay checks.
- The slice preserves the v0.3.0 state/trace semantics while separating the
  source frontend from runtime execution and making transition-occurrence
  injection explicit.
- Multiple enabled candidates are accepted only when their transition declares
  one common `@scope [optimized_score=...]`; the unique greatest exact score is
  selected from candidate after-states and equal best scores reject atomically.
- A procedure contains an initial context/state combination and may declare
  anonymous local automaton transitions. Those declarations are unordered
  graph edges scoped to the procedure, not admission rules, traces or workflow
  steps. Named external work still requires typed TransitionId injection.
- Claims bind explicitly to a trace, state, procedure or named relation. Their five primitive
  temporal operators compile to finite monitor state, and verification explores
  `EmbeddingExpand × ClaimMonitor`; derived temporal names do not add Solver cases.
- Golden replay consumes a complete procedure artifact: initial/checkpoint
  state plus typed transition-occurrence injections. Transition paths and the
  dynamic DAG are recomputed evidence, not replay commands.
- Capture is owned by actual OccurrenceIds. State/transition/procedure filters
  are seed relations; closed capture follows explicit causal predecessors and
  optional RoundId temporal intervals into a generic typed `TraceArtifact`.
  Procedure is only a label and compatibility projection; a lossy projection
  cannot be labelled replayable.
- A quiescent procedure resumes when
  `inject Transition(fields...) @ Procedure` adds an occurrence to its pending
  set. The runtime selects candidates by exact TransitionId and active-state
  signature, then evaluates dynamic `where`; injection never chooses a case or
  directly mutates state.
- RoundId is derived from the dynamic causal DAG layer, while per-procedure
  revision is a distinct state-version counter.
- Completion requires parser/verifier/runtime/CLI/REPL and real PTY evidence,
  plus standard, `-Werror` and ASan/UBSan gates, followed by a scoped commit
  and push.
- This slice does not authorize changes to `main`, tags, host runtime, external VM or any
  other task thread.

## Owner decisions already fixed

- Name: DTESSL（戴特赛尔）.
- Implementation language: C++.
- Version format: `vMilestone.MajorFeature.MinorFeature`.
- State and transition are separate definitions.
- `@` means explicit context/binding, not authority.
- `$` identifies an injected external interface.
- Named action nodes use `name:` labels.
- `,` composes actions serially; `|` composes them in parallel.
- Logical implication uses `->`; action sequencing does not use `then`.
- `where` is the single surface for guards and relational eligibility/search.
- `case (source-set) -> (target-set):` is the primary transition path syntax;
  source items are conjunctive, repeated cases are alternatives, and
  `where/set/do` are path-local.
- `name T` and `T(atom)` are nominal logical identifiers, not strings,
  capabilities or authority.
- `~` is reserved for relation types, literals and typed relation matching.
- `[T]`, `[]` and `[value]` express typed optional cardinality; ordered lists
  use explicit `list[...]` values.
- Physical effects stay in the host; DTESSL computes logical change and typed
  call plans.

## Parking lot

- JIT/native code generation before profiling proves the interpreter/search
  engine insufficient.
- General floating point before a deterministic numeric profile is specified.
- UI designers and graphical statecharts before the canonical model is stable.
- A unified VM bytecode.
