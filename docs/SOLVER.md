# Solver semantic layer

`Solver` is DTESSL's high-performance semantic layer between the typed frontend
and execution, exploration or export backends:

```text
source -> frontend -> typed Program -> Solver -> backend
                                      |
                                      +-> StateExpand x ClaimMonitor
                                                   -> counterexample
```

## Names and ownership

- A source `state` is a static control-location declaration with typed variable
  slots, defaults and invariants. It is not a changing search node.
- A `Configuration` is dynamic: the active location on each `@context` axis plus
  the current typed variable valuation.
- `StateExpand` is the reachable directed graph formed by expanding enabled
  transitions from Configurations. Joins, back-edges and cycles are normal.
- A `ClaimMonitor` is the finite control state compiled from one typed temporal
  formula. `since` contributes recurrence bits, `within` contributes a bounded
  counter, and liveness operators contribute waiting/satisfied/rejected phases.
- The explored verification node is `(Configuration, ClaimMonitorState)`.
  StateExpand remains the base graph; history-sensitive monitor state is not
  smuggled into the source `state` declaration or Configuration digest.
- `ConfigurationStore` canonically encodes and content-deduplicates nodes inside
  one Solver search. Full equality is checked after a digest match.
- A witness parent stored with a Configuration is only one discovery path used
  to print a counterexample. It does not turn StateExpand into a tree.

## Transition expansion

The verified Program owns stable context, state, transition and route IDs.
Production matching uses dense active-location signatures; the original string
and map matcher remains available as a semantic parity baseline. For each
Configuration the Solver:

1. finds candidate transition groups by TransitionId;
2. narrows routes by their source-context signature;
3. evaluates dynamic `where` predicates and relation searches;
4. applies the unique route or explicit unique optimum to produce a successor;
5. inserts the successor Configuration into StateExpand by exact content key.

The current bounded explorer admits zero-parameter transitions. A parameterized
transition needs an explicit finite input domain; until that language feature is
defined, the result is `inconclusive`, never a proof.
Likewise, a Claim, invariant or transition that observes `round` is
`inconclusive` until a finite logical-time component is explicitly included in
the product. The Solver never silently folds different times into one digest.

## Claim automata and results

The P0 temporal basis is `always`, `eventually`, `until`, `within` and `since`.
Ordinary predicate leaves use the same typed expression evaluator, quantifiers,
relations and pure functions as state invariants and transition `where` guards.
`never`, `before` and `weak_until` are frontend definitions lowered to that
basis; the Solver has no separate opcode or search algorithm for them.

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
claims start from the procedure's declared initial Configuration and quantify
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

The currently compiled deterministic monitor fragment admits a future operator
whose operands are state/past predicates, with arbitrary boolean and `since`
nesting inside those operands. Unsupported future-under-future nesting returns
`inconclusive`; it is never approximated as true.

The Solver proposes logical decisions only. Runtime procedure persistence,
capture/replay and ActionPlan production remain in the language runtime;
physical effects, authority and receipts remain with the host.
