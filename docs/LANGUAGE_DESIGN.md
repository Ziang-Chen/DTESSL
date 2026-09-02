# DTESSL Language Design

Status labels used below:

- **Implemented**: accepted and executed through the `v0.4.0` recursive Embedding slice.
- **P0**: required for the complete executable modeling core.
- **P1**: standard library or standard dialect built on the core.
- **P2**: external solver, exporter or advanced assurance integration.

## 1. Semantic center

### Golden procedure/replay/capture principles

These rules override any earlier v0 experimental syntax or implementation:

1. A `procedure` is one persistent automaton instance built from DTESSL
   `state` and `transition` definitions. It is not a workflow script, trace or
   transition list.
2. Replay replays a procedure: its initial embedding, typed context
   injections and resulting logical history. Transition occurrences and the
   dynamic DAG are recomputed evidence, never authoritative replay input.
3. Capture either selects complete procedures or accepts a state/transition/
   procedure filter as a seed. The language must compute causal and data
   dependency closure and emit a complete replayable procedure artifact. A
   partial projection may not claim to be replayable.
4. A procedure begins with initial states plus initial context. Package
   transitions and procedure-local anonymous transitions are automaton edges,
   never an ordered workflow body. The Solver expands anonymous edges directly;
   `inject Transition(...) @ Procedure` adds a typed occurrence for named
   transitions and neither mutates state nor chooses a case.
5. The language RuntimeContext owns procedure instances, state, revisions,
   pending injections and causal frontiers. Different procedures are isolated
   at `ProcedureId / Context / StateField`; safe shared storage is a later,
   explicit distributed-state model.

The core runtime closure is therefore:

```text
inject typed transition occurrence
  -> TransitionId index
  -> active-state-signature case index
  -> evaluate dynamic where predicates
  -> unique candidate, or explicit optimized_score maximum
  -> reject an equal best score
  -> choose a deterministic conflict-free set
  -> commit one causal-DAG round
  -> repeat until quiescent
  -> await the next transition injection
```

All transition nodes in one DAG layer share a `RoundId`. Procedure revision is
only a local state version. Runtime traversal order cannot create logical time.

The implemented minimal injection surface is:

```dtessl
transition Resume(session: SessionId):
  case waiting (Waiting @ workflow) -> (Active @ workflow):
    where:
      session = workflow.owner

procedure Session @ system:
  initial (Idle @ workflow)

trace ResumeExample:
  replay:
    inject Resume(SessionId(a)) @ Session -> Resume.waiting
```

The transition declaration is the message schema. `inject` creates a typed
occurrence addressed by TransitionId and places it in the target procedure's
pending set. Static case topology and dynamic `where` are the admission/search
rules. The engine, not the replay source, selects either the sole path or the
unique optimum under an explicit score. The optional arrow is only a
derived-path assertion.

Multiple enabled candidates are legal only under an explicit transition
optimizer:

```dtessl
transition Choose @ scheduler [optimized_score = utility(scheduler.score - before.scheduler.score)]():
  case low  (Idle @ scheduler) -> (Idle @ scheduler): ...
  case high (Idle @ scheduler) -> (Idle @ scheduler): ...
```

The score expression is typed against the declared `@scope`, event parameters
and pure functions, then evaluated over each candidate's proposed after-state.
It must produce an exact `int` or `rational`; the greatest value wins. Equal
greatest values remain a deterministic ambiguity and reject the round. With no
optimizer, the original exactly-one-enabled-candidate invariant remains.

The v0.4 closure corrects the earlier procedure-owned model. State, transition
and procedure filters are seed relations over actual occurrences. A closed
capture recursively retains explicit causal predecessors by `OccurrenceId`;
procedure is only an optional label and grouping projection, so free-floating
state/transition occurrences remain first-class. An optional `eventually`
target retains the temporal interval from every seed anchor through its first
witness. The replay authority is a generic `TraceArtifact` containing the
typed input prefix required to rederive that selected interval from initial
state. `ProcedureArtifact` remains a compatibility view, while
`capture projected` emits no replay artifact and is explicitly not replayable.
The causal DAG has two independent edge sources: field writer/read dependency
and active-state-axis producer/consumer dependency. The latter preserves
Embedding continuity even for a transition whose assignments are constants.

### Permanent system boundary (2026-08-28)

DTESSL is not a model extractor or universal log engine. It accepts DTESSL
programs and native typed event traces, then computes deterministic logical
state and ActionPlans. Existing systems may actively export a lossy model
projection with provenance, coverage and classified gaps; DTESSL never derives
one from implementation code or runtime logs. Runtime journals, snapshots,
receipts, effect suppression and reconcile are outside DTESSL. Generated mirrors
must remain distinguishable from independent assurance models for the lifetime
of the language.

DTESSL models one discrete simulation round as a relation over typed transition
occurrences delivered to procedure instances:

```text
(RuntimeContextBefore, Bag<ProcedureId x TransitionInput>)
  -> DecisionSet(AfterState, ActionDAGs, Claims, Observations)
```

All transitions in the set read one immutable before snapshot. Their writes are
merged only when disjoint or when a typed merge relation proves confluence. The
runtime accepts the set only after type, conflict, invariant, determinism and
budget checks. An action DAG is a proposal to a host. It is not evidence that a
physical effect happened.

State definitions describe admissible state spaces and their constraints.
Transition definitions describe whole-system changes between state spaces.
This keeps a state from being a thin label while preserving a clean distinction
between “what can hold” and “what can change”.

## 2. One AST, two semantic modes, four projections

The canonical typed AST has two modes:

- `runtime`: every choice must be singleton or use an explicit deterministic
  resolver; time and external facts arrive as events.
- `spec`: bounded nondeterminism, fairness and fault schedules may be expressed,
  but cannot lower to production execution without an explicit resolver.

The same AST supports four projections:

| Projection | Input subset | Output |
| --- | --- | --- |
| Executable reducer | Total deterministic runtime subset | New state and typed action DAG |
| Runtime monitor | Claims, invariants and trace projections | Assertion/monitor automata |
| Bounded explorer | Finite domains, spec choices and fault schedules | Reachability graph and counterexamples |
| Formal export | Typed transition system and selected dialect facts | TLA+, timed-automata or memory-logic model |

Projection-specific data may annotate the AST, but cannot redefine core state
or transition meaning.

## 3. Compact surface vocabulary

### Quick automaton form (v0.3.3)

A declaration beginning with `state`, `trans`, `procedure` or `trace` and
ending in `;` on that same physical line is a compact AST form:

```dtessl
state a, b, c;
trans ctodo: a -> b when b.val == 0;
trans ctodo: a -> c when b.val != 0;
procedure p1 a, b.val = 0, c & inject ctodo;
trace @procedure;
```

It is syntax sugar for rapid modeling, not a second language. Repeated
`trans ctodo:` declarations are paths in one declared transition family;
`inject ctodo` is a checked reference and never declares or renames that
family. Anonymous `trans a -> b` receives a stable generated identity.
Transition-graph
connected components derive orthogonal `@context` axes. The first procedure
state named on an axis is initial; later names on that axis are non-active
model hints. Scalar assignments declare typed axis data. A comma between
top-level `when` expressions is logical conjunction. The compact AST lowers
directly to the ordinary typed AST before verification, indexing, execution,
capture or replay. Newlines before `;` are rejected.

### Structural words

- `state`: typed state space, initial data, derived views and invariants.
- `transition`: event-conditioned relation from source state set to target state
  set.
- `relation`: a pure typed RelationPlan or current-Embedding matcher template.
  Rule relations admit both equal name-first and tuple-first compact
  projections; a state matcher may own only patterns and `where`, never an
  after-Embedding, update, action or temporal obligation.
- `from` / `to`: source and target state patterns. `goto` is not used.
- `where`: eligibility and finite relational search. A closed `where` expression
  is the LTS guard; `guard` is therefore a compiler term, not another surface
  section.
- `do`: labelled action DAG proposed to injected host interfaces.
- `claim`: non-mutating logical property. `->` means logical implication.

### Punctuation

- `name:` declares a node, field or block. `#action` is not a node syntax.
- `@x` selects an explicit context, binding, object or instance. It never grants
  rights.
- `$port.operation(...)` calls a typed injected interface. It never names raw
  authority by string.
- `a, b` creates an action dependency `a -> b`.
- `a | b` creates parallel branches with no ordering edge.
- Between named state-relation templates inside a transition, `R1, R2` is
  conjunction over the same current Embedding; `R1 | R2` is alternative.
  Comma binds more tightly. The matcher expression creates no hidden
  intermediate Embedding and performs no state update.
- `#tag(...)` is reserved for non-executable observation/evidence metadata. It
  cannot be read by expressions or affect transition choice. Editorial tags are
  excluded from executable digests and included in source digests.
- `then` is not part of the core grammar. This avoids overloading logical
  implication and action sequencing.

In an action expression, comma binds more tightly than pipe: `a,b | c,d` is
`(a,b) | (c,d)`. Parentheses make fan-out/fan-in explicit.

## 4. Packages, contexts and shared values

**P0/P1 target:**

```text
package scheduler

import type identity.UserId
import capability ipc: SchedulerIPC

shared const policy: Policy
shared input topology: Relation<Node, Node>
```

- Package imports/exports are typed and version-constrained.
- `shared const` is immutable package data.
- `shared input` is an explicitly supplied model input included in run identity.
- Mutable “global variables” are forbidden; shared mutable data must be a named
  state component so replay and exploration can see it.
- A type class/trait is a compile-time constraint over types, never a hidden
  substate.
- Namespaces only qualify names. Context is a runtime value and Binding is the
  typed bridge between logical and physical identities.

## 5. Type system and mathematical data

### P0 core values

- `bool`, exact bounded/unbounded `int`, exact `rational`, `string`, `bytes`.
- `[T]` options with `[]`/`[value]`, plus `result<T,E>`, explicit
  `list<T> = list[...]`, `set<T>`, `map<K,V>` and `bag<T>`.
- direct unary `relation T`, Product-row `relation <A,B,...>` and deterministic iteration.
- records, variants, enums and nominal `newtype`.
- `name T` with nominal logical atoms `T(a)`, separate from string text and
  from authority-bearing host references.
- opaque `ObjectRef<T>`, `VersionRef<T>`, `RegionRef<T>` and `Binding<K>` values
  that models cannot fabricate from strings or bytes.

The `v0.1.2` slice implements records, zero/unary-payload variants, enums,
newtypes, option/result, field projection and exhaustive expression matching.
Nominal values carry a canonical type identity; records canonicalize field
order. Package-qualified schema identities and schema migration remain part of
CanonicalModule rather than being guessed from source formatting.

The `v0.1.3` slice makes `int` arbitrary precision within an explicit
deterministic magnitude budget and adds normalized exact `rational`. There is no
ambient host floating-point mode. Division by zero and budget exhaustion reject
the logical round before commit; the exact cross-backend rules are frozen in
`EXACT_NUMERIC_PROFILE_V1.md`.

The `v0.2.0` slice implements first-class canonical tuple/relation values,
project/equijoin/compose/inverse/closure/set algebra, `E/A`, deterministic
`select ... by lex`, typed static search-plan summaries and explicit row/work
budgets. Its executable contract is frozen in `RELATION_SEARCH_V1.md`.

The `v0.4.2` surface fixes `~` to one meaning: recursively typed
`subject ~ relation-expression`. Boolean disjunction is written explicitly as
`or`; a predicate-chain comma remains conjunction/serial composition. Inside
an explicit container, comma only separates elements and the container decides
product (`<...>`) versus union (`{...}`).
The typed AST names this predicate `RelationMatch`. Both its value subject and
relation expression retain recursive structure. In a transition it combines
with the recursive control-state selector as
`TransitionMatch = StructuralPattern and RelationMatch`; it does not turn a
control location into an untyped value and does not perform the transition.
At the semantic layer, relation is the umbrella: instantaneous comparison and
membership are `RelationMatch`; trace-position/order formulas are the separate
recursive `TraceRelation` branch. The latter may contain temporal operator
cases because its domain is a trace rather than one Embedding, but both branches
enter verification and Solver through the same typed predicate boundary.
It retains first-class canonical logical names, direct unary-record relations
and bracket option forms. Legacy
`relation<T...>`, `in`, `option<T>`, `none` and `some` remain accepted during
v0 migration, but canonical value rendering uses the compact forms.

### Relation and search algebra

P0 operations include membership, union/intersection/difference, cartesian
product, select, project, join, inverse, relational composition and bounded
transitive closure. Quantifiers are written compactly:

```text
E x ~ xs: predicate(x)
A x ~ xs: predicate(x)
```

Long aliases `exists` and `all` may be accepted as surface sugar; canonical AST
uses `Exists` and `ForAll` nodes.

Deterministic choice is explicit:

```text
select worker ~ eligible
  by lex(load(worker), distance(worker, task), worker.id)
```

Zero candidates disable the transition unless handled. One candidate is
selected. Multiple candidates without `by`, a unique proof or an injected
resolver are a runtime verification error. `choose` belongs to `spec` mode.

### P1 numeric algebra

- Typed vectors, dense/sparse matrices and shape-checked operations.
- Exact integer/rational profiles by default.
- Floating point only under a named consistency profile with canonical NaN,
  rounding and cross-platform rules.
- `argmin`, `argmax` and lexicographic score lower to bounded search plans.

## 6. State definitions

The implemented form is one recursive algebra rather than separate control and
data languages:

```dtessl
state Scheduler @ fabric initial:
  Phase(Idle | Running(Stage(Reserve | Commit), attempts: int = 0,
                         invariant(attempts >= 0)))
  Health(Healthy | Degraded)
  ready: set<TaskId> = {}
  providers: relation Provider = {}

  invariant:
    self ~ ValidScheduler and (self ~ HasCapacity or self ~ MayQueue)
```

`<...>` is ordered product/pattern structure, `{...}` is unordered union/set
structure, and `|` remains parallel/choice syntax where that grammar explicitly
admits it. Outside containers, `,` is the compact conjunction/serial chain.
`A(B)` recursively contains B. A typed
value leaf is semantically a valued state component. A
choice leaf is a finite control component. Both belong to the same recursive
`StateSchema` and concrete `Embedding`, while `@context` remains the instance
address rather than another mutable field.

General and semicolon-terminated compact declarations construct the same AST.
Recursive validation rejects duplicate sibling/path names, empty/singleton
choices, invalid local invariants, excessive depth and excessive node count.
Lowering assigns stable semantic paths and uses `RawKeyMap` to map them to raw
embedding-vector offsets. `EmbeddingExpand` operates on semantic Embeddings;
the dense vectors are implementation details.

State-local invariants filter initial and successor Embeddings; they never
generate repairs or hidden search. Candidate-specific conditions remain in
transition `where`, temporal obligations in `ensure`, and global/path
properties in `Claim`. Future `derive` values remain planned and must be
recomputable rather than silently stored. `v0.4.3` supplies that rule for
named derived relations; general scalar/record derived fields are still absent.

Higher-order facts such as “transition T occurred N times” are modeled over the
explicit trace/history projection, not compiler magic.

State may carry two orthogonal standard axes:

- lifecycle: absent, created, active, stopping, stopped, reaped;
- completion: proposed, accepted, logical, physical, failed.

They are library types, not baked-in keywords.

## 7. Transition definitions

The canonical relation surface makes the semantic shape explicit:

```dtessl
transition Dispatch:
  <{SchedulerIdle @ scheduler, SessionActive @ session},
   {SchedulerBusy @ scheduler, SessionActive @ session}> [label=active]:
    where:
      eligible
    set @ scheduler:
      ready = erase(before.scheduler.ready, task.id)
    do:
      accept: $ipc.accept(task.id) @ scheduler
  | <{SchedulerIdle @ scheduler, SessionExpired @ session},
     {SchedulerIdle @ scheduler, SessionExpired @ session}> [label=expired]
```

This is an intensional typed relation over
`<BeforeEmbedding,AfterEmbedding,Bindings>`. Repeated branches are unioned;
the runtime still requires one selected executable witness unless an explicit
optimizer resolves candidates. `do` is a function from that witness to an
ActionPlan and is deliberately outside relation satisfaction. The legacy
`case (...) -> (...)` surface below lowers to exactly the same branch object.

```text
transition Assign @ Submit(task: Task):
  case active (SchedulerIdle @ scheduler, SessionActive @ session)
    -> (SchedulerBusy @ scheduler, SessionActive @ session):
    where:
      E worker in scheduler.workers: eligible(worker, task)
      and select worker in scheduler.workers
            where eligible(worker, task)
            by lex(load(worker), worker.id)
    set @ scheduler:
      ready = erase(before.scheduler.ready, task.id)
      running = insert(before.scheduler.running, task.id)
    do:
      accept: $ipc.accept(task.id),
      (reserve: $provider.reserve(task.id) @ worker
       | audit: $audit.append(task.id) @ session)

  case expired (SchedulerIdle @ scheduler, SessionExpired @ session)
    -> (SchedulerIdle @ scheduler, SessionExpired @ session):
    do:
      reject: $ipc.reject(task.id) @ session
```

- `@ Submit(...)` is the explicit event/context trigger; there is no separate
  `on` keyword.
- Every `case (source-set) -> (target-set)` is one path. Items inside a set are
  conjunctive; repeated `case` paths are alternatives.
- An optional case name yields the stable qualified identity
  `Transition.caseName`; native trace selectors and count claims may reference
  either that exact path or the transition as a whole.
- Source items accept exact, finite-set and `_` wildcard patterns. Targets are
  exact so executable lowering never invents nondeterministic choice.
- `case` is static topology matching. Its path-local `where` is the dynamic,
  typed value/relation guard; path-local `set` and `do` cannot leak into a
  different alternative.
- Target assignments are evaluated against one immutable before snapshot and
  the selected bindings, then committed atomically.
- A target state set may be selectable only through the same deterministic
  selection rules as relation search.
- A transition that produces multiple unresolved decisions is invalid in
  runtime mode.
- `ensure:` is a path-local temporal obligation activated on the selected
  successor. It is checked by the Solver/monitor product and never used as an
  oracle-like runtime guard. `where` remains the only current-Embedding
  enablement predicate.

The remaining State/Transition operator gaps are explicit rather than hidden
in the runtime:

- State still lacks reusable parameterized schemas, general derived fields,
  explicit history-state operators and dynamically created instance sets.
- Shared mutable state has no declared consistency/merge profile beyond the
  current equal/union merge rules.
- Transition now has a first-class before/after branch surface and reusable
  pure matcher templates. The Transition alone owns successor construction,
  updates, temporal obligations and ActionPlan lowering. Named
  `compose/product/project/hide/rename/refine` over Transition families remain
  absent; an executable transition is not an ordinary finite RelationValue.
- Source patterns do not yet bind arbitrary nested record/variant payloads;
  such destructuring currently belongs to an ordinary typed `match` in the
  guard.
- Priority, fairness and nondeterministic choice remain spec/Scheduler policy,
  not implicit runtime tie breakers.
- Message delivery, retries, compensation and physical completion remain
  explicit state/event/typed-port protocols; there is no ambient operator that
  pretends a host effect completed.

## 8. Guard, predicate, requires and scheduler

These concepts are related but not duplicate syntax:

- **Predicate**: any pure boolean relation over typed values.
- **Guard**: the closed predicate obtained after resolving `where` bindings for
  one candidate transition. It decides LTS enablement.
- **Search**: evaluation of a predicate with explicitly bound finite domains.
- **Requires relation**: edges between action nodes. It is derived from `,`, `|`
  and parentheses, then can be queried as `requires(after, before)` by a
  scheduler model.
- **Scheduler policy**: a separate model that selects among currently ready DAG
  nodes using resource facts and deterministic scoring. It cannot rewrite the
  logical transition that produced the DAG.

Thus a complex `where` may itself compile to an internal search DAG, while the
`do` block compiles to an effect DAG. They are different typed graphs and never
share node kinds accidentally.

### One Property core, distinct failure behavior

`where`, state `invariant`, transition `ensure` and named `Claim` are not four
predicate languages. Their ordinary atoms share `Expr/RelationMatch`; temporal
structure shares `TemporalExpr/ClaimMonitor`. Each typed Property is paired
with a stable semantic `PropertyUse`:

```text
PropertyUse {
  scope:    State | Transition | Trace | Procedure | Relation
  trigger:  Candidate | Successor | Occurrence | Target
  failure:  Disable | Reject | Violation | Counterexample
}
```

The evaluator produces only `Satisfied | Violated | Pending`. The declared
failure behavior then gives the operational disposition:

| Surface use | Trigger | False disposition |
| --- | --- | --- |
| transition `where` | candidate search | disable this candidate and continue searching |
| state `invariant` | initial/successor Embedding | reject the state commit |
| transition `ensure` | selected occurrence | record an activated obligation violation |
| named `Claim` | selected state/procedure/trace/relation target | return a counterexample |

`Pending` always defers; it is never silently converted to success. A Solver
may report a path containing an `ensure` violation as a counterexample to the
verified model, but that does not change `ensure` into an execution guard.
Thus the formula infrastructure is shared while the consequences remain
explicit and auditable.

## 9. External calls and native replay

`do` may contain only pure local calculations and typed `$` calls. Each call
record contains:

- stable call-site ID and label;
- typed interface and operation ID;
- canonical arguments;
- explicit logical context (never authority or a host Binding);
- dependency edges;
- discrete round, causal predecessor set and trace correlation.

DTESSL verifies declared typed ports and emits calls as an ActionPlan. It owns
no host handles, leases, credentials, provider objects, physical scheduling or
completion facts. Native replay consumes only `Program + typed EventTrace` and
recomputes logical results; runtime journals, snapshots and receipts are not
DTESSL inputs.

A named `do` is a reusable typed ActionPlan template, separate from pure
relations and from TransitionRelation successor construction:

```dtessl
do StartTask(task: TaskId, operation: OperationId) @ worker [context=fixed, idempotent_by=(<task, operation>), result=consistent_by(<task, operation>), delivery=at_least_once, ordering=ordered_by(task), retry=safe, replay=suppress]:
  invoke: $worker.start(task, operation)

transition Dispatch @ Submit(task: TaskId, operation: OperationId):
  <Idle @ scheduler, Busy @ scheduler> + StartTask(task, operation)
```

The contract vocabulary is closed and typed: `context=fixed|inherited`,
`idempotent_by=(expr)`, `result=opaque|consistent_by(expr)|nondeterministic`,
`delivery=at_most_once|at_least_once`,
`ordering=unordered|ordered_by(expr)`,
`retry=forbidden|safe|reconcile`, and `replay=suppress|reinject`.
Keys are evaluated from typed parameters into canonical Values. Safe retry and
at-least-once delivery require an idempotency key. Nondeterministic results
require reinjection during replay and are structurally unavailable to the
current transition successor; a later typed input must carry the recorded
outcome. Reinjection never re-executes the port. These declarations are host
obligations, not authority grants or evidence that a provider actually met
them.

Named-do definitions cannot call other named-do definitions in v0.4.8. Compose
them in the transition action expression instead. This keeps exactly one
effect contract attached to each physical typed port call and avoids implicit
contract override or stacking.

### Procedure entry, native trace and claims

- `procedure P @ context: initial (...)` declares one persistent automaton
  instance entry: an initial context identity and finite initial state
  combination. It may contain anonymous `(source-set)->(target-set)` transition
  declarations. They are scoped automaton edges with stable generated typed
  identities, not ordered steps, replay data, capture rules or effects.
- A procedure is not a trace. The language RuntimeContext owns persistent
  procedure instances, injection history, state, local revisions and causal
  frontier.
- `trace T @ root: replay:` primarily lists
  `inject Transition(...) @ Procedure` typed transition occurrences. The
  runtime uses the TransitionId and active-state signature indexes, then
  evaluates the indexed paths' dynamic `where` predicates. An optional
  `-> Transition.case` asserts the path derived by that search; it never
  commands a jump. Legacy `Transition.case(...) @ Procedure` remains accepted
  as an equivalent search assertion during v0 migration.
- One replay source line is one 1-based discrete round and `|` joins
  simultaneous transition occurrences.
- RoundId is a causal-DAG layer shared by all simultaneous occurrences, never a
  per-transition or per-procedure increment. Procedure revision is separate;
  stable occurrence identity plus predecessor edges carry happens-before.
- Every accepted decision owns an immutable `OccurrenceInput` in that RoundId:
  admission kind (open Event or exact TransitionId), requested symbol,
  normalized Event identity, all typed fields, target procedure and its entry
  context. The same decision stores before/after active states and typed values
  plus the resolved ActionPlan. This is primary trace evidence, not something
  reconstructed only from a side artifact.
- A procedure's immutable RoundId frame repeats its before/after Embedding and
  admitted input list so procedure-scoped inspection is complete; idle frames
  have equal before/after values and an empty input list.
  `captured_round_at` and `captured_procedure_at` provide exact RoundId access.
- `$port(...) @ context` is an outbound command description. Its evaluated
  arguments and resolved context live in the ActionPlan; it is never an ambient
  read from host variables or memory. Host input must cross the typed occurrence
  boundary by value. Live pointers are forbidden; future large-memory input
  requires an immutable opaque reference plus content/range evidence.
- Capture lowers to state/transition/procedure seed relations plus an optional
  temporal rule. Closed capture selects matching occurrences, recursively
  retains their explicit causal predecessors by `OccurrenceId`, and emits a
  generic replayable `TraceArtifact`. Procedure-labelled occurrences may also
  be projected as `ProcedureArtifact`; that projection does not own closure.
  Free-floating occurrences require no synthetic procedure.
- Capture selection is a typed relation match with a distinct disposition, not
  a parallel string-filter subsystem. Its subject is a State Embedding,
  Transition occurrence or Procedure instance. A match marks the occurrence,
  expands its actual occurrence/causal closure and emits it to the trace; a
  miss is ignored. The ordinary Transition disposition commits an Embedding,
  while Claim/Property dispositions report evidence or failure. All three
  reuse the same typed relation and structural matching foundation.
- A postfix `[capture=Trace/Session | Other/default]` on `state`, `transition`
  or `procedure` contributes distributed seeds to the corresponding trace
  instance. `default` maps to the bare trace name; other sessions use the
  stable `Trace/Session` identity. Multiple annotations aggregate by that
  identity. State seeds observe both entry and exit, Transition family seeds
  include named cases, and seed kinds are OR alternatives rather than an
  accidental AND filter.
- A distributed-only trace instance is `projected` and non-replayable. An
  explicit `trace` may supply the centralized `closed/projected` mode and its
  filters; a non-default session clones that explicit template before merging
  its distributed seeds. Only explicit `capture closed` may produce a
  replayable closure.
- A trace may put `capture closed/projected:` after `replay:` to declare both a
  replay input and a capture projection; the section order is semantic and
  cannot be reversed.
- A capture may add exactly one `eventually state(S @ context)` or
  `eventually transition(T.case)` target after at least one seed. Each matched
  seed occurrence opens an interval. The first same-or-later RoundId target is
  its witness; an open suffix is `pending`, and closing it without a witness
  makes the interval `unresolved` rather than silently proving it.
- `trace T @ root: capture closed:` retains every native Engine round while
  projecting state to the declared `state @ context` axes.
- `capture projected` retains only decisions touching those axes, records a
  causal gap, sets `replayable=false` and emits no trace artifact. It is
  useful for inspection, not conclusive global assurance.
- `Claim C @ trace T`, `Claim C @ state S`, `Claim C @ procedure P` and
  `Claim C @ relation R` distinguish finite evidence, a local declaration
  check, path exploration and a named derived-relation property.
  An optional `@ (context, ...)` records the explicit logical scope.
- The core temporal basis is `always`, `eventually`, `until`, `within` and
  `since`. `never`, `before` and `weak_until` are standard frontend definitions
  lowered into that basis; ordinary typed `function` continues to define the
  predicate leaves. `count Transition <= N` lowers to the same monitor product.
- Trace ordering uses the same relational surface. In particular,
  `<first, second> ~ happens_before` is a typed `TraceRelationMatch`, and
  `before(first, second)` is compatibility sugar for that node. Temporal
  relations may nest inside temporal formulas. Finite trace evaluation supports
  arbitrary accepted nesting; bounded/unbounded Solver fragments must return
  `inconclusive` for a nesting they cannot compile, never optimistic success.
  A finite open trace returns `pending` unless it
  already contains a decisive counterexample or witness. A closed trace can
  return `satisfied` or `violated`; positive satisfaction is downgraded to
  `pending` when projection gaps exist.
- Dynamic capture consumes only DTESSL-native typed Engine steps. It is not a
  syslog, audit-log, receipt or chenRT journal adapter.

## 10. Time, simultaneity and causality

DTESSL does not increment a global logical clock once per transition. That would
invent an order between independent transitions.

- A simulator **round** is an atomic batch over one before snapshot.
- Events in the same bag are simultaneous at the core level.
- Disjoint/confluent transitions may commit in the same round.
- Conflicting writes require an explicit collision/merge relation; source order
  is never an implicit tie-breaker.
- Core merge relations must be typed, associative, commutative and deterministic.
  The foundation provides `equal` and grow-only set `union`; later user-defined
  merges require a verifier proof/profile rather than an arbitrary callback.
- `happensBefore(a,b)` is a partial order derived from local order, message
  send/receive, data dependencies and action DAG edges.
- A scalar round is useful for deterministic scheduling and trace grouping but
  does not prove causality between members of the same round.
- Vector clocks and other causal profiles are typed standard-library values for
  distributed models, not a mandatory timestamp on every local model.
- Model time is an explicit domain value. Timers remain host commands followed
  by typed firing events; idle wall-clock passage cannot mutate model state.

This follows the distinction between causal partial order and clock-based total
order, and the Parallel DEVS treatment of simultaneous events and transition
collisions. Sources and project inferences are recorded in `REFERENCES.md`.

## 11. Distribution, resources, security and memory

These are standard dialects built on core values and transitions:

- **Time (P1):** discrete instants, clock vectors, guards, resets, deadlines and
  state invariants. Runtime timers are `$timer.arm(...)` plus `TimerFired`
  events.
- **Distribution (P1/spec):** delivery, duplicate, reorder, loss, partition,
  crash/recovery, leader term and quorum are explicit provider facts or spec
  schedules.
- **Resources (P1):** linear/affine leases, credits, capacity bags,
  conservation invariants and backpressure.
- **Security (P1/P2):** `actsFor`, delegation, attenuation, rights lattice,
  confidentiality/integrity labels, explicit declassification and
  noninterference claims. A capability name is never authority.
- **Memory (P1/P2):** `RegionRef`, ownership, borrow/share/disjoint/range,
  separation-like predicates, atomic/happens-before and named consistency
  profiles. Raw addresses are not values.

The C++ reference runtime may use its own bounded arenas, slabs and object pools
for parser/search/trace performance. These allocators are implementation
details, are reset at explicit phase boundaries, expose high-water/budget
metrics and never become language-visible raw pointers. `v0.0.1` includes the
first bounded `ScratchPool` for trivially destructible temporary objects.

## 12. Claims and compositional automata

Claims do not mutate state. Implemented v0.4.0 examples are:

```dtessl
Claim Safe @ procedure Session @ (security, workflow):
  always(security.credits >= 0)

Claim Completes @ procedure Session:
  within(8, workflow.done)

Claim AuthorizedHistory @ trace Audit:
  always(since(security.active, security.accepted))
```

Predicate leaves reuse the state/transition expression, relation and finite
search engine. The frontend lowers temporal structure to a typed AST; the
Solver compiles the supported deterministic fragment to `ClaimMonitor`, then
explores `EmbeddingExpand × ClaimMonitor`. A `since` subformula stores its recurrence
bit, not a copy of the history. `eventually` and strong `until` use waiting
deadlock/cycle detection; `within` uses a bounded counter. Unsupported nesting
returns `inconclusive`, never an optimistic proof.

Product, synchronized event, projection, hiding, renaming, assume/guarantee and
refinement mapping beyond this Claim product remain later compositional
automata work. Unknown/unchecked is distinct from true.

## 13. Backend and provider boundary

DTESSL remains backend-neutral through three typed boundaries:

1. Frontend produces a versioned `CanonicalModule`.
2. A backend consumes that module for a specific projection: reference
   interpreter, native engine, VM lowering, monitor, explorer or formal export.
3. An external host adapter may consume typed `ActionPlan` calls. No provider
   result or receipt enters DTESSL through this boundary.

Backend registration uses typed IDs and declared feature/capability sets, not a
generic string operation bus. A backend must reject unsupported AST features
before execution. VM integration is therefore an adapter alongside other
providers; DTESSL never grows a universal bytecode to accommodate every VM.
The public provider API is introduced only after the canonical AST exists, so
the current private parser tree is not accidentally frozen as an ABI.

## 14. Canonical core and digest boundary

Surface indentation, aliases, comments and formatter choices are not semantic.
The compiler resolves them into a typed canonical AST with explicit:

- fully qualified declaration and type IDs;
- sorted unordered fields and finite collections;
- exact numeric representation;
- de-sugared quantifiers and state updates;
- action/search graph nodes and canonical topological IDs;
- explicit effect, budget and semantic-mode annotations.

Action sequence and graph topology remain semantic. Record field spelling,
resolved type identity and literal bytes remain semantic. Editorial `#` tags
are kept in a source digest but excluded from the executable digest. Contract,
security and assurance annotations are typed AST nodes and included.

## 15. Totality, budgets and hostile inputs

- Runtime expressions must be total or return typed `result` values.
- Recursion requires a structural termination proof or explicit gas bound.
- All search, closure, matrix, trace and exploration operations have declared
  finite budgets.
- Parser and decoder limits cover nesting, token count, identifier/string size,
  collection cardinality and graph fan-out.
- Canonical integer/rational operations reject overflow or use declared
  arbitrary precision; implicit platform arithmetic is forbidden.
- Malformed source, ambiguous choices, stale logical references and typed-port mismatches
  fail before state commit.

## 16. Feature placement

| Placement | Contents |
| --- | --- |
| P0 core | Types, state/transition, finite collections/relations, deterministic search, action DAG, Binding skeleton, claims, canonical AST, budgets, trace/replay contracts |
| P1 standard library/dialects | Matrix algebra, time, workflow, distributed faults, security lattice, resource/lease models, memory model, monitor combinators |
| P2 external integrations | SMT/model checker, TLA+/timed-automata/memory-logic exporters, theorem evidence, platform host adapters, performance-specialized solvers |

This split prevents DTESSL from becoming a general-purpose language or a
single oversized VM instruction set.
