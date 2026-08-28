# Core Logic Surface

Status: implemented on the `v0.2.3` integration branch.

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

The compact relation forms are:

```dtessl
~T              // direct unary relation over T
~(A, B)         // relation over tuple<A, B>
~{...}          // finite relation literal
item ~ relation // typed relation membership
```

A direct unary relation binds its element rather than a one-field tuple. This
makes records the normal row schema:

```dtessl
record Worker:
  id: WorkerId
  capacity: int

workers: ~Worker = ~{
  Worker{id: WorkerId(a), capacity: 2},
  Worker{id: WorkerId(b), capacity: 1}
}
```

Quantification and deterministic selection use the same relation binding:

```dtessl
E worker ~ workers: worker.capacity > 0

select worker ~ workers
where worker.capacity >= minimum
by lex(worker.capacity, worker.id)
```

`~` is not approximate equality or bitwise negation. It has only the relation
meaning. The legacy `relation<T...>`, `relation{...}` and `in` spellings remain
accepted during the v0 migration; legacy unary relations continue to bind a
one-field tuple so existing `.0` programs do not silently change meaning.

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
