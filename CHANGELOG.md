# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.5.0] — 2026-11-09

### Added
- `CodeEditor::xForCol(line, col)`: widget-space x of a text position
  following the glyph engine's cluster advances (public already now because
  M5 decorations need exactly this geometry), and `positionAt(widgetPos)`
  replacing the private grid-based `posFromEvent`. `QWidget::setFont` on the
  editor now works end-to-end: `changeEvent(FontChange)` re-derives cell
  metrics and wipes the layout cache.

### Fixed
- Caret drift on lines containing wide clusters (reported live after
  0.4.0): caret x, selection rectangles, painting and mouse hit-testing now
  share ONE laid-out `QTextLine` per visible line (a fixed, text-keyed LRU
  cache in the view) instead of `col * cellWidth` grid arithmetic. A ZWJ
  family spans 8 code units but only ~1.8 cells of advance — everything
  after it used to slide away from the caret, hit-test and highlight.
  Plain monospace lines keep exact `col * cellWidth` positions (measured
  engine error 0.0), so nothing moved there; a click inside a wide cluster
  floors to the cluster start, and hit-testing keeps the editor-style floor
  semantics (`xToCursor`'s nearest-edge pick is walked back deliberately).
  Qt 6.12 note: `QTextLine::x(pos)/xToPos/cursorRect` are gone — the cache
  builds on `cursorToX`/`xToCursor`, present since Qt 4 and at our 6.6
  floor; a default-constructed `QTextLine` SEGFAULTS on any accessor, so
  every slot use is gated on a `valid` flag. Also measured: on the raster
  paint engine (the widget backing store, both X11 and offscreen)
  `QTextLine::draw`'s pos is the layout TOP-LEFT, not a baseline anchor —
  the engine adds `line.y() + line.ascent()` itself; passing a baseline
  there shifts every drawn line about a line-height below its caret band
  (the first live run of this commit showed exactly that: a fake empty
  line at top, caret a line off the glyphs). New numeric tests include a
  pixel-scan of `grab()` pinning the caret to the layout x (~50px away
  from the old grid position on the probe line) and a per-line ink-band
  assertion catching that draw-anchor class of regression; 55/55 green
  on real X11.

## [0.4.0] — 2026-11-09

### Added
- IME composition (closes Phase C / M2): `inputMethodEvent` renders the
  preedit at the caret with a dotted underline — display-only, it never
  enters the model — and commits arrive through the normal insert path,
  so committed CJK/surrogate text is cluster-safe from the start. Any
  deliberate caret move (keys, mouse, `setPosition`) or document replace
  drops a stale preedit. `inputMethodQuery` answers `ImEnabled`,
  `ImCurrentSelection` and `ImCursorRectangle` (the candidate-window
  anchor trails the preedit, matching the visual caret). Tests drive raw
  `QInputMethodEvent`/`QInputMethodQueryEvent` (Qt 6.12 event shape:
  preedit via ctor, commit via `setCommitString`); 49/49 green on real
  X11.

## [0.3.0] — 2026-11-09

### Added
- M2 word navigation: Ctrl+Left/Right (and Ctrl+Shift to extend) move by
  editor-style word runs (word / space / punctuation classes, VS Code
  `cursorWordRight`/`cursorWordLeft` semantics, deliberately not the ICU
  `Word` boundary), Ctrl+Right then one press crossing the trailing space
  run, Ctrl+Left additionally swallowing the space run before the target
  word. At a line end Ctrl+Right wraps to the next line start (and vice
  versa); at EOL Ctrl+Del joins lines like plain Delete, with a selection
  Ctrl+Del deletes the selection. Targets are always snapped to grapheme
  boundaries.
- M2 grapheme-cluster correctness via `QTextBoundaryFinder` (QtCore,
  ICU-backed): Left/Right arrows move one whole cluster per press (ZWJ
  family sequences, regional-indicator flags, skin-tone modifiers, VS16
  and combining marks measured as single clusters); Backspace/Delete
  remove whole clusters too, superseding the earlier surrogate-pair-only
  guards. A caret sitting inside a cluster (reachable via `setPlainText`,
  `setPosition` or a mid-cluster click) snaps outward on arrows, and
  Backspace/Delete take the WHOLE cluster in both directions, so orphan
  surrogate halves or dangling combining marks can never accumulate.
  `col` numbering stays UTF-16 code units (LSP semantics, deliberate).

### Fixed
- PLAN.md/README M2 tooling note: the earlier "QTextBoundaryRunner
  (Qt ≥ 6.9)" correction was itself wrong — no such class exists in any
  Qt release (verified against the v6.9.0 source tree and the host
  Qt 6.12 headers/symbols); `find_package(Qt6 6.6)` needs no bump.

## [0.2.0] — 2026-11-09

First milestone on the release train (git tag + alpha wheel per milestone).

### Added
- M1b clipboard: Ctrl+C / Ctrl+X / Ctrl+V plus the platform chains matched
  via `QKeySequence` (`Ctrl+Ins`, `Shift+Ins`, `Shift+Del`, Cmd variants on
  macOS). With no selection copy/cut target the whole current line,
  Monaco-style: the non-last line goes with its trailing newline; cutting the
  last line also takes the newline that preceded it (VS Code line-cut
  semantics) so no phantom trailing line appears. Paste replaces any
  selection, normalizes `\r\n`/lone `\r` to `\n`, is a no-op on an empty
  clipboard and never splits a surrogate pair. X11 primary selection: every
  selection (mouse, keyboard, Ctrl+A) mirrors into it and it is released on
  clear/cut; middle-click pastes it at the click position, falling back to
  the regular clipboard where no primary selection exists (Wayland,
  offscreen). Public `copy()`/`cut()`/`paste()`. The suite runs green both
  offscreen (primary-mode tests gate on `supportsSelection()`) and on real
  X11, where all 37 tests execute unskipped.
- PySide6 binding spine (PLAN.md Phase B): the Shiboken6 extension module
  `_qmn` wraps `qmn::view::CodeEditor` + `TextPosition` via the official
  `shiboken_generator_create_binding()` macro; codegen, compile and wheel all
  run in the `bindings/Dockerfile` container (rockylinux:9, PySide6 6.12.0 +
  matching Qt 6.12.0 from pinned mirror archives), driven by
  `bindings/build_in_container.sh`. Packaged with scikit-build-core into a
  `qmn-native` wheel: `_qmn.abi3.so` at site-packages root plus a pure-Python
  `qmn/` shim exposing `qmn.CodeEditor` / `qmn.TextPosition`. Smoke tests
  (build-tree and installed-wheel) assert text round trip, `TextPosition`
  marshalling, `cursorMoved`/`selectionChanged` reaching Python callables, and
  exactly one `libQt6Core.so.6` in the process. Opt-in via
  `-DQMN_BUILD_PYTHON=ON`; host CTest and CI are unaffected.
- M1 selections: Shift+arrows/Home/End/PageUp/PageDown extend from the caret
  (the caret is the active end), Ctrl+Shift+Home/End extend to document
  bounds, Ctrl+A selects all, Escape collapses. Mouse: plain click collapses,
  Shift+click extends, left-drag selects with edge auto-scroll; the caret
  lands at the drag end. Typing/Enter replace the selection; Backspace and
  Delete remove it without touching neighboring text (`selectedText()`,
  `selectedLength()`, `hasSelection()`, `selectionChanged()` signal; the demo
  window title shows cursor position and selection size). Fully covered lines
  paint their selection through EOL using a semi-transparent highlight.
- View-layer edits never split a surrogate pair: Backspace/Delete remove the
  whole pair when adjacent to one (full grapheme clusters remain an M2 item).
- Up/Down navigation on the first/last line now collapses to the line
  start/end (previously it was a no-op, which also made Shift+Down on a
  single-line document select nothing).

### Fixed
- Caret drifted left along the line: cell width was taken from integer
  `QFontMetrics` (e.g. 8px) while the glyph engine advances the pen by the
  font's fractional advance (monospace @14px measures 8.3906px), ~0.39px/col
  drift. The editor now measures the cell width with `QFontMetricsF` (double),
  uses fractional hinting-free metrics (`PreferNoHinting`), places the caret
  at `padding + round(col * cellWidth)`, draws text with a fractional pen
  position, and hit-tests clicks against the same fractional cell grid.
  Regression test: engine advance of a 120-column line must equal
  `120 * cellWidth()` within half a pixel.

## [0.1.0] — 2026-11-09

Initial spike validating the two riskiest layers.

### Added
- `src/core/TextBuffer`: piece-table text model over UTF-16 code units with
  an implicit-treap line index (O(log P) insert/delete and line↔offset
  lookups); debug invariant checking for treap aggregates and heap order.
- `src/view/CodeEditor`: raw-QWidget editor — viewport-culled QPainter line
  painting, blinking caret, keyboard editing (text, Enter, Backspace, Delete,
  arrows, Home/End, PageUp/Down), mouse caret placement, vertical scrolling,
  per-keystroke latency instrumentation.
- `src/app/qmn-spike`: demo/bench harness with `--exit-after-ms` headless
  smoke mode and load-time reporting.
- Tests: property test (2,000 random edits vs naive reference with invariant
  checks after every op) + widget keyboard smoke tests; ctest integration;
  GitHub Actions CI (arch + ubuntu).
- Benchmarks (see README): ~4 ms `setPlainText` for 62 MB, mean ~7 µs per
  random edit inside a 1M-line document.

### Known limitations (v0.1)
- No selection, clipboard, IME, undo/redo, soft wrap, horizontal scrolling,
  syntax highlighting, or grapheme-safe caret movement (UTF-16 code-unit
  semantics; surrogate pairs may be split by programmatic edits, never by the
  view's own input handling).
