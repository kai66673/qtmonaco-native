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

# GATE 2 — IDE surface (opened 2026-11-15) — ✔ CLOSED (v1.9.0 + v1.9.1, 2026-11-21)

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
their own. (AMENDED 2026-11-21: the glue-shelf train G1+G2 — see "The glue
shelf + ProjectSession" below — was ordered BEFORE car 4 by the user, since
the DAP glue lands ON that shelf.)

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

**The fourth deferred sliver is BUILT (2026-11-21, no version bump): the
MOUSE-pointer hover.** The view grew the additive door `setMouseHoverEnabled`
(OFF default; enabling ORs a mouse-tracking bit of its own onto the M22
fold-strip condition) + the signal `mouseHovered(TextPosition)`: a
motionless button-less pointer inside the TEXT region fires it EXACTLY once
after `kMouseHoverDwellMs` (500 — the same hover cadence the glue's caret
trigger uses). This is dwell CHROME on the display side: the view only
REPORTS the point under the pointer (the existing `positionAt` math at FIRE
time, so a scroll under a still pointer hovers what sits under it NOW; the
caret and selection never move) and shows NOTHING. Reporting STOPS over
either popup (a visible hover stays in place — the m6 popup never chases
the mouse — and the completion list owns the moment), over the gutter +
fold strip, the overview ruler and the diff ORIGINAL window, and on the
widget's leaveEvent or any press. The hub route: a `mouseHovered` tap (the
completionAccepted pattern: connected for EVERY registration, the `on`
bit + registration liveness + the completion-owns-the-moment rule make
everything else silent) lands in `fireHoverAt` — the shared-hover
composition core, REFACTORED so both routes ride it anchored at the hover
point, the caret path keeping a thin fire-time wrapper (which also clears
the mouse cache: a caret popup is a different region). A hub-side cache
skips the ask when the pointer re-settles on the SAME point while that
hover is still visible (no second wire ask for one dwell-region). Two new
slots pin it: codeeditor 256 -> 257 (the door-OFF silence even when a move
ARRIVES via sendEvent — the gate is the view's own, not Qt's tracking
filter — the {0,6} report, once-per-stillness, the caret/selection inert,
the gutter/popup/press/leave cancels) and lsp 72 -> 73 (the fakeserver
hover echo `HOVER foo off=12` proves the ASK rode the pointer point, not
the parked caret; a same-cell re-settle keeps the reqlog at ONE hover REQ).
The door + signal ride the ALREADY-BOUND CodeEditor: three-copies
untouched, the `static constexpr` dwell constant stays C++-only exactly
like `kOverviewRulerWidth` (Python sees the pair + the signal, probed in
the container).

**The test-infra flake fix rides this sliver (2026-11-21): the fakeserver
watchdog 20 s/25 s -> 120 s/60 s.** A loaded full-suite run stretched the
lsp binary 35 s -> 150 s and the capability-slot server died at the OLD
20 s lifetime watchdog while its 30 s QTRY doors were still legitimately
open — `exitCodeOf(0) == 3` read as a contract failure but was starvation
(the af0ff35 lesson applied to the leftover half: af0ff35 raised the DOOR
budgets, the SERVER lifetime was missed). The deaf-mode 60 s keeps its
roomy margin over the pinned 10 s initialize timeout; a truly hung session
still dies LOUD (exit 3 + stderr) far inside ctest's 1500 s default. Under
pathological synthetic load (six pinned busy-loops) even a 30 s door can
legitimately expire — the pin then reads as a clean QTest timeout, never
as a confusing exit-code oracle.

**The fifth deferred sliver is BUILT (2026-11-21, no version bump):
GRAMMAR EXTENSIBILITY — a runtime, process-wide grammar registry.** The
user-decided shape: (1) registration BY ADDRESS + a .so loader; (2) id
conflict = UPSERT with a LIVE rebind (a custom `python` MAY shadow the
vendored one); (3) the capture-role override doors ride the SAME sliver.
Six additive static doors on the ALREADY-BOUND `CodeEditor` (three-copies
untouched, rejected-log: unchanged QFlags-only noise):
`registerGrammar(id, qintptr languageAddress, highlightsQuery, foldNodeNames)`,
`unregisterGrammar(id)`, `grammarAddress(id)`, `loadGrammar(fileName,
symbolName)`, `setCaptureRole(id, captureName, TokenKind)` +
`clearCaptureRole`. The address is TYPE-ERASED to `qintptr` (a Python int)
so `tree_sitter/api.h` never enters the public header (Shiboken rule +
the PRIVATE qmn_ts link); Python calls it with `grammarAddress("python")`
out of the box. The mechanics: `HighlightEngine`'s frozen static `kRegistry`
array became a function-local-static `QVector<LangEntry>` (id, language
POINTER, owned query bytes, fold-node list, role-override map) seeded from
the vendored five, plus a generation counter; every engine carries
`regGen_` and `refreshIfNeeded` runs `reconcileRegistry` when it lags —
ONE integer compare on the paint fast path, no new mapping/geometry door.
A write reaches attached editors on their NEXT pass: same language
pointer -> the tree and the line index SURVIVE (query/capture-roles/folds
rebuild only), a DIFFERENT pointer -> `ts_parser_set_language` + tree drop
+ full reparse, entry GONE -> the editor falls to plain text and
`language()` empties (the unplug is next-pass, not door-synchronous —
pinned). `registerGrammar` VALIDATES the language through a scratch
`ts_parser_new`+`set_language` AT THE DOOR (version/ABI checked before
any paint can touch it; a zeroed TSLanguage reads version 0 and is
refused CLEANLY); what no door can validate is the pointer VALUE — the
contract says the address comes from `grammarAddress`/`loadGrammar`/one's
own compilation. Empty query = parse-only mode (folds/line tracking ride
the tree, no colors); foldNodeNames filter through `composeFoldQuery` as
always; role overrides ride ABOVE the fixed `kindForCapture` table AT
QUERY-BUILD time (unknown names GAIN a role, known names change it,
`Text` = paint nothing). `loadGrammar` honors the ecosystem convention
(`tree_sitter_<base>`, explicit symbolName overrides), KEEPS EVERY loaded
handle open forever (grammar unloading dangles — the ecosystem bans it)
and returns 0 on load/symbol failure. The new slot
`grammarRegistryRegistersUpsertsRolesAndUnplugs` (codeeditor 257 -> 258)
drives the whole surface — including a REAL second python parser: the
vendored `parser.c`+`scanner.c` compiled a SECOND time as a SHARED test
fixture (`qmn_pygrammar`, distinct pointer -> pins the live-swap reparse),
plus `qmn_fakegrammar` (libbogus.so: loads, calls, REGISTERS-AS-REFUSED) —
both wired via `target_compile_definitions` paths like the fakeserver
precedent, neither installed (not in any export set; consumer smoke
clean). The tests dir needed its own `enable_language(C)`: third_party's
enablement lives in ITS directory scope and sibling subdirs never see the
language-rule variables. The process-wide table is GUI-thread discipline
like everything here (no atomics); every id the slot mints is unregistered
again, and the vendored five ids stay byte-untouched for the other slots.

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

### The glue shelf + ProjectSession (post-M8 train, DECIDED 2026-11-21; design record)

The user-decided shape: (1) glue becomes a REAL second shelf — an exported
`qmn::glue` with its own semver; (2) ProjectSession's scope is DOCUMENT
SESSION + hub (uri ↔ editor ↔ syncer ↔ language, language resolution, spec
resolution) — NO disk I/O, NO file watching, NO workspace folders; (3) the
shelf is BINDED into the wheel too (accepted cost: `RpcClient::Reply` and
`QJsonObject` cross into Python, and the three-copies widen for the first
time beyond the view).

**G1 — the promotion surgery (no new API).** `src/app/glue/` moves to
`src/glue/` (the layer dir the M8 plan reserved); the target name stays
`qmn_glue`. It becomes a real export on its OWN export set: a second
`install(EXPORT qmnGlueTargets NAMESPACE qmn::)` +
`cmake/qmnGlueConfig.cmake.in` (`find_dependency(qmn)` BEFORE
`include(qmnGlueTargets)` — the config guarantees load order, which is what
satisfies CMake's cross-export-set link warning). Own semver MECHANICS: a
third version literal `set(QMN_GLUE_VERSION …)` in `src/glue/CMakeLists.txt`
(NO nested `project()` — it would reset `PROJECT_VERSION` and poison the
`QMN_PACKAGE_VERSION` define, whose contract is the PACKAGE version: the
hub's `initialize.clientInfo.version` must not start reporting the shelf
version). `write_basic_package_version_file(COMPATIBILITY
SameMajorVersion)` for the shelf; the shelf version starts at 1.0.0 (the
hub is NEW as public surface, though internal since M8). qmn's own
`qmnConfig.cmake` does NOT load glue — separate find, separate decision.
The layering clause INVERTS: `src/glue/` is a real layer (client of
core+view+protocol+lsp, QtCore/Widgets; never any app/demo type), and the
old M8 amendment ("`src/app/glue/` is the ONE app target allowed to know
lsp/protocol") becomes "app targets link the `qmn_glue` target; no glue
type ever appears in an app-defined header other than the forward-decls
already there". AGENTS.md gets the rewritten clause + the third-literal
rule. Includes `app/glue/LspHub.hpp` → `glue/LspHub.hpp` (hub .cpp,
MainWindow, tst_lsp); `examples/consumer` grows a glue consumer TU
(doors compile, NOTHING spawns — clangd is not a CI guarantee).

**G2 — `ProjectSession` (a glue QObject).** The registry of open documents
(uri → editor + languageId) that today lives three times over (MainWindow
tabs, spike's map, every consumer's own). The built-in suffix table mirrors
the spike/demo `languageForSuffix` copies (`.h` resolves to cpp — this
widget's home is C++/Qt; spike KEEPS its copy, it is a bench harness and
stays qmn_view-only); `setLanguageOverride(suffix, id)` + a NEW optional
top-level `"languages": {suffix: id}` key in `.qmn/lsp.json` (the hub
ignores it; the session reads it and hands `servers` to
`LspHub::loadConfigFromJson`). `openDocument(editor, uri, languageOverride)`
RESOLVES the language, calls `editor->setLanguage`, registers, and (hub
attached) calls `LspHub::openDocument` for the specs that claim the id —
one call owns the whole open. The session NEVER OWNS editors (observes
`destroyed()`, the hub's auto-unregister precedent) and never shows UI.
`closeDocument`/`openUris`/`languageForUri` complete the surface. New ctest
entry `glue` (offscreen, drives REAL `CodeEditor`s; no server spawn — a
spec-less hub's `openDocument` false is the honest no-op pin).
MainWindow's tab-open path rewires to the session (tabs stay a demo
presentation detail).

**G3 — the shelf at the wheel boundary (bindings).** `_qmn.abi3.so` absorbs
glue/lsp/protocol statically (the view/core precedent — no runtime .so
proliferation; QtNetwork comes from the PySide6-bundled Qt, in Essentials).
Bound: `LspHub` (+`ServerSpec` record), `ProjectSession`, and the minimum
protocol payload the feature callbacks need (`RpcClient::Reply`) — the
FIRST typesystem/CMake/codegen-CLI edit beyond the view types, rejected-log
audit mandatory. The `TransportFactory` std::function seam stays C++-only
(the `PerfStats` precedent: raw seam, honest absence). (AMENDED 2026-11-21
by the G3 design pass below: the callbacks never cross, so neither does
their payload — `RpcClient::Reply` stays unbound after all.)

**Ordering (user, 2026-11-21): G1+G2 NOW; this PRECEDES car 4 (DAP)** — the
DAP glue (breakpoint/frame feeds onto the decoration API) lands ON this
shelf, so the shelf must exist first. G3 follows once the C++ surface is
settled.

**BUILT 2026-11-21 (G1+G2 shipped together, host ctest 6/6, container +
both consumer smokes green; the shelf opens at 1.0.0, the package version
UNtouched — post-1.8.0 sliver pattern).** G1 shipped as recorded, plus one
decision the record didn't state: the hub's namespace moved `qmn::app` ->
`qmn::glue` (it was never public before, so this is not a break; app code
and tst_lsp follow). The consumer smoke grew a SECOND TU
(`examples/consumer/glue_smoke.cpp`, its own `find_package(qmnGlue)`) and
CI runs both smokes — it pins the cross-export-set load order FOR REAL
(`qmnGlueConfig` find_dependency(qmn) before qmnGlueTargets: the generated
`INTERFACE_LINK_LIBRARIES "qmn::view;qmn::lsp"` resolves cleanly, zero
CMake warnings). G2 shipped with the recorded scope plus the surfaces the
record only implied: the full session doors are attachHub/hub,
setWorkspaceRoot/workspaceRoot (FORWARD to the attached hub: one root, both
sides), loadConfig/loadConfigFromJson, languageForSuffix/setLanguageOverride
(case- and dot-insensitive, an EMPTY id REMOVES a mapping — configs may
unmap a built-in), openDocument/closeDocument/closeUri, openCount/openUris
(insertion order)/languageForUri/editorForUri/uriForEditor (linear: a
session holds tens of documents), documentOpened/documentClosed (the seam a
tab strip rides), and `setPopupsOnOpen` (default ON: the M8 popup-trigger
pair now arms INSIDE the session on a hub-accepted open — the demo's
explicit setPopupTriggers call is gone). Decisions born in the build: (a)
ONE document per uri — a different editor claiming a registered uri is the
honest FALSE (the LSP didOpen-twice rule), the same editor+uri is an
idempotent true; the demo's duplicate-path tab keeps its grammar via an
explicit suffix-table fallback; (b) the session NEVER fights the app: an
unresolved suffix never touches the editor's language (the user's own
setLanguage stays honored); (c) attach-after-open does NOT retro-open
documents (no lazy spawn behind anyone's back) but DOES forward the root
and REPLAY the last config body read, so the demo's --lsp-after-first-open
path still gets its servers — one config file, one reader (the session),
the hub's own loadConfig stays for hub-only users; (d) the demo's session
is ALWAYS on, --lsp only attaches/detaches a hub, and MainWindow.cpp's
local languageForSuffix copy is GONE (the spike keeps its own, bench-only).
The 6th ctest entry `glue` (offscreen, REAL editors, zero spawn: the
attached-hub slots open only languages NO built-in claims — the claimed
path stays tst_lsp's fakeserver turf).

**G3 design pass (DECIDED 2026-11-21, BUILT SAME DAY; the first three-copies edit
beyond the view).** Read against the REAL shelf surface, not the outline:

*The drop list — decided by the existing clause, not newly.* `LspHub`'s
public half is studded with the machinery AGENTS already declares unbound
by design ("the `PerfStats` and the SlotEdit/UndoMark/**std::function**
machinery"): `setTransportFactory`, and all EIGHT feature doors
(`requestHover/Definition/References/Completion/CodeActions/Formatting/
Rename` + `resolveCompletionItem` — their `FeatureHandler` arg), plus the
three unbound-type returners `syncerFor`/`clientForUri`/`clientForSpec`
(binding `LspClient`/`DocumentSyncer` would recursively drag the protocol
closure into the wheel for no Python use), plus all FOUR `apply*` doors —
they die on the defaulted `LspClient* servingClient = nullptr` tail even
though every other parameter is plain `QJsonValue`/`CodeEditor*` (the
`perfStats()` precedent: an unknown type ANYWHERE in the signature drops
the function silently into `mjb_rejected_functions.log`). The amendment to
the outline: with the callbacks never crossing, `RpcClient::Reply` has NO
door left to travel in — it stays unbound; the payload follows if/when the
callbacks cross. What Python LOSES: app-driven feature calls (hover on
demand, the rename flow, formatting, codeAction picking, applying a
reply-held WorkspaceEdit). What Python KEEPS — the entire AUTOMATIC half,
which is the bulk of the UX: config fold (session + hub), lazy server
start, didOpen/didChange/didSave incl. the `willSaveWaitUntil` detour,
pushed diagnostics as decorations, the M8 popup triggers (the session
ARMS them on hub-accepted opens by default), completion accept -> the
hub-side insertion, and server-primed `workspace/applyEdit` (answered
all-or-nothing hub-side; `applyEditRefused` reports). No live demand asks
for more, qtmonaco parity never did (`set_lsp_header` stays an honest
`AttributeError`), and the named extension path stays cheap: signal-relay
feature doors added to the SHELF (additive, shelf semver minor 1.1.0;
`Reply` is a plain 5-field record and becomes a `value-type` the moment a
door carries it) — DEFERRED, not refused. The `showMessageRequest` relay
signal crosses, its ANSWER route (`clientForSpec()->respond...`) does not:
the LspClient default answers for Python apps; documented, not bridged.

*Bound set:* `qmn::glue::ProjectSession` ENTIRELY (every G2 door already
speaks QString/QStringList/int/bool + `CodeEditor*` + `LspHub*` + the
`QJsonObject` body door; PUBLIC ctor — the app builds sessions),
`qmn::glue::LspHub` minus the drop list (root pair, `loadConfig`/
`loadConfigFromJson`, `setServerSpec`/`serverSpec`->`QJsonObject`,
`specNameForLanguage`, the open/close/save triple, `restartServer`, the
debounce pair, `setPopupTriggers`/`popupTriggersEnabled`, ALL seven
signals — QString/int/QStringList payloads plus `serverShowMessageRequest`'s
two `QJsonValue`s), and the nested `LspHub::ServerSpec` `value-type`
(fields name/program/args/languages + the `initializeTemplate`
`QJsonObject`). Lifetime: both classes are Python-built and Python-owned;
the session->hub pointer is raw NON-owning with the destroyed() hook, so a
GC'd hub leaves `session.hub()` reading nullptr — GC order is safe by the
G2 design, documented for the shim docstring.

*The JSON pin (first time anything crosses the wheel):* `QJsonObject`/
`QJsonValue` as DIRECT params have never been exercised here (the view
surface is QString-pure; QVariant-based APIs convert dicts, direct wrapper
params may NOT). PINNED AT THE BUILD (2026-11-21) — the reality is stranger
than EITHER planned branch: PySide6 6.12's own typesystem declares
`QJsonObject` a **PRIMITIVE type** (typesystem_core_common.xml), so there is
NO `QJsonObject` Python class to import AT ALL (QJsonArray/QJsonValue/
QJsonDocument are classes; QJsonObject is not). The Python **dict IS the
JSON object**: the generated doors convert dict↔dict DEEPLY — both
directions, params AND struct members (`loadConfigFromJson({...}) is True`,
`serverSpec()` returns a plain dict, `ServerSpec.initializeTemplate` accepts
one). Neither the planned shim fallback NOR the assumed `fromVariant` bridge
is needed (the latter has no class to hang off). NO string-body C++ door was
added — the ±H-offset lesson of m7 stands (wire-shaped taxes on doors for
binding convenience never pay), and it turned out nothing was even missing.
The smoke pins ARE the record; `QJsonValue` stays a bound class (the signal
params), so BOTH shapes cross now.

*Three-copies + pipeline (both AGENTS drift traps bit HERE, all three
edits + the codegen audit are MANDATORY):* (1) typesystem: a second
`<namespace-type name="glue">` sibling under `qmn` (Shiboken 6.12 still
forbids '::' in name attributes; the nested mirror shape is the view
precedent); (2) `bindings/CMakeLists.txt`: `HEADERS` += the two glue
headers, `_generated` += the PREDICTED TU names — `qmn_glue_wrapper.cpp`,
`qmn_glue_lsphub_wrapper.cpp`, `qmn_glue_lsphub_serverspec_wrapper.cpp`,
`qmn_glue_projectsession_wrapper.cpp` — VERIFY against the `--codegen`
file list before compiling (1.6.1 drift = "specified but not defined"
configure error; 1.7.0 drift = clean configure, silent no-build, import
explodes on `undefined symbol: init_qmn_...`); `LIBRARY_TARGET` stays
`qmn_view` + `target_link_libraries(_qmn PRIVATE qmn_glue)` (static
absorption: glue+lsp+protocol+core ride INTO the .so, no runtime .so
proliferation); `find_package(Qt6 ... COMPONENTS Network)` + the Network
include dirs join the -I loop (protocol's `TcpTransport` carries a real
QTcpSocket). (3) the hand-reproduced `--codegen` CLI in
`build_in_container.sh`: header list += the same two headers. Rejected-log
audit after codegen: the NEW expected entries are exactly the 16 dropped
doors (1+8+3+4) with unknown-`std::function`/unknown-`qmn::lsp` reasons,
and the VIEW half of the log stays byte-comparable to the baseline (the
QFlags noise) — anything else in the log is the 1.7.0 class of silent
drop. The parser also sees `lsp/LspClient.hpp` transitively (the hub
header includes it for the `Reply` typedef) and a forward-declared
`TextPosition` — both merge against the already-parsed view headers;
codegen pins it. PINNED AT THE 2026-11-21 RUN: the codegen file list matched
the four predicted TU names EXACTLY (module init symbols
`init_qmn_glue_LspHub`/`_LspHub_ServerSpec`/`_ProjectSession`); the rejected
log held exactly the 16 doors beyond the usual Qt/`tr`/private-helper noise
(and the bound half never appeared in it); and the signals ride
`PySide::Signal::registerSignals` — the meta-object route, so all SEVEN hub
relays and both session seams cross with NO per-signal generated code.

*Wheel/runtime plumbing:* auditwheel excludes += `libQt6Network.so.6` in
BOTH places (the sh list + pyproject `repair_command` — AGENTS sync rule);
the shim imports `PySide6.QtNetwork` BEFORE `_qmn` (QtWidgets does NOT
load QtNetwork, and the one-Qt-at-runtime rule means the DT_NEEDED
soname must resolve against the preloaded PySide6-bundled copy);
`__all__` += `LspHub`, `ProjectSession` (flat attrs over the
`_ext.qmn.glue.*` namespace shims, the view precedent; `ServerSpec` rides
nested on `LspHub`); `qmn.Monaco` stays UNtouched — the shelf is beyond
parity and the parity file gains NO glue checks. The wheel smoke checks
(move into `_smoke_checks.py`, both smoke scripts ride them): import the
pair, build session+hub, attach, `specNameForLanguage("cpp") == clangd`
(the built-in pin), the SESSION-side `openDocument` TRUE for an
unclaimed language with the hub's honest false BEHIND it — visible as
triggers staying DISARMED (`popupTriggersEnabled` False; pinning the hub
door's own false needs no server to exist — the container has no clangd,
the same zero-spawn discipline as ctest), the uri/registry round-trip doors,
and the maps-assertion EXTENDS to libQt6Network (single copy). The shelf
version becomes REAL: the wheel starts carrying `qmn::glue 1.0.0` (its
own semver now has users), while the hub's `initialize.clientInfo.version`
keeps riding the PACKAGE version — the third-literal contract survives
the wheel. Host builds are unaffected (bindings stay `QMN_BUILD_PYTHON=ON`
container-only; the CI `wheel` job runs the grown `--wheel` pipeline).
BUILT 2026-11-21, all green in one pass: the `--codegen` audit above, the
build-tree smoke, and the full `--wheel` pipeline — auditwheel-repaired
`manylinux_2_34` wheel (the QtNetwork exclusion live in BOTH lists), smoke
+ qtmonaco parity run against the INSTALLED wheel with the repo off
`sys.path`, single QtCore AND single QtNetwork in the maps pins. The JSON
crossing pinned as the dict/primitive-type reality (amended paragraph
above); `Monaco` and the parity file untouched, exactly as decided.

### DAP features (car 4)

Same RpcClient/transports, DAP envelope only. The widget half is ALREADY
DONE (D3: breakpoint glyphs, current-line, `setInlayHints` named for DAP) —
inventory confirms zero view work; verified-breakpoints surface as a
decoration restyle through the existing doors (iide ignored
`body.breakpoints` — we don't). Fake DAP adapter in the same harness style.

**Car 4 (DAP) design pass (DECIDED 2026-11-21, building).** The stub above
survives whole — this pass expands it into machines and records the
decisions the machines force. Four machines, the lsp-binary pattern
throughout: ctest grows the 7th entry `dap` (`tests/tst_dap.cpp`,
offscreen) and GROWS machine by machine; `tests/dapfake/` is a support
binary (NOT an entry) with argv modes + the exit-code oracle + stderr
markers judging its own transcript (the fakeserver style, verbatim). The
base protocol is IDENTICAL (Content-Length framing) — JsonFraming and both
transports ride unchanged.

**D1 — the envelope seam (src/protocol, surgery not API).** Everything in
RpcClient except the WIRE SHAPE is envelope-agnostic: id/seq allocation,
pending map, exactly-once fulfilment, per-door budgets, the cancel-then-
ignore-late rule, the two-phase stop, the gone-latch. The wire shape is
NOT, and RpcClient.cpp hardcodes the LSP one today — the stub's "same
RpcClient" always meant this seam, and iide's self-flagged "Lsp/Dap RPC
duplication" is exactly what D1 kills (one mechanism, two codecs). New
`protocol/EnvelopeCodec.hpp`: a pure-virtual interface on QtCore types
owning — outbound request/response/error builders, notification-vs-event
normalization (DAP `type:"event"` = the notification shape: method := the
`event` name, params := the `body`; the unknown-notification rule carries
across unchanged), the response match key (`id` vs `request_seq`), the
success/error read (`error.code` vs `success:false` + the optional
positive `error.id` table), the cancel shape (`$/cancelRequest`
notification vs the `cancel` REQUEST whose own reply lands on the
unmatched-response ignore path), the mechanical UNREGISTERED-answer
(-32601 vs `success:false` "Unknown request: X" — "always answer, never
lie" rides the codec), and the stop-barrier vocabulary (`shutdown` +
`exit` vs `disconnect {restart:false}` and NO exit message; the kill
escalation stays the Transport's). Id allocation stays RpcClient-owned
(DAP seqs are per-direction and monotonic — the same counter fits;
inbound request ids are adapter seqs, `canonicalId` already handles
ints). `JsonRpcCodec` = the extracted current logic, the ctor-default:
the old ctor keeps working, so the machine tripwire is that the ENTIRE
existing tst_lsp binary lands GREEN with ZERO slot edits (byte-identical
LSP traffic); the new slots (codec round-trips, DAP envelope decode,
malformed still firing per codec, local-vs-wire code banding) grow
`tst_dap` — guiless until a machine needs a widget. Reply.code banding
pinned honest: local `kErr*` are envelope-independent; wire codes are
codec-owned (JSON-RPC negatives vs DAP's positive `error.id`s, 0 when the
adapter omits it) — never conflated (the file header's iide anti-pattern
rule, extended to the second envelope).

**D1 BUILT 2026-11-21**: host ctest 7/7 — the tripwire held exactly as
designed (the pre-existing tst_lsp binary + the fakeserver oracles passed
with ZERO slot edits; only RpcClient.hpp's include chain forced a recompile),
and the new `dap` entry landed as the 7th ctest test with 15 D1 slots green.
Two seam details the build settled (additive, consumer-invisible): an EMPTY
object from `buildNotification`/`buildInvalidIdAnswer`/`buildUnregisteredAnswer`
means "this envelope answers nothing here" and RpcClient writes NOTHING (the
DAP-shaped leniency; JsonRpcCodec never builds an empty one, so LSP bytes
unchanged), and `openIncoming_` became a map id→FULL REQUEST object (DAP
responses echo `command` + the inbound seq, not just the id — JSON-RPC reads
its id back out of the same object, byte-identical). The test-local
`TestDapCodec` in tst_dap.cpp is the blueprint machine D2 promotes into
src/dap as the production `DapCodec`. D2-era correction (decided with the
real handshake, and the reason D1's record said what it said): DAP's
outbound one-way EXISTS — it is the `initialized` EVENT — so
`buildNotification` gained a `qint64 seq` param right at the seam's
landing; RpcClient allocates it from its own counter ONLY when
`responsesCarrySeq()` (JsonRpcCodec ignores it, LSP notify bytes stay
unchanged), and the empty-answer skip rule stands as written — the
test-local codec deliberately keeps its empty one-way as the skip pin.

**D2 — `src/dap/`: `DapCodec` + `DapClient` (new `qmn_dap` static, the
`qmn::dap` export rides `qmnTargets`, the lsp CMake pattern verbatim).**
The layer rule holds literally: `src/dap/` knows ONLY `src/protocol/`.
Handshake: transport up → `initialize` request (args `{adapterID,
clientID, clientName, clientOS, locale, linesStartAt1, columnsStartAt1}`
— the declared 1-based pair IS the spec default, so every
wire↔model conversion lives at the glue boundary, −1 in / +1 out; the
design pass also sketched a `clientCapabilities:{}` member, but the
build settled on NOT sending the member at all (honest-absence style:
we declare no reverse-request capabilities; their relays answer
honestly anyway) — every key the CALLER passes outranks the default)
→ READY means adapter-alive-but-UNconfigured; the `initialized`
EVENT opens the configuration gate and spec ordering is ENFORCED:
configuration commands early are refused locally (id 0 + reason, the
hub featureGate precedent). Then `launch`/`attach` (app-carried args;
10 s budget like initialize — the adapter may build) — the standard
order, the deprecated launch-inside-initialize shape is not spoken. The
initialize response's `body` is stored as the CAPABILITY BAG; a
`capabilities` event re-stores it; capability-gated doors
(`supportsTerminateRequest`, `supportsEvaluateForHovers`,
`supportsConfigurationDoneRequest`) refuse locally with the reason.
Request doors = the raw `request(command, arguments, handler, timeout)`
(the LspClient precedent: the client owns the SESSION, the hub composes
commands); event/answer routes = SIGNALS from day one (the G3 lesson —
meta-object crossings only): `ready()`, `adapterFailed(reason)`,
`dapEvent(name, body)` — ONE generic event signal (stopped/continued/
output/breakpoint/loadedSource/terminated/processExited/capabilities all
ride it; per-event signals would scale binding cost with the spec) —
plus the reverse-request RELAYS: the design pass planned a named pair
(`terminalRequest(id, params)`, `startDebuggingRequest(id, params)`),
but the build settled on ONE generic `dapRequest(request)` signal —
same binding-cost logic as the event signal, and the answerer needs the
request's OWN seq to key its answer, which the full request object
carries (a bare (id, params) pair would strand the pairing key); the
client answers an unanswered relay HONESTLY FAILED (`success:false`,
"not handled: X", error.id 0 — the unregistered-error shape, inventing
no code) the moment the relay returns — the showMessageRequest-wins
order, an app that answers DURING the signal wins (first-answer-wins,
RpcClient refuses the second). A reverse request that arrives UNDECLARED is a wire bug on
the adapter's side but lands on the same lenient path (the tolerant-
inbound precedent). LIFECYCLE DIVERGENCE (decided): debug adapters get NO
auto-restart — an LSP server is a stateless reader whose re-spawn
replays didOpen for free, but respawning an adapter mid-session RE-RUNS
the debuggee and its side effects; death lands FAILED and the only way
back is a fresh startDebugging (a new SESSION, never a resurrection).
dapfake lands with this machine (the machine-2 precedent): C++
`tests/dapfake/` spawned via ProcessTransport by a TestHarness factory;
modes carry the machine tests (lifecycle, breakpoints-verify-move,
steps, reverse-requests, dying).

**D2 BUILT 2026-11-21**: ctest 7/7 (the `dap` entry GREW to 25 slots = 15
D1 + 10 D2, all green; `qmn_dap` STATIC + `qmn::dap` export rode
`qmnTargets` on the lsp CMake pattern, and the packaging smoke re-ran
with the two new headers under `include/qmn/dap/` + `examples/consumer`
configuring and building clean). The layer rule holds literally:
`src/dap/` includes ONLY `src/protocol/`. What the build settled:

- The machine FORCED five new protocol-seam doors (all additive, all
  JSON-RPC-inert — LSP traffic stays byte-identical, the whole pre-
  existing tst_lsp binary again passed with ZERO slot edits):
  `setAnyNotificationHandler` (the event relay) and `setAnyRequestHandler`
  (the reverse-request relay — the handler MUST answer, RpcClient then
  refuses second answers); a static ANSWER SHELF
  `registerMethod`/`registerMethodFailure`/`unregisterMethod` consulted
  BEFORE anyRequest_/the mechanical unregistered answer (dapfake's
  progressStart leg PROVES the shelf outranks the relay: `{}` answered
  without ever reaching `dapRequest`); `setOnCancelRequested` — an
  inbound-cancel HOOK firing from BOTH the Notification and the Request
  dispatch paths (DAP cancels as a request AND as the `$/cancelRequest`
  notification; the Request path registers `openIncoming_` first so the
  hook can answer with its own seq); `cancelPending(id, code, message)`
  fulfilling a LOCAL pending request with NO wire write; and the codec
  gained `cancelCommand()` (JsonRpcCodec keeps `$/cancelRequest`,
  DapCodec says `cancel`) + `cancelTargetId(message)` (absent target →
  0 → honest "cannot be cancelled" answer). One new LOCAL failure code
  `RpcClient::kErrCancelledByPeer = -33104` — deliberately NOT the LSP
  wire's -32802 and never a fabricated wire error.
- `DapCodec` = the promoted TestDapCodec blueprint plus the spec-shape
  upgrades: UNDEFINED args/body members are OMITTED (not sent as null —
  the shape real adapters emit), an error `data` that IS AN OBJECT
  MERGES into the error table (DAP has no nested `data` member; a
  non-object data has no legal spelling and is dropped), cancel is a
  REQUEST carrying `arguments.requestId`, `shutdownParams()` =
  `{restart:false}` with NO exit frame (`hasExitNotification()` false —
  the stop barrier skips that leg), and `classify` normalizes `event`→
  Notification and demands the seq (a DAP message without seq is not an
  envelope).
- `DapClient` = the LspClient lifecycle MIRROR (NotStarted/Starting/
  Ready/Stopping/Stopped/Failed, manual `restartServer()` no-op from
  Starting/Ready/Stopping): the pinned handshake initialize (10 s
  `kInitializeTimeoutMs`, NOT the feature default) → success → `initialized`
  EVENT → Ready; capabilities = the initialize RESULT whole object, a
  `capabilities` EVENT RE-STORES (not merges) it while still riding the
  `dapEvent` relay; NO auto-restart is pinned-by-oracle three ways
  (initfail, deaf-timeout, dead-mid-session all land Failed with
  `spawnCount == 1`, and restartServer() then starts a FRESH session —
  ready() fires again, dapfake exits 0 twice on the clean path);
  inbound `cancel` fulfils the local pending EXACTLY ONCE with
  kErrCancelledByPeer, answers `success:true`, and the adapter's LATE
  answer to the cancelled request is swallowed by the pending map;
  before Ready every feature door answers EXACTLY ONCE with local
  kErrNotRunning and the transport factory is NEVER touched.
- Recorded deviation: the design sentence's "configuration commands
  early are refused locally (the hub featureGate precedent)" did NOT
  land in DapClient — the client's only gate is Ready-vs-not; the
  configurationDone ORDER is D3's hub job (explicit `configurationDone()`
  decided for the hub, no auto-fire). The named relay pair's death into
  ONE `dapRequest(request)` signal is marked in the design text above.
- dapfake for D2: modes `basic` / `initfail` / `dead` / `deaf` /
  `events` / `reverse` / `cancelreq`, exit code = the far-side oracle
  (handshake order, the initialized-event presence, client-seq
  monotonicity, every ANSWER SHAPE it validates itself; exit 2 on any
  lie, watchdog exit 3), stderr-vs-stdout channel split pinned from
  both sides. The design's `breakpoints-verify-move` and `steps` modes
  stay DEFERRED to D3 (dapfake grows with the hub machine — the stub's
  list survives whole).

**D3 — `DapHub` ON THE GLUE SHELF (src/glue; additive growth,
`QMN_GLUE_VERSION` STAYS 1.0.0).** Named for the protocol like its
sibling (the shelf = LspHub + ProjectSession + DapHub); `qmn_glue` links
`qmn_dap` too. The G2 registry earns its keep: `attachProject
(ProjectSession*)` resolves every DAP `source` (its `path`, or
sourceReference → path through the hub's own store) against the session's
uri map (exact, then bare-path normalization both sides, the
languageForUri precedent); a frame in a source NO session editor covers →
`sourceUnresolved(source, frame)` RELAY — the hub NEVER reads disk (the
G2 no-disk-I/O rule; the app opens an editor and `openDocument(editor,
uri)` registers it, destroyed() auto-unregisters per the hub precedent).
Breakpoints: the APP owns the truth (a breakpoint UI is app scope); the
door `setBreakpoints(editor, lines)` stores, sends DAP `setBreakpoints`
({source,path}, `[{line: l+1}]`) and re-feeds decorations from
`body.breakpoints` — **verified lines ride
`CodeEditor::setBreakpointLines` (a dot says executable) and unverified
ones become Warning `Diagnostic` decorations** (message from the reply's
`message`/`instruction`, `source = "dap"`) — the stub's decoration-
restyle clause given its shape, and the iide "ignored body.breakpoints"
anti-pattern dies here; the VERIFIED LINE MAY MOVE (desired 5 answers 7)
so the dot lands on the ACTUAL line and the desired→actual map rides the
store. Breakpoints are line-anchored (no anchor layer exists in the
model), so an edit of a bound document RESENDS setBreakpoints with the
syncer-shaped `kBreakpointResendMs = 400` debounce — line-number honesty,
recorded. `breakpoint` events update the verified store and refeed
through the same doors. The stopped feed: `stopped` → v1 SINGLE-THREAD
(threadId = the last stopped one; a threads UI is a recorded deferred
sliver) → `stackTrace` → top frame → `setSelection(frame, frame)` (caret
jump; line−1, column−1 clamped — recorded limitation: DAP columns are
adapter-unit-dependent, we declare+assume UTF-16 units, ASCII-line
tripwire pins hold the boundary) + a `setHighlightedLines(line,line)`
band, cleared on `continued`. Value hints: `variables` of the first top-
frame scope (top level only, `kMaxValueHints = 20` cap), matched as WORDS
on the stopped line, `= value` painted AT (line, word-end col) via
`setInlayHints` — the M5 door finally carries the DAP meaning its name
was minted with. No text sync at all for DAP: adapters see line numbers,
never bytes. `configurationDone()` is an EXPLICIT hub door (the app says
setup ended; gated + skipped per capability). `disconnect()` rides the
codec-shaped stop barrier; `terminate()` is capability-gated; adapter
death clears every feed and relays. The hub dtor pumps the bounded
kStopBarrierMs barrier (the M8 machine-4 lesson, paid twice). Config: NO
built-in adapter matrix, NO `.qmn/dap.json` (decided: an adapter is not a
language server — one session at a time, program/args/launchArgs carried
by `startDebugging(...)`; a dap config shelf is scope creep until a real
consumer shows up). The `setTransportFactory` seam mirrors LspHub's
(C++-only, honest absence).

**D3 BUILT 2026-11-21**: ctest 7/7 (the `dap` entry GREW to 31 slots =
25 D1/D2 + 6 hub slots; the binary FLIPPED `QTEST_GUILESS_MAIN` →
`QTEST_MAIN`, exactly as the D1 header-comment prediction and tst_lsp's
machine-4 precedent said, and `tst_dap` now links `qmn_glue`). What the
build settled:

- The shelf's THIRD member is `src/glue/DapHub.{hpp,cpp}`;
  `qmn_glue` PUBLIC-links `qmn_dap` (the header's `client()` door names
  `qmn::dap::DapClient*`; that door stays C++-only, and the whole hub is
  shiboken-shaped per the D4 record, `QMN_GLUE_VERSION` stayed 1.0.0).
- Two PLAN names RENAMED at the door: `disconnect()` →
  **`stopDebugging()`** (`disconnect` would shadow the QObject statics on
  a QObject subclass — a hazard for every consumer, not a cosmetic
  choice), and `terminate()` → **`terminateDebugging()`**; the design's
  "gated + skipped per capability" became: the door answers the honest
  FALSE when `supportsTerminateRequest` is absent/false AND still rides
  the plain barrier (a terminate that silently did nothing would be the
  worse lie); `configurationDone()` skips ONLY when
  `supportsConfigurationDoneRequest` is EXPLICITLY false (absence =
  send, the DAP spec's own default).
- The caret leg uses **`CodeEditor::setPosition(TextPosition)`** — the
  design said `setSelection(frame, frame)`, but `setSelection` is a
  PRIVATE editor member (recon error); `setPosition` is the public caret
  door and it keeps the ZERO-new-view-doors rule literal. The band,
  dots, diags and hints ride the M5 doors unchanged.
- One machine-caught bug worth its scar: the DAP `StackFrame` identity
  field is **`id`**, not `frameId` (the REQUEST argument is the one
  spelled frameId) — reading "frameId" off the frame would have silently
  killed the value-hint leg.
- `startDebugging(program, args, launchArgs, adapterId="dap")`: ONE
  session at a time (any live state → honest false); launchArgs
  auto-fills `request:"launch"` + `noDebug:false` only if absent; the
  ready() leg sends `launch` FIRST, then re-pushes stored breakpoints,
  then fires `sessionStarted` — so store-BEFORE-start is a legal order.
  `adapterCapabilities()` is a PROXY to the client (no mirrored bag, no
  re-store drift); `terminated` clears every feed + relays
  `sessionEnded("terminated")` but leaves the CLIENT Ready (v1 split:
  the app decides stop-vs-relaunch); adapter death merges into
  `sessionEnded` and `clearAllFeeds` EMPTIES the feed — it does NOT
  repaint dots as warnings (a dead session gets to say nothing; the
  DESIRED breakpoint truth survives for the next session's re-push).
  Two generation guards: `sessionGen_` (every in-flight reply drops on
  start/adapter-gone) and `stopGen_` (stackTrace→scopes→variables chains
  drop on stopped/continued/death — the chains are three legs deep and
  the old tests never had chains).
- `dapfake` GREW the two planned modes: **`breakpoints`** (explicit
  `supportsConfigurationDoneRequest:false` skip-pin, the launch
  spec-fill audit, the uri→localPath `source.path` equality pin, the
  move pin 9→11, the unverified-with-message pin, the id-keyed
  `breakpoint` EVENT, the pass-2 resend oracle + a disconnect that
  exits 2 if the edit-resend never arrived) and **`steps`** (terminate
  caps pair, the `configurationDone`→`stopped`(threadId 7) leg, the
  stack chain audits, the SECOND-scope POISON reference that exits 2
  far-side so a wrong-scope feed can never print tst-side, the first-
  wins duplicate pin, the BOGUS-source `sourceUnresolved` pin, and a
  disconnect that exits 2 unless `terminate` led). One generic `launch`
  arm was added for the OLD modes: the hub ALWAYS fires launch at Ready,
  so basic/dead/events/reverse/cancelreq now answer it ok (the strict
  audits live only in the two D3 modes).
- Tests ride REAL offscreen editors with VIRTUAL `/dapfake/...` paths
  registered through `ProjectSession::openDocument` — ZERO disk touched
  (the tst_glue precedent), and the near-side spawn/exit oracle moved
  onto the hub's `setTransportFactory` seam (`HubSpawns`, declared
  BEFORE the hub: the dtor pump may still fire a closed() lambda). One
  honest note: dapfake's first-call `breakpoint` event lands in the SAME
  frame batch as the setBreakpoints reply, so the slot pins the MERGED
  post-event truth (dots `{10}`, two Warnings, map `{8→10}`) instead of
  a never-settling intermediate.

**D4 — the demo door + the close ritual.** qmn-demo grows `--dap
<adapter-program> [args…]` — GENERIC by design (this host ships
`lldb-dap` AND gdb 18.1 `-i=dap`, so a real adapter is one flag away);
MainWindow forward-declares `qmn::glue::DapHub` behind a unique_ptr so
tst_demo never sees a `qmn::dap` type (the LspHub shape verbatim). Unlike
the clangd door, dapfake IS CI-built, so the door becomes a REAL
tripwire: one tst_demo slot drives the --dap path against
`$<TARGET_FILE:dapfake>` offscreen — the honest difference from the
`--lsp` door's no-spawn rule, and its reason, recorded. Close-car ritual
(the clangd-probe precedent: manual, never CI): `tools/probe_dap.sh`
compiles a small C++ program and drives the REAL adapter through the hub
(script tries lldb-dap first, gdb -i=dap second): one real breakpoint at
a known line, continue, stopped, a variable value back — result recorded
in the CHANGELOG close entry. The wheel: DAP does NOT cross in car 4 —
deferred G3-style until the shelf's C++ surface settles; but every D3
door is designed SHIBOKEN-SHAPED from day one (signals for answer
routes, QJsonObject bodies = plain dicts per the pinned G3 reality, the
factory seam honestly absent), so the follow-on is a three-copies growth,
not a surgery.

Frozen-surface check: ZERO new view doors — the stub's inventory survives
the design read (`setBreakpointLines`/`setDiagnostics`/`setInlayHints`/
`setHighlightedLines`/`setSelection`/`applyTextEdits` carry every feed);
the protocol surgery is ADDITIVE for consumers (ctor overload + interface
header; the default keeps every LSP byte identical); the layer clauses
hold (dap = protocol-only, never view; the DAP glue sits ON the shelf
where DAP was always told to land); `examples/consumer` grows the
DapHub doors TU (doors compile, NOTHING spawns). Gate 2 closes after this
car — the version close ritual (BOTH package literals at once) is the
close's decision, not this pass's.

**D4 BUILT (2026-11-21) + the two live-adapter amendments it forced.**
The door SHIPPED as designed with one CLI DEVIATION: `--dap
<adapter-program>` + repeatable `--dap-arg <arg>` + `--dap-program
<program-to-debug>` — the designed `--dap <adapter> [args…]` greedy tail
cannot tell adapter args from the launch target, the repeatable flags
keep both honest (recorded deviation; `MainWindow::setDapAdapter(program,
args, launchProgram)` is the programmatic door, OFF by default, hub
auto-starts at the first armed tab, ONE attempt per armed hub). The demo
stores model bps {2,6} BEFORE `startDebugging` (D3's store-before-ready
legality, walked) and every shelf answer route is wired to the STATUS
BAR (Russian strings) — sessionStarted / launchRejected / sessionEnded /
debugStopped / debugContinued / sourceUnresolved. The shelf GREW one
additive door: `launchRejected(message)` relays a Ready-leg launch
refusal (error answer OR the 5 s command timeout) and KEEPS the session
(terminated's recorded split again). tst_demo GREW three slots over the
REAL door — the tripwire spawns `$<TARGET_FILE:dapfake>` (mode
`launchreject` for the refusal slot): the recorded honest EXCEPTION to
the `--lsp` no-spawn rule, dapfake is CI-built (offscreen exit codes
prove nothing — the pgrep sample was the spawn proof during development).

The close-ritual probe (`tools/probe_dap.cpp` + `tools/probe_dap.sh`,
ad-hoc g++ like bench_model, `-no-pie` per the non-PIC archives rule,
never a CI gate) caught REAL spec bugs the fake had blessed — dapfake
had mirrored the WRONG shapes and the tests went tautological:

- Amendment to D2 (the HANDSHAKE DIRECTION): the client-sent
  `initialized` EVENT D2 shipped is OUT of DAP spec — `initialized` is
  the ADAPTER'S event (unlike LSP, where the client-sent one is correct:
  `src/lsp/` untouched). gdb 18.1's python DAP server CRASHES on a
  client-side event frame (`KeyError: 'command'` → "DAP: terminating")
  and no real adapter emits `initialized` early enough to WAIT for
  either (lldb writes it mid-`launch`) — so Ready rides the initialize
  RESPONSE ALONE, the client sends NO event frame (the `event()` door
  stays pinned open but v1 sends nothing through it), and the adapter's
  `initialized` simply rides the generic dapEvent relay. dapfake now
  writes its OWN `initialized` behind every answered initialize and
  EVERY mode exits 2 on a client-side event frame — the crash shape
  pinned as a tripwire so the wrong direction can never creep back;
  the FakePeer handshake slot renumbered (the handshake consumes NO
  client frame any more) and the events-relay slot pins `initialized`
  as the first relayed event.
- Amendment to D3 (the configurationDone ORDER, second live-gdb lesson):
  gating the word on EVERY setup answer DEADLOCKS — the daplog shows
  gdb answers `launch` only AFTER `configurationDone` (its python
  `launch` starts the inferior which waits for the word). The band now
  counts the ANSWERED `setBreakpoints` pushes ONLY (launch deliberately
  OUT), one-shot deferral `cfgDoneWaiting_` settles when the band
  drains. Near-side-only visibility (dapfake's reads cannot see WHICH
  of its answers the hub has processed), so the new FakePeer pin
  `dapConfigurationDoneIsDeferredBehindTheSetupAnswers` scripts the
  answers directly.
- Live wire facts pinned (both adapters green end-to-end, 7 steps each:
  handshake Ready, verified dot at model line 4, stopped shape,
  caret-vs-dot agreement (waitUntil: the stackTrace chain is ASYNC),
  stackTrace→scopes→variables read-back of `answer=41` through REAL
  lldb and REAL gdb, continue reply, terminated→sessionEnded + feeds
  cleared): gdb pre-launch bps answer `verified:false reason:"pending"`
  and UPGRADE the dots via `breakpoint` reason:"changed" verified:true
  EVENTS — D3's id-keyed event machinery already carried it; lldb
  verifies IN the response when bps follow `launch` (bps-before-launch
  proved unreliable: one lldb run never hit the bp); lldb's own
  `initialized` + `capabilities` events arrive late, mid-launch.
`QMN_GLUE_VERSION` stays 1.0.0 (every D4 shelf change is additive); the
PACKAGE close ritual bumps BOTH literals in one commit — car 4 CLOSED.

**Gate 2 CLOSED 2026-11-21** — all four ordered cars (M25 wrap, M10 diff,
M8 LSP, car 4 DAP) plus the glue G-train (G1+G2+G3) shipped; the train
ships as tags `v1.9.0` (the car-4 close) and `v1.9.1` (the hotfix close —
the 1.6.0→1.6.1 precedent, see CHANGELOG). The deferred shelf became
Gate 3's input — the next train was designed the very same day (see
"# GATE 3" below): M8b (signatureHelp, LSP inlay hints,
foldingRange over M13, pull diagnostics), M10b (word-level inline diff,
wrap×diff budgets, original-side wrap), the two verbatim-recorded defects
(the undo-mark drift on type-over-selection, the overview-ruler perf
cliff), DAP-at-the-wheel, a multi-observer core for two editors on one
buffer, and the missing PyPI/Qt-bootstrap half of the cibuildwheel flow.

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

# GATE 3 — debt paydown and the live showcase (opened 2026-11-21)

Everything in this gate was PRE-BOOKED: each debt item carries its own
verbatim record in the sections above (the two recorded defects, the M8b
and M10b follow-on lists, the CodeDocument multi-observer clause, the G3
rejected-log audit, the staged cibuildwheel config), plus TWO showcase
cars the owner asked for (2026-11-21): a live-verification demo in C++
and its Python analog — the demo layer is where the whole Gate-2 surface
was never checked end-to-end (qmn-demo carries no diff UI, tst_demo has
no diff slots, pydemo lost the mirror the moment `--lsp` landed).

**Train ordering (DECIDED 2026-11-21):** defect pair → M10b → M8b →
multi-observer → wheel-2 → S1 → S2 → PyPI. The argument: the recorded
defects have been rotting in shipped releases since 1.6.1 (they go
first, patch-fast); the showcases SILENTLY pin everything before them,
and S2 (Python) cannot mirror the DAP doors before the wheel car ships.
The frozen-surface rule holds through the whole gate: everything lands
ADDITIVELY (Gate 1.5 precedent); a car that would break a frozen door
stops at the door, like the `visible*` UNITS precedent had a sanctioned
record — nothing here asks for that again.

**Version rhythm (DECIDED):** every car CLOSE bumps the package version
(the Gate-2 precedent: M25→1.7.0, M10→1.8.0-as-part, DAP→1.9.0). The
defect pair is a PATCH (1.9.2 — user-visible bugs in shipped releases),
every feature car ships a MINOR, and the gate closes on the last car's
tag with the usual full ritual (both literals + the wheel half + a green
pydemo — the 1.9.1 addition).

**The two showcase knots (DECIDED 2026-11-21):**
- S1 GROWS `qmn-demo` in place — NO second showcase binary. The
  `qmn_demo_app` static lib + `tst_demo` keep driving it offscreen, and
  the pydemo mirror rule (door-for-door CLI + menus) survives for S2.
- TOUR MODE: both demos grow a `--tour` flag — a scripted door-for-door
  self-check that prints a probe-style pin table and EXITS NONZERO on
  the first failed pin (skips print as skips and keep exit 0). Server-
  backed legs (clangd for the LSP legs, lldb-dap/gdb for the DAP legs,
  dapfake as the CI-shape stand-in) SKIP honestly when the server is
  absent — a tour is a LIVE ritual like `probe_dap.sh` and
  `probe_clangd.sh`, NEVER a CI gate (clangd and debuggers are not CI
  guarantees, the recorded rule).

### Car 0 — the recorded defect pair (target 1.9.2, patch)

Two verbatim records above, one car, both fixes independently testable:
- **The undo-mark drift (recorded 2026-11-15).** Bracketed
  type-over-selection in `insertTextAtCaret` pushes TWO undo groups
  (serial +2, depth +2) for ONE `reconcileUndoMarks` call, so
  `undoMarks_` lags the core's history; the release-build symptom is an
  undo that restores a stale caret. Tripwire: reproduce THROUGH the
  accessible `replaceText` door (the discovery route), pin serial +1 and
  depth +1 and the caret-after-undo.
- **The overview-ruler perf cliff (recorded 2026-11-16).** The stripe
  loop's per-sample `offsetOfLine` scans within the holding piece —
  O(doc) on a ONE-PIECE loaded document (~230 ms per paint measured at
  12k lines/6 MiB). The fix belongs to the CORE's line index (per-piece
  newline position lists or a line cache), NOT to the ruler or the wrap
  layer, and the record says its own car with a bench tripwire — this
  is that car. DoD: ruler paint on a big single-piece doc measured by a
  timing slot in the VACUOUS-PASS style of the two existing
  timing-sensitive tests (failure mode never a false fail), plus the
  textbuffer property suite unchanged.

**Car 0 CLOSED (2026-11-22, 1.9.2).** Both records dead, with dated
corrections: the undo drift did NOT ride the view typeover (a core trace
showed the bracketed typeover pushes exactly ONE group — the record's
"two groups" theory is superseded); the drift is one mark per accessible
`deleteText` call (`replaceText` rides it), fixed IN the a11y door with
`deleteLineAt`'s capture/ride/reconcile pattern, `deleteSelection` staying
a bare helper with the contract recorded in the header; tripwire
`accessibleEditDoorsKeepTheUndoMirrorAligned` (codeeditor 258→259, red on
the old door: caret `{1,0}` vs `{0,3}`). The ruler cliff died in the core
as the record demanded: each treap node carries `nl`, an immutable
buffer-absolute newline-offset list + window `[nlLo, nlHi)`, LAZILY
materialized once per piece at the first line read that lands on it
(the eager line-COUNT scan of v1 stays — A/B found materializing at load
or split regressed setText/first-keystroke instead); splits of a BUILT
piece re-cut windows with one `lower_bound` (also erasing the old
per-keystroke O(half-document) line-count re-scan on loaded docs);
`offsetOfNthNewline` indexes directly. Bench tripwire
`rulerLineReadsStayIndexedOnOnePieceDoc` (vacuous-pass style, best-of-3
600 probes < 30 ms): old core measured 601 ms against that budget, the
new ~9 ms; `newlineWindowsSurvivePieceSplits` pins the window algebra;
`checkSubtree` now verifies every window against the piece text through
the property suite. A/B perf gates held: setText 7.7→8–10 ms, 20k random
ops 12.5→11.7 µs/op mean — and the A/B recorded three traps the shipping
shape had to dodge (QVector::append list storage ~50 ms/build, memchr-per-
hit loops ~12 ms/pass at the bench doc's density, materialize-at-split
first-keystroke spikes); the built scan is one SSE2 cmpeq_epi16 pass +
std::vector, scalar only off-x86. The wheel half of the close ritual ran
(the 1.9.1 lesson): container rebuild/repair/smoke/parity green, pydemo's
fourth literal re-pointed, headless pydemo green.

### Car B — M10b (diff follow-on)

Word-level inline highlight inside hunks, the wrap×diff budget math, and
original-side wrap (the three leftovers named in the README M10 entry
and the M10 close record). The pure engine grows in `src/diff/`
(QtCore-only), the view doors stay additive, `renderSideBySide:false`
stays out of v1 by the recorded divergence. The `open_diffs` shim and
the parity script already pin the semantics this car must not break.

**Machine design (written at car start 2026-11-22, the M8/D-car pattern):**

- **B1 — the word-diff engine (`src/diff/WordDiff.hpp/.cpp`, QtCore-only).**
  A line PAIR goes in, per-side UTF-16 code-unit span lists come out
  (deleted-token spans in ORIGINAL coordinates, inserted-token spans in
  MODIFIED coordinates), plus `changedTokens` (the token edit distance,
  the test oracle) and `budgetExhausted`. Token grammar: maximal
  `[letter/digit/_]` runs, maximal whitespace runs, every other
  CODE POINT its own token (surrogate pairs ride atomically — the view's
  boundary discipline, not the core's, so the diff layer keeps spans
  never-splitting-pairs). Miller trim on the token sequences, then the
  SAME Myers D-loop as the line engine: the kernel moves out of
  LineDiff.cpp's anonymous namespace into an INTERNAL shared seam
  (`diff/MyersCore.hpp`, never a public door, never installed —
  the LineDiff tripwire suite pins the extraction before/after).
  Spans group per change BLOCK (a block has no Equal tokens by
  construction, so each side's span is exactly first-token-start..
  last-token-end), then one ABSORB pass merges spans on one side whose
  gap is whitespace-only (a word move reads as one wash, not three —
  presentation only, `changedTokens` stays the honest token count).
  Budget: per-pair auto cap 256 edits (the (D+1)² trace guard, ~1 MiB
  transient per pair); exhaustion = whole-line spans both sides, VALID
  just coarse — the line engine's fallback discipline verbatim.
- **B2 — the inline wash (view paint, NO new public door).** After every
  `recomputeDiff`, each REPLACE-hunk row pair (rowKind 0, both cells
  live) gets a `WordDiff::compute`; the spans live in `DiffState`,
  invalidated with the engine frame (a fold re-key keeps them). Two NEW
  additive Theme Roles (`DiffAddedWord`, `DiffDeletedWord` — the
  decoration-role additive widening is the M5/M10 precedent, Shiboken
  re-runs on the already-bound Theme class). The modified column paints
  the inserted wash INSIDE the row's band, the original column the
  deleted wash; pure insert/delete rows keep the v1 band-only look (the
  band already covers the whole row there — inline detail would paint
  the same pixels twice). Latency guard: the inline pass is capped
  (pairs + elapsed budget, exhaustion = rows stay bare-band, coarse but
  valid — the engine budget story at the view scale). Pixel tripwires
  ride the established `grab()` + color-predicate discipline.
  **B2 BUILD NOTE (landed green 2026-11-22):** the design's "wash INSIDE
  the row's band" was imprecise and the paint settled the truth — the v1
  MODIFIED band rect rides the NUMBER CELL only (`[stripRight,
  contentLeft)`); the text zone of a replace row carries NO band (its
  only v1 change story is the gutter mark). So the inserted-word wash
  IS the text zone's whole inline story (VS Code-style), painted by
  `runDiffInlinePass` (sync tail of `recomputeDiff`, pair order = row
  order, caps 400 pairs + 150 ms — the cap break leaves the LOWER rows
  bare, the visible top wins first) and painted in `drawDiffChromeUnder`
  right after the band/ground fills: modified side via `xForCol` clamped
  into the modified window, original side via `origXAdvance` clamped
  into the read window. Layering (deliberate asymmetry, v1's): the
  modified selection quads run UNDER the chrome pass, so a spanned word
  covers the selection exactly like the band covers it on its cell; the
  original wash rides the opaque Base ground UNDER the orig selection
  wash, so the read side stays selectable-readable. Engine-exhausted
  and empty-span pairs get NO cache entry (whole-line fallback spans
  would repaint the row's own ground — bare reads better at view size).
  Tripwire mechanics pinned here for reuse: the default-theme glyph
  inks are near-gray, so every AA blend pixel off a ground KEEPS the
  ground's channel deltas — forced probe inks are chosen OFF that delta
  line (the v1 test palette (4,5,6)/(16,17,18) family sits ON the Base
  ground's delta-2 line and would collide with low-coverage glyph AA
  pixels in whole-frame COUNT scans; point probes dodge it, counts must
  not). And the caret row's current-line fallback wash composites to an
  exact documented pixel (`#1F2021`, pinned by `diffChromePaintPixels`)
  — ground asserts never ride the caret row.
- **B3 — original-side wrap + the wrap×diff budget math (the deepest
  surgery).** Soft wrap becomes LIVE inside diff mode on BOTH columns.
  The row axis goes from weight-1 rows to WEIGHTED rows: weight(r) =
  max(slots of the ORIGINAL line at the window width, slots of the
  MODIFIED line at the h-window width), blank cells contribute 1, and a
  fold-hidden MODIFIED line still zeroes the whole row (the decided
  honest-v1 twin rule survives). Unmeasured lines keep the M25
  provisional-1 discipline — the totals stay total and monotone, the
  frontier walker measures rows on the ONE shared axis, one
  `kWrapBudgetMs` per pass covers both sides' measurements. The
  ORIGINAL document gets its OWN wrap-plan cache in `DiffState`
  (a different width than the editor's own `wrap_` tables; it
  full-resyncs on original-doc edits — the debounce already recomputes
  the frame anyway, honest v1). `rowPrefix`/`slotToRow`/`diffRowForSlot`
  go weighted (a weighted row emits its row number once PER SLOT —
  materialized inverse, same O(slots) memory as the wrap world);
  paint/hit/hover/drag on BOTH columns resolve (slot − rowPrefix[row])
  as the fragment index; the ORIGINAL hit chain drops the NoWrap
  `OrigLayout` + window-col-clamp for the wrap plan (the recorded
  "triple-click is the way to take a long line" divergence NARROWS —
  README/AGENTS wording follows at close). Caret anchoring across a
  recompute already speaks screen-ROWS via `rowPrefix` — weights just
  make it true. No new public view door anywhere in the car.
  **B3 BUILD NOTE (landed green 2026-11-22):** the design held, and the
  surgery settled these facts on the way in — record them once:
  (1) `weight(row) = foldHidden(mod) ? 0 : (wrap OFF ? 1 :
  max(1, modSlots, origSlots))`, every unmeasured line provisional-1 —
  the axis stays honest-monomotone exactly like M25's world. The engine's
  fits test is ABSOLUTE (`xAt(col) ≤ rowOrigin + width`): a continuation
  row gets the FULL window from its own origin (the test-side simulation
  first got this wrong dimension-wise — the engine's measured totals were
  right and pinned it).
  (2) `wrapSlotCount` must NEVER branch on diff (the generic walkers and
  retune read line-space); the paint-side doors read `lineSlotCount`
  (diff-aware via modLineToRow → rowWeightOfRow; 0 for hidden/unmapped).
  (3) An over-tail slot (the ORIGINAL line wraps deeper than the row's
  MODIFIED cell) is an INVALID RowPlan (`line = -1`): "no mod ink here";
  a click there degrades to col 0 of the nearest mapped modified line,
  never to phantom cols, and the caret paints zero there.
  (4) The original plans validate per-plan only — the epoch/widthKey/
  textLen trio, no table-wide stamps; an external originalDoc edit
  inherits v1's stale-alignment edge honestly (the read side is a canvas,
  not an editing surface — its watcher keys on the READ-WINDOW px: a
  width change wipes `origWrap` and re-keys the axis from inside
  `prepareDiffWrapWindow`'s head).
  (5) `prepareDiffWrapWindow` = eager both sides of the visible rows +
  two frontier walkers under ONE `kWrapBudgetMs` deadline + one
  weighted `rebuildDiffRowAxis`; `setSoftWrapEnabled` re-arms both
  row-cursors and re-keys the axis on BOTH toggle paths.
  (6) The chrome paints FRAGMENT-CUT: both washes cut at the break
  (the uncut `xForCol`/`origXAdvance` anchor would bleed the word wash
  onto sibling slots — tripwire-pinned), the orig selection cut keeps
  the phantom-newline cell on the LAST fragment only (forVisiblePieces'
  rule mirrored read-side), glyph clip closes at the fragment end, the
  layout anchor shifts by −fragOff, the row NUMBER rides slot 0 only,
  and the selection's hairline frame spans the whole ROW (top at
  slot == rowPrefix[row], bottom at slot == rowPrefix[row+1] − 1 — no
  interior stripe ever cuts a wrapped line).
  (7) `diffOriginalHit`: a MEASURED fragment resolves through
  `origColAtX(xLocal + origXAdvance(fStart))` clamped into
  [fStart, fEnd] — the band right of a fragment's last glyph clamps to
  the FRAGMENT end, not the model line's end; unmeasured rows and the
  over-tail keep v1's window-clamp verbatim.
  The five tripwires (`diffWrapKnobInsideDiffWeightsTheRowAxis`,
  `diffWrapOriginalFragmentClicksSelectTheirFragments`,
  `diffWrapModifiedFragmentClicksAndOverTailSlots`,
  `diffWrapInlineWashCutAtTheFragmentBreak`,
  `diffWrapOrigSelectionFrameSpansTheRowNotTheSlot`) build their
  EXPECTED cuts by simulating the engine's break discipline
  (`lineFragCut` in the test file) — never by reading the engine's own
  tables back; every wrap-OFF twin above stayed green byte-for-byte.

**CAR B CLOSED 2026-11-22 at 1.10.0.** All three machines green:
tst_diff 25 slots, three B2 + five B3 codeeditor tripwires, full ctest
7/7. The frozen surface stayed honored — the ONLY public addition in the
whole car is the pair of additive Theme Roles (they cross the wheel as
plain enum values of the already-bound Theme class; the three-copies
were untouched and the `--codegen` rejected-log audit at the close held
the G3 baseline EXACTLY: the 16 glue drops by their pinned reasons,
every other entry pre-existing noise, no new drop). The wheel half of
the ritual ran whole: `qmn_native-1.10.0` rebuilt + auditwheel-repaired,
smoke + qtmonaco parity green against the repaired wheel, pydemo's
path-dep bumped and its headless run exits 0. The README M10 snapshot
line stays verbatim (dated records are snapshots); the divergence
narrowing is recorded here, in the README Gate 3 checklist and in the
CHANGELOG. Car C (M8b) is the open car next.

### Car C — M8b (LSP follow-on)

signatureHelp, LSP inlay hints (the M5 `setInlayHints` door finally
carries its LSP meaning next to DAP's), `textDocument/foldingRange` over
the M13 fold API, and pull diagnostics (the registry already routes the
registration). PRE-DECIDED KNOT: ONE fold provider per editor at a time
— a server that wins the language claim REPLACES the grammar folds
(mixing two fold feeds in one toggle index is the mismatch bug the iide
anti-pattern list warns about); the fold store stays M13's, no new view
doors — the six popup doors carry signatureHelp as they carried hover.

#### Car C design record (written at the car start, 2026-11-22)

FIVE machines, each green before the next opens. Version rhythm: the
car closes at **1.11.0** (additive minor).

Code facts the design rides (read 2026-11-22, all verified in code):
`FoldRange` + `InlayHint` are ALREADY bound types (typesystem
value-entries + `_generated` TUs since 1.6.x) — a plain-Qt-typed door
carrying either crosses the wheel with ZERO three-copies edit (M8 m7
precedent); `LspClient::isRegistered(method)` is the generic registry
(the claim "the registry already routes the registration" VERIFIED —
`textDocument/diagnostic` counts the day it registers); the push decode
lives in `LspHub::onDiagnostics` and refactors into ONE decoder for
both wire shapes; the popup-trigger machinery (`editApplied`/`cursorMoved`
taps, debounce timers, `$/cancelRequest` supersede rule) is the route
signatureHelp and the refresh passes ride.

C1 THE VIEW FOLD-PROVIDER SEAM (view only, zero wire) — ✔ CLOSED
green 2026-11-22: five tripwires in tst_codeeditor, the ctest 7/7
whole. The seam works EXACTLY as written below; C3 feeds it. The honest door
reading of the pre-decided knot: the M8 car's sentence "no new view
doors" cannot hold for foldingRange — M13's `foldLine` folds only lines
its OWN sources made foldable (grammar `@fold` / indent fallback), and
the fold store has no injection seam. The knot is about the STORE, not
about zero doors: the fix grows the fold SOURCE, never a second store.
The surface widens additively — the exact class m6's six popup doors
were: `void setFoldRanges(const QVector<FoldRange>&)` +
`void clearFoldRanges()`. Semantics:
- Installing (ANY vector, empty included) switches the fold source to
  SERVER mode: grammar captures AND the indent fallback go SILENT while
  installed — the one-provider knot ENFORDED on the view side, the
  mismatch bug cannot exist by construction. `clearFoldRanges()`
  restores the grammar/indent source byte-for-byte.
- Server ranges are VERBATIM authority: the view never shifts them
  across edits (the SERVER is authoritative; C3 re-requests after the
  debounced edit — until the re-feed, a moved block keeps stale
  foldables, the same honest staleness the diff alignment carries
  between recomputes). Collapsed folds THEMSELVES keep riding M13's
  edit sweep (shift/die/undo-consistent, view state as always).
- The source-exists auto-expand check SKIPS in server mode (the view
  owns no source to re-check); a server fold lives until the user
  unfolds it, the range dies in an edit sweep, or the next re-feed
  drops it.
- Every M13/hidden-run/slot-space machinery untouched: toggle doors,
  fold zone triangles, caret nav, fold×wrap, the diff row-axis zeroing.
  `foldableRanges()` reports the fed ranges (clamped, ends EXACT — no
  lazy resolution in server mode) — that is the test oracle;
  `isFoldable`/`isFolded` semantics unchanged.
- A document replace (`setPlainText`-class) CLEARS the override exactly
  where M13 clears fold state (ranges speak line numbers; stale by
  definition after a replace); the hub re-feeds after the next answer.
- SOURCE SWITCH CLEARS COLLAPSED STATE, both directions (owner Q&A
  2026-11-22): collapsed folds BELONG to their source — a grammar fold
  collapsed before the server feed is un-recognized in the server set
  (a hidden run with an alien triangle), and a server fold collapsed
  then grammar-restored is the mirror lie. So installing OR clearing
  the override unfolds everything first (the M13 store/machinery stays
  whole; only its STATE resets — same event class as the doc-replace
  clear). Mid-session the grammar stays the serving source UNTIL the
  first foldingRange ANSWER arrives: the switch is atomic at the feed,
  never preempted — until then the user folds grammar folds honestly.
- `FoldRange` already bound → BOTH doors cross the wheel byte-clean;
  the `--codegen` check pins the module init gains them and the
  rejected log gains NOTHING for this machine.
Tripwires (tst_codeeditor): fed ranges drive foldLine/hidden runs/
visibleLineTotal; the GRAMMAR triangles vanish while a server fold is
installed (the knot pinned from the far side — a js doc fed ranges
stops folding its own braces); `clearFoldRanges` restores grammar
folds byte-for-byte; nested fed ranges fold/unfold like nested grammar
folds; a doc replace clears the override; an edit does NOT shift the
fed range (verbatim stance) while a collapsed fold inside it still
shifts/dies on the sweep; and the SOURCE-SWITCH RESET — a grammar fold
collapsed at feed time, and a server fold collapsed at clear time,
both come UNFOLDED (no alien hidden runs survive a provider switch).

C2 SIGNATUREHELP (hub + fakeserver + popup; zero new view types) — ✔
CLOSED green 2026-11-22 (all six slots, three repeat runs clean, the
lsp binary + the full ctest 7/7).
Initialize capability: `signatureHelpProvider{triggerCharacters
["(", ","]}` (static; the registry covers a dynamic counterpart for
free). The hub grows `requestSignatureHelp(editor, position,
FeatureHandler)` — the SEVENTH FeatureHandler door, so the wheel's
rejected log gains EXACTLY this one entry (unknown-`std::function`
reason, the pinned class): the Car C audit RE-PINS the baseline to
17 + new private-section noise, per risk 2 "RE-PINNED, not eyeballed".
Composition rides the SIX popup doors verbatim (the pre-decided knot):
the hub picks `signatures[activeSignature]` (clamped to range), the
plainText is the signature's `label` VERBATIM, the popup is the HOVER
popup anchored at the caret (zero-width `HoverRange`). The ACTIVE
parameter cannot be underlined in a plain-text popup — `activeParameter`
is honored ONLY in picking which signature shows; recorded as the
plain-label divergence (the m6 "no markdown renderer" bullet extended,
never a snippet-engine re-open). Trigger via `setPopupTriggers`: at
fire time the char before the caret is classified — in the server's
advertised `signatureHelpProvider.triggerCharacters` (default `"(" `
and `","`) → fire after `kSignatureTriggerMs`; typing `)` or any other
char CLEARS a visible signature popup; superseded in-flight asks ride
`$/cancelRequest` (the completion rule verbatim). Hover collision:
hover asks already skip while a popup is owned; typing arms signature,
non-typing caret moves arm hover — the two routes are exclusive by
construction, pinned by a no-clobber slot. Fakeserver: `sighelp` rides
as a FLAG of the `sync` MODE (decided at implementation, not a new
mode): the doc mirror, the `test/*` echo doors (`test/signatureState`
counts the asks and echoes kind/character/offset from the server's OWN
frame) and the crashonce drill are already sync's, and the trigger
tests need all three — a separate mode would have had to re-invent the
tracking (the first cut learned this the hard way). Flags: `sighelp`,
`sigdelay=<N>` (holds the answer so a supersede is observable), and
`completiononly` riding it for the both-debounce pin (there `"("` is
also the advertised completion trigger, so one press is kind 2 for
BOTH routes). The mode advertises `hoverProvider:false` — one owner
for the shared popup, the clobber guard is pinned hub-side — and
answers a THREE-signature report with `activeSignature:1`, so picking
`[0]` can never hide behind a shared string. lsp binary +6 slots
(the round, the supersede+teardown kill, the door's Invoked contract +
the initialize-side `contextSupport:true` read back by the server, the
null-answer hide, both-debounce co-existence, the disarm cancels the
in-flight ask). One REAL discovery in the trigger tap: the view's M18
auto-close PAIR press is a TWO-unit pure insertion — pressing `(` IS
the signature gesture — so `onTriggerEdit` recognizes the pair shape
(`isAutoClosePair`, a hand-mirrored copy of the view's `kPairRules`;
the view exports no pair table and none is added) and arms the taps
with `typedChar` = the pressed opener.

C3 FOLDINGRANGE wire → the C1 seam — ✔ CLOSED green 2026-11-22 (five
slots, three repeat rounds clean, the lsp binary + the full ctest 7/7).
Implemented as written below, with three READINGS settled in the code:
(i) the dedupe TIE RULE is the view door's — sort start asc / end desc,
keep the WIDEST per start — NOT "first wire entry" (a decode that kept
the first would delete a range the door folds widest, and the two
layers would disagree about one wire); (ii) the fakeserver's folds are
ANOTHER `sync`-mode FLAG (`foldingrange`, C2's lesson learned up front,
never a separate mode) and its report is scanned from ITS OWN TEXT
MIRROR (brace depth: a line ending `{` opens, a line starting `}`
closes), so an edit changes the ANSWER and the debounced re-feed is
checked end to end — plus deliberate wire noise on every report (start
-3, ends 40/99/100, an inverted 5/3, an empty 4/4, a duplicate start 2
WIDER than the real one, a bare number, an object with no `endLine`) so
the clamp/drop/tie discipline is pinned by ONE exact-set compare: the
shared 9-line js fixture decodes to exactly
{0:1, 1:6, 2:8, 3:4, 7:8} and after `deleteLineAt(6)` to {0:1, 2:7,
3:4}; (iii) `releaseRegistration` grew an `editorAlive` flag — the
`QObject::destroyed` relay must NOT knock a fold door on an object
whose CodeEditor part is gone (UB), and the clear there is pointless
anyway; the client-DEATH shape of the hand-back is a separate walk
(`handBackFolds(specName)` off the `failed` relay). The `refused` bit
clears ONLY on an open/re-READY edge (`armFoldFeed(uri,
authorityEdge=true)`), never on an edit edge — that is the no-hammering
rule on the wire, counted through the fakeserver's `reqlog`. NEW
fakeserver tripwire that costs nothing: a `textDocument/foldingRange`
ask reaching a server WITHOUT the flag is exit 2, so every existing
sync-mode test is also the "the gate keeps the wire silent" pin — and
the fifth slot makes it legible (ZERO `REQ foldingRange` lines for the
same fixture on a provider-less server, whose grammar folds must stay).

Hub-INTERNAL: no FeatureHandler door — the hub owns the feed, no
std::function crosses, so the rejected log grows NOTHING new here
beyond private noise. Capability
`foldingRange{lineFoldingOnly:true}` — honest: the view folds LINES,
wire columns are ignored. Request `{textDocument}` fires: after
didOpen/re-READY, after a document replace, and debounced on
`editApplied` (`kFoldRefreshMs`, one in-flight, the re-arm discipline
of the completion timer). Decode `[{startLine,startCharacter,endLine,
endCharacter,kind}]` → `FoldRange{startLine, endLine}` — the spec's
endLine is INCLUSIVE (the view pair is too); characters DROPPED (the
declared `lineFoldingOnly` makes that conforming); clamp to
`[0, lineCount()-1]`, drop `end <= start` (a fold hiding nothing),
sort + dedupe starts (the door's own contract does it — decode keeps
it honest first). FEED `setFoldRanges`. The null-vs-error asymmetry
recorded: an ERROR answer (server advertised, then refused) →
`clearFoldRanges()` (the grammar serves again — the claim fell through);
null/EMPTY array → server mode with NOTHING foldable (empty is
authority, grammar stays mute while the claim stands). Teardown/
client-death → `clearFoldRanges` (the single-writer convention D3's
hint clear minted). Fakeserver: `foldingrange` mode — nested ranges +
`kind` fields + out-of-range lines (client clamps) + the error-answ
path. lsp +2–3 slots.

C4 LSP INLAY HINTS (hub-internal feed into the ALREADY-BOUND M5 door
`setInlayHints` — the hint record crosses the wheel since 1.6.x, zero
three-copies; this is the D3 record's prophecy paying: the door named
for DAP carries its LSP meaning NEXT TO DAP's). Capability
`inlayHintProvider: {}`. Request `{textDocument, range}` — v1 asks the
WHOLE document (`range` = [0,0]..[lineCount,last-line-length] capped at
`kInlayHintRangeLines` 2000 — the viewport range would need a scroll
signal the hub does not have; the cap is the guard, recorded). Decode:
label string OR `InlayHintLabelPart[]` parts CONCATENATED verbatim;
position through PositionConverter against the MIRROR (the only frame
that keeps the offsets honest); `paddingLeft/paddingRight` → ONE space
baked into `text` (the view anchors at (line,col) — no new record
field, pinned); `kind` carried NOWHERE yet (Type/Parameter share one
paint role — a restyle is a Theme-role ask, not this car). Caps:
`kMaxLspInlayHints` 500 (stop, do not sort-then-trim); out-of-document
positions clamp TOLERANTLY through the PositionConverter (the C3 reading
stands: a generous position reads the way a generous server means it —
a past-the-end line walks to the document's END, so the clamp MOVES
with the mirror and its movement is itself the flush-before-ask proof;
"DROPPED" was the pre-code reading, the code settled on the tolerant
read and the exact decoded-set compare pins both). Refresh: debounced
`editApplied` + re-READY + doc replace; teardown clears its OWN set
(single-writer convention). THE
CO-EXISTENCE STANCE (recorded, not hidden): the door is whole-set and
two producers exist — DAP paints only while a session is STOPPED, LSP
refreshes on every edit; LAST full-set writer wins, a DAP teardown
clears and the next LSP re-fire restores, LSP+DAP hints never coexist
in one set. A merge-by-producer tag on `InlayHint` is a FUTURE door
ask — this car keeps the convention honest instead. Fakeserver: the
hint authority RIDES the `sync` mode as flags (C2/C3's lesson: a
separate mode loses the didOpen/didChange tracking the hint POSITIONS
are only honest on top of) — `inlayhint` advertises
`inlayHintProvider` (after reading the client's `textDocument.
inlayHint` claim back — absent is exit 2) and answers from ITS OWN
TEXT MIRROR (anchor tokens alpha/beta/gamma: an edit changes the
answer, so the debounced re-feed is checked end to end) + fixed wire
noise (a parts label with BOTH paddings at {0,0}, a right-padded
plain label at line 0's exact end, an out-of-doc {400,0}, an empty
label, a position-less entry, a bare number — every sane entry also
carries a `kind` the paint client must decode NOWHERE), so the shared
7-line fixture decodes to exactly {" <h> " @0:0, "PAD " @0:11, RETA
@1:7, RETB @2:8, OUT clamped to the document end}; `hintnull` answers
NULL (the spec's "no hints" — a whole-set authority that paints the
EMPTY set and turns the fed bit), `hintfail=<N>` refuses past ask N
(the ASYMMETRY with folds: NO refused bit, the last set STANDS, the
next edge re-arms — folds stop asking, hints keep asking),
`hintmany=<N>` floods N synthetic hints (the cap pin rides the WIRE
window: a 600-report leaves exactly h0..h499 in the door),
`hintdelay=<N>` holds the answer for the close-over-in-flight cancel
pin, and `test/hintState` echoes the LAST ask's range end — an end
naming the CURRENT mirror is the flush-before-ask proof, FAR SIDE. An
inlayHint ask reaching a provider-less server is exit 2 (the fold
tripwire's twin). lsp +6 slots.

C4 CLOSED — ✔ green 2026-11-22 (all six slots, three repeat rounds
clean, the lsp binary 90/90, the full ctest 7/7). Hub-internal feed as
written; the readings the code settled: (i) the tolerant clamp above
(replacing "DROPPED"); (ii) NO `refused` bit at all for hints — the
fold door hands folds BACK TO A GRAMMAR when the claim stumbles, the
hint door has no grammar under it, and wiping an authority's last set
on a transient error would let a flaky server erase the paint;
(iii) `hintsFedFromServer` reads the SYNCER's uri — the feed key is
the registration key, the `documentUri` PROPERTY is a different thing
entirely (the fold getter already read it this way). Six slots: the
exact decoded FEED + far-side flush proof (asks counted 1→2→3, the
asked range end walks 6:6 → 7:6 with the mirror), the COEXISTENCE
stance both ways (a fed answer overwrites an app's earlier table; a
close over an in-flight NEVER-FED ask clears NOTHING — D3's
single-writer convention verbatim), the error-answer asymmetry (the
set stands, three asks fire anyway, the fed set clears only at close),
NULL as a whole-set authority, the cap's wire window, and the
capability control (ZERO inlayHint bytes, the app's table stands from
open to close).

C5 PULL DIAGNOSTICS (the registry claim made real). Capability
declare `textDocument.diagnostic {relatedDocumentSupport:false}` —
clangd FLIPS to pull exactly because we declared it (the push route
stays armed for servers that never took the pull). PULL MODE live per
client when `capabilityOffered("diagnosticProvider")` OR
`isRegistered("textDocument/diagnostic")` (dynamic registration rides
the generic registry — verified). In pull mode: (1) incoming
`publishDiagnostics` from THAT client is DROPPED (a server that took
the pull and pushes anyway would race two authorities — pinned by a
reqlog spy that paints NOTHING from the push in pull mode); (2) pull
triggers: after didOpen/re-READY, after didSave, debounced
`editApplied` (`kPullDiagnosticMs` 600). Request `textDocument/
diagnostic {textDocument, identifier:"qmn"}` — NEVER a
`previousResultId` (always a full pull; a `resultId` is stored and
unused, recorded): `kind:"full"` → decode; `"unchanged"` → ignore the
answer (paint stays); `"partial"` is spec-impossible without our
previous id — read as a FULL report (the tolerant read, pinned).
`items[]` share the decode function with the push route (ONE decoder
for both wire shapes: `params.diagnostics[]` vs `report.items[]` —
C5's refactor; the severity/range-clamp/swap discipline exists ONCE).
A -32601/-32601-style refusal mid-pull (advertised then refused) →
the client FALLS BACK to push mode (the armed decoder resumes). The
dynamic-registration path: `client/registerCapability`
`textDocument/diagnostic` → `registrationsChanged()` re-modes the hub
mid-session (the registry signal already exists). Fakeserver: `pull`
mode — static advertise + a DYNAMIC register variant + reports with
`kind`/`resultId` + a contradicting publish to pin the suppression.
lsp +3 slots.

Close audit (Car C pinned expectations): `--codegen` rejected log =
the G3 16-drop baseline + `requestSignatureHelp` (std::function, the
pinned class) + private-section noise ONLY — the setFoldRanges/
clearFoldRanges pair must appear BOUND (module init symbols on
CodeEditor), the hint doors byte-unchanged; wheel rebuild + pydemo
headless run per the four-literal ritual (1.11.0); a live clangd
probe (`tools/probe_lsp.sh`-style, manual) earns foldingRange +
inlayHint + pull diagnostics in ONE run against clangd — it is the
showcase's dress rehearsal (S1).

### Car D — multi-observer core (two editors on one CodeDocument)

The CodeDocument record's v1 clause ("one editor per document — a
multi-view machine needs a core multi-observer first") pays here: the
core's single edit-observer becomes a LIST (additive). PRE-DECIDED
knots at this level; machine-level shape decided at the car's design
pass: the HighlightEngine follows the DOCUMENT (one parse tree per
buffer, both editors paint from the same spans); undo history is SHARED
(it lives in the core — one history per buffer, honest); carets and
selections stay per-editor; LSP sync becomes DOCUMENT-scoped — the
ProjectSession registration answers "who owns the uri's didOpen", and
the syncer rides the document's `editApplied` signal (it already fires
for EVERY primitive mutation, whoever typed). tst_codeeditor grows the
two-editor slots; `setDiffDocuments` (two docs, one widget) stays
untouched by this surgery.

### Car E — wheel-2 (DAP across + the answer routes)

The G3 audit's 16 silent drops shrink, and DAP joins the shelf crossing:
`DapHub` (its doors were shiboken-shaped from day one for exactly this)
+ the nested plain-Qt payload records + the `showMessageRequest`-style
ANSWER ROUTES the audit left on the C++ side — the route MUST speak a
SIGNAL plus a plain-typed answer door (the recorded Shiboken fact:
Python cannot emit a C++ signal, and `std::function` params drop
silently). The three-copies rule (typesystem / bindings CMake / the
codegen-CLI list) grows AGAIN, the `--codegen` rejected-log audit is
mandatory (the 1.6.1 and 1.7.0 drift traps), and the 16-drop baseline
in AGENTS is REWRITTEN to whatever the new honest drop set is — EXACTLY
what the log shows, no silent change. `setTransportFactory` and friends
stay C++-only honest absences. Wheel smoke grows the one-Qt proof.

### Car S1 — qmn-demo grows into the live showcase

The demo finally speaks every surface it can reach: a diff door (open
two tabs / two files through `setDiffDocuments` + the F7/F8 nav the
chords already carry), a grammar door (`loadGrammar` through a dialog +
language rebinding), a DAP CONTROLS leg (continue/step/stop when the hub
is alive — the status bar already speaks every shelf signal), and
`--tour` (the pin table above). tst_demo grows a slot per new door
(dapfake spawn allowed — the recorded exception; clangd legs stay
spawn-free). The 15-knobs settings surface and the `--lsp`/`--dap` door
shapes ride unchanged.

### Car S2 — pydemo mirrors the showcase door-for-door

argparse grows the SAME flags (`--diff`, `--lsp`, `--dap`, `--dap-arg`,
`--dap-program`, `--tour`), the same Russian UX, the same status-bar
visibility route. This car CANNOT precede car E (the DAP doors are the
wheel crossing) and its tour legs mirror S1's pin list. The headless
green run stays a close ritual (the 1.9.1 lesson); the LIVE tour
against real clangd + lldb-dap is the gate-close ritual alongside
`probe_dap.sh` and `probe_clangd.sh`.

### Car Py — the PyPI half (last, infra)

The staged `[tool.cibuildwheel]` config grows the missing before-all Qt
bootstrap (the container trick — the PySide6 wheel ships no Qt headers;
the mirror.yandex.ru fetch is already proven), a real publish job for
tagged releases, and the auditwheel-exclude sync rule holds (two homes).
DoD honest: a 1.x.y wheel on test.pypi.org installs clean in a bare
container and the smoke runs from there. This car never blocks the
gate-close tour.

### Explicitly NOT in this gate (owner decision 2026-11-21)

Full GRAPHEME segmentation stays a separate future train: `col` →
graphemes would move the caret/click/selection/LSP-position semantics
across the whole surface and LSP positions are UTF-16-defined, so the
train needs its own mapping-layer design (the record stays in the
README's not-implemented list). `set/get_lsp_header` stay absent ON
PURPOSE (the ±H tax record). Markdown-in-hover and semanticTokens
painting stay "by design" where they are written.

### Gate 3 risks

1. **Core surgery temptation** — cars D and 0's perf fix both reach into
   core; the frozen-surface rule keeps them additive; the perf fix
   rides a timing slot whose FAILURE MODE IS A VACUOUS PASS (the
   recorded style), never a false red on CI.
2. **The rejected-log drift** — car E changes what Python sees; the G3
   audit baseline (16 drops) must be RE-PINNED, not eyeballed (both
   1.6.1 and 1.7.0 codegen traps were silent drift).
3. **Showcase drift vs the mirror rule** — S1 and S2 must stay
   door-for-door; the tour pin list is SHARED between them (one table,
   two drivers), and that table is the drift tripwire.
4. **Tour honesty** — a tour that skips everything still exits 0 by
   design; the gate-close ritual is the LIVE run (clangd + lldb-dap +
   gdb + dapfake all present), recorded in the CHANGELOG entry like the
   probe ritual's outcome was, or the gate may not close.

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



