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
| StringSpecial | — | — | 1 |
| StringSpecialKey (object keys) | — | 30 | — |
| Escape | 1 | 1 | — |
| Embedded (f-string / template expr) | 4 | — | 3 |
| Constant / ConstantBuiltin | — | — + 6 | 2 + 3 |
| Variable | 96 | — | 51 (+4 builtin) |
| Function | 1 | — | — (see note) |
| Property | — | — | 27 |
| Operator | 26 | — | 26 |
| PunctuationSpecial | 4 | — | — |
| PunctuationBracket / Delimiter | — | — | 69 / 62 |

Roles a grammar never emits here are upstream-query facts, not engine
bugs — worth knowing before anyone "fixes" a demo:

* **JSON has no punctuation captures** in its `highlights.scm` at all —
  braces/commas render in the default text color on purpose. Object keys
  get their own color from `key: (_) @string.special.key`, and that works
  because the file was written for tree-sitter's older *first matching
  pattern per node wins* convention — which is exactly what
  `HighlightEngine`'s paint-over merge implements (see the sort comment
  in `src/view/HighlightEngine.cpp`).
* **The pinned python and javascript queries were written for the newer
  convention — *later* patterns override earlier ones — and put their
  catch-all `(identifier) @variable` first** (line 3 of python's, line 4
  of javascript's) expecting every specific role pattern below it
  (`@function`, `@function.method`, `@constructor`, `UPPER_SNAKE`
  `@constant`, …) to override the catch-all. Under our first-wins merge
  the catch-all wins instead, so `def`/call/class names and capitalized
  identifiers paint as plain `Variable` in both grammars. The python
  table's lone `Function` is the `@` of `@dataclass` — its node starts
  before the identifier's, so the catch-all never covers it. Roles bound
  to their own node types (`this`/`super` → `VariableBuiltin`,
  `true/false/null` → `ConstantBuiltin`, `.property` accesses) are
  unaffected, which is why the js column still looks rich.
* Realigning the two query files (moving the catch-all to the end) or
  flipping the merge convention (which would rework the json ordering and
  the M4 merge tests) are both one small, deliberate step away —
  decidedly not done as a side effect of adding demos.
