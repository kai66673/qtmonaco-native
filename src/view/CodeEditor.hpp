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
};

// Spike editor: a raw QWidget painting visible lines with QPainter.
//
// Deliberately included: viewport-culled monospace painting, caret with
// blinking, keyboard editing (text, Enter, Backspace, Delete, arrows,
// Home/End, PageUp/PageDown), mouse caret placement, vertical scrolling,
// per-keystroke latency instrumentation.
//
// Deliberately NOT included (follow-up milestones): selection, IME
// composition, soft wrap, horizontal scrolling, undo/redo, grapheme-safe
// edits. See README roadmap.
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

    int lineHeight() const { return lineHeight_; }
    double cellWidth() const { return cellWidth_; }
    int padding() const { return padding_; }

    PerfStats perfStats() const { return perf_; }
    void resetPerfStats() { perf_ = {}; }

signals:
    void cursorMoved(TextPosition pos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
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
    void setCaret(TextPosition pos);
    TextPosition posFromEvent(const QPoint& widgetPos) const;
    int lineLength(int line) const;

    qmn::core::TextBuffer buffer_;
    TextPosition caret_;
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
