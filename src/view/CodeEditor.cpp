#include "CodeEditor.hpp"

#include <QApplication>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>

namespace qmn::view {

CodeEditor::CodeEditor(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("code editor"));

    font_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font_.setPixelSize(14);
    // Monospace cell arithmetic must exactly match what the glyph engine
    // advances the pen by. With fontconfig autohinting the engine lays out
    // glyphs on fractional (design) advances while integer QFontMetrics
    // rounding would make the caret drift left along the line (the cell
    // delta is smaller than the drawn cell width). Disable hinting so both
    // the engine and our cell width use the design advances, and measure
    // them with QFontMetricsF (fractional).
    font_.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF fm(font_);
    cellWidth_ = fm.horizontalAdvance(QLatin1Char('0'));
    if (!(cellWidth_ > 0.0))
        cellWidth_ = 8.0;
    ascent_ = qCeil(fm.ascent());
    lineHeight_ = qCeil(fm.ascent() + fm.descent() + fm.leading());
    if (lineHeight_ <= 0)
        lineHeight_ = ascent_ + 4;
    setFont(font_); // keep QWidget::font() in sync with the metrics above

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
    anchor_ = {};
    hasSelection_ = false;
    setVScroll(0);
    updateScrollBarRange();
    emit selectionChanged();
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
    const bool had = hasSelection_;
    hasSelection_ = false;
    anchor_ = pos;
    caretOn_ = true;
    if (hasFocus())
        blinkTimer_.start();
    ensureCaretVisible();
    updateScrollBarRange();
    update();
    emit cursorMoved(caret_);
    if (had)
        emit selectionChanged();
}

void CodeEditor::setSelection(TextPosition anchor, TextPosition caret)
{
    anchor_ = clampPos(anchor);
    caret_ = clampPos(caret);
    hasSelection_ = !(anchor_ == caret_);
    caretOn_ = true;
    if (hasFocus())
        blinkTimer_.start();
    ensureCaretVisible();
    updateScrollBarRange();
    update();
    emit cursorMoved(caret_);
    emit selectionChanged();
}

void CodeEditor::moveCaret(TextPosition target, bool extend)
{
    target = clampPos(target);
    if (!extend) {
        setCaret(target);
        return;
    }
    if (!hasSelection_)
        anchor_ = caret_; // shift+arrow from no selection: the caret becomes
                          // the anchor; mid-selection the anchor stays put
    if (target == caret_) {
        // At a boundary (e.g. shift+Left at 0:0): nothing to extend into.
        return;
    }
    const bool had = hasSelection_;
    caret_ = target;
    hasSelection_ = !(anchor_ == caret_);
    caretOn_ = true;
    if (hasFocus())
        blinkTimer_.start();
    ensureCaretVisible();
    updateScrollBarRange();
    update();
    emit cursorMoved(caret_);
    if (had != hasSelection_)
        emit selectionChanged();
}

TextPosition CodeEditor::selStart() const
{
    if (anchor_.line < caret_.line
        || (anchor_.line == caret_.line && anchor_.col <= caret_.col))
        return anchor_;
    return caret_;
}

TextPosition CodeEditor::selEnd() const
{
    if (anchor_.line < caret_.line
        || (anchor_.line == caret_.line && anchor_.col <= caret_.col))
        return caret_;
    return anchor_;
}

QString CodeEditor::selectedText() const
{
    if (!hasSelection_)
        return {};
    const int start = buffer_.offsetOf(selStart().line, selStart().col);
    const int end = buffer_.offsetOf(selEnd().line, selEnd().col);
    return buffer_.textRange(start, end);
}

int CodeEditor::selectedLength() const
{
    if (!hasSelection_)
        return 0;
    const int start = buffer_.offsetOf(selStart().line, selStart().col);
    const int end = buffer_.offsetOf(selEnd().line, selEnd().col);
    return end - start;
}

void CodeEditor::selectAll()
{
    const int lastLine = buffer_.lineCount() - 1;
    setSelection({0, 0}, {lastLine, lineLength(lastLine)});
}

void CodeEditor::clearSelection()
{
    if (hasSelection_)
        setSelection(caret_, caret_);
}

bool CodeEditor::deleteSelection()
{
    if (!hasSelection_)
        return false;
    const TextPosition start = selStart();
    const int startOff = buffer_.offsetOf(start.line, start.col);
    const int endOff = buffer_.offsetOf(selEnd().line, selEnd().col);
    hasSelection_ = false;
    buffer_.removeRange(startOff, endOff);
    setCaret(start);
    return true;
}

int CodeEditor::surrogateBackspaceLen() const
{
    // Never split a surrogate pair from the view layer: if the code unit
    // before the caret is a low surrogate preceded by a high one, remove
    // both units (full grapheme clusters are an M2 item).
    if (caret_.col >= 2) {
        const QString line = buffer_.lineText(caret_.line);
        if (line.at(caret_.col - 1).isLowSurrogate() && line.at(caret_.col - 2).isHighSurrogate())
            return 2;
    }
    return 1;
}

int CodeEditor::surrogateDeleteLen() const
{
    if (caret_.col + 1 < lineLength(caret_.line)) {
        const QString line = buffer_.lineText(caret_.line);
        if (line.at(caret_.col).isHighSurrogate() && line.at(caret_.col + 1).isLowSurrogate())
            return 2;
    }
    return 1;
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
    deleteSelection(); // typing replaces the selection (Monaco semantics)
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
    if (deleteSelection())
        return; // Backspace with a selection deletes only the selection
    if (caret_.col > 0) {
        const int len = surrogateBackspaceLen();
        const int pos = buffer_.offsetOf(caret_.line, caret_.col);
        buffer_.removeRange(pos - len, pos);
        setCaret({caret_.line, caret_.col - len});
    } else if (caret_.line > 0) {
        const int prevLen = lineLength(caret_.line - 1);
        const int nlPos = buffer_.offsetOfLine(caret_.line) - 1;
        buffer_.removeRange(nlPos, nlPos + 1);
        setCaret({caret_.line - 1, prevLen});
    }
}

void CodeEditor::deleteForward()
{
    if (deleteSelection())
        return;
    const int len = lineLength(caret_.line);
    if (caret_.col < len) {
        const int units = surrogateDeleteLen();
        const int pos = buffer_.offsetOf(caret_.line, caret_.col);
        buffer_.removeRange(pos, pos + units);
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
    const int colsVisible = static_cast<int>(width() / cellWidth_) + 2;

    // Selection pass first: paint the background under the glyphs.
    // Semi-transparent highlight so text stays legible on light and dark
    // palettes alike (Monaco uses the same trick with ~40% opacity).
    if (hasSelection_) {
        const TextPosition s = selStart();
        const TextPosition e = selEnd();
        QColor selColor = palette().color(QPalette::Highlight);
        selColor.setAlpha(90);
        for (int line = std::max(first, s.line); line <= std::min(last, e.line); ++line) {
            const int startCol = (line == s.line) ? s.col : 0;
            // On fully covered lines the selection extends past the text to
            // visualize the selected newline; on the end line it stops at
            // the selection end. Clamp to what is visible.
            const int textLen = lineLength(line);
            int endCol = (line == e.line) ? e.col : std::max(textLen, 1);
            endCol = std::min(endCol, std::max(colsVisible, startCol + 1));
            const int x0 = padding_ + qRound(startCol * cellWidth_);
            const int x1 = padding_ + qRound(endCol * cellWidth_);
            if (x1 > x0) {
                const int yTop = padding_ + line * lh - vScroll_;
                p.fillRect(QRect(x0, yTop, x1 - x0, lh), selColor);
            }
        }
    }

    for (int line = first; line <= last; ++line) {
        const int yTop = padding_ + line * lh - vScroll_;
        const int baseline = yTop + ascent_;

        QString text = buffer_.lineText(line);
        // Spike guard against monster single-line files: only paint what fits.
        if (text.size() > colsVisible)
            text = text.left(colsVisible);

        if (line == caret_.line && caretOn_) {
            // Caret x must follow the same fractional advance arithmetic the
            // glyph engine uses; anchoring at padding_ keeps it aligned with
            // the text start of the line.
            const int cx = padding_ + qRound(std::min(caret_.col, colsVisible) * cellWidth_);
            p.fillRect(QRect(cx, yTop, 2, lh), palette().color(QPalette::Text));
        }
        if (!text.isEmpty())
            p.drawText(QPointF(padding_, baseline), text);
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

    const Qt::KeyboardModifiers allMods = event->modifiers();
    const bool shift = allMods & Qt::ShiftModifier;
    const Qt::KeyboardModifiers ctrl =
        allMods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    const int key = event->key();

    if (ctrl != Qt::NoModifier) {
        switch (key) {
        case Qt::Key_Home:
            moveCaret({0, 0}, shift);
            finish();
            return;
        case Qt::Key_End: {
            const int lastLine = buffer_.lineCount() - 1;
            moveCaret({lastLine, lineLength(lastLine)}, shift);
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
        case Qt::Key_A:
            selectAll();
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
        moveCaret(pos, shift);
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
        moveCaret(pos, shift);
        finish();
        return;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        // Column is re-clamped to the target line; visual column memory
        // arrives with M9 (soft wrap work). On the first/last line the
        // motion collapses to the line start/end like real editors do.
        TextPosition pos = caret_;
        if (key == Qt::Key_Up) {
            if (pos.line > 0)
                --pos.line;
            else
                pos.col = 0;
        } else {
            if (pos.line + 1 < buffer_.lineCount())
                ++pos.line;
            else
                pos.col = lineLength(pos.line);
        }
        moveCaret(pos, shift);
        finish();
        return;
    }
    case Qt::Key_Home:
        moveCaret({caret_.line, 0}, shift);
        finish();
        return;
    case Qt::Key_End:
        moveCaret({caret_.line, lineLength(caret_.line)}, shift);
        finish();
        return;
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        TextPosition pos = caret_;
        const int page = std::max(1, visibleLineCount() - 2);
        pos.line += (key == Qt::Key_PageUp) ? -page : page;
        moveCaret(pos, shift);
        finish();
        return;
    }
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        // Indentation policy is a later milestone; typing with a selection
        // present, however, must still replace it — but Tab indenting is
        // deferred, so swallow the key for now.
        finish();
        return;
    case Qt::Key_Escape:
        if (hasSelection_) {
            clearSelection();
            finish();
            return;
        }
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
    pos.col = static_cast<int>((widgetPos.x() - padding_) / cellWidth_);
    pos.col = std::clamp(pos.col, 0, lineLength(pos.line));
    return pos;
}

void CodeEditor::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    setFocus(Qt::MouseFocusReason);
    const TextPosition pos = posFromEvent(event->position().toPoint());
    // Shift+click extends the selection from the existing anchor; a plain
    // click collapses it at the new position.
    if (event->modifiers() & Qt::ShiftModifier)
        moveCaret(pos, true);
    else
        setCaret(pos);
}

void CodeEditor::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton)) {
        event->ignore();
        return;
    }
    QPoint pos = event->position().toPoint();
    // Drag auto-scroll: moving past the top/bottom edge scrolls one line
    // per event and maps the position inside the viewport.
    const int margin = padding_ + 2;
    if (pos.y() < margin) {
        setVScroll(vScroll_ - lineHeight_);
        pos.setY(margin + 1);
    } else if (pos.y() > height() - margin) {
        setVScroll(vScroll_ + lineHeight_);
        pos.setY(height() - margin - 1);
    }
    moveCaret(posFromEvent(pos), true);
    event->accept();
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
