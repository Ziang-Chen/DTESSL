# SemanticDescriptor Projection v1

## Decision

An existing system may actively publish a versioned, backend-neutral model
projection for DTESSL. DTESSL validates and consumes that declared model; it
does not inspect, infer or reverse-engineer the existing system.

```text
existing system --explicit lossy projection--> SemanticDescriptor v1
  --> canonical .dtessl + source map + coverage/gaps
  --> check | logical run | native DTESSL EventTrace replay
  --> typed ActionPlan proposal
```

`SemanticDescriptor` is not a runtime log envelope. DTESSL does not accept
syslog, JSON runtime logs, audit logs, provider receipts, snapshots, runtime
journals or free text through this contract. Collection, runtime replay,
receipts and reconciliation are outside DTESSL.

## Semantic content

Format 1 covers typed state/event/relation fields; transitions with source,
requirements, deterministic `select ... by lex`, assignments and target; state
invariants; typed action ports and serial/parallel stages; semantic field paths;
lifecycle value mappings; producer origin, provenance and classified gaps.

The descriptor cannot define or construct `Handle`, `Binding` or `Lease`
types. Expressions cannot contain `$` calls. Only the generated `do` section
may reference a declared typed port. An action produces an `ActionPlan`; DTESSL
never opens a provider, PEP, raw handle or lease and never claims physical
completion.

## Provenance

Every descriptor chooses exactly one provenance class:

- `generated-operational-mirror`: a model emitted from a declared producer;
- `independent-assurance-model`: a separately authored model claim.

The second value is an author assertion, not proof of independence. Tools keep
it visible and never upgrade a generated mirror to independent assurance.

Every descriptor binds `origin.kind`, `origin.locator` and a lowercase SHA-256
origin digest. A consumer such as a ChenFlow Package adapter verifies that
binding against the exact source artifact; DTESSL does not fetch or decode the
source system.

## Gaps and coverage

Each known loss is named and classified:

- `unmodeled`: in-scope semantics absent from the projection;
- `abstracted`: represented at lower fidelity;
- `unsupported`: the projection format cannot express it;
- `external-runtime`: intentionally outside DTESSL, such as physical receipts.

The manifest reports structural counts, every gap class,
`structural-coverage-complete`, `gap-free` and their conjunction `complete`.
Thus an operational mirror can cover every descriptor dimension while openly
remaining incomplete because physical receipts are outside the model.

## Canonical serialization

Canonical UTF-8 line text begins with:

```text
dtessl.semantic 1 "ModelName" generated-operational-mirror
origin "producer-kind" "producer-locator" "<sha256>"
```

Typed directives are `port`, `state`, `field`, `invariant`, `event`,
`event.field`, `transition`, `require`, `select`, `assign`, `action`,
`lifecycle`, `gap`, then `end`. Canonical printing sorts identified records;
positional argument and lex-score order remain semantic. The descriptor digest
is SHA-256 over exact canonical bytes.

The generated source has a separate SHA-256. Its source map relates each
generated nonblank line to a stable descriptor path. Input order and formatting
cannot alter either canonical digest.

## Check, run and replay

- `descriptor-check` validates the descriptor, generates DTESSL and invokes the
  normal parser/type verifier.
- `descriptor-run` executes one native typed DTESSL event batch and returns
  logical state plus a typed `ActionPlan`, without performing actions.
- `DTESSL EventTrace` is a bounded sequence of native typed event batches.
  Replay rebuilds a fresh Engine and requires identical logical results and
  action plans.

The DTESSL replay contract is only:

```text
DTESSL: Program + DTESSL EventTrace -> logical state + ActionPlan
```

DTESSL has no API in this contract for runtime journals, snapshots, receipts,
reconciliation or re-executing accepted physical effects.

## IR consumer rule

An IR/ChenFlow integration imports only an explicit model projection supplied
by its producer and bound to the canonical Package digest. It must not
synthesize transitions from arbitrary JavaScript, runtime logs or opaque
reducer code. Import verifies origin binding, descriptor/generator digests,
provenance, coverage and gaps; DTESSL remains an optional independent
dependency.
