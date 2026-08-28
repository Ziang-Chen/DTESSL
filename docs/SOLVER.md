# Solver semantic layer

`Solver` is DTESSL's high-performance semantic layer between the typed frontend
and execution, exploration or export backends:

```text
source -> frontend -> typed Program -> Solver -> backend
                                      |
                                      +-> StateExpand / counterexample
```

## Names and ownership

- A source `state` is a static control-location declaration with typed variable
  slots, defaults and invariants. It is not a changing search node.
- A `Configuration` is dynamic: the active location on each `@context` axis plus
  the current typed variable valuation.
- `StateExpand` is the reachable directed graph formed by expanding enabled
  transitions from Configurations. Joins, back-edges and cycles are normal.
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

## Claim results

`always P` searches StateExpand for a reachable `not P` Configuration.
`eventually P` searches for a reachable nonmatching deadlock or nonmatching
lasso. A result is `verified` only after complete finite closure;
`bounded-verified` means no counterexample was found within configured bounds.
Count claims remain inconclusive until Claim monitor state participates in the
product key `Configuration × MonitorState`.

The Solver proposes logical decisions only. Runtime procedure persistence,
capture/replay and ActionPlan production remain in the language runtime;
physical effects, authority and receipts remain with the host.
