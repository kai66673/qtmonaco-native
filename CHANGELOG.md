# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
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
