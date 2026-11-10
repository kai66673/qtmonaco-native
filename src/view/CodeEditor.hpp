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
    // width + padding, plus one extra reserved slot while any breakpoint
    // exists (adding a breakpoint must not shift already-drawn numbers).
    // contentLeft() = widget x where text begins: padding_ + gutterWidth().
    // xForCol()/positionAt()/painting all use it, so callers composing
    // widget x read contentLeft(), not padding().
    int gutterWidth() const;
    int contentLeft() const { return padding_ + gutterWidth(); }

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

    int lineHeight() const { return lineHeight_; }
    double cellWidth() const { return cellWidth_; }
    int padding() const { return padding_; } // vertical/top-edge padding

    // Viewport state read-out (the same window paintEvent and the
    // highlight policy consume). Read-only: scrolling itself happens via
    // wheel/scrollbar. Public for consumers, bindings and the policy test.
    int firstVisibleLine() const;
    int visibleLineCount() const;

    // Widget-space x of the caret position at (line, col), following the
    // glyph engine's cluster advances (not a col * cellWidth grid), gutter
    // included (so xForCol(line, 0) == contentLeft()). A col
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
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
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
    // PRE-ACTION document (offsets and newCaret alike). Applying the edits
    // right-to-left keeps every not-yet-applied edit's offsets valid, and
    // a slot's own newCaret is already the post-edit position, so it is
    // only shifted by edits strictly LEFT of it when those apply.
    struct SlotEdit {
        int start = 0;
        int end = 0;
        QString text; // replacement (empty = pure delete)
        TextPosition newCaret;
    };
    // The per-slot edit for one fan-out key; return false if the slot edits
    // nothing (e.g. Backspace at the document start).
    using SlotEditFn = std::function<bool(const CaretSlot&, SlotEdit&)>;
    // Fans one action out to all carets (primary included) as ONE bracketed
    // core group. Returns true if anything was edited; also normalizes.
    bool applyCaretsFanOut(const SlotEditFn& editFn);
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
    void insertTextAtCaret(const QString& text);
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
    QString preedit_; // IME composition string, display-only: never in buffer_
    QScrollBar* vBar_ = nullptr;
    bool scrollbarSync_ = false;

    QFont font_;
    double cellWidth_ = 8.0;
    int lineHeight_ = 18;
    int ascent_ = 14;

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
