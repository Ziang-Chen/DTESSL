# Backend and Provider Contract

## Purpose

DTESSL must support its C++ reference interpreter, ChenVM and other execution or
verification backends without encoding a particular VM into the language. This
document fixes the integration direction while deliberately leaving artifact
encoding unfrozen until `CanonicalModule` exists.

Adapters may live out of tree and consume DTESSL as an installed CMake package,
linked library or pinned submodule. ChenVM is one such consumer, not a privileged
semantic target. The same negotiation rules apply to every backend domain/name.

## Three boundaries

```text
source -> typed canonical module -> projection backend -> artifact/result
                                      |
decision ActionPlan ----------------> host provider -> typed receipt event
```

1. A frontend owns parsing, name resolution, typing, canonicalization and core
   verification.
2. A projection backend consumes only a versioned canonical module. Projection
   kinds are typed: execute, monitor, explore and formal export.
3. A host provider consumes typed action calls and returns typed receipts. It
   does not receive the parser tree and cannot redefine state-transition
   semantics.

## v0.0.3 discovery seam

`BackendDescriptor` identifies a backend by a structured domain/name/ABI tuple
and declares projection and language-feature sets. `required_features(program)`
derives the features actually used by a verified program.

`negotiate_backend` accepts a backend only if:

- the requested projection is explicitly supported; and
- every required `LanguageFeature` is present.

The result returns a typed missing-feature set. There is no generic
`compile("target")`, `invoke("operation")` or string attribute bag.

Since `v0.2.0`, `search_plans(program)` also exposes bounded summaries for
quantifiers, relation algebra and deterministic selection. A summary names the
typed operation, row/work limits, determinism and ambiguity policy. It is
diagnostic/negotiation data, not the private expression tree and not yet the
CanonicalModule lowering interface.

```cpp
dtessl::BackendDescriptor chen_vm{
    {"chen", "vm", 1},
    {dtessl::Projection::Execute},
    {
        dtessl::LanguageFeature::TypedState,
        dtessl::LanguageFeature::ParallelEventBag,
        dtessl::LanguageFeature::ActionDag,
    },
};

auto result = dtessl::negotiate_backend(program, chen_vm,
                                        dtessl::Projection::Execute);
if (!result.compatible) {
  // Reject before lowering or execution; report result.missing.
}
```

## Canonical module gate

The current `Program::Impl` remains private and is not a backend ABI. The future
canonical boundary must include:

- format/version identifier and feature set;
- fully resolved nominal types and declarations;
- canonical state/transition/search/action graphs;
- effect and authority requirements;
- resource/search budgets;
- semantic mode (`runtime` or `spec`);
- executable digest and optional source/assurance digests.

Only after cross-implementation golden tests exist may backends receive this
module or its stable read-only view.

## VM backend

A VM backend may lower the executable projection to its own typed IR or
bytecode. It must preserve atomic rounds, causal predecessors, action DAG edges,
budget traps and receipt boundaries. It must reject features it cannot preserve.
DTESSL does not mandate ChenBytecode, Wasm, JVM bytecode or a universal common
instruction set.

## Host providers

Host providers are separate from projection backends. They register typed
interfaces/capabilities and are selected by explicit Binding/requirement data.
They own raw handles, file descriptors, sockets, keys, leases and physical
completion. Provider output is untrusted until checked against the declared
receipt schema and correlated call ID.

## Compatibility

- Adding a `LanguageFeature` is additive to the discovery enum but may make an
  older backend incompatible with a program that uses it.
- A backend ABI increment is required when descriptor or canonical-module
  interpretation changes.
- A backend must not claim a broad feature and silently lower only a subset.
- Feature negotiation is verification, not proof that generated artifacts are
  semantically equivalent; each backend still needs conformance tests.
