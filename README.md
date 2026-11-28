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
DAP decorations fanning out to every view of a document. Car S1 (1.14.0)
made `qmn-demo` itself the live showcase (`--diff` trio, grammar door,
typed DAP controls, the `--tour` pin table); S2/Py are DEFERRED by owner
decision, and GATE 4 — the IDE dogfood train — is now the open train: cars
I0+I1 (1.15.0) shipped `qmn-ide-demo`, a second app that lives ONLY on the
exported surface, plus the QtCore-only `src/search/` Find-in-Files library
it earned; car I2 (1.16.0) added quick open + the command palette and the
second born-headless library the scorer earned (`src/fuzzy/`); car I3
(1.17.0) took the IDE to LSP depth — definition, references, rename, code
actions, formatting and the outline dock — with ONE new shelf door
(`LspHub::requestDocumentSymbol`, the tenth feature sibling) and zero new
view API; and car I4 (1.18.0) shipped TASKS + THE PROBLEM MATCHER — a
`.qmn/tasks.json` fold, a `QProcess` runner that is NOT a terminal, the
output pane as a REAL read-only `CodeEditor`, two matchers whose regex
arithmetic makes the cross-refusal symmetric, and problems as the results
panel's third producer plus the gutter's tagged diagnostics feed — with
ZERO new glue, ZERO new library and ZERO new view API.

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
src/dap/    DapCodec + DapClient (knows src/protocol ONLY, never view/core;
            car 4)
src/search/ SearchEngine + SearchSession — the Find-in-Files engine
            (QtCore ONLY too — GATE 4 car I1, born headless: a REAL export
            set qmn::search the IDE app links directly; plain-Qt records,
            results over SIGNALS)
src/fuzzy/  FuzzyMatcher — the quick-open/command-palette scorer
            (QtCore ONLY — GATE 4 car I2, the second born-headless
            promotion: real export set qmn::fuzzy, the score law frozen
            at the header, pinned by a fuzz vs a brute-force reference)
src/app/    qmn-spike  — demo/bench harness, depends on view
            qmn-demo   — full demo app: tabbed editors + settings dialog
            (links qmn_glue PRIVATE for the --lsp door; no glue type ever
            crosses its headers beyond a forward-decl)
            qmn-ide-demo — GATE 4's IDE dogfood pair (I0…I4): project
            tree, tabs (one tab per path), Find-in-Files over qmn::search
            (also fed by Shift+F12 references AND the task problem matcher),
            quick open (Ctrl+P) +
            command palette (Ctrl+Shift+P) + the task picker over the
            qmn::fuzzy scorer, the LSP-depth set — F12 definition, F2 rename,
            Ctrl+. code actions, Shift+Alt+F formatting, the OUTLINE dock —
            and TASKS: a `.qmn/tasks.json` fold, a QProcess runner, a real
            read-only editor as the output pane; the window
            lives in the qmn_ide_app STATIC lib + thin ide_main.cpp,
            qmn_glue/qmn_search/qmn_fuzzy linked PRIVATE, all behind
            fwd-decls
tests/      ten ctest entries: textbuffer + diff + search + fuzzy are
            guiless, lsp + glue + dap + codeeditor + demo + ide run on
            QT_QPA_PLATFORM=offscreen
tools/      standalone micro-benches (model, highlight latency tree split,
            search) + probe_clangd.sh (the manual live-server ritual, never
            a CI gate)
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
./build/src/app/qmn-ide-demo <dir>    # GATE 4 IDE: project tree + tabs +
                                      # Find-in-Files + quick open (Ctrl+P)
                                      # + palette (Ctrl+Shift+P) + tasks
                                      # (Ctrl+Shift+B / Ctrl+Shift+R) — a
                                      # FILE as the project fails loud, exit
                                      # 1; --task LABEL runs ONE configured
                                      # task and the CLI's exit code IS the
                                      # task's (a task NEVER auto-runs)
```

Headless smoke/bench run:

```bash
QT_QPA_PLATFORM=offscreen ./build/src/app/qmn-spike --exit-after-ms 2000 bigfile.py
QT_QPA_PLATFORM=offscreen ./build/src/app/qmn-demo --exit-after-ms 300 bigfile.py
QT_QPA_PLATFORM=offscreen ./build/src/app/qmn-ide-demo examples --exit-after-ms 300
```

Highlighting demos, one per grammar (`.py`/`.json`/`.js`/`.c`/`.cpp` suffixes
pick the grammar automatically — `.h`-family headers go cpp; `--language
python|json|javascript|c|cpp` overrides): see `examples/README.md`.

The showcase doors (car S1): `qmn-demo --diff A B` opens the trio (original |
modified | side-by-side diff, focus on the diff tab; F7/Shift+F7 walk hunks in
diff mode), `--grammar <lib.so>` (+ `--grammar-id/-symbol/-lang/-query/
-suffix`, zipped by occurrence) loads a compiled grammar library and rebinds a
suffix at run time, `--dap` + `--debug-step/--debug-continue/--debug-stop`
ride a queued DAP control walk (the flags without `--dap` fail loud), and
`--tour` runs the 14-row scripted pin table over a frozen built-in corpus
(exits nonzero at the first failed pin; a LIVE ritual, never a ctest entry —
`tst_demo` pins the table itself with the server legs forced off). `--lsp`
and `--dap` stay the off-by-default doors they were.

The IDE doors (GATE 4 cars I0…4): `qmn-ide-demo <project-dir>` is the
first consumer that lives ONLY on the exported surface — project dock (Qt's
own QFileSystemModel tree), editor tabs (ONE tab per path: re-opening
FOCUSES), and a Find-in-Files panel over the QtCore-only `src/search/`
library: case/whole-word/regex toggles, an event-loop-paced cancellable
walk with honest stop faces, results grouped per file with per-line rows,
and row activation that opens-or-focuses the file and reveals caret +
selection on the match — the reveal needed a public door the frozen surface
did not have, and GATE 4's ledger rule closed it the additive way:
`CodeEditor::setSelection (anchor, caret)` is now public (the raw the a11y
bridge and the diff machinery already used). Quick open (`Ctrl+P`) rides the
same library's LIST face (`startList` + `fileListed` — LEDGER catch #2's
additive answer: names only, contents never read) and scores with the
second born-headless library `src/fuzzy/` (score DESC, ties by path); the
command palette (`Ctrl+Shift+P`) runs the window's own command registry;
the popup's Esc is child-scoped so the view's frozen Escape stays the
editor's own. Car I3 put the IDE at LSP depth over the shelf's feature doors:
definition (`F12`), references (`Shift+F12` — the SAME results panel, with
`resultsProducer()` naming who owns it), rename (`F2`, applied as ONE undo
group; a refusal keeps the overlay open), code actions (`Ctrl+.` — v1 never
runs a server's arbitrary command, so a command-only row stays visible and
refuses through the applier), formatting (`Shift+Alt+F`) and the outline
dock (the refresh policy is the app's: open / tab switch / a 600 ms edit
debounce / the dock button / a palette command — never the hub's feed).
Car I4 added TASKS + THE PROBLEM MATCHER: `.qmn/tasks.json` next to
`lsp.json` (labels, a command + args, an optional cwd, a problem matcher),
a `QProcess` runner that is deliberately NOT a terminal (one-shot, no shell,
no PTY, no stdin, stdout+stderr merged in true arrival order, one run at a
time, NEVER auto-run, terminate→kill grace, dies at a project switch and at
window close), the OUTPUT PANE as a real read-only `CodeEditor` (a build
log gets the same wrap, selection and search as the editor), task pick
(`Ctrl+Shift+R`) / run default (`Ctrl+Shift+B`) / stop, and TWO matchers
(`gcc`, `msvc`) that refuse each other's rows by construction — the noise a
real build prints stays text, and a compiler naming a file that no longer
exists stays VISIBLE with `openFile`'s own loud refusal at the reveal.
Problems land in the same results panel (its THIRD producer) and in the
gutter under the M5 diagnostics feed, tagged so a clear restores whatever
another producer owned. `--task LABEL` / `--task-default` is the CLI face,
and its exit code IS the task's (`1…255` verbatim, everything else → 1 — a
wrapper that reports success for a failing build is a lie).

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
- [x] Car S1 — showcase (CLOSED, 1.14.0): `qmn-demo` grows into the live
      showcase IN PLACE — no second binary, the 15-knob settings surface
      and the `--lsp`/`--dap` door shapes ride unchanged; four doors:
      `--diff A B` opens the trio (original | modified | side-by-side
      diff, focus on the diff tab) and F7/Shift+F7 walk hunks in diff
      mode (strict-edge, NO wrap at either edge; honest no-op in plain
      tabs), `--grammar` (+ companions, zipped by occurrence) loads a
      compiled grammar library and rebinds a suffix — a bogus library
      refuses loud; `debugStep`/`debugContinue`/`debugStop` demo doors
      over four NEW plain-typed `DapHub` control doors
      (`continueDebugging`/`stepOver`/`stepIn`/`stepOut` — Ready +
      stopped-thread gate, the thread memory retires on `continued`, so
      the wheel drives a session without ever touching the unbound
      `DapClient`), with a `--debug-*` queued CLI ride; and `--tour`
      runs the 14-row scripted pin table over a FROZEN built-in corpus
      (golden 15-span highlight table, exact DAP status walk against
      dapfake's `steps2` mode, presence-shaped clangd/live-adapter
      legs), printing `PASS/SKIP/FAIL n: leg — detail` and exiting
      nonzero at the FIRST failed pin — a LIVE ritual, never a ctest
      entry, while `tst_demo` pins the TABLE itself with the server
      legs forced off; the shelf bumped 1.1.0 → 1.2.0 (S1-3's four
      doors are Python-visible additive growth)
- [~] Car S2 — pydemo mirrors the showcase door-for-door — DEFERRED by
      owner decision 2026-11-28 (the showcase-mirror story lost the
      dogfooding contest; the deferral + revisit records sit at their
      PLAN sections; pydemo keeps riding its headless close-ritual
      smoke either way)
- [~] Car Py — the PyPI half: the cibuildwheel Qt bootstrap + publish
      job — DEFERRED together with S2 (the container pipeline keeps
      shipping the manylinux wheel either way)

Gate 4 — the IDE dogfood train (OPENED 2026-11-28): `qmn-ide-demo` living
ONLY on the exported surface; the deliverable is the LEDGER of API holes
and the headless library seams it earns (first: `src/search/`); no
terminal by owner decision.
- [x] Car I0 — IDE skeleton (CLOSED, 1.15.0): the `qmn_ide_app` STATIC
      lib + thin `ide_main.cpp` over the demo's exact shape; the project
      door carries tree root + workspace root + `.qmn/lsp.json` fold on
      ONE door; ONE tab per path (a re-open FOCUSES, lexical variants
      included); close/reopen leaves no registry squatter; the `--lsp`
      door rides the demo's law whole (tst_ide NEVER spawns a server —
      the one hub slot opens only unclaimed languages); `ide` joins
      ctest offscreen — nine entries now
- [x] Car I1 — `src/search/` + the Find-in-Files panel (CLOSED, 1.15.0):
      the first new layer since Gate 2's five — QtCore-only `qmn::search`
      (pure `scanText`/`scanFile` faces + the paced `SearchSession` walk,
      results over SIGNALS, refusal faces loud and SYNCHRONOUS inside
      `start()`); the non-overlap law binds ACCEPTED matches only —
      rejected wholeWord occurrences SLIDE (the `wholeWordNestedSlide`
      tripwire); `tst_search` GUITLESS: the naive-reference property pair
      on a fixed-seed corpus + astral UTF-16 column legs; the panel's
      rows/refusals/cancel/reveal pinned through tst_ide; LEDGER catch
      #1 found and closed the SAME car — `CodeEditor::setSelection` went
      PUBLIC as the additive reveal channel; the bench record is measured
      (81 ms over a generated 500-file corpus; 142 ms over the repo's own
      tree; the caps, never wall-clock, are ctest-pinned)
- [x] Car I2 — quick open + command palette (CLOSED, 1.16.0): quick open
      (`Ctrl+P`) rides I1's walker through the LIST face — LEDGER catch #2
      (no file-pool enumeration door) found and closed the SAME car:
      `SearchSession::startList` + `fileListed` landed additively, contents
      NEVER read, the I1 signal contract pinned unchanged both ways; the
      `src/fuzzy/` scorer promoted as the second born-headless lib (real
      export `qmn::fuzzy`, own GUITLESS ctest entry — the 10th; the score
      law frozen at the header, a 40 000-pair fixed-seed fuzz vs a
      brute-force reference pins the tie class from BOTH mechanisms); the
      palette (`Ctrl+Shift+P`) rides the window's own command registry
      (close-then-run, registry-order ties); the chord-table audit rode as
      a tripwire (no frozen view chord re-bound, Key_P/Key_W absent from
      the view's table, the popup Esc child-scoped); symbol quick-open
      DECIDED — NOT shipped, the `requestWorkspaceSymbol` seed rides to I3
- [x] Car I3 — LSP depth + outline (CLOSED, 1.17.0): the IDE's six depth
      features ride the ALREADY-SHELF — five existing `request*` doors + the
      four `apply*` appliers + I1's public `setSelection` reveal, plus ONE
      new shelf door, the tenth feature sibling `LspHub::requestDocumentSymbol`
      (raw `Reply`, LOCAL `featureGate` refusals, `documentSymbolProvider`
      gate, `initialize` now declares `hierarchicalDocumentSymbolSupport`)
      — which drops from the wheel BY TYPE ABSENCE (the tenth honest
      C++-only absence; the three codegen copies untouched, the shelf
      literal stays 1.2.0). F12 definition (caret at the token START),
      Shift+F12 references (the Find-in-Files panel's SECOND producer over
      the engine's own FileMatch currency, `resultsProducer()` names the
      owner), F2 rename (ONE undo group per document via `applyReplyEdit`;
      a refusal keeps the overlay open; the prefill is the app's own
      word/selection math), Ctrl+. code actions (`applyCodeAction` incl.
      the WorkspaceEdit hidden in `command.arguments[0]`; command/create
      items stay visible and refuse THROUGH the applier — v1 never runs a
      server's arbitrary command), Shift+Alt+F formatting, and the outline
      dock with the app-side refresh law (open / tab switch / 600 ms edit
      debounce / dock button / palette) — both wire shapes decode, the flat
      one REBUILT from `containerName` (the rebuild ADMITS itself in the
      summary), a generation counter retires superseded pulls, refusals
      clear the dock LOUDLY, and a claimed document with a still-handshaking
      server gets a BOUNDED out-loud wait because the hub cannot tell a slow
      server from a dead spawn at the door. Far sides: the fakeserver's
      marker-protocol `documentsymbol`/`symflat` legs, three `tst_lsp`
      machines (byte witnesses both shapes + the flush-before-ask proof +
      LOCAL refusals with the reqCount silence witness), and the live clangd
      probe's three new depth legs (PROBE-OK, 14 steps, clangd 23.1.5).
      THE SPAWN GRANT (owner, 2026-11-28): tst_ide's depth drills DO spawn
      the CI-built deterministic fakeserver under the built-in spec NAME
      ("clangd" replaced whole — PATH clangd stays unspawned); every other
      slot keeps the I0 no-spawn law. No new VIEW API; no Ctrl+click jump
      (no view click-out door); `requestWorkspaceSymbol` STILL unshipped —
      no I3 feature asked for a workspace-wide list, it rides to the car
      that asks
- [x] Car I4 — tasks + problem matcher (CLOSED, 1.18.0): `.qmn/tasks.json`
      read by APP code at the ONE config door (tree root + workspace root +
      `lsp.json` + `tasks.json`), a `QProcess` runner that is NOT a terminal
      (one-shot, no shell/PTY/stdin, `MergedChannels` for TRUE arrival order,
      one run at a time, NEVER auto-run, terminate→kill grace, dies at a
      project switch and at window close), the OUTPUT PANE as a real
      `CodeEditor` under the M19 read-only lock (readOnly is an OPTIONS knob —
      the view's own setter stays private; the lock refuses typing/undo/redo,
      copy + selection + the programmatic replace stay free), a paced
      whole-text replace with a 2000-line tail cap + VISIBLE hidden count (the
      APPEND door NOT taken — the close MEASURED the replace: ~183 µs at the
      cap, ~0.09 µs/line, so a 60 ms burst is ~0.4 % of a core; the tail
      follow is the superlinear half, which is the real argument for the cap),
      two matchers `gcc`/`msvc` (+ `none`) refusing each other's rows by
      REGEX ARITHMETIC (no name table), the noise a real build prints staying
      TEXT, the PHANTOM row visible with `openFile`'s own refusal at the
      reveal, and problems as the results panel's THIRD producer
      (`resultsProducer()` gained "problems"; severity/message app-side, the
      search records untouched) plus the M5 feed tagged `source = "qmn-tasks"`
      riding the DapHub merge/restore posture — with `LspHub`'s whole-list
      `published` paint EVICTING task rows RECORDED as the coexistence limit,
      never pinned as a pass. THREE caps are class constants a test can name
      (2000 pane / 500 panel rows / 2000 feed — the panel cap never caps the
      gutter), the skip faces are COUNTED AND SHOWN, and the new mutation
      control on the panel cap says the pin is load-bearing. `qmn::fuzzy`'s
      THIRD user is the task picker (third popup on the I2 overlay law), SIX
      palette appends (10 → 16), TWO new chords (Ctrl+Shift+B / Ctrl+Shift+R)
      both mask-free. ZERO glue, ZERO new library, ZERO new view API: the
      `--codegen` rejected-FUNCTION sets are byte-identical when sorted (1825
      lines) and the generated sets IDENTICAL at 47 files, ZERO app lines,
      shelf literal 1.2.0. Far side: `tests/faketask/` (CI-built,
      `QMN_TEST_FAKE_TASK`, exit code the oracle, it WRITES the file its own
      diagnostics name) — and NO PATH COMPILER is EVER spawned by ctest; the
      real-toolchain story is the live ritual (real g++: 2 rows from 8 lines;
      the matcher vs a naive pass over the SAME real build text: 9 vs 9).
      THREE fixes the drills earned: ONE Esc QAction per popup (a second made
      the shortcut map AMBIGUOUS — reproduced), the project-switch reset ORDER
      (retire the finished run's faces, THEN kill), and the paced replace's
      selection reset documented. The CLI's exit code IS the task's. tst_ide
      28 → 44 slots; ctest stays TEN
- [ ] Car I5 — git gutter (THE OPEN CAR; ledger question: an M5 gutter-stripe
      kind — check the five decoration records before declaring)

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
