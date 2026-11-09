# AGENTS.md

Native code editor widget for Qt6, built from scratch on a raw `QWidget`
(own piece-table model, own QPainter rendering). No Scintilla, no
QPlainTextEdit, no Chromium. Long-term hobby project; see `README.md` Roadmap.

## End goal & artifacts (not built yet — design the C++ API for it now)

Two deliverables: a **Qt6 C++ library** and a **PySide6 binding packaged as a
`.whl`**, to be generated with **Shiboken6** (`shiboken6_generator` is pip
installable; build with scikit-build-core — PySide6's own CMake helper names are
still unverified, confirm them from the installed package). No Python/binding
scaffolding exists yet, so this shapes how `CodeEditor`'s API is designed *today*:

- **Consumed by** the sibling `qtmonaco` (GitHub `bec-project/qtmonaco`; local
  sibling `/home/kai/kaigit/qtmonaco`). Its `Monaco` class is a `QWebEngineView`
  (Chromium) subclass; this widget is the native drop-in meant to replace it.
  Parity target = `qtmonaco`'s public `Monaco` API (`set_text`, `get_text`,
  `set_language`, `open_diffs`, `set_cursor`/`current_cursor`, highlight lines,
  LSP hooks…). The Python-facing binding should mirror those names.
  **qtmonaco is 1-based** (`set_cursor`, `delete_line`, `set_highlighted_lines`,
  `insert_text`'s default column) while `TextPosition` is 0-based — the binding
  must convert deliberately, not implicitly.
  **Deliberate exception: no vim mode** — never implement it, but the binding
  exposes `set_vim_mode_enabled` and **warns** (so drop-in callers don't hit
  `AttributeError`). Same pattern for `set_minimap_enabled`: it maps to the
  overview ruler, never to a Chromium-style minimap.
- **Keep `CodeEditor`'s public API Shiboken-wrappable.** Shiboken can't wrap
  templates or many exotic/overloaded signatures, so keep the public surface
  Qt-native: `Q_OBJECT`, plain Qt types in params/returns, no C++ templates or
  STL types crossing the API boundary (internals are unconstrained). The binding
  wraps the **view** widget; `core` stays QtCore-only and needs no Python type.
- **Qt-version trap:** PySide6 wheels ship their *own* Qt6 (`qtmonaco` pins
  PySide6≥6.8). Build the extension against that Qt, not the distro's system Qt,
  or it ABI-mismatches PySide6 at import. Portable wheels ⇒ cibuildwheel on
  manylinux/musllinux, not against Arch's rolling system Qt.

## Hard rule: layering is contractual

Enforced only by convention + CMake link deps — nothing checks it automatically:

- `src/core/` (`TextBuffer`) — **QtCore ONLY**. Never include QtGui/QtWidgets.
  This is what keeps the model headless, benchmarkable and fuzzable.
- `src/view/` (`CodeEditor`) — core + QtWidgets (the raw QWidget canvas).
- `src/app/` (`qmn-spike`) — view only. Demo/bench harness.

Include headers relative to `src/` (CMake adds `src/` as a PUBLIC include dir):
`#include "core/TextBuffer.hpp"`, `#include "view/CodeEditor.hpp"`.

## Data-model conventions (easy to get wrong)

- All offsets/positions are **0-based UTF-16 code units**, `[start, end)` —
  matches `QString` and LSP position semantics. `col` counts UTF-16 units, not
  graphemes. Arrow keys / clicks also count UTF-16 units (full grapheme
  segmentation is a future milestone).
- Core is code-unit level and will **happily split a surrogate pair**. Keeping
  edits on grapheme/surrogate boundaries is the **view's** job (`CodeEditor`
  backspace/delete are surrogate-safe). Don't add that safety to core.
- `lineCount()` is always `>= 1`, even for an empty document.
- `lineColAt(offset)` pointing at a `'\n'` resolves to the position *before* it.

## Build & test

Requirements: CMake ≥ 3.22, C++20, Qt6 ≥ 6.6 (Core/Gui/Widgets/Test). Ninja is
what CI/README use.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build --output-on-failure
```

Two ctest tests: `textbuffer` (core property tests) and `codeeditor`
(widget tests). ctest already sets `QT_QPA_PLATFORM=offscreen` for `codeeditor`
(via `set_tests_properties`). Run one test:

```bash
ctest --test-dir build -R codeeditor --output-on-failure
```

Run a **single test function** by passing the slot name to the binary (must set
`QT_QPA_PLATFORM=offscreen` yourself when running outside ctest):

```bash
QT_QPA_PLATFORM=offscreen ./build/tests/tst_codeeditor cellWidthMatchesGlyphEngineAdvance
./build/tests/tst_textbuffer randomOpsVsReference          # guiless, no display needed
```

Headless smoke/bench run of the app:

```bash
QT_QPA_PLATFORM=offscreen ./build/src/app/qmn-spike --exit-after-ms 2000 <bigfile>
```

There is **no lint, formatter, or typecheck config** in this repo (no
clang-format / pre-commit). Match surrounding style; don't invent a formatter.

## Testing quirks

- `tst_textbuffer.cpp` is a property test: 2,000 random insert/delete ops
  checked against a naive `QString` reference with a **full treap-invariant
  check after every op**. Fixed seed (`0xC0FFEE`) — deterministic, so a failure
  is reproducible. Uses `QTEST_GUILESS_MAIN` (no display).
- `tst_codeeditor.cpp` drives keyboard/mouse via `QTest`, asserts on model state
  (not pixels). It builds with `QTEST_MAIN` and needs offscreen.
- **No vision here.** Image/screenshot input is unavailable to the agent in this
  repo in principle — never try to verify rendering by reading PNG/screenshot
  files or by "looking" at anything. This is a QPainter renderer, so pixel bugs
  (caret drift, cell metrics) are caught only by state/numeric assertions like
  `cellWidthMatchesGlyphEngineAdvance`, never visually.
- When synthesizing a mouse drag, build press/move/release `QMouseEvent`s
  manually (`QTest::mouseMove` won't carry button state) and **never wrap the
  `sendEvent` calls in `Q_ASSERT`** — in release builds the macro strips the
  whole expression, events included.

## Model micro-benchmark (`tools/bench_model.cpp`)

Standalone g++, not part of CMake. Needs `libqmn_core.a` already built, and the
Qt6 include/lib paths below are **Arch Linux paths** (adjust for other distros):

```bash
g++ -O3 -std=c++20 tools/bench_model.cpp -Isrc/core/.. \
    build/src/core/libqmn_core.a \
    -isystem /usr/include/qt6/QtCore -isystem /usr/include/qt6 \
    -lQt6Core -o bench_model && ./bench_model <big-file>
```

## Reference points

- Layering + roadmap + perf targets: `README.md`.
- Phased plan with DoD and risks (binding spike ordering, parity mapping):
  `PLAN.md`.
- Consumer + parity target: `qtmonaco` (GitHub `bec-project/qtmonaco`) — the
  PySide6/`QWebEngineView`-based editor this project aims to replace natively.
- Changelog for what shipped per milestone (M1 selections are in): `CHANGELOG.md`.
- CI is the source of truth for the supported build: `.github/workflows/ci.yml`
  (archlinux container + ubuntu-24.04, both `-DCMAKE_BUILD_TYPE=Release`).
- Deliberately out of scope: vim mode.
