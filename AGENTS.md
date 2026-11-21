# AGENTS.md

Native code editor widget for Qt6, built from scratch on a raw `QWidget`
(own piece-table model, own QPainter rendering). No Scintilla, no
QPlainTextEdit, no Chromium. Long-term hobby project; roadmap in `README.md`,
phased plan in `PLAN.md`, shipping history in `CHANGELOG.md`.

## Current state (v1.8.0 — M8 CLOSED 2026-11-19) — gates and the frozen surface

- Gate 1 (editor v1) froze the public API at tag `v1.0`; Gate 1.5 (usability
  train M20–M24) closed 2026-11-12 at tag `v1.5.0`. **Gate 2 is the open
  train** since 2026-11-15, ordering decided: M25 soft wrap (SHIPPED) → M10
  diff (cars 1–3 built: engine + view + chrome) → M8 LSP/protocol (**CLOSED
  2026-11-19**, seven machines — see the PLAN "M8 … CLOSED" paragraph) →
  **the glue shelf G1+G2 (DECIDED 2026-11-21, ordered BEFORE car 4 by the
  user — see the PLAN "glue shelf + ProjectSession" record)** →
  **DAP, the one Gate-2 car still open**. The full design record is the
  PLAN.md "GATE 2" section — read it before starting
  a car. Never start work for a later gate while an earlier one is open.
- The frozen surface widens **only additively** (Gate 1.5 precedent): no new
  view mapping/geometry primitives. The M5 decoration API already covers
  diagnostics, breakpoints and inlay hints. Two sanctioned Gate-2 deviations
  (decided 2026-11-15, both in PLAN): the `CodeEditor::buffer_` value member
  moves into `CodeDocument` (ownership surgery, not API surgery), and the
  `visible*` row doors change UNITS (lines → visual slots) without adding
  doors. M8 kept that rule literally: `applyTextEdits`, the six popup doors
  and machine 7's four doors are ALL additive, and the popup anchors reuse
  the existing `xForCol` + characterRect math.
- Both deliverables ship: a C++ static package (export set `qmn::core` /
  `qmn::view`, plus internal `qmn::internal_tree_sitter`, since M8 the
  REAL exports `qmn::protocol` / `qmn::lsp`, and since the G1 promotion
  (2026-11-21) the SECOND-shelf export `qmn::glue` — its own export set +
  `qmnGlueConfig.cmake`, see "Current state" and the layering rule below)
  and a PySide6 wheel
  `qmn-native` (cp310-abi3, manylinux_2_34). See "Python bindings" below.
- **The package version has exactly two literals** — `project(qtmonaco-native
  VERSION …)` in `CMakeLists.txt` and `version` in `pyproject.toml` — and the
  close ritual bumps BOTH in one commit (the 1.8.0 close found them drifted:
  1.6.1 vs 1.7.0). The hub's LSP `initialize` `clientInfo.version` is
  CONFIGURED from `PROJECT_VERSION`, never hand-kept. **G1 exception:** the
  glue SHELF versions itself with a third literal, `QMN_GLUE_VERSION` in
  `src/glue/CMakeLists.txt` (a plain variable, NOT a nested `project()` —
  that would reset `PROJECT_VERSION` and poison the clientInfo contract) —
  it bumps ONLY when the shelf itself breaks, never in the package close
  ritual. **G3 made the shelf's semver real to Python users too** — the
  wheel carries `qmn.LspHub`/`qmn.ProjectSession` (`qmn::glue 1.0.0`):
  shelf breakage is now a wheel-visible story, not only a C++ one.
- **Keep public view API Shiboken-wrappable**: `Q_OBJECT`, plain Qt types in
  params/returns, no C++ templates or STL types crossing the API boundary
  (internals are unconstrained). The binding wraps the **view** widget AND
  (since G3) the **glue shelf's** public doors; `core`/`protocol`/`lsp`
  stay C++-only and need no Python type. The wrappability rule now binds
  the SHELF too: a new glue door that Python should reach speaks plain Qt
  types — `std::function` callbacks and `qmn::lsp`/`qmn::protocol` types
  in a signature drop the door SILENTLY (the G3 audit counted 16 such
  drops); a Python-facing answer route needs a SIGNAL, not a callback.

## Hard rule: layering is contractual

Enforced only by convention + CMake link deps — nothing checks it automatically:

- `src/core/` (`TextBuffer`) — **QtCore ONLY**. Never include QtGui/QtWidgets.
  This is what keeps the model headless, benchmarkable and fuzzable.
- `src/view/` (`CodeEditor` + `CodeEditorOptions`/`Theme`/`LanguageProperties`
  child QObjects, `HighlightEngine`) — core + QtWidgets + PRIVATE vendored
  tree-sitter (it never crosses core or the public view API).
- `src/app/` — view only (`qmn-spike` bench/spike harness and `qmn-demo`
  tabbed demo app). `qmn-demo`'s window/dialog logic lives in the
  `qmn_demo_app` STATIC lib (`demo_main.cpp` is a thin main) because
  `tst_demo` links the lib and drives it offscreen. **G1 promotion
  (2026-11-21) retired the old M8 amendment:** the hub is no longer app
  code — `src/glue/` is a real layer (see the Gate-2 layers bullet), and
  the clause is now "app targets LINK the `qmn_glue` target; no glue type
  ever appears in an app-defined header beyond the forward-decls already
  there". `qmn_demo_app` links `qmn_glue` PRIVATE — qmn-demo's `--lsp`
  door** (built-in clangd spec + `.qmn/lsp.json` + popup triggers on c/cpp
  tabs). The door is OFF by default and MainWindow.hpp forward-declares
  `qmn::glue::LspHub` behind a `unique_ptr`,
  so `tst_demo` never sees an lsp type and never spawns a server (clangd is
  not a CI guarantee; the one demo slot that arms the hub only opens
  languages NO built-in spec claims).
- Gate-2 layers (decided 2026-11-15, built with the owning cars — since M10
  and M8 `src/diff/`, `src/protocol/` and `src/lsp/` are CODE, and car 4's
  machine D2 (2026-11-21) made `src/dap/` CODE too): `src/diff/` — **QtCore
  ONLY** (line-diff + alignment,
  headless like core). `src/protocol/` — QtCore+QtNetwork, **never view**.
  `src/lsp/` and `src/dap/` — only `src/protocol/`, **never view**. The
  view/core never depend on protocol/lsp/dap; LSP/DAP *glue* (decoration
  feeding, syncer wiring) lives in `src/glue/` (G1 promotion 2026-11-21:
  the old `src/app/glue/` became a REAL layer — `src/glue/` is a CLIENT of
  core+view+protocol+lsp (D3 grows it into a `qmn_dap` client too),
  QtCore/QtWidgets, exported as `qmn::glue`; it
  knows NO app/demo type, and the DAP glue of car 4 lands ON this shelf
  (D3 2026-11-21: `DapHub` joined the shelf as its third member —
  LspHub + ProjectSession + DapHub; `qmn_glue` PUBLIC-links `qmn_dap`
  because the hub header names a `DapClient*` door)).
  **M10 amendment (decided
  2026-11-16): `qmn_view` PRIVATE-links `qmn::diff`** — the pure line-diff
  layer is QtCore-only like core, not a network layer; no diff type crosses
  any public view header (the diff doors take `CodeDocument*`/`QString` and
  view-nested types only), and the static-package link rides the
  `qmn::internal_tree_sitter` mechanism (internal export target, link-only,
  consumers never link it themselves). `CodeDocument` (M25) joins
  `src/view/` as an editor-owned child QObject and owns the `TextBuffer`.
- `pydemo/` — a uv project, NOT built by CMake: the Python analog of
  `qmn-demo` running on the shipped wheel (path dep into gitignored
  `dist/`; `uv.lock` stays uncommitted). Mirrors the C++ demo door-for-door
  including the 15 settings knobs (14 via `options()`, the M24 theme preset
  via `theme()`).

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
- **The grammar registry is RUNTIME-extensible** (post-1.8.0 sliver): the old
  static array became a process-wide `QVector<LangEntry>` in
  `HighlightEngine.cpp` seeded from the vendored five; engines reconcile
  against a generation counter inside `refreshIfNeeded` (SAME language
  pointer = tree kept, only query/roles/folds rebuilt). The `CodeEditor`
  doors (`registerGrammar`/`loadGrammar`/`grammarAddress`/`unregisterGrammar`/
  `setCaptureRole`) type-erase the parser as a `qintptr` ADDRESS —
  tree-sitter types stay out of the public header; loaded grammar
  libraries are NEVER unloaded.

## Build & test (C++ host path)

Requirements: CMake ≥ 3.22, C++20, Qt6 ≥ 6.6 (Core/Gui/Widgets/Network/Test).
Ninja is what CI/README use. CI is the source of truth: `.github/workflows/ci.yml`
(archlinux container + ubuntu-24.04, both Release).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build --output-on-failure
```

Seven ctest tests: `textbuffer` (core property tests), `diff` (the pure
line-diff engine, M10), `lsp` (M8: JsonFraming + transports + RpcClient, then
the fakeserver/LspClient/DocumentSyncer/LspHub machines — this binary GROWS
machine by machine through M8, no further ctest entries inside the car),
`glue` (G2: `ProjectSession` over REAL editors — resolution, registry,
config fold, the attached-but-unclaimed hub path; NO server ever spawns,
the claimed-server machinery is tst_lsp's fakeserver turf), `dap` (car 4:
the EnvelopeCodec seam, then the DapCodec/DapClient machines, then the
D3 `DapHub` shelf machines over REAL editors — same
GROWS-machine-by-machine rule, `tests/dapfake/` is a support binary NOT
a ctest entry, its exit code is the far-side oracle), `codeeditor`
(widget tests), `demo` (drives the qmn-demo surface programmatically).
`textbuffer` and `diff` are guiless; since M8 machine 4 the `lsp` binary
drives real `CodeEditor`s too (and D3 flipped `dap` to
`QTEST_MAIN`), so ctest sets
`QT_QPA_PLATFORM=offscreen` for `lsp`, `glue`, `dap`, `codeeditor` and `demo`
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
  `mjb_rejected_functions.log`. **After adding public view/GLUE API, run
  `--codegen` and check the rejected log** — silently dropped functions never
  reach Python. Set `REBUILD_IMAGE=1` to rebuild the (slow) image.
- **Bound-type lists live in THREE copies and must stay in sync:** the
  typesystem's type entries, `bindings/CMakeLists.txt` (`HEADERS` + the
  `_generated` TU names), and the hand-reproduced codegen CLI in
  `build_in_container.sh --codegen`. The 1.6.1 run caught the drift for
  real: adding `CodeEditorTheme` to typesystem+CMake but not to the
  script's header list produced "type ... specified in typesystem, but not
  defined" and NO theme wrapper TU; the 1.7.0 machine-6 run caught the
  OPPOSITE drift (typesystem yes, `_generated` TU list no) — that configures
  CLEANLY and ninja silently compiles nothing for the type: it surfaces at
  container smoke import as `undefined symbol: init_qmn_view_...`. What the
  binding wraps today: the four
  QObjects (`CodeEditor`/`CodeEditorOptions`/`CodeEditorTheme`/
  `CodeDocument`, ctors removed — editor-owned; the M10 standalone
  `CodeDocument()` stays C++-only, the diff view's detached original is fed
  through the string door instead), `TextPosition`, the eight nested record
  structs (the five decoration records + M8 m5's `TextEdit` write record +
  M8 m6's `CompletionItem`/`HoverRange` popup payloads) and every nested
  enum (M8 m7 added NO new type — its doors are plain
  `QString`/`QStringList` on the already-bound `CodeEditor`, so the three
  copies were untouched). **Since G3 the shelf crosses too** (all THREE
  copies grew glue entries): `qmn::glue::LspHub` +
  `qmn::glue::ProjectSession` — PUBLIC ctors, unlike the editor-owned view
  QObjects, the app builds and owns them — and the nested
  `LspHub::ServerSpec` record. The shelf half Shiboken can't translate
  drops BY TYPE ABSENCE (no remove rules): the eight `request*`/
  `resolveCompletionItem` feature doors (`FeatureHandler` = std::function)
  + `setTransportFactory`, the `syncerFor`/`clientFor*` returners
  (`qmn::lsp`/`protocol` types stay unbound — wrapping them would drag the
  whole protocol closure in), and the four `apply*` doors (the defaulted
  `LspClient*` tail). Those 16 must be EXACTLY what the rejected log shows
  beyond the usual Qt/tr noise — the post-codegen audit checks it.
  `RpcClient::Reply` stays unbound (its only carriers were the callbacks);
  `QJsonObject`/`QJsonValue` cross as DIRECT params/members for the first
  time — smoke-pinned, never assumed. Otherwise only `PerfStats` and the
  SlotEdit/UndoMark/std::function machinery stay unbound by design.
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
- **The image's `dejavu-sans-mono-fonts` package is load-bearing:** the
  editor's default is `systemFont(FixedFont)`, and a font-package-less
  Rocky 9 base resolves "monospace" to a PROPORTIONAL fallback (space 4.4px
  vs `w` 11.4px measured), silently skewing the monospace cell arithmetic —
  the M10 parity ORIGINAL-column clicks landed ~2 cols off their word
  (2026-11-17). The gesture pins assume a real monospace, like any real
  user base has.
- **One Qt at runtime:** a `DT_NEEDED libQt6Core.so.6` resolves to the
  already-loaded PySide6-bundled Qt, but `dlopen()` *by path* would load a
  second copy. Never load Qt by absolute path, never ship Qt libs beside the
  extension; the wheel smoke asserts one `libQt6Core` (and since G3 one
  `libQt6Network` — the shelf drags protocol's `TcpTransport`) in
  `/proc/self/maps`.
- **Auditwheel exclude lists live in two places and must stay in sync:**
  `build_in_container.sh --wheel` and `[tool.cibuildwheel.linux]
  repair_command` in `pyproject.toml`. The cibuildwheel config is the *future*
  PyPI flow (its before-all Qt bootstrap is missing); the container pipeline
  is what ships.
- Wheel layout: `_qmn.abi3.so` at site-packages root + pure-Python `qmn/`
  shim (`bindings/python/qmn/__init__.py`) which **imports
  `PySide6.QtWidgets` and `PySide6.QtNetwork` before `_qmn`** (the wrapper
  resolves its `QWidget` base through shiboken's converter registry at
  import; the QtNetwork DT_NEEDED the G3 shelf adds must resolve against
  the ALREADY-LOADED bundled Qt — QtWidgets does not load it on its own).
- The qtmonaco drop-in class `qmn.Monaco` lives **in the Python shim only**:
  qtmonaco is **1-based** (`set_cursor`, `delete_line`, `set_highlighted_lines`,
  `insert_text`) while C++ `TextPosition` is 0-based — the conversion is done
  in Python, never in C++. Read-only edit doors raise `ValueError` Python-side
  (the C++ doors stay silent).
- Deliberate divergences: `set_vim_mode_enabled` exists and only **warns**
  (vim mode is out of scope forever — never implement it);
  `set_minimap_enabled` maps to the overview ruler, never a Chromium-style
  minimap; `open_diffs` SHIPPED with M10 (the plain-QString door + the
  shim; uri/options params accepted-and-ignored); the M8 m7 pair
  (`add_action`, `update_workspace_configuration`) SHIPPED in 1.8.0 on the
  view doors `addContextAction`/`setStoredWorkspaceConfiguration` (m7's
  precedent: plain-Qt-typed doors on an ALREADY-bound class need NO
  three-copies edit — only new record types do);
  `set_lsp_header`/`get_lsp_header` stay absent ON PURPOSE even after M8 —
  an honest `AttributeError` beats a silent no-op (the reason is recorded in
  PLAN's M8 machine-7 paragraph: a wire-only prepended header is a ±H offset
  tax on all six LSP paths).

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
