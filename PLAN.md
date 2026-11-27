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

**STATUS (2026-11-28): CLOSED, with two cars DEFERRED by OWNER DECISION.**
The ordered train defect pair → M10b → M8b → multi-observer → wheel-2 →
S1 all CLOSED (1.9.2, 1.10.0, 1.11.0, 1.12.0, 1.13.0, 1.14.0 — every
close ritual green, the S1 tour face MEASURED live on this host). S2
and Py are DEFERRED, each carries its deferral record at its own
section below; the deferral argument and the GATE 4 that replaced them
live in the "# GATE 4" section at the end of this file. This is the
sanctioned way an earlier gate releases a later one: the owner's call,
written down, before any later-gate work starts.

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

C5 PULL DIAGNOSTICS (the registry claim made real) — ✔ CLOSED green
2026-11-22. Capability
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

C5 CLOSED — ✔ green 2026-11-22 (five slots — the spec said +3, the
two extra are the unchanged-answer leg and the no-provider control —
three repeat rounds clean, the lsp binary 95/95, the full ctest 7/7).
The readings the code settled: (i) the `fed` bit marks ONLY the hub's
PULL generation — push-published marks stay the server's own trail and
a close never wipes them (the pre-C5 M8 stance, untouched); a fallback
or teardown clear wipes a paint ONLY when a pull generation wrote it;
(ii) the local-refusal band is `reply.code <= RpcClient::kErrTimeout`
(-33100 is its most positive member — a -32601 refusal and a transport
death both flip the document back to push, only an open/re-READY edge
re-arms the pull: the no-hammering rule, pins hold); (iii) `kRegistrable`
grew `textDocument/diagnostic` (the spec's DiagnosticRegistrationOptions)
— the lsp layer is C++-only, zero binding surgery; (iv) THE SPEC'S
unregister key is `unregisterations`, and `LspClient` had been reading
`registrations` — a silent unregister-NOTHING the C5 dynamic leg caught
(red test first, the honest order), and the fakeserver's OWN battery
frame had been written to match that bug — both now speak the spec, and
the 9007 battery leg proves the reader from the other side; (v) the
registerCapability exchange rides the HANDSHAKE window (the request
leaves on `initialized`), so its proof is the client registry itself +
the fakeserver's fail()s — stderr markers there race a late-attached
spy. Five slots: the exact decoded report over a no-trailing-newline
fixture (the ONE shared decoder chewing wire noise — an INVERTED
same-line pair swapped, severity-9 clamped into the enum, an out-of-doc
`{999,x}` pair riding the DOCUMENT-END walk, a bare-number item) + the
flush-before-ask proof TWICE (the edited mirror shows in the next
decode, and `test/pullState` echoes the server's own scan seeing the
edited marker count), the `"unchanged"` promise (paint byte-identical,
ZERO door writes, no fed-bit turn), the dynamic REMODE both ways
(register arms the pull — the sole paintable source; the tst-fired
unregister lands the hub back on push and the very "bug" publish the
pull mode suppressed paints, with ZERO further asks), the refusal
fallback (push publishes paint again, exactly ONE ask ever, the fed
door reads false), and the control (no provider: zero asks from any
trigger, published marks survive a close exactly as they did before
C5).

Close audit (Car C pinned expectations): `--codegen` rejected log =
the G3 16-drop baseline + `requestSignatureHelp` (std::function, the
pinned class) + private-section noise ONLY — the setFoldRanges/
clearFoldRanges pair must appear BOUND (module init symbols on
CodeEditor), the hint doors byte-unchanged; wheel rebuild + pydemo
headless run per the four-literal ritual (1.11.0); a live clangd
probe (`tools/probe_lsp.sh`-style, manual) earns foldingRange +
inlayHint + pull diagnostics in ONE run against clangd — it is the
showcase's dress rehearsal (S1).

Car C CLOSED 2026-11-22 at 1.11.0 — every pinned expectation held. The
`--codegen` audit came back EXACTLY the pinned set: qmn-specific drops =
the G3 16-drop baseline + `requestSignatureHelp` (its `FeatureHandler`
is the pinned std::function class) + private-section/Qt-operator noise,
no surprise doors; the fold pair is BOUND — `FoldRange` grew its own
wrapper TU (`qmn_view_codeeditor_foldrange_wrapper.cpp`) and the wheel's
extension strings carry `setFoldRanges`/`clearFoldRanges`/
`setInlayHints`/`diagnosticsFedFromServer` (and `requestSignatureHelp`
as the name-only absence). The wheel half ran per the four-literal
ritual: container `--wheel` rebuilt and repaired to 1.11.0, smoke
(signals, one Qt6Core + one Qt6Network) and qtmonaco parity green,
pydemo's path-dep re-pointed, headless pydemo exits 0. The live probe
grew four legs and ran PROBE-OK (11 steps) twice over against clangd
23.1.5: signatureHelp answered at the typed-comma position, foldingRange
put the editor in SERVER mode with two clangd folds over the grammar's
silenced `@fold`s, the inlayHint feed landed the `firstArg` param-name
hint through the untouched M5 door — and PULL DIAGNOSTICS OBSERVED THE
OTHER BRANCH: clangd 23.1.5 claims NO `diagnosticProvider` (static nor
registered), so the live server keeps the pre-C5 push stance (fed bit
false, zero asks); the pull machinery itself stays the fakeserver's
ctest turf. Two probe-side traps found and recorded in the step texts:
the explicit feature doors need an app-side `syncer->flush()` (the M8
frame rule, live-proved), and a cpp document's unterminated half-typed
line swallows the NEXT declaration into parser error recovery — hover
name-recovers, signatureHelp cannot (the block moved before the broken
line). `tools/probe_clangd.sh` dropped its `-no-pie` (binutils 2.47
rejects it against Qt's PROTECTED data symbols; Arch GCC builds PIE by
default anyway) and grew the `libqmn_dap.a` link (D3's PUBLIC shelf
dep). Car D (multi-observer core) is the open car next.

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

#### Car D design record (written at the car start, 2026-11-22)

SIX machines, each green before the next opens. Version rhythm: the car
closes at **1.12.0** (additive minor). Machine letters restart at D1 —
car 4's DAP machines D1–D4 live in the Gate 2 record; every reference
here means "car D machine N".

Code facts the design rides (read 2026-11-22, all verified in code):
`CodeEditor::buffer_` is a REFERENCE member bound in the init-list
(`document_(new CodeDocument(this))` then `buffer_(document_->buffer_)`,
CodeEditor.cpp:368-371) — so ATTACH IS A CTOR FACT, there is no
`setDocument()` (reseating a reference is impossible; converting the
member to a pointer is hundreds-of-callsite churn for no gain); the
editor's ctor installs the ONE god-observer lambda over the buffer
(CodeEditor.cpp:406-463) whose documented per-mutation order is
highlight → findDirty → fold shift → wrap shift → diff debounce →
fold-hover clear → a11y events → `document_->noteEdit(ev)`
(modificationChanged?, editApplied, document textChanged) → the
editor's own `textChanged()`; a STANDALONE document installs its OWN
observer for the same `noteEdit` pass (CodeDocument.cpp:20-22); the core
funnels every mutation notification through exactly three sites
(`setText` one whole-doc event at TextBuffer.cpp:320, insert at :360,
remove at :377 — the same seam undo/redo replay rides), and
`setEditObserver` (TextBuffer.hpp:79) is the only door; the hub keys
EVERYTHING by uri (`registrations_`, `Registration{editor, specName,
syncer}` LspHub.hpp:429-433) and its live feed already connects
**`CodeDocument::editApplied` → `syncer->applyDelta`** (document-scoped
since M8 — PLAN's "it already fires for whoever typed" VERIFIED), while
its paint doors all target the ONE `reg.editor` (fold/hint/pull
teardown LspHub.cpp:976-1010, feed paints :2242/:2301/:2358/:2510/:2544,
client-death clears :2019-2025/:2086-2092/:2566-2572) and the OWNER
editor's `destroyed()` releases the whole registration; the diff view
ALREADY runs a second engine per buffer (`origHighlight_`
CodeEditor.hpp:1723, attached to `diff_->originalDoc->buffer_` at
:3416-3418 and fed through the document's `editApplied` in
`watchDiffOriginal`), i.e. one-engine-per-buffer is already in the tree,
AND its viewport is already view-scoped on a document-scoped engine (the
paint comment at :6122 "Viewports are per-view" — the shape D4 needs
already exists there); the engine carries a SINGLE viewport window for
the modified side (`vpStart_`/`vpEnd_`,
`setViewport` fed by paint) and exposes `spans`/`spansFor`/`foldsFull`/
`lineStarts` as buffer+language facts; `ProjectSession::openDocument`
refuses a uri held by a DIFFERENT editor (the `squatter` pin,
tst_glue:119) and `uriForEditor` is a bare linear scan over the entry's
editor; DapHub re-resolves editors through the session (DapHub.hpp:199,
206-209). The view's own edit paths assume CARETS DO NOT MOVE unless a
door moved them deliberately — undo/redo/`undoToMark` change the text
"without moving remaining carets" (1701-1718) and only
`applyCaretsFanOut`'s per-slot `finalOffset` re-anchors carets (1590-
1593); nothing in the view maps carets across an edit it did not author
(out-of-range carets survive only because `selRangeOf` clamps LAZILY at
:5294-5300). Undo/redo MARKS are CARET-stamped: `UndoMark{TextPosition
caret, TextPosition anchor, bool hasSelection, QVector<CaretSlot>
extras}` (CodeEditor.hpp:1099-1107 — LINE/COL, never offsets), stacked
1:1 under the core's group log and kept aligned by the per-view
`reconcileUndoMarks`/`lastPushSerial_` scheme (:1304-1321, with the
`Q_ASSERT(undoMarks_.size() == undoDepth())` alignment guard at :1321),
and that reconcile runs ONLY in the acting view's edit paths; the
`breakUndoChain` wrapper (:1354-1356) is what calls the core's
`breakUndoCoalescing`, from ELEVEN motion sites; the find/match set is
absolute offsets that SELF-HEAL by re-scan on any `findDirty_`; the DnD move
marker encodes the SOURCE editor's pointer as an id and
`lastMoveSourceId_` guards the delete half (:824-864, :1043-1055,
:8599). Buffer mutators inside CodeEditor.cpp are ~14 functional
groups (setPlainText :596, the DnD/cut/delete cluster :1027-1356, the
multi-caret fan-out :1597, undo/redo :1725/:1739, the edit-applier batch
:1823, the undo-caret math :5470, typing :5577, insertTextAt :5703,
backspace/word/line-delete :5949-6022, find-replace :7926, replace-all
:7990, the DnD move :8559) — the origin tagging of D3 covers exactly
this list.

D1 THE CORE OBSERVER LIST (core only, additive; QtCore stays QtCore) —
the single `std::function` slot becomes an ORDERED LIST. Doors:
`int addEditObserver(std::function<void(const EditEvent&)>)` returning an
OPAQUE token (monotone for the buffer's life, never reused, so a stale
token removes nothing), `void removeEditObserver(int token)`, `void
clearEditObservers()`. `setEditObserver` STAYS and keeps its v1 reading:
install ONE observer, replacing the whole list (the two in-tree call
sites and `tools/bench_highlight.cpp` ride it byte-identically). The
header's "Single observer — the view's highlight engine" block
(TextBuffer.hpp:65-78) and `editObserver_`'s "single observer (M4)" tag
(:261) are rewritten BY THIS MACHINE — a machine owns its own comments.
Notification contract (pinned): order = registration order; every
observer gets the SAME `const EditEvent&`; an add or a removal DURING a
notification takes effect from the NEXT event (the loop iterates a
COW copy of the list — a self-removing observer is legal, a re-entrant
edit inside an observer is legal and fires its own nested notification);
`setText` stays ONE whole-document event for every observer; nobody may
mutate the list from a different thread (the whole-widget GUI-thread
discipline). Cost note: one extra `std::function` call per observer per
mutation, no per-event allocation while the list is untouched (QVector
COW + refcount); the model bench's µs/op band and both timing tripwires
stay in band (vacuous-pass style, re-measure before touching a budget).
Tripwires (tst_textbuffer, guiless, inside the fixed-seed property
suite so `checkSubtree` runs after every op): registration order and one
event per primitive mutation (insert/delete/undo/redo replay/setText
counted per observer); token add/remove independence + the stale-token
no-op; remove-self-during-notification and add-during-notification
starts next event; a re-entrant insert from inside an observer (nested
event, invariants hold); `clearEditObservers` silences everything.

D2 MULTI-ATTACH MECHANICS (view) — the one public widening of the car:
**`CodeEditor(CodeDocument* document, QWidget* parent = nullptr)`**. The
consumer constructs the document (the existing no-editor ctor becomes
"consumer-owned, ZERO views" — its name stays, its meaning widens
honestly) and hands it to every view's ctor; the doc's view list grows
at ctor time and shrinks as views die. The existing
`CodeDocument(CodeEditor* owner)` stays byte-identical (a one-view
document parented to its editor, exactly today).
- **Ownership/lifetime (the car's #1 risk, CONTRACT not enforcement):**
  a view holds `CodeDocument*` + the `buffer_` REFERENCE into it, so
  THE DOCUMENT MUST OUTLIVE EVERY ATTACHED VIEW. The single-view
  parented shape already satisfies this (children die after the parent);
  a SHARED document must be parented to something longer-lived than the
  views (the app object / a tab host), and reparenting an
  editor-created doc (`doc->setParent(host)`) is the legal way to extend
  an existing document's life. No half-measure "detach" machinery is
  built: an editor whose document dies is UB by the same rule Qt itself
  uses, and the record says so. Pin the safe order in tst (views die
  first, the surviving view keeps text, marks, popups and undo doors
  working).
- **Per-view observer:** each attached view installs its OWN core
  observer (its token) for its per-view state and removes it in its dtor
  — the god-lambda stays ONE per view, MINUS the two document halves.
- **The multiplexer keeps the order byte-identical:** the DOCUMENT
  installs its own core observer (the `noteEdit` pass, the standalone
  shape already does this) and RE-ASSERTS IT AS THE LAST SLOT of the
  list whenever ANY observer arrives on its buffer — a view attaching,
  or (D4) its parse being created — so "document signals last" is a
  standing invariant, not a ctor trick. So per mutation: [view 1 pass]
  [view 2 pass]… → the document (modificationChanged?, editApplied, its
  own textChanged) →
  each view's `textChanged()` (now relayed from the document's
  textChanged, in attach order). For ONE view this is the v1.11 order
  verbatim, and every signal consumer sees EVERY view's state already
  reconciled (the fold/wrap/scroll numbers are current when
  `editApplied` fires) — pinned by a signal-order slot, not asserted in
  prose.
- **`setPlainText` on a multi-view document:** the document mutates
  ONCE (`buffer_.setText`, origin-tagged as document-issued) and then
  calls each view's replace-bookkeeping hook (the tail of today's
  `CodeEditor::setPlainText` minus the mutation: preedit clear, undo
  marks, caret/anchor/extra-carets collapse, scroll + ambient extent,
  the M13 fold drop, the C1 provider override, the wrap re-seed, the
  scrollbar range, `selectionChanged`, update). `CodeEditor::setPlainText`
  keeps its door and becomes the same call, so a replace from ANY view
  resets EVERY view's per-view state — one mutation, N bookkeepings,
  exactly one whole-doc EditEvent (a signal-count pin proves the N+1
  double-mutation trap never opens).
- **THE REPLACE ORDER, machine-critical (the inventory found it):**
  `buffer_.setText` fires the whole-doc event SYNCHRONOUSLY inside the
  mutation, so the multiplexer (the last core observer) would emit
  `editApplied`/`textChanged` BEFORE any replace hook runs — and today
  the clears run after the mutation, so a consumer never sees a folded-
  state mid-replace. Fix: the replace door sets a `replacing_` guard
  BEFORE the mutation; the multiplexer observer sees the guard and
  DEFERS its emission; the hooks run (every view's fold/wrap/caret/
  scroll state now reset, the meaningless whole-doc fold shift each
  view's pass just made is overwritten by its own hook — the same
  harmless-shift note the old comment carries); then the guard clears
  and the document emits its trio EXACTLY ONCE, in the v1.11 order,
  with the core's whole-doc delta as the payload. One event on the wire,
  zero stale-state windows, both orders pinned by count slots.
- **The diff doors' pointer-identity gates re-read, they do not move:**
  `setDiffDocuments` enforces the one-editor contract with raw pointer
  comparisons (`original->editor_` null and `modified->editor_ == this`,
  :2350-2352). Once a document carries a VIEW LIST those two lines read
  as "the original is view-free" and "this widget is a view of the
  modified document" — same rule, same refusals, the diff surface itself
  is otherwise untouched (knot 10). The `original must be STANDALONE`
  contract comment (:859-860) keeps its meaning under D2's
  consumer-owned shape: a document with a view elsewhere can never be a
  diff original, pinned by the existing refusal slots.
- Language/grammar, popups, options, theme, decorations, scroll,
  focus, IME preedit, find state, diff rows, fold toggles, wrap plans,
  geo caches, a11y: **stay per view** (the table below pins the few that
  become document-scoped: the modified flag — already — the undo history
  — already in the core — and the parse, which is D4).
- Tripwires (tst_codeeditor): the two-view ctor + `document()` identity
  both ways; text written in A reads identically through B's line doors
  and both `visibleLineTotal()`; a view dying keeps the document and the
  other view alive (safe order pinned); the consumer-owned doc shape
  (zero views → 1 → 2); `isModified`/`markSaved` are document-scoped
  across two views; per-view options/theme/readonly independence (A
  read-only, B writable); the signal-count and signal-ORDER slots.

D3 FOREIGN-EDIT RECONCILIATION (view) — the correctness core of the car:
today EVERY buffer mutation comes from the one view whose doors maintain
its own caret, so nothing in the view maps carets across a FOREIGN edit.
This machine makes each view reconcile state it did not author.
- **Origin tagging, view-side only (no core change beyond D1):** the
  document holds the "view currently mutating" pointer, set by a tiny
  RAII helper the view wraps its mutation groups with (`setText`/DnD/
  cut/delete/fan-out/undo/redo/applier/typing/insertTextAt/backspace-
  word-line/find-replace/replace-all/DnD-move — the ~14 groups listed
  above, plus the document-issued replace of D2). A view whose tag says
  "me" runs its god-lambda EXACTLY as today (the single-view case is
  byte-identical — that is the machine's headline tripwire, and the
  whole 547-slot suite is the tripwire); a tag saying "someone else"
  runs the mapping below instead of the self-maintained caret. A missed
  tag reads as a foreign edit → a double-shifted caret → the existing
  suite goes red, so the failure mode is LOUD, not silent.
- **The mapping algebra (one rule for caret, anchor, every extra caret
  slot and their selections, and every undo/redo MARK — the marks are
  CARET-stamped `UndoMark{caret, anchor, hasSelection, extras}` records
  of LINE/COL positions, so each position rides the SAME algebra
  THROUGH the offset conversion: resolve the TextPosition to an offset,
  map it, re-resolve to line/col in the post-op frame; mapped marks stay
  exact instead of rotting):**
  offsets STRICTLY AFTER the event's start shift by
  `inserted.length() - (end - start)`; offsets AT or BEFORE the start do
  not move; an offset (or a selection endpoint) INSIDE an eaten range
  clamps to the event's start; a selection wholly eaten becomes an empty
  selection at that clamp with the active end preserved as the identity.
  Caret LINE/COL fields re-resolve through `lineColAt(offset)` in the
  POST-op frame (the same conversion the self path uses) — a foreign
  delete of whole LINES can leave `caret_.line` past the end, which
  today only survives because `selRangeOf` clamps lazily (:5294); the
  re-resolve makes the state honest instead of lazily forgiven.
- **What does NOT move:** the view's scroll position and h-scroll/ambient
  extent (a foreign edit moves content under a viewport that stays put —
  honest, pinned, and the same stance the view already takes toward
  `docWidthPx_`, which no edit recomputes); fold state (the M13 sweep
  already runs per view for ANY event); wrap plans (same seam); the
  find/match set (`findDirty_` + the M6 re-scan self-heal already cover
  a foreign stream); server decorations (M5 sets are RE-FED by their
  owners, never shifted — the C1/C4/C5 staleness stance verbatim, now
  pinned across views); the diff rows (its own debounce already runs per
  view).
- **Signals:** a foreign caret shift does NOT emit `cursorMoved`/
  `selectionChanged` (the user did not move — and the hub's hover/popup
  taps ride those signals, so a foreign edit must not arm asks); the
  QAccessible cursor/selection events DO fire (an AT must follow the
  text). Pin both sides.
- **Popups:** any foreign edit CLOSES this view's open popups
  (completion/hover/signature/code-action) and drops its in-flight ask;
  an open signature/completion popup in B while A types is a stale
  anchor, not a partial truth.
- **Shared undo history (PLAN-pinned, made honest):** one log per buffer
  means Ctrl-Z in B can undo A's edit; coalescing chains and
  `beginEditGroup`/`endEditGroup` nesting are SHARED too, and
  `breakUndoCoalescing` — reached only through the `breakUndoChain`
  wrapper (:1354-1356), which is called from ELEVEN caret/selection
  motion sites (:885, :910, :1008, :1663, :8377, :8996, :9011, :9229,
  :9390, :9530, :9816) — freezes the SHARED chain, so B's mouse motion
  breaks A's typing chain into its own undo step. Recorded as the price
  of one history, not a bug to design around.
- **THE MARK-ALIGNMENT TRAP (the machine's sharpest edge, found in the
  inventory):** the mark bookkeeping is per view (`undoMarks_`/
  `lastPushSerial_`) but it is INDEX-ALIGNED with the buffer's group
  list, and `reconcileUndoMarks(pre)` runs ONLY inside the acting view's
  own edit paths (:1131/:1224/:1230/:1257/:1290/:1775/:1838/:5528/
  :5550/:5588/:5694/:5870/:5937/:5971 — every one a capture-then-
  reconcile pair around a mutation group). With a shared log, A's group
  push leaves B's stack one short and B's serial stale: the
  `Q_ASSERT(undoMarks_.size() == buffer_.undoDepth())` at :1321 fires on
  B's next self-edit (and in Release it silently misaligns, so
  `undoToMark` restores the WRONG caret state). THE RULE: every view
  reconciles on EVERY group the buffer pushes, foreign or not — a view
  that sees the serial bump without having authored the group appends a
  SYNTHESIZED mark built from its own D3-mapped pre-mutation carets, so
  the alignment invariant stays true for every view and the `Q_ASSERT`
  stays a real guard. Second half of the same knot: `undo()` gates on
  `!undoMarks_.isEmpty()` (:1721) — a per-view gate over a shared log —
  so a view that has never typed must still be allowed to undo; the gate
  reads as "no mark to restore" and the synthesized-mark rule makes it
  honest, pinned by the B-undoes-A slot.
- **DnD across two views of one document** is a first-class case, not an
  accident: the move marker carries the SOURCE editor's pointer id and
  `lastMoveSourceId_` guards the delete half, so a move-drag from A into
  B is the same-document move it already is across two editors, and the
  source view's caret/selection ride the foreign mapping. Cross-WIDGET
  copy stays byte-identical.
- Tripwires (tst_codeeditor): the mapping table for insert-at-caret /
  insert-before / insert-after / delete-through / wholly-eaten selection
  / multi-line paste / a whole-doc `setText` from the other view;
  multi-caret slots each mapping; a foreign delete of whole LINES leaving
  B's caret LINE valid (the `lineColAt` re-resolve, not the lazy
  :5294 clamp); undo/redo MARKS stay exact through a foreign burst (the
  LINE/COL stamp rides the offset conversion, so `undoToMark` restores
  the caret that was STAMPED, mapped); **every view's
  mark stack stays index-aligned through a foreign burst** (the :1321
  `Q_ASSERT` holds on BOTH views, and the mark B appends for A's group
  restores B's own mapped carets, not A's); `undo()` in a view that never
  typed undoes A's group once (the :1721 gate re-read); undo in B
  moving A's caret exactly like any foreign edit (and one undo step
  total); a foreign edit leaves scroll and server decorations untouched
  while fold/wrap DO shift; the no-`cursorMoved`/yes-a11y pair; the
  popup-close slot; the chain-break (mouse motion in B) and shared-group
  slots; a move-drag from A into B of the SAME document (the source
  half deletes once, both views' carets map); and the headline
  single-view byte-identical guard (a one-view editor's caret/marks/undo
  after every door, the tag path exercised, no mapping).

**D1–D3 BUILT 2026-11-22** (host ctest 7/7; `tst_codeeditor` runs 287
slots green + 2 pre-existing skips, i.e. the 277-slot pre-car suite + the
10 new two-view slots — the "547-slot" figure in the record above is
simply the wrong number): the origin tag landed as designed (23
`MutationAction` sites — the 22 capture/reconcile pairs plus the
`setPlainText` head), the self path
stayed byte-identical, and D3 added **zero core change beyond D1**. TWO
corrections to the record above, both found by the pins:
- **The algebra paragraph's offset conversion is frame-WRONG as
  literally written** ("resolve the TextPosition to an offset, map it,
  re-resolve" — at observer time the buffer has ALREADY mutated, so
  `offsetOf` speaks the POST-op frame while the bystander's positions
  were written in the PRE-op one; a foreign whole-line delete resolved
  the bystander's (2,2) to offset **2** and parked the caret at {0,0}).
  The built shape: `armForeignMirror` (at `beginAction`, BEFORE the
  action's first mutation — the one moment positions and buffer agree)
  snapshots the bystander's caret state as a `MirrorFrame` of OFFSETS,
  and those offsets ride the pure algebra across every event of the
  action; positions re-resolve through `lineColAt` only in a frame that
  is CURRENT (each event's post-op). `foreignLineDeleteLiftsBystanderCaretAbove`
  is the frame-trap tripwire.
- **Marks ride RAW — the "every MARK rides the SAME algebra" half is
  dropped.** Every mark entry is FRAME-PAIRED with its group slot
  (undoMarks_ = the frame as lived before the group applied, redoMarks_
  = the frame as lived when the matching undo was PRESSED) and the
  core's tape invariant lands a replay byte-BACK into that exact state,
  so mapping a mark through a replaying event restamps it in the wrong
  frame. Consequence for the move branch (a foreign undo pushes the
  group onto the bystander's redo branch): the ARRIVING mark is the arm
  snapshot (`foreignSnap_`, the frame this view lived at the press),
  never the leaving stack entry (the bystander's stale ARM frame); both
  directions symmetric. `bystanderRedoAfterForeignUndoRestoresFrame`
  pins it — it FAILED against the takeLast shape, a real bug the pins
  caught, not a pin-expectation slip. Diagnostic doors added:
  `CodeEditor::undoMarkDepth()`/`redoMarkDepth()` (public, additive;
  `CodeDocument::buffer()` stays private+friend-only, so tests read the
  mirrors through these).

D4 THE PARSE FOLLOWS THE DOCUMENT (view, one engine per buffer) — the
pre-decided knot's machine. D2/D3 deliberately keep one engine PER VIEW
(correct, 2× parse, the interim state recorded); this machine moves the
engine to the document and deletes the duplication.
- The document owns one `HighlightEngine` (view-internal, created
  lazily, attached to its OWN buffer as a core observer — the same
  mechanism the diff's chrome engine already borrowed by signal; once
  the core has a list, the engine rides the list directly and
  `watchDiffOriginal`'s `onEdit` relay DIES with it). Views borrow it:
  the paint path reads `spans`/`spansFor`/`foldsFull`/`lineStarts` from
  the document's engine, and the per-view geo cache/fold store/wrap
  plans stay exactly where they are. The borrow becomes an honest door:
  `CodeDocument` grows a view-internal parse accessor (a friend/private
  door like today's `noteEdit`, NOT public API), which kills the one
  place the view reaches into another document's private buffer
  (`beginDiffMode`'s `&diff_->originalDoc->buffer_` at :3417).
- **The engine keeps its own newline index.** `lineStarts_`
  (HighlightEngine.hpp:~330, shifted at its `onEdit` seam :700-709) is a
  SECOND index parallel to the core's car-0 lazy per-piece newline
  index; the shared engine keeps serving its own `lineStarts_` to every
  view and the two indexes are NOT merged (risk (e)'s no-core-temptation
  rule verbatim: car D does not touch the treap or the core index).
- **The viewport becomes a SET.** The engine's single
  `vpStart_`/`vpEnd_` window grows into per-slot windows keyed by an
  opaque view id (attach id at ctor, released in the dtor), and the
  adaptive policy scopes reparse+query to the UNION (a two-view split
  parses both halves and nothing else). Internals are unconstrained by
  the API rule, so this is engine-side, not a public door.
- **One grammar per buffer (the language knot, DECIDED):** language
  stays a VIEW door (`setLanguage`/`language()`/LanguageProperties stay
  where they are), but the door now drives the DOCUMENT's parse: setting
  it in ANY view re-points the shared parse and every view's ink follows
  it, LAST-WRITER-WINS. That is the fold-seam's one-provider rule read
  one level up: one buffer, one tree-sitter tree, no per-view grammar
  split (the mismatch bug the C1 record names). `language()` reports the
  document's parse language, so two views never disagree about what
  buffer they are looking at; each view keeps its OWN
  LanguageProperties rendering lookup, font and capture-role view of the
  SAME spans. **The diff view's original side** rides the ORIGINAL
  document's parse instead: the string door (`setDiffDocuments` with
  plain strings) creates the diff view's OWN lazy standalone doc, so the
  diff widget sets THAT document's parse language exactly as it sets
  `origHighlight_->setLanguage(language)` today (:3418); a
  CALLER-SUPPLIED original document (the non-owning pointer door,
  CodeEditor.hpp:880) may already have views of its own elsewhere, so
  the diff widget NEVER writes a language into a document it does not
  own — its chrome ink follows that document's parse, plain when unset
  (the no-clobber rule, the one-provider rule read sideways again).
  `origHighlight_` and the standalone-original special case collapse
  into the same mechanism, and `watchDiffOriginal`'s `onEdit` relay dies
  with the signal-fed shape.
- Tripwires: two views typing one keystroke → EXACTLY ONE reparse (a
  counted-refresh engine seam, the parse-count oracle the machine
  needs); both views' exact-span role tables (the `examples/*` fixtures
  the suite already asserts) come out of ONE engine and stay byte-equal
  across an A-typing burst; `setLanguage` in B repaints A's ink (last-
  writer-wins pinned) while A's own font/props survive; the diff view's
  original side still inks through the original doc's parse after
  `origHighlight_` is gone (the existing diff slots carry the oracle);
  viewport union — two views at opposite ends of a big doc parse BOTH
  halves (a state oracle on the coverage window, never a pixel);
  the grammar-registry reconcile (generation counter) still reaches the
  shared engine once, not once per view.

**D4 BUILT 2026-11-23** (host ctest 7/7; `tst_codeeditor` 291 slots green
+ the 2 pre-existing skips — the 287 from D3 + 4 new shared-parse slots;
the whole pre-D4 suite stayed the tripwire, and the two failures it
produced were real code bugs, not pin-expectation slips, see below):
built as designed, zero new public doors (the D2 attach ctor stays the
car's only widening). The document owns the lazy engine
(`CodeDocument::ensureParse` — created by the language door and NOTHING
else; attached to its own buffer, `reassertMuxLast` right after, so the
per-event order is [view passes in attach order, parse, mux]); the view
reads through the friend-only `parse()`/`existingParse()` pair (out-of-
line — CodeDocument stays forward-declared in the public header);
`origHighlight_`, its `beginDiffMode` creation, the `watchDiffOriginal`
`onEdit` relay and the paint's chrome-engine feed are ALL deleted — the
document's own core subscription replaced the whole signal-fed shape.
- **Engine doors moved to a viewport SET** (`setViewport(const void*
  key, start, end)` + `dropViewport`): per-surface windows, the union
  recomputed SYNCHRONOUSLY on every set/drop (the set is a handful of
  entries — a linear scan is the honest cost); the union is a BOUNDING
  BOX — coverage includes the gap between far-apart windows; parsing the
  per-window ranges separately is not on this car (the design record's
  loose "parses both halves and nothing else" phrase read exactly). A
  window can only ever GROW the union while every surface lives, so the
  engine's scoped parse/query window is monotone across alternating
  multi-view paints: at most ONE re-query per union growth, never the
  pre-D4 re-shrink flip-flop. `clearState` (language flip) deliberately
  KEEPS windows_ — a language change re-points the parse, not the set of
  viewing surfaces; every feed clamps into the CURRENT buffer, so a
  stale union can never parse garbage. `detachView` releases a dying
  view's entry; for a BORROW of a foreign document's parse (the diff
  chrome's window into the ORIGINAL's engine) detachView cannot reach —
  the release sites are `clearDiffDocuments` and the `~CodeEditor` diff
  block. Oracle doors added to the engine (internal seam, NOT API):
  `parseCount()` (the machine's counted refresh), `unionStart/End`,
  `covStart/End`, `viewportSurfaceCount`.
- **Language is document state, LAST-WRITER-WINS** through the unchanged
  view door: `setLanguage` forces the document's parse and re-resolves
  EVERY view's tab-stop override against its OWN per-view
  LanguageProperties map (the lookup re-keys live — a bystander with no
  entry for the new language falls back to the template, it does NOT
  inherit the other view's override). `language()` reads
  `existingParse` (empty = no parse = plain). No-clobber face pinned in
  the REWRITTEN `diffOriginalChromeSyntaxHighlight` (the old pin encoded
  the chrome's own engine — its failure was a mechanism change, the one
  legit pin rewrite of this machine): a caller-supplied original with no
  parse of its own inks PLAIN (and `beginDiffMode` never writes one);
  the same document, given a parse honestly by a view of its OWN (the D2
  attach ctor), inks the chrome through it — live edits ride the
  document's own subscription, and `clearDiffDocuments` keeps the
  foreign parse but drops the borrow window.
- **Two holes the pins caught** (both lazy-engine boundary bugs — the
  pre-D4 engine ALWAYS existed, so every "no parse yet" path is new
  territory): (1) `foldEndForLine` must NOT shortcut a parse-less
  document to -1 — the M13 indent fallback stays reachable (`revealAt-
  ScrollsPerModeAndUnfoldsFolds`, a grammar-less doc, failed and pinned
  the fix: the no-engine read is the no-GRAMMAR-captures read); (2)
  reading must never MATERIALIZE a parse — paint feeds and refreshes an
  EXISTING parse only, and the public `highlightSpans` door answers from
  `existingParse` (empty spans, the pre-D4 inactive-engine answer,
  without paying for an engine). `setLanguage` is the ONLY creator.
  Test-shape notes that fall out: feed text BEFORE naming the language
  (the engine's line index rides already-fed text, the M10 flow), and a
  never-painted stack view needs its FIRST paint before a reveal (the
  real scroller's cadence).
- Friend plumbing: `CodeDocument` is now befriended by the test class,
  and that names the trap — an unqualified `friend class TestCodeEditor;`
  inside `qmn::view` declares and befriends `qmn::view::TestCodeEditor`,
  NOT the global test class (the suite had NEVER leaned on friendship
  before D4, so the trap had never fired); the header forward-declares
  `::TestCodeEditor` at global scope and befriends it `::-qualified`.
- Tripwires shipped: `sharedParseOneReparseForTwoViews` (one parse pass
  for two grabs, byte-equal span tables, EXACTLY one counted reparse per
  foreign keystroke across both repaints, surface count 2→1 on view
  death), `sharedLanguageLastWriterWinsAndPropsStayPerView`,
  `viewportUnionCoversBothViewsAndShrinksOnDetach` (1600-line python
  > 16 Ki units; union/cov state oracles, never pixels),
  `registryRemapReachesSharedParseOnce` (a capture-role remap reaches
  the bystander's spans through the one engine, after only the OWNER
  painted).

D5 DOCUMENT-SCOPED LSP GLUE (glue + session + fakeserver) — the
registration, the syncer and the feeds become document-scoped, and the
paint fans out.
- **The VIEW JOIN rule (zero new glue doors):** `LspHub::openDocument
  (editor, uri, languageId, serverName = {})` (LspHub.hpp:115-117) when
  that uri is ALREADY registered and
  `editor->document()` IS the registered document → a VIEW JOIN: the
  registration's view list grows, its caret/popup/completion/mouse taps
  connect for the joining view, and NO second `didOpen`, NO second
  syncer and NO version-stream restart happens (the far-side frame count
  is the oracle). Same uri, DIFFERENT document → today's behavior stands
  byte-identical (release + re-open: the second claim replaces the first;
  two documents claiming one uri stays the app's bug to avoid). Feature
  doors need NOTHING: they already resolve through `editor->document()`
  → uri → the ONE syncer, so a hover asked on view B rides view A's
  mirror — the flush-before-ask rule keeps it honest whoever typed.
- **Paint fan-out:** every fold/hint/diagnostic write
  (`setFoldRanges`/`clearFoldRanges`/`setInlayHints`/`setDiagnostics`)
  goes to EVERY view of the registration, and so does the teardown
  (a client death or an unregistration clears folds/hints/pull marks in
  all views). The state tables stay per-URI (they already are) — the
  feed is document-scoped, the paint is per-view.
- **Teardown ownership:** the OWNER view's `destroyed()` releases the
  registration (today's rule, `editorAlive=false` and all) UNLESS a
  joined view survives — then the ownership MOVES to the next view (the
  `didOpen` stream is NOT re-opened, the syncer stays, the feeds stay
  armed, and the dead owner's fold/hint state dies with its widget);
  a non-owner view's death just removes it from the list (and its paint
  dies with its widget, no clear needed). Pin both directions.
- **ProjectSession alias rule:** the session keeps ONE entry per uri (the
  owner) — `openDocument(viewB, uri)` on a same-document second view is
  an idempotent TRUE that forwards the join to an attached hub, and
  `uriForEditor`/`editorForUri` resolve ANY view of a registered document
  by comparing `editor->document()` (the linear scan grows one pointer
  compare); the `squatter` pin (a DIFFERENT editor with a DIFFERENT
  document on a held uri) stays FALSE, pinned side by side with the new
  true. `closeDocument(anyView)` is the document's close (didClose +
  release) — one document, one close, recorded. DapHub resolves through
  the session, so a breakpoint asked from the second view lands on the
  same document state; the DAP DECORATION PAINT stays single-target (the
  resolved view), recorded as the honest divergence until car E revisits
  the DAP surface.
- Tripwires: tst_glue (no server EVER spawns there — the unclaimed-
  language shape): the session alias pair (join true / squatter false),
  `uriForEditor` both views, close-any-view semantics, the one-entry
  registry; tst_lsp (fakeserver, the far-side oracle): two views →
  exactly ONE `didOpen`, edits from A then B arrive in ONE stream in
  ORDER and in PRE-OP frames (the mirror echo proves it), the version
  stream is monotone across both views' typing, folds AND hints AND
  pull marks reach BOTH editors' doors, the owner-death handover keeps
  the `didOpen` and the feeds, a joined-view death keeps them too, and
  `saveDocument` from either view flushes the ONE syncer (the version
  pin).

**D5 BUILT 2026-11-23** (host ctest 7/7; `tst_lsp` 98 slots green = the 93
that walked in + 5 new, `tst_glue` 8 = 6 + 2 new; zero new public
doors — the D2 attach ctor stays the car's only widening, so the bindings'
three copies are untouched this machine; `tests/fakeserver/` needed NO
change, the far-side oracles the pins lean on were already built):
- **`Registration` grows `QList<CodeEditor*> views`** (owner ≡
  `views.first()`, the handover rule's invariant) and
  `syncerFor(editor)` scans `it->views.contains(editor)` instead of
  comparing one pointer. That single change is what makes EVERY feature
  door (`requestHover`/`completion`/`codeAction`/`willSave`…, `saveDocument`,
  `setPopupTriggers`/`popupTriggersEnabled`) answer for an alias view for
  free — they all resolve editor → document → uri → the ONE syncer.
- **A new `resolveUri` leg the join pin forced:** an EMPTY uri on a
  document the hub already registered resolves to that registration's KEY
  (the scan compares `editor->document()`), BEFORE the `documentUri`/
  `untitled:`-mint legs. Without it the second view of an untitled
  document forked a SECOND `didOpen` under a minted key — which is exactly
  the frame count the machine forbids, and it is the hub's own copy of the
  session's one-document-one-uri rule.
- **The JOIN branch** (`it->editor->document() == editor->document()`):
  view list grows, `connectViewTaps` wires the joiner's caret/accept/mouse
  taps + the `destroyed()` relay, and the three feeds re-arm with
  `authorityEdge=true` — the join is an OPEN EDGE, so the new view is
  painted folds/hints/pull marks WITHOUT any edit. Nothing touches the
  wire, and a view that already carries the document is an idempotent true
  (not even a re-arm). Ask counters are the single-writer proof: 2 asks
  per feed after the join (open edge + join edge), 3 after ONE edit in
  either view — never one ask per view.
- **One piece of new registration state:** the LAST push-published report
  (`published`/`hasPublished`). Folds/hints/pull catch a joiner up by
  RE-ASKING; `publishDiagnostics` has no ask to re-send, so the join
  replays the stored report onto the new view (the WHOLE array, empty
  included — a publish of `[]` is "no more problems" and the joiner
  deserves that same truth). Nothing else in the hub learned to remember.
- **Deaths, all three shapes pinned:** the OWNER's death hands over
  (`views.first()` becomes owner; the far-side `documentCount` stays 1 — a
  stray `didClose` would exit 2 and the door answers -1; the SAME syncer
  pointer and the SAME version number survive, the feeds never go mute and
  the anchor moves); a JOINED view's death is a plain removal (wire and
  the survivor's paints untouched); the LAST view's death releases with
  `editorAlive=false` (didClose, count 0, `clientForUri` null). The pins
  use the lifetime contract's legal extension (`document()->setParent(...)`
  before `delete owner`) so nothing leaks.
- **The popup story, told as designed:** popup-OWNER state (the `on` bit,
  the three debounces, the pending asks, `TriggerState::lastView`) stays
  ONE per document; popups stay PER VIEW, and the one that answers is the
  validated interaction anchor (`interactionView` checks `lastView`
  against the live view list, falls back to the owner). Typing keeps the
  document-scoped edit tap (the signal never names its author) and the
  popup still lands in the view that typed, because that view's own
  `cursorMoved` paired with the edit set the anchor first. Two recorded
  readings this machine had to settle: (1) a BYSTANDER's popup closing on
  another view's keystroke is car D's VIEW rule (the foreign-mutation pass,
  pinned in the D3 slots) — not the hub's fan-out, and the pin says so;
  (2) the hub's OWN dismissals (`setPopupTriggers(…, false)`, a document
  teardown with live views) close EVERY view's popups — the widened shape,
  recorded (a single-view release never dismissed a popup it did not own;
  leaving one showing an orphan the wire can no longer update is the worse
  story). The mouse no-refire pin is read SAME-view-only (one popup per
  view, so a second view dwelling at the same point owes its own), and the
  `workspaceConfiguration` push stays OWNER-only (the buffer is per-view —
  the owner speaks).
- **Session alias rule built** (`Entry{editor, views, languageId}`): one
  entry per uri; same editor + uri = idempotent true; a different editor
  whose `document()` IS the registered document = an ALIAS JOIN (the
  entry's `views` grows, an attached hub gets the same uri call, trigger
  re-arms, and `documentOpened` deliberately does NOT re-fire — a tab
  strip rides document-opened, not view-opened); a different DOCUMENT on a
  held uri keeps the squatter refusal; a document already registered under
  ANOTHER uri is the mirror-half refusal (one document, one uri).
  `uriForEditor` resolves through document identity, `closeDocument(any
  view)` is the document's close, owner death hands over, the last view's
  death drops the entry. `DapHub::editorForSourcePath` carries the
  recorded DAP divergence comment (a breakpoint ask resolves from either
  alias view, the door returns the OWNER, so gutter dots paint there only
  — DAP's decoration table has no per-uri fan-out door; alias paint is
  car E's question). SUPERSEDED at car E machine E1 (2026-11-24): that
  sentence records the 1.12.0 ship, the comment is RETIRED and the alias
  paint closed — see the E1 record below and the door is now
  `editorsForSourcePath` + `ProjectSession::editorsForUri`.
- Tripwires shipped: `hubViewJoinOpensOneDocumentAndStreamsEditsFromBoth
  Views` (one `didOpen`, one syncer, version 0 through the join, edits
  from BOTH views in ONE monotone stream, the server's own `saves` counter
  proves the save door crosses once from either view, any-view close = one
  `didClose` + both views unregistered + a second close is the silent
  no-op), `hubViewJoinFoldHintAndPullPaintBothViewsAndAskOncePerEdge`,
  `hubViewJoinPushDiagnosticsPaintBothViewsAndStayOnClose` (a published
  report is the document's trail: both views wear it, and a close never
  wipes it — the M8 stance widened to the view list, not changed),
  `hubViewHandoverKeepsWireFeedsAndMarksStanding`,
  `hubViewJoinPopupsFollowTheInteractingViewAndDisarmHidesBoth`;
  `tst_glue`'s `aliasViewSharesOneEntryAndAnyViewCloses` and
  `aliasHandoverWhenTheOwnerViewDies` (still NO server anywhere in that
  binary). Two expectation slips worth the record, both MINE re-spelling
  car C's far-side tables over a DIFFERENT fixture, the hub innocent both
  times: the pull marks' clamped end columns ride the MIRRORED line's
  length (line 1 is 7 units in `kFoldFixture`, 10 in `kPullFixture`), and
  the marker-named pull items are Error 0 — only the wire-noise pair
  carries Warning 1.
- Run early, recorded at the machine's head (the ritual repeats it at the
  close): the live probes are BOTH green through the surgery —
  `tools/probe_clangd.sh` PROBE-OK, 11 steps on clangd 23.1.5, and
  `tools/probe_dap.sh` PROBE-RITUAL-OK, both adapters agreed. The hub's
  paint pair is the part of D5 a real server would feel, and nothing moved.

D6 THE CLOSE RITUAL + the docs — the public surface is ONE ctor, so the
wheel side stays honest with almost no code: the typesystem grows
`remove="all"` for `CodeEditor(qmn::view::CodeDocument*, QObject*)` (an
explicit honest absence — Python cannot share a document yet, because
the consumer-owned `CodeDocument()` ctor stays REMOVED for the wheel
too; opening it is a lifetime-pin question for a later car, recorded,
not a code blocker), the THREE-copies lists gain NOTHING (no new bound
type — the D2 rule check), and the `--codegen` rejected log must gain
NOTHING qmn-specific beyond that honest removal + private/Qt noise (the
car C baseline re-pinned, per risk 2). Then: both package literals to
1.12.0 in ONE commit (`QMN_GLUE_VERSION` stays 1.0.0 unless the shelf
itself breaks — D5 widens it additively), the container `--wheel`
rebuild + smoke + parity, pydemo's fourth literal + a headless green
run, ctest 7/7 three times over, the two probes (clangd + dap) still
PROBE-OK at the car's head — the LSP surgery touches the hub's paint
pair, so a probe regression here is cheap to catch and expensive to
carry — CHANGELOG/README/AGENTS records (the "one editor per document"
sentence gets REWRITTEN, not amended, at every place it is a contract:
`CodeDocument.hpp:22` ("owns the single core observer slot"), `:28`
("one buffer, one observer slot"), `:41-47` (the attached-vs-standalone
v1 clause) and `CodeEditor.hpp:854-855` (the diff-mode restatement),
plus AGENTS's same sentence), the stale comment the design pass found
and D6 fixes in passing (`CodeEditor.hpp:869-871` still claims
soft wrap is inert inside diff mode while the `DiffState` doc at
CodeEditor.cpp:64-73 says the knob is LIVE — car B's slip), and the
commit/tag/push asks stay fresh per step.

Pinned knot table (the decisions this record makes, so no machine
re-litigates them):
1. Attach at CTOR only (`buffer_` is a reference); no attach/detach door,
   no pointer-conversion churn.
2. The document must OUTLIVE its views (contract + tst pin), and a
   consumer-owned document is the app's object; the single-view
   editor-parented shape stays byte-identical.
3. The document's multiplexer rides the LAST core-observer slot
   (re-asserted whenever any observer lands on the buffer — attach or
   parse) → the v1.11 signal order is byte-identical for one view and
   every view is reconciled before any signal.
4. One mutation, one EditEvent, N per-view passes, N bookkeepings on a
   replace, and the replace's signals are DEFERRED past the hooks (the
   double-mutation and the stale-window traps both pinned by counts).
5. Caret/selection per view (PLAN-pinned) + the offset-only mapping
   algebra, origin-tagged view-side; single-view paths byte-identical.
6. Foreign edits are silent to `cursorMoved`/`selectionChanged` and loud
   to a11y; they close popups; they re-feed, never shift, decorations.
7. One shared undo log per buffer, shared coalescing/groups/breaks, and
   per-view mark stacks kept index-ALIGNED by the synthesized-mark rule
   (every view reconciles every group, foreign included, and the :1321
   Q_ASSERT stays load-bearing); the modified flag/document replace stay
   document-scoped (already).
8. One parse per buffer, one grammar per buffer LAST-WRITER-WINS, one
   viewport SET (union), per-view geo/fold/wrap/ink props; the diff
   original side rides its own document's parse and `origHighlight_`
   dies.
9. One registration/`didOpen`/syncer/version stream per uri; a
   same-document second view JOINS; paint fans out to every view; the
   registration's ownership hands over; the session keeps one entry per
   uri and resolves every view of a document; DAP paint stays single-
   target (recorded divergence for car E).
10. `setDiffDocuments` (two docs, one widget) is NOT touched by the
    surgery — its pointer-identity refusal gates re-read as view-list
    gates (same rule, same refusals), and D4 removes the chrome engine
    it used to own and gives it the document-parse door instead.

Risks: **(a) lifetime** — a shared document dying before its views is
UB, and nothing enforces it; mitigation is the ctor-time attach, the
recorded contract and a tst slot for the safe order (the diff view's
editor-owned `diffStandaloneOriginal_` never gains a view, pinned).
**(b) the origin tags** — ~14 mutation groups must be tagged; a missed
tag double-shifts a caret and the existing 547-slot suite is the
tripwire. **(c) the ordering flip** — a signal consumer that read view
state during `editApplied` used to see a half-reconciled view in NO
case (the order is preserved), so any red here means the last-slot rule
broke; the ORDER slot is the guard. **(d) D4's viewport union** — the
adaptive policy was measured for ONE window; the union may re-scope a
reparse (the recorded budgets stay, the counted-refresh tripwire is
vacuous-pass style). **(e) core temptation** — only the observer list
lands in core; the newline index, the undo log and the treap stay as car
0 left them.

DoD: ctest 7/7 (three clean rounds); tst_textbuffer + the D1 slots;
tst_codeeditor + the two-view slots of D2/D3/D4; tst_glue + the session
alias slots; tst_lsp + the fakeserver fan-out slots; `--codegen` audit
== the pinned set; the wheel half green at 1.12.0; both live probes
PROBE-OK at the head; the `CodeDocument.hpp` header contract, AGENTS,
README and CHANGELOG records written; the car closes at 1.12.0 and car
E (wheel-2) is the open car next.

**D6 BUILT + CAR D CLOSED 2026-11-23** — the docs/wheel half, then the
ritual. What the car's last machine actually was:
- **The doc audit came FIRST and it shrank D6's list**: the four
  `CodeDocument.hpp`/`CodeEditor.hpp` sentences the design record named
  ("owns the single core observer slot", "one buffer, one observer slot",
  the attached-vs-standalone v1 clause, the diff-mode restatement) were
  ALREADY rewritten at D2, when the attach ctor and the OUTLIVE contract
  moved into the headers. Two real catches survived the audit:
  `CodeEditor.hpp`'s diff block still said "(the one editor per document
  contract untouched)" — car D REPLACED that rule, so the block now says
  the modified document is an ordinary document that may carry other
  views, while the DIFF STATE stays per-view and the standalone original
  stays the borrowed window M10 made it — and the same block claimed
  **soft wrap is inert inside diff mode**, contradicting the `DiffState`
  doc in the `.cpp` (the knob is LIVE, each side wraps at its own window).
  The stale claim was car B's slip, not car D's; both sentences now carry
  the weighted formula. AGENTS/README/CHANGELOG got the car: AGENTS's
  current-state line + data-model conventions carry the two new contract
  bullets (ONE document MANY views + the lifetime rule; DOCUMENT-scoped
  glue with the JOIN/HANDOVER rules and the three recorded divergences),
  and README's perf row was RE-MEASURED, not re-asserted (its ~5 ms
  `setText` / ~7 µs rows were the v1-freeze readings; this runner prints
  ~9 ms / ~11 µs/op — the same shape car 0 recorded).
- **The wheel half, one line of surface**: the typesystem grows
  `remove="all"` for the attach ctor — an honest absence, since Python has
  no consumer-owned document to attach (the standalone `CodeDocument()`
  ctor stays removed) and a garbage-collected owner would decide the
  OUTLIVE contract badly; the door stays open in C++ only. The
  THREE-copies lists gained NOTHING (no new bound type — the D2 check
  held). `--codegen` at the bumped head: the three model checks all report
  OK (class model, typesystem, function modifications — the last one is
  what proves the new `remove="all"` rule matches a REAL door instead of a
  typo'd signature), and the rejected log's delta against the 1.11.0
  baseline is
  exactly 11 qmn entries, every one a PRIVATE seam whose type is
  deliberately unbound (`HighlightEngine*` returns on `CodeDocument::
  existingParse`/`ensureParse` and `CodeEditor::parse`/`existingParse`/
  `diffOriginalParse`, `const TextBuffer::EditEvent&` params on
  `emitEditTrio` and `reconcileForeignEdit`, and four `LspHub` internals
  that speak `QMap<…>::const_iterator` or `qmn::lsp::DocumentSyncer*`),
  and one RENAME (`CodeDocument::noteEdit` → `emitEditTrio`, which is why
  the log shows one entry GONE). The previously-dropped PUBLIC glue
  surface is byte-identical between the two logs (the AGENTS inventory of
  `std::function`- and `qmn::lsp`-typed doors), and the four NEW
  `qmn::glue` entries are all PRIVATE `LspHub` helpers — a private seam
  dropping is not a Python-visible change, which is why the THREE-copies
  lists stayed untouched.
- **The wheel-honesty probe** (a container against the REPAIRED 1.12.0
  wheel, repo off `sys.path`) pinned the absence at the wheel, not just in
  the XML: `CodeEditor(document)` is a `TypeError` naming only
  `(parent=None)`, `CodeDocument()` is a `NotImplementedError`
  ("cannot be instantiated"), and the door set is otherwise byte-intact —
  both `setDiffDocuments` overloads, the fold/decoration/grammar doors,
  `LspHub`/`ProjectSession`/`ServerSpec`, the `Monaco` shim, one
  `libQt6Core` and one `libQt6Network` in `/proc/self/maps`.
- **The bench A/B, with two traps worth their own lines**: a first attempt
  built both cores by hand and read 55 µs/op for HEAD AND for the car — hand
  flags without `-DQT_NO_DEBUG` (and `-fPIC` without
  `-mno-direct-extern-access`) are a DIFFERENT BUILD, not a regression, and
  the same wrong include path can pair one tree's header with another's TU
  and segfault the bench. Built like the release (both archives from the
  CMake Release recipe, the same bench TU, four interleaved runs): the
  pre-car-D core mean **11.0 µs/op**, the car D core mean **10.9 µs/op**,
  `nodes=29973` on both — the LIST costs nothing. And the absolute number
  is not a baseline: the same v1.11.0 source read 7.2 µs/op in an earlier
  hand-invoked run of this session, which is why README's row now carries
  the day it was measured instead of a bare number.
- **The ritual, all green at the head**: the two package literals
  1.11.0 → 1.12.0 (`QMN_GLUE_VERSION` stays 1.0.0 — the shelf widened
  additively), the container `--wheel` rebuilt/repaired/smoke-green/
  parity-green (28 `text_changed` announcements), pydemo's fourth literal
  re-pointed and its headless run green, the C++ packaging smoke (install
  + `examples/consumer`: both smoke binaries build against the installed
  1.12.0 package), ctest 7/7 in three rounds (99-101 s each), and the
  live probes re-run at the head: `tools/probe_clangd.sh` PROBE-OK, 11
  steps on clangd 23.1.5, and `tools/probe_dap.sh` PROBE-RITUAL-OK, both
  adapters agreed.
- **Car D CLOSED at 1.12.0.** Car E (wheel-2) is the open car, and it
  inherits one named question from this one: DAP's breakpoint paint lands
  on the OWNER view only (`DapHub::editorForSourcePath`), because DAP's
  decoration feed has no per-uri fan-out door — alias paint is E's to
  answer, and E is also where the glue drop inventory finally gets read
  against the logs: the "16 glue drops" the G3 audit named was the NAME of
  a set, never a line count — the rejected log actually carried 35 glue
  lines at 1.12.0 (34 `LspHub` + 1 `ProjectSession`, 17 of them public
  doors) plus 9 on `DapHub`; E4's paragraph is the count record.

### Car E — wheel-2 (DAP across + the answer routes)

The audit set the G3 audit named — its "16" was a SET NAME, never a line
count: read against the archived logs the real figure was 35 glue lines at
1.12.0, 9 more on `DapHub` (E4's paragraph is the count record) — and DAP
joins the shelf crossing:
`DapHub` (its doors were shiboken-shaped from day one for exactly this)
+ the nested plain-Qt payload records + the `showMessageRequest`-style
ANSWER ROUTES the audit left on the C++ side — the route MUST speak a
SIGNAL plus a plain-typed answer door (the recorded Shiboken fact:
Python cannot emit a C++ signal, and `std::function` params drop
silently). The three-copies rule (typesystem / bindings CMake / the
codegen-CLI list) grows AGAIN, the `--codegen` rejected-log audit is
mandatory (the 1.6.1 and 1.7.0 drift traps), and the "16-drop baseline"
wording in AGENTS is REWRITTEN to whatever the honest drop set is —
EXACTLY what the log shows, no silent change. `setTransportFactory` and friends
stay C++-only honest absences. Wheel smoke grows the one-Qt proof.

#### Car E design record (written at the car start, 2026-11-24)

FIVE machines, each green before the next opens. Version rhythm: the car closes
at **1.13.0** (additive minor) and the SHELF bumps its own literal 1.0.0 →
**1.1.0** — the G3 wheel record named that bump for exactly this shape
("signal-relay feature doors added to the SHELF (additive, shelf semver minor
1.1.0)"), and since G3 the shelf's semver is Python-visible, so semver proper
applies: additive growth = MINOR. The `src/glue/CMakeLists.txt:33-36` wording
("bumps ONLY when the shelf itself breaks") is pre-crossing and E5 rewrites it.
Machine letters restart at E1. **Name collision, recorded:** PLAN already owns
an `E1`/`E2`/`E3` trio at :271-325 (the Gate-1.5 M7/M9/M13 sections), and
top-risks #3's "Treat E1→E2 as one mapping core" means THOSE; every E-number
below means "car E machine N".

Code facts the design rides (read 2026-11-24, all verified in code): `DapHub`'s
doors really are Shiboken-shaped already — D4's claim ("every ANSWER route is a
SIGNAL and every payload is a plain Qt type", DapHub.hpp:48-49) reads TRUE
against the public surface: the ctor plus FIFTEEN doors (:98-164 —
`setTransportFactory`, `attachProject`, `project`, `startDebugging(QString,
QStringList, QJsonObject, QString)`, `stopDebugging`, `terminateDebugging`,
`isDebugging`, `isReady`, `adapterCapabilities`, `configurationDone`,
`request(QString, QJsonValue)`, `client`, `setBreakpoints(CodeEditor*,
QVector<int>)`, `requestedBreakpoints`, `breakpointMap`→`QMap<int,int>`) —
THIRTEEN of the fifteen cross, plus the ctor (the LspHub precedent: a PUBLIC
`DapHub(QObject* parent = nullptr)`, the app builds and owns it); only two
cannot: `setTransportFactory` (`std::function`) and `client()`
(`qmn::dap::DapClient*`, the `syncerFor` drop-by-type-absence precedent). The
nine signals (:167-187) all carry plain Qt payloads:
`QString`/`int`/`QJsonObject`/`QJsonValue`. On the container question the wheel
has one precedent and one blank: `QVector<int>` ALREADY crosses (the M5 gutter
doors `setBreakpointLines`/`breakpointLines`, CodeEditor.hpp:385-386 — neither
appears in the rejected log, so they wrapped since M5); `QMap<int,int>` has NO
precedent anywhere in the bound set (no bound header carries a map), and
neither does a container of QObject pointers, so the codegen run decides THOSE
two shapes, not this record. The wheel carries ZERO DAP type today (grep `dap`
over `bindings/`: nothing in the typesystem, the CMake `HEADERS`/`_generated`
lists, the `--codegen` CLI, the smoke or the shim), and `_qmn` already absorbs
the DAP layer — `target_link_libraries(_qmn PRIVATE qmn_glue)`
(bindings/CMakeLists.txt:140) over `qmn_glue`'s PUBLIC `qmn_dap` link
(src/glue/CMakeLists.txt:21); `DapHub.hpp` is already an INSTALLED glue header
(:40) and the shelf's first source entry (:12). `ApplyReport`
(LspHub.hpp:387-392) is plain Qt (`bool`/`int`/`int`/`QStringList`) with NO
value-type entry, and the applier family carries a defaulted
`qmn::lsp::LspClient*` tail (:393-414); the serving-client guard is ONE line —
`if (servingClient && client != servingClient)` (LspHub.cpp:3068) — so a null
tail means the gate is OFF, not on. The answer routes each own half a route:
`serverShowMessageRequest`'s comment names its way back ("the app answers
through clientForSpec(spec)->respondShowMessageRequest(id, item)") and
tst_lsp's `showMessageRequestFirstAnswerWinsAndNullDefaultFollows` (:3768)
takes it at :3808, with the fakeserver's `MSGREQ-OK`/`MSGREQ-NULL-OK` markers
(:3821/:3851) as far-side oracle; on the DAP side `dapRequest` relays the whole
request and the answer doors are `DapClient::respond(id, body)` /
`respondError(id, code, message, data)` (DapClient.hpp:133-134 — plain Qt AND
already `bool`, while the two LSP client doors are `void` — the asymmetry E3's
one widening below the shelf settles), which tst_dap's
`dapReverseRelayHonestDefaultAndGlueFirstTurn` (:1270) rides at :1286 while
dapfake's `reverse` mode self-audits that answer (success:true + `body.ok`,
main.cpp:292-308 — any lie exits 2). The generic-signal precedent is PLAN's own
car-4 ruling: ONE generic `dapEvent` and ONE generic `dapRequest` because
"per-event signals would scale binding cost with the spec". The nine
`FeatureHandler` doors' ONLY in-tree users are tst_lsp
(:2920/:2964/:3013/:3444) and `tools/probe_clangd.cpp`
(:185/:206/:247/:292/:407) — ZERO call sites in `src/app` (VERIFIED by grep),
so S2's mirror needs no feature path, and this car adds none without the
showcase losing anything. The bequeathed alias-paint question is one comment
(DapHub.cpp:434-440: "this door returns the OWNER view — so the gutter dots
paint there, not in every alias view. Unlike the LSP feeds, DAP's decoration
table has no per-uri fan-out door to widen"), while
`ProjectSession::Entry::views` already carries "EVERY live view over the one
document" (ProjectSession.hpp:134-141) and the only public door is the
OWNER-only `editorForUri` (:119). DapHub's paint sites are six:
`setBreakpointLines` (:587), `setDiagnostics` (:599), `setPosition` (:701),
`setHighlightedLines` (:704), `setInlayHints` (:803) and the teardown pair
(:810-811), with `stopEditor_` the single band owner (:705/:749/:809) and
`clearAllFeeds` re-resolving per uri (:836-846).

Stub reconciliation (the pre-existing car-E stub named things the code
refuses, and the record says so rather than quietly dropping them):
(a) it USED to open "The G3 audit's 16 silent drops shrink" and promise
the "16-drop baseline in AGENTS is REWRITTEN" — the stub is rewritten in
place at E5's close commit (it never reached git history), because that
baseline was never a line budget (the real counts, read from the archived
logs: 26 glue lines at 1.10.0, 31 at 1.11.0, 35 at 1.12.0, of which 17 are
public doors), so the rewrite is FIGURE-honest and the genuine shrink is
`DapHub` crossing with 2 public drops on the run that ships (its block is
9 log lines: those 2 + 6 private-seam bookkeeping + `tr`), NOT a bulk
bridging of the LspHub family (E4 names what stays);
(b) its "the nested plain-Qt payload records" — `DapHub` has NO public nested
record: both its structs (`BrEntry` :190, `BoundSource` :198) sit under
`private:` (:189), so the binding grows ZERO value-types (unlike the LspHub's
`ServerSpec`) and the three-copies growth is one `object-type` + one header +
one `_generated` TU name. The stub's other promises ride verbatim: the answer
routes get a SIGNAL plus a plain-typed door (the SIGNAL halves exist already —
E3 adds only the doors), the three copies grow again and the `--codegen` audit
is mandatory, `setTransportFactory` stays an honest C++-only absence, and the
smoke's one-Qt proof re-pins (`_smoke_checks.py:142` Core, :225 Network — the
helper dedups on the mapped PATH, so "one" means one on-disk file, already the
truth since G3, nothing to fix there).

E1 THE DAP FEEDS FAN OUT OVER THE VIEW LIST (glue + one additive ProjectSession
door; pure C++ — the wheel does not move yet) — the bequeathed question
ANSWERED, not deferred, and placed FIRST on purpose: binding `DapHub` (E2)
freezes what Python sees, so the alias behavior must be right BEFORE the door
list crosses. `ProjectSession` grows `QList<qmn::view::CodeEditor*>
editorsForUri(const QString& uri) const` — the sibling of `editorForUri`: EVERY
live view, OWNER first, attach order, car D's handover already re-keys it; the
door is additive and stays C++-only by intent (its container-of-QObject-pointer
either wraps or lands in E2's re-pinned log — the log decides, the record
follows). `DapHub`'s `BoundSource::editor` becomes `views` (a
`QList<QPointer<CodeEditor>>` LAST-KNOWN list, re-resolved through the session
whenever the session answers — the same "nothing is cached from the map" rule
as today), and `stopEditor_` becomes the list it always meant to be
(`stopViews_`). Pinned semantics:
- The band (`setHighlightedLines`), the value hints (`setInlayHints`), the
  gutter dots (`setBreakpointLines`) and the dap diagnostics (`setDiagnostics`)
  land on EVERY live view of the document. The hint MATH runs ONCE per stop
  (the word scan rides document text, not view state) and the SAME hint list
  feeds every view — N feeds, not N scans.
- The CARET door (`setPosition`, DapHub.cpp:701) rides the OWNER view ONLY,
  recorded as a divergence with its reason: caret/selection is per-view state
  and a stop must not yank the caret of a view the user is typing in; the owner
  is where the debug action came from, and after a handover the next view owns
  the caret.
- The single-writer teardown rule (`setInlayHints({})`, :811) is unchanged, run
  per view. `handleScopes`/`handleVariables` take the list (private seams; they
  drop from the log as private noise, not as a Python-visible change).
- `BoundSource::editConn` stays exactly ONE connection per uri onto the
  DOCUMENT's `textChanged` (document-scoped since car D): a second view's
  `setBreakpoints` re-anchors it onto the same document (same identity, still
  one connection) and NEVER doubles it — a doubled connection would double-send
  `setBreakpoints`, and dapfake's `breakpoints` mode exits 2 on wire drift, so
  the far side is the oracle.
Tripwires (tst_dap, `QTEST_MAIN` over REAL editors, the D3 shape): two views of
one document opened under one uri (the alias join is car D's rule, walked in
tst_glue/tst_dap already) — dots AND dap diagnostics read back through BOTH
editors' decoration doors after an answered `setBreakpoints` (no pixels, state
only); a stopped feed lands band + hints on both views and the caret on the
owner only; a JOINED view dying mid-stop keeps the survivor's feed intact
(handover), the LAST view's death clears the store; the debounce resend burst
still arrives as EXACTLY TWO completed `setBreakpoints` calls (the setup push +
the one resend): dapfake's `disconnect` arm is the FLOOR — it exits 2 unless at
least two arrived, so it proves the resend landed (main.cpp:441-444) — while a
doubled connection showing up as THREE is a NEAR-side count, and the machine
pins that floor and that ceiling honestly on each side. One MORE leg the
design did not name but the resolution order needs: a THIRD view joins AFTER
the last feed (the store never saw it) and the NEXT feed — a `breakpoint`
EVENT, which costs no push of its own — paints it too; that is the
session-answers-first half of `liveViews` pinned as behavior, not as plumbing
(the store-list fallback would leave that gutter blind). CARET correction,
made by the machine that rides it (E1's re-read, not the car-start record's
first claim): the single-view caret pin DOES already exist —
`dapHubStoppedFeedPaintsStackAndValueHints` reads `ed.position()` (line 2 /
col 5 of the stopped frame) at tst_dap.cpp:1605-1606; my car-start grep
looked for "cursor" and the pin spells it `position()`, so the "no pin
today" sentence was FALSE. What E1 actually ADDS is the two-view OWNER-ONLY
pin (the stop moves the OWNER's caret and leaves the joined view's caret
exactly where its user left it) plus the handover pin (after the owner dies,
the SURVIVOR's caret rides the next stop). The `editorForSourcePath` CAR-D
DIVERGENCE comment is RETIRED by this machine (a machine owns its own
comments) and AGENTS's divergence bullet gets rewritten in the same commit
(recorded divergences die in the machine that kills them; the car-D CHANGELOG
and close-record sentences stay — they recorded what shipped at 1.12.0).

E2 `DAPHUB` CROSSES THE WHEEL (the three-copies rule grows — and one
`src/dap`/`src/lsp` capture-order defect that the new smoke leg CAUGHT and
this machine KILLED, paragraph "FOUND AND KILLED" below, so "bindings" is not
the whole of it) —
the typesystem's `<namespace-type name="glue">` grows `<object-type
name="DapHub">` + its `<include file-name="glue/DapHub.hpp">`;
`bindings/CMakeLists.txt` grows `HEADERS` (after the two glue headers,
:122-123) and `_generated` (after the four glue TU names, :89-92) with the
PREDICTED `qmn_glue_daphub_wrapper.cpp` — PREDICTED from the pattern (the
namespace TU `qmn_glue_wrapper.cpp` already exists, and DapHub adds no
value-type, so exactly ONE new TU), CONFIRMED against a `--codegen` file list
BEFORE any compiling — the script ALREADY prints that list (`find /tmp/gen
-type f | sort`, build_in_container.sh:54), so the ritual costs one read, not
new machinery (the 1.6.1 "specified but not defined" configure error and the
1.7.0 "clean configure, no build, `undefined symbol: init_qmn_glue_…` at
import" trap are both killed by that confirmation, and the G3 comment at CMake
:83-88 is the record of it); `build_in_container.sh --codegen`'s
hand-reproduced header list (:48-51, the glue pair on :51) grows the same
header. The shim grows `DapHub = _ext.qmn.glue.DapHub` + an `__all__` entry + a
paragraph in the docstring that lists the honest absences (it is where the G3
"plain objects the app builds and owns" text lives). No new record type, no new
Qt module, no link change (the absorption already rides the shelf).
Pin-the-reality knots, decided by the RUN, never assumed: (a) the two container
crossings — `QVector<int>`↔`list` is PRECEDENTED (the M5 gutter doors, see the
code facts) so it is a RE-PIN that the DapHub breakpoints read back as the same
list, while `QMap<int,int>`↔`dict` is the genuinely new shape, expected to ride
PySide 6.12's generic container conversion; if the log rejects either, the fix
is a typesystem `container-type` / `convert-to-python-object` entry on the
DapHub type, NOT a door change. RUN FACT (the 1.12.0-era `--codegen` run for
this machine, and the smoke that exercises them): NEITHER was rejected —
`QVector<int>` reads back as the same `list` and `breakpointMap` answers the
empty `dict`, and NO container entry was needed; (b) whether `editorsForUri`
wraps or drops — RUN FACT: it WRAPS (the door never appears in the rejected
list, so `QList<CodeEditor*>` and its `CodeEditor*` elements cross), and the
smoke CALLS it, because a wrapped door whose return-element conversion never
runs is still unproven. The honest Python shape is a ONE-element list, and the
reason is worth saying in the same breath as car D's rule: a two-view JOIN
needs the attach ctor `CodeEditor(CodeDocument*, QWidget*)`, which stays
`remove="all"` (car D's lifetime-pin question), so Python cannot build a
joined pair until a later car opens that door; (c) `client()` and
`setTransportFactory` must be ABSENT (drop-by-type-absence, the G3 convention:
NO `remove` rules, so the log stays a live oracle of drift) — RUN FACT: exactly
those two public doors are in the log, and the smoke's `hasattr` loop asserts
them absent; (d) the new DapHub PRIVATE seams land in the log as private noise
— they are not Python-visible changes and the count separates them from the
public set: the DapHub block of the log is NINE lines, six of them private
bookkeeping (`handleScopes`, `handleStackTrace`, `handleVariables`,
`liveViews`, `boundForUri`, `findBound`) plus `tr` noise, and only the two
public doors above are crossings-that-never-happened. The TU-name prediction is
CONFIRMED the same run: `qmn_glue_daphub_wrapper.cpp`/`.h` in the file list,
exactly ONE new TU. Order
honesty, said plainly: at THIS machine the answer doors (E3) do not exist yet,
so E2's log is the shelf's shape WITHOUT them; E4's run is the one that carries
the doors across and the one whose numbers E5 accounts, and nothing shipped is
affected because the wheel THAT SHIPS is built at E5 — while E2's own DoD still
demands the repaired-wheel smoke+parity proof, so an INTERIM 1.12.0-version
wheel is built here too (named in the DoD paragraph below; E5's close replaces
it). Smoke
(the shared `_smoke_checks.py` leg, BOTH drivers — no adapter ever RUNS: the
container has no lldb-dap/gdb, the tst_glue NO-SERVER discipline rides, and the
one spawn the leg makes is REFUSED at execve, so it is a death, never a
session;
build `qmn.DapHub()`; `setBreakpoints` on a Python editor whose document has NO
uri answers `False` (the identity rule — `resolveEditorUri` takes the
DOCUMENT's uri first, so no session is even needed, DapHub.cpp:323-335 with the
door at :357); after `editor.document().setDocumentUri(...)` (the CodeDocument
door, CodeDocument.hpp:167 — the smoke already walks this exact door at
_smoke_checks.py:105) it answers `True`, `requestedBreakpoints` reads back the
SAME list (the `QVector<int>` pin) and `breakpointMap` reads back a dict (empty
before any answered reply — the `QMap<int,int>` empty case, pinned not
assumed); `attachProject` + `project()` round-trip the Python `ProjectSession`
(the non-owning QPointer crosses back, the hub/session precedent); all nine
signals exist as PySide attributes and connect; `startDebugging` against a
nonexistent program answers `True` (an async spawn is not yet a verdict) and
the REFUSAL then lands as `sessionEnded("transport never opened: StartFailed:
<Qt errorString>")` — pinned as OUR prefix plus a NON-EMPTY Qt tail (the Qt
text is version-dependent and does not name the program), with
`sessionStarted` never firing and `isDebugging()` back to `False`;
`stopDebugging`/`isDebugging`/
`isReady`/`adapterCapabilities` all answer honestly with no session (`{}`
before the handshake); and `client`/`setTransportFactory` do NOT exist
(`hasattr` probes — the G3 honest-absence loop grows), and the fan-out door
`editorsForUri` is CALLED (its `QList<CodeEditor*>` crossing, one element, the
reason it cannot be more named above). The one-Qt proof grows:
DAP must add NO second `libQt6Core` and no NEW module (the `.so` already needs
QtNetwork through protocol) — if `/proc/self/maps` shows anything else, an
auditwheel exclude moved. RUN FACT: the leg measures a REAL before/after diff
of the `/libQt6*` lines across the whole DAP block, and the delta is `[]` on
both drivers (build tree and repaired wheel). Test-authoring trap the leg hit
first, worth recording because it blurs the very thing it measures: pumping the
spawn verdict with `PySide6.QtTest` loads `libQt6Test.so.6` INSIDE the
measured window, and the delta then reports the test's own import — the pump
uses stdlib `time.sleep`.

FOUND AND KILLED at this machine (why the heading no longer says "bindings
only"): the new spawn-FAILURE leg refused to pass, and the refusal was a real
defect, not a bad pin. `Transport::transportError`'s FIRST slot is RpcClient's
own `onTransportError` (connected in its ctor), whose `markGone()` emits
`transportGone` SYNCHRONOUSLY — so `onGone()` runs NESTED INSIDE that
`transportError` emission, at THAT signal's slot order. Both clients connected
their `lastTransportIssue_` capture BELOW the `new RpcClient(...)` line, so the
capture was the SECOND slot and every ProcessTransport StartFailed built its
reason from an EMPTY `lastTransportIssue_`: `transport never opened: ` with a
bare tail. The comment at both sites claimed the opposite ("Connected BEFORE
transport->start(): the StartFailed error fires synchronously inside start()
and must already be captured") — true about `start()`, useless about SLOT
order, and the DAP path is not even synchronous there: `start()` returned true
and the verdict arrived later as `QProcess::errorOccurred`. Why no test ever
saw it: the only existing pins on these strings asserted NON-EMPTY (`tst_dap`'s
crash slot: `QVERIFY(!reason.isEmpty())` — the bare prefix satisfies that) or
asserted state and spawn count and never read the string at all (`tst_lsp`'s
bad-program slot). It started to matter the moment E2 made the reason
PYTHON-visible as `qmn.DapHub.sessionEnded`. E2 killed it: the same 3-line move
in `src/dap/DapClient.cpp` and `src/lsp/LspClient.cpp` (the capture connect
goes ABOVE the `new RpcClient` construction; both layers are C++-only, no wire
change, no public door touched), plus the missing `ServerDied` case in
`DapClient::errorKindName` — the LspClient twin already named it, the DAP
switch fell through to `Unknown`, and the most common adapter death of all
read `transport closed: Unknown: process exited with code 7`. Both clients were
fixed together on purpose: leaving the LSP twin broken would have made two
libraries behave differently under one identical comment. Pins, owned by this
machine: `tst_dap` gains
`dapHubSpawnRefusalReasonCarriesTheTransportIssue` (NO adapter involved —
execve refuses, nothing spawns, the no-spawn rule stands — prefix pin,
non-empty-tail pin, never-Ready-so-never-`sessionStarted`, no stderr, and the
post-death doors answer honestly), `tst_dap`'s crash slot trades its vacuous
`!isEmpty()` for the exact string `transport closed: ServerDied: process exited
with code 7`, `tst_lsp`'s `lspBadProgramPathFailsWithNoRestartLoop` grows the
same prefix + tail pins, and the wheel smoke pins the same shape end-to-end
through `qmn.DapHub`. Both halves of the pin are deliberate: the
`errorKindName` prefix is OUR stable vocabulary, the Qt errorString behind it is
not.
DoD: `--codegen` clean + its file list confirms every
`_generated` name; build-tree smoke AND repaired-wheel smoke + qtmonaco parity
green; the rejected log captured as an INTERIM baseline — E4's run, which wraps
the E3 doors too, is the log E5 accounts against, and the delta between the two
is named by E4's run (it also answers `answerRequestError`'s defaulted tail).
MET at this machine (2026-11-25 runs): the two drivers and the parity test are
green in both shapes (build tree, and the repaired wheel installed into a
scratch prefix with the repo off `sys.path`); the interim codegen log is
`/tmp/opencode/codegen_e2.log` (scratch, not in the repo). One thing this
machine did that the close ritual must not forget: proving the repaired-wheel
half meant BUILDING a wheel here, so `dist/` now holds
`qmn_native-1.12.0-cp310-abi3-linux_x86_64.whl` and `dist/repaired/` the
repaired `…-manylinux_2_34_x86_64.whl` — both still at the PRE-bump 1.12.0
version, and both are interim artifacts the E5 close replaces (the 1.9.0 drift
lesson is exactly this: a stale wheel in `dist/repaired/` plus a pydemo path-dep
that spells a gone version). Because that rebuild REPLACED the 1.12.0 wheel
pydemo's path-dep points at, `pydemo`'s headless run was re-greened here too
(`uv run --offline main.py --exit-after-ms 400`, exit 0) after refreshing the
gitignored `pydemo/uv.lock` — uv stores the wheel's hash, so a rebuilt wheel at
the same path makes a stale lock FAIL to install, not merely warn.

E3 THE ANSWER ROUTES GET THEIR PLAIN-TYPED DOORS (glue + tst_lsp/tst_dap legs;
pure C++ — the far side is the oracle) — this is the half the G3 record left
written-but-unbridged — "the route MUST speak a SIGNAL plus a plain-typed
answer door" (the car-E stub, :3682-3684) and the G3 record's half of the same
story: "its ANSWER route (`clientForSpec()->respond...`) does not: the
LspClient default answers for Python apps; documented, not bridged"
(:1655-1656). Decided here, from the code: - **The client-layer doors are
ALREADY plain Qt, but they are VOID** — `respondApplyEdit(QJsonValue id, bool
applied)` (LspClient.hpp:112) and `respondShowMessageRequest(QJsonValue id,
QJsonValue actionItem)` (:118-119) both `return;` early on a dead session and
otherwise throw away `session_->rpc->respond(...)`'s bool;
`DapClient::respond`/`respondError` (DapClient.hpp:133-134) ALREADY return
bool. So a SHELF door that speaks the truth needs exactly ONE widening below
the shelf: `respondShowMessageRequest` grows `bool` (`return
session_->rpc->respond(...)`; the null-session early return becomes `return
false`). That is an `src/lsp/` internal, NOT the frozen view/glue surface — the
layer is C++-only and never wrapped — and it is source-compatible for every
existing caller, which all discard the value (`LspHub.cpp:3210`'s applier,
tst_lsp:1574/:3808; VERIFIED: no `&LspClient::respond*` connect-by-pointer
anywhere). `respondApplyEdit` stays `void`: its only caller is the hub's own
applier, which self-answers, and E3 adds no applyEdit twin. What the shelf adds
is THREE doors, no new record type: `bool
LspHub::answerShowMessageRequest(const QString& specName, const QJsonValue& id,
const QJsonValue& actionItem)`, and on `DapHub` `bool answerRequest(const
QJsonValue& id, const QJsonValue& body)` plus `bool answerRequestError(const
QJsonValue& id, int code, const QString& message, const QJsonValue& data =
{})`. Every parameter is the same type family the G3 run already shipped across
(`QString`/`QJsonValue`/`int`/`bool`). - **The LSP door is spec-named, not
uri-named** — and that is a wire fact, not a taste: `window/showMessageRequest`
is a SERVER→client request, so only the wire that asked can carry the answer,
and the hub's relay signal already hands out the spec name
(`serverShowMessageRequest(specName, id, params)`, LspHub.hpp:456 as it
reads after this machine). The
door consumes exactly what the signal emits, resolves through `specEntry`
(LspHub.hpp:474 — CORRECTED at the run: the door resolves through the
lazy-start pin's READ side, `clientForSpec`, the relay's own way back, and
`specEntry` was deliberately NOT used: it CREATES and lazy-STARTS a spec it
only wants to reply to, and an answer door that can spawn a server is its own
bug class. The FAILED-entry case stays honest either way — a failed client
has no `session_` (LspClient.cpp:175 nulls it at teardown), and both respond
doors guard on `!session_`, so the door answers false) and forwards to that
client's respond door. A uri-keyed
variant would be a door that can answer the WRONG server. - **`applyEdit` gets
NO twin — recorded decision with its reason.** The shelf's applier answers that
request itself, always: `LspHub.cpp:3210` (`client->respondApplyEdit(id,
report.applied) // the truth, either way`), and `RpcClient`'s erase-once guard
(`openIncoming_.erase` inside `respond`/`respondError`, RpcClient.cpp:204-221)
makes a SECOND answer to one id physically impossible. A Python door that can
never win is worse than an honest absence, and the report the hub hands its C++
caller instead (`ApplyReport`, LspHub.hpp:387-392) stays a type the wheel never
carries — E4's drop accounting pins why. `respondApplyEdit` keeps its `void`
signature: the widening lands only where a bool route actually exists. -
**First-answer-wins now binds the SHELF route too, and no new policy is
minted** (AGENTS's ask answered in the open): the client-layer half is settled
— `dapReverseRelayHonestDefaultAndGlueFirstTurn` (tst_dap:1270, answers at
:1286) on the DAP side,
`showMessageRequestFirstAnswerWinsAndNullDefaultFollows` (tst_lsp:3768, answers
at :3808) on the LSP side — and the mechanism already enforces it one layer
down: `openIncoming_` registration happens BEFORE the handler runs
(RpcClient.cpp:401/:425/:432) and erase-once refuses the second. The hub doors
inherit it; the shelf adds no re-answer bookkeeping. - **The DAP id is the
request's `seq`** — `dapRequest(QJsonObject)` relays the whole request frame,
and the answer doors take the `seq` value out of it. A stale id never reaches
the codec: `RpcClient::respond` looks the id up in `openIncoming_` first and
answers false on an unknown or already-answered one, and only the surviving
frame goes to `DapCodec::buildResponse` (the codec's `baseResponse` re-read
reads that same registered frame, it is not a second lookup). Tripwires,
far-side first: tst_lsp grows the hub-door version of the two
`showMessageRequest` legs it already owns (:3768 onward — the
`MSGREQ-OK`/`MSGREQ-NULL-OK` markers stay the oracle, picked out of the
server's own `actions` array) PLUS a second-answer leg that pins the widened
bool near-side (`True` on the first answer, `False` on the second — the very
refusal `RpcClient::respond` already makes, whose comment reads
"unknown/already-answered: double answers are never sent") while its far side
sees exactly ONE answer frame; tst_dap grows a DapHub-driven reverse leg that
reuses dapfake's `reverse` mode UNCHANGED (its self-audit already validates
request_seq match, `success:true` and `body.ok`, and exits 2 on a lie,
main.cpp:292-308; `runInTerminal` keeps the shelf's honest default and
`progressStart` stays the client-layer static shelf — a hub leg must not
disturb either) plus an `answerRequestError` leg, whose envelope shape is
dapfake-audited through `DapCodec::buildErrorResponse` (DapCodec.cpp:66-86: the
whole error TABLE merges into `error`, so `code`→`error.id` and
`message`→`error.format`). No new dapfake mode is planned — if the run finds
one is needed, the close record says so rather than pretending.
MET at this machine (2026-11-25 runs): the three doors landed exactly as
specified, no new record type, and the ONE widening below the shelf is the
`void`→`bool` on `LspClient::respondShowMessageRequest` (declaration now
LspClient.hpp:122, the comment block above it carrying the reason for the
widening). Two things the run DECIDED beyond the stub, both from the code:
- **The `specEntry` sentence above is corrected in place** (the lazy-start
  trap — the door resolves through `clientForSpec`, the read side). A test
  pins the consequence: `showMessageRequestAnswerDoorRefusalsHaveNoSideEffects`
  calls the door with a NEVER-STARTED spec name and `clientForSpec` still
  reads `nullptr` for that name immediately afterwards — an answer door that
  spawns is caught red-handed, not just commented away (the same phantom
  call repeats mid-relay and still refuses).
- **No new dapfake mode was added, but the `reverse` mode's arms GREW — that
  is the sentence this paragraph owed the reader.** The stub said the tst_dap
  legs reuse the mode "UNCHANGED"; the run found step 1 (`startDebugging`)
  could not witness BOTH answer shapes, because the D2 slot's
  success:true/body.ok and the client layer's honest
  `error{id:0, format:"not handled: …"}` default were the only two spellings
  the arm knew, and an app-answered ERROR from the hub door would have read
  exactly like the default's refusal. The fix is a DISCRIMINATOR, not a mode:
  only the shelf door can put `error.id == 9001` on this wire (the client
  default invents `id:0` and the static shelf never sees this request), so
  the step-1 arm now accepts either shape and audits the error TABLE when it
  sees 9001 — the door's message in `error.format` and the OBJECT-data merge
  (`appId:"daphub-door"`, `DapCodec::buildErrorResponse`, DapCodec.cpp:66-86,
  read at this run) landing as members of the `error` table itself. The 9001
  branch sits FIRST and the original success:true/body.ok checks follow it
  verbatim (the tst-side D2 slot itself is untouched and stays green). A
  second dapfake growth came with it: `disconnect` in `reverse`
  mode now EXITS 2 if the battery never reached step 2 (main.cpp:489-496) —
  an early client exit silently skipping the answer legs would have made the
  mode's whole purpose vacuous.

Tripwires shipped (far side first, all read out of the code after the run):
tst_lsp's `showMessageRequestFirstAnswerWinsAndNullDefaultFollows` (now
:3785) grew the shelf-door block — answering INSIDE the relay through
`hub.answerShowMessageRequest`, pinning the widened bool near-side (`True`
first answer, `False` second) while the far side still sees exactly one
accepted answer (`MSGREQ-OK`, and no `MSGREQ-NULL-OK`), so the second frame
provably never travelled — and the new slot
`showMessageRequestAnswerDoorRefusalsHaveNoSideEffects` (:3947) pins the
refusal half: an unknown id answers false WITHOUT burning the real request (the Null default still answers it,
`MSGREQ-NULL-OK` arrives, the session stands). tst_dap's no-session sweep
(`dapHubRefusesEverythingWithoutAnAdapter`) grew both DAP answer doors
reading `false` with no wire at all — no local failure frame is invented —
and two new slots drive the reverse battery at the SHELF layer:
`dapHubAnswerDoorWinsTheReverseRaceAndTheShelfStands` (id = the relayed
frame's own `seq`; first answer wins, second refused; `runInTerminal` stays
the client-layer default's leg, `progressStart` stays the static shelf and
never reaches the relay — the D2 pin, proven one layer up; dapfake's exit 0
is the far-side "that is exactly the answer set I promised") and
`dapHubErrorDoorTravelsWholeErrorTable` (the 9001 leg: dapfake validates the
envelope end-to-end; the post-error success-shaped re-answer reads false and
writes nothing; and an in-envelope failure is NOT a death — `isDebugging()`
stands). One more shape decision lives in the code, not above it: the DAP
answer doors carry NO lifecycle state gate (unlike the forward `request`
door) — an answer is judged by `openIncoming_`, and inventing a
`kErrNotRunning`-shaped frame for a reverse answer would be a wire lie; the
`!client_` guard is the only near-side filter. The fakeserver's MSGREQ arm
comment was updated to say the answer may ride either door (markers
byte-identical — the far side cannot tell the doors apart, which is the
point). `answerRequestError`'s defaulted tail waits for E4's codegen log, as
the paragraph above promised. Runs: full ctest 7/7 green (lsp 73.52 s, dap
18.92 s, total 101.83 s), both new tst_dap slots green standalone, both
showMessageRequest slots green standalone.

E4 THE ANSWER ROUTES CROSS, AND THE LIVE DAP WHEEL LEG IS RUN (bindings + the
drop-inventory accounting + the manual live ritual) — the doors E3 added are
public and plain-typed, so the codegen run MUST wrap them: the load-bearing
direction of the audit flips here. A NEW public plain-typed door that shows up
in `mjb_rejected_functions.log` is a REAL regression, and that check is the
only automatic tripwire this car gets on the wrappability rule. The smoke grows
a shape leg for the routes (no adapter is ever spawned in the container —
lldb-dap/gdb are not there and the tst_glue NO-SERVER discipline rides):
`hasattr` probes prove the three new doors and the nine `DapHub` signals exist;
`hub.answerRequest(4242, {"ok": True})` on a hub with no session answers
`False`, and `hub.answerShowMessageRequest("nope", 1, {})` answers `False`
(both doors return false on an unknown id/spec — the honest near-side answer
with no wire at all). The G3 smoke leg's own comment ("The hub relays cross as
ordinary PySide signals (the answer route `clientForSpec()->respond...` is
C++-only - documented)", `_smoke_checks.py:224-226`) is exactly the sentence E4
turns into a real probe, and it gets rewritten in the same machine (a machine
owns its own comments). The WIRE truth of the routes lives in C++ (far-side
markers), the same split G3 shipped for the shelf — the record says it, it is
not implied. **And because the whole point of the car is "DAP across the
wheel", the manual live ritual is part of this machine, not a nice-to-have**: a
scratch Python script (NOT CI, NOT the smoke) driving `qmn.DapHub` against the
REAL adapters `tools/probe_dap.sh` uses (log:52-53 — `lldb-dap` FIRST, then
`gdb -i=dap`), pinning `sessionStarted` → `setBreakpoints` → answered reply →
`debugStopped` → `stopDebugging`, read out of the live wire. The recorded
numbers belong to the close record, and a leg that cannot run is written as
"did not run", never as a pass. Drop-inventory accounting (the DoD the AGENTS
rewrite rides, every figure read out of the archived logs, not derived): the
1.12.0 run's rejected log carries **35 glue-wide drop lines, not 16** — 34 on
`LspHub` (17 public doors + one `tr` line + 16 more lines that are private
seams and their rejections: the struct-carrier noise
`SpecClient`/`TriggerState`/`FoldFeed`/`HintFeed`/ `PullDiagFeed`, the
`QMap::const_iterator` returns of
`registrationAt`/`interactionView`/`hidePopupsEverywhere`, `connectViewTaps`,
`featureGate`, `pullModeClient`, `wireDocumentPosition`, `wirePosition`) and 1
on `ProjectSession` (`tr`). The count GREW with the shelf, machine by machine:
the 1.10.0 log has 26 (25 + 1) and the 1.11.0 log 31 (30 + 1), and at 1.10.0
only EIGHT public doors carried `FeatureHandler` — car C's
`requestSignatureHelp` is the ninth. No baseline ever counted the private
lines, and AGENTS's sentence named the halves wrong. The two `tr` lines
(log:281-282) are NOT hub signals: they are the static `tr()` every Q_OBJECT
class carries, rejected by the generator's own bundled system rule — the log
prints that rule verbatim (`matches class "^.*$" and "^tr$"`), and no `tr` rule
exists anywhere in `bindings/*.xml` (VERIFIED) — so they are counted separately
from the 17, never as a lost door. The VERIFIED public 17, by name:
`setTransportFactory`, `syncerFor`, `clientForUri`, `clientForSpec`, the eight
`request*` doors
(`Hover`/`Definition`/`References`/`Completion`/`SignatureHelp`/
`CodeActions`/`Formatting`/`Rename`), `resolveCompletionItem`, and the four
`ApplyReport`-returning appliers (`applyWorkspaceEdit`, `applyCodeAction`,
`applyReplyEdit`, `applyTextEditList`). So the AGENTS sentence is wrong twice
over: the callback family is NINE doors (eight `request*` +
`resolveCompletionItem`, all carrying `FeatureHandler`), and the appliers do
NOT drop on "the defaulted `LspClient*` tail" — all five apply lines (the four
public ones plus the private `applyDecoded`, log:759-763) report the SAME
reason: `unmatched return type 'qmn::glue::LspHub::ApplyReport': Cannot find
type entry`. One typesystem `value-type` entry would carry all five, which is
exactly why adding it is a DECISION, not an oversight to fix on the way:
`ApplyReport` stays unbound this car, because the wheel has no door that
returns it to Python in any shape the wheel needs — the applier runs HUB-side
on a server-primed `workspace/applyEdit` and reports refusals through the
`applyEditRefused` signal instead (LspHub.cpp:3210 answers with the truth). The
nine feature doors and the applier family stay C++-only honest absences, in the
same voice as the `set_lsp_header` line: zero call sites anywhere in `src/app`
(VERIFIED by grep — the only users are tst_lsp at :2920/:2964/:3013/:3444 and
`tools/probe_clangd.cpp` at :185/:206/:247/:292/:407), and the wheel's user
gets completion/hover/signature-help popups from the HUB's own taps, never from
an app-driven feature door. G3's other deferred half stays deferred for the
same reason: no generic feature door is added, so `RpcClient::Reply` never gets
a door to carry it and never becomes a `value-type` (PLAN:1652-1654). What
genuinely shrinks is the shelf's crossing cost, and the number the record
carries instead of the 16-sentence it replaces: `DapHub` crosses with **exactly
2 drops** (`setTransportFactory`, `client()`) plus the generic `tr` line: the
run that ships is E4's, and by then the answer doors already exist as
plain-typed members, so nothing in the wheel depends on the E2-then-E3 order
(the wheel THAT SHIPS is built once, at E5 — E2 built an interim 1.12.0-version
wheel to prove its own repaired-wheel DoD, and E5's close build replaces it, the
same drift class the 1.9.0 audit named). That is the sentence AGENTS should own,
and
the new log must show it. One new signature shape that run must also answer:
`answerRequestError`'s defaulted tail (`const QJsonValue& data = {}`, mirroring
`DapClient::respondError`, DapClient.hpp:134-135) — the log decides whether a
Python-visible overload exists; if it reports the tail, the fix is an explicit
two-arity pair on the SHELF door (still plain-typed, still one class), never a
`remove` rule.

The stale figure's other homes, and what E5 did with each: the number 16
lived in PLAN at car B's close record, car C's pinned-expectation paragraph,
car C's CLOSED append record, car D's close paragraph (forward-looking half),
the car-E stub's opening lines, and the Gate-3 risk-2 line — and in AGENTS.
Read against the logs, "16 drops" was never a line budget — it was the NAME
of a set, and the car-B and car-C audits were SET-DIFFS ("that baseline plus
exactly `requestSignatureHelp`, nothing else"), which is exactly why they read
as true while the line count grew 26 → 31 → 35 underneath them. So the three
HISTORICAL shipped records keep their words (they recorded set-diffs
honestly, and rewriting a shipped close record is its own dishonesty) while
the FORWARD-LOOKING sentences — car D's close paragraph, the car-E stub
opening, the risk-2 line — were rewritten IN PLACE at E5's close commit (none
of those three had ever reached git history), plus AGENTS, all pointing here
for the figures.
MET at this machine (2026-11-25 runs): the flipped audit ran BOTH ways and
held. The `--codegen` run (exit 0, log `/tmp/opencode/codegen_e4.log`,
scratch) shows **no rejection line for any of E3's three doors** — they all
crossed as ordinary wrapped members — and the glue-class drop set is
BYTE-IDENTICAL to the 1.12.0 baseline: a diff of the two logs' `qmn::glue::`
lines is EMPTY (34 `LspHub` + 1 `ProjectSession` + 9 `DapHub`, the DapHub
block still its six private-seam bookkeeping lines + `tr` + `setTransportFactory`
+ `client()`). No new `_generated` TU (the glue file list is unchanged) and no
three-copies edit, m7's precedent holding: plain-Qt-typed doors on an
ALREADY-bound class. The defaulted-tail question this paragraph posed got its
answer from the SMOKE, not the log: the log never mentioned the door, and the
plain 3-arity `answerRequestError(4242, 0, "msg")` call works from Python —
Shiboken carried the default itself, so the pre-authorized "explicit
two-arity pair" fix was NOT triggered and no shelf overload exists. Smoke legs
shipped (in `_smoke_checks.py`, growing E2's sections in place): the G3 relay
sentence rewritten to the crossed-answer-route truth plus
`answerShowMessageRequest` False on an unknown spec AND on a
registered-but-never-started spec (the lazy-start read-side half is pinned in
C++ by tst_lsp's `showMessageRequestAnswerDoorRefusalsHaveNoSideEffects`; the
Python side cannot see `clientForSpec`, dropped — the smoke pins SHAPE, never
wire); the no-wire `answerRequest`/`answerRequestError` False legs; and a
DEAD-session leg on E2's spawn-refusal corpse — the hub does NOT reset
`client_` at session end (only the next `startDebugging` replaces it,
DapHub.cpp:123), so that leg's False provably comes from the client's
`!session_` guard one layer down, read through the wheel. Container runs:
full pipeline EXIT=0 (build-tree smoke + qtmonaco parity green, Qt delta
`[]`), `--wheel` EXIT=0 (the interim 1.12.0 wheel REBUILT with the doors and
the grown smoke, repaired, then smoke + parity against the repaired wheel
with the repo off `sys.path`, Qt delta `[]`; `dist/repaired/` again holds an
interim wheel E5 must replace — the recorded drift class, and pydemo's venv
had to re-hash `uv.lock` for the third time for exactly that reason).
THE LIVE DAP WHEEL LEG RAN, both adapters, host-side through the pydemo venv
(no new install — PySide6 + the wheel path-dep were already there), scratch
script `/tmp/opencode/dap_wheel_ritual.py` (logs `e4_ritual_lldb.log`,
`e4_ritual_gdb.log`): PLAN's five anchors on the live wire, lldb-dap FIRST —
Ready with **28 caps fields**, `setBreakpoints [4]` answered with
`breakpointMap {4: 4}` and the verified dot at model line 4, `debugStopped
(thread 2751115, "breakpoint")`, the hub's caret feed agreeing at line 4,
`stopDebugging` riding `sessionEnded("terminated")` — and gdb -i=dap SECOND:
**26 caps fields**, `{4: 4}`, thread 1, `"breakpoint"`, caret 4,
`sessionEnded("stopped")`. WHEEL-DAP-RITUAL-OK, exit 0, twice. Two honest
printings the record keeps: NEITHER live adapter relayed a single reverse
request that session (`dapRequest` fired zero times both runs — the answer
doors' LIVE exercise therefore did not happen; their far-side audit stays
dapfake's, in C++, and the wheel-side legs pin only the refusal shapes — the
pinned five anchors never asked for a reverse leg and none was faked), and
the first lldb-dap attempt died in the SCRATCH script's own slot bug
(`sessionStarted` takes NO args; `list.append` crashed as a swallowed PySide
slot exception) — no repo code involved, the fixed script is what both OK
runs used. The ctest half of this machine is the E3 run's (no C++ changed
after the 7/7 run: 101.83 s); E4 touched only the smoke file and this record.

E5 THE CLOSE RITUAL (both wheels + the shelf literal + the record half) — the
full car-D close list, plus the half the 1.9.0 close forgot: bump BOTH package
literals to **1.13.0** (`CMakeLists.txt` + `pyproject.toml`) in one commit,
bump `QMN_GLUE_VERSION` to **1.1.0** and rewrite its comment (the "bumps ONLY
when the shelf itself breaks" wording is pre-crossing: since G3 a shelf break
is wheel-visible, so additive growth is a minor bump); `ninja -C build` +
`ctest` **7/7**; the packaging smoke (`cmake --install` + `examples/consumer` —
its glue TU grows the DapHub door read so the INSTALLED shelf is proven, the G1
rule); rebuild the wheel in the container (`--codegen` clean + the confirmed
file list, build-tree smoke, `auditwheel repair`, repaired-wheel smoke +
qtmonaco parity with the repo off `sys.path`); bump the pydemo path-dep to the
new wheel and run the headless green pydemo (`QT_QPA_PLATFORM=offscreen uv run
main.py --exit-after-ms …`) — that run is what catches a resolver drift, and
pydemo grows NO new door this car (the `--lsp`/`--dap` mirror doors are S2's);
re-run the live probes whose ground this car touched: `tools/probe_dap.sh`
against BOTH adapters (the ritual car 4 made mandatory, and E1 changed the
breakpoint paint it pins), and
`tools/probe_clangd.sh` as a REGRESSION guard only — its steps exercise none
of the new answer door, so the fakeserver markers stay that door's only wire
proof and the close record says so; the clangd step count gets re-read from
the run, not restated from AGENTS. Then update README/CHANGELOG/AGENTS (the
drop baseline, the alias-paint divergence being retired, the "one document,
many views" DAP bullet; the bench-hygiene paragraph rides unchanged) AND the
car-E-facing 16-figures PLAN itself carries forward — car D's close
paragraph, this car's own stub opening and the Gate-3 risk-2 line get
rewritten in the same commit, E4's paragraph being the record of the
real counts;
annotated tag `v1.13.0`, push main + tag — fresh ask before each out-of-tree
step.
MET at this machine (2026-11-25 close): the full ritual ran green. ctest 7/7
at the BUMPED head (101.70 s; `lsp` 73.46, `dap` 18.94). The packaging smoke
grew as this paragraph promised — `glue_smoke.cpp` now READS the three E3
answer doors through the INSTALLED shelf, and the LspHub leg re-asserts
`clientForSpec("clangd") == nullptr` right after the answer-door call (the
installed shelf's answer door does not spawn clangd either): both consumer
binaries green, no server and no adapter spawned. The container trio:
`--codegen` exit 0 (the drop set again 34 `LspHub` + 1 `ProjectSession` + 9
`DapHub`, ZERO rejection lines for the three doors, and the wrapper-TU file
list byte-identical to E4's run), the no-flag full pipeline exit 0 (build-tree
smoke + qtmonaco parity, Qt delta `[]`), and `--wheel` exit 0 — the shipping
wheel is `qmn_native-1.13.0-cp310-abi3-manylinux_2_34_x86_64.whl`,
rebuilt/repaired/smoke-green/parity-green with the repo off `sys.path`.
pydemo's fourth literal re-pointed to the 1.13.0 wheel, its lock re-resolved
(the wheel-hash trap bit AGAIN at exactly this bump — refresh the lock after
any rebuild), headless run exit 0, and pydemo grew NO new door this car (the
`--lsp`/`--dap` mirror doors are S2's). Live probes re-run at the head:
`tools/probe_dap.sh` PROBE-RITUAL-OK, 7 steps both adapters agreed (lldb-dap
**28** caps fields, gdb **26** — the same cap counts E4's wheel ritual
printed, C++ and Python reading the same adapters), and `tools/probe_clangd.sh`
PROBE-OK, **11 steps** on clangd 23.1.5, re-read from the run not restated —
and NONE of clangd's eleven steps exercises the answer door, exactly as this
paragraph predicted, so the fakeserver's `MSGREQ`/`MSGREQ-NULL-OK` markers
stay that door's only live-wire proof. E4's live-wheel legs ran against the
INTERIM wheel (1.12.0-spelled, carrying all of car E's code); the shipping
1.13.0 wheel carries the same crossing surface and the close trio above is
what proves it. `QMN_GLUE_VERSION` rode 1.0.0 → 1.1.0 with its comment
rewritten at the bump site (the first shelf bump ever — car D's close kept
1.0.0 deliberately). **CAR E CLOSED at 1.13.0**, as ONE commit (owner's call:
this car's design record never reached git history, so the record, the
machine run records, the code and the close ride a single commit — car D's
two-commit shape not repeated) plus the annotated tag. Car S1 (qmn-demo grows
into the live showcase) is the open car next.

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

**S1 DESIGN RECORD (owner-reviewed 2026-11-25; machines below are build order).**
The exploration before the record corrected three points of the stub above —
recorded here because they change the machines:

- **F7/F8 were never chords anywhere.** `nextChange()/previousChange()` are
  M10 DOORS; the "the F7/F8 nav the chords already carry" phrasing spoke the
  CHANGELOG's Monaco-parity naming (the `nextChange/previousChange` pair IS
  F7/F8 in the qtmonaco/Monaco world, and `nextChange`'s own comment says
  so), but no key handler in the view or the app mentions F7/F8 (grep-verified
  at 1.13.0). S1 ADDS the chord pair to the view's key-press switch (the
  fold/zoom chord-table precedent, M13/M17) — additive, no new door. A bare
  F7 today falls to the switch's default and is swallowed silently
  (function keys carry no printable text, so the insert arm never sees them).
- **The diff door's "two tabs" shape is the STRING door's shape.** The
  pointer door refuses an original carrying ANY views (the zero-view gate,
  `setDiffDocuments`, CodeEditor.cpp:2672) — and every demo tab's document
  is editor-attached — so the honest demo shape (owner's call): `--diff A B`
  opens THREE tabs — plain A, plain B, and a third DIFF tab built through
  the string door (its own document IS the modified side, caret space =
  modified), FOCUS ON THE DIFF TAB. Pins ride the diff tab.
- **The DAP control route is C++-only at 1.13.0** — wheel-probed, not
  assumed: `DapHub::request`/`client()` drop by type absence and
  `dapfake steps` answers ONLY `continue` (+`terminate`); a `next` frame
  exits it 2. EVERYTHING ELSE the showcase reaches already crosses the
  wheel (live 1.13.0 wheel probe: `highlightSpans`, `setDiffDocuments`,
  `nextChange/previousChange`, `isDiffMode`, `loadGrammar`,
  `registerGrammar`, `grammarAddress`, `setCaptureRole`, and the whole
  `DapHub` control face EXCEPT the request/client pair). S2's door-for-door
  mirror therefore needs typed control doors — see S1-3.
  (The pydemo CLI is ALREADY behind on `--lsp/--dap` — that catch-up is
  S2's committed flag list from its stub, NOT pulled into S1.)

Owner decisions (2026-11-25): typed control doors ON THE HUB (the shelf
grows Python-visible this car); three-tab diff shape with diff-tab focus;
tour pins EXACT on a frozen built-in corpus; dapfake grows the step arm.

### Machine S1-1 — the diff door (view chord + demo door + `--diff`)

- **View**: the F7 / Shift+F7 pair (Monaco's spellings; the record's F8
  never was a real Monaco key — `previousChange` is Shift+F7. Consumer
  check ran 2026-11-25: the qtmonaco sibling lives at
  `/home/kai/kaigit/PYQTP/packages/qtmonaco` (AGENTS's stated path is
  stale — a close-pass docs fix), and it carries NO keybindings of its
  own for the pair (its F7/F8 heritage lives in the minified bundled
  Monaco assets, unreadable here) — the chord is OURS to define, and the
  tour pin table below freezes the exact mapping) joins the
  key-press switch's command band (before the caret-motion section, like
  the fold chords), plain-key match, no modifiers beyond Shift; calls
  `nextChange()/previousChange()` unchanged. The doors stay DIFF-GATED
  exactly as M10 shipped them (`if (!diff_) return;`, CodeEditor.cpp:2752/
  2773 — the record's first draft claimed a mode-agnostic hunk axis and
  the code says otherwise; the pinned M10 semantics ride untouched), and
  the tripwire pins BOTH faces: hunks walked in diff mode AND the chord's
  honest no-op on a plain (non-diff) editor.
- **Demo**: `MainWindow::openDiffPair(pathA, pathB)` — `openPath` A,
  `openPath` B, then a third tab whose editor gets
  `setDiffDocuments(textA, textB, languageForSuffix(B))`; focus lands on
  the diff tab; the door NEVER touches the two plain tabs' state. The
  diff tab registers through B's REAL path (the session's one-uri
  refusal lands the duplicate tab; the tab TITLE reads `B ⇄ A` but the
  registry key stays honest). CLI `--diff A B` rides the door — and its
  shape took TWO Qt6 traps at build (2026-11-25, both MEASURED with
  standalone probes, both pinned by the process-level tripwire):
  (1) `QCommandLineOption` has NO two-value-name ctor — the four-arg
  braced `addOption({"diff", desc, "a", "b"})` spelling binds the 4th
  string as the DEFAULT VALUE; `--diff A B` collected [A] with B a
  positional and the exit stayed 0 — a SILENT dead door.
  (2) `QCommandLineParser::process(const QStringList&)` SKIPS the list's
  first element as argv[0] — a scan-rewritten list that forgets to
  prepend `argv[0]` drops its own leading flag (the self-quit timer
  vanished and the spawned demo hung). The shipped shape: a raw-argv
  PRE-SCAN (`--diff` + exactly two non-flag tokens, else stderr + exit
  1 — never a silent zero), the three tokens removed, `argv[0]`
  prepended, the parser over the trimmed QStringList. pydemo's S2
  catch-up must expect the same for its argparse-free parity shim.
- **Tripwires** (all four shipped 2026-11-25, exact slot names):
  `diffNavChordsWalkHunksInDiffAndNoOpInPlainMode` (tst_codeeditor —
  the chord itself incl. the Ctrl+F7/Alt+F7 unclaimed-chrome leg and the
  plain-editor no-op; the door semantics stay M10's pinned tests);
  `diffDoorOpensThreeTabsAndDrivesTheNavThroughTheView` (tst_demo —
  three tabs, diff tab current, `isDiffMode`, caret after F7 injection
  exact on the tour corpus, the two plain tabs' text byte-identical,
  the failed-read half-pair leg);
  `diffCliDoorCollectsThePairAndFailsLoudOnMisShape` (tst_demo — spawns
  the REAL qmn-demo binary, the same CI-built-spawn class as the
  dapfake slots: pair-at-front and mid-list exit 0, zero/one-value
  shapes exit 1 with "EXACTLY two files" on stderr; this slot CAUGHT
  trap (2) — the pre-fix binary passed the trio leg vacuously).
- DoD: ctest green (7/7 at the 2026-11-25 build); no public view door
  added anywhere (the chord sits inside the existing keyPress switch).

### Machine S1-2 — the grammar door (`loadGrammar` dialog + rebinding + `--grammar`)

- **Demo**: `MainWindow::loadGrammarLibrary(path, id = {}, language = {},
  symbolName = {}, queryPath = {})` — `CodeEditor::loadGrammar` (the
  handle stays open forever; 0 = load/symbol failure → false + status) →
  `registerGrammar(id, addr, query, {})` (the view's door VALIDATES at
  itself: the zeroed bogus fixture loads with a non-zero address yet the
  registration refuses it, the table changes NOTHING) → tabs already
  speaking the target flip LIVE from the registration ALONE (every engine
  reconciles against the registry generation inside refreshIfNeeded — the
  tst_codeeditor live-UPSERT mechanism); the door's re-assert of
  setLanguage(id) on those tabs is a repaint nudge + the status вкладок
  count, NOT the rebind step. A tab that TRIED the id BEFORE the load
  stays plain: the engine drops unknown ids at its door (language()
  empty), reconcileRegistry early-returns on the plain state ("nothing
  attached can care"), so only a fresh setLanguage or the suffix table
  lights it — pinned honestly in the tripwire (вкладок: 0, then
  setLanguage, then the span-table EQUALITY leg). Defaults: `id` = the library stem (the loader's own derivation),
  `language` = `id`, `symbolName` = the `tree_sitter_<stem>` derivation,
  `queryPath` = explicit .scm path (empty = parse-only, NO sidecar magic
  — the CLI flag and the tour config name it). Fold node names stay OUT
  of the door (no consumer today; honest deviation from the first draft's
  "roleMap" wording — registerGrammar takes the query + fold names, not
  a role map). Future tabs need NO wiring: the suffix table is the
  session's (G2) — `MainWindow::setLanguageForSuffix(suffix, id)` is the
  new one-line forwarding door onto `ProjectSession::setLanguageOverride`
  (the CLI's `--grammar-suffix` pairs and the tripwire's future-tab leg
  ride it). Programmatic door = the CLI/test path (the no-modal rule
  holds); the Файл→«Загрузить грамматику…» slot is the mouse route only,
  stem id + derived symbol + parse-only defaults.
- **CLI shape** (corrected at build 2026-11-25 — the first draft's
  repeatable `--grammar <file> <id> <language>` rides the S1-1 trap (1):
  a Qt6 value option collects ONE token per occurrence and the
  multi-value braced spelling is the silent default-value misbind):
  `--grammar <file>` repeatable, each occurrence zipped BY INDEX with the
  same-index values of the single-value companions `--grammar-id`,
  `--grammar-symbol`, `--grammar-lang`, `--grammar-query <scm>` and
  `--grammar-suffix <sfx:id>` pairs; the process-level tripwire pins the
  zip (S1-1's spawn class). pydemo's S2 catch-up mirrors the SAME flag
  names door-for-door.
- **Fixtures**: the existing `qmn_pygrammar` (the vendored parser compiled
  a second time — a DISTINCT language pointer, the live-swap proof) and
  `qmn_fakegrammar` (the zeroed TSLanguage the door must refuse). tst_demo
  grows the env defines `QMN_TEST_PY_GRAMMAR`/`QMN_TEST_FAKE_GRAMMAR`/
  `QMN_TEST_PY_QUERY` (the vendored python highlights.scm path — the door
  names query FILES, never query text) + `add_dependencies` (the
  tst_codeeditor precedent).
- **Tripwires** (built green 2026-11-25):
  `grammarDoorLoadsRebindsAndRefusesTheBogusLibrary` (tst_demo) — the
  corpus opened as `witness.py` (built-in python, the GOLDEN span table)
  and `probe.pyext` (plain: the engine drops unknown ids, `language()`
  empty — a pinned truth, not an assumption). The load face: status
  `вкладок: 0` (registration reconciles only tabs ALREADY speaking the
  id — the probe was plain), then `setLanguage('pyext')` lights it and
  its spans EQUAL the golden table ROW FOR ROW (second compiled parser +
  same query == same table — the drift the tour leg 5 kills). The UPSERT
  face: same-id reload with a comment-only `.scm` file swaps the query
  LIVE through the engine's generation reconcile (comments-only spans,
  `вкладок: 1`, no setLanguage). The FUTURE-TAB face: `setLanguageForSuffix`
  then a fresh `.pyext` tab equals the swapped table. Three refusal faces
  each false + exact status + registry unmoved: LIBRARY (missing .so),
  QUERY (unreadable .scm — the handle loads, the table does not move),
  VALIDATION (libbogus.so LOADS non-zero yet the scratch-parser check
  refuses it). Teardown: `unregisterGrammar` + the suffix unbind — the
  registry is process-wide and the S1-4 tour slot's before-face leans on
  a clean `availableLanguages()`; the probe tab falls to plain on its
  next pass (pinned).
  `grammarCliDoorZipsOccurrencesAndFailsLoud` (tst_demo, the S1-1 spawn
  class): the full companion zip rides exit 0; and the stem-derived
  NEGATIVE (no `--grammar-symbol`: `libqmn_pygrammar.so` derives
  `tree_sitter_qmn_pygrammar`, which does not exist) FAILS LOUD by itself
  — stderr `--grammar failed` + exit 1, so a silent companion drop flips
  the positive leg on its own. Malformed `--grammar-suffix` pairs exit 1
  with the `<suffix:<id>` shape on stderr.
- DoD (met 2026-11-25): ctest 7/7 green (one NOTE outside S1's closure:
  tst_lsp's `lspCapabilityRegistryAndApplyEditDoor` flaked 2/8 ISOLATED
  runs that day, each failing run at ~91.5s = three of its own documented
  30000ms QTRY windows starved — the slot's own comment names the busy-
  host class; nothing in S1-2 links into tst_lsp (no view/glue/lsp source
  changed), the binary byte-matched the earlier 7/7 run, and the full
  ctest came up 7/7 twice). No public view door added; the Python doors
  already cross — no three-copies edit.

### Machine S1-3 — DAP controls: typed hub doors + demo doors + the dapfake step arm

- **Shelf (qmn_glue, additive)**: four plain-typed doors on `DapHub` —
  `continueDebugging()`, `stepOver()`, `stepIn()`, `stepOut()` — each
  `bool`: true iff the request went out (Ready + a known stopped thread;
  DAP's `threadId` rides the last `debugStopped`, the single-thread v1
  shape). They ride the existing fire-and-forget `request()` seam, so the
  wire shape (no event frames from the client, answers ride events) is
  byte-unchanged. `stopDebugging()/terminateDebugging()` already exist —
  the demo door for stop is pure app glue.
- **Codegen (three copies)**: NO new type — the methods cross on the
  already-bound `DapHub`; the close's `--codegen` run must SHOW the four
  doors in the generated face and the rejected-log diff (the E4/E5
  drop-SET baseline) gains nothing but the usual noise.
- **dapfake (support binary, NOT a ctest entry)**: the step battery lands
  in a NEW mode `steps2` (argv: frame path, line, col + the step-arm
  frame line), so the D3 `steps` pins stay byte-frozen. `steps2` = the
  full `steps` conversation plus `next/stepIn/stepOut` → ok + a SECOND
  `stopped` event (reason `step`, new frame line from argv), then the
  `continue`/`terminate` arms and the terminate-before-disconnect rule
  unchanged.
- **Demo**: `MainWindow::debugContinue()/debugStep()/debugStop()` status
  doors (Step = stepOver, the everyday button; the hub's own answers ride
  the already-wired status signals) + CLI `--debug-continue`,
  `--debug-step`, `--debug-stop` (the tour calls the doors directly).
  Stop routes through `terminateDebugging()` where the caps allow (the
  dapfake/adapter terminate-before-disconnect demand, live-probe-pinned).
- **Tripwires**: `dapControlDoorsStepStopAndReapThroughTheDemo` (tst_demo,
  `steps2`) — the exact status-string sequence pause → step → pause(new
  frame, caret on the new frame line) → continue → terminated/ended;
  the caret-follow pin (OWNER-only is fine: the tour opens ONE C tab); the
  teardown exits dapfake 0 (the terminate arm). The hub-level typed-door
  pins live in tst_dap (same grow rule).
- DoD: shelf minor bump queued for the close (1.1.0 → 1.2.0, additive
  Python-visible growth — the E-close comment at the bump site carries the
  rule); tst_dap + tst_demo + full ctest green.
- **BUILT 2026-11-26** (host ctest 7/7 green, the S1-2 lsp flake class did
  NOT reappear — 108.7s total, `dap` 19.3s = the grown binary). Built-face
  deltas and the records the code pins:
  (1) The four doors share one private `threadControl(command)` helper
  riding `request()`; true iff `isReady() && lastThreadId_ != 0`. The
  thread memory is RETIRED by zeroing `lastThreadId_` in the hub's
  `continued` and `terminated` event arms — no existing reader survives
  those emissions, so NO D3 pin moved. A fifth public `stoppedThreadId()`
  door was considered at build and DROPPED: the surface stays exactly the
  four PLAN names.
  (2) `MainWindow::debugStop()` refuses only when there is no session
  (`!dapHub_ || !isDebugging()`); it calls `terminateDebugging()` (which
  internally falls back to `stopDebugging()` where the caps say no) and
  does NOT read that false as a refusal — the hub's `sessionEnded` string
  is the session's real report.
  (3) The CLI flags are a QUEUED RIDE, not startup calls (a control door
  called at startup could only ever refuse — the session is async):
  `MainWindow::queueDebugRide(actions)` + a private `maybeFireDebugRide`
  fired from the ALREADY-wired `debugStopped`/`debugContinued` status
  lambdas; step/continue press at a `stopped`, stop at `stopped` OR
  `continued`; the queue is cleared on sessionEnded, on disarm and on
  re-arm. LOUD face: the flags WITHOUT `--dap` are stderr + exit 1.
  The presses ride a 300 ms BEAT (`QTimer::singleShot`), not the signal
  emission itself — the first shape fired each door inside the emission
  and the ride slot pinned the defect: every status string flickered by
  in one round trip, unreadable and unobservable. The beat makes the
  CLI walk readable, keeps the doors out of the hub's emission
  re-entrance, and a session that dies mid-beat meets an honest refusal.
  (4) `steps2` grew one arm the demo walk forced: a `setBreakpoints`
  answer (ALL verified at the same line — the dots are witnesses, the
  step ride is the point). The demo stores its {2,6} pair BEFORE
  startDebugging (the hub's legal order), so the Ready push lands in
  steps2 too. `steps` mode keeps that arm FATAL — its D3 pins never
  push a breakpoint and stay byte-frozen, next/stepIn/stepOut there
  still exit 2 on purpose.
  (5) Tripwire slot names (all four green at first run after the beat
  fix): `dapHubTypedControlDoorsGateStepAndRetireThroughTheWire`
  (tst_dap — the gates where the client cannot audit itself: false
  pre-Ready, false Ready-but-never-stopped (deterministic: the stopped
  event only ever follows our own `configurationDone`), false again
  after `continued`, false after the session ends; plus the steps2
  happy leg where the far side audits `arguments.threadId==7` on EVERY
  step arm and the post-step stackTrace answers the argv[5] line — band
  2→3, caret (2,5)→(3,5), the 2-hint set shrinks to the ONE
  ` = 42` hint at (3,1));
  `dapControlDoorsStepStopAndReapThroughTheDemo` (tst_demo — the same
  walk through the DEMO doors with the exact Russian status literals,
  because they ARE the tour's pin literals — pause `(breakpoint)` →
  `debugStep()` → pause `(step)` → `debugContinue()` → `продолжено` →
  `debugStop()` → `сессия завершена (stopped)` — plus the three refusal
  faces after the end and the disarm face; dapfake exits 0 = the
  far-side receipt of debugStop's terminate-first routing);
  `debugRideQueueWalksTheSessionWithoutAnyManualPress` (tst_demo, IN
  PROCESS — the CLI queue's semantics with NO hand on the doors, the
  slot that caught the flicker defect and pins the beat);
  `debugCliRideFlagsNeedTheAdapterAndRideClean` (tst_demo, the S1-1
  spawn class — flags-without-`--dap` exit 1 naming `--debug-step` on
  stderr, and the full adapter+file+three-flags walk exit 0 on the
  self-quit timer, pinning the parser registration from both sides).

### Machine S1-4 — `--tour`: the SHARED PIN TABLE (the risk-3 artifact)

The table below is THE artifact both demos mirror (one table, two drivers).
Rules: the tour is a scripted walk of the DEMO's OWN doors (the tour calls
`openPath/setDiffDocuments`-via-`openDiffPair/loadGrammarLibrary/debug*`
— never a private seam), prints one probe-style line per pin
(`PASS n: leg — detail`, the `probe_dap.sh` shape), prints skips as skips,
EXITS NONZERO at the FIRST failed pin (the print lands first), otherwise 0.
Output rides `qDebug()` (the ctest face captures it at the message
handler; the CLI face installs its own stderr printer for the run —
off a TTY Qt routes debug logs to the journal, see the BUILT record).
Waits ride a local spin helper (processEvents + 20 ms tick + deadline —
Qt6::Test NEVER links into `qmn_demo_app`: only test binaries see it).

**The corpus** — frozen literals in the shared tour file
(`src/app/ShowcaseTour.{hpp,cpp}` in `qmn_demo_app`; S2 re-declares the
same strings as Python literals — the corpus itself is the drift check).
FOUR files, as frozen at build (the block's original draft had drifted —
its 4-line tour.py carried neither the `sum ` leg-3 selection nor leg 2's
`lineCount()==5`, and "model lines 1 and 4" disagreed with the demo's
stored breakpoint PAIR, see the BUILT record's delta list below; THE
LITERALS BELOW ARE THE TRUTH S2 COPIES BYTE-FOR-BYTE):

```python tour.py                       (5 lines, NO trailing newline)
import sys
values = [1, 2]
x = sum (values)
def greet():
    return x  # greet
```
```text diff_orig.txt / diff_mod.txt  (the exact pair, 5 lines each, TWO
alpha\none\nbeta\ntwo\ngamma          replace-hunks at MODIFIED lines 1
alpha\nONE\nbeta\nTWO\ngamma          and 3 (0-based))
```
```c demo.c
int main(void) { return 0; }
```
There is NO separate DAP source: **tour.py ITSELF is the dap-dapfake
leg's session document** — tab 0 never closes and the door's auto-start
rides `editorAt(0)`, so the steps2 frames carry tour.py's abs path (the
DAP leg identity-pins `editorAt(0) == the boot-leg editor` first).
probe.pyext + future.pyext are grammar-leg DERIVED copies of tour.py's
content (same golden table), not frozen entries. tour.py is deliberately
NOT executable (no shebang, no exec bit) — see the live-adapter face.

**The golden highlight table** (the highlight leg prints it row-per-span
over tour.py's whole document — 15 spans, from the frozen vendored py
query; the grammar legs pin the pyext tabs' spans EQUAL it; this is THE
drift killer between the two drivers): `0-6:Keyword 7-10:Variable
11-17:Variable 18-19:Operator 21-22:Number 24-25:Number 27-28:Variable
29-30:Operator 31-34:FunctionBuiltin 36-42:Variable 44-47:Keyword
48-53:Function 61-67:Keyword 68-69:Variable 71-78:Comment`

**The printed table** — 14 rows, frozen order. Measured offline face
(`QMN_TOUR_NO_LIVE_DAP=1`, exit 0, 11 PASS + 3 SKIP); the detail texts
are the printed ones (abridged where marked `…`). NOTE the row numbers
are the PRINTED counter, not the code's leg labels (the code walks 9
legs: the grammar leg prints rows 6–8, corpus and teardown print their
own rows; `qmn-demo --tour` numbers and S2 mirrors the ROW TABLE, leg-
for-row):

```
PASS 1: corpus — 4 frozen files under <dir>
PASS 2: boot — 1 tab, lineCount 5, caret {0,0}
PASS 3: model — setPosition {2,4}, selection 'sum ', insertTextEdits
        's'->`sums `, undo byte-identical
PASS 4: highlight — 15 spans: <the golden table>
PASS 5: diff — the door's 3 tabs (tour-side count 4, the witnesses
        still open), diff focused, F7 walk 1->3->3->1->1,
        text == modified corpus
PASS 6: grammar — before-face: pyext absent, a pre-set tab stays plain
        (language() empty, zero spans)
PASS 7: grammar-load — load==вкладок: 0, probe stays plain until
        setLanguage, spans EQUAL the golden table, future .pyext tab too…
PASS 8: grammar-refusal — bogus library false + exact status, registry
        unmoved
PASS 9: settings — 4-knob subset (tabStop 7, ruler 80, softWrap on,
        Dark preset) rode options()+theme() and collected back, baseline
        restored
SKIP 10: lsp-clangd — clangd is not on PATH …   (live host: PASS —
        claimed + 3 diagnostics after `bad(` on clangd 23.1.5)
PASS 11: dap-dapfake — exact status walk: запущена -> пауза 7
        (breakpoint) [band 2, caret {2,5}, dots {2,6} verbatim past EOF,
        hint {2,1}=' = 42' first-wins] -> step [пауза 7 (step), band 3,
        caret {3,5}, hints 0 on def greet()] -> продолжено [band
        cleared] -> завершена (stopped) -> three exact refusals ->
        адаптер остановлен; sequence exact
SKIP 12: dap-lldb-dap — <reason>    SKIP 13: dap-gdb — <reason>
        (ctest face: `%1: the ctest face forces the PATH legs off (the
        --lsp no-guarantee law) — the host ritual owns these rows`;
        CLI kill-switch: `%1: scoped off by the operator (QMN_TOUR_NO_-
        LIVE_DAP=1) — the binary may well be installed, the leg just
        did not run`; discovery face: `%1 not found on PATH`)
PASS 14: teardown — every leg tab closed, every hub disarmed, tab
        count 0 …
```

**Doors per leg** (ONLY the demo's public doors + public VIEW doors,
never a private seam; S2's wheel names ride 1:1 on the demo doors —
`qmn.Monaco`'s `open_path/open_diffs/load_grammar`-style surface is
S2's own catch-up question, pinned at S2): boot `openPath`; model
`setPosition/selectedText/applyTextEdits/undo/toPlainText`; highlight
`highlightSpans`; diff
`openDiffPair` + raw F7/Shift+F7 `QKeyEvent`s through `QApplication::
sendEvent` + `position()`; grammar `loadGrammarLibrary/setLanguageFor-
Suffix/setLanguage/availableLanguages` + the bogus fixture; settings
`applyEditorSettings/collectSettings/options()/theme()`; lsp
`setLspEnabled` + `openPath(demo.c)` + `applyTextEdits("bad(")` +
`diagnostics()`; dap `setDapAdapter(program, {"steps2", <tour.py abs>,
"3","6","4"})` (armed LATE -> auto-start registers tab 0 = tour.py) +
`debugStep/debugContinue/debugStop` + `breakpointLines/highlightedLines-
Start/position/inlayHints/dapEnabled`; teardown `closeTabAt` +
`dapEnabled/lspEnabled`. Every wait rides the local spin helper;
Qt6::Test is NEVER linked into `qmn_demo_app`.

**BUILT 2026-11-26** — `src/app/ShowcaseTour.{hpp,cpp}` (in
`qmn_demo_app`; `demo_main.cpp` registers `--tour`), tst_demo's
`tourTableRunsGreenMinusTheServerLegs`. Delta vs this car's original
spec (all owner-settled in code-reading; the code + the printed faces
above ARE the record):

- Corpus: FOUR files (the spec's separate DAP source is DROPPED —
  tour.py is itself the session doc, tab 0 never closes, the auto-start
  rides `editorAt(0)`; the DAP leg's first pin is that tab-0 identity).
  The spec's `import demo` 4-line draft is REPLACED by the 5-line
  literal block above — `sum (values)` is what makes the {2,4} +
  4xShift+Right `sum ` selection and the 15-span golden table work.
- `DapHub::setBreakpoints` takes 0-BASED MODEL lines and the demo's
  stored PAIR is `{2, 6}` (MainWindow.cpp, wire lines 3/7) — the spec's
  "model lines 1 and 4" note was wrong. On the 5-line tour.py line 6 is
  PAST EOF, and `breakpointLines()` reads the pair back VERBATIM (the
  decoration store keeps what it was given; no dot painted past EOF) —
  pinned exactly. steps2 argv is `{"steps2", <tour.py abs>, "3", "6",
  "4"}` (DAP 1-based frame lines; MODEL 2 then 3 -> band 2 -> 3, carets
  `{2,5}` -> `{3,5}`, the tst_demo S1-3 slot's frozen pair byte-for-
  byte). The model's `x = sum (values)` frame line yields exactly ONE
  value hint `{2,1," = 42"}` (first-wins over dapfake's WRONG-DUP `x`);
  the `def greet():` step frame yields ZERO hints — the tst slot's
  2->1->0 shape was THAT fixture's line content, not a law.
- The whole spoken status sequence is pinned BYTE-EXACT and IN ORDER
  (`QStatusBar::messageChanged` collector on a scope-lifetime receiver,
  so no pin can be overtaken by a later face): arm face `адаптер %1 —
  сессия начнётся с первой вкладкой` -> запущена -> пауза (breakpoint)
  -> пауза (step) -> продолжено -> завершена (stopped), then the three
  exact refusal faces (шаг / продолжение / остановка) and the disarm
  face `адаптер остановлен`.
- The diff leg's tab arithmetic rides the live tour (tour.py + probe/
  future witnesses still open): count 4 / currentIndex 3 — the 3-tab
  shape stays pinned in tst_demo; the tour-side pin is on the FOCUS and
  the F7 walk (the frozen hop table itself is the S1-1 strict-edge
  no-wrap law, pinned in `tst_codeeditor`).
- The settings leg rides a 4-knob SUBSET (tabStop 7, ruler 80, soft-
  Wrap ON — pinned against the OFF baseline via `!collected.softWrap-
  Enabled` — Dark preset) and RESTORES the baseline afterwards, so
  later legs see the demo's defaults; the 15-knob surface itself
  untouched.
- Fixture plumbing (CLI `--tour`): corpus dir under a temp dir unless
  `QMN_TOUR_OUT` names one; build-tree guesses off the executable, each
  overridable by env (`QMN_TOUR_PYGRAMMAR/PYQUERY/FAKEGRAMMAR/DAPFAKE`);
  <=3 positional tail args override (corpusDir, dapfake, bogus); the
  bogus candidate is `build/tests/libbogus.so` (OUTPUT_NAME `bogus`);
  clangd/lldb-dap/gdb ride PATH (`QStandardPaths`),
  `QMN_TOUR_NO_LIVE_DAP=1` prints those legs as skips, and a NAMED-but-
  missing adapter path (`QMN_TOUR_LLDBDAP`/`QMN_TOUR_GDB`) is a LOUD
  stderr + exit 1. Same law on the fixture side (measured): an env-
  NAMED-but-nonexistent grammar lib is NOT a skip — the leg FAILs with
  the door's own face (`FAIL 7: grammar-load — loadGrammarLibrary
  returned false`) and the table stops there; only a DISCOVERY-EMPTY
  path prints its SKIP.
- **The pin table channel**: the CLI face installs its own stderr
  message handler around the run — off a TTY Qt routes `qDebug()` to
  the JOURNAL and a piped `--tour` printed ZERO pin lines (measured);
  the tst face captures at `qInstallMessageHandler` and restores.
- LIVE-adapter face (host: clangd 23.1.5 + lldb-dap + gdb present,
  re-measured at this build): rows 1–11 all PASS (clangd claimed
  demo.c, 3 diagnostics after `bad(`; the dapfake leg's exact walk
  green on the live table too); row 12 FAILs with the adapter's own
  face — `DAP: пуск отклонён: (no message)` — because the auto-start
  launches tour.py ITSELF, which has no shebang and no exec bit
  (ENOEXEC); the table stops at the first FAIL (exit 1), so the gdb
  leg never prints. This
  exit-1 is the owner-ACCEPTED live face (settled 2026-11-26): the
  exact live adapter pins stay `tools/probe_dap.sh`'s (its launches use
  REAL binaries); an operator scopes the ritual to the offline-green
  table with `QMN_TOUR_NO_LIVE_DAP=1` (measured: exit 0, 11 PASS +
  3 SKIP).
- ctest face: the slot runs LAST in tst_demo (slot order is
  load-bearing — the grammar slots' own `unregisterGrammar("pyext")`
  teardown runs BEFORE it, so the grammar before-face's honest "pyext
  absent" holds; the tour leaves pyext registered behind itself and
  nothing runs after). The slot pins the printed table ITSELF: 14
  lines, frozen PASS/SKIP prefixes in frozen order, zero FAIL, the
  golden 15-span LITERAL (so the S2 drift artifact is frozen in CI,
  not only printed), the diff detail's `1->3->3->1->1` walk, the
  dap-dapfake detail's frozen witnesses (`dots {2,6}`, both carets,
  `= 42`, `sequence exact`), `EQUAL the golden table`, `baseline
  restored`, `tab count 0`, and both forced-off SKIP reasons (the
  honesty law is part of the pinned table). The
  tour NEVER spawns clangd/lldb-dap/gdb from ctest (the `--lsp`
  no-guarantee law); dapfake IS spawned (the CI-built far-side oracle;
  its exit stays 0 through the terminate arm). Tripwires: the two slots
  `tourCliDoorFailsLoudOnBadTailAndMissingNamedAdapter` (the CLI wiring's
  loud faces — a spawn that NEVER runs the table, the knot stays intact)
  and `tourTableRunsGreenMinusTheServerLegs` (tst_demo, 23 slots now);
  the offline CLI face (14 rows, exit 0) and the live host face above
  were both MEASURED runs, not assertions.
- DoD (met 2026-11-26): the 14-row table prints and tst_demo pins it
  (23 slots); ctest 7/7 green on the shipping run PLUS three extra
  `-j 7 --repeat until-fail:3` full passes (the LAST of those ran with
  the recorded tst_lsp liveness fix below in the tree); the offline CLI
  face exits 0
  (11 PASS + 3 SKIP, honest reasons) and the live host face exits 1 at
  row 12 (the owner-accepted adapter face). No new public view door
  (`CodeEditor.hpp` untouched — only the S1-1 `.cpp` no-wrap law), the
  four plain-typed `DapHub` doors ride the already-bound class (m7
  precedent, no three-copies edit), and `Qt6::Test` stays out of
  `qmn_demo_app` (the link line proves it; the tour's waits ride its
  own spin helper).
- NOTE (CAPTURED defect + fix, in `tests/tst_lsp.cpp` only): a full
  ctest run showed `lsp` Failed; the slot was caught red-handed from
  the log BEFORE any rerun:
  `hubViewHandoverKeepsWireFeedsAndMarksStanding` — its LAST liveness
  leg sampled `reqCount(stderrSpy, "textDocument/foldingRange") >
  asksAfterJoin` immediately after `waitUntil(version()==2)`. The
  mechanism is a fixed-race, not load magic: on an edit the fold feed
  re-arms at `kFoldRefreshMs` 300 ms and the syncer flushes at
  `kDefaultDebounceMs` 400 ms, so the version bump returns ~100 ms
  after the ask is SENT — and that was the whole budget for the ask's
  round trip into the fakeserver's stderr echo landing in the spy.
  Measured face: **6/30 isolated failures at this head AND 6/30 in a
  throwaway `HEAD` (= v1.13.0) worktree** — the flake PREDATES S1 (the
  earlier "matches the recorded starved-host class" attribution was
  WRONG and is retracted: it is a DIFFERENT slot than
  `lspCapabilityRegistryAndApplyEditDoor` and the cause is the
  100 ms window, not starved 30 s QTRYs). The fix follows the slot's
  OWN idiom (the leg two steps up already `waitUntil`s on the req
  count): the pin now WAITS for the far-side witness — a dead feed
  still fails (the ask never arrives, the 8 s wait times out), only
  the "must arrive inside the version pump" coupling died. Verified:
  0/30 isolated after the fix (was 6/30) and the `-j7
  --repeat until-fail:3` full-suite stress (the load that surfaced
  it) came up 7/7 ×3, 1563 PASS lines, zero failures. This change is
  a TEST-only edit — no src/ file beyond the already-recorded S1 set
  rides it.

CI shape: tst_demo's `tourTableRunsGreenMinusTheServerLegs` builds a
`TourConfig` with the fixture paths (`QMN_TEST_PY_GRAMMAR`,
`QMN_TEST_PY_QUERY`, `QMN_TEST_FAKE_GRAMMAR`, `QMN_TEST_DAP_SERVER` —
the DAP slot composes the steps2 args itself), clangd/lldb-dap/gdb legs
forced off, and asserts the tour function returns TRUE with legs 1–9,
dap-dapfake and teardown all PASS — the table ITSELF is thus pinned in
CI; the tour binary flag stays a live ritual, never a ctest entry (the
knot). The live shape (host): `qmn-demo --tour` resolves clangd/lldb-
dap/gdb from PATH and prints the full table; the S2 close diffs the two
tours' printed tables leg-for-leg.

### Machine S1-5 — the close (target 1.14.0)

Two package literals + the wheel half (rebuild + repair + smoke + the
FOURTH literal `pydemo/pyproject.toml` re-point + green headless pydemo —
the 1.9.0 lesson), the SHELF literal 1.1.0 → 1.2.0 (S1-3's Python-visible
growth; the bump-site comment carries the rule), the `--codegen`
rejected-log diff against the E4/E5 drop-SET baseline (the four hub doors
must be ABSENT from the log; a drop the log does not show crossed by
accident — both ways), CHANGELOG + README/AGENTS wording (the F7/F8
chord joins the keymap records; the `--diff/--grammar/--tour` CLI joins
the demo records), then the fresh out-of-tree ask as always.

### Car S2 — pydemo mirrors the showcase door-for-door

**DEFERRED (owner decision 2026-11-28, not cancelled).** The car as
designed below mirrors S1's showcase door-for-door and re-pins it from
the wheel. Its tripwire VALUE (the two printed tables diffed
leg-for-leg) only pays while the showcase-mirror story is the product
showcase; the owner judged that story lost the dogfooding contest to
GATE 4 (an IDE living IN the library finds API holes the mirror
cannot). What survives regardless: pydemo stays the wheel smoke the
close ritual already runs (headless green at every wheel-shipping
close — that leg of S2 is ALREADY paid), and the attach-ctor lifetime
question S1-4 recorded in PLAN stays open for whoever wants Python
multi-view. REVISIT RULE: if a real Python consumer shows up, S2
un-defers as pre-booked below, unchanged.

argparse grows the SAME flags (`--diff`, `--lsp`, `--dap`, `--dap-arg`,
`--dap-program`, `--tour`), the same Russian UX, the same status-bar
visibility route. This car CANNOT precede car E (the DAP doors are the
wheel crossing) and its tour legs mirror S1's pin list. The headless
green run stays a close ritual (the 1.9.1 lesson); the LIVE tour
against real clangd + lldb-dap is the gate-close ritual alongside
`probe_dap.sh` and `probe_clangd.sh`.

### Car Py — the PyPI half (last, infra)

**DEFERRED (owner decision 2026-11-28, rides the S2 deferral).** The
CONTAINER pipeline keeps shipping wheels at every close (that half is
the paid close ritual, unchanged); what defers is the test.pypi.org
PUBLISH path below. Revisit when publishing actually matters to the
owner.

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
2. **The rejected-log drift** — car E changes what Python sees; the audit's
   drop SET must be RE-PINNED from the log, not eyeballed (both the 1.6.1
   and 1.7.0 codegen traps were silent drift). RETIRED at car E machine E4:
   reading the archived logs showed the "16 drops" baseline was a SET NAME,
   never a line budget — the honest counts (34 `LspHub` + 1 `ProjectSession`
   at 1.12.0, 9 more on `DapHub`) live in E4's paragraph, and E4/E5 both
   diffed the drop SET against the archived baseline instead of trusting
   any number.
3. **Showcase drift vs the mirror rule** — S1 and S2 must stay
   door-for-door; the tour pin list is SHARED between them (one table,
   two drivers), and that table is the drift tripwire.
4. **Tour honesty** — a tour that skips everything still exits 0 by
   design; the gate-close ritual is the LIVE run (clangd + lldb-dap +
   gdb + dapfake all present), recorded in the CHANGELOG entry like the
   probe ritual's outcome was, or the gate may not close.

# GATE 4 — the IDE dogfood train (opened 2026-11-28, owner decision)

**STATUS (2026-11-28): cars I0+I1 CLOSED at 1.15.0** — the pair landed
and closed as ONE version exactly as the version-rhythm record planned.
The LEDGER rule paid its first fare: catch #1 (no public programmatic-
selection door) was found by the app living only on the exported
surface, written before any workaround, and CLOSED the same car
additively (`CodeEditor::setSelection` public + tripwire; the a11y-
channel workaround was NOT taken). The wheel half of the close ritual
rode whole: the container rebuilt/repaired `qmn_native-1.15.0`, the
`--codegen` rejected-log set rode a BOTH-WAYS diff against the archived
1.13.0 baseline — 1824 lines, ZERO delta — and `setSelection` got a
LIVE presence+behaviour pin against the REPAIRED wheel (callable
through the wheel, produced the expected selection); pydemo's fourth
literal + its headless smoke green. **I2 quick-open/palette is THE OPEN
CAR.**

**WHY (the owner's words, paraphrased):** a showcase that mirrors
itself proves the surface is SELF-consistent; it does not prove the
surface is USEFUL. The strongest test a text-editor library can get is
an application living inside it — so the gate's product is a small but
FULL IDE, `qmn-ide-demo`, built ONLY through the exported/public doors,
while the real deliverable is what the walk exposes: API holes (the
LEDGER), library seams worth promoting, and the binding shape those
seams must keep. The IDE is a means; the honest library is the end.

**CONSTANTS (never in question):** the frozen surface widens only
additively; every public door speaks Shiboken-wrappable plain Qt types
(car E's law: a Python-facing route for a callback rides a SIGNAL, not
a `std::function`); a leg that cannot run prints as a skip, never as a
pass; ctest is the far-side oracle where a far side exists (fakeserver/
dapfake class — CI-built spawn IS allowed; clangd/live adapters are not
CI guarantees, the `--lsp` no-guarantee law rides every new test face);
no vision — every IDE face is pinned programmatically (state/numbers).

**THE LEDGER rule (this gate's core ritual):** whenever the IDE needs
a door the shelf does not have, the hole gets written in the ledger
BEFORE any workaround; the fix lands as an ADDITIVE door in the car
that needs it, with its own tripwires, and if the door crosses the
wheel it rides the three-copies + rejected-log audit like every other
crossing. Seeds already known from the S1-era surface audit (closed
when a consumer actually asks):
- `LspHub::requestDocumentSymbol` — the outline sidebar (I3);
- `LspHub::requestWorkspaceSymbol` — symbol quick-open (decide at I2);
- a possible M5 decoration KIND for gutter/git stripes (I5 — check what
  the five shipped decoration records cover before declaring the hole);
- a possible read-only editor mode for the task-OUTPUT panel (I4 —
  check `CodeEditor`'s surface at I4 opening; if a door exists, the
  output panel is a dogfooded editor; if not, ledger first).

**LEDGER catch #1 (FOUND and CLOSED at car I1, 2026-11-28):** the panel's
reveal (open the row's file, caret + selection on the match) hit a real
hole — `CodeEditor::setSelection(anchor, caret)` was PRIVATE machinery:
the frozen public surface carried only `selectAll`/`clearSelection` plus
the M11 a11y text-interface `setSelection` channel for programmatic
selection. The hole went into the ledger BEFORE any workaround, and the
workaround (driving the a11y channel from app code) was NOT taken: the
fix landed the same car as an ADDITIVE widening — the raw door that the
a11y bridge and the diff machinery already route through became PUBLIC
(contract written at the declaration: both ends clamped through
`clampPos`, reversed ends legal because read/draw sort, the CARET is the
second argument, D1 single-caret law — extra carets cleared, any edit
chain broken, `cursorMoved` + `selectionChanged` fire on EVERY write
including a collapse, the view scrolls itself). Tripwire:
`tst_codeeditor::programmaticSetSelectionDoorReveals` (signals count,
clamped/reversed/cross-line faces, the collapse face tells the truth).
Wheel shape: a plain-Qt-typed door on the ALREADY-bound `CodeEditor` —
m7's precedent, NO three-copies edit; the close-ritual `--codegen` audit
must find it in the generated wrapper and its ABSENCE from the rejected
log (both directions checked, as always).

**LIBRARY-PROMOTION law (G1's precedent, stated general):** app code
becomes a library when it earns a SECOND user — with one exception:
code born headless and pure MAY start as a library on day one (the
`src/search/` case, I1). A promoted/seeded library is QtCore-only
(headless, benchmarkable, fuzzable — the core/diff class), keeps its
public records plain-Qt-typed, and delivers results through SIGNALS on
a QObject. Promotions happen at CAR BOUNDARIES with their own record —
never mid-car surgery (this is what tempts refactor of `qmn_demo_app`
mid-gate: resist, write the candidate down instead).

**ctest shape:** every new lib grows its own guiless ctest entry (the
textbuffer/diff precedent); the IDE app grows as STATIC lib + thin
main (`qmn_ide_app` + `qmn-ide-demo`, the `qmn_demo_app`/`tst_demo`
pattern) so its faces are drivable offscreen from `tst_ide`.

**Version rhythm:** every car close bumps the package (Gate precedent);
I0+I1 are planned to land and close as ONE version (1.15.0 target —
the two cars are one shape: an app skeleton + the lib it consumes;
revisit at close if I1 slips).

## Train ordering (DECIDED 2026-11-28): I0 → I1 → I2 → I3 → I4 → I5

### Car I0 — `qmn-ide-demo` skeleton: project folder, tree, tabs

- New target pair in `src/app/ide/` (or `src/app/` siblings, mirror the
  qmn-demo shape): `qmn_ide_app` STATIC lib + thin `qmn-ide-demo` main;
  links `qmn::view` + `qmn::glue` (PRIVATE, like demo — no glue type in
  an app-defined header beyond forward-decls).
- The window: a sidebar dock (`QTreeView` over `QFileSystemModel` —
  Qt's own tree, app code, nothing to promote; the qmn code is only
  the open-on-double-click), the tab area (`CodeEditor` tabs), the
  status bar (the SAME `messageChanged` visibility route the tour
  reads — keep that channel honest for future tour legs), a project =
  the folder from argv (`qmn-ide-demo <dir>`) or a folder dialog;
  `.qmn/lsp.json` pickup and the `--lsp`/`--dap` door shapes ride
  UNCHANGED from the demo.
- FIRST REAL PRESSURE POINT, deliberately: the IDE keeps ONE tab per
  path — opening the same file twice FOCUSES the existing tab. The
  second tab-group story (same document, two views, ONE `didOpen`) is
  car D's feature; the IDE is its first real consumer, and `tst_ide`
  pins: double-open → one registration, one `didOpen` (the fakeserver-
  style far side is unavailable in tst_ide — the pin rides the glue's
  own `clientForUri`/registration reads, the tst_glue class), plus the
  file-tree not double-listing.
- I0 carries NO search, NO terminal (OUT OF SCOPE by owner decision —
  a terminal drags PTY/pixel questions far from the library's honesty),
  NO git (I5).
- DoD: binary opens with a real project dir, tree-open → tab (offscreen
  drivable), same-file double-open pin, headless `--exit-after-ms`
  exit-0 face, ctest GAINS `ide` (offscreen; NO server spawn; the
  no-guarantee law rides).

**I0 CLOSED 2026-11-28 (rides the 1.15.0 pair-close) — what the code
actually says:**
- The pair landed as `src/app/` SIBLINGS (not an `ide/` subdir):
  `qmn_ide_app` STATIC (`IdeWindow.cpp`) + thin `ide_main.cpp`;
  `qmn_glue` + `qmn_search` linked PRIVATE — `tst_ide` compiles against
  `IdeWindow.hpp` (glue/search types stay fwd-decl'd behind unique_ptrs,
  the AGENTS app-header law) and never names one itself.
- The project door `openProject(dir)` is EARLIER than the demo's
  first-openPath root: the tree root, the session's workspace root and
  the `.qmn/lsp.json` fold all land on this one door; the CLI face names
  it from argv and refuses a FILE root LOUD (stderr + exit 1, measured).
- The ONE-tab law pinned AS ADOPTED: a re-open FOCUSES (tab count stays,
  the status bar says the focus happened — never silence), and a
  LEXICAL variant of the same path (`dir/./file`) focuses the same tab
  (tab key = the normalized absolute path). DEVIATION from the car
  sketch, stated honestly: the sketch asked tst_ide to pin "one
  registration, one didOpen" through glue registration reads — tst_ide
  has NO such reading door (adding one was rejected: an observation door
  the app itself does not need), so that face stays tst_glue's JOIN
  territory and the IDE pin rides the tab-count + focus-message face.
  When a later car gives the IDE a second-VIEW face, that is the moment
  the JOIN story appears in the IDE with its own record — not now.
- `tst_ide` (offscreen, `QTEST_MAIN`): 11 slots green — project door
  round-trip, tab/language faces (session suffix table; unmapped suffix
  honest plain), the failed-open face (false + non-modal status message),
  one-tab + close/reopen (no squatter), the panel faces (see I1), and
  the `--lsp` door slot mirrors tst_demo's law whole: the ONE hub slot
  opens only UNCLAIMED languages, nothing ever spawns.
- Headless faces measured: `qmn-ide-demo examples --exit-after-ms 300`
  exit 0; a file as the project root exits 1 LOUD.

### Car I1 — `src/search/`: the engine library + Find-in-Files panel

- The FIRST new layer since the Gate-2 five: `src/search/`, export set
  `qmn::search`, **QtCore ONLY** — headless like core/diff, its own
  guiless ctest entry from day one (born-headless promotion exception,
  stated above). The IDE app PRIVATE-links it (the `qmn_view`⟶
  `qmn::diff` precedent's mechanism: internal export target is NOT
  needed here — search is a real export set the app links directly).
- TWO faces on one engine: (a) the PROJECT walk — bounded recursive
  scan of the project dir with the walk CONTRACT pinned by tests:
  hidden entries skipped, binary sniff (a NUL in the first read chunk
  skips the file), size cap, hard-skips (`.git`), plus the user's
  ignore globs later cars may add (NOT I1's question); (b) the
  IN-BUFFER face over an open document's text — the same engine fed a
  `QString` (the 0-based UTF-16-code-unit `[start,end)` law holds for
  its offsets, exactly like everywhere else).
- PUBLIC shape (wrappability from birth): records
  `SearchQuery{text, caseSensitive, wholeWord, isRegex}`,
  `MatchSpan{startCol, endCol}`, `FileMatch{filePath, line, spans,
  preview}` — plain Qt types only; the searcher is a QObject with
  `start(query, roots)` / `cancel()` / progress + results as SIGNALS
  (car E's law — a Python-facing callback route is a signal); a bogus
  REGEX pattern is a loud QUERY-REFUSAL record, never a crash and
  never an empty-looking success. The fixed-string path never compiles
  a regex (the perf path); wholeWord rides word-boundary handling on
  the regex path.
- TESTS are the heart (guiless, deterministic): property test against
  a NAIVE `QString::indexOf`-style reference on a `QTemporaryDir`
  corpus generated with a FIXED seed (the `tst_textbuffer` style), the
  case/wholeWord/regex faces, the walk-contract faces (binary skip,
  size cap, hidden skip), the cancel leg (a half-scan cancels and
  reports honestly, NOT zero-results-as-success), and the UTF-16
  column pins — a match AFTER an astral character must speak UTF-16
  UNITS (this leg is what catches the byte-offset slip; the tree-sitter
  2× trap's sibling class).
- The PANEL (in the IDE): a query row (case/word/regex toggles) +
  results tree; double-click opens the file (or focuses the tab) and
  the caret lands + the selection reveals the match — pinned
  offscreen through `tst_ide` (counts, caret, selection text). Status
  bar: the Russian UX law keeps working (`N файлов, M совпадений,
  K пропущено`).
- Bounded perf face (a MEASURED record, not a frozen ctest pin):
  `tools/bench_search.cpp` (the `bench_model` precedent) over a
  generated corpus; the DoD records the measured number, and the
  ctest pins the CAP behavior instead of wall-clock (the AGENTS
  vacuous-pass style for timing).
- DoD: ctest 8 entries (search guiless + ide offscreen both green),
  the panel faces in tst_ide, the measured bench number in the record;
  the engine scanned against the repo's OWN tree as a smoke whose
  COUNT is a recorded measurement, not a pin (trees drift).

**I1 CLOSED 2026-11-28 (rides the 1.15.0 pair-close) — the honest
shape, code over sketch:**
- `src/search/` shipped as sketched: QtCore-ONLY, real export set
  `qmn::search` (core-shape install, NOT the internal_diff link-only
  mechanism), the app PRIVATE-links it, `tst_search` GUITLESS from day
  one. (The DoD's "ctest 8 entries" was a COUNT SLIP in the plan — the
  head rode 7 entries; search + ide make NINE, all green.)
- The public records deviate from the sketch in ONE way and the record
  says so: `FileMatch{filePath, lines}` NESTS a per-line
  `LineMatch{line, spans, lineText}` (the panel needs whole-file groups
  and per-line rows; the sketch's flat line/spans/preview spelling
  cannot carry a file's many lines). `lineText` IS the capped PREVIEW
  (240 units + '…', cap+1 length; the CR of a CRLF line is stripped for
  PRINT only — spans speak the raw between-`'\n'` slice). Query/refusal/
  walk-knobs/stopReason prefixes all froze as written at the door
  (header): `""`, `"query: …"`, `"root: …"`, `"cancelled"`,
  `"budget: …"`; refusal faces emit `finished` SYNCHRONOUSLY inside
  `start()` — the connect-before-start law written at the door, paid
  once in the panel's CTOR.
- A real semantic divergence CAUGHT AT WRITING TIME and kept as a named
  tripwire (`tst_search::wholeWordNestedSlide`): the non-overlap law
  binds ACCEPTED matches only — a REJECTED wholeWord occurrence must
  SLIDE one position, never skip past its window ("a.a" over "xa.a.a":
  the occurrence at 1 has a dirty left boundary, the nested one at 3 is
  clean; jump-past-window answers ZERO hits, slide answers exactly
  3-6). The fixed-string `indexOf` loop and the regex pattern-wrap face
  (PCRE slides lookaround-failed starts by itself) agree at 3-6; the
  naive reference carries its OWN slide loop — three mechanisms, one
  answer, that is the pin. Documented seam kept OUT of the corpus on
  purpose: regex `\w` with unicode properties covers combining MARKS,
  which the shared letter/number/'_' boundary class does not — a face-
  agreement question for the ledger IF a consumer ever needs marks.
- `tst_search` (guiless): 19 slots — the FIXED-SEED (0xC0FFEE) corpus
  vs a NAIVE per-position reference (the wholeWord-regex face pairs two
  DIFFERENT mechanisms: the engine's lookaround-wrap vs the reference's
  raw-iterator + manual boundary check + manual slide), the astral-
  column UTF-16 legs (the tree-sitter-2× trap's sibling class), the
  walk-contract faces (binary NUL sniff, stat-first size cap, hidden +
  `.git` default-skip and their un-skipped faces, maxDepth, budget cap,
  the cancel leg's honest "cancelled" never a clean zero), the
  scanFile≡scanText face-agreement, and the synchronous-refusal +
  reentry legs.
- The PANEL (I1's half in the IDE): query row + case/word/regex toggles,
  the results tree (file groups, matched-LINE leaves), rows → the engine
  walk's own deterministic order (the engine sorts per-directory
  codepoint-order, locale-INDEPENDENT — spelled inside the engine, not
  the locale-aware `QDir::Name` sort); row activation opens-or-focuses
  the file and reveals caret+selection through the PUBLIC view doors
  (that is LEDGER catch #1 above, closed in this same car). Panel read
  doors `searchResultRowCount/searchResultFileAt/searchResultLineAt/
  searchSummary` are the tst_ide observation channel (app-level doors,
  NOT library widenings — the engine keeps no row-flattening concept,
  that is the panel's own model). `tst_ide` pins rows/lines out-of-range
  honesty, the reveal faces (caret at span END, selectedText == the
  needle), the refusal faces riding the engine's MACHINE reason verbatim
  inside the Russian UX, and the panel-level cancel face ("остановлен ·
  частичный", never the completed spelling).
- The MEASURED perf record (`tools/bench_search.cpp`, the bench_model
  precedent — release-built archive, this time `-fPIC`-linked: THIS TU
  destructs inline QByteArray temporaries and a direct PC32 reference
  to Qt's PROTECTED `QByteArray::_empty` gets a copy relocation the
  linker REJECTS in a PIE link — the bare bench_model recipe does not
  ride on that luck, the trap line sits at the tool header):
  * generated corpus (seeded, 500 files × 400 lines ≈ 16.3 MB UTF-16,
    "const" fixed-string, caps OFF): BEST of 3 event-loop-paced walks
    **81 ms** (162 µs/file, 449 566 spans reported); pure `scanText`
    face **73 ms** over the same corpus in one buffer;
  * the repo's OWN tree smoke (working tree incl. `build/`, .git
    default-skipped): **142 ms** best of 3, 1510 files scanned / 368
    skipped / 16 241 matches, pure face 53.6 MB in 80 ms — a snapshot
    measurement of one day's tree, NOT a pin (the DoD's own law).
  ctest pins the CAP faces (budget/cancel/sizes), never wall-clock —
  the vacuous-pass law rides the timing class.
- The wheel crossing of `qmn::search` was NOT taken at this close: the
  library is wrappable BY CONSTRUCTION (plain-Qt records, signal
  routes — the header states it) but no consumer asked for a Python
  face yet; the crossing is a three-copies entry whenever one does.

### Car I2 stub — quick open + command palette

`Ctrl+P` fuzzy file finder (rides I1's walker for the file list +
app-local fuzzy scoring; the `qmn::fuzzy` promotion decision is taken
AT this car: if quick-open AND the palette AND a search-boost all use
the scorer, it promotes per the law — headless QtCore lib with a
subsequence-soundness property test vs a brute-force reference, fixed
seed). `Ctrl+Shift+P` over the window's own command registry. Symbol
quick-open decides here: grep-style vs the ledger's
`requestWorkspaceSymbol` door. Chord-table law: NEVER re-bind a frozen
view chord (F7 is diff-nav since S1 — the IDE's palette chords must
mask against the view's keymap table at I2 opening).

### Car I3 stub — LSP depth + outline

Rename (F2), references (land in the SAME results panel — the panel's
second user, promotion pressure noted but a WIDGET is not a library),
definition jump, code actions, formatting — ALL through the EXISTING
nine `request*` doors (the FeatureHandler shape is fine in C++; nothing
crosses to Python and the rejected-log diff must show ZERO new
drop lines from I3's app code). The outline dock needs the ledger's
`LspHub::requestDocumentSymbol` — the gate's first shelf widening:
additive, plain-Qt record list, fakeserver far-side leg in tst_lsp
(grows machine-by-machine, the inside-car rule), the three-copies +
rejected-log run BEFORE deciding whether it crosses the wheel NOW or
the door rides C++-only until a Python consumer asks (the honest
default: door exists, wheel crossing is a MINOR shelf bump, decided at
that car's close).

### Car I4 stub — tasks + problem matcher

`.qmn/tasks.json` (label/command/cwd/problemMatcher) — rides the
ProjectSession config-fold precedent's file layout. The runner is app
QProcess; the OUTPUT panel tries the dogfood bet (an editor view as
the output pane IF the read-only surface exists — ledger otherwise).
The problem matcher parses compiler-shaped lines into the SAME
`FileMatch` records (the engine's third user — this is `src/search/`
earning its keep as a lib) fed as diagnostics through the M5 door.
`tools/` gets no new spawn rule: tst_ide drives one task against a
CI-built fake command (the dapfake spawn class), and the repo-building
itself is a MEASURED record (the ninja task run against this repo,
errors deliberately seeded in a scratch file).

### Car I5 stub — git gutter

`git diff` SUBPROCESS (no libgit2 — the dependency bar stays what CI
already ships) → the hunks compared with the editor's ORIGINAL side
through `qmn::diff` (the layer earns a third user) → gutter stripes
through the decoration API after checking whether the five shipped
records already cover a line-stripe kind (ledger entry if not —
additive, and M5 records are view-nested so the three-copies question
fires immediately if it crosses). Git status colors in the file tree.
BLAME is OUT (its own future train if ever). Tests run against a
`QTemporaryDir` scratch repo (init/config/commit) — git IS
CI-safe-when-built class, but the no-guarantee law still rides: the
slot SKIPS honestly where git is absent, pinned skip, not a fake pass.

### Gate 4 risks

1. **Scope creep toward 'real VS Code'** — the train's product is the
   library's honesty, not feature parity; anything that would BREAK a
   frozen door stops at the door (the ledger grows; nothing else).
2. **Mid-car promotion surgery** — the second-user temptation attacks
   DURING a car (the results panel, the settings dialog, the tree);
   promotions land only at car boundaries with their own record.
3. **Search perf cliffs** — the walk caps/skips are ENGINE CONTRACT,
   tested; wall-clock is a bench tool + a measured record, never a
   flaky ctest pin (the recorded timing class).
4. **ctest creep** — tst_ide must not quietly become a second
   tst_glue-with-servers; spawn is legal only for CI-built far sides
   (dapfake class), clangd/live adapters stay out.
5. **UI without vision** — every IDE pin is state/numeric (the No-
   vision law); 'looks right' is never a DoD.
6. **Ledger rot** — an entry opened must close in a named car or be
   explicitly carried; a hole found and not written down is worse than
   the hole.

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



