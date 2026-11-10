# Highlighting demos

One demo document per grammar pinned in `third_party/tree-sitter`
(pins + upstream query provenance: `third_party/tree-sitter/VENDORED.md`).
Each file is deliberately dense with the token roles its `highlights.scm`
actually emits — the tables below are the measured span tally per document
(whole-document merged spans), not an idealized wish-list.

```bash
./build/src/app/qmn-spike examples/demo.py        # python
./build/src/app/qmn-spike examples/demo.json      # json
./build/src/app/qmn-spike examples/demo.js        # javascript
```

`qmn-spike` guesses the grammar from the file suffix (`.py`, `.json`,
`.js`/`.mjs`/`.cjs`); `--language python|json|javascript` overrides the
guess (an unknown id falls back to plain text, by the `setLanguage`
contract). These files double as fixtures for the exact-span tests' role
table — the engine's colors are asserted numerically in
`tests/tst_codeeditor.cpp`, never visually.

## What each demo actually produces (measured spans per merged TokenKind)

| Role | demo.py | demo.json | demo.js |
|---|---|---|---|
| Comment | 8 | — | 9 |
| Keyword | 26 | — | 32 |
| Number | 9 | 7 | 7 |
| String | 11 | 16 | 13 |
| StringSpecial (regex) | — | — | 1 |
| StringSpecialKey (object keys) | — | 30 | — |
| Escape | 1 | 1 | — |
| Embedded (f-string / template expr) | 4 | — | 3 |
| Constant / ConstantBuiltin | 1 + — | — + 6 | 5 + 3 |
| Constructor | 6 | — | 4 |
| Function | 10 | — | 2 |
| FunctionBuiltin | 1 | — | — (`require` only) |
| FunctionMethod | — | — | 11 |
| Type (py annotations) | 11 | — | — |
| Variable | 54 | — | 38 (+6 builtin) |
| Property | 14 | — | 16 |
| Operator | 26 | — | 26 |
| PunctuationSpecial | 4 | — | 3 |
| PunctuationBracket / Delimiter | — | — | 66 / 62 |

Totals: 186 / 60 / 307 spans. The refined roles (`@function`,
`@constructor`, `@constant`, `@type`, …) fire because the merge follows
tree-sitter master's *later matching pattern wins* convention AND the
engine evaluates the queries' `#match?`/`#eq?` predicates itself — the
v0.27.2 runtime hands the cursor's raw emission stream to its consumer
without applying them (probe-verified). Without either half the role
table collapses the other way: catch-all `@variable` everywhere (no
later-wins) or every call name yellow via `FunctionBuiltin` (no
predicates). See the sort comment in `src/view/HighlightEngine.cpp` and
the tests `jsonKeyQueryPatternWinsOverPlainString` /
`queryPredicatesFilterRefinedRoles`.

Roles a grammar never emits here are upstream-query facts, not engine
bugs — worth knowing before anyone "fixes" a demo:

* **JSON has no punctuation captures** in its `highlights.scm` at all —
  braces/commas render in the default text color on purpose. Object keys
  get their own color from `key: (_) @string.special.key`; that block
  sits AFTER the plain `(string) @string` because our merge is
  later-wins (the upstream v0.24.8-era file ordered it first for the old
  first-wins convention — VENDORED.md records the reorder).
* **The pinned python query has no parameter-role capture and no
  `class_definition` name capture**: `def f(...)` parameters and a
  `class StockLedger:` header name paint as plain `Variable`. Its
  `#match? @constructor "^[A-Z]"` fires on capitalized identifiers
  ANYWHERE (annotation positions, `except` clauses, call sites), which
  is where demo.py's 6 `Constructor` spans come from (`Iterable` ×2,
  `ValueError`, `OSError`, `LedgerLoader`, `StockLedger`); the later
  `@type`/`@function.method` patterns retake a few of those —
  e.g. `-> StockLedger` counts as `Type`, `counter.add(...)` as
  `Property`. The 11 `Type` spans are exactly the parameter/return
  annotations the `(type (identifier) @type)` pattern covers.
* **Javascript mirrors that**: the same `^[A-Z]` constructor pattern is
  position-agnostic, so demo.js's 4 `Constructor` spans are capitalized
  identifiers at `new`/bare-expression positions (`TokenCounter` ×2,
  `new Set`, a bare `Infinity`); UPPER_SNAKE names go to the even later
  `@constant` pattern instead. `@function` fires only in direct call
  position — `summarize` at its declaration but plain `Variable` at
  `export default summarize`; the 11 `FunctionMethod` spans cover
  `class` methods (`constructor` itself included) and member calls.
  Formal parameters are plain identifiers too (no `variable.parameter`
  capture).
* `@function.builtin` in js is only `require` (`#eq?` — it never hits
  `readFile`, that stays `Function` via the call pattern), in py it is
  the builtins list (`#match?`): demo.py's lone `FunctionBuiltin` is
  `print(...)`, demo.js has none.
