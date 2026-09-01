# State case axes, finite constraints, and deltas

## Stable distinction

`state` defines the carrier set of legal configurations. `transition` defines
an explicit guarded rewrite relation over that set. Declaring alternatives in
a state never infers edges between them.

```dtessl
state Scheduler @ session initial:
  retries: int32[0:1:2] = 0
  case Phase:
    Idle
    | Running(case Step: Prepare | Execute)
    | Done
```

- `case Phase` introduces one named choice axis.
- `|` denotes mutually exclusive alternatives on that axis.
- `(A, B)` denotes one composite alternative whose children coexist.
- A nested `case` is active only under its parent alternative for structural
  matching. Its slot may remain latent in the physical embedding; ancestor
  matching prevents it from becoming an active semantic state by accident.
- Optional anonymous `case:` receives a canonical structural identity. Name a
  case whenever transitions, traces, or diagnostics need a stable human path.

Transitions alone rewrite axes:

```dtessl
transition Advance():
  case execute
    (Scheduler(Phase.Running.Step.Prepare) @ session)
    ->
    (Scheduler(Phase.Running.Step.Execute) @ session):
    where:
      true
```

Unmentioned orthogonal axes and values are retained. A target nested below a
different parent also names the necessary ancestor targets, so the rewrite is
atomic. Source ordering and the order of state alternatives never imply time.
For a product branch, transition patterns use its structural members directly:

```dtessl
state Pair @ local initial:
  case Shape:
    (A, B) | C

transition Fold():
  case both (Pair(Shape.A, Shape.B) @ local) -> (Pair(Shape.C) @ local):
    where:
      true
```

`Shape.A` and `Shape.B` normalize to the same product alternative; they do not
create two runtime choice axes or two independent transitions.

## Recursive grammar

```text
state-decl   := "state" Name ["@" Name] ["initial"] ":" state-member*
state-member := field | invariant | case-decl | legacy-choice
case-decl    := "case" [Name] ":" case-expression
case-expression := branch ("|" branch)+
branch       := control | "(" control ("," control)+ ")"
control      := Name ["(" state-member ("," state-member)* ")"]
```

The compact form uses exactly the same parser and typed AST:

```dtessl
state Switch @ local = changes: int8[0:3] = 0,
  case Mode: Off | On(case Level: Low | High);
```

## Static constraint profile

Finite domains are the enumerable fragment of ordinary static constraints:

```dtessl
small: typetrait<int8> = 0
retry: int32[0:5:100] = 0
mode: Mode{Mode.idle, Mode.busy} = Mode.idle
worker: WorkerId{WorkerId(a), WorkerId(b)} = WorkerId(a)
```

`T[begin:end]` uses stride one; `T[begin:stride:end]` is inclusive. Initial
values and transition assignments must satisfy the same constraint admission
function. Solver generation enumerates only bounded finite profiles and then
uses the normal `where`, relation, and invariant evaluator to filter generated
candidates. Constraints over infinite domains may filter explicit candidates,
but cannot establish exhaustive verification without an explicit finite
generator; that result is `inconclusive`.

## Trace and search delta

Every logical round derives a canonical `EmbeddingDelta`:

```text
controls: path, before?, after?
values:   path, before?, after?
```

Paths are semantic schema paths, not `RawKeyMap` offsets. Both lists are
strictly ordered. Runtime decisions attach the parent/child embedding digests;
replay can start at a checkpoint, apply deltas, and verify each child digest.
Solver witness edges retain the same patch and validate it when inserting the
child into the content-addressed `EmbeddingStore`. Unchanged subtrees are not
represented in the delta.
