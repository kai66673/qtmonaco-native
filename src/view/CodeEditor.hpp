#pragma once

#include <QElapsedTimer>
#include <QFont>
#include <QPoint>
#include <QScrollBar>
#include <QString>
#include <QTimer>
#include <QWidget>

#include "core/TextBuffer.hpp"

namespace qmn::view {

// 0-based text position. col is counted in UTF-16 code units.
struct TextPosition {
    int line = 0;
    int col = 0;

    bool operator==(const TextPosition&) const = default;
};

// Editor widget: a raw QWidget painting visible lines with QPainter.
//
// Deliberately included: viewport-culled monospace painting, caret with
// blinking, keyboard editing (text, Enter, Backspace, Delete, arrows,
// Home/End, PageUp/PageDown), mouse caret placement and drag selection,
// Shift-navigation + Ctrl+A selections with replace-on-type semantics,
// vertical scrolling, per-keystroke latency instrumentation.
//
// Deliberately NOT included (follow-up milestones): clipboard, IME
// composition, soft wrap, horizontal scrolling, undo/redo, column memory
// for Up/Down, word navigation, double-click word selection, full
// grapheme-cluster safety. See README roadmap.
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

    int lineHeight() const { return lineHeight_; }
    double cellWidth() const { return cellWidth_; }
    int padding() const { return padding_; }

    PerfStats perfStats() const { return perf_; }
    void resetPerfStats() { perf_ = {}; }

signals:
    void cursorMoved(TextPosition pos);
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
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
    TextPosition posFromEvent(const QPoint& widgetPos) const;
    int lineLength(int line) const;
    int surrogateBackspaceLen() const; // 2 if a surrogate pair precedes caret
    int surrogateDeleteLen() const;    // 2 if a surrogate pair follows caret

    qmn::core::TextBuffer buffer_;
    TextPosition caret_;
    TextPosition anchor_; // inactive end of the selection
    bool hasSelection_ = false;
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
