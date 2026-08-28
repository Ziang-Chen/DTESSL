# DTESSL Language Service and REPL

DTESSL v0.2.2 introduces the language-workbench foundation used by the CLI
REPL today and intended for editor/LSP adapters later. It does not implement
the Language Server Protocol transport yet.

## One language pipeline

There is one authoritative path:

```text
UTF-8 source
  -> production lexer + source spans
  -> production parser
  -> semantic verifier
  -> LanguageAnalysis
```

`dtessl check`, `dtessl highlight`, the REPL and the C++ language-service API
all use this implementation. Highlighting is lexical today, but its spans are
emitted by the same lexer that feeds the parser. No regular-expression grammar
or editor-only parser is maintained.

## Public API

Include `dtessl/language_service.hpp`.

- `analyze_source` returns highlight spans and located diagnostics.
- `LanguageDocument` owns a URI, monotonically increasing version and UTF-8
  source snapshot.
- `LanguageDocument::apply_edits` accepts non-overlapping edits against one
  pre-edit snapshot and applies them deterministically.
- `LanguageService` owns open documents and provides open/change/close/analyze
  operations that map directly onto a future protocol adapter.

Positions use one-based line and column values plus a zero-based UTF-8 byte
offset. Byte offsets are the internal edit authority. A future LSP adapter must
translate client UTF-16 positions at its boundary and must not change the core
representation.

Diagnostic families are stable at the category level:

- `DTESSL1001`: lexical diagnostics;
- `DTESSL1002`: parser diagnostics;
- `DTESSL2001`: semantic verification diagnostics.

The current parser stops at the first error. Multi-error recovery, symbols,
references, completion and semantic tokens can extend `LanguageAnalysis`
without changing the document/version/edit contract.

## CLI highlighting

```sh
build/dtessl highlight examples/scheduler.dtessl
```

The output is deterministic and machine-readable by line/column:

```text
2:1-2:6 keyword state
2:7-2:16 identifier Scheduler
```

## REPL workbench

Start with an existing program or an empty buffer:

```sh
build/dtessl repl examples/scheduler.dtessl
build/dtessl repl
```

The workbench supports:

- up/down history, left/right cursor navigation, Home/End, Delete/Backspace
  and common shell-style Ctrl editing keys;
- `:show`, `:check`, `:highlight`;
- `:append`, `:insert`, `:replace`, `:delete` with multi-line blocks;
- `:undo`, `:redo`, `:history`;
- `:load`, `:write`;
- `:reset` and `:run Event field=value` for persistent logical execution.

Interactive `:run` output is a presentation-layer decision card showing only
state deltas plus reads, writes, causal predecessors and the ActionPlan DAG.
The core `result_text` representation remains stable and unstyled for tests,
logs and machine comparison.

A single `.` terminates a multi-line edit block. Any non-command input is
appended as one source line. Editing invalidates the current engine so an event
can never execute against stale source.

## LSP continuation boundary

The next tooling slice can add a thin JSON-RPC/LSP adapter over
`LanguageService`. The core additions needed before claiming LSP support are:

1. UTF-16 boundary conversion and incremental line index;
2. parser recovery and multiple diagnostics;
3. stable symbol identities and scope/reference index;
4. semantic tokens, definition/references, hover and completion;
5. rename and fix-it workspace edits;
6. cancellation, request budgets and conformance tests.

Transport code must not own a second lexer, parser, symbol table or diagnostic
model.

The lexer already reserves the upstream core spellings `name` and `~` so
tooling can classify them while the corresponding parser/type/execution slice
is integrated. `[]/[value]` reuse punctuation spans and explicit `list[...]`
reuses the built-in `list` type token. The language service must follow the
upstream language design rather than invent alternate editor syntax.
