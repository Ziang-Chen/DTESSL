# Solver semantic layer

`Solver` is DTESSL's high-performance semantic layer between the typed frontend
and execution, exploration or export backends:

```text
source -> frontend -> typed Program -> Solver -> backend
                                      |
                                      +-> EmbeddingExpand x ClaimMonitor
                                                   -> counterexample
```

The implementation follows the same stability boundary:

| File | Ownership | Expected change rate |
| --- | --- | --- |
| `frontend.cpp` | source AST, lexer, parser, typing/lowering orchestration | high |
| `semantics.cpp` | typed relations plus Property truth/failure disposition | low |
| `runtime.cpp` | Engine, procedure and replay orchestration | medium |
| `trace_semantics.cpp` | finite trace-position and temporal interpretation | low |
| `monitor_semantics.cpp` | formula normalization and incremental ClaimMonitor | low |
| `solver.cpp` | EmbeddingExpand × ClaimMonitor Product exploration | low |
| `backend.cpp` | feature discovery and projection negotiation | low |

The private typed AST is still shared by one translation unit, so these are
textual implementation fragments rather than independently compiled public
modules. The split is nevertheless semantic: parser changes must lower into
the stable operations instead of adding alternate evaluator paths.

Predicate evaluation has one typed route. Surface equality/order/membership
operators (`=`, `!=`, `<`, `<=`, `>`, `>=`, `in`, `~`) lower to the
`RelationMatch` AST family. Boolean nodes only compose those matches. Runtime
guards and invariants, finite trace checks, and ClaimMonitor atom evaluation all
call the same verifier and evaluator; EmbeddingExpand and ClaimMonitor do not
carry private comparison or membership implementations. Canonical ordering
used to index values and rank an explicit optimization score is an ordering
primitive, not a second predicate evaluator.

Every boolean or temporal use also enters one Property policy core. It separates
formula truth (`satisfied/violated/pending`) from operational disposition:
guards disable candidates, invariants reject successors, transition ensures
record violations, and Claims request counterexamples. `compile_property_monitor`
is shared directly by named Claims and transition ensures; an ensure is no
longer compiled by constructing a synthetic Claim declaration.

## Names and ownership

- A source `StateSchema` recursively composes product children, named or
  anonymous `case` choice axes, and typed value leaves. A `case` enumerates
  legal alternatives but creates no transition edge by itself.
- An `Embedding` is dynamic: the active location on each `@context` axis plus
  the current typed variable valuation.
- `EmbeddingExpand` is the reachable directed graph formed by expanding enabled
  transitions from Embeddings. Joins, back-edges and cycles are normal.
- A `ClaimMonitor` is the finite control state compiled from one typed temporal
  formula. `since` contributes recurrence bits, `within` contributes a bounded
  counter, and liveness operators contribute waiting/satisfied/rejected phases.
- The explored verification node is `(Embedding, ClaimMonitorState)`.
  EmbeddingExpand remains the base graph; history-sensitive monitor state is not
  smuggled into the source `state` declaration or Embedding digest.
- One `verify_claim` call owns one on-demand Product graph and one base
  EmbeddingStore. Exactly one executable Engine snapshot is retained per unique
  base Embedding. Product nodes contain only the Embedding index, monitor state,
  active obligations and witness edge; different monitor histories therefore
  do not copy the Engine. Separate `verify_claim` calls do not yet share a
  cross-claim cache.
- `RawKeyMap` maps recursive semantic control/value paths to offsets in the
  lowered raw embedding vector. It is representation metadata, not identity.
- `EmbeddingStore` canonically encodes and content-deduplicates nodes by exact
  raw bytes. Digest is evidence only and cannot merge nodes. Every witness edge
  stores a canonical `EmbeddingDelta`; insertion reapplies that patch to the
  parent and rejects it unless it reconstructs the exact child Embedding.
- A witness parent stored with an Embedding is only one discovery path used
  to print a counterexample. It does not turn EmbeddingExpand into a tree.

## Transition expansion

The verified Program owns stable context, state, transition and route IDs.
Production matching uses dense active-location signatures; the original string
and map matcher remains available as a semantic parity baseline. For each
Embedding the Solver:

1. finds candidate transition groups by TransitionId;
2. narrows routes by their source-context signature;
3. generates parameter values from enumerable static constraints, then
   evaluates dynamic `where` predicates and relation searches;
4. applies the unique route or explicit unique optimum to produce a successor;
5. inserts the successor Embedding into EmbeddingExpand by exact raw content.

The bounded explorer admits zero-parameter transitions and parameterized
transitions whose types have an enumerable static constraint: `bool`, a small
fixed-width `typetrait`, a payload-free enum trait, `T{...}`, or
`int[begin:stride:end]`. The enumeration profile is bounded by
`finite_domain_value_limit`. An unconstrained/infinite parameter, or a finite
domain above that operational limit, makes the result `inconclusive`, never a
proof. The generated values still pass through the same typed constraint,
`where`, relation, and invariant admission path as ordinary execution.
Likewise, a Claim, invariant or transition that observes `round` is
`inconclusive` until a finite logical-time component is explicitly included in
the product. The Solver never silently folds different times into one digest.

## Claim automata and results

The P0 temporal basis is `always`, `eventually`, `until`, `within` and `since`.
Ordinary predicate leaves use the same typed expression evaluator, quantifiers,
relations and pure functions as state invariants and transition `where` guards.
`never` and `weak_until` are frontend definitions lowered to that basis;
`before(p, q)` is compatibility sugar for `(p, q) ~ happens_before`. The
Solver has no duplicate search algorithm for these forms.

Trace relations form a distinct typed branch under the common relation model:
`(first, second) ~ happens_before` produces `TraceRelationMatch`, not an
instantaneous membership test. The monitor compiler canonically lowers it to
the temporal core. This keeps trace access out of the instantaneous evaluator
while preserving one syntax and one solver entrance. Unsupported nested future
fragments are explicitly `inconclusive`.

```text
never(p)          := always(not p)
before(p, q)      := until(not q, p and not q)  // strict; same-round is false
weak_until(p, q)  := until(p, q) or always(p)
```

They behave like a tiny typed temporal prelude. Ordinary `function` remains the
mechanism for naming pure state predicates; temporal formulas themselves are
not first-class runtime values.

- `always P` rejects the first reachable product state where `P` is false.
- `eventually P` rejects a waiting deadlock or a reachable waiting cycle.
- `P until Q` rejects `not P and not Q`, or a deadlock/cycle that postpones `Q`
  forever.
- `within(n, P)` uses a bounded monitor counter and rejects after `n`
  transition distances without `P`; distance zero includes the current state.
- `P since Q` uses `S' = Q or (P and S)` and therefore stores one bit per
  syntactic `since`, not the trace prefix.
- `count T <= n` is likewise a monitor counter in the Product key.

Trace claims use finite-trace semantics and include logical Round 0. Procedure
claims start from the procedure's declared initial Embedding and quantify
over all admitted zero-parameter transition paths. `Claim C @ state S` is a
local check and cannot discharge an unresolved future obligation.

Transition `ensure` clauses compile through the same ClaimMonitor builder. An
occurrence activates its obligation on the successor state. This is not a
runtime guard: execution never predicts a future state to decide whether a
transition is enabled. Repeated identical pending obligations coalesce to the
oldest (and therefore strongest) bounded deadline.

The Product graph is generated on demand. Immediate rejected states provide
finite counterexamples; waiting liveness states use cycle detection and return
a prefix plus a closing cycle edge. A result is `verified` only after complete
finite closure; `bounded-verified` means no counterexample was found within the
configured Product-state/depth bounds.

The CLI reports `embeddings`, `product`, and `snapshots` separately. It is
normal for `product > embeddings` when the same logical state is reached with
different monitor histories; the storage invariant is
`snapshots == embeddings`.

The currently compiled deterministic monitor fragment admits a future operator
whose operands are state/past predicates, with arbitrary boolean and `since`
nesting inside those operands. Unsupported future-under-future nesting returns
`inconclusive`; it is never approximated as true.

The Solver proposes logical decisions only. Runtime procedure persistence,
capture/replay and ActionPlan production remain in the language runtime;
physical effects, authority and receipts remain with the host.
