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

## Phase 0 — toolchain unblock (before any binding work)

Measured on this box: Qt **6.12.0** in `/usr/lib`, gcc 16.2.1, cmake 4.4.4,
ninja 1.13.2, Python **3.14.8**, **no PySide6 installed**, **no
KF6SyntaxHighlighting installed**. PyPI currently has `PySide6 == 6.12.0`
(ships its own Qt; `abi3`, cp310+; `requires_python >=3.10,<3.16` ⇒ 3.14 OK),
plus pip-installable `shiboken6_generator == 6.12.0`.

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install "PySide6[full]"        # [full] pulls shiboken6_generator
python -c "import PySide6.QtCore as C; print(C.qVersion())"
```

**DoD:** venv reports PySide6's Qt version; compare it to `/usr/lib` Qt and
record the pin. Today they coincide at 6.12.0 — that is luck, not design.

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

- `pyproject.toml` with `scikit-build-core`; Shiboken typespec + `.glue` driven
  by `shiboken6_generator`; `find_package(Shiboken6)`/`find_package(PySide6)`.
- **Unverified:** PySide6's CMake helper/macro names for generating a module.
  Discover them from the installed package before writing CMakeLists, e.g.
  `ls "$(python -c 'import PySide6,os;print(os.path.dirname(PySide6.__file__))')"/lib/cmake"`.
- Build against **PySide6's own Qt headers/libs from site-packages**, never
  `/usr/lib`.

**DoD:** a local wheel installs into a fresh venv; an offscreen smoke script
imports it, instantiates `CodeEditor`, round-trips text, and receives a Qt
signal in Python. That proves ABI + packaging — nothing else is in scope.
**Kill criterion:** if it mismatches, pin the PySide6/Qt pair and document it.

## Phase C — M2 text correctness (must precede widening the bound API)

Grapheme/word boundaries, IME composition. Position semantics change here, so
finish it before the binding surface grows.
**DoD:** arrows/Backspace/Delete act on grapheme clusters; verified by numeric
assertions (no screenshots — see `AGENTS.md`).

## Phase D — editing depth

M3 undo/redo (+ multiple carets) · M4 syntax highlighting · M5 decoration
layers + line-number gutter + markers · M6 find/replace.
**Open decision (M4):** `KSyntaxHighlighting` is **not installed** here and is a
KF6 framework (drags KDE cmake infra). Choose: take the KF6 dependency, or write
a small purpose-built highlighter.

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

1. **Qt drift** — Arch's system Qt rolls forward; PySide6 lags and ships its
   own. Always build against the venv's PySide6 Qt and pin the pair.
2. **1-based vs 0-based** — silent off-by-one across the whole parity surface.
3. **M9 already failed once** — the horizontal-scroll attempt was committed as
   `2809945` and dropped from `main` (recoverable via reflog while present).
   Review that attempt before retrying instead of re-deriving it; treat M9 as
   high-risk.
4. **KF6 weight** — see M4 open decision above.
5. **No vision** — every rendering milestone (gutter, ruler, diff, wrap) must be
   proven by numeric/state assertions; precedent is
   `cellWidthMatchesGlyphEngineAdvance`.
6. **API freeze ordering** — don't widen the bound surface before M2 lands.

Out of scope by design: vim mode.
