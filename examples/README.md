# Highlighting demos

One demo document per grammar pinned in `third_party/tree-sitter`
(pins + upstream query provenance: `third_party/tree-sitter/VENDORED.md`).
Each file is deliberately dense with the token roles its `highlights.scm`
actually emits — the tables below are the measured span tally per document
(whole-document merged spans), not an idealized wish-list.

`examples/consumer/` is separate: the Phase F C++ consumer smoke, an
external `find_package(qmn)` project built against the INSTALLED package
(`cmake --install build --prefix /tmp/qmn-prefix`, then see the header
comment in its `CMakeLists.txt`; both CI jobs run it).

```bash
./build/src/app/qmn-spike examples/demo.py        # python
./build/src/app/qmn-spike examples/demo.json      # json
./build/src/app/qmn-spike examples/demo.js        # javascript
./build/src/app/qmn-spike examples/demo.c         # c
./build/src/app/qmn-spike examples/demo.cpp       # cpp
```

`qmn-spike` guesses the grammar from the file suffix (`.py`, `.json`,
`.js`/`.mjs`/`.cjs`, `.c`; `.h`/`.cpp`/`.cc`/`.cxx`/`.hpp`/`.hh`/`.hxx` →
cpp); `--language python|json|javascript|c|cpp` overrides the guess (an
unknown id falls back to plain text, by the `setLanguage` contract). These
files double as fixtures for the exact-span tests' role table — the
engine's colors are asserted numerically in `tests/tst_codeeditor.cpp`,
never visually.

## What each demo actually produces (measured spans per merged TokenKind)

| Role | demo.py | demo.json | demo.js | demo.c | demo.cpp |
|---|---|---|---|---|---|
| Comment | 8 | — | 9 | 15 | 31 |
| Keyword | 26 | — | 32 | 26 | 36 |
| Number | 9 | 7 | 7 | 11 | 21 |
| String | 16 | 17 | 16 | 8 | 11 |
| StringSpecial (regex) | — | — | 2 | — | — |
| StringSpecialKey (object keys) | — | 30 | — | — | — |
| Escape | 1 | 1 | — | — | 4 |
| Embedded (f-string / template expr) | 7 | — | 2 | — | — |
| Constant / ConstantBuiltin | 1 + — | — + 6 | 5 + 3 | 10 + — | 8 + — |
| Constructor | 6 | — | 4 | — | — |
| Function | 10 | — | 2 | 10 | 12 |
| FunctionBuiltin | 1 | — | — (`require` only) | — | — |
| FunctionMethod | — | — | 11 | — | — |
| Type | 11 | — | — | 14 | 24 |
| Variable | 54 | — | 38 (+6 builtin) | 29 | 42 |
| Property | 14 | — | 16 | 9 | 4 |
| Operator | 26 | — | 26 | 27 | — |
| PunctuationSpecial | 6 | — | 3 | — | — |
| PunctuationBracket / Delimiter | — | — | 66 / 62 | — / 21 | — / — |

Totals: 196 / 61 / 310 / 180 / 193 spans. (These numbers post-date the
mid-span repaint fix in `rebuildSpans`: an earlier pattern's span now
survives on BOTH sides of a later inner capture, so a string re-inked by
an `@escape`/f-string `{…}`/template `${…}` capture keeps its tail and
counts as two String spans — the pre-fix tables undercounted String /
Embedded / PunctuationSpecial.)

The refined roles (`@function`,
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
* **C is byte-upstream (no local adaptation at all)** and carries its
  grammar's quirks: the c pin has no external scanner, so `#define`
  BODIES are monolithic `preproc_arg` tokens — no ink inside them (the
  macro name + parameter are ordinary nodes: demo.c's 10 `Function`
  spans include the macro name `SQ` via `@function.special` plus its
  call site);
  enumerators like `RED` ride the ALL_CAPS `@constant` rule; `goto` and
  the label colon are uncaptured upstream (plain ink), while both label
  identifier sites take `@label` → function ink; `printf`/`puts` calls
  are `Function` — the c query has no builtin list; `char_literal` is
  one Number span (upstream never captures escapes inside it, so demo.c
  has zero `Escape` spans); punctuation captures are only `.`/`;`
  (`@delimiter`), everything else plain.
* **C++ is the appended-query demo** (the upstream v0.23.4 file colors
  almost nothing — VENDORED.md records the append block): comments,
  numbers, plain strings, primitive types, `true`/`false` and control
  keywords come from the append. `std` fails the upstream
  `namespace_identifier` `^[A-Z]` predicate (plain ink) while the
  `string` in `std::string` is a `type_identifier` in type position
  already (Type ink, no extra pattern needed). The cpp query captures
  NO punctuation at all: `;`, `::`, parens stay plain. Destructors get
  function ink from the added `destructor_name` pattern (it has no
  `declarator` field in this pin). UPPER_CASE macro guards like
  `DEMO_H_` are `Constant` via the ALL_CAPS rule — but a
  `#define SQ(x)`-style name re-takes its span as `Function` from the
  `preproc_function_def` pattern appended after it. Do not put
  `#pragma`/`#error`/`#undef` or range-for loops in the fixture: those
  are not tokens/nodes of the v0.23.4 pin.
