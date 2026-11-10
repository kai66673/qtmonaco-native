# Vendored tree-sitter

Pinned at PLAN.md Phase D2 (engine decision C). Everything here is vendored
source: no tree-sitter CLI or network access is needed to build; the CLI is
only needed to REGENERATE a parser.

| component           | version  | upstream release tarball sha256                                                    |
|---------------------|----------|------------------------------------------------------------------------------------|
| runtime (`runtime/`)| v0.27.2  | `356528363bf7fbad12a2cfeb5a2678c0118ef886a4d69ad9a379db5fdef0735f` (github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.27.2.tar.gz) |
| python grammar      | v0.25.0  | `4609a3665a620e117acf795ff01b9e965880f81745f287a16336f4ca86cf270c` |
| json grammar        | v0.24.8  | `acf6e8362457e819ed8b613f2ad9a0e1b621a77556c296f3abea58f7880a9213` |
| javascript grammar  | v0.25.0  | `9712fc283d3dc01d996d20b6392143445d05867a7aad76fdd723824468428b86` |

The grammars' generated `parser.c` declare ABI 14/15; the v0.27.2 runtime
(ABI 15) accepts grammars >= 13.

## What was kept vs dropped from each archive

- runtime: only `lib/src/` (compiled as the single amalgamation TU
  `lib/src/lib.c`), `lib/include/` and `lib/LICENSE` were kept; the repo's
  Rust/WASM/cli/test tree was dropped.
- grammars: only `src/parser.c`, `src/scanner.c` (python + javascript have
  an external C scanner; json has none), `src/tree_sitter/` (the generated
  parser's own headers) and `queries/highlights*.scm` were kept. The
  generator inputs `grammar.json` / `node-types.json`, the test corpus and
  the unused query files (tags/locals/injections, and javascript's
  `highlights-jsx.scm` / `highlights-params.scm` — not loaded by
  `HighlightEngine` in v1) were dropped. `highlights.scm` files are
  unmodified upstream copies except for ONE local adaptation: json's
  `key: (_) @string.special.key` block was moved to the END of the file
  (the upstream v0.24.8-era file placed it first, for tree-sitter's old
  first-wins conflict convention; our merge follows master's later-wins
  convention, see the sort comment in `src/view/HighlightEngine.cpp`).
  Capture-name and predicate filtering happens in the engine, never by
  editing the query files.

## Regenerating a parser (not needed to build)

```
tree-sitter init-config && tree-sitter parse ...   # only when changing grammar.js
```
requires the tree-sitter CLI; the committed `parser.c` already matches the
pinned tag, so codegen never runs in CI (same rule as the Shiboken
codegen note in AGENTS.md).

## Licenses

All MIT; the runtime license is at `LICENSE` (copied verbatim), each grammar
keeps its own `grammars/<g>/LICENSE`.
