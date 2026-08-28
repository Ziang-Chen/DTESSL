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
