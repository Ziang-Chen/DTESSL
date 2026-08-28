# DTESSL Language Design

Status labels used below:

- **Implemented**: accepted and executed by the released `v0.2.1` slice.
- **P0**: required for the complete executable modeling core.
- **P1**: standard library or standard dialect built on the core.
- **P2**: external solver, exporter or advanced assurance integration.

## 1. Semantic center

### Permanent system boundary (2026-08-28)

DTESSL is not a model extractor or universal log engine. It accepts DTESSL
programs and native typed event traces, then computes deterministic logical
state and ActionPlans. Existing systems may actively export a lossy model
projection with provenance, coverage and classified gaps; DTESSL never derives
one from implementation code or runtime logs. Runtime journals, snapshots,
receipts, effect suppression and reconcile are outside DTESSL. Generated mirrors
must remain distinguishable from independent assurance models for the lifetime
of the language.

DTESSL models one discrete simulation round as a relation over an event bag:

```text
(BeforeState, Bag<Event>, ExplicitContext)
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
- `option<T>`, `result<T,E>`, `list<T>`, `set<T>`, `map<K,V>`, `bag<T>`.
- `relation<A,B,...>` with deterministic iteration.
- records, variants, enums and nominal `newtype`.
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

### Relation and search algebra

P0 operations include membership, union/intersection/difference, cartesian
product, select, project, join, inverse, relational composition and bounded
transitive closure. Quantifiers are written compactly:

```text
E x in xs: predicate(x)
A x in xs: predicate(x)
```

Long aliases `exists` and `all` may be accepted as surface sugar; canonical AST
uses `Exists` and `ForAll` nodes.

Deterministic choice is explicit:

```text
select worker in eligible
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
  from {Scheduler as before, Session[task.session] as session}

  to {Scheduler as after}:
    after.ready = erase(before.ready, task.id)
    after.running = insert(before.running, task.id)

  where:
    session.status = Active
    and E worker in before.workers: eligible(worker, task)
    and select worker in before.workers
          where eligible(worker, task)
          by lex(load(worker), worker.id)

  do:
    accept: $ipc.accept(task.id),
    (reserve: $provider.reserve(task.id) @ worker
     | audit: $audit.append(task.id) @ session)
```

- `@ Submit(...)` is the explicit event/context trigger; there is no separate
  `on` keyword.
- `from` and `to` are sets of typed state patterns. Multiple components may be
  independent or coupled by `where` predicates.
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
