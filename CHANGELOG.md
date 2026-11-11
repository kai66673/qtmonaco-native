# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- Phase F C++ packaging: `install(TARGETS)` + exported namespace `qmn::`
  (`qmn::core`, `qmn::view`, internal `qmn::internal_tree_sitter`),
  `qmnConfig.cmake`/version file (`find_dependency(Qt6 6.6)`), public-
  header allow-list (HighlightEngine/QueryData tree-sitter seam and
  CodeEditorAccessible deliberately not installed), and an external
  consumer smoke (`examples/consumer/`, `find_package(qmn)` against the
  INSTALLED package only; also in both CI jobs) exercising language
  doors, undo, options, decorations and the a11y interface across the
  export boundary. STATIC archives shipped instead of shared (SOVERSION
  is a no-op there — documented deviation from PLAN's freeze line); the
  vendored tree-sitter PRIVATE-static export hole (CMake drops a private
  archive from another STATIC lib's export) closed by shipping
  `libqmn_ts.a` inside the package and re-referencing it from qmn_view
  via `$<INSTALL_INTERFACE:qmn::internal_tree_sitter>`.
- M3 undo/redo, core + view. `TextBuffer` now owns the undo history: every
  `insertAtOffset`/`removeRange` is recorded as an op (`start/end/removed/
  inserted`), grouped into undo steps with auto-coalescing — typing bursts
  merge into one step (64-code-unit cap, newlines break the chain),
  Backspace/Delete runs merge into one chunk without a size cap, and only
  same-kind ops ever coalesce. `beginEditGroup`/`endEditGroup` bracket a
  compound user action into one atomic step (nesting depth-counted, bracketed
  groups never coalesce), `breakUndoCoalescing()` freezes the top chain, and
  `undo()/redo()/undoDepth()/redoDepth()/undoPushSerial()` drive the stacks.
  History caps at 2048 groups / 32 MB of logged text, dropping oldest groups;
  `setText()` wipes both stacks.
- `CodeEditor::undo()/redo()/canUndo()/canRedo()` bound to Ctrl+Z,
  Ctrl+Shift+Z and Ctrl+Y. The view mirrors the core stacks one mark per
  group, so undo/redo restores the caret — and the selection, when the undone
  action consumed one. Caret motion (arrows, click, drag, `setPosition`,
  Ctrl+A) freezes the typing chain, so click-then-type is its own undo step;
  paste/cut are bracketed into single steps, Backspace runs coalesce, Enter
  breaks chains.
- Multiple carets (Phase D1). Alt+Click adds a caret (the clicked caret
  becomes PRIMARY, the old one demotes to an extra); Alt+press on an
  existing caret arms an Alt+Drag that selects inside that slot; plain
  click, middle click, `setPosition()`, `selectAll()` and `setPlainText()`
  collapse the set back to the primary caret. Escape is two-stage: the
  extras drop first, the primary selection second; a third Escape passes
  through. Typed keys, Enter, Backspace/Delete, Ctrl+Backspace/Ctrl+Delete
  (word runs), clipboard cut/paste and every navigation key fan out per
  caret; copy joins ALL slots' selection texts (or the distinct caret lines
  when nothing is selected) with `\n`. One key press is ONE bracketed undo
  group across all carets, and undo/redo restores every slot (the undo mark
  carries the extras). Public surface: `additionalCaretCount()`,
  `additionalCarets()` (doc order), `clearAdditionalCarets()`.
  Normalization after every gesture/edit: exactly-stacked slots fold (the
  primary survives), a selection contained in another dies, a bare caret
  strictly inside another slot's selection dies.
  Deliberate v1 divergences from VS Code, pinned by tests: overlapping
  per-caret edits keep the RIGHTMOST edit and drop the colliding leftward
  one (no merge); paste puts the SAME text at every caret (multi-line
  clipboards are never distributed line-per-caret); whole-line cut lands
  stacked carets at the merged block start and folds them in one action;
  Alt+click does not remove a caret (no toggle); a reverse selection
  extended past its own anchor collapses to zero-length instead of
  re-orienting.
- M4 syntax highlighting (Phase D2), vendored tree-sitter. `third_party/
  tree-sitter/` vendors runtime v0.27.2 (single amalgamation TU) plus
  generated parsers python v0.25.0, json v0.24.8, javascript v0.25.0 with
  their upstream `highlights.scm` (embedded into the build as raw strings;
  pins + sha256 in `VENDORED.md`; only json's copy is locally reordered —
  the pair-key capture moved last — to fit the engine's merge convention).
  The engine, `qmn::view::
  HighlightEngine` (`qmn_ts` stays PRIVATE to `qmn_view`; `src/core/` stays
  QtCore-only), feeds the parser zero-copy from the piece table via
  `TSInputEncodingUTF16LE` — tree-sitter byte offsets are exactly 2× model
  offsets, no transcoding. Every primitive core edit becomes one
  `ts_tree_edit` immediately (new QtCore-only core hooks:
  `setEditObserver` firing one `EditEvent` per piece-table mutation,
  `pieceAt`/`PieceView` zero-copy reader); the reparse itself rides the
  next `paintEvent`. Bench-settled adaptive policy (no threads for editor
  v1 — threading deferred to M13; `tools/bench_ts_split.cpp` measured
  python's incremental full reparse at ~23 ms/keystroke vs ≤0.7 ms for
  viewport-range parses): full-incremental until the measured parse
  exceeds an 8 ms budget, then viewport-range parses anchored at the
  nearest col-0 non-blank line; capture queries are window-scoped
  (`ts_query_cursor_set_byte_range`) on any document ≥ 16k UTF-16 units.
  The policy is invisible through the public API: `highlightSpans()`
  always returns exact spans for the requested window, escalating with at
  most one synchronous full-document pass (`HighlightEngine::spansFor`)
  — pinned by `largeDocViewportPolicyKeepsSpansExact`. Public view API:
  `setLanguage(id)`/`language()`/`availableLanguages()` (python, json,
  javascript; unknown ids fall back to plain text), `highlightSpans(
  start, end)` returning `HighlightSpan{start, end, kind}` in model
  offsets, and `TokenKind` (24-value Q_ENUM: the union of the vendored
  grammars' capture names; the view assigns roles, themes own the colors). Painted colored runs keep the single-layout invariant
  (`QTextLayout::draw` with `FormatRange`s, run edges snapped to grapheme
  cluster boundaries). Captures merge paint-over: later-starting (inner)
  captures win; identical spans resolve to the LATER query pattern —
  tree-sitter master's conflict convention, the one the current upstream
  py/js queries are written for (catch-all `(identifier) @variable`
  first). The v0.27.2 runtime does not evaluate query predicates (its
  cursor emits every syntactically matching capture, probe-verified), so
  the engine applies `#eq?` (exact text equality) and `#match?`
  (unanchored regex search) itself; directives like `#is-not? local`
  pass as satisfied. Pinned by `queryPredicatesFilterRefinedRoles` and
  `jsonKeyQueryPatternWinsOverPlainString`. A/B against the pre-flip
  engine (`tools/bench_highlight`, 100 KB python doc): per-keystroke
  `edit+refresh` mean 9.4 ms vs 7.7 ms, i.e. engine predicate filtering
  costs ≈ +1.7 ms/keystroke there — accepted for correct role resolution.
  Also public: read-only viewport accessors `firstVisibleLine()` and
  `visibleLineCount()`.
- `examples/` highlighting gallery: one demo document per pinned grammar
  (`demo.py`, `demo.json`, `demo.js`), dense in exactly the token roles
  their vendored `highlights.scm` emits — `examples/README.md` carries the
  measured per-grammar role tables and documents which upstream roles
  never fire for structural query reasons (json punctuation, python
  parameter names and `class` header names, js formal parameters).
  `qmn-spike` now picks the grammar from the file suffix
  (`.py`/`.json`/`.js`/`.mjs`/`.cjs`) and accepts `--language
  python|json|javascript` to override.
- M5 decorations + line-number gutter (Phase D3), the design-forward
  Gate-2 bridge: three layers that must express diagnostics, breakpoints
  and inlay hints without new view primitives. Gutter glyph layer
  (breakpoint dots + diagnostic severity icons; fold triangles join at
  M13), whole-line bands under selections (qtmonaco's highlighted-lines
  pair + an opt-in current-line band), inline layer (severity wavy
  underlines, inlay hints painted WITHOUT re-flow — adding re-flow later
  is a rendering refinement, not an API change). All APIs are batch
  replace-all (add = set, update = set again, remove = set without the
  item), so M8/LSP and DAP glue stay thin consumers.
- `setLineNumbersMode(LineNumbersMode::{Off,On,Relative,Interval})`,
  default On (Monaco parity, qtmonaco's `set_line_numbers_mode` row);
  `lineNumberText(line)` exposes the displayed string (Relative =
  distance to the caret line, caret line itself absolute; Interval =
  only 1-based multiples of 10; Monaco behaviors). The digit column is
  `qRound(digits * cellWidth)` and grows at the 9→10-line crossover;
  one breakpoint slot is reserved while ANY breakpoint exists (dots
  never shift the digits), while diagnostic icons ride in the digit's
  right padding — LSP state never re-lays-out the gutter. New x anchor
  `contentLeft()` (= padding + gutterWidth, 0-width when Off) drives
  caret/selection/paint/hit-test, `xForCol`, `positionAt` and the IM
  cursor anchor alike; clicks inside the gutter resolve to col 0.
- Public decoration state: `setDiagnostics(QVector<Diagnostic>)`
  (range + `Severity::{Error,Warning,Information,Hint}` + message held
  for consumer hover, zero-width ranges paint one phantom cell, stored
  sorted), `setBreakpointLines()` (sorted + deduped), `setInlayHints()`
  (sorted by line,col), `setHighlightedLines(start,end)` /
  `clearHighlightedLines()` (inclusive 0-based pair, swapped args
  tolerated; binding converts qtmonaco's 1-based pair),
  `setCurrentLineHighlight(bool)` (off until the set_theme pass picks
  the default). Decoration ink (severity colors, breakpoint red) is a
  fixed VS Code dark constant until `set_theme` routes it through the
  palette.
- M6 find/replace (Phase D4). Public API: `setFindText`/`findText`,
  `setFindFlags`/`findFlags` (`FindFlag::{MatchCase,WholeWord,UseRegex}`,
  Q_FLAG trio, default MatchCase), `findPatternValid()`, `matches()`
  (full sorted `MatchRange` list as model ranges — D4 DoD), `matchCount()`,
  `currentMatch()` (the EFFECTIVE index: counts only while the selection
  covers the match, -1 after any caret move — read-only, never mutated by
  paint/label refreshes), `gotoMatch(index)`, `findNext()`/`findPrevious()`
  (wrap, F3/Shift+F3), `replaceNext(replacement)` (own undo step, caret
  lands at the replacement end, never wraps behind your back),
  `replaceAll(replacement)` (the whole batch is ONE bracketed D1 action;
  returns the count), `showFindBar`/`hideFindBar`/`toggleFindBar`/
  `findBarVisible`/`clearFind`. Shortcuts via standard-key matching:
  Ctrl+F, Ctrl+H, F3, Shift+F3 — they work with the bar closed too (the
  query persists, Monaco parity). The bar is a file-local `FindBar` child
  widget (anon namespace, objectName `qmn-find-bar`) talking only through
  the public API: query + replace rows, Match Case / Whole Word / .*
  toggles, prev/next ▲▼, Replace / All, × close, and a `n/m` count label;
  Esc closes it, the state persists for reopen; top-right anchored
  (4px margin, clamps to a 300px floor at narrow widths — controls
  compress, the bar never vanishes). The D4 confirm step: **Replace All
  is button-only** — Enter/Shift+Enter navigate, Enter in the replace
  row replaces the current match, no key combination ever fires the batch.
- Engine semantics (documented v1 contract): PER-LINE search — matches
  never cross a line break (a pattern mentioning one matches nothing);
  literal queries escape into the same `QRegularExpression` path;
  WholeWord wraps `\b(?:…)\b` (regex-class `\w`, the documented v1
  approximation of Unicode-aware boundaries); an invalid pattern clears
  the match set and `findPatternValid()` tells "invalid" from "no
  matches". Surrogate safety: match bounds snap OUTWARD over split
  surrogate pairs, snapped-equal duplicates merge (PCRE2 steps through a
  pair one unit at a time), and the scan advance is
  `max(rawWidth, 1, from + 1)` — so replacing a match never orphans half
  a pair (surrogate replaceAll pinned by test). Matches recompute
  lazily: any edit marks the set stale and the next paint or find action
  refreshes it (the same at-most-once-per-keypress cadence as the M4
  highlight engine; the set follows Ctrl+Z/Ctrl+Y through the M3 edit
  observer). Painting rides the decoration pass: the match list is
  sorted and the paint binary-searches the viewport window
  (viewport-only renderer preserved).
- M7 overview ruler (Phase E1), the native minimap substitute: a fixed
  44px right-edge strip (`kOverviewRulerWidth`) mapping the WHOLE document
  pixel space `[0, 2*padding + lineCount*lineHeight)` linearly onto the
  strip height — one ratio drives the click gesture, the viewport frame
  and the paint, so the three can never drift apart. Public API:
  `setOverviewRulerEnabled`/`overviewRulerEnabled` (default ON, Monaco
  minimap parity; the binding maps qtmonaco's `set_minimap_enabled` here,
  never onto a Chromium token minimap), `overviewRulerWidth`,
  `overviewRulerGeometry`, `rulerLineAt(y)` and
  `overviewRulerViewportRect` (pure mapping math, the geometry-state
  test surface), plus the `verticalScrollOffset()` viewport getter.
  Gestures: left/middle click or drag inside the strip scrolls
  (clicked line centered, Monaco click-to-center semantics) and never
  moves the caret; middle click never pastes there; the vertical
  scrollbar overlays the strip's right edge, VS Code-style.
  Cost is O(strip height), never O(document): line stripes step by
  `ceil(1/pxPerLine)` (bounded by strip rows), each line read is a
  capped `textRange` window (a 1M-unit line costs one small slice), and
  huge marker sets (breakpoints/diagnostics/find matches) are
  stride-subsampled. Marker layers reuse the M5/M6 inks; ruler chrome
  is deliberately cool-ink (R==G==B) so the warm/red numeric pixel probes
  keep exact-zero negative controls. Painted as the LAST pass over the
  text's right edge. Tests: mapping-vs-duplicated-formula equality per
  strip row, viewport-rect formula across wheel scrolls, disabled-state
  empties, click/drag scroll state assertions, strip-window pixel probes
  (breakpoint red / highlight cool-blue / find warm bands light exactly
  one predicate each).

- M9 horizontal scrolling (Phase E1): the horizontal half of the viewport
  story (soft wrap stays deferred to Gate 2). The text window runs from
  `contentLeft()` to the overview-ruler chrome; the pixel offset lives in
  the same integer space as vScroll and shifts every text-space x (match
  quads, selections, carets, preedit, squiggles, inlay hints — the gutter
  and ruler are chrome and never ride it). Public API:
  `setHorizontalScrollOffset`/`horizontalScrollOffset`,
  `horizontalViewportWidth()` and `maxHorizontalScroll()` (the clamp
  contract: max = docWidth + padding - window), plus the qtmonaco-parity
  hook `setScrollBeyondLastLineEnabled`/`scrollBeyondLastLineEnabled`
  (Monaco's scrollBeyondLastLine; on, totalHeight gains
  `height()-lineHeight_` so the LAST line reaches the viewport top — and
  one formula keeps the vBar clamp, firstVisibleLine and the ruler
  mapping consistent). The doc width is AMBIENT: a visited line's
  phantom-aware px width (same xInLine math as the hit test, exact
  `col*cellWidth` on monospace) joins the extent — the view NEVER walks
  the whole document for the widest line (same no-O(document) rule as the
  ruler); stale widths decay the same ambient way, and `setPlainText`
  resets offset and extent together. `scrollBeyondLastLine` defaults OFF
  — a deliberate divergence from Monaco's ON default that keeps every
  existing end-of-scroll/ruler contract bit-stable; revisit before the
  API freeze. Input: trackpad `angle.x` and Shift+wheel (3 cells per
  notch, mirrors the hBar singleStep; under Shift the vertical axis swaps
  to horizontal, it does not stack), caret-keep-visible flush-to-edge on
  arrows/End/Home (monospace-exact: `ceil(caretX - win + 1)`), horizontal
  drag auto-scroll at the window edges, and an h QScrollBar that stops at
  the ruler chrome (the vBar lives inside the strip's right edge,
  VS Code-style). Paint is windowed: each line pass is clipped to the
  text window and lines fully scrolled past are skipped in O(1) (the
  hitLine visit keeps feeding the ambient extent even for skipped lines).
  Documented v1 trade: per-line glyph tessellation is bounded by the
  `geoMaxUnits` layout budget (~4 viewports wide); past it the far-right
  region of monster lines carries caret/selection/hit-test geometry only
  (phantom cellWidth extrapolation), no painted glyphs. Tests:
  ambient-extent seeding + exact clamp contract, per-arrow-key closed-form
  caret-keep offsets (120 steps), wheel/Shift-wheel axis semantics,
  click mapping through the offset, scroll-past-end flag + ruler-total
  ride-along, and a pixel-band probe (zero-ink control at h=0, then Q
  glyph / selection blend / 2px caret bar ink exactly at predicted
  shifted widget x).
- M13 code folding (Phase E2). Fold ranges come from tree-sitter `@fold`
  captures per grammar (python/javascript/json node lists composed into one
  query by `composeFoldQuery`; every name is filtered through
  `ts_language_symbol_for_name`, so a grammar rename just drops that node
  instead of failing the query); the view's indent fallback
  (`indent(L+1) > indent(L)`, blanks count as no-indent) runs in
  grammar-LESS modes only — with a fold grammar active, nesting is what
  the brackets/syntax say (see Fixed). Public API: `foldableRanges()`,
  `isFoldable(line)`, `isFolded(line)`, `foldLine/unfoldLine/toggleFold`,
  `collapseAll()/expandAll()`, and the ONE view-line map everything routes
  through: `visibleLineTotal()`, `visibleIndexOfModelLine(line)` (-1 =
  hidden), `modelLineForVisibleIndex(i)` (clamped both ways). Painting,
  caret navigation (Up/Down/PageUp/PageDown step SLOTs, jumping a whole
  fold as one unit), selection, gutter, ruler, the v-scroll space
  (`totalHeight()` rides slot space: hidden runs cost no pixels) and every
  hit test ride the map — a click or arrow key can never address a hidden
  line.
- Fold UX: a fixed 18px fold zone at the gutter's right edge
  (`foldZoneWidth()`; gutter Off removes the chrome but the fold API keeps
  working) with ▾/▸ triangles in a fixed cool gray (the warm/red
  decoration pixel probes stay green). Zone clicks toggle WITHOUT moving
  the caret; non-foldable rows fall through to plain caret placement and
  Alt+Click keeps its multi-caret meaning there. Keyboard: Ctrl+Shift+[/]
  fold/unfold at the caret, Ctrl+Alt+[/] collapse-all/expand-all (bracket
  or brace spelling, Meta noise tolerated; the caret is never moved).
  Reveal rule, ONE guard (`ensureCaretVisible`): a caret route that names
  a hidden line opens the containing fold first — and since mouse/keyboard
  are map-routed, only programmatic `setPosition` can name one; a drag
  across a fold never opens it, the selection just spans the hidden text.
- Fold edit bookkeeping (fold state itself is NOT an undoable action —
  documented v1 stance — but edits and undo keep it consistent): the
  buffer observer shifts collapsed ranges by the op's net line delta —
  folds entirely below the edit line shift whole, folds containing it
  shift their END only (a join at the header EOL shortens the body and the
  fold survives), a fold whose end collapses onto its start dies, and
  `setPlainText` resets all fold state. Paint stays O(viewport): foldable
  STARTS resolve inside the paint window (indent ends stay lazy — the
  triangle only needs the start), whole-document fold queries run only on
  explicit user actions (`foldLine`/`collapseAll`), never in a paint pass.
  The ruler rides the same map: its M7 contract recomputes over slot
  space, so hidden lines cost no strip rows and a strip row never maps to
  a hidden line.
- M14 Tab indentation + auto-indent (Phase E3). Soft tabs only: public API
  `setTabStopWidth(int)` (clamped ≥1, default 4 — Monaco's tabSize; a
  change resets the geometry cache and the painter's `QTextOption` tab
  stops, so hit tests and painting agree with the model),
  `setAutoIndentEnabled(bool)` (default ON) and `setTabChangesFocus(bool)`
  (default OFF; ON makes Tab/Backtab `event->ignore()` and hand off to Qt
  focus traversal without touching the model). Plain Tab with one caret
  pads to the next stop boundary right of the caret's VISUAL column
  (a literal `\t` left of the caret counts as its grid advance, a
  mid-band caret only crosses the next boundary); a single-line selection
  under the key is REPLACED, bracketed as one compound undo step, while a
  plain pad stays unbracketed so a Tab burst coalesces like typing. Any
  multi-line selection or extra carets route to `blockIndent()`: every
  line any caret's selection touches gets one stop, MIDDLE lines included,
  the end line excluded only when its END sits at col 0 (the selection
  ends ON it, it doesn't touch it) — one bracketed group, one undo step,
  and every caret remaps through the applied edits (inside a removed
  prefix collapses to the edit start). Shift+Tab is VS Code's "unindent":
  always `blockOutdent()` of the touched whole lines — width w retires to
  `((w-1)/stop)*stop` (a whole stop from a boundary, just the overshoot
  mid-band), a tab never splits, and a col-0 doc records nothing, not even
  an empty bracket. Enter's payload is policy-computed per caret
  (`autoIndentNewline`): leading whitespace rides over, one more stop when
  the caret sits after a trailing `{([:` (whitespace-skipping probe; Enter
  AT col 0 stays plain), and a caret flush inside an empty pair splits it
  — `{|}`, `}|{`, `{|` at EOL (the payload CONJURES the missing closer)
  and `|}` at col 0 — the closer parks on its own line at the parent
  indent and the caret stays on the child line (per-slot `caretTrim` on
  the fan-out commit parks each caret mid-payload); Alt+Enter is the
  documented escape hatch — always a plain `\n`, never the policy. All
  indent math is UTF-16 code units, same as everywhere else.
- M15 caret/mouse gestures (Phase E3, no new public API — behavior only,
  deliberately). Smart Home: Home lands on the first non-blank, and col 0
  only when the caret is ALREADY at the first non-blank — the target is
  derived from the caret position, so there is no smart-Home editor state
  and every other caret motion resets the toggle (unindented lines are
  plain Home and idempotent; blank/EOL targets too; applies per slot
  under multi-caret; End untouched). Double-click selects the word under
  the pointer via the M2 class runs, but as ONE pure run: `wordSelRange`
  pivots right (the run starting at/right of the hit) — pairing
  wordLeftCol with wordRightCol at a class boundary spans TWO runs (
  wordLeftCol skips the space AND the word before it, that is Ctrl+Left
  navigation, not a selection bound), so double-clicking the space
  between words selects exactly that space run; at EOL the run left of
  the hit wins, word-less spots just re-place the caret (Monaco parity).
  Chaining: Qt folds a fast click run into DblClick events, so the chain
  is detected from event timestamps + line + x (slop max(8, cellWidth))
  against `QApplication::doubleClickInterval()`; a re-press at
  mousePress resets it. Triple-click (2nd chained DblClick) selects the
  whole line INCLUDING the trailing newline (caret at next-line col 0;
  last line stops at EOL), and 4th+ clicks grow the span one line per
  click, flipping direction at document edges. A double-click arms a
  WORD DRAG: the selection always covers whole words at both ends (pins
  = the double-clicked word, plus the word under the pointer), release
  disarms and further drags are char-wise again. Ctrl+D (Ctrl only,
  Alt/Meta stay unclaimed): target = the primary selection's text, or
  the caret's word when nothing/whitespace/multi-line is selected (v1:
  occurrences are single-line, plain case-sensitive substring — never
  regex), scanned line-by-line strictly right of the rightmost slot
  edge, never wrapping; a hit becomes a NEW SELECTED extra slot
  (additionalCarets() shows the match END; the new slots are real carets
  so typing fans out over all of them as one undo step), a caret parked
  at its own word start CONVERTS the primary into that word's selection
  instead (VS Code first-press parity, no duplicate slot), and no match
  means: with a selection a no-op (this was the last match), without one
  the caret's word gets selected. The ruler/fold chrome keep their
  gestures: double-clicks there accept-and-no-op, Alt+click double
  events never multi-click. Tests gained a TextPosition QDebug streamer
  so QCOMPARE failures print {line,col} instead of <null>.
- M16 context menu + text DnD (Phase E3, no new public API — user-facing
  behavior; the menu factory + caret-placement rules sit under `protected:`
  as a test seam only, invisible to the binding). Right-click opens a
  QMenu(Cut/Copy/Paste/Select All) whose actions reuse the exact Ctrl-chord
  implementations (M1b whole-line semantics included); Paste alone gates
  on the clipboard having text — and `QClipboard::mimeData()` is NULL
  until the session fills it (and again after a clear(), measured on the
  offscreen plugin), so it is null-checked before deref. Caret rules: a
  right-click outside the selection re-places the caret first (Qt
  convention), inside — inclusive of both edges — keeps the selection
  intact for the menu; ruler and fold-zone right-clicks stay chrome
  guards (accept, no caret, no menu). Drag-out: a press held inside the
  live selection does NOT collapse it; sub-`startDragDistance()` moves
  are swallowed, a never-grown release collapses at the release point,
  and past the threshold a QDrag starts with mime = plain text PLUS a
  private `application/x-qmn-selection` marker
  (`widgetAddressHex:startOffset:endOffset`), `exec(Copy|Move,
  preferred=Move)` (Ctrl at the drop degrades to copy). Drop-in: any
  text payload is accepted (urls count as text — Qt mirrors the uri-list
  into `text()`, so a file-manager drop lands its path); CRLF is
  sanitized like the paste path; a drop over gutter/ruler clamps to the
  text edge via `positionAt` like a click. Internal MOVE (own marker AND
  marker offsets == live primary selection — a foreign widget's or a
  stale selection degrade to copy): the del+ins pair goes through ONE
  bracketed `applyBlockEdits` step (caret pre-parked at the drop offset,
  the remap lands it at the block's end); a drop STRICTLY inside the
  source range is a special-cased caret no-op — the overlapping pair
  must never reach `applyBlockEdits`, whose contract is non-overlapping
  edits. Drag-moves ride `edgeAutoScroll` (the M1 selection-drag helper)
  for autoscroll. Offscreen test traps proven along the way: Qt 6.12's
  `QApplication::notify` routes synthetic DragMove/Drop only to the
  QDragManager's currentTarget (an accepted DragEnter first, DragLeave
  to clean up), and `QDrag::exec` returns NoAction synchronously there.
  9 new slots (161/0/2).
- M17 code font + zoom API. `CodeEditor::setCodeFont(QFont)`/`codeFont()`
  and `setZoomFactor(double)`/`zoomFactor()`/`zoomIn()`/`zoomOut()`/
  `resetZoom()`, with the ladder published as public
  `kZoomFactorStep`/`kZoomFactorMin`/`kZoomFactorMax` (1.25, clamped to
  [0.25, 4.0]). The BASE font is what the setters store; the widget's
  QWidget::font() always mirrors the EFFECTIVE font — base scaled to an
  integer pixel size (pointSizeF as fallback) with PreferNoHinting imposed,
  so the cell-width arithmetic never drifts. Changing either re-measures
  cellWidth/lineHeight/ascent and wipes the geo-cache outright (the font
  is NOT in the cache key); gutter, fold zone, contentLeft, scroll ranges
  and hit testing all re-read the live metrics with no extra invalidation
  of their own (the ambient doc-width extent deliberately stays, M14
  precedent). Chords (VS Code's table, deliberately NOT
  QKeySequence::ZoomIn/Out — the offscreen default table misses the
  canonical Ctrl+=, measured): Ctrl+= / Ctrl++ (Key_Equal|Key_Plus),
  Ctrl+- / Ctrl+_ (Key_Minus|Key_Underscore), Ctrl+0 and the numpad
  Ctrl+. reset — Qt 6.12 has no Key_Kp* constants, the keypad delivers
  the same codes with Qt::KeypadModifier (tolerated), Alt keeps combos
  unclaimed. Ctrl+mouse-wheel is the FONT zoom here (no Chromium-style
  chrome zoom exists): sub-notch trackpad deltas accumulate to whole
  steps, at most 5 per event, and the scroll legs stay fully suppressed;
  a plain wheel resets the accumulator. A raw QWidget::setFont from
  foreign code REBASES: changeEvent sees a font that is not exactly
  scaleFontForZoom(base, factor), adopts it as the new base and returns
  the factor to 1.0, so codeFont() never lies about what is drawn.
  Shiboken-clean (QFont/double only). 6 new slots (167/0/2).
- **M18 auto-close pairs + long-line ruler + current line.** Typing one
  of `()`/`[]`/`{}`/`""`/`''`/`` `` `` (VS Code parity) is claimed by
  `handleAutoClosePair`: per slot it decides Skip > (quote guard) > Wrap >
  Pair > Plain. Skip (closer already at an unselected caret) is pure
  caret motion — no edit, no undo entry, and `moveCarets` grew a
  `freezeChain` param precisely so the hop also leaves the coalescing
  chain alone; a typed closer never wraps or pairs. The quote word-guard
  (`isLetterOrNumber || '_'` touching either side) applies at a BARE
  caret only — prose `don't` stays one char, wrap is never gated. Wrap
  replaces the selection with open+sel+close, caret after the close, ONE
  bracketed step (undo restores the selection whole). Multi-caret: all
  Plain bails to the ordinary typing fan-out, all Skip is a single
  motion pass, mixed rides ONE bracketed fan-out where Skip slots take a
  net-zero replace (closer removed + reinserted identical) to park past
  it — one Ctrl+Z for the whole batch. Backspace with the caret between
  an empty pair removes BOTH sides (textual rule, `pairAroundCol` —
  works on typed/pasted pairs too; single path coalesces, fan-out rides
  the per-press group). `setLongLineRulerColumn(int)` (0=off default,
  negatives clamp, getter verbatim) paints a 1px Text@40 guide at
  `contentLeft + qRound(col*cellWidth) - hScroll`, full height, clipped
  to the text viewport (never under gutter/overview-ruler chrome;
  ambient — no geo-cache or scroll-range change). `CurrentLineMode
  { Off, Background, BackgroundFrame }` (Q_ENUM) replaces the M5 bool:
  default `Background`, wash ink Text@16→30, BackgroundFrame adds a
  cosmetic Text@90 band outline; the old bool setter/getter survive as
  one-line delegates. Shiboken-clean (bool/Q_ENUM/int only). 7 new
  slots (174/0/2).
- **M19 `CodeEditorOptions` (settings object).** `CodeEditor::options()`
  returns a parented child QObject carrying TEN knobs, each with a
  `*Changed` signal: the nine existing editor knobs (lineNumbersMode,
  currentLineMode + the currentLineHighlight bool delegate,
  longLineRulerColumn, tabStopWidth, autoIndentEnabled,
  tabChangesFocus, overviewRulerEnabled, scrollBeyondLastLineEnabled)
  plus TWO options-only settings (the M19 rule: new settings live
  only here): `autoCloseEnabled` (default ON; OFF degrades pair
  typing to plain single-char typing — Pair/Wrap/Skip all die, the
  textual "backspace between an empty pair removes both" rule stays,
  it never claimed pairing) and `setReadOnly`/`isReadOnly` (default
  OFF; VS Code model: EVERY user edit gesture is refused — typing
  incl. IME commit, Enter, Tab/Shift+Tab indentation, Backspace and
  Delete incl. the Ctrl word variant, paste, cut (the clipboard
  stays untouched too), DnD drops (refused at the drag-enter door),
  find/replace ACTIONS, undo/redo — while caret navigation,
  selection, copy, finding and programmatic setPlainText stay fully
  alive; the applyCaretsFanOut/applyBlockEdits entry gates are the
  backstop: a forgotten front door cannot mutate, and callers'
  captureEditMark/reconcile see the zero depth delta and push no
  undo mark). DESIGN note (deliberate deviation from PLAN's literal
  "options-as-store / flat setters become delegates" wording,
  mirror-proxy instead): the EDITOR keeps every value and every
  setter body verbatim — the options setters are one-line
  delegations INTO it (so validation, clamping and invalidation are
  inherited for free and options getters can never drift stale),
  and each editor setter ends `emit options_->xChanged(value)` —
  ONE notification no matter which door the change came through,
  same-value sets stay silent (the early-return guards predate the
  emit), clamped values are notified as-applied. No Q_PROPERTY
  (moc/Shiboken discipline: plain methods + Qt6 signals; the Q_ENUM
  knobs ride QSignalSpy metatypes). 3 new slots (177/0/2).
- **M11 accessibility bridge (QAccessible).** `CodeEditorAccessible`
  (view layer) makes the editor legible to assistive technologies: a
  `QAccessibleWidget` + `QAccessibleTextInterface` + `QAccessible-
  EditableTextInterface` with role EditableText, Name "code editor"
  (a widget property), Value = the whole document (Qt's multiline-
  editor convention) and state {selectableText, multiLine, editable}
  — the M19 lock flips the state to {readOnly, !editable} through the
  options door while the programmatic Value WRITE door keeps working
  (same split as M19). Registration is automatic and needs no app
  wiring: the CodeEditor ctor calls an idempotent `install()` that
  registers one factory keyed on `CodeEditor::staticMetaObject
  .className()` — NOT a hand-spelled "CodeEditor": Q_OBJECT in a
  namespace makes className() QUALIFIED, and the metaObject chain
  means subclasses hit the same entry. Offsets are the RAW document
  frame: 0-based UTF-16 code units INCLUDING the '\n' separators
  (exactly TextBuffer's own frame — `text(from,to)` is `textRange`
  verbatim, clamps and all), no line-number/offset translation layer.
  Boundaries are native over the line model (never materialize the
  whole document): Char/Line/Paragraph native; Line/Paragraph spans
  INCLUDE the trailing '\n' and the before-frame is the Qt default's
  EXACT semantics (line-by-line verified against 6.10's
  textLineBoundary helper: the span terminated by the LAST '\n'
  strictly before the offset; offset ON a '\n' of line 0 fails);
  Word is LINE-LOCAL (letter/number + '_' runs; a '\n' ends the hunt
  — deliberate Gate-1 simplification, documented in the header);
  NoBoundary yields the whole prefix/suffix/document (the Qt default
  fails NoBoundary-before — our friendlier choice, documented);
  Sentence delegates to the Qt base (rare for code, pays the
  whole-doc detour once). The -1 (document end) / -2 (cursor) offset
  conventions and the failure frame ("" with *start=*end=-1) follow
  Qt. Selection is the ONE primary selection (count 0/1): extra
  carets stay AT-invisible (Gate-1), setSelection/addSelection drive
  the primary (reversed ends sort, caret rides endOffset),
  selection(idx!=0) answers the -1/-1 frame. Geometry rides the M13
  view map: characterRect is the GLOBAL rect of the cell (fold-aware
  y through visibleIndexOfModelLine, x through xForCol; EOL/'\n'
  cells clamp to 1px wide), offsetAtPoint is its inverse
  (mapFromGlobal + positionAt), scrollToSubstring is the ensureCaret-
  Visible flush math WITHOUT moving the caret, attributes() returns
  the empty role map with the line's span sans '\n' (Gate-1: no
  syntax roles across the a11y boundary yet). The editable interface
  routes through the editor's own caret doors: insert/replace/delete
  honor the M19 readOnly refusal, ride the undo brackets (Ctrl+Z
  undoes an AT edit as one step) and move the PRIMARY caret (single
  documented deviation for multi-caret states). Live events: cursor-
  Moved/selectionChanged re-emit as TextCursor/TextSelection events,
  the core EditEvent observer re-emits TextInsertEvent and
  TextRemoveEvent — the Remove payload is POSITION only, no text
  (deliberate: the removed text can be MBs; AT queries what it needs)
  — all gated on `QAccessible::isActive()` so inactive apps pay zero
  per-edit cost. Event DELIVERY is not observable offscreen, so the
  4 new slots assert the state/query contracts (interface identity
  through the QAccessible cache, readOnly mirror, offset/caret/
  selection round-trips, boundary frames incl. every fail case,
  char-rect/point-hit round trip, scrollToSubstring flush math +
  caret-never-moves); 181/0/2.

### Changed
- The x anchor of all text geometry (caret, selection, painting, hit
  testing, IM anchor) moved from `padding()` to `contentLeft()` =
  padding + gutter width. With the gutter Off the geometry is
  bit-for-bit the pre-M5 position; `padding()` now means the vertical
  chrome only. Widget tests migrated to the new anchor.

### Fixed
- M13 fold asymmetry (owner-reported): an edit killing a fold's SOURCE
  (the grammar capture, or the indent pair in grammar-less modes) dropped
  the fold marker of an unfolded fold, but a COLLAPSED one kept its ▸ —
  and its hidden rows — forever, since the marker-persistence branch was
  unconditional. The persistence now has a source re-check: a collapsed
  fold whose header line is no longer foldable by the current source
  AUTO-EXPANDS on that paint, body back and marker gone, fully symmetric
  with the unfolded twin. Grammar modes validate against the windowed
  `@fold` captures; indent modes against the MODEL-level indent pair
  (the fold itself hid line+1, and blank runs skip exactly as the source
  rule does). The check rides the paint-path fold-cache recompute (per
  keystroke `foldsFull()` measured 24ms+ per key on big docs), so it
  costs one O(1) probe per visible collapsed start and never a
  whole-document fold query.
  Test: `foldAutoExpandOnSourceDeath` (both sources × dead and alive).
- M13 fold source slip (owner-reported): the first cut ran the indent
  fallback in ALL modes, so js/json got python-style pseudo-block folds
  following leading whitespace — a deeper-indented continuation or JSON
  key line became a fold start, "nesting depth" tracked indentation. The
  indent scan is now the fallback it was specced to be: grammar-less
  modes, or a language whose fold query failed to compose. With a working
  fold grammar the engine captures are the ONLY source. What the fallback
  used to give grammar modes is now covered BY the grammar: kFoldPython
  gained dictionary/list/set/comprehensions/generator/tuple/parenthesized/
  argument_list/parameters/concatenated_string nodes, kFoldJavascript
  gained call_expression/arguments/formal_parameters — VS Code-like
  multiline bracket folds stay available, single-line captures are
  filtered so one-line literals cost nothing.
  Test: `grammarFoldsFollowBracketsNotIndent`.
- Editing a vendored `highlights.scm` did not re-run CMake configure, so
  incremental builds silently kept the OLD embedded query text
  (`configure_file` is configure-time only). Now the three .scm files are
  `CMAKE_CONFIGURE_DEPENDS` of the `third_party/tree-sitter` directory.
- `TextBuffer`'s constructor never created the add buffer: a document fed
  without a leading `setText()` wrote its pieces into the empty placeholder
  buffer (`buf = 0`), tripping the "piece out of buffer" invariant check.
  Only paths that skip `setText` were affected (exposed by the new cap tests).
- `deleteSelection()` now emits `selectionChanged()` — it was the only path
  annihilating a selection that stayed silent.

### Notes
- Qt 6.12 generic-defaults probe: `QKeySequence::Redo` contains only
  Ctrl+Shift+Z (Ctrl+Y does NOT match), so Ctrl+Y is wired as an explicit
  fallback after the standard-key matches.
- M6 measured traps (both bit hard during development): Qt6 `QFlags(Enum)`
  stores the enumerator VALUE as the mask as-is (no automatic `1<<n`
  shift), so a flag enum with implicit values makes its first enumerator
  (0) an empty mask and `testFlag` on it means "no flags at all" —
  `FindFlag` uses explicit powers of two. And `QStringLiteral` prefixes
  `u""`, so UTF-8 byte hex escapes (`"\xE2\x96\xB2"`) sign-extend into
  mojibake; non-ASCII glyphs there must be `\uXXXX` escapes.
- M13 measured traps: `modelLineForVisibleIndex` CLAMPS, so a foldable
  slot walk bounded only by model lines spins forever when the paint
  window ends at the document end (measured hang on first paint — the
  loop must break on `slot >= visibleLineTotal()`); and libstdc++'s
  `std::binary_search` comparator-argument-order gotchas bit, so the map/
  fold lookups use `lower_bound` + explicit equality everywhere.

## [0.5.0] — 2026-11-09

### Added
- `CodeEditor::xForCol(line, col)`: widget-space x of a text position
  following the glyph engine's cluster advances (public already now because
  M5 decorations need exactly this geometry), and `positionAt(widgetPos)`
  replacing the private grid-based `posFromEvent`. `QWidget::setFont` on the
  editor now works end-to-end: `changeEvent(FontChange)` re-derives cell
  metrics and wipes the layout cache.

### Fixed
- Caret drift on lines containing wide clusters (reported live after
  0.4.0): caret x, selection rectangles, painting and mouse hit-testing now
  share ONE laid-out `QTextLine` per visible line (a fixed, text-keyed LRU
  cache in the view) instead of `col * cellWidth` grid arithmetic. A ZWJ
  family spans 8 code units but only ~1.8 cells of advance — everything
  after it used to slide away from the caret, hit-test and highlight.
  Plain monospace lines keep exact `col * cellWidth` positions (measured
  engine error 0.0), so nothing moved there; a click inside a wide cluster
  floors to the cluster start, and hit-testing keeps the editor-style floor
  semantics (`xToCursor`'s nearest-edge pick is walked back deliberately).
  Qt 6.12 note: `QTextLine::x(pos)/xToPos/cursorRect` are gone — the cache
  builds on `cursorToX`/`xToCursor`, present since Qt 4 and at our 6.6
  floor; a default-constructed `QTextLine` SEGFAULTS on any accessor, so
  every slot use is gated on a `valid` flag. Also measured: on the raster
  paint engine (the widget backing store, both X11 and offscreen)
  `QTextLine::draw`'s pos is the layout TOP-LEFT, not a baseline anchor —
  the engine adds `line.y() + line.ascent()` itself; passing a baseline
  there shifts every drawn line about a line-height below its caret band
  (the first live run of this commit showed exactly that: a fake empty
  line at top, caret a line off the glyphs). New numeric tests include a
  pixel-scan of `grab()` pinning the caret to the layout x (~50px away
  from the old grid position on the probe line) and a per-line ink-band
  assertion catching that draw-anchor class of regression; 55/55 green
  on real X11.

## [0.4.0] — 2026-11-09

### Added
- IME composition (closes Phase C / M2): `inputMethodEvent` renders the
  preedit at the caret with a dotted underline — display-only, it never
  enters the model — and commits arrive through the normal insert path,
  so committed CJK/surrogate text is cluster-safe from the start. Any
  deliberate caret move (keys, mouse, `setPosition`) or document replace
  drops a stale preedit. `inputMethodQuery` answers `ImEnabled`,
  `ImCurrentSelection` and `ImCursorRectangle` (the candidate-window
  anchor trails the preedit, matching the visual caret). Tests drive raw
  `QInputMethodEvent`/`QInputMethodQueryEvent` (Qt 6.12 event shape:
  preedit via ctor, commit via `setCommitString`); 49/49 green on real
  X11.

## [0.3.0] — 2026-11-09

### Added
- M2 word navigation: Ctrl+Left/Right (and Ctrl+Shift to extend) move by
  editor-style word runs (word / space / punctuation classes, VS Code
  `cursorWordRight`/`cursorWordLeft` semantics, deliberately not the ICU
  `Word` boundary), Ctrl+Right then one press crossing the trailing space
  run, Ctrl+Left additionally swallowing the space run before the target
  word. At a line end Ctrl+Right wraps to the next line start (and vice
  versa); at EOL Ctrl+Del joins lines like plain Delete, with a selection
  Ctrl+Del deletes the selection. Targets are always snapped to grapheme
  boundaries.
- M2 grapheme-cluster correctness via `QTextBoundaryFinder` (QtCore,
  ICU-backed): Left/Right arrows move one whole cluster per press (ZWJ
  family sequences, regional-indicator flags, skin-tone modifiers, VS16
  and combining marks measured as single clusters); Backspace/Delete
  remove whole clusters too, superseding the earlier surrogate-pair-only
  guards. A caret sitting inside a cluster (reachable via `setPlainText`,
  `setPosition` or a mid-cluster click) snaps outward on arrows, and
  Backspace/Delete take the WHOLE cluster in both directions, so orphan
  surrogate halves or dangling combining marks can never accumulate.
  `col` numbering stays UTF-16 code units (LSP semantics, deliberate).

### Fixed
- PLAN.md/README M2 tooling note: the earlier "QTextBoundaryRunner
  (Qt ≥ 6.9)" correction was itself wrong — no such class exists in any
  Qt release (verified against the v6.9.0 source tree and the host
  Qt 6.12 headers/symbols); `find_package(Qt6 6.6)` needs no bump.

## [0.2.0] — 2026-11-09

First milestone on the release train (git tag + alpha wheel per milestone).

### Added
- M1b clipboard: Ctrl+C / Ctrl+X / Ctrl+V plus the platform chains matched
  via `QKeySequence` (`Ctrl+Ins`, `Shift+Ins`, `Shift+Del`, Cmd variants on
  macOS). With no selection copy/cut target the whole current line,
  Monaco-style: the non-last line goes with its trailing newline; cutting the
  last line also takes the newline that preceded it (VS Code line-cut
  semantics) so no phantom trailing line appears. Paste replaces any
  selection, normalizes `\r\n`/lone `\r` to `\n`, is a no-op on an empty
  clipboard and never splits a surrogate pair. X11 primary selection: every
  selection (mouse, keyboard, Ctrl+A) mirrors into it and it is released on
  clear/cut; middle-click pastes it at the click position, falling back to
  the regular clipboard where no primary selection exists (Wayland,
  offscreen). Public `copy()`/`cut()`/`paste()`. The suite runs green both
  offscreen (primary-mode tests gate on `supportsSelection()`) and on real
  X11, where all 37 tests execute unskipped.
- PySide6 binding spine (PLAN.md Phase B): the Shiboken6 extension module
  `_qmn` wraps `qmn::view::CodeEditor` + `TextPosition` via the official
  `shiboken_generator_create_binding()` macro; codegen, compile and wheel all
  run in the `bindings/Dockerfile` container (rockylinux:9, PySide6 6.12.0 +
  matching Qt 6.12.0 from pinned mirror archives), driven by
  `bindings/build_in_container.sh`. Packaged with scikit-build-core into a
  `qmn-native` wheel: `_qmn.abi3.so` at site-packages root plus a pure-Python
  `qmn/` shim exposing `qmn.CodeEditor` / `qmn.TextPosition`. Smoke tests
  (build-tree and installed-wheel) assert text round trip, `TextPosition`
  marshalling, `cursorMoved`/`selectionChanged` reaching Python callables, and
  exactly one `libQt6Core.so.6` in the process. Opt-in via
  `-DQMN_BUILD_PYTHON=ON`; host CTest and CI are unaffected.
- M1 selections: Shift+arrows/Home/End/PageUp/PageDown extend from the caret
  (the caret is the active end), Ctrl+Shift+Home/End extend to document
  bounds, Ctrl+A selects all, Escape collapses. Mouse: plain click collapses,
  Shift+click extends, left-drag selects with edge auto-scroll; the caret
  lands at the drag end. Typing/Enter replace the selection; Backspace and
  Delete remove it without touching neighboring text (`selectedText()`,
  `selectedLength()`, `hasSelection()`, `selectionChanged()` signal; the demo
  window title shows cursor position and selection size). Fully covered lines
  paint their selection through EOL using a semi-transparent highlight.
- View-layer edits never split a surrogate pair: Backspace/Delete remove the
  whole pair when adjacent to one (full grapheme clusters remain an M2 item).
- Up/Down navigation on the first/last line now collapses to the line
  start/end (previously it was a no-op, which also made Shift+Down on a
  single-line document select nothing).

### Fixed
- Caret drifted left along the line: cell width was taken from integer
  `QFontMetrics` (e.g. 8px) while the glyph engine advances the pen by the
  font's fractional advance (monospace @14px measures 8.3906px), ~0.39px/col
  drift. The editor now measures the cell width with `QFontMetricsF` (double),
  uses fractional hinting-free metrics (`PreferNoHinting`), places the caret
  at `padding + round(col * cellWidth)`, draws text with a fractional pen
  position, and hit-tests clicks against the same fractional cell grid.
  Regression test: engine advance of a 120-column line must equal
  `120 * cellWidth()` within half a pixel.

## [0.1.0] — 2026-11-09

Initial spike validating the two riskiest layers.

### Added
- `src/core/TextBuffer`: piece-table text model over UTF-16 code units with
  an implicit-treap line index (O(log P) insert/delete and line↔offset
  lookups); debug invariant checking for treap aggregates and heap order.
- `src/view/CodeEditor`: raw-QWidget editor — viewport-culled QPainter line
  painting, blinking caret, keyboard editing (text, Enter, Backspace, Delete,
  arrows, Home/End, PageUp/Down), mouse caret placement, vertical scrolling,
  per-keystroke latency instrumentation.
- `src/app/qmn-spike`: demo/bench harness with `--exit-after-ms` headless
  smoke mode and load-time reporting.
- Tests: property test (2,000 random edits vs naive reference with invariant
  checks after every op) + widget keyboard smoke tests; ctest integration;
  GitHub Actions CI (arch + ubuntu).
- Benchmarks (see README): ~4 ms `setPlainText` for 62 MB, mean ~7 µs per
  random edit inside a 1M-line document.

### Known limitations (v0.1)
- No selection, clipboard, IME, undo/redo, soft wrap, horizontal scrolling,
  syntax highlighting, or grapheme-safe caret movement (UTF-16 code-unit
  semantics; surrogate pairs may be split by programmatic edits, never by the
  view's own input handling).
