#include "CodeEditor.hpp"

#include <QApplication>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>

namespace qmn::view {

CodeEditor::CodeEditor(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("code editor"));

    font_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font_.setPixelSize(14);
    const QFontMetrics fm(font_);
    charWidth_ = fm.horizontalAdvance(QLatin1Char('0'));
    if (charWidth_ <= 0)
        charWidth_ = 8;
    ascent_ = fm.ascent();
    lineHeight_ = fm.ascent() + fm.descent() + fm.leading();
    if (lineHeight_ <= 0)
        lineHeight_ = ascent_ + 4;

    vBar_ = new QScrollBar(Qt::Vertical, this);
    vBar_->hide();
    connect(vBar_, &QScrollBar::valueChanged, this, [this](int value) {
        if (!scrollbarSync_)
            setVScroll(value);
    });

    const int flash = qApp ? qApp->cursorFlashTime() : 1000;
    blinkTimer_.setInterval(flash > 0 ? flash / 2 : 500);
    connect(&blinkTimer_, &QTimer::timeout, this, [this] {
        caretOn_ = !caretOn_;
        update();
    });
}

CodeEditor::~CodeEditor()
{
    if (perf_.count > 0) {
        fprintf(stderr, "CodeEditor typing perf: keys=%lld mean=%lldns max=%lldns\n",
                perf_.count, perf_.meanNs(), perf_.maxNs);
        fflush(stderr);
    }
}

QSize CodeEditor::sizeHint() const
{
    return QSize(800, 600);
}

void CodeEditor::setPlainText(const QString& text)
{
    buffer_.setText(text);
    caret_ = {};
    setVScroll(0);
    updateScrollBarRange();
    update();
}

QString CodeEditor::toPlainText() const
{
    return buffer_.text();
}

int CodeEditor::lineLength(int line) const
{
    if (line < 0 || line >= buffer_.lineCount())
        return 0;
    return buffer_.lineText(line).size();
}

TextPosition CodeEditor::clampPos(TextPosition pos) const
{
    pos.line = std::clamp(pos.line, 0, buffer_.lineCount() - 1);
    pos.col = std::clamp(pos.col, 0, lineLength(pos.line));
    return pos;
}

void CodeEditor::setPosition(TextPosition pos)
{
    setCaret(clampPos(pos));
}

void CodeEditor::setCaret(TextPosition pos)
{
    caret_ = pos;
    caretOn_ = true;
    if (hasFocus())
        blinkTimer_.start();
    ensureCaretVisible();
    updateScrollBarRange();
    update();
    emit cursorMoved(caret_);
}

void CodeEditor::ensureCaretVisible()
{
    const int caretTop = caret_.line * lineHeight_;
    const int caretBottom = caretTop + lineHeight_;
    int target = vScroll_;
    if (caretTop - padding_ < vScroll_)
        target = caretTop - padding_;
    else if (caretBottom + padding_ > vScroll_ + height())
        target = caretBottom + padding_ - height();
    setVScroll(target);
}

int CodeEditor::totalHeight() const
{
    return 2 * padding_ + buffer_.lineCount() * lineHeight_;
}

int CodeEditor::firstVisibleLine() const
{
    return std::clamp((vScroll_ - padding_) / lineHeight_, 0, buffer_.lineCount() - 1);
}

int CodeEditor::visibleLineCount() const
{
    return height() / lineHeight_ + 2;
}

void CodeEditor::setVScroll(int pixels)
{
    const int maxScroll = std::max(0, totalHeight() - height());
    vScroll_ = std::clamp(pixels, 0, maxScroll);
    scrollbarSync_ = true;
    if (vBar_) {
        vBar_->setValue(vScroll_);
        vBar_->setVisible(maxScroll > 0);
    }
    scrollbarSync_ = false;
    update();
}

void CodeEditor::updateScrollBarRange()
{
    if (!vBar_)
        return;
    const int maxScroll = std::max(0, totalHeight() - height());
    vBar_->setRange(0, maxScroll);
    vBar_->setPageStep(std::max(1, height()));
    vBar_->setSingleStep(lineHeight_);
    vBar_->setVisible(maxScroll > 0);
    const int barWidth = vBar_->isVisible() ? vBar_->sizeHint().width() : 0;
    vBar_->setGeometry(width() - barWidth, 0, barWidth, height());
    if (vScroll_ > maxScroll)
        setVScroll(maxScroll);
}

void CodeEditor::insertTextAtCaret(const QString& text)
{
    if (text.isEmpty())
        return;
    const int offset = buffer_.offsetOf(caret_.line, caret_.col);
    buffer_.insertAtOffset(offset, text);
    TextPosition pos = caret_;
    const int newlines = text.count(QLatin1Char('\n'));
    if (newlines > 0) {
        pos.line += newlines;
        pos.col = static_cast<int>(text.size()) - text.lastIndexOf(QLatin1Char('\n')) - 1;
    } else {
        pos.col += static_cast<int>(text.size());
    }
    setCaret(clampPos(pos));
}

void CodeEditor::backspace()
{
    if (caret_.col > 0) {
        const int pos = buffer_.offsetOf(caret_.line, caret_.col);
        buffer_.removeRange(pos - 1, pos);
        setCaret({caret_.line, caret_.col - 1});
    } else if (caret_.line > 0) {
        const int prevLen = lineLength(caret_.line - 1);
        const int nlPos = buffer_.offsetOfLine(caret_.line) - 1;
        buffer_.removeRange(nlPos, nlPos + 1);
        setCaret({caret_.line - 1, prevLen});
    }
}

void CodeEditor::deleteForward()
{
    const int len = lineLength(caret_.line);
    if (caret_.col < len) {
        const int pos = buffer_.offsetOf(caret_.line, caret_.col);
        buffer_.removeRange(pos, pos + 1);
        setCaret(caret_);
    } else if (caret_.line + 1 < buffer_.lineCount()) {
        const int nlPos = buffer_.offsetOfLine(caret_.line + 1) - 1;
        buffer_.removeRange(nlPos, nlPos + 1);
        setCaret(caret_);
    }
}

void CodeEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Base));
    p.setPen(palette().color(QPalette::Text));
    p.setFont(font_);

    const int lh = lineHeight_;
    const int first = firstVisibleLine();
    const int last = std::min(buffer_.lineCount() - 1, first + visibleLineCount());
    const int colsVisible = width() / charWidth_ + 2;

    for (int line = first; line <= last; ++line) {
        const int yTop = padding_ + line * lh - vScroll_;
        const int baseline = yTop + ascent_;

        QString text = buffer_.lineText(line);
        // Spike guard against monster single-line files: only paint what fits.
        if (text.size() > colsVisible)
            text = text.left(colsVisible);

        if (line == caret_.line && caretOn_) {
            const int cx = padding_ + std::min(caret_.col, colsVisible) * charWidth_;
            p.fillRect(QRect(cx, yTop, 2, lh), palette().color(QPalette::Text));
        }
        if (!text.isEmpty())
            p.drawText(padding_, baseline, text);
    }
}

void CodeEditor::keyPressEvent(QKeyEvent* event)
{
    perfTimer_.start();
    auto finish = [this] {
        if (perfTimer_.isValid()) {
            const qint64 ns = perfTimer_.nsecsElapsed();
            perf_.count += 1;
            perf_.sumNs += ns;
            perf_.maxNs = std::max(perf_.maxNs, ns);
            if (perf_.count % 500 == 0) {
                fprintf(stderr, "CodeEditor typing perf: keys=%lld mean=%lldns max=%lldns\n",
                        perf_.count, perf_.meanNs(), perf_.maxNs);
                fflush(stderr);
            }
        }
    };

    const Qt::KeyboardModifiers mods =
        event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    const int key = event->key();

    if (mods != Qt::NoModifier) {
        switch (key) {
        case Qt::Key_Home:
            setPosition({0, 0});
            finish();
            return;
        case Qt::Key_End: {
            const int lastLine = buffer_.lineCount() - 1;
            setPosition({lastLine, lineLength(lastLine)});
            finish();
            return;
        }
        case Qt::Key_Down:
            setVScroll(vScroll_ + lineHeight_);
            finish();
            return;
        case Qt::Key_Up:
            setVScroll(vScroll_ - lineHeight_);
            finish();
            return;
        default:
            event->ignore();
            return;
        }
    }

    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        insertTextAtCaret(QStringLiteral("\n"));
        finish();
        return;
    case Qt::Key_Backspace:
        backspace();
        finish();
        return;
    case Qt::Key_Delete:
        deleteForward();
        finish();
        return;
    case Qt::Key_Left: {
        TextPosition pos = caret_;
        if (pos.col > 0)
            --pos.col;
        else if (pos.line > 0) {
            --pos.line;
            pos.col = lineLength(pos.line);
        }
        setPosition(pos);
        finish();
        return;
    }
    case Qt::Key_Right: {
        TextPosition pos = caret_;
        if (pos.col < lineLength(pos.line))
            ++pos.col;
        else if (pos.line + 1 < buffer_.lineCount()) {
            ++pos.line;
            pos.col = 0;
        }
        setPosition(pos);
        finish();
        return;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        TextPosition pos = caret_;
        pos.line += (key == Qt::Key_Up) ? -1 : 1;
        setPosition(pos);
        finish();
        return;
    }
    case Qt::Key_Home:
        setPosition({caret_.line, 0});
        finish();
        return;
    case Qt::Key_End:
        setPosition({caret_.line, lineLength(caret_.line)});
        finish();
        return;
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        TextPosition pos = caret_;
        const int page = std::max(1, visibleLineCount() - 2);
        pos.line += (key == Qt::Key_PageUp) ? -page : page;
        setPosition(pos);
        finish();
        return;
    }
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        // Indentation policy is a later milestone; swallow for now.
        finish();
        return;
    case Qt::Key_Escape:
        event->ignore();
        return;
    default:
        break;
    }

    const QString text = event->text();
    if (!text.isEmpty() && text[0].isPrint()) {
        insertTextAtCaret(text);
        finish();
        return;
    }
    event->ignore();
}

TextPosition CodeEditor::posFromEvent(const QPoint& widgetPos) const
{
    TextPosition pos;
    pos.line = (widgetPos.y() - padding_ + vScroll_) / lineHeight_;
    pos.line = std::clamp(pos.line, 0, buffer_.lineCount() - 1);
    pos.col = (widgetPos.x() - padding_) / charWidth_;
    pos.col = std::clamp(pos.col, 0, lineLength(pos.line));
    return pos;
}

void CodeEditor::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        setPosition(posFromEvent(event->position().toPoint()));
    }
}

void CodeEditor::wheelEvent(QWheelEvent* event)
{
    const int lines = event->angleDelta().y() / 120 * 3;
    if (lines != 0)
        setVScroll(vScroll_ - lines * lineHeight_);
    event->accept();
}

void CodeEditor::resizeEvent(QResizeEvent*)
{
    updateScrollBarRange();
}

void CodeEditor::focusInEvent(QFocusEvent*)
{
    caretOn_ = true;
    blinkTimer_.start();
    update();
}

void CodeEditor::focusOutEvent(QFocusEvent*)
{
    caretOn_ = false;
    blinkTimer_.stop();
    update();
}

} // namespace qmn::view
