# qtmonaco-native

A native code editor widget for Qt6, built from scratch on a raw `QWidget`:
own text model, own rendering, no Scintilla, no QPlainTextEdit, no Chromium.

This is the start of a long-term hobby project. Goals:

- A native Qt widget for editing source code.
- Two final artifacts: a **Qt6 C++ library**, and a **PySide6 binding packaged
  as a wheel (`.whl`)**, to be generated with Shiboken6.
- Grow the widget's features toward **Monaco parity** — specifically the public
  API of [`qtmonaco`](https://github.com/bec-project/qtmonaco)'s `Monaco` class,
  which today embeds the Monaco JS editor in a `QWebEngineView` (Chromium).
  qtmonaco-native is meant to replace that with a native, dependency-light
  widget. Deliberate exception: no vim mode. See *Roadmap* below.

## Current state (v0.1 — spike)

The spike validates the two make-or-break risks of the plan:

- **Text model** — a piece table over UTF-16 code units, organized as an
  implicit treap with per-subtree aggregates (`chars`, `\n`-count). Gives
  O(log P) line↔offset conversion and O(log P) insert/delete, P = piece count.
- **Rendering** — viewport-culled painting of visible lines with QPainter on
  a raw QWidget, with blinking caret, keyboard/mouse editing, and vertical
  scrolling. Geometry has a single source of truth: each visible line is
  laid out once (per-line `QTextLine` cache), and caret x, selection
  rectangles, drawn glyphs and mouse hit-testing all read the same glyph
  engine advances — wide clusters (emoji ZWJ sequences, flags) never drift
  the caret off the text; on monospace ASCII the result is bit-identical to
  the original `col * cellWidth` grid model.

Measured on a 61 MiB / 1,000,001-line document (Linux, GCC 16, `-O3`;
re-measured at the v1 freeze 2026-11-12 on HEAD, undo log included — the
exact document is reproducible, recipe under "Model micro-benchmark" below):

| Operation | Result |
|---|---|
| `setPlainText` (whole document) | ~5 ms after file read (~110 ms I/O + decode cold, ~2 ms cached) |
| 20,000 random edits (insert/delete) inside the document | mean ~7 µs/op, ~30k pieces after the run |
| first-edit worst spike | ~20 ms (cold cache; later ops are µs) |

The core is under constant property-test attack: 2,000 random
insert/delete operations with a naive `QString` reference and a full
treap-invariant check after **every** operation (`tests/tst_textbuffer.cpp`).

Deliberately **not implemented yet** (see roadmap): soft wrap, horizontal
scrolling, column memory, full
grapheme segmentation (`col` still counts UTF-16 code units even though
arrow keys and edits snap to cluster boundaries), diff view, LSP, scrolling
past the end.

## Layout & layering

```
src/core/   TextBuffer — pure Qt-based C++ (QtCore ONLY), headless, testable
src/view/   CodeEditor — the raw QWidget canvas (QPainter), depends on core
src/app/    qmn-spike  — demo/bench harness, depends on view
            qmn-demo   — full demo app: tabbed editors + settings dialog
tests/      property tests (core) + keyboard smoke tests (view)
tools/      standalone micro-benches (model, highlight latency tree split)
third_party/ vendored tree-sitter runtime + generated grammars (M4; the
            qmn_ts static lib links PRIVATE into qmn_view — tree-sitter
            never crosses src/core/ or the public view API)
```

Layering rules are contractual: `core` must never include QtGui/QtWidgets.
This keeps the model benchmarkable and fuzzable without a display.

## Build

Requirements: CMake ≥ 3.22, a C++20 compiler, Qt6 ≥ 6.6 (Core/Gui/Widgets/Test).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build          # core tests need no display
./build/src/app/qmn-spike [file]
./build/src/app/qmn-demo [files...]   # tabbed demo: Файл->Открыть…, Настройки
```

Headless smoke/bench run:

```bash
QT_QPA_PLATFORM=offscreen ./build/src/app/qmn-spike --exit-after-ms 2000 bigfile.py
QT_QPA_PLATFORM=offscreen ./build/src/app/qmn-demo --exit-after-ms 300 bigfile.py
```

Highlighting demos, one per grammar (`.py`/`.json`/`.js` suffixes pick the
grammar automatically; `--language` overrides): see `examples/README.md`.

## Roadmap (hobby-scale, see estimates in the qtmonaco discussion)

Milestones, in build order. **Gate 1** (editor v1: M1b…M13 + M12 completion)
ships a self-contained editor usable from C++ and PySide6; **Gate 2** (IDE
surface) starts only after the v1 API freeze — details in *PLAN.md*.

Gate 1 — Code Editor v1:

- [x] M0 spike: model + painting + caret + typing + v-scroll + perf stats
- [x] M1 selection: shift-navigation, drag (with autoscroll), shift+click,
      Ctrl+A, replace-on-type/Enter/Backspace/Delete, Escape clears
- [x] M1b clipboard: copy/cut/paste, middle-click paste, X11 primary selection
- [x] M2 grapheme/word boundaries (QTextBoundaryFinder): cluster-safe
      arrows/backspace/delete, editor-style Ctrl+arrows/Ctrl+Del word motion
- [x] M2 IME composition (preedit rendered at caret, commit via insert)
- [x] M3 undo/redo: op log in the core with coalescing + compound groups,
      caret/selection-restoring undo in the view (Ctrl+Z/Ctrl+Shift+Z/Ctrl+Y)
- [x] multiple carets: Alt+Click/Alt+Drag add and select per slot, typing,
      deletion, clipboard and navigation fan out per caret; one undo step
      per multi-caret user action (see CHANGELOG for v1 divergences)
- [x] M4 syntax highlighting (vendored tree-sitter runtime + generated
      grammars; incremental, viewport-scoped — see PLAN.md D2)
- [x] M5 decoration layers + line-number gutter + markers
- [x] M6 find/replace (QRegularExpression): Ctrl+F bar, per-line engine,
      match highlights, F3/Shift+F3, replace + one-action replace-all
- [x] M7 overview ruler (replaces minimap): 44px right-edge strip, whole-doc
      linear mapping, click/drag scroll, O(strip) marker stripes + viewport frame
- [x] M9 horizontal scrolling (h-bar + ambient doc-width extent, caret-keep-visible,
      Shift/trackpad wheel, scroll-past-end flag; soft wrap deferred to Gate 2)
- [x] M13 code folding (tree-sitter `@fold` ranges + indent fallback; gutter
      ▾/▸ toggles + Ctrl+Shift+[/] chords; ONE view-line map routing paint,
      caret nav, selection, gutter, ruler and the v-scroll space)
- [ ] E3 usability polish before the freeze: M14 Tab/auto-indent ✔ · M15 ✔
      (smart Home + double/triple-click + Ctrl+D) · M16 context menu + text
      DnD ✔ · M17 font/zoom API ✔ · M18 auto-close pairs + long-line ruler +
      current-line style ✔ · M19 CodeEditorOptions settings object ✔
- [x] M11 accessibility (QAccessible bridge: role EditableText, full
      text + editable-text interfaces over the raw document frame, M13
      view-map geometry, live events; final polish stays non-gating)
- [x] M12 PySide6 binding + wheel: expose `CodeEditor` via Shiboken6
      (scikit-build-core + cibuildwheel), production abi3 wheel +
      C++ install/export — the v1 API freeze (see PLAN.md Phase F)

Gate 1.5 — usability interlude (post-freeze minor train, see PLAN.md):

- [x] M20 font/zoom onto the settings surface (the M17 knobs mirrored
      onto options() with the bus covering every door incl. the
      foreign-setFont rebase; qmn-demo template + dialog wired)
- [x] M21 indent guides (indent lines — options-only `indentGuidesEnabled`
      (default ON): per-line 1px tab-stop guides in the leading whitespace,
      tab landings included (VS Code rule); M18 ruler ink + clip recipe;
      qmn-demo 13th knob wired)
- [x] M22 fold-gutter hover region + innermost-fold click (hover over the
      fold strip highlights the INNERMOST fold containing the row (model-row
      anchored, scrolls glued); a click anywhere inside a fold now toggles it
      (marker-row-only rule widened, caret never moves); no new public API —
      hover rides setMouseTracking gated by the strip (Qt 6.12 has no widget
      hover virtuals); click-to-fold on the marker row already exists since M13)
- [ ] M23 language-scoped properties (tab width first; overrides ride
      above the editor settings — Monaco's `[languageId]` model)
- [ ] M24 themes (chrome colors + token map; the default theme keeps
      today's pixels, all "set_theme routes it" inks finally routed)

Gate 2 — IDE surface (after the freeze):

- [ ] M8 LSP client (stdio JSON-RPC via QProcess) — completion, diagnostics, hover
- [ ] M10 diff view → `open_diffs` parity
- [ ] Soft wrap (the deferred half of M9)
- [ ] DAP features via decorations (breakpoints, current frame, value hints)

Out of scope by design: vim mode. The `set_vim_mode_enabled` setter is still
exposed by the Python binding for API compatibility, but only warns (see
*PLAN.md*).

License: TBD (parent project qtmonaco is BSD-3-Clause).

## Model micro-benchmark

```bash
g++ -O3 -std=c++20 tools/bench_model.cpp -Isrc/core/.. \
    build/src/core/libqmn_core.a \
    -isystem /usr/include/qt6/QtCore -isystem /usr/include/qt6 \
    -lQt6Core -o bench_model && ./bench_model <big-file>
```

The perf-table rows use a synthetic 61 MiB / 1,000,001-line document,
regenerable deterministically (seed 1; the bench itself seeds 42):

```bash
python3 - > bench1m.py <<'EOF'
import random
random.seed(1)
kw = ["def", "class", "if", "else", "for", "while", "return",
      "import", "from", "with", "as", "pass", "yield"]
for i in range(1_000_001):
    print("    %s value_%d = compute(x, y) + offset # comment %d"
          % (random.choice(kw), i, i))
EOF
```
