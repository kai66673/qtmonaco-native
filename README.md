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
re-measured at the 1.12.0 close 2026-11-23 on the shipping flags — the
exact document is reproducible, recipe under "Model micro-benchmark"
below):

| Operation | Result |
|---|---|
| `setPlainText` (whole document) | ~9 ms after file read (~110 ms I/O + decode cold, ~2 ms cached) |
| 20,000 random edits (insert/delete) inside the document | mean ~11 µs/op, ~30k pieces after the run |
| worst single edit | ~25 ms — always the FIRST edit of the run (measured: op #0 of 20,000): the first split of the one loaded piece re-counts newlines over its half-piece; later ops are µs |

The core is under constant property-test attack: 2,000 random
insert/delete operations with a naive `QString` reference and a full
treap-invariant check after **every** operation (`tests/tst_textbuffer.cpp`).

Deliberately **not implemented yet** (see roadmap): full
grapheme segmentation (`col` still counts UTF-16 code units even though
arrow keys and edits snap to cluster boundaries), markdown rendering in
the hover popup (plain text by design) and semanticTokens PAINTING (the
decoder exists, tree-sitter stays the paint master). Soft wrap, the diff
view, horizontal scrolling with column memory, scrolling past the end,
the whole LSP stack (protocol → client → syncer → app-side hub → popups)
and the whole DAP stack (codec seam → dap layer → DapHub on the glue
shelf, breakpoints/frames/value hints via the M5 decorations) all shipped
in 1.7.0 – 1.9.1; cars B/C/D/E of Gate 3 (1.10.0 – 1.13.0) added the
word-level inline diff wash + weighted diff rows, signatureHelp, LSP folds,
LSP inlay hints and pull diagnostics, any number of editor views over
one document (shared text/undo/parse, per-view carets and popups), and
`qmn.DapHub` + the plain-typed answer doors across the Python wheel with
DAP decorations fanning out to every view of a document.

## Layout & layering

```
src/core/   TextBuffer — pure Qt-based C++ (QtCore ONLY), headless, testable
src/diff/   LineDiff — pure line diff + alignment table (QtCore ONLY too)
src/protocol/ JsonFraming + transports + RpcClient (QtCore + QtNetwork, NEVER
            view/core — the M8 layer rule; LSP/DAP glue lives in src/glue)
src/lsp/    LspClient + DocumentSyncer + PositionConverter + WorkspaceEdit
            (knows src/protocol ONLY, never view/core)
src/view/   CodeEditor — the raw QWidget canvas (QPainter), depends on core
src/glue/   LspHub (+ ProjectSession, G2) — the LSP glue: a CLIENT of view,
            lsp AND protocol, never the reverse; promoted out of src/app by
            the G1 surgery (2026-11-21) into its OWN export set qmn::glue
            with its own semver; car 4's DAP glue lands on this shelf
src/app/    qmn-spike  — demo/bench harness, depends on view
            qmn-demo   — full demo app: tabbed editors + settings dialog
            (links qmn_glue PRIVATE for the --lsp door; no glue type ever
            crosses its headers beyond a forward-decl)
tests/      six ctest entries: textbuffer + diff are guiless, lsp + glue +
            codeeditor + demo run on QT_QPA_PLATFORM=offscreen
tools/      standalone micro-benches (model, highlight latency tree split) +
            probe_clangd.sh (the manual live-server ritual, never a CI gate)
third_party/ vendored tree-sitter runtime + generated grammars (M4; the
            qmn_ts static lib links PRIVATE into qmn_view — tree-sitter
            never crosses src/core/ or the public view API)
```

Layering rules are contractual: `core` must never include QtGui/QtWidgets.
This keeps the model benchmarkable and fuzzable without a display.

## Build

Requirements: CMake ≥ 3.22, a C++20 compiler, Qt6 ≥ 6.6 (Core/Gui/Widgets/Network/Test).

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

Highlighting demos, one per grammar (`.py`/`.json`/`.js`/`.c`/`.cpp` suffixes
pick the grammar automatically — `.h`-family headers go cpp; `--language
python|json|javascript|c|cpp` overrides): see `examples/README.md`.

### Python demo (pydemo/)

`pydemo/` is a `uv` project mirroring `qmn-demo` door-for-door in Python on
top of the shipped wheel (the same tabbed UI, the same settings-template
semantics, the same `--exit-after-ms` headless contract). Build the wheel
first (`./bindings/build_in_container.sh --wheel`), then:

```bash
cd pydemo
QT_QPA_PLATFORM=offscreen uv run main.py ../examples/demo.py --exit-after-ms 400
uv run main.py [files...]                    # the visible app
```

### Editor setup (VS Code / Code OSS)

`.vscode/` is gitignored, so each dev configures their editor locally; the
house style for it (the setup used against the Arch `code` OSS build):

- **IntelliSense: clangd**, reading the `compile_commands.json` the root
  CMake always exports (`build/`). The server actually serving this workspace
  is the **kylin-clangd** extension's bundled clangd (its own fork); the
  official clangd extension (`clangd.path: /usr/bin/clangd`) is configured as
  the alternative — never run both servers at once. MS cpptools is not
  installable on Code OSS — keep its IntelliSense disabled if it shows up.
- **`.clangd`** (tracked): every clangd server reads it. Arch GCC 16 injects
  `-mno-direct-extern-access` into every compile line and CMake bakes it into
  the compile db; clang drivers don't know it, and the kylin-bundled clangd
  publishes it as a bogus line-0 error. The config removes the flag from the
  parse command and suppresses that `drv_unknown_argument` diagnostic —
  IntelliSense only, the GCC build itself stays untouched.
- **CMake Tools**: generator Ninja, build dir `build/`, `CMAKE_BUILD_TYPE`
  Release, test environment `QT_QPA_PLATFORM=offscreen` — i.e. the extension
  drives exactly the canonical CLI commands above.
- **Debug: CodeLLDB** (`lldb` from the Arch `clang` package; cpptools' debug
  adapter is not installable on OSS) — launch configs for `qmn-spike`,
  `qmn-demo` (headless + visible) and the two widget-test binaries, all
  offscreen by the ctest convention.

## Roadmap (hobby-scale, see estimates in the qtmonaco discussion)

Milestones, in build order. **Gate 1** (editor v1: M1b…M13 + M12 completion)
ships a self-contained editor usable from C++ and PySide6; **Gate 2** (IDE
surface) starts only after the v1 API freeze — it CLOSED 2026-11-21 with
tags `v1.9.0`/`v1.9.1`; **Gate 3** (debt paydown + the live C++/Python
showcase) opened that same day — details in *PLAN.md*.

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
- [x] E3 usability polish before the freeze: M14 Tab/auto-indent ✔ · M15 ✔
      (smart Home + double/triple-click + Ctrl+D) · M16 context menu + text
      DnD ✔ · M17 font/zoom API ✔ · M18 auto-close pairs + long-line ruler +
      current-line style ✔ · M19 CodeEditorOptions settings object ✔
- [x] M11 accessibility (QAccessible bridge: role EditableText, full
      text + editable-text interfaces over the raw document frame, M13
      view-map geometry, live events; final polish stays non-gating)
- [x] M12 PySide6 binding + wheel: expose `CodeEditor` via Shiboken6
      (scikit-build-core + cibuildwheel), production abi3 wheel +
      C++ install/export — the v1 API freeze (see PLAN.md Phase F)

Gate 1.5 — usability interlude (post-freeze minor train, see PLAN.md;
✔ closed with M24 — released as one minor `v1.5.0` covering M20–M24):

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
- [x] M23 language-scoped properties (tab width first; overrides ride
      above the editor settings — Monaco's `[languageId]` model;
      public `languageProperties(id)` + tri-state child QObject, built-in
      registry starts EMPTY, settings mirror/applyEditorSettings stay
      template-based)
- [x] M24 themes (chrome colors + token map on the `CodeEditorTheme`
      child QObject — 28 ink roles + a `TokenKind`→QColor map, tri-state
      with palette fallbacks so the default theme keeps today's pixels
      (upgrade-identical, tripwired by the untouched paint suite) plus
      explicit Light/Dark presets and a kind tracker; all "set_theme
      routes it" inks finally routed; demo dialog theme combo)

Gate 2 — IDE surface (after the freeze; **CLOSED 2026-11-21**, v1.9.0 +
v1.9.1):

- [x] M8 LSP client (stdio JSON-RPC via QProcess) — completion, diagnostics,
      hover, plus the whole write path (the all-or-nothing `WorkspaceEdit`
      applier, rename, formatting, the `willSaveWaitUntil` save round), the
      view-side completion/hover popups, the app-side `LspHub` glue (server
      matrix + `.qmn/lsp.json`) and the qtmonaco parity hooks (`add_action`,
      `update_workspace_configuration`); live-clangd probe PROBE-OK 7/7;
      the popup TRIGGERING + completion ACCEPT + shared-HOVER +
      MOUSE-HOVER follow-ons shipped after the 1.8.0 close (debounced
      hub-side triggers; VS Code-style row nav and accept — the view
      emits, the glue applies the insertion; the hover popup also carries
      the hovered line's diagnostics with their `[source]` labels, and
      rides either the still caret or a still mouse pointer)
- [x] M10 diff view → `open_diffs` parity (the line-diff engine + the two-
      column chrome + the wheel/shim crossing; inline word-level highlight,
      wrap×diff budget math and original-side wrap ride a follow-on M10b)
- [x] Soft wrap (the deferred half of M9) — the M25 car
- [x] Grammar extensibility (a post-1.8.0 follow-on): a process-wide runtime
      grammar registry behind additive `CodeEditor` doors — register a parser BY
      ADDRESS (`qintptr`, type-erased), load a compiled grammar library
      (`loadGrammar`, tree-sitter's `tree_sitter_<name>` convention), UPSERT with
      a live rebind into attached editors (a custom `python` may shadow the
      vendored one), capture-role overrides above the fixed role table, and
      unregister -> plain text; the five vendored grammars seed the table
- [x] The glue shelf + `ProjectSession` (the post-M8 G-train, before DAP):
      `src/app/glue` promoted into a REAL second shelf — its own export set
      `qmn::glue` with its own semver (`find_package(qmnGlue)`) — and a new
      document session on it: the uri ↔ editor ↔ language registry, the
      suffix table with overrides, the `.qmn/lsp.json` fold for BOTH sides
      (the top-level `languages` map finally has an owner; `servers` still
      reach the hub), one call to open a document (hub open + popup
      triggers included), and the demo tabs rewritten as its presentation
- [x] G3 — the shelf at the wheel boundary (PLAN's "G3 design pass"):
      `qmn.LspHub` (+ `ServerSpec`) and `qmn.ProjectSession` ride the wheel
      — the first typesystem/CMake/codegen-CLI growth beyond the view; the
      AUTOMATIC LSP half (config fold, lazy start, sync, diagnostics, popup
      triggers, accept, server-primed applyEdits + relays) is Python-live;
      the callback-carrying feature doors stay C++-only, honest absence
- [x] DAP features via decorations (car 4, the last Gate-2 machine): the
      `src/dap/` layer over the protocol envelope seam (`EnvelopeCodec` —
      LSP bytes byte-identical) + `DapHub` on the glue shelf: verified
      breakpoint dots via `setBreakpointLines` (unverified ones become
      `source:"dap"` Warnings, the desired store survives sessions), the
      caret + frame band at stopped (`setPosition` + `setHighlightedLines`),
      scope values as `setInlayHints` at word-end; the demo `--dap` door
      and the live-adapter probe ritual (lldb-dap AND gdb green); DAP does
      NOT cross the wheel yet — the hub doors are shiboken-shaped by design

Gate 3 — debt paydown and the live showcase (opened 2026-11-21, the day
Gate 2 closed; see PLAN.md for the car-by-car design record):

- [x] Car 0 — the recorded defect pair (CLOSED, 1.9.2): the undo-mark
      drift (it rides the accessible deleteText/replaceText door, the
      record's typeover theory superseded) + the overview-ruler perf
      cliff on one-piece docs (fixed in the CORE: lazy per-piece
      newline-offset lists + window re-cuts at splits, bench tripwire
      601 ms → indexed)
- [x] Car B — M10b (CLOSED, 1.10.0): word-level inline diff highlight
      (`WordDiff` + the shared Myers seam + two new Theme roles),
      wrap×diff budget math (the row axis goes WEIGHTED, one budget
      covers both sides), original-side wrap (the read column wraps with
      the knob — clicks ride their fragment; the "triple-click is the
      way to take a long line" divergence NARROWS)
- [x] Car C — M8b (CLOSED, 1.11.0): signatureHelp (the 7th feature door,
      typed `(`/`,` triggers), LSP inlay hints (hub feed into the
      already-bound M5 door, zero new view API), `foldingRange` over the
      M13 fold API (ONE fold provider per editor — the C1
      `setFoldRanges`/`clearFoldRanges` seam is the car's only surface
      widening), pull diagnostics (declare-then-pull, one shared decoder
      with the push route, refusal falls back to push)
- [x] Car D — multi-observer core (CLOSED, 1.12.0): ANY number of
      `CodeEditor` views over ONE `CodeDocument` (the attach ctor
      `CodeEditor(document, parent)` is the car's only surface widening —
      attach at ctor only, no detach door, and the document must outlive
      its views); the core's single edit-observer became an observer LIST
      with the document as the one multiplexer (its mux slot re-asserted
      LAST, so the single-view signal order stays byte-identical); ONE
      shared tree-sitter parse per document with a per-surface viewport
      set (two views at opposite ends parse both halves, and one keystroke
      reparses ONCE for both); `LspHub`/`ProjectSession` went
      document-scoped — a second view JOINS the registration (one
      `didOpen`, one syncer, one monotone version stream whoever types),
      fold/hint/diagnostic paints and hub dismissals fan out over the view
      list, and the owner view's death hands the wire over instead of
      closing it
- [x] Car E — wheel-2 (CLOSED, 1.13.0): DAP crosses the wheel —
      `qmn.DapHub` joins the Python shelf (no new record types: the hub's
      two structs are PRIVATE, so only one entry per copy of the
      three-copies lists grew) + the signal+plain-typed ANSWER routes
      cross as three doors (`LspHub::answerShowMessageRequest` — resolved
      through the READ side, an answer door never spawns a server — and
      `DapHub::answerRequest`/`answerRequestError`), while the 17
      `LspHub` feature doors stay honest C++-only absences; the DAP
      alias-paint divergence car D named is CLOSED —
      `ProjectSession::editorsForUri` fans dots, dap diagnostics, the
      stopped band and the value hints out to EVERY live view of the
      document (the caret stays owner-only), and the live DAP ritual ran
      through the wheel on both lldb-dap and gdb `-i=dap`; the
      rejected-log audit was re-pinned FROM the archived logs — the "16
      drops" baseline was a set name, never a line count (the real set:
      34 `LspHub` + 1 `ProjectSession` + 9 `DapHub` lines)
- [ ] Car S1 — `qmn-demo` grows into the live showcase: diff door,
      grammar door, DAP controls, `--tour` (a scripted pin table that
      exits nonzero on the first failed pin; live ritual, never CI)
- [ ] Car S2 — pydemo mirrors the showcase door-for-door (same flags,
      same tour legs — needs car E first)
- [ ] Car Py — the PyPI half: the cibuildwheel Qt bootstrap + publish
      job, DoD a test.pypi.org wheel smoke

Out of scope by design: vim mode. The `set_vim_mode_enabled` setter is still
exposed by the Python binding for API compatibility, but only warns (see
*PLAN.md*).

License: BSD-3-Clause (see `LICENSE`; the parent project qtmonaco is
BSD-3-Clause too). The vendored tree-sitter runtime and grammars stay under
their own MIT licenses - every upstream `LICENSE` file is kept in place under
`third_party/tree-sitter/` (see its `VENDORED.md`). The PySide6 wheel carries
this same BSD-3-Clause over our code only; consumers link PySide6/Qt under
their own licenses, as usual.

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
