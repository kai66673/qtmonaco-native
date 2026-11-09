#include "CodeEditor.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStringList>
#include <QTextBoundaryFinder>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>

namespace qmn::view {

namespace {

// Document order for text positions (multi-caret bookkeeping). Lexicographic
// (line, col) is exactly document order for resolved positions.
bool posLess(const TextPosition& a, const TextPosition& b)
{
    return a.line != b.line ? a.line < b.line : a.col < b.col;
}

// M2 grapheme/word boundary helpers. QTextBoundaryFinder is ICU-backed on
// both of our build targets (Arch's Qt links ICU 78; PySide6's Qt bundles
// ICU) — measured here: ZWJ family sequences, regional-indicator flags,
// skin-tone modifiers, VS16 and combining marks each form exactly ONE
// cluster. Qt's non-ICU fallback tables predate the GB11 ZWJ rule, so the
// ICU linkage is load-bearing (and both shipped Qt builds have it).
// Probed semantics (Qt 6.12): after setPosition(col), toPrevious/NextBoundary
// move strictly outward whether col is exactly on a boundary or inside a
// cluster; at the string edges they return -1, so callers clamp.

int prevGraphemeCol(const QString& text, int col)
{
    QTextBoundaryFinder f(QTextBoundaryFinder::Grapheme, text);
    f.setPosition(col);
    const int pos = f.toPreviousBoundary();
    return pos < 0 ? 0 : pos;
}

int nextGraphemeCol(const QString& text, int col)
{
    QTextBoundaryFinder f(QTextBoundaryFinder::Grapheme, text);
    f.setPosition(col);
    const int pos = f.toNextBoundary();
    return pos < 0 ? static_cast<int>(text.size()) : pos;
}

bool isGraphemeBoundary(const QString& text, int col)
{
    QTextBoundaryFinder f(QTextBoundaryFinder::Grapheme, text);
    f.setPosition(col);
    return f.isAtBoundary();
}

// Editor-style word motion (VS Code semantics), deliberately not the ICU
// Word boundary: UAX#29 words treat "foo_bar" as several words and would
// land on space runs, which is not what an editor does. Identifier-ish
// class: letters/digits/underscore; whitespace and everything else
// (punctuation, emoji runs) are their own classes. One press crosses one
// run of the current class; Ctrl+Left additionally swallows the space run
// left behind, so "word |" -> previous word start in a single press.
enum class WordClass { Word, Space, Other };

WordClass wordClassOf(QChar ch)
{
    if (ch.isSpace())
        return WordClass::Space;
    if (ch.isLetterOrNumber() || ch == u'_')
        return WordClass::Word;
    return WordClass::Other;
}

int wordRightCol(const QString& text, int col)
{
    const int n = static_cast<int>(text.size());
    if (col >= n)
        return col; // at EOL; the caller decides about crossing lines
    const WordClass cls = wordClassOf(text.at(col));
    int i = col;
    while (i < n && wordClassOf(text.at(i)) == cls)
        ++i;
    // A stop inside a cluster (e.g. right after the base of "e + U+0301"):
    // never split a cluster, snap the target to its right edge.
    if (i < n && !isGraphemeBoundary(text, i))
        i = nextGraphemeCol(text, i);
    return i;
}

int wordLeftCol(const QString& text, int col)
{
    const int n = static_cast<int>(text.size());
    int i = std::min(col, n);
    // Snap out of a cluster first, then move by class runs.
    if (i > 0 && !isGraphemeBoundary(text, i))
        i = prevGraphemeCol(text, i);
    if (i <= 0)
        return 0;
    if (wordClassOf(text.at(i - 1)) == WordClass::Space) {
        while (i > 0 && wordClassOf(text.at(i - 1)) == WordClass::Space)
            --i;
        if (i > 0) { // then cross the single run before the spaces
            const WordClass cls = wordClassOf(text.at(i - 1));
            while (i > 0 && wordClassOf(text.at(i - 1)) == cls)
                --i;
        }
    } else {
        const WordClass cls = wordClassOf(text.at(i - 1));
        while (i > 0 && wordClassOf(text.at(i - 1)) == cls)
            --i;
    }
    if (i > 0 && !isGraphemeBoundary(text, i))
        i = prevGraphemeCol(text, i);
    return i;
}

} // namespace

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
    preedit_.clear(); // a programmatic document replace ends any composition
    buffer_.setText(text); // also clears the core undo/redo stacks
    undoMarks_.clear();
    redoMarks_.clear();
    lastPushSerial_ = buffer_.undoPushSerial();
    caret_ = {};
    anchor_ = {};
    hasSelection_ = false;
    extraCarets_.clear(); // the document replace collapses to one caret
    altDragSlot_ = -1;
    altDragPrimary_ = false;
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

// --- per-line glyph-engine layout cache (caret/selection/hit-test truth) --
//
// Qt 6.12 removed QTextLine::x(pos)/xToPos/cursorRect; the surviving
// primitives cursorToX(int*, Edge)/xToCursor are ancient API, so they are
// also present at our Qt 6.6 floor. Measured semantics (probe):
//  - ASCII monospace: cursorToX(c) == c * cellWidth EXACTLY (the old grid
//    stays bit-for-bit intact for plain lines);
//  - col inside a cluster: the engine snaps the col to a cluster edge and
//    reports it via the out-param;
//  - xToCursor: floor-like on monospace, nearer-edge resolution inside wide
//    clusters, but x=1e9 wraps around to 0 -> callers must clamp first;
//  - emoji ZWJ family advance = 1.77 cells while it spans 8 code units:
//    this is exactly the drift the grid model had;
//  - touching any accessor of a default-constructed QTextLine SEGFAULTS
//    (no eng guard in the .so), hence the valid gate on every slot use.

int CodeEditor::geoMaxUnits() const
{
    // Layout budget for monster lines. 4x the visible cell count covers even
    // lines where clusters are far narrower than their code-unit count
    // (combining marks); the truncation edge always lands off-screen, so the
    // caret/hit-test math never notices it (horizontal scrolling is M7).
    return std::max(64, 4 * (static_cast<int>(width() / cellWidth_) + 2));
}

const CodeEditor::GeoSlot* CodeEditor::hitLine(int line) const
{
    if (line < 0 || line >= buffer_.lineCount())
        return nullptr;
    const QString full = buffer_.lineText(line);
    const int maxUnits = geoMaxUnits();
    const QString text = full.size() > maxUnits ? full.left(maxUnits) : full;

    for (GeoSlot& s : geo_) // geo_ is mutable: this is a const-by-API cache
        if (s.keyLine == line && s.text == text) {
            s.stamp = ++geoClock_;
            return &s;
        }

    GeoSlot* victim = nullptr;
    for (GeoSlot& s : geo_) {
        if (s.keyLine < 0) { // first free slot wins
            victim = &s;
            break;
        }
        if (!victim || s.stamp < victim->stamp)
            victim = &s; // plain-scan LRU victim; kGeoSlots is tiny
    }
    Q_ASSERT(victim);
    victim->keyLine = line;
    victim->text = text;
    if (text.isEmpty()) {
        // Empty line: NO QTextLine exists. Dead-line calls segfault, so the
        // slot is marked invalid and every consumer routes around `line`.
        victim->valid = false;
        victim->line = QTextLine();
        victim->stamp = ++geoClock_;
        return victim;
    }
    // Re-arm the in-place layout (it is copy/move-deleted: never reseat it).
    victim->layout.clearLayout();
    victim->layout.setText(text);
    victim->layout.setFont(font_);
    QTextOption opt;
    opt.setWrapMode(QTextOption::NoWrap);
    victim->layout.setTextOption(opt);
    victim->layout.beginLayout();
    victim->line = victim->layout.createLine();
    if (victim->line.isValid())
        victim->line.setLineWidth(Q_INFINITY); // natural width; view scrolls
    victim->layout.endLayout();
    victim->valid = victim->line.isValid();
    victim->stamp = ++geoClock_;
    return victim;
}

// Line-space x (before padding_) of caret position `col`, clamped to a
// phantom-cell extrapolation beyond the laid-out prefix (monster-line guard
// and the selected-newline cell both land there; the region is off-screen).
double CodeEditor::xInLine(const GeoSlot* s, int col) const
{
    if (!s)
        return 0.0;
    if (!s->valid) // empty line: col 0 or the phantom newline cell
        return std::max(col, 0) * cellWidth_;
    const int laidOut = static_cast<int>(s->text.size());
    if (col <= 0)
        return 0.0;
    if (col > laidOut) {
        int end = laidOut;
        const double xEnd = s->line.cursorToX(&end, QTextLine::Leading);
        return xEnd + (col - laidOut) * cellWidth_;
    }
    int snapped = col; // engine may move a mid-cluster caret to an edge
    return s->line.cursorToX(&snapped, QTextLine::Leading);
}

double CodeEditor::xForCol(int line, int col) const
{
    const int c = std::clamp(col, 0, lineLength(line));
    return padding_ + xInLine(hitLine(line), c);
}

TextPosition CodeEditor::positionAt(const QPoint& widgetPos) const
{
    TextPosition pos;
    pos.line = (widgetPos.y() - padding_ + vScroll_) / lineHeight_;
    pos.line = std::clamp(pos.line, 0, buffer_.lineCount() - 1);
    const GeoSlot* s = hitLine(pos.line);
    if (!s || !s->valid) {
        pos.col = 0; // empty line: every x is the line start
    } else {
        // xToCursor misbehaves far outside the laid-out extent (measured:
        // 1e9 maps back to 0), so clamp into the extent first: clicks past
        // the line end resolve to EOL, as they used to via the grid clamp.
        const double x = std::clamp(widgetPos.x() - static_cast<double>(padding_),
                                    0.0, s->line.naturalTextWidth());
        // xToCursor picks the NEAREST cursor edge; the old grid used floor
        // (click at col*cell + cw/2 -> col), and every editor does the same.
        // Walk back until the position's left edge is at or left of x - the
        // grapheme-aware previousCursorPosition chain stops exactly at valid
        // cluster edges, so a mid-wide-cluster click floors to the cluster
        // start while x just past it jumps to the next cluster.
        int c = std::max(s->line.xToCursor(x), 0);
        while (c > 0) {
            int snapped = c;
            const double left = s->line.cursorToX(&snapped, QTextLine::Leading);
            if (left <= x) {
                c = snapped;
                break;
            }
            c = s->layout.previousCursorPosition(snapped);
        }
        pos.col = std::clamp(c, 0, lineLength(pos.line));
    }
    return pos;
}

TextPosition CodeEditor::clampPos(TextPosition pos) const
{
    pos.line = std::clamp(pos.line, 0, buffer_.lineCount() - 1);
    pos.col = std::clamp(pos.col, 0, lineLength(pos.line));
    return pos;
}

void CodeEditor::setPosition(TextPosition pos)
{
    breakUndoChain(); // a deliberate programmatic move ends any edit chain
    clearAdditionalCarets(); // programmatic single-caret semantics (D1)
    setCaret(clampPos(pos));
}

void CodeEditor::setCaret(TextPosition pos)
{
    preedit_.clear(); // any deliberate caret move ends an in-flight composition
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
    breakUndoChain(); // deliberate selection motion ends any edit chain
    extraCarets_.clear(); // a programmatic selection is single-caret (D1)
    altDragSlot_ = -1;
    altDragPrimary_ = false;
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
    updatePrimarySelection();
}

void CodeEditor::moveCaret(TextPosition target, bool extend)
{
    preedit_.clear(); // motion keys end an in-flight composition (see setCaret)
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
    updatePrimarySelection(); // keep primary in step with shift-drag/motion
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
    breakUndoChain(); // deliberate selection motion ends any edit chain
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
    updatePrimarySelection(); // the removed text must not linger in primary
    emit selectionChanged();  // the edit path also annihilates a selection:
    // keep the signal as faithful as the navigation paths already are
    return true;
}

// --- Clipboard (M1b) ----------------------------------------------------

void CodeEditor::copy()
{
    QClipboard* cb = QGuiApplication::clipboard();
    if (!cb)
        return;
    if (!extraCarets_.isEmpty()) {
        // Multi-caret copy (D1, VS Code): with any selections active the
        // clipboard is those selection texts joined by newlines (carets
        // without a selection contribute nothing); with none it is the
        // distinct caret lines in document order joined by newlines.
        QStringList parts;
        bool anySel = false;
        for (const CaretSlot& s : allCarets()) {
            if (!s.hasSelection)
                continue;
            anySel = true;
            const auto [st, en] = selRangeOf(buffer_, s);
            parts << buffer_.textRange(st, en);
        }
        if (!anySel) {
            int prevLine = -1;
            for (const CaretSlot& s : allCarets()) {
                if (s.caret.line == prevLine)
                    continue; // two carets on one line copy that line once
                prevLine = s.caret.line;
                parts << buffer_.lineText(s.caret.line);
            }
        }
        cb->setText(parts.join(QLatin1Char('\n')), QClipboard::Clipboard);
        return;
    }
    if (hasSelection_) {
        cb->setText(selectedText(), QClipboard::Clipboard);
        return; // primary already mirrors the live selection
    }
    // Empty selection: copy the whole current line, Monaco-style. A
    // non-last line comes with its trailing newline (so one Ctrl+C, Ctrl+V
    // sequence clones the line wherever the caret sits); the last line has
    // no newline of its own to give.
    const int line = caret_.line;
    const int start = buffer_.offsetOfLine(line);
    const int end = (line + 1 < buffer_.lineCount())
        ? buffer_.offsetOfLine(line + 1)
        : buffer_.offsetOf(line, lineLength(line));
    cb->setText(buffer_.textRange(start, end), QClipboard::Clipboard);
}

void CodeEditor::cut()
{
    QClipboard* cb = QGuiApplication::clipboard();
    if (!cb)
        return;
    // Unbracketed: the cut's delete may coalesce with an adjacent delete
    // chain (the preceding selection motion already froze any older chain).
    const UndoMark pre = captureEditMark();
    if (!extraCarets_.isEmpty()) {
        // Multi-caret cut (D1). VS Code line-cut semantics with selections
        // present are a documented v1 divergence: any selections cut delete
        // ALL selections and the clipboard is the selection texts joined
        // (carets without a selection stay put, riding the remap).
        bool anySel = false;
        for (const CaretSlot& s : extraCarets_)
            anySel = anySel || s.hasSelection;
        anySel = anySel || hasSelection_;
        if (anySel) {
            QStringList parts;
            for (const CaretSlot& s : allCarets()) {
                if (!s.hasSelection)
                    continue;
                const auto [st, en] = selRangeOf(buffer_, s);
                parts << buffer_.textRange(st, en);
            }
            cb->setText(parts.join(QLatin1Char('\n')), QClipboard::Clipboard);
            applyCaretsFanOut([this](const CaretSlot& s, SlotEdit& ed) {
                if (!s.hasSelection)
                    return false;
                const auto [st, en] = selRangeOf(buffer_, s);
                ed.start = st;
                ed.end = en;
                return true;
            });
            reconcileUndoMarks(pre);
            return;
        }
        // No selections: whole-line cut at every caret (distinct lines once,
        // document order). Clipboard = the line texts joined by newlines
        // (the join replaces the per-line '\n'; a trailing-newline-free
        // v1 clipboard).
        QStringList parts;
        QVector<int> lines; // distinct caret lines, ascending (allCarets is caret-sorted)
        for (const CaretSlot& s : allCarets()) {
            if (lines.isEmpty() || lines.last() != s.caret.line)
                lines.append(s.caret.line);
        }
        for (const int line : lines)
            parts << buffer_.lineText(line);
        // Cut ranges: [lineStart, nextLineStart) carries the line's own
        // '\n'; the last line borrows the PRECEDING newline (as the single
        // caret path does) so no phantom trailing line appears. Adjacent
        // ranges then merge into one delete.
        QVector<std::pair<int, int>> ranges; // ascending, will be merged
        for (const int line : lines) {
            int st, en;
            if (line + 1 < buffer_.lineCount()) {
                st = buffer_.offsetOfLine(line);
                en = buffer_.offsetOfLine(line + 1);
            } else {
                st = line > 0
                    ? buffer_.offsetOf(line - 1, lineLength(line - 1))
                    : buffer_.offsetOfLine(0);
                en = buffer_.offsetOf(line, lineLength(line));
            }
            if (!ranges.isEmpty() && st <= ranges.last().second)
                ranges.last().second = std::max(ranges.last().second, en);
            else
                ranges.append({st, en});
        }
        cb->setText(parts.join(QLatin1Char('\n')), QClipboard::Clipboard);
        // Caret landing: every cut caret ends at the start of its (merged)
        // cut range, i.e. where the deleted block used to begin. Track the
        // pre-action caret offsets right-to-left like the fan-out does.
        struct CutSlot {
            CaretSlot slot;
            bool primary;
            int fp;
        };
        QVector<CutSlot> cutSlots;
        for (const CaretSlot& s : extraCarets_)
            cutSlots.append({s, false, buffer_.offsetOf(s.caret.line, s.caret.col)});
        cutSlots.append({CaretSlot{caret_, anchor_, hasSelection_}, true,
                         buffer_.offsetOf(caret_.line, caret_.col)});
        buffer_.beginEditGroup(); // whole multi-line cut = one undo step
        for (int i = ranges.size() - 1; i >= 0; --i) {
            const auto [st, en] = ranges[i];
            for (CutSlot& sl : cutSlots) {
                if (en <= sl.fp)
                    sl.fp += st - en;
                else if (st < sl.fp && sl.fp < en)
                    sl.fp = st;
            }
            buffer_.removeRange(st, en);
        }
        buffer_.endEditGroup();
        for (CutSlot& sl : cutSlots) {
            const auto lc = buffer_.lineColAt(sl.fp);
            sl.slot.caret = clampPos(TextPosition{lc.line, lc.col});
            sl.slot.anchor = sl.slot.caret;
            sl.slot.hasSelection = false;
        }
        for (const CutSlot& sl : cutSlots) {
            if (sl.primary) {
                caret_ = sl.slot.caret;
                anchor_ = sl.slot.anchor;
                hasSelection_ = sl.slot.hasSelection;
            }
        }
        QVector<CaretSlot> kept;
        for (const CutSlot& sl : cutSlots) {
            if (!sl.primary)
                kept.append(sl.slot);
        }
        std::stable_sort(kept.begin(), kept.end(),
                         [](const CaretSlot& a, const CaretSlot& b) { return posLess(a.caret, b.caret); });
        extraCarets_ = kept;
        normalizeCarets(); // stacked landing points fold (documented v1)
        caretOn_ = true;
        if (hasFocus())
            blinkTimer_.start();
        ensureCaretVisible();
        updateScrollBarRange();
        update();
        emit cursorMoved(caret_);
        emit selectionChanged();
        updatePrimarySelection();
        reconcileUndoMarks(pre);
        return;
    }
    if (hasSelection_) {
        cb->setText(selectedText(), QClipboard::Clipboard);
        deleteSelection(); // also drops the primary mirror
        reconcileUndoMarks(pre);
        return;
    }
    const int line = caret_.line;
    if (line + 1 < buffer_.lineCount()) {
        // Whole-line cut (newline included): the following line slides up
        // and the caret waits at its start, ready for another Ctrl+X.
        const int start = buffer_.offsetOfLine(line);
        const int end = buffer_.offsetOfLine(line + 1);
        cb->setText(buffer_.textRange(start, end), QClipboard::Clipboard);
        buffer_.removeRange(start, end);
        setCaret({line, 0});
    } else {
        // Last line: take the newline that ends the previous line instead
        // (VS Code line-cut semantics), so the document does not gain a
        // phantom trailing line; the caret lands at the previous line end.
        const int start = line > 0
            ? buffer_.offsetOf(line - 1, lineLength(line - 1)) // the '\n' before this line
            : buffer_.offsetOfLine(0);
        const int end = buffer_.offsetOf(line, lineLength(line));
        cb->setText(buffer_.textRange(start, end), QClipboard::Clipboard);
        buffer_.removeRange(start, end);
        if (line > 0)
            setCaret({line - 1, lineLength(line - 1)});
        else
            setCaret({0, 0});
    }
    reconcileUndoMarks(pre);
}

void CodeEditor::paste()
{
    QClipboard* cb = QGuiApplication::clipboard();
    if (!cb)
        return;
    pasteText(cb->text(QClipboard::Clipboard));
}

void CodeEditor::pasteText(const QString& text)
{
    QString clean = text;
    // The buffer only knows '\n' separators, so clipboard text arriving with
    // '\r\n' (Windows) or lone '\r' must be normalized. Order matters: the
    // two-char sequence first, then whatever '\r' remains.
    clean.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    clean.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (clean.isEmpty())
        return; // empty clipboard: paste is a no-op, not a delete
    const UndoMark pre = captureEditMark();
    // Paste is its own undo step: the bracket fences it off from the
    // typing chain (even when inserted right after typed text) and merges
    // a replace-selection + insert into one compound group. begin/
    // endEditGroup nest, so the bracket inside insertTextAtCaret (over a
    // selection) commits only at this outer end.
    buffer_.beginEditGroup();
    insertTextAtCaret(clean);
    buffer_.endEditGroup();
    if (QClipboard* cb = QGuiApplication::clipboard();
        cb && cb->supportsSelection()) {
        // X11 convention: the freshly pasted text becomes the new primary
        // selection (the old primary was the selection it just replaced).
        cb->setText(clean, QClipboard::Selection);
    }
    reconcileUndoMarks(pre);
}

// --- undo/redo mirror (M3) -----------------------------------------------
// The core TextBuffer owns the op log; the view keeps caret/selection
// states stacked underneath it 1:1 and reconciles after every mutating
// action. See the UndoMark declaration in the header for the alignment
// scheme.

CodeEditor::UndoMark CodeEditor::captureEditMark() const
{
    return UndoMark{caret_, anchor_, hasSelection_, extraCarets_};
}

void CodeEditor::reconcileUndoMarks(const UndoMark& pre)
{
    const int u = buffer_.undoDepth();
    const qint64 s = buffer_.undoPushSerial();
    if (s > lastPushSerial_) {
        // A fresh group landed (one action pushes at most one group;
        // coalescing and no-ops leave the serial alone): its mark is the
        // pre-action state captured at the action's start.
        undoMarks_.append(pre);
    }
    lastPushSerial_ = s;
    // The core drops oldest groups at its history caps: realign by
    // dropping our oldest marks too.
    if (undoMarks_.size() > u)
        undoMarks_.remove(0, undoMarks_.size() - u);
    if (buffer_.redoDepth() == 0)
        redoMarks_.clear(); // the core truncated the redo branch
    Q_ASSERT(undoMarks_.size() == static_cast<int>(buffer_.undoDepth()));
}

void CodeEditor::applyEditMark(const UndoMark& m)
{
    // Text changed under the marks, so positions are re-clamped (a mark is
    // normally in range by construction, but clamping keeps state total).
    preedit_.clear();
    caret_ = clampPos(m.caret);
    anchor_ = clampPos(m.anchor);
    hasSelection_ = m.hasSelection && !(anchor_ == caret_);
    // Multi-caret (D1): the mark was taken before the action, so the text
    // restored by the core puts these positions back in range exactly.
    extraCarets_.clear();
    for (const CaretSlot& s : m.extras) {
        CaretSlot r;
        r.caret = clampPos(s.caret);
        r.anchor = clampPos(s.anchor);
        r.hasSelection = s.hasSelection && !(r.caret == r.anchor);
        extraCarets_.append(r);
    }
    altDragSlot_ = -1;
    caretOn_ = true;
    if (hasFocus())
        blinkTimer_.start();
    ensureCaretVisible();
    updateScrollBarRange();
    update();
    emit cursorMoved(caret_);
    emit selectionChanged();
    updatePrimarySelection(); // keep primary in step with the restored state
}

void CodeEditor::breakUndoChain()
{
    buffer_.breakUndoCoalescing();
}

// --- multiple carets (Phase D1) ------------------------------------------
// extraCarets_ holds every slot except the primary (caret_/anchor_/
// hasSelection_ keep their exact single-caret semantics, so every
// single-caret path below stays byte-for-byte the behavior the suite
// pins). Fan-out paths run only when extraCarets_ is non-empty.

std::pair<int, int> CodeEditor::selRangeOf(const qmn::core::TextBuffer& buf, const CaretSlot& s)
{
    // Same sorted-ends logic as the primary's selStart()/selEnd(), factored
    // out so the normalization rules can address every slot alike. Ends are
    // equal when the slot carries no selection.
    if (s.caret == s.anchor)
        return {buf.offsetOf(s.caret.line, s.caret.col), buf.offsetOf(s.caret.line, s.caret.col)};
    const int a = buf.offsetOf(s.caret.line, s.caret.col);
    const int b = buf.offsetOf(s.anchor.line, s.anchor.col);
    return a < b ? std::pair{a, b} : std::pair{b, a};
}

QVector<TextPosition> CodeEditor::additionalCarets() const
{
    QVector<TextPosition> out;
    out.reserve(extraCarets_.size());
    for (const CaretSlot& s : extraCarets_)
        out.append(s.caret);
    return out;
}

void CodeEditor::clearAdditionalCarets()
{
    if (extraCarets_.isEmpty())
        return;
    extraCarets_.clear();
    altDragSlot_ = -1;
    update();
    emit selectionChanged(); // the painted extra selections are gone
}

QVector<CodeEditor::CaretSlot> CodeEditor::allCarets() const
{
    QVector<CaretSlot> all = extraCarets_;
    all.append(CaretSlot{caret_, anchor_, hasSelection_});
    std::stable_sort(all.begin(), all.end(),
                     [](const CaretSlot& a, const CaretSlot& b) { return posLess(a.caret, b.caret); });
    return all;
}

void CodeEditor::normalizeCarets()
{
    if (extraCarets_.isEmpty())
        return;

    // Refs view the whole caret set (extras + primary) so the rules below
    // can express "the extra goes, not the primary".
    struct Ref {
        CaretSlot slot;
        bool primary;
        bool dead = false;
    };
    QVector<Ref> refs;
    refs.reserve(extraCarets_.size() + 1);
    for (const CaretSlot& s : extraCarets_)
        refs.append({s, false, false});
    refs.append({CaretSlot{caret_, anchor_, hasSelection_}, true, false});

    // R1: exact duplicate slots (same caret AND selection): the primary
    // wins, the later extra is dropped.
    for (int i = 0; i < refs.size(); ++i) {
        if (refs[i].dead)
            continue;
        for (int j = i + 1; j < refs.size(); ++j) {
            if (refs[j].dead)
                continue;
            const CaretSlot& a = refs[i].slot;
            const CaretSlot& b = refs[j].slot;
            if (a.caret == b.caret && a.anchor == b.anchor && a.hasSelection == b.hasSelection)
                refs[refs[j].primary ? i : j].dead = true; // primary survives
        }
    }

    // R2: overlapping selections. A fully contained selection loses its
    // whole slot when it belongs to an extra (it is redundant with the
    // outer caret anyway); when the contained one is primary, the extra
    // holding the outer selection goes instead. Partial overlaps keep the
    // LEFT-starting selection and collapse the other to a bare caret.
    auto overlaps = [this](const CaretSlot& a, const CaretSlot& b) {
        if (!a.hasSelection || !b.hasSelection)
            return false;
        const auto [as, ae] = selRangeOf(buffer_, a);
        const auto [bs, be] = selRangeOf(buffer_, b);
        return as < be && bs < ae;
    };
    auto contains = [this](const CaretSlot& outer, const CaretSlot& inner) {
        const auto [os, oe] = selRangeOf(buffer_, outer);
        const auto [is, ie] = selRangeOf(buffer_, inner);
        return os <= is && ie <= oe && !(os == is && ie == oe); // strictly smaller
    };
    for (int i = 0; i < refs.size(); ++i) {
        if (refs[i].dead || !refs[i].slot.hasSelection)
            continue;
        for (int j = i + 1; j < refs.size(); ++j) {
            if (refs[j].dead || !refs[j].slot.hasSelection)
                continue;
            if (!overlaps(refs[i].slot, refs[j].slot))
                continue;
            Ref& a = refs[i];
            Ref& b = refs[j];
            if (contains(a.slot, b.slot)) { // b inside a
                if (b.primary)
                    a.dead = true; // the extra yields to the primary
                else
                    b.dead = true;
            } else if (contains(b.slot, a.slot)) { // a inside b
                if (a.primary)
                    b.dead = true;
                else
                    a.dead = true;
            } else { // partial overlap: the LEFT selection keeps its range
                const auto [as, ae] = selRangeOf(buffer_, a.slot);
                const auto [bs, be] = selRangeOf(buffer_, b.slot);
                Ref* loser = nullptr;
                if (as != bs)
                    loser = as < bs ? &b : &a;
                else
                    loser = a.primary ? &b : &a; // equal starts: primary wins
                if (loser->primary) { // the primary never dies: it just
                    loser->slot.hasSelection = false; // collapses to a caret
                    loser->slot.anchor = loser->slot.caret;
                } else {
                    loser->dead = true;
                }
            }
        }
    }

    // R3: a bare caret sitting strictly INSIDE another slot's selection is
    // redundant; if that caret is primary, the extra's selection goes.
    for (int i = 0; i < refs.size(); ++i) {
        if (refs[i].dead || refs[i].slot.hasSelection)
            continue;
        for (int j = 0; j < refs.size(); ++j) {
            if (i == j || refs[j].dead || !refs[j].slot.hasSelection)
                continue;
            const auto [s, e] = selRangeOf(buffer_, refs[j].slot);
            const int c = buffer_.offsetOf(refs[i].slot.caret.line, refs[i].slot.caret.col);
            if (s < c && c < e) {
                if (refs[i].primary)
                    refs[j].dead = true;
                else
                    refs[i].dead = true;
                break;
            }
        }
    }

    // Rebuild: primary back into the classic fields, surviving extras
    // re-sorted into document order.
    for (const Ref& r : refs) {
        if (!r.primary)
            continue;
        caret_ = r.slot.caret;
        anchor_ = r.slot.anchor;
        hasSelection_ = r.slot.hasSelection;
    }
    QVector<CaretSlot> kept;
    for (const Ref& r : refs) {
        if (!r.primary && !r.dead)
            kept.append(r.slot);
    }
    std::stable_sort(kept.begin(), kept.end(),
                     [](const CaretSlot& a, const CaretSlot& b) { return posLess(a.caret, b.caret); });
    extraCarets_ = kept;
    altDragSlot_ = -1; // indices just moved; any in-flight drag anchor is moot
}

bool CodeEditor::applyCaretsFanOut(const SlotEditFn& editFn)
{
    normalizeCarets();

    struct Target {
        CaretSlot slot;
        bool primary;
        bool edited = false;
        SlotEdit ed;
        int finalOffset = -1; // caret after this slot's edit, in the frame
                              // where every edit to its RIGHT is already applied
    };
    QVector<Target> targets;
    targets.reserve(extraCarets_.size() + 1);
    for (const CaretSlot& s : extraCarets_)
        targets.append({s, false, false, {}, -1});
    targets.append({CaretSlot{caret_, anchor_, hasSelection_}, true, false, {}, -1});
    for (Target& t : targets)
        t.edited = editFn(t.slot, t.ed);

    // Apply right-to-left so not-yet-applied (leftward) offsets never shift.
    QVector<int> order;
    for (int i = 0; i < targets.size(); ++i) {
        if (targets[i].edited)
            order.append(i);
    }
    std::sort(order.begin(), order.end(), [&targets](int a, int b) {
        if (targets[a].ed.start != targets[b].ed.start)
            return targets[a].ed.start > targets[b].ed.start;
        return targets[a].ed.end > targets[b].ed.end;
    });
    struct Applied { int start; int end; };
    QVector<Applied> appliedRanges; // disjoint, to the RIGHT of the candidate
    QVector<int> applyOrder;
    for (int idx : order) {
        const SlotEdit& ed = targets[idx].ed;
        bool clash = false;
        for (const Applied& a : appliedRanges) {
            if (ed.end > a.start) { // leftward candidate running into it
                clash = true;
                break;
            }
        }
        if (clash) { // v1 divergence: VS Code merges/adjusts; we KEEP the
            targets[idx].edited = false; // right edit and drop this leftward
            continue;                    // candidate's colliding edit
        }
        applyOrder.append(idx);
        appliedRanges.append({ed.start, ed.end});
    }
    if (applyOrder.isEmpty())
        return false; // nothing to record: NOT even an empty bracket

    // Unedited slots still have a caret that must ride every edit's
    // remap, so seed them with their pre-action offset.
    for (Target& t : targets) {
        if (!t.edited)
            t.finalOffset = buffer_.offsetOf(t.slot.caret.line, t.slot.caret.col);
    }

    buffer_.beginEditGroup();
    for (int idx : applyOrder) {
        Target& t = targets[idx];
        const int delta = t.ed.text.size() - (t.ed.end - t.ed.start);
        // Slots whose own edit is still pending never shift (their final
        // offsets sit left of this edit); everyone else follows the rule.
        for (Target& o : targets) {
            if (o.edited && o.finalOffset < 0)
                continue; // pending: its finalOffset is frame-independent
            int& p = o.finalOffset;
            if (t.ed.end <= p)
                p += delta;
            else if (t.ed.start < p && p < t.ed.end)
                p = t.ed.start;
        }
        // Self first: our own final caret is start + text in this frame.
        t.finalOffset = t.ed.start + t.ed.text.size();
        if (t.ed.end > t.ed.start)
            buffer_.removeRange(t.ed.start, t.ed.end);
        if (!t.ed.text.isEmpty())
            buffer_.insertAtOffset(t.ed.start, t.ed.text);
    }
    buffer_.endEditGroup();

    // Commit the caret state. EVERY slot lands where its tracked offset
    // says: edited slots own theirs, unedited (and clash-dropped) ones
    // rode the remap from their pre-action caret.
    for (Target& t : targets) {
        const auto lc = buffer_.lineColAt(t.finalOffset);
        t.slot.caret = clampPos(TextPosition{lc.line, lc.col});
        t.slot.anchor = t.slot.caret;
        t.slot.hasSelection = false;
    }
    for (Target& t : targets) {
        if (t.primary) {
            caret_ = t.slot.caret;
            anchor_ = t.slot.anchor;
            hasSelection_ = t.slot.hasSelection;
        }
    }
    QVector<CaretSlot> kept;
    for (const Target& t : targets) {
        if (!t.primary)
            kept.append(t.slot);
    }
    std::stable_sort(kept.begin(), kept.end(),
                     [](const CaretSlot& a, const CaretSlot& b) { return posLess(a.caret, b.caret); });
    extraCarets_ = kept;
    normalizeCarets(); // edits can stack carets onto one point: fold

    caretOn_ = true;
    if (hasFocus())
        blinkTimer_.start();
    ensureCaretVisible();
    updateScrollBarRange();
    update();
    emit cursorMoved(caret_);
    emit selectionChanged();
    return true;
}

void CodeEditor::moveCarets(const std::function<TextPosition(const CaretSlot&)>& targetFn,
                            bool extend)
{
    breakUndoChain(); // deliberate motion freezes any edit chain

    struct Ref {
        CaretSlot slot;
        bool primary;
    };
    QVector<Ref> refs;
    refs.reserve(extraCarets_.size() + 1);
    for (const CaretSlot& s : extraCarets_)
        refs.append({s, false});
    refs.append({CaretSlot{caret_, anchor_, hasSelection_}, true});
    for (Ref& r : refs) {
        const TextPosition target = clampPos(targetFn(r.slot));
        if (extend) {
            if (!r.slot.hasSelection) {
                r.slot.anchor = r.slot.caret; // the walk starts at the caret
                r.slot.hasSelection = true;
            }
            r.slot.caret = target;
            if (r.slot.caret == r.slot.anchor)
                r.slot.hasSelection = false; // a zero-length extend is no selection
        } else {
            r.slot.caret = target;
            r.slot.anchor = target;
            r.slot.hasSelection = false;
        }
    }
    for (const Ref& r : refs) {
        if (!r.primary)
            continue;
        caret_ = r.slot.caret;
        anchor_ = r.slot.anchor;
        hasSelection_ = r.slot.hasSelection;
    }
    QVector<CaretSlot> kept;
    for (const Ref& r : refs) {
        if (!r.primary)
            kept.append(r.slot);
    }
    std::stable_sort(kept.begin(), kept.end(),
                     [](const CaretSlot& a, const CaretSlot& b) { return posLess(a.caret, b.caret); });
    extraCarets_ = kept;
    normalizeCarets(); // stacked/duplicated targets fold like VS Code

    caretOn_ = true;
    if (hasFocus())
        blinkTimer_.start();
    ensureCaretVisible();
    update();
    emit cursorMoved(caret_);
    emit selectionChanged();
    updatePrimarySelection();
}

void CodeEditor::undo()
{
    if (!buffer_.canUndo() || undoMarks_.isEmpty())
        return; // out of sync would be a bug; refuse rather than corrupt
    const UndoMark current = captureEditMark(); // what redo must restore
    const UndoMark pre = undoMarks_.takeLast();
    buffer_.undo();
    redoMarks_.append(current);
    lastPushSerial_ = buffer_.undoPushSerial(); // a move does not bump it
    applyEditMark(pre);
}

void CodeEditor::redo()
{
    if (!buffer_.canRedo() || redoMarks_.isEmpty())
        return;
    const UndoMark current = captureEditMark(); // the group's pre-state again
    const UndoMark after = redoMarks_.takeLast(); // state the undo tore down
    buffer_.redo();
    undoMarks_.append(current);
    lastPushSerial_ = buffer_.undoPushSerial();
    applyEditMark(after);
}

void CodeEditor::updatePrimarySelection()
{
    QClipboard* cb = QGuiApplication::clipboard();
    // Wayland and the offscreen platform have no primary selection;
    // supportsSelection() is the gate (tests must gate on it the same way).
    if (!cb || !cb->supportsSelection())
        return;
    if (!extraCarets_.isEmpty()) {
        // Multi-caret (D1): the primary selection mirrors ALL slot
        // selections joined by newlines, mirroring the copy() rule.
        QStringList parts;
        for (const CaretSlot& s : allCarets()) {
            if (!s.hasSelection)
                continue;
            const auto [st, en] = selRangeOf(buffer_, s);
            parts << buffer_.textRange(st, en);
        }
        cb->setText(parts.join(QLatin1Char('\n')), QClipboard::Selection);
        return;
    }
    if (hasSelection_)
        cb->setText(selectedText(), QClipboard::Selection);
    else
        cb->setText(QString(), QClipboard::Selection); // release ownership
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
    const UndoMark pre = captureEditMark();
    if (!extraCarets_.isEmpty()) {
        // Multi-caret typing (D1): one bracketed group per key press, so a
        // multi-caret action is exactly ONE undo step and deliberately does
        // NOT coalesce across presses (the bracket fences every group).
        applyCaretsFanOut([this, &text](const CaretSlot& s, SlotEdit& ed) {
            if (s.hasSelection) {
                const auto [st, en] = selRangeOf(buffer_, s);
                ed.start = st;
                ed.end = en;
            } else {
                ed.start = buffer_.offsetOf(s.caret.line, s.caret.col);
                ed.end = ed.start;
            }
            ed.text = text;
            return true;
        });
        reconcileUndoMarks(pre);
        return;
    }
    // Typing over a selection: the delete + insert is ONE compound undo
    // step; the bracket also fences the pair off from neighboring chains.
    // Without a selection the insert stays unbracketed so consecutive
    // typing coalesces in the core (see core/TextBuffer.hpp).
    const bool bracket = hasSelection_;
    if (bracket)
        buffer_.beginEditGroup();
    deleteSelection(); // typing replaces the selection (Monaco semantics)
    const int offset = buffer_.offsetOf(caret_.line, caret_.col);
    buffer_.insertAtOffset(offset, text);
    if (bracket)
        buffer_.endEditGroup();
    TextPosition pos = caret_;
    const int newlines = text.count(QLatin1Char('\n'));
    if (newlines > 0) {
        pos.line += newlines;
        pos.col = static_cast<int>(text.size()) - text.lastIndexOf(QLatin1Char('\n')) - 1;
    } else {
        pos.col += static_cast<int>(text.size());
    }
    setCaret(clampPos(pos));
    reconcileUndoMarks(pre);
}

void CodeEditor::backspace()
{
    const UndoMark pre = captureEditMark(); // unbracketed: backspace runs
    if (!extraCarets_.isEmpty()) {         // coalesce into one chunk
        // Multi-caret (D1): per-slot grapheme-safe deletes, one bracketed
        // group per press (see insertTextAtCaret for the undo rationale).
        applyCaretsFanOut([this](const CaretSlot& s, SlotEdit& ed) {
            if (s.hasSelection) {
                const auto [st, en] = selRangeOf(buffer_, s);
                ed.start = st;
                ed.end = en;
                return true;
            }
            if (s.caret.col > 0) {
                const QString line = buffer_.lineText(s.caret.line);
                const int start = prevGraphemeCol(line, s.caret.col);
                const int end = isGraphemeBoundary(line, s.caret.col)
                    ? s.caret.col : nextGraphemeCol(line, s.caret.col);
                ed.start = buffer_.offsetOf(s.caret.line, start);
                ed.end = buffer_.offsetOf(s.caret.line, end);
                return true;
            }
            if (s.caret.line > 0) {
                const int nl = buffer_.offsetOfLine(s.caret.line) - 1;
                ed.start = nl;
                ed.end = nl + 1;
                return true;
            }
            return false; // at the document start: this slot edits nothing
        });
        reconcileUndoMarks(pre);
        return;
    }
    if (!deleteSelection()) {
        if (caret_.col > 0) {
            // Grapheme-cluster backspace (M2): deletes exactly the cluster
            // before the caret. When the caret sits inside a cluster (possible
            // after a programmatic setPlainText split one, or a mid-cluster
            // mouse click) the WHOLE cluster goes, so orphan surrogate halves
            // or dangling combining marks never accumulate.
            const QString line = buffer_.lineText(caret_.line);
            const int start = prevGraphemeCol(line, caret_.col);
            const int end = isGraphemeBoundary(line, caret_.col)
                ? caret_.col : nextGraphemeCol(line, caret_.col);
            buffer_.removeRange(buffer_.offsetOf(caret_.line, start),
                                buffer_.offsetOf(caret_.line, end));
            setCaret({caret_.line, start});
        } else if (caret_.line > 0) {
            const int prevLen = lineLength(caret_.line - 1);
            const int nlPos = buffer_.offsetOfLine(caret_.line) - 1;
            buffer_.removeRange(nlPos, nlPos + 1);
            setCaret({caret_.line - 1, prevLen});
        }
    }
    reconcileUndoMarks(pre);
}

void CodeEditor::deleteForward()
{
    const UndoMark pre = captureEditMark(); // unbracketed, see backspace
    if (!extraCarets_.isEmpty()) {
        applyCaretsFanOut([this](const CaretSlot& s, SlotEdit& ed) {
            if (s.hasSelection) {
                const auto [st, en] = selRangeOf(buffer_, s);
                ed.start = st;
                ed.end = en;
                return true;
            }
            const int len = lineLength(s.caret.line);
            if (s.caret.col < len) {
                const QString line = buffer_.lineText(s.caret.line);
                const int start = isGraphemeBoundary(line, s.caret.col)
                    ? s.caret.col : prevGraphemeCol(line, s.caret.col);
                const int end = nextGraphemeCol(line, s.caret.col);
                ed.start = buffer_.offsetOf(s.caret.line, start);
                ed.end = buffer_.offsetOf(s.caret.line, end);
                return true;
            }
            if (s.caret.line + 1 < buffer_.lineCount()) {
                const int nl = buffer_.offsetOfLine(s.caret.line + 1) - 1;
                ed.start = nl;
                ed.end = nl + 1;
                return true;
            }
            return false; // at the very document end: this slot edits nothing
        });
        reconcileUndoMarks(pre);
        return;
    }
    if (!deleteSelection()) {
        const int len = lineLength(caret_.line);
        if (caret_.col < len) {
            // Grapheme-cluster delete (M2); mirrors backspace's no-orphan rule,
            // snapping the caret back when the delete started inside a cluster.
            const QString line = buffer_.lineText(caret_.line);
            const int start = isGraphemeBoundary(line, caret_.col)
                ? caret_.col : prevGraphemeCol(line, caret_.col);
            const int end = nextGraphemeCol(line, caret_.col);
            buffer_.removeRange(buffer_.offsetOf(caret_.line, start),
                                buffer_.offsetOf(caret_.line, end));
            setCaret({caret_.line, start});
        } else if (caret_.line + 1 < buffer_.lineCount()) {
            const int nlPos = buffer_.offsetOfLine(caret_.line + 1) - 1;
            buffer_.removeRange(nlPos, nlPos + 1);
            setCaret(caret_);
        }
    }
    reconcileUndoMarks(pre);
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
    const QFontMetricsF fmF(font_); // preedit advance measurement

    // Selection pass first: paint the background under the glyphs.
    // Semi-transparent highlight so text stays legible on light and dark
    // palettes alike (Monaco uses the same trick with ~40% opacity).
    // D1: every caret slot's selection is painted, not just the primary.
    QColor selColor = palette().color(QPalette::Highlight);
    selColor.setAlpha(90);
    for (const CaretSlot& slot : allCarets()) {
        if (!slot.hasSelection)
            continue;
        const bool anchorFirst = posLess(slot.anchor, slot.caret);
        const TextPosition s = anchorFirst ? slot.anchor : slot.caret;
        const TextPosition e = anchorFirst ? slot.caret : slot.anchor;
        for (int line = std::max(first, s.line); line <= std::min(last, e.line); ++line) {
            const int startCol = (line == s.line) ? s.col : 0;
            // On fully covered lines the selection extends one phantom cell
            // past the text to visualize the selected newline; on the end
            // line it stops at the selection end.
            const int textLen = lineLength(line);
            const int endCol = (line == e.line) ? e.col : std::max(textLen, 1);
            const GeoSlot* gs = hitLine(line);
            const int x0 = padding_ + qRound(xInLine(gs, startCol));
            const int x1 = padding_ + qRound(xInLine(gs, endCol));
            if (x1 > x0) {
                const int yTop = padding_ + line * lh - vScroll_;
                p.fillRect(QRect(x0, yTop, x1 - x0, lh), selColor);
            }
        }
    }

    for (int line = first; line <= last; ++line) {
        const int yTop = padding_ + line * lh - vScroll_;
        const GeoSlot* gs = hitLine(line);

        if (line == caret_.line && caretOn_) {
            // The caret follows the very cluster advances the line below is
            // drawn with (same QTextLine), so emoji ZWJ sequences no longer
            // push the caret away from the glyphs. During composition the
            // visual caret trails the preedit (which is not in the buffer).
            double cx = padding_ + xInLine(gs, caret_.col);
            if (!preedit_.isEmpty())
                cx += fmF.horizontalAdvance(preedit_);
            p.fillRect(QRect(qRound(cx), yTop, 2, lh), palette().color(QPalette::Text));
        }
        // D1: additional carets blink in sync with the primary (one shared
        // blink timer), drawn with the same 2px rule off the same layout.
        for (const CaretSlot& slot : extraCarets_) {
            if (slot.caret.line != line || !caretOn_)
                continue;
            const double cx = padding_ + xInLine(gs, slot.caret.col);
            p.fillRect(QRect(qRound(cx), yTop, 2, lh), palette().color(QPalette::Text));
        }
        if (gs->valid) {
            // Draw with the cached QTextLine itself, not a fresh drawText:
            // glyphs, caret and hit-testing then share one layout.
            // MEASURED (numeric band scan, raster backing store = the only
            // engine our widgets use): draw's pos is the layout TOP-LEFT,
            // not a baseline anchor - the engine places glyphs at
            // pos.y + line.y() + line.ascent() on its own (first line has
            // y()==0). Passing baseline=yTop+ascent_ here drew every line
            // ~one line-height too low, an empty band at the top and a
            // caret visibly a line above its text.
            gs->line.draw(&p, QPointF(padding_, yTop));
        }
        if (line == caret_.line && !preedit_.isEmpty()) {
            // Composition text at the caret with a dotted underline (Qt's
            // own line-edit convention) to show it is not committed yet.
            // drawText below IS baseline-anchored, so align it with the
            // layout engine's own baseline for the line above.
            const double px = padding_ + xInLine(gs, caret_.col);
            const double textBaseline = yTop + (gs->valid ? gs->line.ascent() : ascent_);
            p.drawText(QPointF(px, textBaseline), preedit_);
            p.setPen(QPen(palette().color(QPalette::Text), 0, Qt::DotLine));
            const double pw = fmF.horizontalAdvance(preedit_);
            p.drawLine(QPointF(px, yTop + lh - 2), QPointF(px + pw, yTop + lh - 2));
            p.setPen(palette().color(QPalette::Text));
        }
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

    // Clipboard shortcuts go through QKeySequence matching so the full
    // standard key chains are covered (Ctrl+C / Ctrl+Ins / Cmd+C for copy,
    // Shift+Del for cut on X11, Shift+Ins for paste…). This must run before
    // the modifier catch-all below, which would otherwise swallow Ctrl+C.
    // matches() checks the modifier set exactly, so Ctrl+Alt+C stays free.
    if (event->matches(QKeySequence::Copy)) {
        copy();
        finish();
        return;
    }
    if (event->matches(QKeySequence::Cut)) {
        cut();
        finish();
        return;
    }
    if (event->matches(QKeySequence::Paste)) {
        paste();
        finish();
        return;
    }
    // M3 undo/redo shortcuts. QKeySequence matching covers Ctrl+Z and
    // Ctrl+Shift+Z (measured on Qt 6.12 generic defaults; Redo there does
    // NOT contain Ctrl+Y). Every mainstream editor binds Ctrl+Y too and
    // our Ctrl switch ignores it otherwise, so wire it as an explicit
    // fallback. The Undo match runs first, so even on platform styles that
    // map Undo to Ctrl+Y this fallback can never steal it.
    if (event->matches(QKeySequence::Undo)) {
        undo();
        finish();
        return;
    }
    if (event->matches(QKeySequence::Redo) ||
        (key == Qt::Key_Y && ctrl == Qt::ControlModifier)) {
        redo();
        finish();
        return;
    }

    // Any deliberate caret motion freezes the edit chain in the core, so
    // click + type is its own undo step even when the offsets touch; wrap
    // all pure-navigation moves (edit actions break chains themselves via
    // their brackets or the delete/insert kind switch).
    auto nav = [this](TextPosition target, bool extend) {
        breakUndoChain();
        moveCaret(target, extend);
    };

    if (ctrl != Qt::NoModifier) {
        switch (key) {
        case Qt::Key_Home:
            if (!extraCarets_.isEmpty()) {
                moveCarets([](const CaretSlot&) { return TextPosition{0, 0}; }, shift);
                finish();
                return;
            }
            nav({0, 0}, shift);
            finish();
            return;
        case Qt::Key_End: {
            const int lastLine = buffer_.lineCount() - 1;
            if (!extraCarets_.isEmpty()) {
                moveCarets(
                    [this, lastLine](const CaretSlot&) {
                        return TextPosition{lastLine, lineLength(lastLine)};
                    },
                    shift);
                finish();
                return;
            }
            nav({lastLine, lineLength(lastLine)}, shift);
            finish();
            return;
        }
        // M2 word navigation: one press per class run, targets already
        // snapped to grapheme boundaries by the helpers.
        case Qt::Key_Left: {
            if (!extraCarets_.isEmpty()) {
                moveCarets(
                    [this](const CaretSlot& s) {
                        const int target = wordLeftCol(buffer_.lineText(s.caret.line), s.caret.col);
                        if (target < s.caret.col)
                            return TextPosition{s.caret.line, target};
                        if (s.caret.line > 0)
                            return TextPosition{s.caret.line - 1, lineLength(s.caret.line - 1)};
                        return s.caret; // boundary: this caret stays put
                    },
                    shift);
                finish();
                return;
            }
            const int target = wordLeftCol(buffer_.lineText(caret_.line), caret_.col);
            if (target < caret_.col)
                nav({caret_.line, target}, shift);
            else if (caret_.line > 0)
                nav({caret_.line - 1, lineLength(caret_.line - 1)}, shift);
            finish();
            return;
        }
        case Qt::Key_Right: {
            if (!extraCarets_.isEmpty()) {
                moveCarets(
                    [this](const CaretSlot& s) {
                        const int target = wordRightCol(buffer_.lineText(s.caret.line), s.caret.col);
                        if (target > s.caret.col)
                            return TextPosition{s.caret.line, target};
                        if (s.caret.line + 1 < buffer_.lineCount())
                            return TextPosition{s.caret.line + 1, 0};
                        return s.caret; // boundary: this caret stays put
                    },
                    shift);
                finish();
                return;
            }
            const int target = wordRightCol(buffer_.lineText(caret_.line), caret_.col);
            if (target > caret_.col)
                nav({caret_.line, target}, shift);
            else if (caret_.line + 1 < buffer_.lineCount())
                nav({caret_.line + 1, 0}, shift);
            finish();
            return;
        }
        case Qt::Key_Delete: {
            if (!extraCarets_.isEmpty()) {
                // Multi-caret word delete (D1): bracketed group, one step.
                const UndoMark pre = captureEditMark();
                applyCaretsFanOut([this](const CaretSlot& s, SlotEdit& ed) {
                    if (s.hasSelection) { // selections win, as with Delete
                        const auto [st, en] = selRangeOf(buffer_, s);
                        ed.start = st;
                        ed.end = en;
                        return true;
                    }
                    const int target = wordRightCol(buffer_.lineText(s.caret.line), s.caret.col);
                    if (target > s.caret.col) {
                        ed.start = buffer_.offsetOf(s.caret.line, s.caret.col);
                        ed.end = buffer_.offsetOf(s.caret.line, target);
                        return true;
                    }
                    if (s.caret.line + 1 < buffer_.lineCount()) {
                        const int nl = buffer_.offsetOfLine(s.caret.line + 1) - 1; // join next line
                        ed.start = nl;
                        ed.end = nl + 1;
                        return true;
                    }
                    return false;
                });
                reconcileUndoMarks(pre);
                finish();
                return;
            }
            const UndoMark pre = captureEditMark(); // unbracketed word delete
            if (deleteSelection()) { // selection takes precedence, as with Delete
                reconcileUndoMarks(pre);
                finish();
                return;
            }
            const int target = wordRightCol(buffer_.lineText(caret_.line), caret_.col);
            if (target > caret_.col) {
                const int start = buffer_.offsetOf(caret_.line, caret_.col);
                const int end = buffer_.offsetOf(caret_.line, target);
                buffer_.removeRange(start, end);
                setCaret(caret_); // caret stays put; force a repaint
            } else {
                deleteForward(); // at EOL: join the next line
            }
            reconcileUndoMarks(pre);
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
        if (!extraCarets_.isEmpty()) {
            moveCarets(
                [this](const CaretSlot& s) {
                    TextPosition p = s.caret;
                    if (p.col > 0)
                        p.col = prevGraphemeCol(buffer_.lineText(p.line), p.col);
                    else if (p.line > 0) {
                        --p.line;
                        p.col = lineLength(p.line);
                    }
                    return p;
                },
                shift);
            finish();
            return;
        }
        TextPosition pos = caret_;
        if (pos.col > 0)
            pos.col = prevGraphemeCol(buffer_.lineText(caret_.line), pos.col);
        else if (pos.line > 0) {
            --pos.line;
            pos.col = lineLength(pos.line);
        }
        nav(pos, shift);
        finish();
        return;
    }
    case Qt::Key_Right: {
        if (!extraCarets_.isEmpty()) {
            moveCarets(
                [this](const CaretSlot& s) {
                    TextPosition p = s.caret;
                    if (p.col < lineLength(p.line))
                        p.col = nextGraphemeCol(buffer_.lineText(p.line), p.col);
                    else if (p.line + 1 < buffer_.lineCount()) {
                        ++p.line;
                        p.col = 0;
                    }
                    return p;
                },
                shift);
            finish();
            return;
        }
        TextPosition pos = caret_;
        if (pos.col < lineLength(pos.line))
            pos.col = nextGraphemeCol(buffer_.lineText(caret_.line), pos.col);
        else if (pos.line + 1 < buffer_.lineCount()) {
            ++pos.line;
            pos.col = 0;
        }
        nav(pos, shift);
        finish();
        return;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        // Column is re-clamped to the target line; visual column memory
        // arrives with M9 (soft wrap work). On the first/last line the
        // motion collapses to the line start/end like real editors do.
        if (!extraCarets_.isEmpty()) {
            const bool up = key == Qt::Key_Up;
            moveCarets(
                [this, up](const CaretSlot& s) {
                    TextPosition p = s.caret;
                    if (up) {
                        if (p.line > 0)
                            --p.line;
                        else
                            p.col = 0;
                    } else {
                        if (p.line + 1 < buffer_.lineCount())
                            ++p.line;
                        else
                            p.col = lineLength(p.line);
                    }
                    return p;
                },
                shift);
            finish();
            return;
        }
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
        nav(pos, shift);
        finish();
        return;
    }
    case Qt::Key_Home:
        if (!extraCarets_.isEmpty()) {
            moveCarets(
                [this](const CaretSlot& s) { return TextPosition{s.caret.line, 0}; },
                shift);
            finish();
            return;
        }
        nav({caret_.line, 0}, shift);
        finish();
        return;
    case Qt::Key_End:
        if (!extraCarets_.isEmpty()) {
            moveCarets(
                [this](const CaretSlot& s) {
                    return TextPosition{s.caret.line, lineLength(s.caret.line)};
                },
                shift);
            finish();
            return;
        }
        nav({caret_.line, lineLength(caret_.line)}, shift);
        finish();
        return;
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        if (!extraCarets_.isEmpty()) {
            const int page = std::max(1, visibleLineCount() - 2);
            const int delta = key == Qt::Key_PageUp ? -page : page;
            moveCarets(
                [this, delta](const CaretSlot& s) {
                    TextPosition p = s.caret;
                    p.line += delta;
                    return p; // clampPos inside moveCarets pins the edges
                },
                shift);
            finish();
            return;
        }
        TextPosition pos = caret_;
        const int page = std::max(1, visibleLineCount() - 2);
        pos.line += (key == Qt::Key_PageUp) ? -page : page;
        nav(pos, shift);
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
        // Two-stage (D1): Escape drops the additional carets FIRST, the
        // primary selection second, matching multi-caret editor muscle memory.
        if (!extraCarets_.isEmpty()) {
            clearAdditionalCarets();
            finish();
            return;
        }
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

// --- IME composition (Phase C) ------------------------------------------

void CodeEditor::inputMethodEvent(QInputMethodEvent* event)
{
    // Deliberately minimal composition: the preedit is rendered at the caret
    // but never enters the model, and only the commit string reaches the
    // buffer. The replacement start/length the event carries address the
    // interior of our single preedit run (attributes, styling); since we do
    // not render those, they are unused here.
    if (!event->commitString().isEmpty())
        insertTextAtCaret(event->commitString()); // setCaret inside clears preedit_
    preedit_ = event->preeditString(); // assigned after the insert on purpose
    ensureCaretVisible(); // the preedit can overflow the viewport
    update();
}

QVariant CodeEditor::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return true; // StrongFocus widget: composition is always available
    case Qt::ImCursorRectangle: {
        // Candidate-window anchor: the visual caret, which during
        // composition trails the preedit instead of the buffer caret.
        double x = padding_ + xInLine(hitLine(caret_.line), caret_.col);
        if (!preedit_.isEmpty())
            x += QFontMetricsF(font_).horizontalAdvance(preedit_);
        const int y = padding_ + caret_.line * lineHeight_ - vScroll_;
        return QRect(qRound(x), y, 2, lineHeight_);
    }
    case Qt::ImCurrentSelection:
        return hasSelection_ ? selectedText() : QString();
    default:
        return QWidget::inputMethodQuery(query);
    }
}

void CodeEditor::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        // X11 paste gesture: place the caret at the click, then insert the
        // primary selection there. Platforms without a primary selection
        // (Wayland, offscreen) fall back to the regular clipboard, so the
        // middle-click button always does something predictable.
        setFocus(Qt::MouseFocusReason);
        breakUndoChain(); // the click move freezes any prior edit chain
        clearAdditionalCarets(); // a click targets ONE caret (D1)
        setCaret(positionAt(event->position().toPoint()));
        if (QClipboard* cb = QGuiApplication::clipboard()) {
            QString text = cb->text(QClipboard::Selection);
            if (text.isEmpty())
                text = cb->text(QClipboard::Clipboard);
            pasteText(text);
        }
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    setFocus(Qt::MouseFocusReason);
    breakUndoChain(); // a click is deliberate motion: freeze the edit chain
    const TextPosition pos = positionAt(event->position().toPoint());
    if (event->modifiers() & Qt::AltModifier) {
        // D1 multi-caret add gesture (left button only). Clicking exactly
        // on an existing caret starts an Alt+Drag that resizes THAT slot
        // (its anchor stays put); anywhere else adds a new caret, which
        // becomes the primary (the mouse owns position()) while the old
        // primary demotes into the extras. v1 has no Alt+click REMOVE
        // toggle; duplicates fold via normalizeCarets.
        altDragPrimary_ = false;
        altDragSlot_ = -1;
        if (pos == caret_) {
            altDragPrimary_ = true; // resize the primary slot on drag
            if (!hasSelection_)
                anchor_ = pos; // anchor pinned at the pressed caret
        } else {
            for (int i = 0; i < extraCarets_.size(); ++i) {
                if (extraCarets_[i].caret == pos) {
                    altDragSlot_ = i; // anchor is wherever that slot has it
                    break;
                }
            }
            if (altDragSlot_ < 0) {
                CaretSlot demoted{caret_, anchor_, hasSelection_};
                caret_ = pos;
                anchor_ = pos;
                hasSelection_ = false;
                extraCarets_.append(demoted);
                std::stable_sort(extraCarets_.begin(), extraCarets_.end(),
                                 [](const CaretSlot& a, const CaretSlot& b) { return posLess(a.caret, b.caret); });
                altDragPrimary_ = true; // dragging follows the newest click
                normalizeCarets(); // Alt+click inside a selection: the rules decide
                altDragPrimary_ = pos == caret_; // normalize may have dropped the add
                emit cursorMoved(caret_);
                emit selectionChanged();
            }
        }
        update();
        event->accept();
        return;
    }
    // A plain (or Shift+) left click collapses the caret set to ONE (D1).
    extraCarets_.clear();
    altDragSlot_ = -1;
    altDragPrimary_ = false;
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
    breakUndoChain(); // drag motion freezes the edit chain (idempotent)
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
    const TextPosition target = positionAt(pos);
    if (altDragPrimary_) {
        // Alt+Drag on the primary caret: resize its selection, the anchor
        // pinned at Alt+Press (a plain extend-drag from that caret end).
        caret_ = target;
        hasSelection_ = !(anchor_ == caret_);
        caretOn_ = true;
        if (hasFocus())
            blinkTimer_.start();
        update();
        emit cursorMoved(caret_);
        emit selectionChanged();
        updatePrimarySelection();
        event->accept();
        return;
    }
    if (altDragSlot_ >= 0 && altDragSlot_ < extraCarets_.size()) {
        CaretSlot& s = extraCarets_[altDragSlot_];
        if (!s.hasSelection) {
            s.anchor = s.caret; // the drag starts at the pressed caret
            s.hasSelection = true;
        }
        s.caret = target;
        if (s.caret == s.anchor)
            s.hasSelection = false;
        caretOn_ = true;
        update();
        emit selectionChanged();
        updatePrimarySelection();
        event->accept();
        return;
    }
    moveCaret(target, true);
    event->accept();
}

void CodeEditor::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && (altDragPrimary_ || altDragSlot_ >= 0)) {
        // End the Alt+Drag: the dragged selection may now overlap others,
        // so let the normalization rules settle the caret set.
        altDragPrimary_ = false;
        altDragSlot_ = -1;
        normalizeCarets();
        update();
        emit cursorMoved(caret_);
        emit selectionChanged();
        updatePrimarySelection();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
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

void CodeEditor::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    // The font is deliberately NOT part of the layout-cache key (lines are
    // re-fetched by text anyway); therefore a font change must wipe the
    // cache outright, and the derived metrics must be re-measured.
    if (event->type() == QEvent::FontChange && !(font_ == font())) {
        font_ = font();
        // Same no-hinting rule as the constructor: engine advances and cell
        // width must agree exactly, or the caret drifts along the line.
        font_.setHintingPreference(QFont::PreferNoHinting);
        const QFontMetricsF fm(font_);
        cellWidth_ = fm.horizontalAdvance(QLatin1Char('0'));
        if (!(cellWidth_ > 0.0))
            cellWidth_ = 8.0;
        ascent_ = qCeil(fm.ascent());
        lineHeight_ = qCeil(fm.ascent() + fm.descent() + fm.leading());
        if (lineHeight_ <= 0)
            lineHeight_ = ascent_ + 4;
        for (GeoSlot& s : geo_) { // reset in place: QTextLayout is uncopyable
            s.keyLine = -1;
            s.text.clear();
            s.layout.clearLayout();
            s.line = QTextLine();
            s.valid = false;
            s.stamp = 0;
        }
        updateScrollBarRange();
        ensureCaretVisible();
        update();
    }
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
