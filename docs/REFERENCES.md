# Research references and DTESSL inferences

DTESSL uses primary papers for semantic grounding. The paper statements below
are source facts; the indented DTESSL rules are project inferences.

## Event order and causality

- Leslie Lamport, “Time, Clocks, and the Ordering of Events in a Distributed
  System,” CACM 21(7), 1978.
  [Publication and paper](https://www.microsoft.com/en-us/research/publication/time-clocks-ordering-events-distributed-system/).
  The paper defines happens-before as a partial order and distinguishes it from
  a clock-derived total order.

  DTESSL inference: a scalar per-transition counter must not be treated as
  concurrency semantics. Causal order is explicit; a simulator round only
  groups an atomic event batch.

- Friedemann Mattern, “Virtual Time and Global States of Distributed Systems,”
  1989. [Author-hosted paper](https://vs.inf.ethz.ch/publ/papers/VirtTimeGlobStates.pdf).
  The paper explains why a linearly ordered time structure loses information
  about concurrent events and gives a partially ordered vector-clock model.

  DTESSL inference: vector time is an optional typed profile for distributed
  models; the core keeps causal predecessor relations instead of forcing every
  model into one global vector shape.

## Simultaneous discrete events

- A. C.-H. Chow, “Parallel DEVS: A Parallel, Hierarchical, Modular Modeling
  Formalism and its Distributed Simulator,” 1996.
  [Paper](https://www.bgc-jena.mpg.de/~twutz/devsbridge/pub/chow96_parallelDEVS.pdf).
  Parallel DEVS explicitly treats simultaneous events and transition collisions
  rather than relying on a tie-breaking select order.

  DTESSL inference: one round evaluates an event bag on one before snapshot;
  disjoint or explicitly confluent writes merge, while unresolved collisions
  reject the batch.

## Parallel discrete-event execution

- Jayadev Misra, “Distributed Discrete-Event Simulation.”
  [Author-hosted paper](https://www.cs.utexas.edu/~misra/scannedPdf.dir/DiscreteEventSimulation.pdf).

- David R. Jefferson, “Virtual Time,” ACM TOPLAS 7(3), 1985,
  DOI 10.1145/3916.3988.

  DTESSL inference: conservative/optimistic parallel simulation, rollback and
  global virtual time are backend strategies. They must not change source
  transition semantics, and optimistic execution requires checkpoint/rollback
  evidence before it can be enabled.

## Temporal logic and automata products

- Moshe Y. Vardi and Pierre Wolper, “An Automata-Theoretic Approach to
  Automatic Program Verification,” LICS 1986.
  [Author-hosted paper](https://www.cs.rice.edu/~vardi/papers/lics86.pdf).
  The paper translates a temporal specification to an automaton and reduces
  finite-state program verification to an automata-language/product problem.

  DTESSL inference: the language AST contains temporal formulas, while the
  Solver owns their monitor automata and the on-demand
  `StateExpand × ClaimMonitor` graph. Product state is not added to source
  `state` declarations.

- C. Courcoubetis, M. Vardi, P. Wolper and M. Yannakakis, “Memory Efficient
  Algorithms for the Verification of Temporal Properties,” CAV 1990 / FMSD
  1992. [Author-hosted paper](https://orbi.uliege.be/bitstream/2268/177132/1/CVWY%20CAV%2090.pdf).
  The paper reduces verification to emptiness of the program/property product
  Büchi automaton and studies SCC-avoiding/nested-search memory tradeoffs.

  DTESSL inference: v0.3.5 materializes only reached Product nodes and detects
  pending liveness cycles on that graph. Alternative nested DFS, SCC and
  probabilistic bitstate backends may optimize the same semantics later.

- Giuseppe De Giacomo and Moshe Y. Vardi, “Linear Temporal Logic and Linear
  Dynamic Logic on Finite Traces,” IJCAI 2013.
  [Author manuscript](https://www.diag.uniroma1.it/degiacom/papers/2013/IJCAI13dv.pdf).
  The paper gives finite-trace temporal semantics and relates LTLf/LDLf to
  finite-state automata.

  DTESSL inference: native closed Trace claims use finite-trace semantics,
  including logical Round 0. Open traces retain `pending` unless the observed
  prefix already gives a decisive safety/bounded witness.

- Gerard J. Holzmann, [SPIN theoretical background](https://spinroot.com/spin/theory.html)
  and [never-claim reference](https://spinroot.com/spin/Man/never.html).
  SPIN documents the use of Büchi/never automata, nested DFS, partial-order
  reduction and bitstate hashing for temporal verification.

  DTESSL inference: `Counterexample` remains the public artifact name; SPIN's
  surface term `never claim` is not copied. `never(p)` is a derived DTESSL
  predicate lowered to `always(not p)`.
