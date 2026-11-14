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
| c grammar           | v0.24.2  | `2eeb4db31f8fa0865e45488503d13403923bcb485a1bdb637abff8c42dd97364` |
| cpp grammar         | v0.23.4  | `7a2c55afe3028f4105f25762ea58cc16537d1f5a1dcd9cca90410b3cd5d46051` |

The grammars' generated `parser.c` declare ABI 15 (python, javascript, c)
and ABI 14 (json, cpp); the v0.27.2 runtime (ABI 15) accepts grammars >= 13.

## What was kept vs dropped from each archive

- runtime: only `lib/src/` (compiled as the single amalgamation TU
  `lib/src/lib.c`), `lib/include/` and `lib/LICENSE` were kept; the repo's
  Rust/WASM/cli/test tree was dropped.
- grammars: only `src/parser.c`, `src/scanner.c` (python + javascript + cpp
  have an external C scanner; c and json have none — the c grammar's
  preprocessor is context-free enough to live in the LR tables),
  `src/tree_sitter/` (the generated parser's own headers) and
  `queries/highlights*.scm` were kept. The
  generator inputs `grammar.json` / `node-types.json`, the test corpus and
  the unused query files (tags/locals/injections, and javascript's
  `highlights-jsx.scm` / `highlights-params.scm` — not loaded by
  `HighlightEngine` in v1) were dropped. `highlights.scm` files are
  unmodified upstream copies except for TWO local adaptations:
  * json's `key: (_) @string.special.key` block was moved to the END of
    the file (the upstream v0.24.8-era file placed it first, for
    tree-sitter's old first-wins conflict convention; our merge follows master's
    later-wins convention, see the sort comment in
    `src/view/HighlightEngine.cpp`).
  * cpp's upstream v0.23.4 file (70 lines) colors almost nothing — no
    comments, numbers, plain strings, primitive types, control keywords,
    no `(identifier)` catch-all at all. Its exact upstream lines stay
    byte-identical FIRST; a documented LOCAL APPEND block was added at the
    END: comment, the 8 preprocessor tokens this pin actually lexes
    (`#pragma`/`#error`/`#undef` are NOT tokens of v0.23.4 — excluded on
    purpose), number/char/string/escape/system_lib_string, `true`/`false`,
    the C-flavored control/storage keyword list, `(identifier) @variable`
    (mirroring the c grammar's line 1), types + `field_identifier`
    `@property`, the ALL_CAPS `@constant` rule, and re-asserted `@function`
    patterns LAST (including bare-name `function_declarator` declarators,
    `destructor_name` — which has NO `declarator` field, plain
    `call_expression` identifiers and `obj.member()` calls the upstream
    query misses, and `preproc_function_def` names as `@function.special`
    like the c query). Appending is load-bearing: later-wins means the
    append only REFINES upstream captures, never stomps them, and the
    bottom `@function` block beats the generic catches above.
  Capture-name and predicate filtering still happens in the engine, never
  by editing the query files (the append block only ADDS patterns whose
  names the engine's capture table already knows).

Grammar properties worth knowing before "fixing" a query around them:
the c pin has no external scanner, so macro bodies after `#define NAME`
lex as monolithic `preproc_arg` named tokens — the upstream c query
captures nothing inside them (name + parameters ARE ordinary nodes);
`goto` and the label colon are likewise uncaptured upstream. The cpp pin
parses `void main` as an ERROR node and has no `switch_body`/`for_each`
foldable node names (the fold lists `kFoldC`/`kFoldCpp` only use names
verified against these pins' `parser.c`).

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
