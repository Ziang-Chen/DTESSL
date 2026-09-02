# Core Logic Surface

Status: `v0.4.4` typed RelationValue, RelationPlan and TransitionRelation surface.

This profile separates source identifiers, logical names and human text, and
gives relations and optional values compact syntax without changing their
deterministic runtime model.

## Logical names

```dtessl
name WorkerId
name TaskId
```

`WorkerId(a)` is a first-class logical name. Its canonical value contains the
nominal type identity `WorkerId` and the atom `a`; it is not represented as a
string and it grants no authority. Atoms use the identifier grammar
`[A-Za-z_][A-Za-z0-9_]*`.

`WorkerId(a)` and `TaskId(a)` are different values. `"a"` remains human text.
A bare `a` in an expression remains a variable reference, never an implicit
name literal. Name construction is intentionally static; dynamic typed names
arrive through typed state/event values rather than string conversion.

## Relations

The core relation form is:

```dtessl
subject ~ relation-expression
```

The subject may recursively contain logical names, typed values, embedding
value projections, tuples, records and variants. Boolean relation predicates
compose with `and`/`or`; a comma at predicate-chain level is compact `and`:

```dtessl
<stateA, stateB> ~ Equal and
  (<stateA, stateB> ~ SameEpoch or <stateA, stateB> ~ Migratable)
```

Inside `<...>` or `{...}`, the comma is only an element separator. The outer
container assigns product or union semantics, so it cannot be confused with
predicate-chain conjunction.

`~` never starts an implicit search; an unbound subject is legal only under an
explicit `E`, `A`, `select`, or finite transition-parameter generator.

Anonymous relation containers are structural rather than hidden boolean syntax:

```dtessl
<a ~ P1, b ~ P2>   // ordered/product relation pattern
{a ~ P1, b ~ P2}   // unordered union of two singleton relation patterns
```

Thus `{a ~ P1, b ~ P2}` is canonically
`{a ~ P1} union {b ~ P2}`. It does not mean `P1(a) or P2(b)`, and it is not a
tuple product. Nesting preserves the same distinction.

In a derived set, each `name:` starts an independent anonymous branch. Filters
inside that branch are full expressions and may be grouped explicitly:

```dtessl
{a: a in A, (a ~ P1 or a ~ P3) and not (a ~ Rejected),
 b: b in B, b ~ P2}
```

Within a transition, this is the relational half of a small matching DSL. A
case is normalized to `StructuralPattern and RelationMatch`: the recursive
state pattern checks which control locations are active, while `~` checks typed
values projected from that embedding. Control-state names are deliberately not
coerced into strings or ordinary relation rows.

An ordered compact rewrite `<a,b> -> <a',b'>` is normalized as an intensional
`relation <Embedding,Embedding>`. The left product is conjunctive matching; the
right product constructs one atomic successor from the same before snapshot.
If an intermediate state must be visible, write two transitions or explicitly
compose their relations rather than relying on assignment order.

A direct unary relation binds its element rather than a one-field tuple. This
makes records the normal row schema:

```dtessl
record Worker:
  id: WorkerId
  capacity: int

workers: relation Worker = {
  Worker{id: WorkerId(a), capacity: 2},
  Worker{id: WorkerId(b), capacity: 1}
}
```

A named rule and a named derived plan are distinct declarations:

```dtessl
relation Edge(a: Node, b: Node):
  <a,b> in edges

relation Reachable: relation <Node, Node> = closure(Edge)
```

The first is a membership predicate that may be decidable without being
enumerable. The second is an explicitly typed, enumerable algebra plan. Both
have stable names and one dependency DAG, but neither is a mutable global table.

Quantification and deterministic selection use the same relation binding:

```dtessl
E worker ~ workers: worker.capacity > 0

select worker ~ workers
where worker.capacity >= minimum
by lex(worker.capacity, worker.id)
```

`~` is not a type constructor, approximate equality or bitwise negation. The
prefix `~T`/`~{...}` forms remain accepted only for v0 source migration.
`relation T`, `relation <A,B>` and context-typed `{...}` are the canonical
declaration/literal forms. Legacy `relation (A,B)` is migration input;
`relation <T>` retains the old unary Product-row binding so existing `.0`
programs do not silently change meaning.

## Transition relations

The canonical executable branch is one ordered before/after Product:

```dtessl
transition Dispatch:
  <{Idle @ worker, Open @ session},
   {Busy @ worker, Closed @ session}> [label=accept, where=(eligible)]
  | <{Busy @ worker, Closed @ session},
     {Idle @ worker, Open @ session}> [label=release]
```

The outer `<before-set,after-set>` is ordered. Each inner `{...}` is an
unordered conjunctive Embedding configuration. `|` is set union over
intensional TransitionRelation branches, not ActionPlan parallelism. A branch
is normalized to the same internal route used by the older `case` surface:

```text
TransitionBranch(before, after, bindings)
  := source-pattern(before)
     and where(before, bindings)
     and after = atomic-update(before, bindings)
     and target-pattern(after)
```

`set` contributes the atomic successor function. `do` remains attached
`<before,after,bindings> -> ActionPlan` lowering and is evaluated only after a
unique executable witness is selected. It never changes relation
satisfaction. `[label=...]`, `[where=(...)]`, and `[do=(...)]` are compact
branch extensions; their block forms use the same verifier, runtime, Solver,
trace identity and replay path.

## Optional values

```dtessl
[T]     // zero or one T
[]      // absent
[value] // present
```

`select worker ~ workers ...` returns `[Worker]`. An empty literal requires an
expected `[T]` type; standalone untyped `[]` is rejected.

Pattern matching uses the same shapes:

```dtessl
match chosen {
  [] -> false,
  [worker] -> worker.capacity > 0
}
```

The legacy `option<T>`, `none`, `some(value)` forms remain accepted during the
v0 migration. Runtime and canonical values still have explicit option type
identity; brackets are not erased into an untyped null.

Because brackets now belong to optional cardinality, list values are rendered
and should be written explicitly:

```dtessl
queue: list<TaskId> = list[TaskId(a), TaskId(b)]
```

The previous bare list literal is accepted only as v0 compatibility syntax.

## Determinism

Name order is bytewise atom order within one nominal name type and is permitted
as a `lex` score. Different name types cannot be compared. A `select` still
rejects distinct rows with an equal complete score; neither source order nor
hash iteration is a hidden tie breaker.

Multiline values inside `()`, `[]` and `{}` are layout-insensitive. Outside
delimiters, indentation continues to define DTESSL blocks.
