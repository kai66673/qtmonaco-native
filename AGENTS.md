# AGENTS.md

Native code editor widget for Qt6, built from scratch on a raw `QWidget`
(own piece-table model, own QPainter rendering). No Scintilla, no
QPlainTextEdit, no Chromium. Long-term hobby project; roadmap in `README.md`,
phased plan in `PLAN.md`, shipping history in `CHANGELOG.md`.

## Current state (v1.6.0) — gates and the frozen surface

- Gate 1 (editor v1) froze the public API at tag `v1.0`; Gate 1.5 (usability
  train M20–M24) closed 2026-11-12 at tag `v1.5.0`. **Gate 2 is the open
  train**: M8 LSP, M10 diff view, soft wrap, DAP-via-decorations. Never start
  work for a later gate while an earlier one is open.
- The frozen surface widens **only additively** (Gate 1.5 precedent): no new
  view mapping/geometry primitives. The M5 decoration API already covers
  diagnostics, breakpoints and inlay hints, so Gate 2 must need no view
  surgery.
- Both deliverables ship: a C++ static package (export set `qmn::core` /
  `qmn::view`, plus internal `qmn::internal_tree_sitter`) and a PySide6 wheel
  `qmn-native` (cp310-abi3, manylinux_2_34). See "Python bindings" below.
- **Keep public view API Shiboken-wrappable**: `Q_OBJECT`, plain Qt types in
  params/returns, no C++ templates or STL types crossing the API boundary
  (internals are unconstrained). The binding wraps only the **view** widget;
  `core` stays QtCore-only and needs no Python type.

## Hard rule: layering is contractual

Enforced only by convention + CMake link deps — nothing checks it automatically:

- `src/core/` (`TextBuffer`) — **QtCore ONLY**. Never include QtGui/QtWidgets.
  This is what keeps the model headless, benchmarkable and fuzzable.
- `src/view/` (`CodeEditor` + `CodeEditorOptions`/`Theme`/`LanguageProperties`
  child QObjects, `HighlightEngine`) — core + QtWidgets + PRIVATE vendored
  tree-sitter (it never crosses core or the public view API).
- `src/app/` — view only. `qmn-spike` (bench/spike harness) and `qmn-demo`
  (tabbed demo app). `qmn-demo`'s window/dialog logic lives in the
  `qmn_demo_app` STATIC lib (`demo_main.cpp` is a thin main) because
  `tst_demo` links the lib and drives it offscreen.

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
- **Highlighting engine is vendored tree-sitter** (pinned v0.27.2, ABI 15,
  in `third_party/tree-sitter/`). Fed `TSInputEncodingUTF16LE` straight from
  piece-table chunks — never transcode. A tree-sitter byte offset (and
  `TSPoint.column`, byte-based too) is **exactly 2×** our UTF-16-code-unit
  offset; the `get_column()` scanner callback counts code points instead, but
  that never crosses our boundary.
- **Highlight merge convention (user decision): tree-sitter master's LATER
  pattern wins** for identical spans (sort tie-break pattern_index ascending
  in `rebuildSpans`). The pinned py/js/c queries rely on it (catch-all
  `(identifier) @variable` first); the cpp scm's LOCAL APPEND block is
  designed around it — upstream's 70 lines stay byte-identical FIRST and the
  added patterns only REFINES upstream captures; the `@function` re-asserts
  sit deliberately LAST (`VENDORED.md` records the append). The runtime does
  NOT evaluate query
  predicates — its cursor emits every syntactically matching capture — so the
  engine evaluates `#eq?`/`#match?` itself (`buildQueryPredicates`; predicate
  capture ids are GLOBAL name-table ids in v0.27.2, same namespace as cursor
  indices — no remapping; directives pass as satisfied; a QRegularExpression
  compile failure drops the PREDICATE, fail-open). Vendored json
  highlights.scm is LOCALLY REORDERED (pair-key capture moved to END) and
  cpp's is REORDERED-BY-APPEND — both recorded in `VENDORED.md`; py/js/c scm
  stay byte-upstream. A span painted over by a LATER capture fully inside it
  keeps remnants on BOTH sides (string tail after an `@escape` stays inked) —
  never let the split keep only the left one; and never compute the insert
  position as `i = spans_.insert(...) - spans_.begin()`: the operands are
  indeterminately sequenced, a realloc'ing insert plus an early-evaluated
  `begin()` yields a garbage index and heap corruption (the v1.6.0 crash —
  comment at that site records the trap). Tripwire tests:
  `jsonKeyQueryPatternWinsOverPlainString`, `queryPredicatesFilterRefinedRoles`,
  `cSpansMapToModelOffsets`, `cppSpansMapToModelOffsets`.
- **System-tree-sitter trap (build portability):** on the Arch host the
  SYSTEM libtree-sitter headers (`/usr/include/tree_sitter/`) silently absorb
  any missing-vendored-include mistake - `tst_codeeditor` compiled against
  them instead of the pinned v0.27.2 `api.h` until the clean wheel container
  exposed it (the v1.6.0 fix: an explicit vendored include dir on the test
  target; `qmn_view`'s qmn_ts link is PRIVATE and never propagates). Any
  consumer of `view/HighlightEngine.hpp` outside `qmn_view` (tests, bench
  tools) must add `third_party/tree-sitter/runtime/include` itself.
- **`.scm` queries are embedded at CMake CONFIGURE time** (`configure_file`
  in `third_party/tree-sitter/CMakeLists.txt`); the query files are
  `CMAKE_CONFIGURE_DEPENDS` of that dir, so a query edit re-runs configure on
  the next `ninja` automatically.

## Build & test (C++ host path)

Requirements: CMake ≥ 3.22, C++20, Qt6 ≥ 6.6 (Core/Gui/Widgets/Test). Ninja is
what CI/README use. CI is the source of truth: `.github/workflows/ci.yml`
(archlinux container + ubuntu-24.04, both Release).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build --output-on-failure
```

Three ctest tests: `textbuffer` (core property tests), `codeeditor` (widget
tests), `demo` (drives the qmn-demo surface programmatically). ctest already
sets `QT_QPA_PLATFORM=offscreen` for `codeeditor` and `demo` (via
`set_tests_properties`). Run one test:

```bash
ctest --test-dir build -R codeeditor --output-on-failure
```

Run a **single test function** by passing the slot name to the binary (must set
`QT_QPA_PLATFORM=offscreen` yourself when running outside ctest):

```bash
QT_QPA_PLATFORM=offscreen ./build/tests/tst_codeeditor cellWidthMatchesGlyphEngineAdvance
./build/tests/tst_textbuffer randomOpsVsReference          # guiless, no display needed
```

Headless smoke/bench runs of the apps (grammar comes from the file suffix;
`--language python|json|javascript` overrides on qmn-spike):

```bash
QT_QPA_PLATFORM=offscreen ./build/src/app/qmn-spike --exit-after-ms 2000 bigfile.py
QT_QPA_PLATFORM=offscreen ./build/src/app/qmn-demo  --exit-after-ms 300 bigfile.py
```

- **Never enable the Python extension on an Arch host.** The root option
  `QMN_BUILD_PYTHON` is OFF by default and CI stays Python-free; ON needs the
  container env (`bindings/Dockerfile`).
- Root CMake enables `CMAKE_POSITION_INDEPENDENT_CODE` **only** under
  `QMN_BUILD_PYTHON`, on purpose: the per-keystroke perf budgets were measured
  on non-PIC archives. Don't set PIC globally.
- C++ packaging smoke (both CI jobs run it): `cmake --install build --prefix
  <p>`, then configure `examples/consumer` with
  `-DCMAKE_PREFIX_PATH=<p>` — an external `find_package(qmn)` project.
  `qmn::internal_tree_sitter` is internal; consumers never link it themselves.
- There is **no lint, formatter, or typecheck config** in this repo (no
  clang-format / pre-commit). Match surrounding style; don't invent a formatter.

## Testing quirks

- `tst_textbuffer.cpp` is a property test: 2,000 random insert/delete ops
  checked against a naive `QString` reference with a **full treap-invariant
  check after every op**. Fixed seed (`0xC0FFEE`) — deterministic, so a failure
  is reproducible. Uses `QTEST_GUILESS_MAIN` (no display).
- Two tests are **timing-sensitive by design, and their failure mode is a
  vacuous pass, never a false fail**: `textRangeWalkStaysPrunedWithManyPieces`
  (guards the Gate-1 perf gate: 1,000 end-of-document probes on a 20k-piece
  doc, best-of-3, < 10 ms, ~100x margin) and `foldEndEscapesViewportParseClamp`
  (guards fold-vs-viewport-parse coverage). On a timing failure re-measure the
  bench before touching the budget.
- `tst_codeeditor.cpp` drives keyboard/mouse via `QTest`, asserts on model
  state (not pixels). It builds with `QTEST_MAIN` and needs offscreen.
- **No vision here.** Image/screenshot input is unavailable to the agent in
  this repo in principle — never try to verify rendering by reading
  PNG/screenshot files or "looking" at anything. This is a QPainter renderer,
  so pixel bugs (caret drift, cell metrics) are caught only by state/numeric
  assertions like `cellWidthMatchesGlyphEngineAdvance`. `examples/demo.{py,
  json,js}` double as fixtures for the exact-span role tables asserted there.
- When synthesizing a mouse drag, build press/move/release `QMouseEvent`s
  manually (`QTest::mouseMove` won't carry button state) and **never wrap the
  `sendEvent` calls in `Q_ASSERT`** — in release builds the macro strips the
  whole expression, events included.

## Python bindings (Shiboken6 wheel) — container-only pipeline

`./bindings/build_in_container.sh` (docker, mounts the repo):

- (no flag) — configure `-DQMN_BUILD_PYTHON=ON` + build + build-tree smoke and
  qtmonaco parity test; `--wheel` — **the shipping pipeline** (also the CI
  `wheel` job): `pip wheel` → `auditwheel repair --plat manylinux_2_34_x86_64`
  → smoke + parity against the *repaired* wheel with the repo off `sys.path`;
  `--codegen` — run the generator alone and print the generated files plus
  `mjb_rejected_functions.log`. **After adding public view API, run
  `--codegen` and check the rejected log** — silently dropped functions never
  reach Python. Set `REBUILD_IMAGE=1` to rebuild the (slow) image.
- **Pins are load-bearing:** `PySide6-Essentials==6.12.0` /
  `shiboken6_generator==6.12.0` at build time, `~=6.12.0` at runtime — the
  generated wrappers reference Qt_6.12 versioned symbols and versioned
  libpyside/libshiboken SONAMEs. Don't loosen.
- Codegen cannot run on the Arch host (the pip generator binary needs
  `libxml2.so.2`; Arch ships `libxml2.so.16` — do not symlink). The PySide6
  wheel ships **no Qt headers and no `Qt6Config.cmake`**, so `Dockerfile`
  unpacks a real Qt **6.12.0** — fetched from `mirror.yandex.ru/mirrors/qt.io`
  (`download.qt.io` connect-times out from this network, and aqt 3.3.0 can't
  parse the Qt≥6.9 one-package layout — don't try it).
- **One Qt at runtime:** a `DT_NEEDED libQt6Core.so.6` resolves to the
  already-loaded PySide6-bundled Qt, but `dlopen()` *by path* would load a
  second copy. Never load Qt by absolute path, never ship Qt libs beside the
  extension; the wheel smoke asserts one `libQt6Core` in `/proc/self/maps`.
- **Auditwheel exclude lists live in two places and must stay in sync:**
  `build_in_container.sh --wheel` and `[tool.cibuildwheel.linux]
  repair_command` in `pyproject.toml`. The cibuildwheel config is the *future*
  PyPI flow (its before-all Qt bootstrap is missing); the container pipeline
  is what ships.
- Wheel layout: `_qmn.abi3.so` at site-packages root + pure-Python `qmn/`
  shim (`bindings/python/qmn/__init__.py`) which **imports `PySide6.QtWidgets`
  before `_qmn`** (the wrapper resolves its `QWidget` base through shiboken's
  converter registry at import).
- The qtmonaco drop-in class `qmn.Monaco` lives **in the Python shim only**:
  qtmonaco is **1-based** (`set_cursor`, `delete_line`, `set_highlighted_lines`,
  `insert_text`) while C++ `TextPosition` is 0-based — the conversion is done
  in Python, never in C++. Read-only edit doors raise `ValueError` Python-side
  (the C++ doors stay silent).
- Deliberate divergences: `set_vim_mode_enabled` exists and only **warns**
  (vim mode is out of scope forever — never implement it);
  `set_minimap_enabled` maps to the overview ruler, never a Chromium-style
  minimap; `open_diffs` and LSP hooks are absent until Gate 2 — an honest
  `AttributeError` beats a silent no-op.

## Model micro-benchmark (`tools/bench_model.cpp`)

Standalone g++, not part of CMake. Needs `libqmn_core.a` already built, and the
Qt6 include/lib paths below are **Arch Linux paths** (adjust for other distros):

```bash
g++ -O3 -std=c++20 tools/bench_model.cpp -Isrc/core/.. \
    build/src/core/libqmn_core.a \
    -isystem /usr/include/qt6/QtCore -isystem /usr/include/qt6 \
    -lQt6Core -o bench_model && ./bench_model <big-file>
```

The perf-table document (61 MiB / 1,000,001 lines) is regenerable
deterministically — recipe in `README.md` "Model micro-benchmark".
`tools/` also has `bench_highlight.cpp` and `bench_ts_split.cpp`.

## Reference points

- Layering + roadmap + perf targets: `README.md`. Phased plan with DoD and
  risks (gate definitions, parity mapping, decided divergences): `PLAN.md`.
- What shipped per milestone (through v1.5.0): `CHANGELOG.md`.
- Vendored tree-sitter pins + query provenance + the json scm deviation:
  `third_party/tree-sitter/VENDORED.md`.
- Consumer + parity target: `qtmonaco` (GitHub `bec-project/qtmonaco`; local
  sibling `/home/kai/kaigit/qtmonaco`) — the PySide6/`QWebEngineView` editor
  this widget replaces. Its Python API is mirrored by `qmn.Monaco`; parity is
  checked by `bindings/tests/parity_monaco.py`.
- Highlight demo documents and measured span tallies: `examples/README.md`.
