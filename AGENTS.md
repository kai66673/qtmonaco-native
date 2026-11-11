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
- **Shiboken codegen does not run on the Arch host.** The pip generator binary
  needs `libxml2.so.2` and `libxslt.so.1`; Arch ships `libxml2.so.16`
  (incompatible ABI — do not symlink). Run binding codegen in a manylinux/
  RHEL9-base container (`bindings/Dockerfile`), which also supplies `clang-libs`
  — the wheel's libclang is statically linked but has no resource dir of its
  own, so point `CLANG_INSTALL_DIR=/usr` at clang-libs' `/usr/lib/clang/<v>/
  include` or ApiExtractor fails on builtin headers. Keep C++ dev + CTest on
  the host.
- **The PySide6 wheel ships no Qt headers and no `Qt6Config.cmake`** — only
  `PySide6Config`/`Shiboken6Config`, PySide6's own wrapper headers, and Qt
  *runtime* libs under `PySide6/Qt/lib`. There is no `qwidget.h`/`qglobal.h`
  anywhere in it. So an extension build always needs a real Qt install for
  headers, `moc` and link libraries; `bindings/Dockerfile` pins Qt **6.12.0**
  (exact match for the Qt PySide6 6.12.0 bundles). Configure also needs
  `libglvnd-devel` or `Qt6Gui` fails via `WrapOpenGL`.
- **Qt download:** `download.qt.io` and `master.qt.io` connect-timeout from this
  network; `mirror.yandex.ru/mirrors/qt.io` answers in 0.32 s from inside the
  container (`ftp.fau.de/qtproject` 0.40 s works too). RHEL9 has no Qt6 dev
  packages, so Qt is unpacked from the repository's own archives.
- **Qt ≥ 6.9 Linux desktop is one package** (`qt.qt6.6120.linux_gcc_64`), and
  aqt 3.3.0 **cannot parse it** ("The packages ['qt_base', 'qtbase'] were not
  found", with or without `-m qtbase`) — `bindings/Dockerfile` instead curls the
  pinned archive pair straight from the mirror's rev directory and verifies the
  mirror's `.7z.sha1` sidecars. Only `qtbase-…-RHEL_9_6-GCC-….7z` + the **ICU**
  bundle are needed; the archives are `RHEL_9_6` based, which lines up with our
  manylinux_2_34 target. In these new archives there are **no flat forwarding
  headers at `include/` top level** (unlike old `gcc_64`): only per-module dirs,
  so a bare `#include <QWidget>`/`<QElapsedTimer>` needs that module's
  `include/Qt<Module>` dir on the search path.
- **Single-Qt-at-runtime rule (measured on glibc):** a `DT_NEEDED
  libQt6Core.so.6` is satisfied by an **already-loaded object of that SONAME**, so
  the extension binds to the Qt bundled in the PySide6 wheel even though it was
  *linked* against a different Qt 6.12.0 build and carries a RUNPATH to it (two
  builds, one `liba.so.1` SONAME probe: the dependent lib resolved to the
  first-loaded instance). But `dlopen()` **by path** of a same-SONAME library
  loads a **second copy**. So never load Qt by absolute path and never ship Qt
  libs beside the extension; the smoke test asserts one `libQt6Core` in
  `/proc/self/maps` — it passes for both the build-tree and installed-wheel
  smokes. PySide6's wheel Qt has SONAME `libQt6Core.so.6` and bundles **ICU 73**;
  Qt's exported CMake interface mentions no `ICU::` targets, so no `libicu-devel`
  is needed (RHEL9's ICU 67 headers would only poison the link interface).
- **Binding macro gotchas (Shiboken 6.12.0, measured):** `shiboken_generator_
  create_binding()` forwards `-I` only for the *listed* `QT_MODULES` targets'
  own interface dirs (`Qt6::Widgets` = `[include, include/QtWidgets]`), so
  Core/Gui dirs must be added via `SHIBOKEN_EXTRA_OPTIONS` (one `-I` per dir —
  a multi-dir property expands into bare positional args otherwise). Compiling
  the generated code needs `PySide6/include/PySide6/Qt{Core,Gui}` subdirs for
  the `pyside6_qtgui_python.h` include chain. `typesystem_widgets.xml` comes
  from `PySide6/typesystems` (`PYSIDE_TYPESYSTEMS` is set only by the abi3
  config flavour). `PySide6::pyside6` links no Qt — our CMake links Qt6 itself.
- **Wheel layout:** `_qmn.abi3.so` sits at site-packages root (a `qmn/`
  subpackage does NOT put `_qmn` on `sys.path`), with the pure-Python `qmn/`
  shim importing `PySide6.QtWidgets` **before** `_qmn` — the generated wrapper
  resolves its `QWidget` base through shiboken's converter registry at import.
  scikit-build-core spells the option `cmake.build-type`, not top-level
  `build-type`.
- **Wheel audit facts (measured, Phase F):** scikit-build-core 1.1.2 maps
  `wheel.py-api = "cp310"` onto the wheel tag without touching the compile
  (the build was ALWAYS abi3 — libshiboken's INTERFACE_COMPILE_DEFINITIONS
  carry `Py_LIMITED_API=0x030a0000`). `auditwheel repair --plat` wants the
  FULL tag (`manylinux_2_34_x86_64`, not the policy name — an invalid-choice
  error otherwise) and emits a DUAL tag when the wheel also qualifies lower
  (auto verdict here `manylinux_2_24`: the extension ELF references only
  GLIBC ≤ 2.14 once PySide6-provided libs are `--exclude`d — excludes prune
  whole ldd subtrees, which is how Qt's ICU deps vanish too; libGLX/libOpenGL
  are direct DT_NEEDED but not on the 2_34 lib_whitelist, so they'd be
  grafted unless excluded). `install.components = ["python"]` +
  `COMPONENT python` on the extension's install rule keeps the C++ install
  tree (`include/`, `lib64/*.a`) out of the wheel — sbc then runs
  `cmake --install --component python`, and ONLY that component lands.
  patchelf comes from PyPI (RHEL9 repos lack it) and both auditwheel+patchelf
  bake into `/opt/venv` (runs execute as `--user`, cannot pip at runtime).

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
- **M4 engine is vendored tree-sitter** (pinned v0.27.2, ABI 15). Fed
  `TSInputEncodingUTF16LE` straight from piece-table chunks — never transcode.
  A tree-sitter byte offset (and `TSPoint.column`, which is byte-based too) is
  **exactly 2×** our UTF-16-code-unit offset; the `get_column()` scanner
  callback counts code points instead, but that never crosses our boundary.
- **Highlight merge convention (M4.1, user decision): tree-sitter master's
  LATER pattern wins** for identical spans (sort tie-break pattern_index
  ascending in `rebuildSpans`). The pinned py/js queries rely on it
  (catch-all `(identifier) @variable` first). Consequences: the runtime does
  NOT evaluate query predicates — its cursor emits every syntactically
  matching capture (probe-proven) — so the engine evaluates `#eq?`/`#match?`
  itself (`buildQueryPredicates`; predicate capture ids are GLOBAL
  name-table ids, shared across patterns in v0.27.2, same namespace as
  cursor indices — no remapping; directives/#is-not? pass as satisfied;
  a QRegularExpression compile failure drops the PREDICATE, fail-open,
  so the role survives unfiltered rather than being lost). And vendored
  json highlights.scm is LOCALLY REORDERED (pair-key capture last —
  recorded in VENDORED.md); py/js scm stay byte-upstream. Tripwire tests:
  `jsonKeyQueryPatternWinsOverPlainString`, `queryPredicatesFilterRefinedRoles`.
- **`.scm` queries are embedded at CMake CONFIGURE time** (`configure_file`
  in `third_party/tree-sitter/CMakeLists.txt`); the three query files are
  `CMAKE_CONFIGURE_DEPENDS` of that dir, so a query edit re-runs configure
  on the next `ninja` automatically (before that fix, incremental builds
  silently kept the OLD embedded query).

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
- **Two product gates (PLAN.md):** Gate 1 = editor v1 (no LSP/DAP/diff/soft
  wrap); Gate 2 = IDE surface after the v1 API freeze. Never start Gate-2
  work while Gate 1 is open; the M5 decoration API must already cover
  diagnostics, breakpoints and inlay hints so Gate 2 needs no view surgery.
- CI is the source of truth for the supported build: `.github/workflows/ci.yml`
  (archlinux container + ubuntu-24.04, both `-DCMAKE_BUILD_TYPE=Release`).
- Deliberately out of scope: vim mode.
