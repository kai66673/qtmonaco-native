#pragma once

#include <QElapsedTimer>
#include <QFont>
#include <QMetaType>
#include <QPoint>
#include <QScrollBar>
#include <QString>
#include <QTextLayout>
#include <QTextLine>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include "core/TextBuffer.hpp"

#include <array>

class QInputMethodEvent;

namespace qmn::view {

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
// vertical scrolling, per-keystroke latency instrumentation.
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

    int lineHeight() const { return lineHeight_; }
    double cellWidth() const { return cellWidth_; }
    int padding() const { return padding_; }

    // Widget-space x of the caret position at (line, col), following the
    // glyph engine's cluster advances (not a col * cellWidth grid). A col
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
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;
    QSize sizeHint() const override;

private:
    int firstVisibleLine() const;
    int visibleLineCount() const;
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
    };
    UndoMark captureEditMark() const;
    void reconcileUndoMarks(const UndoMark& pre); // after any mutating action
    void applyEditMark(const UndoMark& m);        // restore caret/selection
    void breakUndoChain(); // freeze coalescing on deliberate caret motion

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

    mutable std::array<GeoSlot, kGeoSlots> geo_{};
    mutable quint64 geoClock_ = 0;

    qmn::core::TextBuffer buffer_;
    QVector<UndoMark> undoMarks_; // aligned with buffer_ undo groups
    QVector<UndoMark> redoMarks_; // aligned with buffer_ redo groups
    qint64 lastPushSerial_ = 0;   // last seen buffer_.undoPushSerial()
    TextPosition caret_;
    TextPosition anchor_; // inactive end of the selection
    bool hasSelection_ = false;
    QString preedit_; // IME composition string, display-only: never in buffer_
    QScrollBar* vBar_ = nullptr;
    bool scrollbarSync_ = false;

    QFont font_;
    double cellWidth_ = 8.0;
    int lineHeight_ = 18;
    int ascent_ = 14;

    int vScroll_ = 0; // pixels
    int padding_ = 4;

    QTimer blinkTimer_;
    bool caretOn_ = true;

    QElapsedTimer perfTimer_;
    PerfStats perf_;
};

} // namespace qmn::view

// Needed by the Python binding only: PySide's generic signal connector resolves
// signal parameter types through QMetaType, so cursorMoved(TextPosition) is not
// connectable from Python without this. Qt insists the declaration sit at global
// scope - putting it inside qmn::view does not compile. Harmless for C++ users.
Q_DECLARE_METATYPE(qmn::view::TextPosition)
