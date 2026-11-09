# PLAN

Working plan for qtmonaco-native. Phases map to the `README.md` roadmap
(M0…M12) and obey the constraints in `AGENTS.md` (layering rule, wrap-friendly
public API, no vision ⇒ numeric assertions only).

## Status (2026-11-09, baseline `9362281`)

Done: **M0** spike (piece table + painter + caret + typing + v-scroll), caret-drift
fix (fractional cell metrics), **M1** selections, bench tool.
Nothing else is started.

## Why this order

The PySide6 wheel is a *deliverable*, and its failure modes (Qt ABI drift,
packaging matrix) are cheap to find early and expensive to find late — and the
Python binding **freezes the C++ public API**. So: run a *thin* binding slice
early, widen it later. Conversely M2 grapheme segmentation **changes position
semantics**, so it must land before the bound surface grows.

## Phase 0 — toolchain unblock (**done**, 2026-11-09)

Host: Qt **6.12.0** in `/usr/lib`, gcc 16.2.1, cmake 4.4.4, ninja 1.13.2,
system Python 3.14.8, **no KF6SyntaxHighlighting**.

Setup actually used (note: Arch's system `python3` has **no pip**; `uv 0.13.6` is
on PATH, and `python3 -m venv` does bootstrap pip on its own):

```bash
uv venv .venv                                  # picked CPython 3.13.18
uv pip install --python .venv "PySide6-Essentials==6.12.0" \
    "shiboken6_generator==6.12.0" scikit-build-core ninja cmake
```

Result: PySide6 6.12.0, **bundled Qt 6.12.0 == system Qt 6.12.0** — today they
coincide, which is luck, not design. `Essentials` is enough (QtWidgets +
QtPrintSupport); no need to pull the large Addons. glibc is **2.44**, well above
PySide6's `manylinux_2_34` baseline.

### Blocker found: Shiboken codegen cannot run on this Arch host

The pip `shiboken6_generator` binary fails immediately:

```
error while loading shared libraries: libxml2.so.2: cannot open shared object file
```

Arch ships `libxml2.so.16` (2.14 renamed the soname), and the wheel does **not**
bundle libxml2 or libclang. Don't work around this with a
`libxml2.so.2 -> libxml2.so.16` symlink — the ABI is not compatible.

**Verified resolution — do the codegen in a container.** The RHEL9 base that
manylinux_2_34 is built on has everything the generator needs:

```bash
docker run --rm rockylinux:9 bash -lc \
  'ls /usr/lib64/libxml2*; dnf -q repoquery --whatprovides "libclang.so*"'
# -> /usr/lib64/libxml2.so.2 (2.9.13); clang-libs provides libclang.so
```

So: C++ development and the existing CTest suite stay on the Arch host;
**binding codegen + wheel building go into a manylinux container**, which is the
same environment Phase F/cibuildwheel needs anyway. Docker 29.9.2 works here.

## Phase A — M1b clipboard

copy/cut/paste, middle-click X11 primary, paste replaces selection.
**DoD:** state-based tests for copy→paste round trip, primary selection, and
paste-over-selection; no surrogate pair split.
**Watch:** X11 clipboard under `QT_QPA_PLATFORM=offscreen` may not work — that
test may need a real/Xvfb display (qtmonaco itself uses `pytest-xvfb`). Gate it
rather than making the suite depend on a display.

## Phase B — thin binding spine (risk spike, do it before feature depth)

Bind exactly one class and the *smallest stable surface*: `setPlainText`/
`toPlainText`, cursor get/set, insert/delete text, `cursorMoved` /
`selectionChanged` signals. **Do not bind grapheme-sensitive API yet.**

- `pyproject.toml` with `scikit-build-core`; binding driven by the **verified**
  macro `shiboken_generator_create_binding()`, provided by
  `find_package(Shiboken6Tools)` (lives in
  `shiboken6_generator/lib/cmake/Shiboken6Tools/Shiboken6ToolsMacros.cmake`).
  Required kwargs: `EXTENSION_TARGET`, `GENERATED_SOURCES`, `HEADERS`,
  `TYPESYSTEM_FILE`, `LIBRARY_TARGET`; optional `QT_MODULES`,
  `SHIBOKEN_EXTRA_OPTIONS`, `FORCE_LIMITED_API`. It emits an `add_library(... MODULE)`,
  sets `PREFIX ""` + a python extension suffix, and links
  `Shiboken6::libshiboken` + `PySide6::pyside6` + `Qt6::<module>`.
- Build against **PySide6's own Qt headers/libs from site-packages**, never
  `/usr/lib` — and run the codegen in the container from Phase 0, not on the
  Arch host.

**DoD:** a local wheel installs into a fresh venv; an offscreen smoke script
imports it, instantiates `CodeEditor`, round-trips text, and receives a Qt
signal in Python. That proves ABI + packaging — nothing else is in scope.
**Kill criterion:** if it mismatches, pin the PySide6/Qt pair and document it.

**Status: DONE (2026-11).** No ABI mismatch: exactly one `libQt6Core.so.6`
(PySide6's) is mapped in both the build-tree and installed-wheel smokes
(`/proc/self/maps` assertion in `bindings/tests/_smoke_checks.py`). The pair
is pinned anyway — PySide6 **6.12.0** + Qt **6.12.0** (`bindings/Dockerfile`);
`bindings/build_in_container.sh` runs the whole chain (codegen via the official
macro, compile, smoke, wheel). Gotchas that cost time are recorded in AGENTS.md.
Next binding widening happens only after Phase C (M2 graphemes).

## Phase C — M2 text correctness (must precede widening the bound API)

Grapheme/word boundaries, IME composition. Position semantics change here, so
finish it before the binding surface grows.
**DoD:** arrows/Backspace/Delete act on grapheme clusters; verified by numeric
assertions (no screenshots — see `AGENTS.md`).

## Phase D — editing depth

M3 undo/redo (+ multiple carets) · M4 syntax highlighting · M5 decoration
layers + line-number gutter + markers · M6 find/replace.
**Open decision (M4) — highlight engine.** Three options; tree-sitter facts
verified against v0.27.2 sources (released 2026-11-07):

- **A. KF6 `KSyntaxHighlighting`** — not installed here; a KF6 framework that
  drags KDE cmake infra + KConfigCore/KCoreAddons into the dependency set.
  Problematic for the wheel deliverable: PySide6 wheels bundle no KDE libs, so
  we would ship KF6 runtime libs + definition XMLs beside `_qmn`. Only
  attractive if the wheel stops being first-class.
- **B. Own Monarch-style line highlighter** — zero deps, UTF-16 native, mirrors
  the Monaco look qtmonaco users expect (Monarch is Monaco's own engine). But
  no syntax tree: nothing reusable for folding/smart-select, M10 structural
  diff or M8 assist, and every language definition is ours to author forever.
- **C. Vendored tree-sitter** (runtime + committed generated `parser.c` per
  grammar) — pure C, compiles straight into the library/`_qmn` (single
  amalgamation TU `lib/src/lib.c` + one `parser.c` per grammar), wheel-safe,
  no runtime deps. Verified fit with our model: `TSInputEncodingUTF16LE` is
  first-class and the lexer advances by encoded-unit size, so a tree-sitter
  byte offset is **exactly 2×** our 0-based UTF-16-unit offset — no
  transcoding, no byte/rope mapping pain; the pull-model `read` callback can
  hand out piece-table chunks (QString data *is* UTF-16LE on LE) zero-copy.
  Incremental is built-in: `ts_tree_edit` + reparse with old tree,
  `ts_parser_set_included_ranges` for viewport-scoped parsing,
  `TSParseOptions.progress_callback` for time-sliced parses. ABI 15 runtime,
  accepts grammars ≥13 — pin the vendored copy, commit generated parsers
  (CLI needed only to regenerate, not to build). Costs: adapter plumbing
  (piece table ↔ `TSInput`, edits in byte units), per-language `.scm` query
  curation, ~0.1–1 MB of parser tables per grammar in the wheel.
**Decided: C — vendored tree-sitter** (pin v0.27.2), with a thin engine seam
so grammar-less languages (plain text, …) keep a trivial fallback. Options A
and B are kept above as the recorded rationale.

## Phase E — M7…M10 + parity surface

| qtmonaco API | our state today | where |
|---|---|---|
| `set_text` / `get_text` | `setPlainText` / `toPlainText` | bind now |
| `insert_text` / `delete_line` | `insertAtOffset` / `removeRange` | thin, now |
| `set_cursor` / `current_cursor` | `setPosition` / `position` | bind now |
| `set_language` / `get_language` | none | M4 |
| `set_highlighted_lines` / `clear_highlighted_lines` | none | M5 |
| `set_line_numbers_mode` (on/off/relative/interval) | gutter not built | M5 |
| `set_theme`, `set_readonly` | none | new view flags |
| `set_scroll_beyond_last_line_enabled` | explicitly not implemented | tie to it |
| `set_minimap_enabled` | no minimap by design | M7 ruler (**maps to it**, decided) |
| `open_diffs` | none | M10 |
| LSP hooks (`update_workspace_configuration`, `add_action`, lsp header) | none | M8 |
| `set_vim_mode_enabled` | out of scope | binding **warns, never implements** |

**Off-by-one trap (do this deliberately, not accidentally):** qtmonaco is
**1-based** — `delete_line(line)` says so in its docstring, `set_cursor(line,
column=1)` and `set_highlighted_lines(start, end)` are 1-based, and
`insert_text` defaults `column` to 1. Our `TextPosition` is 0-based for both.
The binding must convert explicitly and be covered by its own tests. Also match
error semantics: qtmonaco raises `ValueError` when readonly, and
`current_cursor` is a *property*.

**Decided divergences (binding-level behavior):**
- **vim** — out of scope, but `set_vim_mode_enabled` is *exposed* and emits a
  warning instead of being omitted, so drop-in callers don't hit
  `AttributeError`. Never implement vim.
- **minimap** — `set_minimap_enabled` maps onto the M7 overview ruler (same
  on/off meaning). No Chromium-style minimap is ever built.

## Phase F — M12 packaging/CI hardening

cibuildwheel matrix + `auditwheel` repair. Note PySide6's own manylinux baseline
is `manylinux_2_34_x86_64`; match it and set the Python floor to 3.10 (abi3).
Pin `PySide6>=<the version we built against>` in the wheel metadata.

## Top risks

1. **Qt drift + host toolchain mismatch** — Arch's system Qt rolls forward while
   PySide6 lags and ships its own. Always build against the venv's PySide6 Qt and
   pin the pair. This already bit once: the pip Shiboken generator cannot run on
   the host at all (missing `libxml2.so.2`) — see Phase 0 for the container fix.
2. **1-based vs 0-based** — silent off-by-one across the whole parity surface.
3. **M9 already failed once** — the horizontal-scroll attempt was committed as
   `2809945` and dropped from `main` (recoverable via reflog while present).
   Review that attempt before retrying instead of re-deriving it; treat M9 as
   high-risk.
4. **Highlight-engine weight** — see M4 open decision above (KF6 wheel burden
   vs vendored tree-sitter vs own highlighter).
5. **No vision** — every rendering milestone (gutter, ruler, diff, wrap) must be
   proven by numeric/state assertions; precedent is
   `cellWidthMatchesGlyphEngineAdvance`.
6. **API freeze ordering** — don't widen the bound surface before M2 lands.

Out of scope by design: vim mode.
