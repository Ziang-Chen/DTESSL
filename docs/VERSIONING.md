# Versioning

DTESSL versions have exactly three numeric components:

```text
v<Milestone>.<MajorFeature>.<MinorFeature>
```

They deliberately do not claim Semantic Versioning semantics.

- `Milestone` changes when the project crosses a system-level acceptance gate.
  A milestone can change language, canonical AST, runtime or tooling contracts.
- `MajorFeature` identifies a coherent feature slice inside that milestone, such
  as relation search or canonical packages.
- `MinorFeature` is an additive refinement, bug fix or evidence improvement
  inside the same major feature slice.

Rules:

1. The first executable baseline is `v0.0.1`.
2. Every tag must point to a clean commit whose stated acceptance tests pass.
3. Implemented syntax and planned syntax must never share an undocumented
   version label.
4. Source files may declare a language version only after package syntax exists;
   until then the compiler version selects the accepted grammar.
5. Canonical AST and trace formats get their own explicit format versions and
   are not inferred from the CLI version.
6. A `Milestone` increment requires a migration note and compatibility matrix.
7. Published tags are immutable. A correction creates the next
   `MinorFeature`, never a moved tag.

## Milestone gates

| Milestone | Meaning | Exit gate |
| --- | --- | --- |
| `v0.*.*` | Language and semantic foundation | Design closes; behavior may still evolve between feature slices. |
| `v1.*.*` | Stable executable modeling system | Canonical packages, host ABI, traces, replay and tooling are stable. |
| `v2.*.*` | Multi-projection verification system | One typed AST drives execution, monitors, bounded exploration and formal export. |
| `v3.*.*` | Distributed assurance ecosystem | Standard distributed/security/resource dialects and evidence workflows are validated. |

The detailed feature sequence and acceptance evidence are maintained in
`docs/ROADMAP.md` and `docs/PROJECT_QC.md`.
