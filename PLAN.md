# PLAN

Working plan for qtmonaco-native. Milestones map to the `README.md` roadmap
(M0…M13) and obey the constraints in `AGENTS.md` (layering rule, Shiboken-
wrappable public API, no vision ⇒ numeric assertions only). Toolchain facts
that were *measured* (container, Qt download, macro gotchas, single-Qt rule)
live in `AGENTS.md` — this file is the phase plan, not the fact log.

## Status (2026-11-09, baseline `9666f71`)

Done: **M0** spike (piece table + painter + caret + typing + v-scroll + perf
stats), caret-drift fix (fractional cell metrics), **M1** selections, bench
tool, **Phase B** thin binding spine (wheel + single-QtCore ABI proof),
**M4 engine decision** (vendored tree-sitter). Next up: **Phase A — M1b
clipboard**, then Phase C.

## Product model — two gates (decided 2026-11-09)

**Gate 1 — Code Editor v1.** A complete, self-sufficient native editor
widget: graphemes/IME, undo/redo + multiple carets, syntax highlighting,
gutter + decorations, find/replace, overview ruler, horizontal scroll, code
folding. **Exit DoD:** the same widget usable from **C++** (installed +
CMake-exported package, frozen public headers) **and PySide6** (production
manylinux wheel, plain `pip install` works). No LSP, no DAP, no diff view,
no soft wrap inside Gate 1.

**Gate 2 — IDE surface.** LSP client, DAP-aware features, diff view
(`open_diffs`), soft wrap, remaining qtmonaco parity hooks. Starts only
after the v1 API freeze. Design-forward requirement: the Gate-1
decoration/inline API (Phase D3) must already cover diagnostics,
breakpoints and inlay hints — Gate 2 must never re-operate on the view.

Why this shape: the editor alone is a shippable product (writing code
without an IDE is most of real usage), while IDE integration carries its own
failure surface (server/process lifecycle, protocol churn) that must not
gate the editor release. The thin-binding-slice-first logic still holds:
Python bindings freeze the C++ API, and packaging failure modes are cheap to
find early, expensive late — which is why Phase B ran before feature depth.

**Release train** (counterweight to Gate 1's length, decided 2026-11-09):
every completed milestone gets a git tag `v0.x` and an alpha wheel from the
container into `dist/` (its `cp311` tag stays cosmetic until Phase F's real
abi3 retag). Keeps the wheel pipeline hot and progress tangible.

**Hard ordering rules inside Gate 1:**
- **M2 graphemes before any binding widening** — position semantics change;
  API-freeze rule (risk #6).
- **M4 tree-sitter before M13 folding** — fold ranges come from the AST.
- **M9 h-scroll before M13 folding** — E1/E2 together are one deliberate
  view-line-mapping effort (model line ↔ visible line map), not scattered
  features. Folding makes lines *disappear* from the viewport; h-scroll
  already failed once (risk #3).

# GATE 1 — Code Editor v1

## Phase 0 — toolchain unblock (**done**, 2026-11-09)

Host reality: Qt **6.12.0** system + PySide6 **6.12.0** venv (they coincide —
luck, not design), glibc 2.44, Docker works. **Shiboken codegen cannot run on
the Arch host** (pip generator needs `libxml2.so.2`; Arch ships
`libxml2.so.16` — never symlink it). Resolution actually used and now pinned
in `bindings/Dockerfile` + `bindings/build_in_container.sh`: **rockylinux:9
container** (the RHEL9 base manylinux_2_34 is built on) runs codegen,
compile, smoke and wheel; C++ dev + CTest stay on the host. The venv that
bootstrapped it (Arch's system python3 has no pip; `uv` is on PATH):

```bash
uv venv .venv                                  # picked CPython 3.13.18
uv pip install --python .venv "PySide6-Essentials==6.12.0" \
    "shiboken6_generator==6.12.0" scikit-build-core ninja cmake
```

## Phase A — M1b clipboard

copy/cut/paste, middle-click X11 primary, paste replaces selection.
**DoD:** state-based tests for copy→paste round trip, primary selection, and
paste-over-selection; no surrogate pair split.
**Watch:** X11 clipboard under `QT_QPA_PLATFORM=offscreen` may not work —
that test may need a real/Xvfb display (qtmonaco itself uses `pytest-xvfb`).
Gate it rather than making the suite depend on a display.

## Phase B — thin binding spine (**DONE** 2026-11)

Bound exactly one class (`CodeEditor`) + smallest stable surface; the macro
`shiboken_generator_create_binding()` + scikit-build-core produce
`_qmn.abi3.so` + `qmn/` shim; container chain green end-to-end; wheel smoke
asserts exactly one `libQt6Core.so.6` in `/proc/self/maps`. All measured
gotchas (macro signature and its `-I` limits, `CLANG_INSTALL_DIR`,
`libglvnd-devel`, pinned Qt archives, wheel layout) are in **AGENTS.md** —
re-read them before touching `bindings/`. The pair PySide6 6.12.0 / Qt 6.12.0
is pinned there. The bound surface stays *thin* until Phase C lands.

## Phase C — M2 text correctness (must precede binding widening)

Grapheme/word boundaries + IME composition. Use **QTextBoundaryFinder**
(public since Qt 4; `Grapheme`/`Word` BoundaryTypes). History of this line:
README once said "QStringConverter iterators" (wrong: those are codec
classes), and the earlier "correction" to **QTextBoundaryRunner (Qt ≥ 6.9)**
was itself wrong — **no such class exists in any Qt release** (verified
2026-11-09 against the v6.9.0 `src/corelib/text` tree and against the host
Qt 6.12 headers + `nm -D libQt6Core`: only `QTextBoundaryFinder` is shipped).
ZWJ/emoji cluster correctness comes from the ICU backend: Arch's Qt and the
PySide6-bundled Qt are both ICU builds (fallback tables predate the GB11 ZWJ
rule) — keep `find_package(Qt6 6.6)` as is, no 6.9 floor is needed for M2.
Arrow Left/Right move by grapheme clusters; Backspace/Delete remove whole
clusters (superseding the surrogate-pair-only guards, still view-only); word
navigation (Ctrl+arrows, Ctrl+Del) is **editor-style** (word/space/punct
class runs, VS Code semantics; deliberately not the ICU `Word` boundary,
which splits `foo_bar` and stops on spaces), targets snapped to cluster
boundaries. `col` numbering **stays UTF-16 code units** (LSP semantics —
deliberate).
**DoD:** numeric assertions for ZWJ emoji sequences, combining marks,
surrogate pairs; no screenshots (`AGENTS.md`).
**Status: DONE (2026-11-09).** Boundaries + word motion (9 tests: clusters
move/delete as units, mid-cluster carets snap without orphans,
`ctrlDeleteNeverSplitsClusters`) and IME composition (preedit is
display-only at the caret, commit goes through `insertTextAtCaret`;
`ImCursorRectangle` trails the preedit). Phase C complete → binding
widening may start.

## Phase D — editing depth

### D1 — M3 undo/redo, then multiple carets

Undo/redo over the piece table (op log with inverses; snapshots as periodic
compaction). **Multiple carets deliberately inside Gate 1** (decided
2026-11-09): a v1 editor without them looks incomplete; generalize the
selection model to a list. Alt+Click / Alt+Drag add carets; typed edits,
Backspace and selection ops fan out per caret; undo groups one user action
into one step.
**DoD:** undo/redo round-trip property test against a naive reference (same
style as `randomOpsVsReference`); multi-caret state tests (N carets → one
edit → N model edits; caret-normalization on overlapping selection).
**Status: M3 DONE (2026-11-09)** — op log in `TextBuffer` (coalescing by
pr-hole adjacency, bracketed compound groups, 2048-group/32MB caps), view
mirror marks restore caret+selection per step; 11 core tests (incl.
`randomUndoRedoVsReference`, the DoD property test) + 11 widget tests.
Multiple carets: next, D1 stays open until they land.
**D1 COMPLETE (2026-11-09)** — multiple carets landed: Alt+Click/Alt+Drag
gestures, per-slot fan-out for typing/Enter/Backspace/Delete/word-delete/
clipboard/navigation, one bracketed undo group per key press with every
slot restored from the undo mark, normalization rules (stack fold,
contained-selection death, bare-caret-inside-selection death) and 19 state
tests in `tst_codeeditor.cpp`. Documented v1 divergences (overlap keeps the
right edit, no paste line-distribution, no Alt+click remove) are pinned by
tests.

### D2 — M4 syntax highlighting: vendored tree-sitter (engine decided)

Three options were considered; facts verified against tree-sitter v0.27.2
sources (released 2026-11-07):

- **A. KF6 `KSyntaxHighlighting`** — not installed; a KF6 framework dragging
  KDE cmake infra + KConfigCore/KCoreAddons into the dependency set.
  Problematic for the wheel deliverable: PySide6 wheels bundle no KDE libs,
  so we would ship KF6 runtime libs + definition XMLs beside `_qmn`. Only
  attractive if the wheel stops being first-class.
- **B. Own Monarch-style line highlighter** — zero deps, UTF-16 native,
  mirrors the Monaco look (Monarch is Monaco's own engine). But no syntax
  tree: nothing reusable for folding, smart-select, M10 structural diff or
  M8 assist; every language definition ours forever.
- **C. Vendored tree-sitter** (runtime + committed generated `parser.c` per
  grammar) — pure C, compiles straight into the library/`_qmn` (single
  amalgamation TU `lib/src/lib.c` + one `parser.c` per grammar), wheel-safe,
  no runtime deps. Verified fit: `TSInputEncodingUTF16LE` is first-class and
  the lexer advances by encoded-unit size, so a tree-sitter byte offset is
  **exactly 2×** our UTF-16-unit offset (see AGENTS.md) — no transcoding; the
  pull-model `read` callback hands out piece-table chunks (QString data *is*
  UTF-16LE on LE) zero-copy. Incremental built-in: `ts_tree_edit` +
  reparse-with-old-tree, `ts_parser_set_included_ranges` for
  viewport-scoped parses. (`TSParseOptions.progress_callback` was on this
  list as a time-slicing mechanism; measured later it is cancel-only —
  cancelling returns NULL and throws the parse away, no resumable state —
  so it buys nothing for our latency story.) ABI 15 runtime accepts
  grammars ≥ 13; pin the vendored copy, commit generated parsers (CLI only
  needed to regenerate).

**Decided: C** — pin v0.27.2, with a thin engine seam so grammar-less modes
(plain text, …) keep a trivial fallback. A and B stay as the recorded
rationale.

First steps: vendor `third_party/tree-sitter` (v0.27.2, verified archive);
grammars python + json + javascript first (c later), generated `parser.c`
committed; `HighlightEngine` seam in `src/view/` (core stays untouched —
tree-sitter never enters `src/core/`). The open threading sub-decision was
settled by bench, not taste — numbers and verdict in the DONE block below.
**DoD:** capture ranges from `.scm` queries map to model offsets under
numeric assertions (feed edits, compare token spans to a reference on
`QString`); incremental reparse stays O(edit neighborhood), proven by the
bench tool on a big file.

**Status: M4 DONE (2026-11-10)** — vendored tree (see
`third_party/tree-sitter/VENDORED.md` for pins + measured API gotchas:
inconsistent point/byte pairs in `TSInputEdit`/`TSRange` poison the parser;
`TSPoint.column` is byte-based = 2× our units). Engine per plan; core got
QtCore-only hooks only (`setEditObserver` fires exactly one `EditEvent`
per primitive piece-table mutation, `pieceAt`/`PieceView` zero-copy
reader). DoD met by 12 M4 slots in `tst_codeeditor.cpp` (exact span
tables for all three grammars, pattern tie-breaks, window clipping,
surrogate pairs, incremental quotes+undo, ink/geometry pixel scans, and
`largeDocViewportPolicyKeepsSpansExact` pinning the policy contract).

Threading/perf sub-decision, **bench-settled** (4k-line synthetic docs,
dev machine, `tools/bench_ts_split.cpp` + `tools/bench_highlight.cpp`):

| grammar    | cold full parse | incremental reparse / keystroke | full-doc query        | viewport parse+query |
|------------|-----------------|----------------------------------|-----------------------|----------------------|
| python     | ~24 ms          | 22.9 ms (subtree reuse ~nil — indent external scanner) | 6.2 ms (37k captures) | ≤0.7 ms |
| json       | 17.3 ms         | 1.08 ms                          | 11.2 ms (36k capt.)   | ≤0.6 ms |
| javascript | 32.2 ms         | 3.3 ms (7.2 ms when edits hit keywords) | 22–24 ms (196k captures!) | ≤1 ms |

Verdict: **no threads for editor v1** (deferred to M13 — folding needs
full trees anyway, and the snapshot-copy handoff cost measured trivial;
a concurrent parser would also need read-only piece-table views). Full
incremental reparse fits the 16 ms frame for json/js but not python, so
`HighlightEngine` is adaptive instead: full-incremental until the
*measured* parse exceeds the 8 ms budget, then viewport-range parses
anchored at the nearest col-0 non-blank line above the viewport
restores top-level context for column-sensitive grammars (python);
capture scoping via `ts_query_cursor_set_byte_range` is always-on once
the document ≥ 16k UTF-16 units and a viewport has been painted.
Scroll re-parses the window in 0.04–0.5 ms. Correctness: the window
tree's captures inside the window are **identical** to the full tree's
(bench, all three grammars) and the public-API contract is pinned by
`largeDocViewportPolicyKeepsSpansExact` — spans served from the window,
by re-query on the current tree, or by exactly one synchronous
full-document escalation when a programmatic request reaches past the
window (then the next paint flips back to Viewport).

### D3 — M5 decoration layers + line-number gutter + markers

Decoration API is the **design-forward bridge to Gate 2** (decided
2026-11-09): three consumer classes must be expressible without new view
primitives — (1) gutter glyph layer (diagnostics icons, breakpoint dots,
fold triangles), (2) whole-line layer (current line, highlighted lines,
stack-frame line), (3) inline text layer (underlines/wavy lines, inlay
hints rendered after/between text). Gutter: line numbers with the
qtmonaco modes (off/on/relative/interval, `set_line_numbers_mode`), current
line, M4 markers ride layer (1).
**DoD:** state tests — decoration add/update/remove round-trips, gutter
width growth at 9→10 lines, relative-mode math; renderer stays
viewport-only (no off-screen cost beyond the ruler's scaled view).

### D4 — M6 find/replace

Inside Gate 1 because Chromium-Monaco ships its own find widget — a drop-in
user must not lose it. Ctrl+F bar, `QRegularExpression`, match highlights
via decoration layer (3), `F3`/`Shift+F3`, replace + replace-all with
confirm step.
**DoD:** match ranges as model ranges (numeric assertions incl. surrogate
safety); replace-all is one undoable D1 action.

## Phase E — viewport mapping (one deliberate effort, two steps)

### E1 — M7 overview ruler + M9 horizontal scroll (**done**, 2026-11-10)

Both halves shipped (M7 ruler in `7d4b656`, M9 h-scroll right after).
NOTE: the planned "study the failed `2809945` attempt" step was explicitly
overridden by the owner — M9 was implemented **clean-slate** instead and
landed green in one pass (123-slot suite, zero regressions). Deliverables: h-scrollbar + caret-keep-visible on
long lines, `set_scroll_beyond_last_line_enabled` flag, M7 overview ruler
(`set_minimap_enabled` maps onto it — decided) showing the full-document
minimap of lines/markers/folds.
**DoD:** numeric viewport assertions (x-offset after arrow/scroll, caret
scroll-into-view), ruler geometry state tests; no off-screen paint cost.

### E2 — M13 code folding (**done**, 2026-11-10)

Shipped with the single **view-line map** as designed: hidden runs live in
a prefix table (`HiddenRun{first,last,hiddenBefore}`), and paint, caret
nav (arrows + Page keys step slots), selection, gutter, ruler, v-scroll
space and hit tests all route through it. Fold sources: tree-sitter `@fold`
captures (py/js/json, windowed at paint, whole-doc only on explicit
actions); the indent fallback runs in grammar-LESS modes only — the
first cut unioned it everywhere and js/json got python-style
indent pseudo-blocks (owner fix, 2026-11-11; the multiline bracket
folds the fallback used to fake are now real grammar captures). UX: 18px gutter fold zone with ▾/▸ triangles
(clicks toggle, caret untouched) + Ctrl+Shift+[/] and Ctrl+Alt+[/] chords;
ONE reveal guard (programmatic moves into a hidden line open the fold;
drags across folds never do). Fold state rides edits via the buffer
observer (shift/die rules, undo-consistent; the fold itself is not an
undoable action — v1 stance); an edit that kills a fold's SOURCE
(grammar capture / indent pair) AUTO-EXPANDS the collapsed fold on the
next paint — owner symmetry ask, 2026-11-11 — the source re-check rides
the paint-window fold-cache recompute, keeping it O(viewport).
Suite: 136 slots (13 M13), spike smoke
on 22k lines stays O(viewport).
Fold ranges from tree-sitter `@fold` captures per grammar + an
indent-based fallback for grammar-less modes. Fold/unfold via gutter
triangles (D3 layer 1) and keyboard. This is where lines *disappear* from
the viewport: introduce a single **view-line map** (model line ↔ visible
line, fold ranges) and route *everything* through it — painting, caret
navigation (folds jump as units), selection, gutter, ruler, h-scroll from
E1. Folding is only correct if E1's mapping landed first.
**DoD:** numeric state tests — visible-line count after fold, map lookups
across fold boundaries, caret jump over a fold, unfolded content unchanged,
ruler reflects fold ranges; large file stays O(viewport) via D2's
included-ranges story.

### E3 — usability polish (M14–M19, pre-freeze; owner package 2026-11-11)

Owner-requested usability bundle, deliberately BEFORE the Phase-F freeze:
every item below adds public API, and post-freeze view surgery is a
Gate-1 design bug per the PLAN rules. Decisions taken with the owner:
long-line ruler is a FIXED column (default off), settings move into a
`CodeEditorOptions` QObject child (existing setters stay as thin
delegates — qtmonaco parity names untouched), current-line highlight
defaults ON with a stronger style (enum), Ctrl+D rides the existing
multi-caret machinery. Bundles (one commit each, tests per bundle):

- **M14 Tab indent + Enter auto-indent.** Tab inserts spaces to the next
  tab stop (`tabStopWidth`, default 4 — soft tabs; pasted `\t` glyphs
  render to the same stop grid via `QTextOption`), Tab/Shift+Tab block
  indent/outdent across every line a selection touches, Enter copies the
  header line's leading whitespace and adds one level after a trailing
  opener `{([:`. `setTabChangesFocus(bool)` (default false) keeps
  keyboard users able to leave the widget (a11y lever).
  **M14 COMPLETE (2026-11-11)** — landed as specced plus two refinements
  proven in test: block indent covers ALL selection middle lines (end
  line excluded only at col 0), Enter at col 0 stays plain, and Enter's
  pair split (`{|}`, `}|{`, `{|` at EOL CONJURING the missing closer,
  `|}` at col 0) parks the caret on the child line via a per-slot
  `caretTrim` on the fan-out commit; Alt+Enter is a plain-`\n` escape
  hatch. 10 new slots, 146/0/2 suite, spike smoke green.
- **M15 caret/mouse gestures.** Two-tap smart Home (first-non-blank ⇄
  col 0; End stays EOL); double-click selects the word (M2 word
  boundaries), drag then extends word-by-word; triple-click selects the
  line (repeat wraps to the next); Ctrl+D adds the next occurrence of
  the word under the caret as a caret slot (word only, never regex; no
  match → select-word first, VS Code order).
  **M15 COMPLETE (2026-11-11)** — landed as specced, no new public API;
  test-proven refinements: smart Home is POSITION-DERIVED (zero editor
  state, resets on any other motion), double-click/word-drag ranges are
  ONE pure class run via a right-pivoted `wordSelRange` (naive
  wordLeftCol+wordRightCol pairing spans two runs at a class boundary —
  wordLeftCol is Ctrl+Left navigation, not a selection bound), chains
  ride event timestamps + landing spot (Qt folds fast click runs into
  DblClick events; a re-press resets), Ctrl+D is Ctrl-only and has a
  CONVERSION branch (caret at its word start turns the primary into the
  word selection instead of stacking a duplicate slot). 6 new slots
  (152/0/2) + a TextPosition QDebug streamer in the test file.
- **M16 context menu + text DnD.** Right-click QMenu (Cut/Copy/Paste/
  Select all, paste-clipboard vs primary), enabled-state per selection;
  internal drag of a selection via `createMimeDataFromSelection` +
  `startDrag` (move by default, Ctrl copy, Alt copy — decided: Ctrl
  copy like Qt widgets), external text drops accepted
  (`Qt::TextDrop`/`qmn-internal-mime` marker), drop caret tracked,
  autoscroll during drag rides the M1 helper.
  **M16 COMPLETE (2026-11-11)** — landed as specced, no new public API;
  deviations recorded by measurement: hand-rolled press-hold arm inside
  the live selection replaces the QAbstractScrollArea drag hooks (press
  does NOT collapse; <startDragDistance moves swallowed; release
  collapses at the release point); mime = text + private
  `application/x-qmn-selection` marker (`hex(this):start:end`, Move
  preferred, Ctrl drop degrades to copy); internal-move gate = own id
  AND marker offsets == live primary selection (foreign/stale degrade
  to copy); drop STRICTLY inside the source range = caret no-op, the
  overlapping del+ins pair never reaches `applyBlockEdits` (contract:
  non-overlapping edits); urls payloads ACCEPT as text (Qt mirrors the
  uri-list into `text()`); menu factory + caret rules are a protected
  test seam (QMenu::exec blocks offscreen tests); offscreen traps:
  `QClipboard::mimeData()` is null until first fill (null-guard!),
  Qt 6.12 `QApplication::notify` routes synthetic DragMove/Drop only to
  the QDragManager currentTarget (accepted DragEnter first), and
  `QDrag::exec` returns NoAction synchronously. 9 new slots (161/0/2).
  **Post-1.0 DnD polish (2026-11-12)** closes two gaps with no public
  API change: the drop-target CARET is now painted (the "drop caret
  tracked" the spec always promised was only the re-derived drop offset —
  enter/move park private document-coordinate state, painted with the
  caret's 2px rule without blink gate, cleared by a new `dragLeaveEvent`
  override and by `dropEvent` itself), and Shift-at-drop forces COPY
  (target-side Shift-aware acceptance at all three protocol stages —
  the answered action is what `drag.exec()` sees, so the source-side
  Move-delete stays disarmed — plus move-recognition bypass at drop;
  Qt's own Shift-inversion is only a source-side default choice, not
  something the target can rely on). 2 new slots (186/0/2).
- **M17 font/zoom API.** `setCodeFont(QFont)`/`codeFont()` recompute
  cell/line metrics + invalidate every geometry/fold cache (today
  metrics die at construction — QWidget::setFont silently desyncs);
  zoom = a factor over the base font: `zoomIn()/zoomOut()/resetZoom()`
  + Ctrl+= / Ctrl+- / Ctrl+0 and Ctrl+wheel. Shiboken-clean (QFont is
  a registered PySide6 type).
  **M17 COMPLETE (2026-11-11)** — landed as specced; effective font =
  `scaleFontForZoom(baseFont_, zoomFactor_)` mirrored into
  QWidget::font(), PreferNoHinting imposed inside the helper so
  changeEvent's internal-vs-external test reproduces it exactly; a
  FOREIGN raw setFont REBASES (new base at factor 1.0). Chords are
  explicit gates, not QKeySequence::ZoomIn/Out (offscreen defaults
  bind only "Ctrl++"/"Ctrl+-" and miss Ctrl+=, measured); Key_Period
  is the Ctrl+Numpad0 reset spelling (Qt 6.12 has no Key_Kp* —
  numpad reuses the codes + KeypadModifier, tolerated; Alt excluded).
  Ctrl+wheel = FONT zoom (sub-notch accumulator, max 5 steps/event,
  scroll fully suppressed, plain wheel resets the acc). Chrome
  geometry stays live off cellWidth_ (no extra invalidation);
  docWidthPx_ stays ambient (M14 precedent). Zoom chains are exact
  in binary (1.25 = 5/4), tests never round-trip through 0.8.
  Offscreen trap: QWheelEvent's Orientation ctor is GONE in 6.12
  (single 10-arg ctor) and an explicit null device segfaults the
  QSinglePointEvent ctor — lean on the default. 6 new slots (167/0/2).
- **M18 COMPLETE (2026-11-11): auto-close pairs + long-line ruler +
  current line.** Typing
  `([{`'` wraps the selection, inserts the pair at a bare caret, and
  skips a typed closer when it is already there; backspace on a paired
  empty pair removes both. `setLongLineRulerColumn(int)`/`0=off` paints
  a fixed guide line at the column (rides h-scroll, chrome-independent);
  `CurrentLineMode { Off, Background, BackgroundFrame }` replaces the
  bool (bool setter kept as delegate), default `Background`, wash ink
  strengthened.
- **M19 COMPLETE (2026-11-11): `CodeEditorOptions` (settings object).**
  Child QObject returned by `CodeEditor::options()`; every knob (gutter
  mode, current line, ruler column, tab stop, scroll-past-end,
  ruler/minimap, readonly, auto-indent/auto-close flags,
  tab-changes-focus…) lives there with notify-on-change; CodeEditor
  listens and applies. Existing flat setters become ONE-LINE delegates
  (API never breaks); NEW settings exist only on the options object.
  Shiboken: plain QObject, plain Qt types, enums as nested Q_ENUM —
  wrappable without typesystem surgery.
  LANDED AS: mirror-proxy (dev decision, deliberate deviation from the
  literal "options-as-store / CodeEditor listens" wording above): the
  editor is the single source of truth — options setters delegate INTO
  the editor's existing setters (validation/clamping/invalidation
  inherited, getters never stale), and the editor's setters emit the
  options' `*Changed` signal — one notify per change through EITHER
  door, silent on same-value re-sets. Flat setters kept verbatim (they
  ARE the store); new knobs (autoCloseEnabled, readOnly) live only on
  options. Same DoD: 10 knobs + signals, Shiboken-clean, no Q_PROPERTY.
- **M11 COMPLETE (2026-11-11): QAccessible bridge.** Full text interface
  over the final frozen surface: role EditableText, document/caret/
  selection queries through the M13 view map, characterRect/offsetAtPoint
  geometry, editable-text methods, live TextCursor/TextSelection/
  TextInsert/TextRemove events.
  LANDED AS: all of the above plus documented contracts — offsets are
  the RAW TextBuffer frame (UTF-16 code units incl. '\n', no trans-
  lation layer); selection is the ONE primary (extra carets AT-in-
  visible, Gate-1); Word boundaries are line-local ('\n' ends the
  hunt); Line/Paragraph before-frame is the Qt default's EXACT frame
  (verified against 6.10's textLineBoundary helper), NoBoundary-before
  yields the prefix where Qt fails (friendlier, documented); Sentence
  delegates to the Qt base; Remove events carry POSITION only (no MB
  payloads); editable methods honor M19 readOnly and move the primary
  caret; factory key taken from `staticMetaObject.className()` (Q_OBJECT
  in a namespace = QUALIFIED name — a hand-spelled "CodeEditor" never
  matches); events gated on `QAccessible::isActive()`; attributes()
  empty (no syntax roles across the boundary until Gate 2). 4 new
  slots (181/0/2).

Order above = dependency order (options object LAST so the new knobs of
M14–M18 land in it once; a11y LAST-LAST so it mirrors the frozen API).
Each bundle: ctest green + spike smoke on `big_py.py` unchanged.
New API discipline: plain Qt types at the boundary (Shiboken), behavior
defaults documented on the setters (freeze-bound).

## Phase F — ship v1 (gate exit) — ✔ COMPLETE (v1.0.0, 2026-11-12)
- **Python:** cibuildwheel matrix on `manylinux_2_34_x86_64` (match PySide6's
  own baseline) + `auditwheel` repair; real **abi3** wheel tag (`cp310-abi3`,
  Python floor 3.10); pin `PySide6>=6.12` in wheel metadata; the `cp311`
  cosmetic tag from Phase B dies here.
  ✔ (measured): abi3 tag is honest — the build was always abi3
  (`Py_LIMITED_API=0x030a0000` inherited from `Shiboken6::libshiboken`);
  repair is wired into `build_in_container.sh --wheel` (pinned
  `--plat manylinux_2_34_x86_64`, excludes = PySide6-provided + host libglvnd)
  and the CI `wheel` job produces the artifact from the pinned container;
  metadata pins `PySide6-Essentials~=6.12.0` (the versioned SONAMEs
  `*.abi3.so.6.12` + `Qt_6.12` refs demand the patch series);
  `install.components=["python"]` keeps the C++ install tree out of the wheel.
  The `[tool.cibuildwheel]` section is staged for a future PyPI flow — its
  before-all must bootstrap Qt 6.12 (the PySide6 wheels ship no Qt headers).
- **C++:** `install(TARGETS)` + exported namespace `qmn::` +
  `qmnConfig.cmake`/version-file + public-header review (re-check
  Shiboken-wrappability) + an example consumer built against the *installed*
  package. Today the repo has no install/export at all — this is what makes
  "usable from C++" real.
- **Parity surface (Gate-1 rows bound at freeze):**

  | qtmonaco API | our state today | where |
  |---|---|---|
  | `set_text` / `get_text` | `setPlainText` / `toPlainText` | ✔ bound (shim) |
  | `insert_text` / `delete_line` | view doors `insertTextAt`/`deleteLineAt` (NOT the core `insertAtOffset`/`removeRange` — those skip the view's caret/undo/surrogate machinery) | ✔ bound |
  | `set_cursor` / `current_cursor` | `setPosition` + `revealAt` / `position` | ✔ bound (shim keeps the 1-based dict from `cursorMoved`) |
  | `set_language` / `get_language` | none | D2, ✔ bound (unknown id → native plain-text fallback, Python signal still echoes the request, qtmonaco-style) |
  | `set_highlighted_lines` / `clear_highlighted_lines` | none | D3, ✔ bound |
  | `set_line_numbers_mode` | gutter not built | D3, ✔ bound (string→enum on the shim, ValueError on unknown) |
  | `set_theme`, `set_readonly` | none | D2–D3, ✔ bound |
  | `set_scroll_beyond_last_line_enabled` | not implemented | E1, ✔ bound |
  | `set_minimap_enabled` | no minimap by design | E1 ruler (**maps to it**, decided), ✔ bound |
  | `set_vim_mode_enabled` | out of scope | binding **warns, never implements** — ✔ stub exists |
  | `open_diffs` | none | **Gate 2** (M10) — deliberately ABSENT (honest AttributeError) |
  | LSP hooks (`update_workspace_configuration`, `add_action`, lsp header) | none | **Gate 2** (M8) — deliberately ABSENT |

- **Freeze:** bump `SOVERSION`, CHANGELOG discipline, then the v1 tag.
  ✔ Done at freeze: package VERSION 1.0.0 (SOVERSION is a no-op for the
  static-archive package — deviation documented in `CMakeLists.txt`),
  CHANGELOG `[1.0.0] — 2026-11-12`, pyproject aligned, tag `v1.0`.
  M11 accessibility (QAccessible — a custom-painted widget gives screen
  readers nothing by default) is Gate-1 **polish**, non-gating: ship alpha
  without it, close it before calling v1 "finished".

**Off-by-one trap (deliberate, never accidental):** qtmonaco is **1-based** —
`delete_line(line)` docstring says so; `set_cursor(line, column=1)`,
`set_highlighted_lines(start, end)` and `insert_text`'s default `column=1`
too. Our `TextPosition` is 0-based. The binding converts explicitly, with its
own tests. Match error semantics: qtmonaco raises `ValueError` when readonly,
and `current_cursor` is a *property*.

**Decided divergences (binding-level behavior):**
- **vim** — out of scope forever, but `set_vim_mode_enabled` is *exposed* and
  warns instead of being omitted, so drop-in callers don't hit
  `AttributeError`.
- **minimap** — `set_minimap_enabled` maps onto the E1 overview ruler. No
  Chromium-style minimap is ever built.

**Gate-1 exit checklist:** ctest green + wheel smoke + C++ consumer smoke +
bench within perf targets (README) on the big file → tag `v1.0`.

✔ **CLOSED 2026-11-12 (v1.0.0 / tag `v1.0`):** ctest 2/2 (textbuffer +
codeeditor); wheel smoke + qtmonaco parity against the auditwheel-REPAIRED
`cp310-abi3-manylinux_2_34` wheel from the pinned container (one libQt6Core
in maps — PySide6's own); consumer smoke against the installed exported
`qmn::` package (41 units, 8 spans); bench **7.2 µs/op** on the 1,000,001-line
document — the README perf table reproduces, after the gate did its job: it
caught M3's undo-capture walk at 37 µs/op and that regression was fixed
(`72fd735`, pruned `textRange` walk + deque undo stack) before the freeze.
The v1 API is FROZEN: Gate-2 work may not require new view primitives.

# GATE 2 — IDE surface (after v1 freeze)

Everything here is a *consumer* of the frozen v1 widget API; none of it may
require new view primitives (if it does, that is a Gate-1 design bug to fix,
not a Gate-2 workaround).

- **M8 LSP client** — new layer `src/lsp/`, QtCore/QtNetwork only, **zero
  widget dependency**; glue (diagnostics → decoration layer 1+3, hover/
  completion popups) is a thin consumer. **Transport correction:** LSP speaks
  JSON-RPC over **stdio pipes to a spawned server process** (`QProcess`);
  README's old "QWebSocket" claim was wrong (websocket is a rare secondary
  transport) — corrected in README.
- **DAP features** — the widget's contribution already exists after D3:
  breakpoint glyphs, current-stack-frame line, inlay hints for variable
  values. A DAP protocol client would sit at the same QtCore level as
  `src/lsp/`. No view surgery needed or allowed.
- **M10 diff view** — structural alignment on tree-sitter ASTs + inline or
  side-by-side layout (open decision at start); bind `open_diffs`.
- **Soft wrap** — the deferred half of M9 (decided 2026-11-09: v1 ships
  h-scroll only); rides E2's view-line map.
- **Parity leftovers:** LSP hooks + `open_diffs` rows from the F table.

## Top risks

1. **Qt drift + host toolchain mismatch** — Arch's system Qt rolls forward
   while PySide6 lags and ships its own. Always build against the venv's
   PySide6 Qt and pin the pair. This already bit once: the pip Shiboken
   generator cannot run on the host at all (missing `libxml2.so.2`) — the
   container fix is Phase 0 / AGENTS.md.
2. **1-based vs 0-based** — silent off-by-one across the whole parity
   surface; conversion only at the binding layer, with its own tests.
3. **Viewport mapping is the scar tissue** — M9 already failed once
   (`2809945`, recoverable via reflog while present) and M13 folding piles a
   second mapping axis on top. Treat E1→E2 as one mapping core: unit-test
   the view-line map as a pure thing *before* wiring the widget, and review
   the old attempt before retrying.
4. **Highlight engine — decided** (vendored tree-sitter, v0.27.2). Residual
   risks: adapter byte-unit correctness (mitigation: property tests feeding
   random edits and asserting token spans, `TSInputEdit` in exactly-2×
   units), per-language `.scm` curation load, ABI hygiene only when bumping
   the vendored copy (grammars ≥ 13, runtime ABI 15).
5. **No vision** — every rendering milestone (gutter, ruler, folding,
   h-scroll, later wrap/diff) is proven by numeric/state assertions only;
   precedent is `cellWidthMatchesGlyphEngineAdvance`.
6. **API freeze ordering** — never widen the bound surface before M2 lands;
   never freeze v1 before Phase F's C++-export + wheel work is green.
7. **Gate 1 is long** — the two-gate model's one real downside; the release
   train (alpha wheel + `v0.x` tag per milestone) is the agreed mitigation
   for hobby-scale motivation accounting.

Out of scope by design: vim mode (setter exists and warns). Soft wrap is
*deferred* to Gate 2, not out of scope.



