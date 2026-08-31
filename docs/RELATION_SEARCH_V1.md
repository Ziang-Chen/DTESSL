# Relation and Search Profile v1

This profile defines the deterministic runtime subset introduced by DTESSL
`v0.2.0`. It is a finite relational model, not a generic database query engine
and not a source of runtime nondeterminism.

DTESSL `v0.2.3` adds the compact `~` surface and direct unary-record binding.
The bounds and deterministic search semantics in this profile are unchanged.
DTESSL `v0.4.0` supersedes only that surface: canonical source writes
`relation T` / `relation (A,B)` and reserves infix `~` for recursive
`subject ~ relation-expression`. Prefix `~T` and `~{...}` remain migration
input; the finite relation algebra and budgets below are unchanged.

## Values and bounds

- `tuple<T...>` is a positive-arity structural product.
- `relation<T...>` is a finite set of tuples with exactly the declared arity
  and column types.
- `relation T` is a direct unary relation whose binder is `T`;
  `relation (A,B,...)` is the tuple-relation spelling, and `relation{...}` is
  the canonical rendered literal form. Prefix `~T`/`~{...}` is migration input.
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
