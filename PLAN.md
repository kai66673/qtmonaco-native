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

## Product model — two gates + interlude (decided 2026-11-09; 1.5 added 2026-11-12)

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

**Gate 1.5 — usability interlude** (owner decision, 2026-11-12): a
post-freeze MINOR train (M20–M24: font/zoom on the settings surface,
indent guides, fold-gutter hover region, language-scoped properties,
themes) runs BETWEEN the two gates; Gate 2 opens only when 1.5 closes.
It widens the frozen v1 surface strictly additively (settings mirror,
theme object, language overrides — v1.x minors on the release train);
view *mapping/geometry* primitives stay exactly as frozen.

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

**License** (owner decision, 2026-11-21): **BSD-3-Clause**, copyright
`kai66673`, one `LICENSE` file covering BOTH deliverables (the C++ package
and the `qmn-native` wheel; the wheel declares it via PEP 639 metadata).
Chosen to match the parent qtmonaco; the vendored tree-sitter + grammars
keep their own MIT licenses, every upstream `LICENSE` staying in place under
`third_party/tree-sitter/` (VENDORED.md already records the set). Before
this, the README said "TBD" — a hard blocker for any third-party use.

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

Post-1.0 addendum (2026-11-12, owner bug): the map's cached slot total
went stale for FOLDLESS docs — `shiftFoldStateForEdit`'s
`collapsed_.isEmpty()` fast path never refreshed `foldVisibleTotal_`, so
Enter at EOL painted nothing on the new last line (caret included) until
some fold op rebuilt the runs. Fixed there; tripwired by
`foldlessEditKeepsPaintSlotCache`. Coverage gap acknowledged: the 13
M13 slots all exercised folds OPEN (which rebuilds on every edit), so
the DoD never probed the foldless fast path.

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
  | `open_diffs` | none | **Gate 2 (design decided 2026-11-15)**: M10 door `setDiffDocuments` + shim; binds at the diff car; until then the honest AttributeError stands |
  | LSP hooks (`update_workspace_configuration`, `add_action`, lsp header) | none | **Gate 2 (design decided 2026-11-15, M8 machine 7 SHIPPED 2026-11-19)**: config-buffer door (`setStoredWorkspaceConfiguration` — the view holds it OPAQUE, the app-side hub pushes it) and `addContextAction(id,label)` (**renamed from the planned `addAction`**: that name would hide `QWidget::addAction`) + the trigger signal, ✔ bound; `set_lsp_header`/`get_lsp_header` stay ABSENT ON PURPOSE (decided 2026-11-19: the header is wire-only prepended text, and a ±H offset tax on all six LSP paths buys nothing a native stack needs — the honest AttributeError stands) |

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
- **Gate-2 set, decided 2026-11-15, landing with the owning cars:**
  `add_action`'s `language` precondition is ACCEPTED AND IGNORED (the native
  language scoping on the editor does the job); `open_diffs`'s `options`
  dict is accepted with unknown keys ignored and `renderSideBySide:false`
  unimplemented (side-by-side only in v1); no LSP client class ever crosses
  the wheel — the config door carries compact JSON as `QString` (PySide6
  has no `QJsonObject`), and `set/get_lsp_header` stay pure-Python shim
  text-prepending.

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

Post-1.0 addendum (2026-11-12, owner request): **qmn-demo**, the full C++
demo app — tabbed QTabWidget main window, Файл->Открыть… per-file tabs
(suffix->grammar guess), Настройки->dialog applying ten M19 knobs to
EVERY open editor on accept (+ template for later tabs). Lives entirely
in `src/app/` (static lib `qmn_demo_app` + executable); the frozen view
API gained nothing. Offscreen-tested via the menu slots' programmatic
doors (`tests/tst_demo`, ctest now 3/3) — the dialog/QFileDialog exec
path is never entered by tests by design.

# GATE 1.5 — usability interlude (M20–M24; owner proposal 2026-11-12) — ✔ CLOSED (v1.5.0, 2026-11-12)

Owner-requested usability round BETWEEN the gates; the list order below
is the owner's and the dependency chain runs the same way. Post-freeze
discipline: every item EXTENDS the frozen v1 surface, so each milestone
ships as a minor (`v1.1.0`, `v1.2.0`, …) under the release train —
ctest green + wheel rebuilt from the pinned container + qmn-demo wired
is the tag gate. What stays inviolate: NO new view mapping/geometry
primitives — 1.5 adds settings/theme/behavior surface only.

State corrections against the owner's five points (measured in code,
2026-11-12):
- (1) font/size/zoom — the LIBRARY knobs are DONE (M17):
  `setCodeFont`/`codeFont` (the size rides inside the QFont, not a
  separate knob), `setZoomFactor`/`zoomFactor`/`zoomIn`/`zoomOut`/
  `resetZoom` + Ctrl+= / Ctrl+- / Ctrl+0 + Ctrl+wheel. Missing ONLY:
  the M19 mirror (`options()` carries neither knob, so the *Changed
  bus is blind to font/zoom changes) and the qmn-demo wiring (font/
  zoom were deliberately left out of the demo template). M20 = pure
  wiring, no new library machinery. (CLOSED by M20 below, 2026-11-12.)
- (2) indent lines — absent; the only vertical guide today is the M18
  long-line ruler (Text@alpha40, h-scroll-clip recipe). Fully new.
  (CLOSED by M21 below, 2026-11-12.)
- (3) fold gutter — click-to-fold EXISTS (M13): a left click inside the
  strip toggles the fold STARTING on that row, caret never moves,
  Alt+click keeps its multi-caret meaning, marker-less rows fall
  through to caret placement. What does NOT exist: any hover state (no
  setMouseTracking, no QHoverEvent handling) and no deepest-fold
  targeting. M22 builds hover + innermost targeting on top of the
  existing hit handler.
- (4) language — today a bare grammar id: `setLanguage`/
  `availableLanguages()` over HighlightEngine's configure-time
  parser+.scm table; ZERO per-language properties. The suspected
  HighlightEngine connection is real — the id IS its key — so the
  property registry belongs beside that table.
- (5) theme — absent but pre-claimed everywhere: chrome paints mostly
  from live QPalette roles (Base/Text/Highlight/PlaceholderText), and
  every hardcoded ink (fold triangles, breakpoints, severities, find
  quads, overview stripes, `HighlightEngine::colorFor` = "v1 fixed
  theme") carries a "set_theme routes it" tripwire comment;
  `kOverviewRulerWidth` is "fixed until set_theme". Themes cover
  COLORS only — metrics stay constants.

- **M20 COMPLETE (2026-11-12): font/zoom onto the settings surface.**
  Mirrored the two M17 knobs onto `CodeEditorOptions` exactly as
  specced (the store stays in the editor, the options object mirrors,
  the editor emits so the bus is never blind) — final signal names are
  `codeFontChanged`/`zoomFactorChanged`, getter-named like the rest of
  the M19 bus, replacing the tentative `optionsFontChanged/
  optionsZoomChanged` spellings of the plan. Emission covers ALL doors:
  both flat setters, the chords riding `setZoomFactor` (the emitted
  value is the CLAMPED truth), and the foreign `QWidget::setFont`
  REBASE path in `changeEvent` (per-half emits: base/zoom each
  announced only if it moved; the self-doors' FontChange lands in the
  non-rebasing path and stays silent there, so nothing double-fires).
  qmn-demo's template grew QFont + zoom fields (defaults via a shared
  `demoDefaultCodeFont()` recipe copy — the drift tripwire now compares
  whole QFont definitions), the dialog grew the QFontComboBox + px
  spin + zoom double-spin group, editing only family/px/factor while
  the seed font carries the rest through (round-trip QFont equality;
  `setCurrentFont` deliberately avoided — fontconfig rewrites alias
  families like "monospace"). Offscreen-safe; DoD met: mirror shows the
  post-rebase truth, demo tests extended in the existing shape.
- **M21 COMPLETE (2026-11-12): indent guides.** Options-only knob
  `indentGuidesEnabled` (M19 rule: new settings live ONLY on the options
  object). Painted inside the existing line loop: one 1px vertical guide
  per tab-stop strictly inside a line's leading-whitespace span; a
  literal tab covers every stop it crosses (VS Code rule); ambient
  decoration — no cache-key change, no scroll-range change, h-scroll-
  clipped like the M18 ruler, collapsed interiors paint nothing because
  the view-line map already routes them. Ink Text@alpha40; joins M24's
  role sweep with its own role. DoD: pixel band scans at known stop
  columns (ink at every interior stop for BOTH tab- and space-indented
  leads, none past the first non-ws char, none on empty lines), OFF
  kills all ink, negative-h-scroll clamp. Landed as specced: the guide
  set per line is {k·stop : k·stop < W} ∪ {the stops literal tabs land
  on}, W = the tab-EXPANDED rendered leading-ws width — so "\tx" lights
  column 4 while the same-width "    x" lights nothing (the VS Code
  asymmetry the tab clause carries); guides are per-line 1px segments
  (band height, unlike the full-height ruler) painted in the text slot
  loop UNDER the caret/glyphs, ink + gutter/ruler clip copied verbatim
  from M18, and the leading-ws scan is windowed + early-exiting like
  indentOfModelLine (kIndentWindow cap; stops past the window's right
  edge never computed). tabStopWidth_ is read LIVE each paint — the M23
  fan-out seam. Tests: `indentGuidesKnobOnOptions` (default ON, one
  emit per real change, same-value silence) +
  `indentGuidesPaintBandsAndHScroll` (pixel band scans over the
  tab/space/mixed/empty grid, OFF frame kills every lit column, ON-vs-
  OFF diff frames at h=60 show the shifted columns and ZERO diff left
  of contentLeft — the negative clamp — and a −100 public-door set
  clamps to 0 with the grid back at its unscrolled columns);
  `optionsMirrorsKnobsAndNotifies` grew the third options-only spy.
  One existing probe needed a pin: `paintClusterWindowXOffsetProbe`
  asserts its 200-space lead stays EXACTLY base, so it now pins the
  decoration OFF next to the M18 wash pin (same spirit, same comment
  slot). qmn-demo: `EditorSettings.indentGuidesEnabled` (13th knob) +
  the "Вертикальные линии отступов" checkbox in "Текст и отступы";
  defaults tripwire + customSettings extended.
- **M22 COMPLETE (2026-11-12): fold-gutter hover region + innermost-fold
  click.** No new public API at all (the probe-free DoD held): tracking +
  pixels + click state proved everything. The mandated offscreen probe
  ran FIRST and re-shaped the seam: Qt 6.12 REMOVED the QWidget hover
  virtuals (qwidget.h keeps only a `class QHoverEvent;` forward decl —
  the override attempt fails to compile), so QHoverEvent/WA_Hover are
  dead ends: the seam is `setMouseTracking` + the already-overridden
  `mouseMoveEvent` with a `Qt::NoButton` branch (the tracking gate IS
  the fold-strip gate: ctor + setLineNumbersMode flip both together).
  Hover state is anchored in MODEL rows (`hoverFoldRow_` + resolved
  start/end — the M16 drop-caret pattern: scrolling keeps the painted
  region glued to the fold for free); `innermostFoldAt(row)` resolves ON
  DEMAND at hover-move and FRESH at click-press (never on the paint
  path, the click never trusts the prior hover): a fold STARTING at the
  row wins (foldEndForLine's tightest end = exactly what the click will
  fold), else the nearest start above covering the row — indent mode
  scans model rows above (kFoldHoverScanCap 2048), grammar mode
  reverse-walks `highlight_->foldsFull()` (start asc/end desc ⇒ reverse
  visits each group's ends ascending, first end > start IS the tightest
  end — mirrors foldLine's same-start rule; groups short of the row are
  unreachable folds and the walk moves on). `foldables_` stays the
  windowed paint cache untouched; enclosing folds above the viewport are
  the resolver's scan's business, not the cache's. Invalidation is the
  buffer edit observer (the universal choke: typing/paste/undo/DnD/
  setPlainText alike) + every fold mutation door (foldLine/unfoldLine/
  revealLine/expandAll; collapseAll/toggleFold ride them) + ANY press
  (the dragLeave lesson: the clear never RELIES on a leave arriving) +
  the new `leaveEvent` override. Paint fills only the hovered fold's
  strip cells (x ∈ [textLeft−fz, textLeft), slots clamped to the window;
  hidden interior rows (visibleIndex −1) shrink a COLLAPSED fold's
  region to its start row by the map alone); ink is a fixed cool
  `QColor(150,150,150,46)` next to foldInk (R==G==B keeps the warm/red
  decoration predicates green; M24 routes it as a role). Click
  semantics widened deliberately from marker-row-only: a mid-fold click
  toggles the covering fold (caret NEVER moves), uncovered rows keep the
  caret fall-through, Alt+click/dbl-click/context-menu guards untouched.
  Tests: `mouseClickFoldZoneToggles` rewritten for the new rule (mid-
  fold toggle w/o caret move + fall-through asserted on an uncovered
  row); `foldHoverRegionPaintsAndClears` (single clean strip column
  xL−fz+2 — measured metrics dodge triangle/digits/hairline — exact-
  rect fill means covered slots read exactly lh band pixels of the
  blendOver(Base,150,150,150,46) predicate, everything else zero; all
  invalidators probed incl. the shrunken collapsed region and the
  press-clears-then-strip-press-toggle combo, and a wheel leg (one
  notch = 3*lh px) proves the MODEL-row anchor in pixels — the lit set
  travels with the fold, slot 3 lit and slot 9 (starting exactly where
  the fill ends) dark; the leg also caught that the probe column must
  be RE-DERIVED per document (the gutter widens with the digit count:
  46→55 for two-digit numbers));
  `foldHoverTrackingGatedByFoldStrip` (hasMouseTracking flips with the
  chrome; a FORCED sendEvent hover stays inert while the strip is gone:
  ZERO whole-frame diff — no repaint fires);
  `foldClickResolvesInnermostFold` (indent nest head/[0..4]/[1..3]: mid-
  fold folds the INNER, start-row self wins, subsumption through the
  door, fall-through caret) and `foldHoverGrammarInnermostClick`
  (python (0,4)+(2,3): hover pixel row-2/3-only proves innermost-over-
  outer on the reverse-walk; mid-inner click folds inner; next row
  resolves the outer (inner group's tightest end stops short — walk
  skips it) and folding subsumes the collapsed inner; fall-through on
  the grammar path too). qmn-demo: no new knob, nothing to wire.
- **M23 COMPLETE (2026-11-12): language-scoped properties.** Generalized
  the id into a small registry beside HighlightEngine's grammar table:
  per-language settable properties, first = `tabStopWidth`. Resolution
  while a language is active: LANGUAGE override (when set) > editor
  options > factory default — exactly Monaco's `[languageId]` model,
  which is what the owner asked ("языковые свойства перекрывают настройки
  редактора"). Tri-state WITHOUT std::optional crossing the boundary
  (Shiboken): the public child QObject `view/LanguageProperties.hpp` —
  set/clear + has-getters, -1 un-set sentinel on the getter AND the
  `tabStopWidthChanged(int)` signal (same-value sets + double clears
  silent), plain Qt types. `CodeEditor::languageProperties(id)` hands
  out lazily-created editor-owned children keyed by the SAME
  normalization HighlightEngine::setLanguage applies (trimmed +
  lowercased; the empty key = the plain-text state), connect-on-creation
  to the one fan-out seam. The effective value fans out to EVERY
  tab-width consumer: `tabStopWidth_` became the EFFECTIVE stop (all
  six live reads unchanged: soft-tab insert, the geo-slot tab-stop
  render grid, autoIndent math x2, guide stops, outdent) and
  `refreshTabStopWidth()` (override > template; on a real change: the
  pre-M23 setTabStopWidth body verbatim — resetGeoCache +
  updateScrollBarRange + update) runs from any override change, from
  `setLanguage`, and from the template door; `setTabStopWidth` now
  stores `tabStopWidthOption_` (the TEMPLATE; the options mirror reads
  it through friendship, so the bus and the demo settings round-trip
  never see a per-language number — the DoD's applyEditorSettings rule
  holds because `EditorSettings` goes through that mirror). The
  built-in registry starts EMPTY — upgrades stay behavior-identical,
  per-language defaults are IDE-side later. `availableLanguages()`
  unchanged; language-aware autoIndent (e.g. Python colon indent) is
  a named future hook, NOT in scope. DoD: met by
  `languagePropertiesTriStateAndSharedRegistry` (sentinel/signal,
  normalization identity, parenting, empty built-ins + unchanged
  availableLanguages), `languageTabStopOverrideFansOut` (insert count +
  xForCol literal-tab advance flips prove the geo wipe + autoIndent
  stop math), `languageTabStopOverrideGuideStopsPaint` (M21 pixel idiom:
  every guide column shifts with the override/clear),
  `languageTabStopFollowsLanguageSwitch` (flips with the active grammar,
  both normalization sides, unknown-id + empty-key rules) and
  `languageTabStopRidesAboveTemplateAndSettings` (template moves under
  the override, bus + mirror stay template-based, fall on clear).
  qmn-demo: no new global knob to wire (the dialog stays global by DoD).
  Binding re-run green is owner-side at tag time.
- **M24 COMPLETE (2026-11-12): themes.** One theme object landed exactly
  as specced: the public child QObject `view/CodeEditorTheme.hpp` (on
  the install allow-list, CodeEditorOptions/M23 pattern: back-pointer to
  the editor, ctor only stores it). Shiboken-safe surface: a 28-value
  `Role` enum + QColor get/set/clear/has + a `TokenKind`→QColor map +
  Q_OBJECT `themeChanged()`. Tri-state maps: an explicit QColor paints
  VERBATIM, an un-set entry falls back to the historical table — live
  QPalette nudges (Selection=Highlight@90, band=Highlight@45, wash=
  Text@30, frame=Text@90, sep/guides/ruler=Text@45/40/40, gutter digits
  =PlaceholderText) AND the fixed constants (fold gray 150, fold-region
  tint 150@46, find oranges, the severity family via `severityInk
  (Severity)`, ruler chrome + viewport frame). `Preset {Default,Light,
  Dark,Custom}` is a KIND TRACKER: any manual set/clear moves it to
  Custom, `applyPreset` replaces the whole map and announces exactly
  once (default "palette" = the empty map = today's live-QPalette
  behavior; Light/Dark are explicit pinned pairs, Dark's token map =
  today's fixed one — a test asserts value-equality with
  `HighlightEngine::colorFor` per kind). The editor gained the
  `theme()` door; its `themeChanged` connection wipes the GEO CACHE
  before repainting (token ink rides the cached format ranges — chrome
  reads live and needs no wiping), and `formatRangesForLine` consults
  `theme_->tokenColor(kind)` before `HighlightEngine::colorFor` (the
  invalid→skip-span rule unchanged). Every "set_theme routes it"
  comment resolved: the anonymous-namespace fixed-ink helpers (severity/
  find/breakpoint/ruler/fold/fold-region) are GONE, all paint sites read
  `theme_->ink(Role::…)`; the find QUAD wash layers its own paint-time
  alpha 80/130 on top of the role ink (documented). Deliberate scope
  note: the find bar and the DnD drag chip are separate widgets — they
  keep riding plain QPalette (documented in code). Metrics:
  `kOverviewRulerWidth` stays a constant (chrome geometry is not a
  theme knob). qmn-demo: 14th knob `EditorSettings.themePreset`, dialog
  "Тема:" combo (Default/Light/Dark rows — Custom is never a row; a
  Custom seed clamps to Default), applySettings SKIPS `applyPreset`
  when the kind already matches (no map-wipe/repaint per dialog accept),
  collectSettings reads the live `kind()`. DoD met: six new slots —
  `themeObjectTriStateSignalsAndKindTracker` (signal discipline + kind
  tracker + map namespaces), `themePresetSwapsGroundAndDarkTableMatches
  Engine` (ground probe BEFORE any preset + Light/Dark ground flips +
  Dark==engine table per kind), `themeRoleSwapFlipsBaseAndTextInk`
  (Base/Text swap), `themeTokenOverrideFlipsCommentInkAndRestores`
  (known-span token ink moves + only Comment moves + clear restores —
  no manual invalidation, so this also proves the geo wipe),
  `themeSeverityOverrideMovesDiagnosticInk` and
  `themeChromeRoleOverridesFlipFoldBreakpointFindProbes` (fold/breakpoint/
  severity/find-quad inks invert their M5/M6/M7/M13 probe predicates
  under role overrides and restore live on clear) — and the upgrade
  tripwire: the DEFAULT (empty-map) theme reproduces the pre-M24
  suite's pixels, proven by every untouched paint test passing
  unchanged (206 passed / 2 skipped, ctest 3/3).
  Binding re-run green is owner-side at tag time.

Phase DoD: each M2x tagged with ctest green + wheel rebuilt from the
pinned container (the binding picks up the options-mirror/theme/
language doors); qmn-demo wired per milestone; README/AGENTS gate
wording kept honest. Gate 2 opens when M20–M24 close.
✔ CLOSED 2026-11-12: all five cars landed (M20 06bf7b3, M21 7f4b8b8,
M22 3fbf574, M23 3291e4e, M24 7370598) and the untagged train ships as
ONE minor — tag `v1.5.0` (package VERSION 1.5.0, CHANGELOG cut, ctest
3/3 at HEAD: 206 passed / 2 skipped). The wheel rebuilt from the pinned
container AT the tag is the owner's release step; it carries the
options-mirror/theme/language doors into Python for the first time.
**Gate 1.5 CLOSED — Gate 2 may open.**

# GATE 2 — IDE surface (open since 2026-11-15)

Everything here is a *consumer* of the frozen v1 widget API; none of it may
require new view primitives (if it does, that is a Gate-1 design bug to fix,
not a Gate-2 workaround). The design knots below were decided 2026-11-15 in
planning, with the Vala/GTK4 IDE `iide` (`/home/kai/kaigit/iide`) mined for
prior art; its own `RECOMMENDATIONS.md` doubles as our anti-pattern list.

**Train ordering (decided): wrap → diff → LSP → DAP.** Wrap is literally M9's
deferred half — by the gate rule above that is a Gate-1 debt, and it is paid
BEFORE anything builds on row math: the diff alignment rows live in slot
space, so a late wrap would force a full revalidation round on diff tests.
The wheel-parity leftovers ride their owning cars, they are not a car of
their own.

**Status at the 1.8.0 close (2026-11-19): wrap (M25), diff (M10 cars 1–3) and
LSP (M8 machines 1–7) are SHIPPED.** DAP is the one car left open, and the
deferred slivers ride it or their own follow-ons: M8b (signatureHelp, inlay
hints, foldingRange over the M13 fold API, pull diagnostics via the
registration registry), M10b (inline word-level diff, wrap×diff budget math,
original-side wrap), and the two pre-existing defects recorded verbatim below
(the undo-mark drift on type-over-selection, the overview-ruler perf cliff).
**The first deferred sliver is already BUILT (2026-11-19, right after the
close, no version bump): the glue-side popup TRIGGERING.** The hub owns the
DECISION, the view popups stay display-only: `LspHub::setPopupTriggers(editor,
bool)` (opt-in per OPEN document, OFF default = zero wire-byte change for
every existing test) taps `editApplied`/`cursorMoved` per registration; typing
a unit debounces `kCompletionTriggerMs` and classifies at fire time
(identifier -> Invoked, an advertised `triggerCharacters` char ->
TriggerForCharacter, anything else fires nothing), a superseded in-flight
completion is `$/cancelRequest`'d (`LspClient::cancel`, local -32802, the late
reply never clobbers a newer popup), and hover asks after `kHoverTriggerMs` of
idle NON-typing caret moves — skipped while completion owns the popup.
Completion rows decode to `{label, detail}` capped at 100; hover contents
pass VERBATIM (no markdown rendering, PLAN's popup shape); the hover frame is
the server's `range`, else the word at the caret, against the syncer MIRROR.
`tst_lsp` grew 62 -> 66 slots on the fakeserver's new `hoveron`/`reqlog`/
`cancellog`/`completiondelay=` flags (plain-`features` pins byte-identical).
The LIVE demo path shipped with it: `qmn-demo --lsp` (hub OFF by default —
`tst_demo` never spawns a server; clangd is not a CI guarantee), which wires
openDocument + `setPopupTriggers(true)` for every c/cpp tab on the hub's
built-in clangd spec.

**The second deferred sliver is BUILT (2026-11-21, no version bump): the
completion ACCEPT.** The VS Code shape (the user's choice): while the list
is visible the view routes Up/Down/PageUp/PageDown/Home/End to the ROWS
(the caret never moves), Enter/Tab accept, and a mouse release ON A ROW
accepts — a press alone only SELECTS. The one recorded DEVIATION from the
m6 display-only contract: the completion list loses
`WA_TransparentForMouseEvents` (rows became click-targets); a press on the
popup CHROME, a press on the editor, and any other key still dismiss both
popups, and Esc hides ONLY the list (a visible hover deliberately survives).
The view NEVER inserts: `CompletionItem` gained the additive `TextEdit
textEdit` field (all-zero default, carried OPAQUELY) and the view emits
`completionAccepted(index, item)` exactly once per accept (hide BEFORE
emit, so a glue re-show rides a closed popup). The insertion lives in the
hub: the `completionAccepted` tap connects for EVERY registration but the
trigger `on` bit + registration liveness + an all-zero-payload refusal make
anything unintended a silent no-op; the edit goes through the frozen
`applyTextEdits` door — one accept = one undo group, the syncer rides
`textChanged` like any local edit (server arrival proved by CONTENT: the
fakeserver hover echoes the word only a delivered didChange can spell),
and NO auto re-fire (the multi-unit accept edit is not typing). The hub
decodes the wire `textEdit` in both legal shapes — plain `{range,newText}`
and the insert/replace alternative ranges, PREFERING `insert` (anchored at
the request position) — and otherwise synthesizes `insertText`-or-`label`
as a zero-width insertion at the caret captured at trigger FIRE time;
`initialize` now advertises completion `insertTextFormat` 1 (PLAIN TEXT,
`snippetSupport` false — the popups have no snippet engine). The new field
+ signal ride ALREADY-BOUND classes: the three-copies stayed untouched, and
`CompletionItem` got a `Q_DECLARE_METATYPE` (signal parameter, the
`cursorMoved` precedent; container codegen rejected-log: unchanged
QFlags-only noise). Tst grew: codeeditor 254 -> 256 slots, lsp 66 -> 69
(the fakeserver decorates its EXISTING items — every row-count/label pin
holds — with the three payload shapes: plain textEdit, the insert/replace
pair one unit APART to pin the insert-preference, and insertText-only; the
sync-arrival slot arms `setDebounceMs(50)` so the accept's didChange sits
in the server long before the 500 ms hover ask reads it).

**The third deferred sliver is BUILT (2026-11-21, no version bump): the
SHARED hover popup — diagnostics ride the hover.** The user-decided shape:
ONE popup, the caret-line's decoded DIAGNOSTICS lead it (one line each:
the severity word, then the `[source]` label when the producer sent one,
then the message — multi-line messages print their FIRST line only, the
popup is a strip, not an output panel) and the server's hover contents
append at the end. Match = the range OVERLAPPING the caret's LINE (a
column mismatch never hides a current-line entry; the view's set is
sorted, so rows read top-left). `Diagnostic` grew the additive `source`
label — carried OPAQUELY (the painter ignores it, the round-trip getter
echoes it, Python sees it through the already-bound record, three-copies
untouched); the hub decodes the wire `source` and re-encodes it into the
codeAction `context.diagnostics` ONLY when non-empty, so machine 5's
scan-compare stays byte-symmetric (the fakeserver grew the `diagsource`
flag: both sides label, or neither does). The diagnostics half needs NO
provider and NO wire byte: the pushed set already lives in the view's
decoration state — with no hoverProvider the hover question is refused
LOCALLY and the popup fills from the diagnostics right away (pinned: the
reqlog spy sees ZERO hover REQs and the server stays READY). The recorded
DEVIATION from m6's contract: a null/empty hover result no longer HIDES
while the diagnostics half carries the popup — the hide survives only for
the EMPTY composition (clean caret line + no contents = m6 as shipped).
The FRAME: the server's `range` still wins, else the diagnostics frame
(the record CONTAINING the caret, else the first overlapping its line),
else the word at the caret, else zero-width — the wire parts decoded
against the syncer's MIRROR as always. The fakeserver's marker scan
(`diags`) feeds one error + one warning on line 0; three new lsp slots
(69 -> 72) pin the merged label text, the provider-free path and the
clean-line plain contents; the codeeditor round-trip grew the `source`
echo inside `decorationsRoundTripUpdateRemove` (no new slot).

**Known pre-existing bug (recorded 2026-11-15, deferred to its own small
car):** bracketed type-over-selection in `insertTextAtCaret` (the
`beginEditGroup` bracket around `deleteSelection` + insert, found via a
typeover through the accessible `replaceText` door) pushes TWO undo groups
(serial +2, depth +2) for ONE `reconcileUndoMarks` call, so `undoMarks_`
lags the core's history by one entry. Debug-only symptom: the
`Q_ASSERT(undoMarks_.size() == buffer_.undoDepth())` trips in
`accessibleTextOffsetsMapDocumentAndDriveCaret`; it REPRODUCES ON CLEAN v1.6.1
main (verified by stash), release builds stay silent but caret marks drift
(undo restores a stale caret position). This is frozen Gate-1 undo mechanics —
fixing it is deliberately NOT folded into an M25 car; it gets its own fix car
with its own tripwire.

**Known pre-existing perf cliff (recorded 2026-11-16, deferred to its own
car):** the overview-ruler stripe loop (v1.6.1 code, unchanged by M25 except
the lines→slots unit) calls `buffer_.offsetOfLine` once per stride sample,
and `offsetOfNthNewline` scans WITHIN the holding piece from the piece start —
on a ONE-PIECE (loaded, not typed-into) document that is O(doc). Measured on
a 12k-line/6MiB single-piece doc: ~1 ms per call x ~400 samples = ~230 ms
per paint, IDENTICAL with wrap ON and OFF (bit-parity holds; the wrap layer
itself stays inside its 8 ms escalation budget — the M25 resize-storm test
measures the wrap legs with the ruler OFF and says so). A production user
resizing a big loaded file pays this in shipped v1.6.1 too. The fix belongs
to the core's line index (per-piece newline position lists or a line cache),
not to the wrap layer — its own car with a bench tripwire, never folded into
M25.

### CodeDocument (new public view QObject; lands first inside M25)

Decided 2026-11-15 (owner proposal). `src/view/CodeDocument.{hpp,cpp}`: a
QObject that OWNS the `TextBuffer` (the editor's `buffer_` value member
becomes a pointer — the one honest piece of view surgery in Gate 2, guarded
by the 210-slot `tst_codeeditor` suite); `CodeEditor::document()` returns the
editor-owned document (lifetime pattern = `options()`, non-null, attached in
the ctor). Public: `setPlainText/toPlainText`, read-only line getters,
`setDocumentUri(QString)/documentUri()`, modified = `markSaved() ->
isModified()/modificationChanged(bool)` (v1: reset ONLY by explicit
`markSaved()` — the undo-back-to-baseline refinement waits on a core undo
hook, documented interim), `documentSaved()`-style signals stay consumer
concerns (real I/O is theirs). Signals: `textChanged()`, **`editApplied(int
start, int end, QString inserted)`** — offsets in the PRE-OP frame, exactly
LSP `contentChanges` semantics; one emit per primitive mutation including
undo/redo replay; emitted after the mutation, before the `textChanged()`
ping; plain Qt types so it Shibokens free. Contract: **one editor per
document in v1** — two editors on one buffer would fight over the single
core edit-observer (the highlight engine's slot); multi-view editors are a
separate future machine that needs a core multi-observer first, and the
diff car does NOT need it (two documents in one widget, not two widgets on
one document). Language stays on the VIEW (a render property; moving
languageId to the document touches `LanguageProperties` machinery — not in
this car).

### M25 — soft wrap (car 1; includes the CodeDocument migration)

- **Units (the one sanctioned contract change):** no new public doors.
  `visibleLineTotal()` counts VISUAL SLOTS; `visibleIndexOfModelLine()` = the
  line's FIRST slot (-1 hidden); `modelLineForVisibleIndex()` maps a slot to
  its owning line (continuation slots → the same line); `firstVisibleLine/
  visibleLineCount` stay row-windowed; `totalHeight() = 2·padding +
  visibleLineTotal()·lineHeight`. CHANGELOG carries this as a semantic
  (unit) change, `parity_monaco.py`'s `lh` scroll math is revalidated.
- **One slot-space table:** `HiddenRun` moves to slots; fold∧wrap stays ONE
  table — `foldLine` hides the slot range `[slot(start+1) .. slot(end+1))`
  and may clip a wrapped line's slots PARTIALLY (decided over the
  "hide-the-whole-line" simplification). Blank alignment rows (car 2) will
  be the positive-weight direction of the SAME table.
- **Behavior:** WordWrap; unbreakable tokens OVERFLOW (never char-break);
  wrap width = viewport minus gutter/padding/fold-zone; when wrap is ON the
  h-scroll doors degrade to no-ops; continuation rows get full width (visual
  indent = a later `options()` knob, not in this car); current-line wash
  covers ALL slots of the line; fold markers ride the first slot, the
  hidden-run ellipsis the last visible slot before the range; caret up/down
  keeps column proximity across slots.
- **Cache:** GeoSlot grows wrap segments, keyed `(line, text, width)`;
  resize invalidates LAZILY per viewport with the HighlightEngine budget +
  escalation pattern. Perf gate: wrap-OFF cost must be byte-identical to
  v1.6.1 (early-out), per-keystroke budgets unchanged.
- **Knob:** `softWrapEnabled` in `CodeEditorOptions` (14th) — EditorSettings
  + dialog + `tst_demo` drift tripwire move together.
- **DoD:** pure view-line-map tests BEFORE wiring (the E1→E2 lesson, risk
  №3); numeric tripwires — slot-sum identity, caret-y = slot·lh, hit-test
  correctness mid-wrap, up/down column proximity; resize storm on the perf
  doc inside the escalation budget.

### M10 — diff view (car 2)

- **`src/diff/`** — new QtCore-only layer: Miller–Myers over hashed lines
  (pure, headless, tested like `tst_textbuffer`; ctest grows a 4th test
  `diff`) producing hunks + the alignment table. **Layering decision
  (2026-11-16, owner):** `qmn_view` PRIVATE-links `qmn::diff` — diff is a
  pure QtCore layer like `core`, NOT a network layer, so the
  "never in the view" isolation applies to protocol/LSP/DAP only; no diff
  type appears in ANY public view header (the doors take `CodeDocument*`/
  `QString` + view-nested types only, so they stay Shiboken-wrappable and
  the link never propagates to consumers' compiles; installed-package
  export mirrors the `qmn::internal_tree_sitter` mechanism: link-only
  internal target). The demo/app side still owns *when* and *what* to diff.
- **Doors (widget-shaped parity, because qtmonaco's `open_diffs` is a
  method OF the editor, not a container):** `setDiffDocuments(CodeDocument*
  original, CodeDocument* modified, language)` + a plain-QString overload,
  `clearDiffDocuments()`, `isDiffMode()`, `nextChange()/previousChange()`
  (over the existing `revealAt`). In diff mode: caret/undo/`textChanged`/
  `cursorMoved`/`toPlainText()` all speak the MODIFIED document (qtmonaco:
  `_value = modified`); original is selectable-for-copy but never editable;
  find/fold/`setHighlightedLines` apply to modified; `readOnly` applies to
  modified (original is always read-only, same as Monaco).
- **Paint:** two content columns + a gutter per column + the connector
  strip; ONE row axis — `visibleLineTotal()` = aligned rows, blank rows are
  the positive-weight extension of the HiddenRun machinery in slot space
  (no second map).
- **Theme:** additive `Role` entries `DiffAddedBand/DiffModifiedBand/
  DiffDeletedMarker/DiffConnectorFill/DiffChangeGutterMark` — severity
  colors are NOT overloaded for diff.
- **Live update:** `editApplied` (the signal's car is THIS one, not M8 —
  the diff needs re-key deltas just as badly as the syncer does) → hunks
  debounced-recomputed in v1 with a budget, caret-anchored re-alignment (no
  row jumps); incremental application and word-token inline highlight are
  M10b.
- **Descoped (honest):** the tree-sitter structural-alignment pass moves to
  a follow-on car — line diff is the permanent basis and the permanent
  fallback; AST alignment can only ever be a REFINEMENT and physically
  cannot live in `src/diff/` (vendored tree-sitter is `qmn_view`-private).
  Inline/unified diff mode and Monaco `options` mapping: out of v1,
  documented.

- **Car-round ownership + axis decisions (2026-11-16):** the editor's OWN
  attached document IS the modified side — caret/undo/find/fold/
  `textChanged`/`toPlainText`/`readOnly` speak it natively, zero surgery;
  the original is a **detached (standalone) `CodeDocument`** — the new
  no-editor ctor installs its own core edit-observer, so its `editApplied`/
  `textChanged`/`modificationChanged` fire (this replaces M25's "detached
  documents are silent" note, which already reserved the diff car). The
  pointer door requires a detached `original` and keeps it NON-OWNING (a
  `destroyed()` hookup clears diff mode); a `modified` pointer different
  from `document()` is a one-shot text feed through the editor's replace
  machinery (the editor's model stays the single writable truth). The
  string door feeds both sides (the original into a lazy editor-owned
  child standalone doc). `language` = ONE language for the whole diff
  view (routes through `setLanguage`).
- **Row axis (the M25 machinery generalized):** aligned rows are the slot
  space, exactly one live space at a time (diff > wrap > fold/plain):
  blank cells are weight-1 pseudo-rows with no backing line (the
  positive-weight twin of the fold `HiddenRun` machinery — same prefix-sum
  algebra, keyed by ROW not line, no second map); a collapsed fold zeroes
  the weight of every row whose MODIFIED line it hides (the original
  twin hides with them — honest v1); soft wrap is **inert in diff v1**
  (the wrap seam keeps fed so the map stays alive for the diff-off world;
  two-column wrap budget math is a follow-on car). `visibleIndexOfModelLine`
  / `modelLineForVisibleIndex` speak MODIFIED lines over that axis
  (blank-slot inverse = nearest visible modified line, nearest-below
  first). `nextChange`/`previousChange` use the strict-edge rule (from any
  line at/inside a hunk, next goes to the NEXT hunk) and do NOT wrap
  around at the document edges (documented divergence from Monaco's
  cyclic navigation).
- **Live update (v1 mechanics):** every primitive mutation of the modified
  buffer (the editor's observer pass) and every `editApplied` of the
  original doc → a 120 ms single-shot debounce → FULL recompute with the
  engine's auto budget → caret-anchored scroll restore (the caret keeps
  its screen row; a caret inside a vanished line degrades to nearest-
  line clamping). Fold-map changes re-key the row axis WITHOUT recompute
  (prefix tables only).
- The `diff` engine result never leaks through the header: the state sits
  behind a forward-declared `DiffState` (defined in the .cpp, holds the
  engine records + the row prefix tables + the modLine→row map).
- **Machine 3 — the two-column chrome (decided + built 2026-11-17).** The
  ORIGINAL side enters the widget THROUGH THE GUTTER: `gutterWidth()` grows
  by `diffChromeWidth()` and the x-frame tiles the gutter with zero slop
  (`pad` = `gutterInnerPad()`, `bpPad` = the v1 breakpoint slot, `fz` =
  `foldZoneWidth()`):
  `[padding_, +bpPad)` breakpoint slot (v1) | the fold zone FLIPS from the
  gutter's right edge to its LEFT one — the new door `foldZoneLeft()` =
  `padding_+bpPad` (flush, no inner pad) — | ORIGINAL digits column
  (`diffChromeDigitsCol` = orig-digit-count × cell + 2pad; the ORIGINAL
  document decides the digit count, it never reflows) | ORIGINAL text read
  window (`diffChromeTextCol` = the 50/50 split:
  `clamp(round((width() − 2·padding_ − gutterV1Width() − digits − gap) / 2),
  12..60 cells)`, so the read window and the MODIFIED text region come out
  the SAME width up to ≤1px qRound slop — the live-check fix of
  2026-11-17 replaced a flat 0.30 × width(), which left the left half
  visibly narrower; never document- or hScroll-dependent; the read
  window has NO horizontal scroll, clicks clamp to its last reachable
  col) | CONNECTOR strip (`diffChromeGap` = max(10, 1.5 × cellW)) | the
  MODIFIED number column `[stripRight, contentLeft())` keeps v1 verbatim
  (digits + both pads, right-aligned at `textLeft − pad`). `stripRight ==
  origTextRight + diffChromeGap()` (== chrome block's end); `diffChromeHit`
  = `[foldZoneLeft()+fz, stripRight)` — the modified number column stays
  OUTSIDE the hit zone, so v1 gutter clicks (breakpoints, caret-to-col-0)
  keep their exact v1 semantics. The three public geometry doors
  (`diffChromeTextCol/DigitsCol/Gap`, summed by `diffChromeWidth()`) are
  the single definition `diffFrame()` lays out and `gutterWidth()` sums —
  no mapping/geometry PRIMITIVE added (Gate-1.5 additive precedent).
- **The ORIGINAL column is a read-only canvas with ONE data path** (the
  public door `originalSelectionText()`): click/drag/double-click selection
  in ORIGINAL coordinates + `copy()`. Selection state = anchor+caret (v1's
  `anchor_/caret_` mirror) with an offset-sorted pair derived by the ONE
  writer; a plain chrome press sets anchor==caret (a placement), Shift
  moves the FAR end from the anchor only when the chrome already owns a
  caret (a first Shift+click into an empty column places), drags extend
  from the anchor. Chained double-clicks run v1's 3-step chain verbatim
  in ORIGINAL space: dclick#1 word, dclick#2 whole line + trailing
  newline, dclick#3+ grow one line per click with v1's edge-flip rule.
  Routing: a press outside the chrome block drops any live original
  selection (the selection focus rule); the chrome swallows right-clicks
  and its own context menu (the selection stays live — Ctrl+C is its
  exit); ANY key press drops it EXCEPT the Copy shortcut and bare
  modifier presses (Shift must be holdable for Shift+click); `cut()`
  drops it and runs v1's verb on the MODIFIED side (the ORIGINAL is never
  cuttable, its document is untouchable); `paste()` is deliberately NOT
  exempt (it speaks the clipboard into the modified buffer).
- **Paint split:** `drawDiffChromeUnder` (after the selection quads,
  before the gutter pass) paints the MODIFIED bands (`rowKind`: replace →
  ModifiedBand, pure insert → AddedBand, pure delete → no band — the
  hatch is its marker) and an OPAQUE Base ground over the chrome block
  (the v1 current-line/highlighted-lines wash spans x=0..width() and
  paints BEFORE this pass — the read side gets its own frame; the fold
  zone stays OUTSIDE the ground and rides the v1 wash verbatim);
  `drawDiffChromeOver` (after the text loop) paints the connector
  wedges, the 45° delete hatch + row frame over the blank MODIFIED number
  cell, the 2px `DiffChangeGutterMark` bar at the strip's right edge for
  replace rows, and the original-selection wash/extent hairlines (clamped
  to the read window). Geometric AA is off (no render hints), so the
  pixel tripwires assert exact role colors.
- **The read window speaks the SAME token roles (live-check follow-up,
  2026-11-17):** the chrome's text pass is syntax-highlighted through a
  SECOND `HighlightEngine` instance (`origHighlight_`) over the ORIGINAL
  buffer — same grammar, same protocol: created fresh in `beginDiffMode`
  (attach → `setLanguage` rebuilds its line index from the fed text),
  dropped by `clearDiffDocuments()` (it borrows the original's buffer),
  fed incrementally by the original doc's `editApplied` fan (the same
  connect that restarts the diff debounce), and windowed by paint from
  the ORIGINAL line range of the rows about to be drawn (not the
  modified-side model window). Drawing rides v1's FormatRange protocol
  verbatim (theme token map → `HighlightEngine::colorFor` fallback →
  skip invalid, grapheme-snap; uncolored runs and language-less sessions
  keep the GutterDigits pen, the single-pen path). Word-level INLINE
  diff coloring stays M10b's. Tripwire:
  `diffOriginalChromeSyntaxHighlight` (Keyword+String inks in the read
  window + the live-edit re-color through the original doc's own door).

### M8 — protocol core + LSP (car 3) — ✔ CLOSED 2026-11-19 (package 1.8.0)

✔ **CLOSED 2026-11-19 (1.8.0, all seven machines landed):** 1 protocol
7212000 → 2 `LspClient` + fakeserver 86fdeed → 3 syncer + position converter
99e14d1 → 4 the `LspHub` app glue 089159f → 5 the write path f25640c → 6 the
popups + `willSaveWaitUntil` 449e11c → 7 the qtmonaco parity wrap 23c537a.
Gates at the close: ctest 5/5 with the `lsp` binary at 62 slots (grown inside
ONE entry, per the 2026-11-17 amendment) and `tst_codeeditor` at 254; the
consumer packaging smoke green against the WIDENED export set
(`qmn::protocol` + `qmn::lsp` are real exports, `qmn_glue` is app code and
ships nothing); the pinned container's `--wheel` green (codegen clean, smoke +
qtmonaco parity against the REPAIRED 1.8.0 wheel); and the manual close-car
clangd probe **PROBE-OK (7 steps)** against clangd 23.1.5 — the first run of
this ritual, exactly as PLAN's 2026-11-18 decision required it. Two car-sized
lessons survive in the code: the layer rule is now literal (nothing under
`src/protocol|lsp` knows a widget, the hub is a CLIENT of both sides), and the
fakeserver's exit code is an oracle no assertion can fake — it caught our own
encoder counting CODE POINTS where the model counts UTF-16 CODE UNITS. What
M8 deliberately did NOT do, all recorded above: no `set_lsp_header` pair, no
semanticTokens painting (decoder-only), no markdown in the popups, glue-side
popup TRIGGERING left to the app — that last one is WIRED post-close (see the
status paragraph above: `setPopupTriggers`, 62 -> 66 slots). DAP (car 4) is
the one Gate-2 car left open.

Machines built (2026-11-18): 1 `src/protocol/` ✅ → 2 `LspClient` + fakeserver
oracle ✅ → 3 document syncer + position converter + the `sync`-mode fakeserver
✅ (lsp binary at 39 slots; the crashonce one-shot is HARNESS-side — the
fakeserver flag is per-process, so only spawn #0's argv carries it) →
4 app-layer glue ✅ (2026-11-19): `src/app/glue/LspHub` (a NON-exported
`qmn_glue` static lib — app code like `qmn_demo_app`, package surface
unchanged): server matrix + `.qmn/lsp.json` deep merge + lazy start +
`editApplied`→`applyDelta` + the diagnostics decode into
`CodeEditor::setDiagnostics`. lsp binary at 46 slots and now
widget-carrying (`QTEST_MAIN`, offscreen, links `qmn_view`). Two lessons
this car paid for: **the hub destructor owns the stop barrier** (a bounded
`kStopBarrierMs` pump — without it `~QProcess` SIGKILLs a server that never
answered its `exit`, "QProcess: Destroyed while process is still running");
and the fakeserver's OWN encoder counted CODE POINTS where the model frame
counts utf-16 CODE UNITS — one character per surrogate pair of drift, caught
by the two-sided design on the first utf-8 diagnostics run. Machine 5
(the WRITE path — feature request doors + the four-shape `WorkspaceEdit`
decoder + an ALL-OR-NOTHING applier + the view door `CodeEditor::applyTextEdits`
+ the fakeserver `features` mode) ✅ (2026-11-19): lsp binary at 57 slots,
ctest still 5 entries. It is also the FIRST M8 machine to widen the VIEW
surface — the binding's three copies grew the nested `TextEdit` record
(typesystem entry + a `_generated` TU name; the codegen CLI rides the
already-listed `CodeEditor.hpp`), `--codegen` rejected-log clean for
`applyTextEdits`, and the door was probed from the container build tree
(list in, bottom-up walk, ONE undo group reverses it). The hover/completion
POPUPS and `willSaveWaitUntil` slide to machine 6 — the write path ships
without them. Rules machine 5 decided (in code now): every feature door
flushes the syncer first and refuses LOCALLY (SERVER code `-32601` on the
reply, id 0) when no spec claims the language, no live server serves the
uri, or the capability is absent — nested gates included
(`completionProvider.resolveProvider`); the applier gates EVERY uri of a
decoded edit before touching anything — structure operations (create/rename/
delete file) hand to the app whole, version-pinned `documentChanges` are
checked AFTER a flush (version == what the server holds), cross-wire
(`servingClient`) guard, readOnly and overlapping-range refusals — and then
applies per document via the `beginServerEdit`/`endServerEdit` pair
(N documents = N undo steps); the hub answers the server's OWN
`workspace/applyEdit` with the truth (`respondApplyEdit`) and relays
`window/showMessageRequest` WITHOUT answering — the first answer to
`respondShowMessageRequest` wins over the client layer's Null default, and
the fakeserver judges both halves from its side (`APPLYREQ-OK`,
`APPLYCHECK-OK`, `APPLYREFUSE-OK`, `MSGREQ-OK`/`MSGREQ-NULL-OK` ride the
didSave-marker + stderr channel, since client→server answers are traffic
the test cannot see).
Machine 6 (the POPUPS + the `willSaveWaitUntil` save path) ✅ (2026-11-19):
  lsp binary at 60 slots, ctest still 5 entries. The VIEW side (surface
  widened additively again): nested records `CompletionItem{label,detail}` +
  `HoverRange{startLine,startCol,endLine,endCol}` and six doors
  (`showCompletionList`/`hideCompletionList`/`completionListVisible`,
  `showHover(range,plainText)`/`hideHover`/`hoverVisible`); the popups are
  `FindBar`-style view-internal widgets in CodeEditor.cpp — display-only and
  FOCUS-INERT (`Qt::NoFocus` + `WA_ShowWithoutActivating`, and
  `WA_TransparentForMouseEvents` on the list AND its viewport, else the list
  swallows the click and the auto-hide contract breaks on REAL clicks —
  QTest-on-the-popup alone never catches that), a selectable hover label
  would too; press on EITHER popup dismisses BOTH. Anchors ride EXISTING
  geometry only (the a11y characterRect y-formula + `xForCol`): the
  completion anchor freezes at SHOW time, scroll/resize RE-PLACE a visible
  popup, the wheel never hides one (documented choice), the shared placer
  FLIPS above when the below branch overflows; empty payload HIDES, re-show
  REPLACES in place. +3 tst_codeeditor slots (the visible-door pins need
  `editor->show()` — `isVisible()` is false under a hidden ancestor, the M6
  findBar trap again). The SAVE path is HUB-side, not syncer-side: the
  `textDocumentSync` OPTIONS-OBJECT gate (`willSaveWaitUntil:true` — the
  integer change form can never carry it) → flush → request (reason 1) →
  the reply decodes as bare `TextEdit[]` OR `WorkspaceEdit` → the applier
  (its `endServerEdit` full sync rides BEFORE the didSave, so the server
  saves the EDITED model) → didSave → `markSaved` rides the REPLY; refusal
  or failure never vetoes the save; no capability = byte-identical
  synchronous door. The fakeserver `wswu` flag declares the options object
  and runs the drills (bare list replies applied to its own model + the
  `g_applyVerify` oracle reused; the REFUSE drill answers a
  WorkspaceEdit naming a not-open uri — honest deviation: the spec's
  result is `TextEdit[]|null`, only this shape can carry a refusal the
  uri/version guards act on); tripwires exit 2 if a wswu request reaches a
  server that never advertised it or if didSave overtakes the sync
  (`WSWU-SAVE-OK`/`WSWU-REFUSE-SAVE-OK`). The binding's three copies grew
  BOTH records (typesystem + two `_generated` TU names, 14→16; the codegen
  CLI rode the already-listed `CodeEditor.hpp`) — and the drift tripwire
  earned its keep: forgetting the CMake TU entries configured CLEANLY,
  ninja silently skipped compiling them, and smoke died at import with
  `undefined symbol: init_qmn_view_CodeEditor_CompletionItem`. Probed from
  the container build tree (shown editor): the doors bind camelCase and
  work; records are built default-ctor + field assignment — kwargs being
  silently swallowed is shiboken UNIVERSAL (PySide6's own
  `QPointF(x=1.5,y=2.5)` reads back 0.0/0.0, and our v1.5 `TextPosition`
  behaves identically), not a machine-6 regression.
Machine 7 (the PARITY WRAP — qtmonaco's `add_action` +
  `update_workspace_configuration`) ✅ (2026-11-19): lsp binary 60→62 slots,
  tst_codeeditor 252→254, ctest still 5 entries. The VIEW side (surface widened
  additively): doors `addContextAction(id, label)` +
  `storedWorkspaceConfiguration()`/`setStoredWorkspaceConfiguration(QString)`
  and signals `contextActionTriggered(QString)` +
  `workspaceConfigurationChanged()`; custom items append BELOW a separator
  under the built-ins in registration order, re-adding an id REPLACES its
  label and keeps the slot, an empty id is a local refuse, and the read seam
  is `QStringList contextActionIds()` — deliberately NO new bound record type
  (a record would grow the binding's three copies for one string). Naming
  divergence (decided): the door is `addContextAction`, NOT `addAction`,
  which would hide `QWidget::addAction(QAction*)` from every C++ consumer.
  The qtmonaco `language` precondition stays a SHIM-only accepted-and-ignored
  parameter — the native editor is already scoped per document. The VIEW is
  the OPAQUE BUFFER (it never parses, never applies — the glue is the only
  reader); the HUB owns "buffered until init": `LspClient::ready` pushes the
  buffer for that spec's registrations, `openDocument` pushes after `open`,
  and the live re-push connect's context is `reg.syncer` so it dies with the
  registration and a re-OPEN cannot double-send. Two refusals are LOCAL and
  silent: an EMPTY buffer means "never configured" and sends NOTHING (an
  empty `settings` object would claim "no settings", a different thing), and
  a buffer that is not a JSON object has no legal params shape. The
  fakeserver `config` flag judges the wire from its side — config before
  `initialized`, or absent/non-object params, or an absent/empty `settings`,
  is exit 2 — and echoes every accepted object as `CONFIG-<n> <compact json>`
  through `window/showMessage`, so order AND payload are pinned byte for
  byte; the second slot runs machine 4's crash ladder (buffer set while the
  client is FAILED → nothing travels → the app's `restartServer` re-reads the
  VIEW, not a hub-side cache). No new bound type, so no codegen run owed for
  this machine. Shim side: `add_action(action_id, label, language=...)` and
  `update_workspace_configuration(dict)` (dict → sorted compact JSON, the
  exact string the hub pushes), relaying `context_menu_action_triggered(str)`
  and `workspace_config_updated(bool, str)` — where `True` means the buffer
  was accepted LOCALLY, not that a server confirmed it (v1 has no round-trip
  to report, decided divergence). And `set_lsp_header`/`get_lsp_header` stay
  ABSENT ON PURPOSE: qtmonaco's header is text prepended to the document ON
  THE WIRE ONLY, while all six native LSP paths rest on the invariant "the
  server's document IS the editor's document" — caret requests, response
  ranges, syncer deltas and undo would each pay a manual ±H shift, and the
  all-or-nothing applier + version pins turn any off-by-one into silent
  feature death; server context belongs in `initializationOptions`, the
  workspace configuration, or a second document.

- **Layers:** `src/protocol/` (QtCore+QtNetwork, zero view — Framing with
  full header-block parse + 64 MiB upper bound that fails as a transport
  error, not an allocation; ProcessTransport via QProcess stdio;
  TcpTransport; RpcClient = id allocation + pending-map + per-door timeouts
  (initialize 10 s, feature doors 5 s, NOT a blanket 15 s) + `$/cancelRequest`
  on supersede/timeout + a write queue whose errors SURFACE + the two-phase
  stop barrier + pending-eviction with a `TransportClosed` error) →
  `src/lsp/` (LspClient, document syncer, position converter, server
  matrix) → `src/dap/` (car 4, same RpcClient/transports, DAP envelope
  `seq/request_seq/command`). New export sets (planned: `qmn::protocol`,
  `qmn::lsp`); the protocol layers NEVER know CodeEditor/CodeDocument —
  "zero widget dependency" holds literally; glue (diagnostics→
  `setDiagnostics`, syncer↔`CodeDocument::editApplied`) lives in the app
  layer. QtNetwork joins `find_package` (qt6-base is already a CI dep).
- **Server→client rule "always answer, never lie":** implement
  `workspace/applyEdit` (→ glue), `workspace/configuration`,
  `workspace/workspaceFolders`, `window/showMessage` (→ SIGNAL, visible —
  not a debug print), `window/logMessage`/`$/logTrace`, `$/progress` +
  `workDoneProgress/create`; unknown request → `-32601`, unknown
  notification → ignored. **`client/registerCapability` gets a real
  capability registry** (decided 2026-11-15 over the `-32601` shortcut: the
  server drives; statics are not declared where dynamic registration is
  expected; unknown registration method → error response). v1 registration
  sources: completion/hover/definition/references/codeAction/formatting/
  rename/textDocumentSync-change.
- **semanticTokens (decided 2026-11-15):** the WIRE + decoder ships (full
  tokens, legend, relative encoding, `resultId`, server-side dynamic
  registration) but we NEVER REQUEST them (no static capability, no accept
  of unregistered pushes for painting — decoded and kept as data only).
  tree-sitter stays the paint master; a blend option is a future car.
- **Position units:** declare `positionEncoding:"utf-16"`, honor the
  server (clangd → `utf-8`); the utf-8/utf-32 column converter lives in
  `src/lsp/`; spec default when absent = utf-16. Round-trip tests on
  Cyrillic/emoji/surrogates per encoding against fake servers; live clangd
  is a manual probe only (CI has no clangd).
- **Lifecycle:** lazy start on first `didOpen` per language; one client per
  server spec (multi-document, VS Code shape); crash → auto-restart ONCE
  with backoff, then FAILED + `restartServer()` door; after re-READY a
  **re-didOpen self-heal** re-syncs every document; per-server PROTOCOL and
  STDERR logs stay separate channels.
- **Syncer contract:** deltas batch into ONE `didChange` `contentChanges`
  array (spec = sequential application, no coalescing by design); 400 ms
  debounce + a typing cap; version increments per flush; FULL fallback
  (range-less didChange) on any desync detector and after server-primed
  edits — the syncer suppresses the queue during its own WorkspaceEdit
  application, then sends one full-text sync (iide's
  `suppress_lsp_echo`+`force_full_sync` pair, living on the syncer side so
  the view stays dumb); `didOpen/didSave/didClose`, `willSaveWaitUntil` when
  the server declares it.
- **Feature doors v1:** publishDiagnostics (push), completion (+resolve),
  hover, definition, references, codeAction + edit application (all THREE
  WorkspaceEdit shapes incl. the clangd `command.arguments[0]` form; edits
  applied DESCENDING inside ONE undo group), formatting, rename (single-file
  applies, multi-file hands the structure to glue). M8b: signatureHelp,
  inlay hints, foldingRange (the M13 fold API takes an LSP provider), pull
  diagnostics (enabled by the registration registry). Not now:
  didChangeWatchedFiles, willRename/didRename.
- **Config:** built-in server matrix with initialize templates + the
  `${WORKSPACE_ROOT}` macro, deep-merged `.qmn/lsp.json` (objects recurse,
  arrays replace whole) loaded BEFORE anything starts; `setWorkspaceRoot()`
  is an app decision.
- **Hover/completion popups:** view-INTERNAL popup widgets (the M6 `FindBar`
  precedent) fed through record-struct doors (`showCompletionList`,
  `showHover(range, plainText)`, hide + auto-hide on key/mouse); anchors use
  the EXISTING `xForCol` machinery — no new geometry primitives; plain-text
  content, no markdown renderer. All triggering is glue-side (`cursorMoved`/
  `editApplied`).
- **Fake servers (the CI gate):** `tests/fakeserver/` — a C++ LSP server
  spawned via QProcess; recorded-transcript replay PLUS a smart mode with a
  true server-side text model that rebuilds from our `contentChanges` and
  answers VERIFY per flush — deltas are validated before the wire. The
  harness copies the EXACT initialize templates (iide lesson: a harness that
  diverges from the template lies). Live clangd = `tools/probe_clangd.sh`,
  not a gate — but a CLOSE-CAR RITUAL (decided 2026-11-18): the probe must
  run green against the dev machine's clangd before M8 closes, result
  recorded in the CHANGELOG entry (manual close-car gate, never a CI gate —
  clangd versions drift and CI has no clangd). ctest grows test 5 `lsp` (AMENDED 2026-11-17:
  `tests/tst_lsp.cpp`, STARTS at machine 1 guiless with the protocol
  framing/RpcClient tests and grows machine by machine — no further ctest
  entries inside M8; the fakeserver lands at machine 2 into the same
  binary. AMENDED AGAIN 2026-11-19 by machine 4: the hub slots drive a real
  `CodeEditor`, so that ONE binary is now widget-carrying — `QTEST_MAIN` +
  `QT_QPA_PLATFORM=offscreen`.)

### DAP features (car 4)

Same RpcClient/transports, DAP envelope only. The widget half is ALREADY
DONE (D3: breakpoint glyphs, current-line, `setInlayHints` named for DAP) —
inventory confirms zero view work; verified-breakpoints surface as a
decoration restyle through the existing doors (iide ignored
`body.breakpoints` — we don't). Fake DAP adapter in the same harness style.

### Gate 2 at the wheel boundary (rides the owning cars)

Parity tripwire = the qtmonaco `monaco.py` `__main__` block reproduced
door-for-door in `bindings/tests/parity_monaco.py` (its "Gate 2 absent"
asserts INVERT into behavior asserts):
- `open_diffs` → the plain-QString overload of the M10 door (SHIPPED with
  car 2: the shim `Monaco.open_diffs` + the close_diffs/set_text diff-exit
  pair). `uri` params are accepted-and-IGNORED, permanently by design: the
  typesystem removes BOTH CodeDocument ctors (the M10 standalone `()` ctor
  included — Python never builds a document, so there is no Python-side doc
  to URI); `options` dict is accepted, unknown keys ignored,
  `renderSideBySide:false` documented as a divergence (inline mode is out
  of v1). The pointer door `setDiffDocuments(CodeDocument*,...)` stays
  C++-only for the same reason.
- `set/get_lsp_header` — **NOT built, decided 2026-11-19** (this line used to
  plan them as PURE PYTHON: the shim prepending/stripping a header while the
  native model stayed header-free, mirroring qtmonaco's `_value`). The
  reason it was dropped, in one sentence: every native LSP path rests on
  "the server's document IS the editor's document", so a wire-only header is
  a manual ±H shift in caret requests, response ranges, syncer deltas and
  undo — and the all-or-nothing applier + version pins turn an off-by-one
  into silent feature death. The honest `AttributeError` stays; extra server
  context belongs in `initializationOptions`, the workspace configuration,
  or a second document.
- `add_action` — SHIPPED (M8 machine 7) as the view door
  `addContextAction(QString id, QString label)` + `contextActionTriggered(QString
  id)` into the M16 context menu (**renamed from the planned `addAction`,
  which would hide `QWidget::addAction`**); the shim keeps the qtmonaco
  signature and IGNORES the `language` precondition (native language
  scoping does the job) — decided divergence.
- `update_workspace_configuration` — SHIPPED (M8 machine 7) as the view door
  `setStoredWorkspaceConfiguration`
  (compact JSON as `QString`; PySide6 6.12 has NO `QJsonObject` — verified
  in the container) + getter + `workspaceConfigurationChanged()` signal;
  the buffer lives on the view until app-side glue consumes it — qtmonaco's
  buffered-until-init semantics without shipping any LSP client to Python
  (the hub that consumes it is C++ app glue).
  Shiboken fact this rests on: **Python cannot emit a C++ signal**, so every
  parity signal stays Python-declared exactly as the three existing ones.
- Binding routine: two new bound-QObject entries (`CodeDocument`, popup/
  diff record structs) through ALL THREE copies (typesystem / bindings
  CMake / the codegen CLI list) + mandatory `--codegen` rejected-log check
  per door batch (the 1.6.1 drift trap) + auditwheel excludes in their two
  homes.

### Prior art: iide — borrow / anti-pattern

BORROW (their working parts, verified by read): the two-phase stop barrier;
separate protocol/stderr log channels; re-didOpen self-heal after re-READY;
`suppress_lsp_echo`+`force_full_sync` convergence pair; 400 ms debounce +
monotonic version + FULL fallback; descending edit application inside one
undo group; the three WorkspaceEdit shapes incl. clangd's
`command.arguments[0]`; raw-JSON diagnostics echo in `codeAction` context;
initialize-as-template + `${WORKSPACE_ROOT}` + project deep-merge loaded
before session restore; lazy per-language server start; the frozen-delta
(PendingChange) idea — our model is UTF-16 natively so deltas are already
LSP-shaped. NEVER COPY (mostly self-declared in their RECOMMENDATIONS.md):
byte/codepoint/UTF-16 unit mixing on the inbound path (their
`get_iter_at_line_index` at UTF-16 columns); hardcoded codeAction range
`0..99`; the "no diagnostics on line" actions gate; line-only diagnostics
(columns dropped); "first provider only" for definition/rename/formatting;
sequential workspace/symbol fan-out; `registerCapability` acked-and-ignored;
missing `didChangeWatchedFiles`/`didChangeConfiguration`/`didSave`/
`willSaveWaitUntil`; blanket 15 s timeouts + fake `-32801`; no
`$/cancelRequest`; no auto-restart; the god-class `LspClient` and the
Lsp/Dap RPC duplication their own doc flags; write errors swallowed
mid-queue. NEGATIVE CONTROL: iide has NO diff view, NO change gutter, and
never enables wrap (every renderer assumes 1 visual = 1 logical) — the wrap
and diff halves of Gate 2 have NO prior art there and are designed from our
own E1/E2 map.

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
   the old attempt before retrying. Gate 2 mitigation (decided 2026-11-15):
   the train runs wrap FIRST precisely so nothing is built on row math
   before the units settle; the fold∧wrap interaction (partial slot hides)
   is the single known-hard seam and gets dedicated tripwire tests.
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
8. **Gate 1.5 widens a FROZEN surface** — the one sanctioned exception;
   discipline: additive-only minors (`v1.1.0`, `v1.2.0`, …), the built-in
   language registry starts EMPTY and the default theme reproduces
   today's pixels exactly, so every upgrade is behavior/pixel-identical
   until the user opts in; binding + wheel re-run per milestone; the
   synthetic-hover offscreen routing is probed BEFORE M22's tests are
   trusted.

Out of scope by design: vim mode (setter exists and warns). Soft wrap is
*deferred* to Gate 2, not out of scope.



