# DTESSL Language Design

Status labels used below:

- **Implemented**: accepted and executed through the `v0.3.0` state/trace slice.
- **P0**: required for the complete executable modeling core.
- **P1**: standard library or standard dialect built on the core.
- **P2**: external solver, exporter or advanced assurance integration.

## 1. Semantic center

### Golden procedure/replay/capture principles

These rules override any earlier v0 experimental syntax or implementation:

1. A `procedure` is one persistent automaton instance built from DTESSL
   `state` and `transition` definitions. It is not a workflow script, trace or
   transition list.
2. Replay replays a procedure: its initial configuration, typed context
   injections and resulting logical history. Transition occurrences and the
   dynamic DAG are recomputed evidence, never authoritative replay input.
3. Capture either selects complete procedures or accepts a state/transition/
   procedure filter as a seed. The language must compute causal and data
   dependency closure and emit a complete replayable procedure artifact. A
   partial projection may not claim to be replayable.
4. A procedure begins with initial states plus initial typed context. When its
   automaton reaches quiescence, later typed context may be injected through a
   procedure-local admission rule. `when` matches the static state topology;
   `where` checks dynamic values and relations. Injection does not directly
   mutate state or choose a transition.
5. The language RuntimeContext owns procedure instances, state, revisions,
   pending injections and causal frontiers. Different procedures are isolated
   at `ProcedureId / Context / StateField`; safe shared storage is a later,
   explicit distributed-state model.

The core runtime closure is therefore:

```text
inject typed context
  -> search enabled transitions inside each procedure
  -> choose a deterministic conflict-free set
  -> commit one causal-DAG round
  -> repeat until quiescent
  -> await the next admitted context injection
```

All transition nodes in one DAG layer share a `RoundId`. Procedure revision is
only a local state version. Runtime traversal order cannot create logical time.

The implemented minimal injection surface is:

```dtessl
procedure Session @ system:
  initial (Idle @ workflow)

  inject Resume(session: SessionId):
    when (Waiting @ workflow)
    where:
      session = workflow.owner
```

`inject` declares what typed context may resume a quiescent procedure; it is not
an imperative step. Its parameters must exactly match an event schema used by a
global transition. `when` is topology and `where` is a typed dynamic admission
predicate. The admitted context is consumed as one event occurrence; the engine,
not the replay source, selects the unique transition path.

The v0.3 native closure is deliberately conservative: a selected state or path
first identifies its owning procedure instances, then capture retains each
whole procedure from declared initial state through every typed injection and
every global RoundId, including idle frames. This is wider than a minimal
backward slice but cannot omit a causal/data dependency. It emits a public
`ProcedureArtifact` that `replay_procedures` re-admits and compares twice.
`capture projected` emits no replay artifact and is explicitly not replayable.
Checkpoint starts, canonical artifact serialization and program digests extend
this contract later; they do not weaken the initial-state replay now implemented.

### Permanent system boundary (2026-08-28)

DTESSL is not a model extractor or universal log engine. It accepts DTESSL
programs and native typed event traces, then computes deterministic logical
state and ActionPlans. Existing systems may actively export a lossy model
projection with provenance, coverage and classified gaps; DTESSL never derives
one from implementation code or runtime logs. Runtime journals, snapshots,
receipts, effect suppression and reconcile are outside DTESSL. Generated mirrors
must remain distinguishable from independent assurance models for the lifetime
of the language.

DTESSL models one discrete simulation round as a relation over typed context
available to procedure instances:

```text
(RuntimeContextBefore, Bag<ContextInjection>)
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

### Structural words

- `state`: typed state space, initial data, derived views and invariants.
- `transition`: event-conditioned relation from source state set to target state
  set.
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
- direct unary `~T`, tuple relation `~(A,B,...)` and deterministic iteration.
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

The `v0.2.3` surface adds first-class canonical logical names, direct
unary-record relations, `~` relation matching and bracket option forms. Legacy
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

Current flat fields are retained as sugar for `data`. The target form is:

```text
state Scheduler @ fabric initial:
  data:
    mode: Mode = Idle
    ready: set<TaskId> = {}
    capacity: bag<Resource> = {}
    bindings: set<Binding<Provider>> = {}

  derive:
    available(t) = E p in bindings: satisfies(p, t.requirement)

  invariant:
    capacity >= 0
    disjoint(running, cancelled)
```

A state can therefore contain domain data, named resource values, derived
relations and invariants. Higher-order facts such as “transition T occurred N
times” are modeled over the explicit trace/history projection, not compiler
magic.

State may carry two orthogonal standard axes:

- lifecycle: absent, created, active, stopping, stopped, reaped;
- completion: proposed, accepted, logical, physical, failed.

They are library types, not baked-in keywords.

## 7. Transition definitions

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

### Procedure entry, native trace and claims

- `procedure P @ context: initial (...)` declares one persistent automaton
  instance entry: an initial context identity and finite initial state
  combination. Its `inject Event(fields...)` rules admit typed context with
  static `when` topology plus dynamic `where` predicates. It contains no
  transitions, replay, capture or ordered steps.
- A procedure is not a trace. The language RuntimeContext owns persistent
  procedure instances, injection history, state, local revisions and causal
  frontier.
- `trace T @ root: replay:` primarily lists `Event(...) @ Procedure` typed
  context injections. The engine checks the procedure admission rule and
  searches the global transition set. `Transition.case(...) @ Procedure` is
  also retained as search replay: it derives the same event input and asserts
  that the named path was selected. The assertion never commands a jump.
- One replay source line is one 1-based discrete round and `|` joins
  simultaneous transition occurrences.
- RoundId is a causal-DAG layer shared by all simultaneous occurrences, never a
  per-transition or per-procedure increment. Procedure revision is separate;
  stable occurrence identity plus predecessor edges carry happens-before.
- Capture lowers to `CaptureFilter{states, transitions, procedures}`. Closed
  capture treats this filter as a seed, identifies matching procedure
  instances and conservatively retains each complete procedure from initial
  state through all typed injections and RoundIds. It emits replayable
  `ProcedureArtifact` values. A captured procedure retains one immutable frame
  for every global RoundId, including idle frames, and can be queried by
  `(trace, procedure, RoundId)`.
- A trace may put `capture closed/projected:` after `replay:` to declare both a
  replay input and a capture projection; the section order is semantic and
  cannot be reversed.
- `trace T @ root: capture closed:` retains every native Engine round while
  projecting state to the declared `state @ context` axes.
- `capture projected` retains only decisions touching those axes, records a
  causal gap, sets `replayable=false` and emits no procedure artifact. It is
  useful for inspection, not conclusive global assurance.
- `Claim C @ T` currently supports `always`, `eventually` and
  `count Transition <= N`. A finite open trace returns `pending` unless it
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

Claims do not mutate state:

```text
claim SessionBound:
  A e in EventLog:
    has(e.sessionID) -> E s in Sessions: s.id = e.sessionID
```

The monitor/explorer layers support product, synchronized event, projection,
hiding, renaming, assume/guarantee, refinement mapping and monitor automata.
Runtime-checkable claims lower to monitors; proof-oriented claims lower to
bounded checks or external exporters. Unknown/unchecked is distinct from true.

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
