# qtmonaco-native

A native code editor widget for Qt6, built from scratch on a raw `QWidget`:
own text model, own rendering, no Scintilla, no QPlainTextEdit, no Chromium.

This is the start of a long-term hobby project intended to eventually replace
the embedded Monaco editor used by [qtmonaco](../qtmonaco) with a native
implementation exposing the same high-level API (see *Roadmap* below).

## Current state (v0.1 — spike)

The spike validates the two make-or-break risks of the plan:

- **Text model** — a piece table over UTF-16 code units, organized as an
  implicit treap with per-subtree aggregates (`chars`, `\n`-count). Gives
  O(log P) line↔offset conversion and O(log P) insert/delete, P = piece count.
- **Rendering** — viewport-culled painting of visible lines with QPainter on
  a raw QWidget, with blinking caret, keyboard/mouse editing, and vertical
  scrolling.

Measured on a 62 MB / 1,000,001-line document (Linux, GCC 16, `-O3`):

| Operation | Result |
|---|---|
| `setPlainText` (whole document) | ~4 ms after file read (~104 ms I/O + decode) |
| 20,000 random edits (insert/delete) inside the document | mean ~7 µs/op, ~30k pieces after the run |
| first-edit worst spike | ~20 ms (cold cache; later ops are µs) |

The core is under constant property-test attack: 2,000 random
insert/delete operations with a naive `QString` reference and a full
treap-invariant check after **every** operation (`tests/tst_textbuffer.cpp`).

Deliberately **not implemented yet** (see roadmap): selection, IME,
soft wrap, horizontal scrolling, undo/redo, syntax highlighting, grapheme-safe
edits (UTF-16 offsets may split surrogate pairs; the view layer does not do
this yet), diff view, LSP, scrolling past the end.

## Layout & layering

```
src/core/   TextBuffer — pure Qt-based C++ (QtCore ONLY), headless, testable
src/view/   CodeEditor — the raw QWidget canvas (QPainter), depends on core
src/app/    qmn-spike  — demo/bench harness, depends on view
tests/      property tests (core) + keyboard smoke tests (view)
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
```

Headless smoke/bench run:

```bash
QT_QPA_PLATFORM=offscreen ./build/src/app/qmn-spike --exit-after-ms 2000 bigfile.py
```

## Roadmap (hobby-scale, see estimates in the qtmonaco discussion)

Milestones, in build order:

- [x] M0 spike: model + painting + caret + typing + v-scroll + perf stats
- [ ] M1 selection (logical + visual), clipboard
- [ ] M2 IME composition, grapheme/word boundaries (QStringConverter iterators)
- [ ] M3 undo/redo (piece-list snapshots/ops), multiple carets
- [ ] M4 syntax highlighting (KSyntaxHighlighting + async rehighlight with state checkpoints)
- [ ] M5 decoration layers + line-number gutter + markers
- [ ] M6 find/replace (QRegularExpression)
- [ ] M7 overview ruler (replaces minimap)
- [ ] M8 LSP client (QWebSocket + JSON-RPC) — completion, diagnostics, hover
- [ ] M9 soft wrap, horizontal scrolling
- [ ] M10 diff view
- [ ] M11 accessibility (QAccessible), final polish

Out of scope by design: vim mode (explicitly dropped).

License: TBD (parent project qtmonaco is BSD-3-Clause).

## Model micro-benchmark

```bash
g++ -O3 -std=c++20 tools/bench_model.cpp -Isrc/core/.. \
    build/src/core/libqmn_core.a \
    -isystem /usr/include/qt6/QtCore -isystem /usr/include/qt6 \
    -lQt6Core -o bench_model && ./bench_model <big-file>
```
