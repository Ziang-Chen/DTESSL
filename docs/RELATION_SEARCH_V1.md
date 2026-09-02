# Relation and Search Profile v1

This profile defines the deterministic runtime subset introduced by DTESSL
`v0.2.0`. It is a finite relational model, not a generic database query engine
and not a source of runtime nondeterminism.

DTESSL `v0.2.3` adds the compact `~` surface and direct unary-record binding.
The bounds and deterministic search semantics in this profile are unchanged.
DTESSL `v0.4.0` supersedes only that surface: canonical source writes
`relation T` / `relation <A,B>` and reserves infix `~` for recursive
`subject ~ relation-expression`. Prefix `~T` and `~{...}` remain migration
input; the finite relation algebra and budgets below are unchanged.

DTESSL `v0.4.2` adds lazy typed relation plans. General and compact
declarations lower to one AST:

```dtessl
relation Eligible(worker: Worker, task: Task):
  worker in Workers,
  task in Tasks,
  worker ~ Active,
  <worker, task> ~ CapabilityMatch

relation <worker: Worker, task: Task> ~ Eligible:
  worker in Workers, task in Tasks, worker ~ Active, <worker, task> ~ CapabilityMatch;
```

Membership runs the rule directly. Consumers that need rows enumerate only on
demand, and require every parameter to have a collection generator or static
finite type domain.

DTESSL `v0.4.3` adds named derived plans:

```dtessl
relation Reachable: relation <Node, Node> = closure(Edges)
relation Carrier: relation Node = domain(Reachable)
```

The declared result type is checked exactly. Dependencies form a static DAG;
cycles are rejected unless recursion is made finite and explicit through an
operator such as `closure`. A plan may observe typed fields in the current
Embedding, but it is not a mutable relation table or a first-class closure.

## Values and bounds

- `<T,...>` is the canonical positive-arity structural product type and
  `<value,...>` its value/pattern container. The old tuple spellings remain
  migration input.
- `relation <T...>` is a finite set of Product rows with exactly the declared arity
  and column types.
- `relation T` is a direct unary relation whose binder is `T`;
  `relation <A,B,...>` is the Product-row spelling, and `relation{...}` is
  the canonical rendered literal form. Parenthesized relation types and prefix
  `~T`/`~{...}` are migration input.
- Rows use the canonical total value order and contain no duplicates.
- Arity is at most 64, a materialized relation contains at most 4096 distinct
  rows, and one relation/search plan performs at most 1,000,000 inspected work
  items.
- The limits are public constants and part of backend negotiation. A backend
  may impose a smaller declared run budget, but cannot silently produce a
  different accepted result.

## Algebra

The following operations are total within their verified type and runtime
budgets:

- `project(r, i...)` keeps the listed distinct literal columns in that order
  and removes duplicate result rows.
- `join(l, li, r, ri)` is an equijoin on compatible literal columns. Its output
  concatenates the complete left and right tuples, including both join columns.
- `compose(l, r)` accepts compatible binary relations `(A,B)` and `(B,C)` and
  returns `(A,C)`.
- `inverse(r)` swaps the columns of a binary relation.
- `closure(r)` returns the non-reflexive transitive closure of a homogeneous
  binary relation. Reflexive pairs occur only when implied by an actual cycle.
- `union`, `intersection` and `difference` require identical relation types.
- `domain(r)` and `range(r)` return canonical direct unary relations for the
  first and second column of a binary relation.
- `product(l,r)` returns the Cartesian product with concatenated row columns.
- `identity(xs)` turns a finite set or unary relation into `<x,x>` rows.
- `image(r,xs)` and `preimage(r,ys)` project reachable codomain/domain values.
- `reflexive_closure(r)` adds identity rows for the carrier occurring in a
  homogeneous binary relation. Use `union(r, identity(domain))` when isolated
  carrier members must also be included.

The following finite properties return `bool` and share the ordinary Property
path used by guards, invariants, ensures and Claims:

```text
subset       disjoint       functional    injective
reflexive    irreflexive    symmetric     antisymmetric
transitive   acyclic        equivalence   partial_order
left_total   surjective     bijective     total_order
```

`reflexive`, `equivalence`, `partial_order` and `total_order` take an explicit
finite carrier as their second argument. `left_total` and `surjective` take the
relevant domain/codomain carrier; `bijective` takes both. Explicit carriers
prevent an isolated member from disappearing merely because it does not occur
in a relation row. Carrier-relative algebra also rejects rows outside the
declared carrier; a law cannot become true by silently treating those rows as
part of a larger universe.

Operands may be materialized relation values, lazy relation comprehensions,
finite declared relations, or results of other algebra operations. A declared
relation never has to be rewritten as a set/relation literal first.

## Relation Claims and higher-order values

```dtessl
Claim ReachIsAcyclic @ relation Reachable:
  always:
    acyclic(Reachable)
```

The relation target is checked during verification and gives the Property a
stable scope. Its atoms still use the single typed evaluator; the Solver checks
the property at each reachable Embedding and returns an ordinary Product
counterexample when a transition changes a dependency of the plan.

`RelationValue<Row>` is recursively canonical: a row column may itself contain
a finite RelationValue, and codec/digest/equality recurse through it. This is
the supported higher-order data boundary. `RelationPlan<Row>` is intensional
and may capture the current Embedding, so it cannot be stored as a row, passed
as a closure, or compared by source identity. Using a named plan where a value
is required explicitly materializes its finite result under the public budgets.

## Lazy comprehensions and pattern containers

```dtessl
{x : x in A, x ~ P}
{x: x in A, x ~ P, y: y in B, y ~ Q}
{x: x in A, (x ~ P or x ~ Q) and not (x ~ Rejected)}
{<x,y> : x in A, y in B, <x,y> ~ R}
relation {<x,y> : x in A, y in B, <x,y> ~ R}
```

The second form is an unordered lazy union of two independently generated set
branches; it is not a Cartesian tuple. The third form explicitly requests the
product projection. Generators and filters remain lazy until a consumer pulls
rows; storing the result is an explicit materialization boundary. The unordered
union itself is a canonical finite merge barrier, so permuting its source
branches cannot change iteration order, a selected witness, or a digest.

The same rule applies to anonymous relation patterns:

```dtessl
{a ~ P1, b ~ P2} == union({a ~ P1}, {b ~ P2})
<a ~ P1, b ~ P2>  // ordered product, not the same relation
```

In particular, `{a: a in A, b: b in B}` is the union of the independently
derived `a` and `b` sets. Only `{<a,b>: a in A, b in B}` asks for Cartesian
enumeration and tuple projection. A branch filter is an ordinary typed logical
expression: parentheses determine grouping and `not`, `and`, `or`, and `->`
retain their normal precedence. The comprehension parser does not implement a
second, weaker filter language.

Compact Transition patterns use `<...>` for ordered products and `{...}` for
unordered canonical set-patterns. Containers may nest; leaf predicates are
conjoined and leaf updates lower to ordinary Transition assignments:

```dtessl
trans Step: <a ~ Pa, b ~ Pb> -> <a.value = 1, b.phase = 2>;
trans Scatter: {{a ~ Pa}, <b ~ Pb>} -> {{b.phase = 2}, <a.value = 1>};
```

The whole case is an intensional binary relation over Embeddings:

```text
Rcase(before, after) := ProductPattern(before)
                     and after = apply_atomic(before, ProductUpdate)
```

Thus `<a,b> -> <a',b'>` needs no new state-machine value kind: each side is an
existing typed product, while the arrow is the existing Transition relation.
The ordered RHS records a stable structural order, but every update reads the
same `before` snapshot and the successor commits atomically. A genuinely staged
`a-update then b-update` is `compose(Ta,Tb)` (or two explicit transition edges)
and therefore has an observable intermediate Embedding.

`<a,b> ~ (a ~ Pa, b ~ Pb) ~ Pab` lowers to
`Pa(a) and Pb(b) and Pab(<a,b>)`.

Column indices are compile-time non-negative integer literals. Invalid arity,
column or type combinations are verifier errors, not runtime string dispatch.

## Quantifiers

Both long and compact spellings lower to the same typed nodes:

```text
exists x in domain where predicate
E x in domain: predicate
A x in domain: predicate
E x ~ domain: predicate
A x ~ domain: predicate
```

Sets bind their element type. Relations bind one `tuple<T...>` row, addressed
as `row.0`, `row.1`, and so on. Enumeration is canonical. On an empty domain,
`E` is false and `A` is true.

The direct unary form `relation Worker` binds `Worker` itself. Such a relation
therefore exposes `worker.id`, while legacy `relation<Worker>` continues to
expose `row.0` during v0 migration. This compatibility distinction prevents an
existing program from silently changing meaning.

## Deterministic selection

```text
select row in candidates
  where eligible(row)
  by lex(score(row), row.0)
```

The expression returns `option<tuple<T...>>`:

- no eligible row returns `none`;
- one lowest score returns `some(row)`;
- if distinct rows have the same complete lexicographic score, evaluation
  rejects the candidate round as ambiguous.

The direct unary equivalent is:

```text
select worker ~ workers
  where worker.capacity >= minimum
  by lex(worker.capacity, worker.id)
```

For `workers: ~Worker`, it returns `[Worker]`: `[]` for no candidate and
`[worker]` for the unique minimum. These are typed option values, not an
untyped null or list.

There is no source-order, hash-order or random tie breaker. A model that wants a
total choice must include a stable unique component in `lex`, normally the
logical identifier. `choose` remains spec-only and is not accepted here.

## Plans and failure atomicity

`search_plans(program)` exposes typed summaries for quantifiers, selection and
relation algebra: operation name, row/work bounds, determinism and ambiguity
policy. It does not expose the private parser AST.

The CLI renders the same data with `dtessl plans model.dtessl`, allowing a host
or backend test to inspect budgets and ambiguity policy before execution.

Row, work, ambiguity and type failures occur while evaluating the immutable
before snapshot. The engine does not advance the round, mutate state or emit an
external action plan on failure.
