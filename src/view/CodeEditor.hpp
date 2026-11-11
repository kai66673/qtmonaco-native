#pragma once

#include <QElapsedTimer>
#include <QFont>
#include <QMetaType>
#include <QPoint>
#include <QScrollBar>
#include <QString>
#include <QStringList>
#include <QTextLayout>
#include <QTextLine>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include "core/TextBuffer.hpp"

#include <array>
#include <functional>
#include <memory>

// M16 seams, pointer-only in the API (the menu/DnD machinery lives in the .cpp).
QT_FORWARD_DECLARE_CLASS(QMenu)
QT_FORWARD_DECLARE_CLASS(QMimeData)
QT_FORWARD_DECLARE_CLASS(QContextMenuEvent)
QT_FORWARD_DECLARE_CLASS(QDragEnterEvent)
QT_FORWARD_DECLARE_CLASS(QDragMoveEvent)
QT_FORWARD_DECLARE_CLASS(QDropEvent)

// Used by a private method signature only (the paint pass lives in the .cpp).
QT_FORWARD_DECLARE_CLASS(QPainter)
#include <utility>

class QInputMethodEvent;

namespace qmn::view {

class HighlightEngine; // M4 private seam; defined in view/HighlightEngine.hpp

// 0-based text position. col is counted in UTF-16 code units.
struct TextPosition {
    int line = 0;
    int col = 0;

    // A real constructor (not aggregate init) is what the Python binding needs
    // to expose TextPosition(line, col) the way qtmonaco's set_cursor does.
    // Brace uses like TextPosition{1, 0} keep working unchanged.
    TextPosition() = default;
    TextPosition(int line, int col) : line(line), col(col) {}

    bool operator==(const TextPosition&) const = default;
};

// (Q_DECLARE_METATYPE for this type is at the bottom of this header, at global
// scope, where Qt requires it - see the note there.)

// Editor widget: a raw QWidget painting visible lines with QPainter.
//
// Deliberately included: viewport-culled monospace painting, caret with
// blinking, keyboard editing (text, Enter, Backspace, Delete, arrows,
// Home/End, PageUp/PageDown), grapheme-cluster-aware arrows and editing
// (QTextBoundaryFinder, ICU-backed: ZWJ sequences, flags, combining marks
// move and delete as one unit), Ctrl+arrows word navigation and Ctrl+Del
// word deletion, mouse caret placement and drag selection,
// Shift-navigation + Ctrl+A selections with replace-on-type semantics,
// clipboard copy/cut/paste (whole-line when no selection, Monaco-style),
// X11 primary selection mirroring + middle-click paste, IME composition
// (preedit rendered at the caret, only committed text reaches the model),
// vertical scrolling, per-keystroke latency instrumentation, a
// line-number gutter (off/on/relative/interval) with breakpoint and
// diagnostic glyphs, whole-line highlight bands, diagnostic wavy
// underlines and inlay hints (M5), find/replace with a built-in Ctrl+F
// bar plus match highlights (M6), and
// tree-sitter-backed syntax highlighting with a grammar-less fallback
// (M4: python/json/javascript, colors painted through the same cached
// QTextLine layout as the plain path).
//
// `col` stays UTF-16 code units everywhere (LSP semantics); the
// caret may sit inside a cluster only via setPlainText/setPosition/mouse
// clicks, and arrows/edits snap outward from there without ever splitting
// a cluster.
//
// Geometry has a single source of truth: every visible line gets a
// laid-out QTextLine (per-line LRU cache below), and caret x, selection
// x, painting and mouse hit-testing all read the same glyph-engine
// advances. Wide clusters (emoji ZWJ sequences, flags) therefore never
// drift the caret away from the drawn text; pure-ASCII lines keep exact
// col*cellWidth positions.
//
// Deliberately NOT included (follow-up milestones): soft wrap, horizontal
// scrolling, column memory for Up/Down, double-click word selection. See
// README roadmap.
class CodeEditor : public QWidget {
    Q_OBJECT
public:
    struct PerfStats {
        qint64 count = 0;
        qint64 sumNs = 0; // keyPressEvent logic (model edit + scheduling)
        qint64 maxNs = 0;
        qint64 meanNs() const { return count ? sumNs / count : 0; }
    };

    explicit CodeEditor(QWidget* parent = nullptr);
    ~CodeEditor() override;

    void setPlainText(const QString& text);
    QString toPlainText() const;

    void setPosition(TextPosition pos);
    TextPosition position() const { return caret_; }
    int lineCount() const { return buffer_.lineCount(); }

    // Selection: the caret is always the active end; anchor_ is the inactive
    // end. Both ends are always fully resolved text positions, so painting
    // and hit-testing never need to reason about "virtual" columns.
    bool hasSelection() const { return hasSelection_; }
    QString selectedText() const;
    int selectedLength() const; // UTF-16 code units, O(log P), no copy
    void selectAll();
    void clearSelection();

    // Clipboard (M1b). With no selection, copy/cut target the whole current
    // line (newline included) — Monaco semantics. paste() inserts the
    // clipboard text, replacing the selection, with CRLF normalized to '\n'.
    void copy();
    void cut();
    void paste();

    // Block indent/outdent (M14, also Tab / Shift+Tab in the key handler).
    // Every line any caret's selection touches is shifted by one tab stop
    // (indent: stop-many spaces at col 0; outdent: up to one stop of
    // leading whitespace removed). The selection END line is excluded when
    // the end sits at col 0. All carets end collapsed; one call = one
    // undo step, multi-caret included.
    void blockIndent();
    void blockOutdent();

    // Undo/redo (M3). One step = one user action: typing runs coalesce into
    // a burst step (broken by Enter, caret moves and selection-replacing
    // edits), paste and over-selection typing are single compound steps.
    // undo() restores the caret/selection as it stood before the action;
    // redo() restores the state from when it was undone. setPlainText()
    // clears the history. Keys: Ctrl+Z, Ctrl+Shift+Z / Ctrl+Y.
    void undo();
    void redo();
    bool canUndo() const { return buffer_.canUndo(); }
    bool canRedo() const { return buffer_.canRedo(); }

    // Multiple carets (Phase D1). Alt+Click adds a caret, Alt+Drag drags a
    // selection for the caret under its start; typed edits, Enter,
    // Backspace/Delete, paste, cut and the navigation keys then act on
    // EVERY caret (one key press = one bracketed undo step; multi-caret
    // typing deliberately does NOT coalesce across presses). The "primary"
    // caret is the one position() reports and the one the mouse/keyboard
    // last owned; a plain click, setPosition, selectAll or Escape
    // collapses back to it. Carets normalize VS Code-style on add, drag
    // end, navigation and edits: duplicate carets fold, a caret inside
    // another slot's selection is dropped (the extra goes if the caret is
    // primary), and intersecting selections keep the LEFT one.
    int additionalCaretCount() const { return static_cast<int>(extraCarets_.size()); }
    QVector<TextPosition> additionalCarets() const; // caret positions, doc order
    void clearAdditionalCarets(); // collapse to the primary caret

    // Syntax highlighting (M4, Phase D2). Backed by the vendored
    // tree-sitter engine behind a private seam (view/HighlightEngine.hpp):
    // the public surface here stays plain-Qt so it remains
    // Shiboken-wrappable and tree-sitter never leaks through headers.
    // setLanguage("") or an unknown id disables highlighting; the widget
    // then paints everything in the default text color (same look as
    // before M4 - this is the grammar-less fallback path).
    // availableLanguages() = ids with a parser + highlights query.
    // qtmonaco parity note: the Python binding exposes this as
    // set_language()/get_language() with the same id strings.
    enum class TokenKind : quint16 {
        // Semantic roles mapped 1:1 from tree-sitter capture names (the
        // union of the vendored grammars' highlights.scm). Color mapping
        // is the caller's/theme's job; the view only assigns roles.
        Text = 0, // explicit: "default text color"; never emitted as a span
        Comment, Keyword, Number, String, StringSpecial, StringSpecialKey,
        Escape, Embedded, Constant, ConstantBuiltin, Constructor, Function,
        FunctionBuiltin, FunctionMethod, Type, Variable, VariableBuiltin,
        Property, Operator, Punctuation, PunctuationBracket,
        PunctuationDelimiter, PunctuationSpecial,
    };
    Q_ENUM(TokenKind)

    void setLanguage(const QString& language);
    QString language() const;
    static QStringList availableLanguages();

    // Merged, non-overlapping token spans intersecting [startOffset,
    // endOffset) (clamped), sorted by start - document UTF-16 offsets.
    // Forces a reparse+query if dirty. Primary consumer: tests (numeric
    // mapping assertions); the widget paints from the same engine state.
    struct HighlightSpan {
        int start = 0;
        int end = 0;
        TokenKind kind = TokenKind::Text;

        bool operator==(const HighlightSpan&) const = default; // test comparisons
    };
    QVector<HighlightSpan> highlightSpans(int startOffset, int endOffset);

    // Decorations + gutter (M5, Phase D3). Three layers, matching the
    // Gate-2 consumers that must be expressible WITHOUT new view primitives
    // (PLAN.md): (1) gutter glyph layer - breakpoint dots, diagnostic
    // severity icons (fold triangles join in M13), (2) whole-line layer -
    // highlighted-line bands, current-line band, (3) inline text layer -
    // diagnostic wavy underlines, inlay hints. All APIs are batch
    // replace-all: add = set, update = set again, remove = set without the
    // item - which keeps the state round-trip trivial and LSP/DAP glue a
    // thin consumer. Positions are the same 0-based UTF-16 (line, col) as
    // everywhere; stored decorations are NOT clamped to the document
    // (edits move lineCount - out-of-range items simply never paint).
    //
    // qtmonaco parity (binding converts 1-based): set_line_numbers_mode <->
    // setLineNumbersMode, set_highlighted_lines/clear_highlighted_lines <->
    // setHighlightedLines/clearHighlightedLines.

    // (1) Gutter line numbers. Default On (Monaco parity).
    enum class LineNumbersMode { Off, On, Relative, Interval };
    Q_ENUM(LineNumbersMode)
    void setLineNumbersMode(LineNumbersMode mode);
    LineNumbersMode lineNumbersMode() const { return lineNumbersMode_; }

    // What the gutter displays for a 0-based line: On = line+1; Relative =
    // distance to the caret line (the caret line itself shows its absolute
    // number); Interval = the absolute number only on 1-based multiples of
    // 10, empty string elsewhere (Monaco's interval behavior). Empty
    // string also means "draw nothing here" (Off mode, invalid line).
    QString lineNumberText(int line) const;

    // Gutter geometry. gutterWidth() = 0 in Off mode; otherwise digit
    // width + padding, plus the fold zone (M13: a fixed strip along the
    // gutter's right edge where the fold triangles live — reserved
    // whenever the gutter is on, so folding never shifts the numbers),
    // plus one extra reserved slot while any breakpoint exists (adding a
    // breakpoint must not shift already-drawn numbers).
    // contentLeft() = widget x where text begins: padding_ + gutterWidth().
    // xForCol()/positionAt()/painting all use it, so callers composing
    // widget x read contentLeft(), not padding().
    int gutterWidth() const;
    int contentLeft() const { return padding_ + gutterWidth(); }
    // The M13 fold zone: fixed-width strip at [contentLeft()-foldZoneWidth(),
    // contentLeft()), 0 in Off mode. Clicks inside it toggle the fold of
    // that row's line and never move the caret.
    int foldZoneWidth() const;

    // (1) Breakpoint glyphs: one filled dot per line, in the reserved
    // slot. Getter returns the sorted, deduplicated lines.
    void setBreakpointLines(const QVector<int>& lines);
    QVector<int> breakpointLines() const { return breakpointLines_; }

    // (3) Diagnostics: wavy underline over [start..end] (clamped to the
    // line when painting) + a severity icon in the gutter anchored at
    // startLine. Stored sorted by start. Message is carried for consumers
    // (hover UI is Gate-2 glue); it is not rendered by the view in v1.
    enum class Severity { Error, Warning, Information, Hint };
    Q_ENUM(Severity)
    struct Diagnostic {
        int startLine = 0;
        int startCol = 0;
        int endLine = 0;
        int endCol = 0;
        Severity severity = Severity::Error;
        QString message;

        bool operator==(const Diagnostic&) const = default; // test round-trips
    };
    void setDiagnostics(const QVector<Diagnostic>& diagnostics);
    QVector<Diagnostic> diagnostics() const { return diagnostics_; }

    // (3) Inlay hints: `text` painted in hint color at (line, col) WITHOUT
    // re-flowing the following text - the caller (e.g. a DAP consumer)
    // anchors hints into existing whitespace. v1 renders inline after the
    // text pass; adding real re-flow later is a rendering refinement, NOT
    // an API change. Getter returns the sorted hints.
    struct InlayHint {
        int line = 0;
        int col = 0;
        QString text;

        bool operator==(const InlayHint&) const = default;
    };
    void setInlayHints(const QVector<InlayHint>& hints);
    QVector<InlayHint> inlayHints() const { return inlayHints_; }

    // (2) Whole-line bands, drawn UNDER selections. Inclusive 0-based
    // range (the binding subtracts 1 from qtmonaco's 1-based pair; end<
    // start is swapped). Getters report -1 when unset.
    void setHighlightedLines(int startLine, int endLine);
    void clearHighlightedLines();
    int highlightedLinesStart() const { return highlightStart_; }
    int highlightedLinesEnd() const { return highlightEnd_; }

    // (2) Current-line band (Monaco's renderLineHighlight). Default off;
    // the theme pass (set_theme row, D3) picks the shipped default.
    void setCurrentLineHighlight(bool enabled);
    bool currentLineHighlight() const { return currentLineHl_; }

    // Indentation + auto-indent (M14). SOFT TABS ONLY: Tab typing never
    // puts literal '\t' into the buffer; literal tabs (paste, setPlainText)
    // stay in the text and RENDER on the same stop grid. All three knobs
    // are freeze-bound (Phase F binds the qtmonaco indent surface).
    //
    // Tab stop width: Tab advances the caret to the next column that is a
    // multiple of this width (counted on the stop grid from the line
    // start, so a literal '\t' left of the caret counts as its own grid
    // advance), inserting exactly that many spaces; Shift+Tab is the VS
    // Code "unindent": it retires the touched whole lines to the next
    // lower stop boundary. Default 4 (Monaco's tabSize). Also drives the
    // RENDER width of literal '\t' in the text (stops at every N cells
    // from the line start), so typed and pasted indentation line up on
    // one grid. Clamped to >= 1.
    void setTabStopWidth(int columns);
    int tabStopWidth() const { return tabStopWidth_; }

    // Enter auto-indent (default ON): the new line copies the current
    // line's leading whitespace; a trailing opener `{([:` right before the
    // caret (whitespace skipped) adds one more stop — Enter AT col 0 never
    // does. When the caret sits flush inside an empty pair it SPLITS:
    // `{|}`, `}|{` (also () and []), `{|` at EOL — which CONJURES the
    // missing closer — and `|}` at col 0; the closer parks on its own line
    // at the parent indent and the caret stays on the child line.
    // Alt+Enter and typed/pasted '\n' never auto-indent (plain newline).
    void setAutoIndentEnabled(bool enabled);
    bool autoIndentEnabled() const { return autoIndent_; }

    // Tab as focus traversal (default false): with ON, Tab/Shift+Tab are
    // ignored and ride Qt's focus chain — the a11y escape hatch out of
    // the editor. With OFF, Tab indents (see above).
    void setTabChangesFocus(bool enabled);
    bool tabChangesFocus() const { return tabChangesFocus_; }

    // Find & replace (M6, Phase D4). The engine lives in the widget (a
    // drop-in of Chromium-Monaco must not lose its find UX); the built-in
    // Ctrl+F / Ctrl+H bar is the primary UI, every action below is also
    // public so consumers can drive their own find surface. Ctrl+F opens
    // the find row, Ctrl+H the replace row, F3/Shift+F3 cycle matches even
    // while the bar is closed (the query persists); Enter in the find row
    // = next, Shift+Enter = previous, Enter in the replace row replaces
    // the current match then advances. The confirm step for replace-all
    // is the button itself: Enter never fires it, and the whole batch
    // lands as ONE undoable D1 action (undo once restores everything).
    //
    // Engine semantics (v1): the search is PER-LINE - matches never cross
    // a line boundary (a pattern containing a newline matches nothing).
    // Literal queries are escaped into the same QRegularExpression path.
    // Match bounds snap outward over split surrogate pairs and duplicate
    // matches merge (PCRE2 in code-unit mode steps through a surrogate
    // pair one unit at a time), so replacing a match never orphans half a
    // pair. An invalid pattern clears the match set (findPatternValid()
    // tells "invalid" from "no matches"). Matches recompute lazily: an
    // edit marks the set stale and the next paint (or find action)
    // refreshes it - the same at-most-once-per-keypress cadence as the
    // M4 highlight engine; highlights and the match count stay live
    // through edits. Painting is viewport-only: the match list is sorted
    // and the paint pass binary-searches the window.
    struct MatchRange {
        int startLine = 0;
        int startCol = 0;
        int endLine = 0; // == startLine while the engine is per-line
        int endCol = 0;  // [start, end) half-open, UTF-16 code units

        bool operator==(const MatchRange&) const = default; // test round-trips
    };

    // Flags mirror the bar's toggles. Default: MatchCase (Monaco parity).
    // The Q_DECLARE_FLAGS/Q_FLAG trio is the same modern Qt6 flags pattern
    // PySide6 itself wraps, so the binding gets a natural Python flag set.
    // The enumerators must be EXPLICIT bit masks: Qt6 QFlags stores the
    // enumerator value as the mask as-is (no auto 1<<n shift), so an
    // implicit first entry (value 0) would make MatchCase an empty mask and
    // testFlag(MatchCase) mean "no flags at all" - measured in this Qt
    // 6.12's qflags.h. Powers of two are also what PySide6 exposes.
    enum class FindFlag { MatchCase = 1 << 0, WholeWord = 1 << 1, UseRegex = 1 << 2 };
    Q_DECLARE_FLAGS(FindFlags, FindFlag)
    Q_FLAG(FindFlags)

    void setFindText(const QString& text); // recomputes matches
    QString findText() const { return findText_; }
    void setFindFlags(FindFlags flags); // recomputes matches
    FindFlags findFlags() const { return findFlags_; }
    bool findPatternValid() const { return findPatternValid_; }

    // Match set state. matches() is the full sorted list (model ranges);
    // note it can be stale after an edit until the next paint/action -
    // matchCount()/currentMatch() are the refresh-on-read entry points.
    // currentMatch() reports the EFFECTIVE current match: the index in the
    // list only while the selection covers it, -1 otherwise (a click or
    // typing moves on, Monaco-style). A fresh set has none until a find
    // action selects one.
    QVector<MatchRange> matches() const { return matches_; }
    int matchCount(); // refreshes a stale set (paint is viewport-only)
    int currentMatch();
    // Select + reveal the match at index (no wrap checks outside range);
    // false when the index is stale (e.g. after a refresh emptied the set).
    bool gotoMatch(int index);
    bool findNext();     // first match after the selection end, wrapping
    bool findPrevious(); // first match before the selection start, wrapping

    // Replacements. replaceNext(): current match, or next at/after the
    // caret when none; one bracketed edit (own undo step), caret lands at
    // the replacement end and the set refreshes. replaceAll(): whole
    // document, replacements applied back-to-front so offsets stay valid,
    // ONE undo group restores the batch; returns the replaced count.
    bool replaceNext(const QString& replacement);
    int replaceAll(const QString& replacement);

    // Built-in bar (lazy child widgets). showFindBar()/showReplaceBar()
    // open it and focus the respective field, selectively selecting the
    // remembered query; Esc or the X button hides it and returns focus
    // (the query and highlights persist until clearFind()).
    void showFindBar();
    void showReplaceBar();
    void hideFindBar();
    bool findBarVisible() const;
    // Clears query + flags + matches (the bar text fields included);
    // does not hide the bar.
    void clearFind();

    // Overview ruler (M7, Phase E1) — the minimap substitute: a right-edge
    // strip in which the WHOLE document is scaled to pixels (model px
    // space [0, 2*padding + lineCount*lineHeight) maps linearly onto the
    // strip height). It paints sampled line stripes (one stripe per strip
    // pixel-row budget, never more — the document is never rendered
    // off-screen: cost is O(strip height), not O(document)), the marker
    // layers (breakpoints, diagnostics, highlighted lines, search matches
    // — subsampled to the strip when huge) and the viewport rectangle.
    // Click/drag inside the strip scrolls (the clicked line centers in the
    // viewport); it never moves the caret. The vertical scrollbar
    // overlays the strip's right edge, VS Code-style. The Python binding
    // maps qtmonaco's set_minimap_enabled here — never onto a
    // Chromium-style token minimap.
    static constexpr int kOverviewRulerWidth = 44; // px, fixed until set_theme
    void setOverviewRulerEnabled(bool enabled);
    bool overviewRulerEnabled() const { return rulerEnabled_; }
    int overviewRulerWidth() const { return rulerEnabled_ ? kOverviewRulerWidth : 0; }
    QRect overviewRulerGeometry() const; // empty rect when disabled
    // Pure mapping math (numeric contract, drives the click gesture):
    // doc line under a widget-space y inside the strip; -1 when disabled.
    int rulerLineAt(int y) const;
    // Rectangle marking the visible window inside the strip (empty when
    // disabled). The DoD "ruler geometry state" entry point.
    QRect overviewRulerViewportRect() const;

    // Horizontal scrolling (M9, Phase E1) — the h-scroll half of the Gate-1
    // viewport story (soft wrap stays deferred to Gate 2). The text window
    // runs from contentLeft() to the overview-ruler chrome, and the offset
    // is in pixels, the same integer space vScroll lives in (the monospace
    // grid tests assert exact offsets against cellWidth arithmetic).
    void setHorizontalScrollOffset(int pixels); // clamped (see max…)
    int horizontalScrollOffset() const { return hScroll_; }
    int horizontalViewportWidth() const; // visible text px, ruler excluded
    // The clamp contract: max offset = docWidth + padding - window. The
    // doc width is AMBIENT: a line's measured px width joins the extent
    // when painting/caret/hit-testing visits it, and the extent is the max
    // over visited lines — the view never walks the whole document to find
    // the widest line (same no-O(document) rule as the ruler, so the bar
    // grows while the doc is being visited; stale widths after edits
    // decay the same ambient way). Reset on setPlainText.
    int maxHorizontalScroll() const;
    // Parity hook: qtmonaco's set_scroll_beyond_last_line_enabled
    // (Monaco's scrollBeyondLastLine option). On, the doc pixel space
    // grows by height() - lineHeight_, letting the LAST line scroll up to
    // the viewport top (and the ruler mapping spans that taller space).
    // Default OFF — a deliberate divergence from Monaco's ON default that
    // keeps every existing end-of-scroll contract (ruler mapping
    // included) bit-stable; revisit the default before the API freeze.
    void setScrollBeyondLastLineEnabled(bool enabled);
    bool scrollBeyondLastLineEnabled() const { return scrollBeyondLastLine_; }

    // Code folding (M13, Phase E2). A fold is an inclusive pair of MODEL
    // lines (0-based) with startLine < endLine; collapsing it hides
    // (startLine, endLine] — the header line never disappears (js/json
    // closing braces hide with their body, python blocks end at the last
    // body line). Foldable starts come from the engine's @fold captures
    // whenever a fold grammar is active; the view's indent rule (the next
    // visible line is deeper than this one) is the fallback for
    // grammar-LESS modes only — in js/json/python nesting is what the
    // brackets/syntax say, and leading whitespace alone must never fake
    // python-style blocks there (owner report; see PLAN E2).
    //
    // Fold state is VIEW state: not undoable in v1, kept index-correct
    // across edits on a best-effort sweep (a fold whose body was deleted
    // vanishes), and cleared entirely by setPlainText/set_language-level
    // document replaces. A collapsed fold also AUTO-EXPANDS when an edit
    // kills its source (the grammar capture or indent pair that makes the
    // line foldable is gone): the body reappears, mirroring the way the
    // same edit just drops the marker of an unfolded fold — the asymmetry
    // that otherwise leaves ▸ + hidden rows on a block nothing folds.
    // Hidden lines have no visible index: use the view-line map below
    // before addressing any geometry.
    struct FoldRange {
        FoldRange() = default;
        FoldRange(int startLine_, int endLine_) : startLine(startLine_), endLine(endLine_) {}
        int startLine = 0;
        int endLine = 0;
        bool operator==(const FoldRange&) const = default;
    };

    // Foldable ranges as the view currently knows them: the active source
    // (grammar captures, or the indent fallback in grammar-less modes),
    // sorted by start, duplicate starts resolved (engine wins — its ends
    // are exact). Non-const because lazy indent-rule ends get
    // resolved exactly here (an explicit-caller cost, never a paint cost).
    // Viewport-scoped on big documents (paint keeps the cache warm; an
    // out-of-window fold is discovered exactly when scrolling reveals it).
    QVector<FoldRange> foldableRanges();
    bool isFoldable(int line) const; // would foldLine(line) find a fold?
    bool isFolded(int line) const;   // line starts a currently collapsed fold

    // Toggle on a foldable/collapsed START line. Targets hidden inside
    // another fold do nothing (return false): the caret-based entry points
    // unfold to reveal first, the mouse can only ever hit visible lines.
    // Returns true if the fold state changed. Keyboard: Ctrl+Shift+[ / ]
    // fold/unfold at the caret, Ctrl+Alt+[ / ] collapse/expand everything.
    bool foldLine(int line);
    bool unfoldLine(int line);
    bool toggleFold(int line);
    // Explicit whole-document passes (O(document) is acceptable here —
    // never on a paint path): collapse every outermost fold, then every
    // fold; expandAll unfolds everything.
    void collapseAll();
    void expandAll();

    // The ONE view-line map (M13): visible slots are numbered
    // [0, visibleLineTotal()) top-to-bottom; a hidden (folded-away) model
    // line has visible index -1. Paint, caret navigation, hit-testing,
    // scroll clamps and the ruler all route through it, so model space and
    // screen space can never drift apart across a fold boundary.
    int visibleLineTotal() const;
    int visibleIndexOfModelLine(int line) const; // -1 = hidden
    int modelLineForVisibleIndex(int visibleIndex) const; // clamped both ways

    int lineHeight() const { return lineHeight_; }
    double cellWidth() const { return cellWidth_; }
    int padding() const { return padding_; } // vertical/top-edge padding

    // Viewport state read-out (the same window paintEvent and the
    // highlight policy consume). Read-only: scrolling itself happens via
    // wheel/scrollbar. Public for consumers, bindings and the policy test.
    // firstVisibleLine() reports the MODEL line of the top visible slot
    // (fold-aware since M13: across a collapsed fold it jumps to the line
    // after the hidden run; hidden lines are never reported);
    // visibleLineCount() is the slot budget the paint loop walks (a
    // scroll-space measure, not the hidden-adjusted total — see
    // visibleLineTotal() for that).
    int firstVisibleLine() const;
    int visibleLineCount() const;
    int verticalScrollOffset() const { return vScroll_; } // px, clamped

    // Widget-space x of the caret position at (line, col), following the
    // glyph engine's cluster advances (not a col * cellWidth grid), gutter
    // included (so xForCol(line, 0) == contentLeft() while hScroll is 0).
    // Shifted LEFT by the current horizontalScrollOffset() — it is the
    // widget x the user would see (off-screen positions go negative or past
    // the right edge, exactly like the painted glyph). A col
    // inside a grapheme cluster is snapped to a cluster edge by the engine;
    // the text position itself is untouched. Out-of-range line/col clamp.
    // Public because decorations (M5: diagnostics, inlay hints) need the
    // same geometry as the caret.
    double xForCol(int line, int col) const;

    // Text position for a widget-space point (used by mouse handling and
    // wrappable for bindings). Cols inside a cluster resolve to the nearer
    // cluster edge; x beyond the line's laid-out width resolves to EOL.
    TextPosition positionAt(const QPoint& widgetPos) const;

    PerfStats perfStats() const { return perf_; }
    void resetPerfStats() { perf_ = {}; }

signals:
    void cursorMoved(TextPosition pos);
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    // M16 context menu + text DnD.
    void contextMenuEvent(QContextMenuEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;
    QSize sizeHint() const override;

private:
    // --- multiple carets (Phase D1) ---------------------------------------
    // A caret plus its optional selection. The primary caret keeps living
    // in the classic caret_/anchor_/hasSelection_ fields (single-caret
    // semantics unchanged); extraCarets_ holds the additional slots in
    // document order. All fan-out helpers treat the union.
    struct CaretSlot {
        TextPosition caret;
        TextPosition anchor;
        bool hasSelection = false;
    };

    // --- undo mirror (M3) ------------------------------------------------
    // The core owns the op log (see core/TextBuffer.hpp); the view stacks
    // caret/selection states 1:1 underneath it: undoMarks_[i] is the state
    // before core undo-group i (front-aligned after core cap-drops), and
    // redoMarks_[j] is the state an undone group left behind. Alignment is
    // reconciled after every mutating action via (undoDepth, undoPushSerial)
    // deltas, which distinguish "coalesced into top" from "pushed, oldest
    // dropped at the cap" from "nothing recorded".
    struct UndoMark {
        TextPosition caret;
        TextPosition anchor;
        bool hasSelection = false;
        // Multi-caret (D1): the additional slots as they stood before the
        // action. A multi-caret key press is ONE bracketed core group, so
        // one mark covers all carets and undo/redo restores every caret.
        QVector<CaretSlot> extras;
    };
    UndoMark captureEditMark() const;
    void reconcileUndoMarks(const UndoMark& pre); // after any mutating action
    void applyEditMark(const UndoMark& m);        // restore caret/selection
    void breakUndoChain(); // freeze coalescing on deliberate caret motion

    // One fan-out key's edit for a single caret, computed against the
    // PRE-ACTION document. Applying the edits right-to-left keeps every
    // not-yet-applied edit's offsets valid, and a slot's own final caret is
    // its edit's end (payload end minus caretTrim), so it is only shifted
    // by edits strictly LEFT of it when those apply.
    struct SlotEdit {
        int start = 0;
        int end = 0;
        QString text; // replacement (empty = pure delete)
        // Code units trimmed off the payload's END when parking this slot's
        // caret (M14 pair-split Enter): the caret lands at the payload's end
        // minus this much; 0 for every plain replace/insert.
        int caretTrim = 0;
    };
    // The per-slot edit for one fan-out key; return false if the slot edits
    // nothing (e.g. Backspace at the document start).
    using SlotEditFn = std::function<bool(const CaretSlot&, SlotEdit&)>;
    // Fans one action out to all carets (primary included) as ONE bracketed
    // core group. Returns true if anything was edited; also normalizes.
    bool applyCaretsFanOut(const SlotEditFn& editFn);

    // --- indentation machinery (M14; semantics on the public blockIndent/
    // --- indent setters). Lines are collected per slot: caret line always,
    // --- plus every line a selection touches — the selection's END line is
    // --- excluded when the end sits at col 0 (the selection visually ends
    // --- ON it, VS Code's line-count convention). Indent inserts one stop
    // --- of spaces at col 0 of each touched line; outdent removes up to one
    // --- stop of LEADING whitespace (a '\t' counts as the advance to its own
    // --- stop, so mixed-tab files survive the first outdent). All carets
    // --- collapse; one action = one bracketed core group = one undo step.
    bool blockTouchesLines() const; // any caret's selection spans lines?
    QVector<int> blockTouchedLines() const; // sorted+deduped, end-col-0 rule
    void applyBlockEdits(const QVector<SlotEdit>& edits); // bracket + caret remap
    // Single caret, no multi-line selection and no extra carets: Tab
    // inserts spaces to the next stop; a selection under the key is
    // replaced (typing semantics).
    void tabInsertSpaces();
    // The text Enter should insert at one caret of `line` (auto-indent
    // policy, see setAutoIndentEnabled). The pair-split case returns
    // "\n" + childIndent + "\n" + parentIndent.
    QString autoIndentNewline(int line, int col) const;
    static int autoIndentCaretTrim(const QString& payload); // pair-split caret park
    // Indent-edit arithmetic over line pieces (statics: pure functions of
    // (text, stop); the '\t'-aware stop grid is anchored at the line start).
    static QString leadingWsPrefix(const QString& line);
    static int indentWidthOf(const QString& line, int stop);
    static SlotEdit makeIndentEdit(const qmn::core::TextBuffer& buf, int line, int count);
    static int prefixCutForOneStop(const QString& prefix, int stop);
    static SlotEdit makeOutdentEdit(const qmn::core::TextBuffer& buf, int line, int stop);
    static int visualColOf(const QString& line, int col, int stop); // caret col -> stop grid
    // One shared caret-movement target fn applied to every caret (nav fan-
    // out; no document change). `extend` grows each caret's own selection.
    void moveCarets(const std::function<TextPosition(const CaretSlot&)>& targetFn,
                    bool extend);
    // VS Code-style caret cleanup after adds/moves: fold duplicate carets,
    // drop carets strictly inside another slot's selection, and resolve
    // intersecting selections by keeping the LEFT one and collapsing the
    // right slot's selection to its caret. Never drops the primary.
    void normalizeCarets();
    QVector<CaretSlot> allCarets() const; // extras + primary, doc order
    // Sorted [start,end) buffer offsets of a slot's selection (equal ends
    // when the slot has none) — the offset view the normalization rules use.
    static std::pair<int, int> selRangeOf(const qmn::core::TextBuffer& buf, const CaretSlot& s);

    int totalHeight() const;
    void setVScroll(int pixels);
    void updateScrollBarRange();
    void ensureCaretVisible();
    void insertTextAtCaret(const QString& text, int caretTrim = 0);
    void backspace();
    void deleteForward();
    TextPosition clampPos(TextPosition pos) const;
    void setCaret(TextPosition pos); // always collapses the selection
    void moveCaret(TextPosition target, bool extend);
    void setSelection(TextPosition anchor, TextPosition caret);
    bool deleteSelection(); // true if a selection was removed
    // Sorted ends of the current selection (valid only if hasSelection_).
    TextPosition selStart() const;
    TextPosition selEnd() const;
    void pasteText(const QString& text); // sanitize + insert + primary update
    void updatePrimarySelection(); // mirror an active selection to X11 primary
    int lineLength(int line) const;

    // --- M15 multi-click / Ctrl+D ----------------------------------------
    // Ctrl+D: next occurrence of the selection text / caret's word becomes
    // a new SELECTED slot (word-only, never wraps; semantics at the impl).
    void addNextOccurrenceCaret();
    // Commit the triple+-click whole-line span [lineSelAnchor_..lineSelEnd_].
    void applyLineSpan();

    // --- M16 context menu + text DnD --------------------------------------
    // Menu factory behind contextMenuEvent (split out as a test seam: the
    // exec() inside contextMenuEvent would block an offscreen test run).
    // Both sit under protected so the test subclass can reach the seam;
    // the Python binding surface keeps them invisible (internal classes).
protected:
    QMenu* createContextMenu();
    // Right-click caret rules: a click outside the selection re-places the
    // caret first (Qt convention), one inside keeps the selection intact.
    void prepareContextMenuAt(const QPoint& widgetPos);
private:
    // Drag-out of the live selection (armed by a press inside it, fired at
    // startDragDistance): mime = plain text + our move marker; exec prefers
    // Move, and a Move accepted elsewhere still owes the source-side delete.
    void startSelectionDrag();
    // True iff the mime carries our move marker AND it names THIS widget AND
    // the marker's offsets are exactly the live primary selection (a marker
    // from a sibling editor or a round-tripped external copy degrades to a
    // text-only copy drop).
    bool internalMoveSource(const QMimeData* mime, int& srcStart, int& srcEnd) const;
    // Copy-drop insertion at an offset (not at the caret): one bracketed
    // edit, caret at the inserted end, primary mirror like pasteText.
    void insertAtDrop(const QString& text, int dropOff);
    // M1 drag auto-scroll: past the viewport edges scroll and map the point
    // back inside. Shared by mouseMoveEvent and (M16) dragMoveEvent.
    QPoint edgeAutoScroll(QPoint pos);

    // Per-line glyph-engine layout cache. A laid-out QTextLine is the sole
    // authority for caret x, selection x and mouse hit-testing (measured on
    // Qt 6.12: emoji ZWJ families occupy ~1.8 cells, not their code-unit
    // count, so grid arithmetic drifts the caret on such lines).
    //
    // Key = (line index, laid-out text equality): a QTextLine depends only
    // on (text, font, infinite width), so an identical string needs no
    // relayout - no buffer revision needed. Font changes are NOT part of the
    // key; changeEvent(FontChange) wipes the cache instead.
    //
    // QTextLayout is copy/move-DELETED and a default-constructed QTextLine
    // makes x()/cursorToX() SEGFAULT (measured, no eng guard in Qt 6.12), so
    // slots hold the layout in place inside the fixed array and `valid`
    // gates every QTextLine use.
    struct GeoSlot {
        int keyLine = -1; // -1 = empty slot
        QString text;     // what `line` was laid out for (possibly truncated)
        QTextLayout layout;
        QTextLine line;
        quint64 stamp = 0; // LRU bookkeeping
        bool valid = false; // false for empty lines: never touch line
    };
    static constexpr int kGeoSlots = 128; // >= a 4K-tall viewport of lines;
                                          // overflow only costs relayouts
    // Laid out with NoWrap and setLineWidth(Q_INFINITY): the line takes its
    // natural width, horizontal scrolling (M7) is the view's problem. Very
    // long lines are laid out only up to geoMaxUnits() code units.
    const GeoSlot* hitLine(int line) const;
    int geoMaxUnits() const;
    double xInLine(const GeoSlot* s, int col) const; // line-space x (no padding)

    // M4: per-line QTextCharFormat ranges for the highlight engine's
    // merged spans, in line-local laid-out coordinates, snapped so a
    // range edge never splits a grapheme cluster (shared boundaries of
    // adjacent spans snap identically, keeping the partition exact).
    // Empty vector = "no colors for this line, plain path".
    QVector<QTextLayout::FormatRange> formatRangesForLine(int line, const GeoSlot* gs) const;

    mutable std::array<GeoSlot, kGeoSlots> geo_{};
    mutable quint64 geoClock_ = 0;
    void resetGeoCache(); // wipe slots in place (QTextLayout is uncopyable)

    qmn::core::TextBuffer buffer_;
    // M4 highlight engine (declared right after the model it observes).
    std::unique_ptr<HighlightEngine> highlight_;
    QVector<UndoMark> undoMarks_; // aligned with buffer_ undo groups
    QVector<UndoMark> redoMarks_; // aligned with buffer_ redo groups
    qint64 lastPushSerial_ = 0;   // last seen buffer_.undoPushSerial()
    TextPosition caret_;
    TextPosition anchor_; // inactive end of the selection
    bool hasSelection_ = false;
    // D1 additional caret slots in document order (the primary caret is
    // NOT inside this list; allCarets() merges the two views).
    QVector<CaretSlot> extraCarets_;
    // Which extra slot an Alt+Drag currently resizes (index into
    // extraCarets_; -1 = no Alt drag on an extra), plus whether the drag
    // targets the primary caret instead (Alt+Press landed on it).
    int altDragSlot_ = -1;
    bool altDragPrimary_ = false;

    // M15 multi-click + word-drag state. The chain lives across the DblClick
    // events Qt delivers for a fast click run (a re-press at mousePress
    // resets it); wordSelLo_/Hi_ pin the double-clicked word for the drag.
    bool wordDrag_ = false;
    TextPosition wordSelLo_, wordSelHi_;
    int multiClickDepth_ = 0;                  // chained DblClick count (2 = line mode)
    int lineSelAnchor_ = 0, lineSelEnd_ = 0;   // triple+-click line span, inclusive
    int lineSelDir_ = 1;                       // repeat wraps: +1 down, -1 up
    qint64 lastDblMs_ = 0;                     // event timestamps, chaining window
    int lastDblLine_ = -1, lastDblX_ = 0;

    // M16 DnD state. pressInSelection_ arms the drag gesture: a plain press
    // strictly inside the live selection keeps the selection (no collapse),
    // moves under startDragDistance are swallowed, and a release without
    // drag collapses at the release point. dndSelfDragLive_ marks "inside
    // our own QDrag::exec" (drop-side copy/move decisions); dndDropHandled_
    // says the internal drop already performed the whole move, so the
    // post-exec MoveAction branch must not delete the source again.
    bool pressInSelection_ = false;
    QPoint pressPos_;
    bool dndSelfDragLive_ = false;
    bool dndDropHandled_ = false;
    QString preedit_; // IME composition string, display-only: never in buffer_
    QScrollBar* vBar_ = nullptr;
    bool scrollbarSync_ = false;

    QFont font_;
    double cellWidth_ = 8.0;
    int lineHeight_ = 18;
    int ascent_ = 14;

    // --- M14 indentation state (semantics: public API comments) -----------
    int tabStopWidth_ = 4;  // soft-tab stop + literal-tab render grid
    bool autoIndent_ = true; // Enter copies leading whitespace (+ stop)
    bool tabChangesFocus_ = false; // Tab rides Qt's focus chain instead

    int vScroll_ = 0; // pixels
    int padding_ = 4;

    // --- M5 decoration + gutter state (semantics: public API comments) ---
    LineNumbersMode lineNumbersMode_ = LineNumbersMode::On;
    QVector<int> breakpointLines_; // sorted + deduped (binary_search on paint)
    QVector<Diagnostic> diagnostics_; // sorted by (startLine, startCol)
    QVector<InlayHint> inlayHints_; // sorted by (line, col)
    int highlightStart_ = -1; // inclusive whole-line range; -1 = unset
    int highlightEnd_ = -1;
    bool currentLineHl_ = false;
    int gutterInnerPad() const; // gutter digit/glyph padding (M5)

    // --- M6 find/replace state (semantics: public API comments) ---
    // The bar is a QWidget* only here: its class (FindBar) is defined
    // later in CodeEditor.cpp and talks to the editor purely through the
    // public API - internal linkage never crosses the Shiboken boundary.
    QWidget* findBar_ = nullptr; // lazy child; show/hide via findBar_*()
    QString findText_;
    FindFlags findFlags_ = FindFlag::MatchCase;
    // Lazy engine cache: an edit only marks it stale (the paint and every
    // entry point refresh on demand), so the mutable mirrors the M4
    // highlight engine's at-most-once-per-keypress reparse cadence.
    mutable QVector<MatchRange> matches_; // sorted by (startLine, startCol)
    mutable int currentMatch_ = -1;
    mutable bool findDirty_ = false; // edits mark stale; paint/entry refresh
    mutable bool findPatternValid_ = true;
    mutable bool findRepainting_ = false; // set only around the refresh in
                                          // paintEvent: a recompute there
                                          // must not schedule another paint
    void refreshFindMatches(); // no-op unless findDirty_ (not const: a
                               // refresh schedules a repaint via update())
    void recomputeFindMatches(); // full per-line scan (the DoD math)
    void syncFindBarLabel() const; // bar count label after state moves
    bool replaceRange(const MatchRange& m, const QString& replacement);

    // --- M7 overview ruler state (semantics: public API comments) ---
    bool rulerEnabled_ = true; // Monaco-minimap-parity default (the
                               // set_minimap_enabled hook defaults on too)
    bool rulerDrag_ = false;   // left/middle press inside the strip
    void paintOverviewRuler(QPainter& p) const; // last pass: it OVERPAINTS
                                                // the text's right edge
    void rulerScrollTo(int y); // center the strip point's line in the view

    // --- M9 horizontal scroll state (semantics: public API comments) ---
    int hScroll_ = 0;                    // px, clamped like vScroll_
    bool scrollBeyondLastLine_ = false;  // Monaco-parity hook, see public docs
    mutable int docWidthPx_ = 0;         // ambient extent of visited lines
    bool hScrollbarSync_ = false;        // setHScroll <-> hBar_ reentry guard
    QScrollBar* hBar_ = nullptr;
    void setHScroll(int pixels); // the one clamping writer of hScroll_

    // --- M13 folding state (semantics: public API comments) --------------
    // Collapsed folds, model lines. Kept sorted by (start asc, end desc);
    // strictly-contained entries are dropped at collapse time, so any
    // remaining overlap is only partial (stale-edit residue) and rides the
    // run merge below. Ends are clamped to the document on rebuild.
    struct FoldState {
        int start = 0;
        int end = 0;
    };
    QVector<FoldState> collapsed_;
    // Hidden runs derived from collapsed_: disjoint, NON-TOUCHING (runs
    // that abut or overlap are merged — so the line after a run's last is
    // always visible or past the document), sorted, each carrying the
    // count of hidden lines before it. The view-line map is binary search
    // over this table; visibleLines_ caches lineCount() - hidden total.
    struct HiddenRun {
        int first = 0;        // first hidden model line
        int last = 0;         // last hidden model line (inclusive)
        int hiddenBefore = 0; // hidden lines in earlier runs
    };
    static constexpr int kIndentWindow = 256; // indentOfModelLine read cap
    QVector<HiddenRun> hiddenRuns_;
    int foldVisibleTotal_ = 0; // cached visibleLineTotal() (set by rebuildHiddenRuns)
    int foldDocLines_ = 0;     // lineCount() at the last fold sweep (edit diff)
    void rebuildHiddenRuns();  // collapsed_ -> hiddenRuns_/visibleLines_
    void shiftFoldStateForEdit(const qmn::core::TextBuffer::EditEvent& ev);
    // Foldable starts known for the painted window: engine @fold captures
    // (exact ends) where a fold grammar is active, else the indent rule
    // (start-only; ends resolved lazily by foldEndForLine on the actual
    // fold request, keeping paint O(view)). Never both — see
    // indentFoldsActive(). Mutable like the highlight engine: paintEvent
    // keeps it warm.
    mutable QVector<FoldState> foldables_; // sorted by start, one per start
    void recomputeFoldables(int firstModel, int lastModel);
    int foldEndForLine(int line); // exact end (engine full query, else indent scan)
    int indentOfModelLine(int line) const; // leading ws units; -1 = blank line
    // True when the indent heuristic is the fold source: no grammar loaded
    // (plain/unknown language), or the language's composed fold query failed
    // to build (then the engine contributes nothing and the fallback keeps
    // folding alive). In grammar-fold modes (js/json/python) leading
    // whitespace must NOT fake python-style nesting (owner report).
    bool indentFoldsActive() const;
    // Does a COLLAPSED fold's indent source still exist, checked against
    // MODEL lines (the fold itself hid line+1, so recomputeFoldables'
    // visible-adjacency rule can never fire on its own start)? Same rule
    // as foldEndForLine's scan, first step only: the first non-blank
    // model line below the header must out-indent it. False means an
    // edit killed the block and the fold auto-expands.
    bool indentFoldSourceAlive(int header) const;
    // Unfold every collapsed fold strictly containing line (outermost
    // first). True if anything opened. Caret choke points use this (a
    // caret/selection target is never hidden); paint never reveals.
    bool revealLine(int line);
    int nextVisibleModelLine(int line) const; // slot+1 walk (caret jump over folds)
    int prevVisibleModelLine(int line) const; // slot-1 walk
    int slotOfModelLine(int line) const;      // visibleIndexOfModelLine, clamped >= 0
    // Model line whose slot row contains widget y (-1 outside text rows).
    int modelLineForY(int y) const;

    QTimer blinkTimer_;
    bool caretOn_ = true;

    QElapsedTimer perfTimer_;
    PerfStats perf_;
};

// moc rejects the generated binary operators inside a class body, and Qt's
// own in-class flag types (QFont::StyleStrategy &co.) pair Q_DECLARE_FLAGS
// in-class with this macro at namespace scope - same here, so
// FindFlag::MatchCase | FindFlag::UseRegex composes naturally for C++
// consumers (Shiboken exposes the Q_FLAG(FindFlags) meta-flag regardless).
Q_DECLARE_OPERATORS_FOR_FLAGS(CodeEditor::FindFlags)

} // namespace qmn::view

// Needed by the Python binding only: PySide's generic signal connector resolves
// signal parameter types through QMetaType, so cursorMoved(TextPosition) is not
// connectable from Python without this. Qt insists the declaration sit at global
// scope - putting it inside qmn::view does not compile. Harmless for C++ users.
Q_DECLARE_METATYPE(qmn::view::TextPosition)
// Same reason for the M13 fold record (foldableRanges() returns a
// QVector<FoldRange> the binding's container converter must resolve).
Q_DECLARE_METATYPE(qmn::view::CodeEditor::FoldRange)
