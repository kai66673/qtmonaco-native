#include "CodeEditor.hpp"

#include "view/HighlightEngine.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF> // M13 fold triangles
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringList>
#include <QTextBoundaryFinder>
#include <QVBoxLayout>
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

// M4: shared snap used by BOTH edges of a colored-run boundary. Adjacent
// spans snap the same interior boundary identically, so the colored
// fragments stay an exact partition (no gap, no double paint), and an
// edge never lands inside a surrogate pair or a combining sequence
// (QTextLine fragment painting splits clusters at the given indices,
// measured: a mid-cluster cut drops or double-draws the cluster's glyphs).
int snapToGraphemeForward(const QString& text, int col)
{
    const int n = static_cast<int>(text.size());
    if (col <= 0 || col >= n)
        return std::clamp(col, 0, n);
    if (isGraphemeBoundary(text, col))
        return col;
    return nextGraphemeCol(text, col);
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

    // M4: one core EditEvent per primitive mutation keeps the highlight
    // tree current (ts_tree_edit happens inside the engine; the reparse
    // itself waits for refreshIfNeeded() in paint, so a whole key press -
    // multi-caret fan-out included - reparses at most once).
    highlight_ = std::make_unique<HighlightEngine>();
    highlight_->attach(&buffer_);
    buffer_.setEditObserver([this](const qmn::core::TextBuffer::EditEvent& ev) {
        highlight_->onEdit(ev);
        findDirty_ = true; // M6: typing invalidates the match set lazily;
                           // the next paint (or find action) refreshes it
        // M13: keep collapsed-fold indices aligned with the model (a
        // best-effort line-index sweep; fold STATE is not undoable in v1).
        shiftFoldStateForEdit(ev);
    });

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

    // M9: the horizontal bar is bottom chrome; its range follows the
    // ambient doc-width extent (public docs on maxHorizontalScroll).
    hBar_ = new QScrollBar(Qt::Horizontal, this);
    hBar_->hide();
    connect(hBar_, &QScrollBar::valueChanged, this, [this](int value) {
        if (!hScrollbarSync_)
            setHScroll(value);
    });

    const int flash = qApp ? qApp->cursorFlashTime() : 1000;
    blinkTimer_.setInterval(flash > 0 ? flash / 2 : 500);
    connect(&blinkTimer_, &QTimer::timeout, this, [this] {
        caretOn_ = !caretOn_;
        update();
    });

    // M13: seed the fold-map caches before the first paint/edit (empty
    // runs: slot == model line, visibleLineTotal() == lineCount()).
    foldDocLines_ = buffer_.lineCount();
    rebuildHiddenRuns();
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
    hScroll_ = 0;       // M9: new doc, new ambient extent (hitLine reseeds it)
    docWidthPx_ = 0;
    collapsed_.clear(); // M13: fold state is per-document; a document
    foldables_.clear(); // replace drops every fold (the whole-document
    hiddenRuns_.clear(); // EditEvent the core emits would otherwise shift
    foldVisibleTotal_ = buffer_.lineCount(); // stale folds across the replace)
    foldDocLines_ = buffer_.lineCount();
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
    // (combining marks). Past the budget the xInLine/xToCursor math keeps
    // working on the phantom cellWidth extrapolation (exact on monospace —
    // M9 h-scroll relies on it), but NO glyphs are laid out or painted
    // there: on monster lines the far-right region carries caret/selection
    // geometry only (v1 trade, recorded in the CHANGELOG).
    return std::max(64, 4 * (static_cast<int>(width() / cellWidth_) + 2));
}

const CodeEditor::GeoSlot* CodeEditor::hitLine(int line) const
{
    if (line < 0 || line >= buffer_.lineCount())
        return nullptr;
    const QString full = buffer_.lineText(line);
    const int maxUnits = geoMaxUnits();
    const QString text = full.size() > maxUnits ? full.left(maxUnits) : full;

    // M9 ambient horizontal extent: whichever way a line enters the glyph
    // cache (paint, caret geometry, hit test) its phantom-aware px width
    // is now known and joins the doc extent — an O(1) read, never a
    // document-wide scan (public docs on maxHorizontalScroll).
    const auto noteWidth = [this](const GeoSlot* s, int fullLen) {
        const double w = xInLine(s, fullLen);
        if (w > docWidthPx_)
            docWidthPx_ = int(std::ceil(w));
    };

    for (GeoSlot& s : geo_) // geo_ is mutable: this is a const-by-API cache
        if (s.keyLine == line && s.text == text) {
            s.stamp = ++geoClock_;
            noteWidth(&s, full.size());
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
    noteWidth(victim, full.size());
    return victim;
}

// Line-space x (left of the content area) of caret position `col`, clamped
// to a phantom-cell extrapolation beyond the laid-out prefix (monster-line
// guard and the selected-newline cell both land there; the region is
// off-screen). Callers add contentLeft() for widget x (M5: that area now
// includes the gutter; it is padding_ + gutterWidth()).
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
    // M9: widget space rides the current horizontal scroll offset (this is
    // the x the user sees; off-screen targets go negative/past the edge,
    // matching the painted glyph — see public docs).
    return contentLeft() + xInLine(hitLine(line), c) - hScroll_;
}

TextPosition CodeEditor::positionAt(const QPoint& widgetPos) const
{
    TextPosition pos;
    // M13: rows are SLOTS now — the slot under the point maps to its model
    // line through the view-line map, so clicks never land on a hidden
    // line and clicks past the last visible row clamp to the last VISIBLE
    // line (fold-aware identity with the no-fold case: runs empty ⇒ slot ==
    // model line, formula bit-for-bit the old one).
    const int row = (widgetPos.y() - padding_ + vScroll_) / lineHeight_;
    pos.line = modelLineForVisibleIndex(row); // clamps to the visible range
    const GeoSlot* s = hitLine(pos.line);
    if (!s || !s->valid) {
        pos.col = 0; // empty line: every x is the line start
    } else {
        // xToCursor misbehaves far outside the laid-out extent (measured:
        // 1e9 maps back to 0), so clamp into the extent first: clicks past
        // the line end resolve to EOL, as they used to via the grid clamp.
        // M9: the click rides the h-scroll offset, and the clamp target is
        // the phantom-aware full-line width (xInLine), so clicks past the
        // layout budget keep mapping to EOL (monospace cell math up there).
        const int len = lineLength(pos.line);
        const double lineEndX = xInLine(s, len);
        const double x = std::clamp(widgetPos.x() - static_cast<double>(contentLeft())
                                        + hScroll_,
                                    0.0, lineEndX);
        if (x > s->line.naturalTextWidth()) {
            // Phantom region (past the layout budget): cellWidth cell math.
            pos.col = std::min(len, static_cast<int>(s->text.size())
                                      + int((x - s->line.naturalTextWidth()) / cellWidth_));
            return pos;
        }
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
        pos.col = std::clamp(c, 0, len);
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
    // M13: a caret that ended up hidden (an edit merged its line into a
    // collapsed fold's body) opens its fold first — the reveal choke-point
    // rule: the caret is NEVER addressed through a hidden line. Slot space
    // then carries the caret row (with no folds: slot == model line, the
    // pre-M13 formula exactly).
    if (visibleIndexOfModelLine(caret_.line) < 0)
        revealLine(caret_.line);
    const int caretTop = slotOfModelLine(caret_.line) * lineHeight_;
    const int caretBottom = caretTop + lineHeight_;
    int target = vScroll_;
    if (caretTop - padding_ < vScroll_)
        target = caretTop - padding_;
    else if (caretBottom + padding_ > vScroll_ + height())
        target = caretBottom + padding_ - height();
    setVScroll(target);

    // M9 horizontal half: the caret rides cluster advances (monospace:
    // col*cellWidth EXACTLY, which is what the exact-offset tests assert).
    // Flush-to-edge shifts only - the minimal scroll, no margin math.
    const double caretX = xInLine(hitLine(caret_.line), caret_.col); // line space
    const double win = horizontalViewportWidth();
    if (win > 0.0) {
        if (caretX < hScroll_) {
            setHScroll(int(std::floor(caretX))); // caret flush at the left edge
        } else if (caretX > hScroll_ + win - 1.0) {
            // the 2px caret cell fits at the right edge (past the ruler)
            setHScroll(int(std::ceil(caretX - win + 1.0)));
        }
    }
}

int CodeEditor::totalHeight() const
{
    // M13: the slot space — visibleLineTotal() already discounts every
    // hidden run, so the vBar range, the setVScroll clamp, the ruler
    // mapping and the paint window all shrink with a fold for free.
    int total = 2 * padding_ + visibleLineTotal() * lineHeight_;
    // M9 scroll-past-end (Monaco's scrollBeyondLastLine): the bonus lets
    // the LAST line reach the viewport top. One formula, so the vertical
    // bar, the clamp math and the ruler mapping all stay consistent.
    if (scrollBeyondLastLine_)
        total += std::max(0, height() - lineHeight_);
    return total;
}

int CodeEditor::firstVisibleLine() const
{
    // M13: the top SLOT's model line (no folds: identity with the old
    // clamp — hiddenRuns_ empty, visibleLineTotal() == lineCount()).
    return modelLineForVisibleIndex((vScroll_ - padding_) / lineHeight_);
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

// --- M9 horizontal scroll plumbing (one clamping writer, one contract
// formula; the public API docs carry the ambient-extent semantics) ---

void CodeEditor::setHorizontalScrollOffset(int pixels)
{
    setHScroll(pixels);
}

int CodeEditor::horizontalViewportWidth() const
{
    // The text window ends where the ruler chrome begins (the ruler is
    // painted last and overpaints the text's right edge, so text under it
    // is not "visible" in any sense that matters).
    return std::max(0, width() - contentLeft() - overviewRulerWidth());
}

int CodeEditor::maxHorizontalScroll() const
{
    return std::max(0, docWidthPx_ + padding_ - horizontalViewportWidth());
}

void CodeEditor::setHScroll(int pixels)
{
    const int maxScroll = maxHorizontalScroll();
    hScroll_ = std::clamp(pixels, 0, maxScroll);
    hScrollbarSync_ = true;
    if (hBar_) {
        // Sync the range here too: the ambient extent may have grown since
        // the last updateScrollBarRange (this is where the bar wakes up).
        hBar_->setRange(0, maxScroll);
        hBar_->setValue(hScroll_);
        hBar_->setVisible(maxScroll > 0);
    }
    hScrollbarSync_ = false;
    update();
}

void CodeEditor::setScrollBeyondLastLineEnabled(bool enabled)
{
    if (enabled == scrollBeyondLastLine_)
        return;
    scrollBeyondLastLine_ = enabled;
    updateScrollBarRange(); // totalHeight moved; the vBar range and the
                            // clamp below re-tune vScroll_ (ruler mapping
                            // follows the same total, by contract)
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

    if (hBar_) {
        const int maxH = maxHorizontalScroll();
        hBar_->setRange(0, maxH);
        hBar_->setPageStep(std::max(1, horizontalViewportWidth()));
        hBar_->setSingleStep(3 * std::max(1, int(std::ceil(cellWidth_))));
        hBar_->setVisible(maxH > 0);
        const int hBarH = hBar_->isVisible() ? hBar_->sizeHint().height() : 0;
        // The bar stops at the ruler chrome (the ruler spans full height and
        // the vBar lives inside its right edge, VS Code-style).
        const int rightExcl = std::max(overviewRulerWidth(), vBar_->isVisible() ? barWidth : 0);
        hBar_->setGeometry(0, height() - hBarH, std::max(0, width() - rightExcl), hBarH);
        if (hScroll_ > maxH)
            setHScroll(maxH);
    }
}

// --- M7 overview ruler: pure viewport-strip mapping (numeric contract,
// PLAN E1 DoD "ruler geometry state tests"). The strip maps the document
// pixel space [0, totalHeight()) linearly onto [0, height()); every
// mapping goes through that one ratio so the click gesture, the viewport
// rectangle and the tests never disagree.

void CodeEditor::setOverviewRulerEnabled(bool enabled)
{
    if (enabled == rulerEnabled_)
        return;
    rulerEnabled_ = enabled;
    // M9 cross-link: the ruler width IS the h-window's right boundary, so
    // toggling it changes both the clamp contract and the hBar geometry.
    updateScrollBarRange();
    update();
}

QRect CodeEditor::overviewRulerGeometry() const
{
    if (!rulerEnabled_)
        return {};
    return QRect(width() - kOverviewRulerWidth, 0, kOverviewRulerWidth, height());
}

int CodeEditor::rulerLineAt(int y) const
{
    if (!rulerEnabled_)
        return -1;
    const int total = std::max(1, totalHeight());
    const int h = std::max(1, height());
    const int docPx = int(qint64(std::clamp(y, 0, h - 1)) * total / h);
    // M13: totalHeight IS the slot space, so this pixel row is a slot row;
    // the map turns it into a model line (hidden runs are simply not
    // addressable from the strip — same rule as the click gesture).
    return modelLineForVisibleIndex((docPx - padding_) / lineHeight_);
}

QRect CodeEditor::overviewRulerViewportRect() const
{
    if (!rulerEnabled_)
        return {};
    const int total = std::max(1, totalHeight());
    const int h = std::max(1, height());
    const int y0 = int(qint64(vScroll_) * h / total);
    const int y1 = int(qint64(std::min(total, vScroll_ + height())) * h / total);
    return QRect(width() - kOverviewRulerWidth, y0, kOverviewRulerWidth, std::max(1, y1 - y0));
}

// Center the line under the strip point in the viewport (Monaco minimap
// click gesture). setVScroll does the clamping and the repaint.
void CodeEditor::rulerScrollTo(int y)
{
    const int line = rulerLineAt(y);
    if (line < 0)
        return;
    // M13: a fold click in the strip lands on a VISIBLE line by map
    // construction; centering addresses it in slot space (its own scroll
    // never lands a needed caret on a hidden row).
    setVScroll(padding_ + slotOfModelLine(line) * lineHeight_ + lineHeight_ / 2
               - height() / 2);
}

// --- M13 folding (Phase E2) -----------------------------------------------
// One view-line map (slots) is the single source of truth for vertical
// geometry: paint, caret navigation, hit-testing, scroll clamps, the ruler
// and the gutter all route through hiddenRuns_, so model and screen space
// cannot drift apart across a fold boundary. Semantics: a collapsed fold
// {start, end} hides (start, end] — the header never disappears — and
// rebuildHiddenRuns() MERGES touching/overlapping runs, which makes
// "slot+1" a guaranteed-visible walk (the caret jumps over a whole fold
// with zero extra logic in the nav code).

int CodeEditor::visibleLineTotal() const
{
    return foldVisibleTotal_;
}

int CodeEditor::visibleIndexOfModelLine(int line) const
{
    const int docLines = buffer_.lineCount();
    if (line < 0 || line >= docLines)
        return -1;
    // Last run starting at or before `line`: if it also ends at/after it,
    // the line is hidden; otherwise hiddenBefore + (len past that run) is
    // the number of hidden lines strictly before it (runs are disjoint and
    // sorted, and each caches its own hiddenBefore prefix).
    auto it = std::upper_bound(hiddenRuns_.begin(), hiddenRuns_.end(), line,
                               [](int l, const HiddenRun& r) { return l < r.first; });
    if (it != hiddenRuns_.begin()) {
        --it;
        if (line <= it->last)
            return -1; // inside a hidden run
        return line - (it->hiddenBefore + (it->last - it->first + 1));
    }
    return line; // before the first run: no hidden lines above
}

int CodeEditor::modelLineForVisibleIndex(int visibleIndex) const
{
    const int vt = visibleLineTotal();
    if (vt <= 0)
        return 0; // only reachable on an inconsistent cache; keep it total
    visibleIndex = std::clamp(visibleIndex, 0, vt - 1);
    // gapBefore(run) = the visible index the run's first line WOULD have
    // had unhidden; the last run with gapBefore <= index is the (merged)
    // run the index jumps over, and adding its full hidden-through count
    // lands on a visible line (or the walk never needed a jump at all).
    auto it = std::upper_bound(hiddenRuns_.begin(), hiddenRuns_.end(), visibleIndex,
                               [](int v, const HiddenRun& r) {
                                   return v < r.first - r.hiddenBefore;
                               });
    if (it != hiddenRuns_.begin()) {
        --it;
        visibleIndex += it->hiddenBefore + (it->last - it->first + 1);
    }
    return std::clamp(visibleIndex, 0, buffer_.lineCount() - 1);
}

void CodeEditor::rebuildHiddenRuns()
{
    const int docLines = std::max(1, buffer_.lineCount());
    std::sort(collapsed_.begin(), collapsed_.end(), [](const FoldState& a, const FoldState& b) {
        if (a.start != b.start)
            return a.start < b.start;
        return a.end > b.end;
    });
    hiddenRuns_.clear();
    int hidden = 0;
    for (const FoldState& f : collapsed_) {
        const int first = f.start + 1;
        if (first >= docLines)
            continue; // empty body after edits/clamping: drop the run
        const int last = std::min(f.end, docLines - 1);
        if (last < first)
            continue;
        if (!hiddenRuns_.isEmpty() && first <= hiddenRuns_.back().last + 1) {
            // touching or overlapping: merge, so runs stay disjoint AND
            // non-adjacent — the invariant that makes nextVisible land on
            // a visible line (or past the document) by construction
            HiddenRun& r = hiddenRuns_.last();
            if (last > r.last) {
                hidden += last - r.last;
                r.last = last;
            }
            continue;
        }
        hiddenRuns_.push_back(HiddenRun{first, last, hidden});
        hidden += last - first + 1;
    }
    foldVisibleTotal_ = docLines - hidden; // runs never cover line 0, so >= 1
}

void CodeEditor::shiftFoldStateForEdit(const qmn::core::TextBuffer::EditEvent& ev)
{
    // The observer fires POST-mutation, but EditEvent offsets are in the
    // PRE-op frame: ev.start itself survives every edit path (inserts land
    // AT it, deletes remove what follows it), so its post-op line IS its
    // pre-op line — the one anchor both frames agree on. EditEvent carries
    // no deleted text, so the line delta of this single op comes from the
    // cached doc lineCount: delta = newlines added (net).
    const int docLines = buffer_.lineCount();
    if (collapsed_.isEmpty()) {
        foldDocLines_ = docLines;
        return;
    }
    const int delta = docLines - foldDocLines_;
    foldDocLines_ = docLines;
    const int anchorLine = buffer_.lineColAt(ev.start).line;
    QVector<FoldState> kept;
    kept.reserve(collapsed_.size());
    bool dirty = false;
    for (FoldState& f : collapsed_) {
        if (f.start > anchorLine) {
            f.start += delta; // entirely below the edit: whole fold shifts
            f.end += delta;
        } else if (f.end > anchorLine) {
            f.end += delta; // contains (or ends below) the edit line
        }
        if (f.end <= f.start || f.start < 0 || f.start >= docLines)
            dirty = true; // body deleted / fold slid off the end: it dies
        else
            kept.push_back(f);
    }
    if (dirty || kept.size() != collapsed_.size() || delta != 0) {
        collapsed_ = std::move(kept);
        rebuildHiddenRuns(); // clamp ends into the new doc shape too
    }
}

bool CodeEditor::indentFoldsActive() const
{
    // The indent heuristic is the fallback source exactly where PLAN E2
    // put it: grammar-less modes (language() empty — plain/unknown), or a
    // loaded language whose composed fold query could not be built (then
    // the engine contributes nothing and folding needs some source). With
    // a working fold grammar (js/json/python) the scan goes silent:
    // leading whitespace alone must not fake python-style nesting there
    // (owner-reported M13 regression — see grammarFoldsFollowBracketsNotIndent).
    return highlight_->language().isEmpty() || !highlight_->hasFoldQuery();
}

int CodeEditor::indentOfModelLine(int line) const
{
    if (line < 0 || line >= buffer_.lineCount())
        return -1;
    // Read only a leading window: paint calls this per VISIBLE line, and a
    // 1M-unit line must not pay a full materialization (same O(view) rule
    // as the geo cache). A leading whitespace run deeper than the window
    // (pathological code) clamps at the window size — an accepted v1
    // approximation for the indent fold fallback.
    const int off = buffer_.offsetOfLine(line);
    const int winEnd = std::min(off + kIndentWindow, buffer_.charCount());
    const QString s = buffer_.textRange(off, winEnd);
    int i = 0;
    while (i < s.size()
           && (s.at(i) == QLatin1Char(' ') || s.at(i) == QLatin1Char('\t')))
        ++i;
    if (i < s.size()) {
        if (s.at(i) == QLatin1Char('\n'))
            return -1; // blank (whitespace-only) line: no indent at all
        return i;      // the line's content starts here: indent = i
    }
    // The whole window is whitespace: blank line if the '\n' (or EOF) sits
    // right behind it, else an absurdly deep indent — report the window
    // size, which behaves as "very deep" in every > / <= comparison.
    if (winEnd >= buffer_.charCount())
        return -1; // EOF inside a whitespace-only window: blank at EOF
    return buffer_.textRange(winEnd, winEnd + 1) == QLatin1String("\n")
               ? -1
               : kIndentWindow;
}

bool CodeEditor::indentFoldSourceAlive(int header) const
{
    // A fold's own hidden body is the usual next line — and hidden lines
    // are ordinary MODEL lines for this probe, which is exactly why it
    // works on a folded header at all (its visible successor is no longer
    // the fold's line+1). Blank runs skip, matching foldEndForLine's
    // scan: a fold created through a blank run under its header survives
    // that run the same way. Worst case O(blank run) per folded start,
    // and only folded starts INSIDE the paint window pay it.
    const int base = indentOfModelLine(header);
    if (base < 0)
        return false; // header went blank (or slid off the document)
    for (int L = header + 1; L < buffer_.lineCount(); ++L) {
        const int ind = indentOfModelLine(L);
        if (ind < 0)
            continue; // blank: neither ends nor starts the block
        return ind > base; // the first real line below decides the pair
    }
    return false; // only blanks below: the block is gone
}

int CodeEditor::foldEndForLine(int line)
{
    if (line < 0 || line >= buffer_.lineCount())
        return -1;
    // Grammar source first (exact ends from the CURRENT tree; a whole-doc
    // fold query at an explicit user action may pay one O(tree) walk —
    // paint never comes through here). One start line can carry several
    // captures (python: decorated_definition vs function_definition): the
    // TIGHTEST end is the fold the user means at that very line.
    const auto& ff = highlight_->foldsFull();
    int best = -1;
    for (size_t i = 0; i < ff.size(); ++i) {
        if (ff[i].startLine < line)
            continue;
        if (ff[i].startLine > line)
            break;
        if (ff[i].endLine > line && (best < 0 || ff[i].endLine < best))
            best = ff[i].endLine;
    }
    if (best > line)
        return std::min(best, buffer_.lineCount() - 1);
    if (!indentFoldsActive())
        return -1; // grammar fold mode: a line without a grammar capture is
                   // NOT foldable — leading whitespace alone must never fake
                   // python-style nesting in js/json/python (owner report)
    // Indent source (grammar-LESS modes — or a language whose fold query
    // failed to compose — only): forward scan from the start line. Blank
    // lines are skipped (they neither end nor extend a fold); the fold ends
    // at the LAST line deeper than the header. This is an explicit user
    // action, so an O(body) scan is acceptable here — paint resolves only
    // start lines (see recomputeFoldables).
    const int base = indentOfModelLine(line);
    if (base < 0)
        return -1;
    int last = line;
    for (int L = line + 1; L < buffer_.lineCount(); ++L) {
        const int ind = indentOfModelLine(L);
        if (ind < 0)
            continue;
        if (ind <= base)
            break;
        last = L;
    }
    return last > line ? last : -1;
}

// Paint-window foldable starts: engine @fold captures (exact ends, from
// the refresh that produced THIS paint's spans — and, after an explicit
// foldsFull() call, possibly a whole-document superset: filtering by the
// window here is what keeps that harmless) union the local indent rule.
// Indent ends stay UNRESOLVED at paint time (end = start: the triangle
// only needs the start line; foldEndForLine resolves the end on the
// actual fold request) — paint stays O(viewport), even on a 5k-line
// block. Blanks read as "no indent", so a fold never starts on a blank
// line and blank runs between blocks keep separate folds separate.
// ALSO the fold lifecycle choke point: a visible collapsed-fold start
// whose source no longer marks it (grammar capture gone, or — in indent
// modes — the model-level indent pair dead) AUTO-EXPANDS here: the body
// comes back and the ▸ goes away, exactly mirroring the way the same
// edit just drops the marker of an unfolded fold. Grammar validation
// must ride this paint-scoped pass (foldsFull() per keystroke measured
// 24ms+ per key on big docs) — the fold list and the fresh source list
// meet nowhere cheaper.
void CodeEditor::recomputeFoldables(int firstModel, int lastModel)
{
    foldables_.clear();
    const bool useIndent = indentFoldsActive(); // grammar modes: engine ONLY
    const auto& ef = highlight_->folds(); // (start asc, end desc)
    size_t i = 0;
    while (i < ef.size() && ef[i].startLine < firstModel)
        ++i;
    for (; i < ef.size() && ef[i].startLine <= lastModel; ++i) {
        if (!foldables_.isEmpty() && foldables_.back().start == ef[i].startLine)
            continue; // same start: the engine's widest capture (outermost)
                      // leads; VS Code folds outermost on the header line
        if (visibleIndexOfModelLine(ef[i].startLine) < 0)
            continue; // start hidden inside a collapsed run: no row to mark
        foldables_.push_back(FoldState{ef[i].startLine, ef[i].endLine});
    }
    const int docLines = buffer_.lineCount();
    QVector<int> unfoldNow; // collapsed starts whose source died (applied
                            // AFTER the walk: mutating collapsed_ mid-walk
                            // would move the very map the walk rides)
    // first/last are VISIBLE lines (map outputs), so the walk starts at
    // first's own slot; the vt guard matters because modelLineForVisibleIndex
    // CLAMPS: past the last slot it keeps returning the last line, which
    // for a window ending at the document end would never satisfy line >
    // lastModel (measured: that spin hangs the first paint).
    for (int slot = std::max(0, visibleIndexOfModelLine(firstModel));; ++slot) {
        if (slot >= visibleLineTotal())
            break;
        const int line = modelLineForVisibleIndex(slot);
        if (line > lastModel)
            break;
        if (line < firstModel)
            continue; // above the window: skip WITHOUT laying out the geo
                      // cache on off-screen lines (visit order stays painted)
        // The indent rule only fires between two ADJACENT model lines: if
        // line+1 is hidden (inside a collapsed run), `line` is that run's
        // fold start and needs no second opinion from the ADJACENCY rule.
        // But it does need one from the SOURCE rule: a collapsed fold on
        // a line nothing folds anymore auto-expands (unfoldNow below) —
        // the marker asymmetry the owner asked to close.
        if (isFolded(line)) {
            if (!isFoldable(line)) {
                if (useIndent && indentFoldSourceAlive(line))
                    foldables_.push_back(FoldState{line, line}); // end lazy
                else
                    unfoldNow.push_back(line);
            }
            continue;
        }
        if (!useIndent)
            continue; // grammar fold mode: deeper next line is NOT a block —
                      // js/json/python nesting is bracket/syntax defined
        const int ind = indentOfModelLine(line);
        if (ind >= 0 && line + 1 < docLines && visibleIndexOfModelLine(line + 1) >= 0) {
            const int nextInd = indentOfModelLine(line + 1);
            if (nextInd > ind
                && !isFoldable(line)) { // lower_bound walk over the sorted cache
                foldables_.push_back(FoldState{line, line}); // end lazy
            }
        }
    }
    // The indent loop above searched a cache that was only PARTIALLY sorted
    // (engine entries first, indent entries appended in a second ascending
    // run), so a duplicate could slip past. One stable sort + unique on
    // start restores the strict invariant every consumer (isFoldable, the
    // gutter lower_bound, foldLine) relies on; stability keeps engine
    // entries ahead of same-start indent entries, so the exact end wins.
    std::stable_sort(foldables_.begin(), foldables_.end(),
                     [](const FoldState& a, const FoldState& b) { return a.start < b.start; });
    foldables_.erase(std::unique(foldables_.begin(), foldables_.end(),
                                 [](const FoldState& a, const FoldState& b) {
                                     return a.start == b.start;
                                 }),
                     foldables_.end());
    if (!unfoldNow.isEmpty()) {
        // AUTO-EXPAND (owner-requested symmetry with unfolded folds): an
        // edit killed these folds' sources, so open them — the hidden
        // rows return and this paint simply stops drawing their ▸ (the
        // dead starts were deliberately not re-added to the cache). The
        // map grew mid-paint, so ask for one more repaint: this frame's
        // slot window was computed pre-expansion (inner folds newly
        // visible get their own markers, and any stale inner fold is
        // caught by this same check, on that next pass).
        for (const int line : unfoldNow) {
            auto at = std::lower_bound(
                collapsed_.begin(), collapsed_.end(), line,
                [](const FoldState& f, int l) { return f.start < l; });
            while (at != collapsed_.end() && at->start == line)
                at = collapsed_.erase(at);
        }
        rebuildHiddenRuns();
        updateScrollBarRange(); // the slot space grew back: bar follows
        update();
    }
}

// Open every collapsed fold strictly containing `line` (start visible, so
// a simple containment sweep; one pass suffices — unfolding an outer run
// can only REVEAL inner starts, never hide one). Choke points only:
// setPosition/setSelection/moveCaret/ensureCaretVisible — never paint.
bool CodeEditor::revealLine(int line)
{
    if (collapsed_.isEmpty() || line < 0 || line >= buffer_.lineCount())
        return false;
    if (visibleIndexOfModelLine(line) >= 0)
        return false;
    bool changed = false;
    for (int i = 0; i < collapsed_.size();) {
        const FoldState& f = collapsed_[i];
        if (f.start < line && line <= f.end) {
            collapsed_.removeAt(i);
            changed = true;
        } else {
            ++i;
        }
    }
    if (changed) {
        rebuildHiddenRuns();
        updateScrollBarRange(); // the slot space grew back: bar follows
    }
    return changed;
}

int CodeEditor::nextVisibleModelLine(int line) const
{
    const int docLines = buffer_.lineCount();
    if (line >= docLines - 1)
        return line; // at (or past) the document end: caller clamps
    const int s = visibleIndexOfModelLine(line);
    if (s >= 0) {
        // visible: the next slot, which the run merge guarantees lands on
        // a visible line (or the walk is over)
        if (s + 1 < visibleLineTotal())
            return modelLineForVisibleIndex(s + 1);
        return line;
    }
    // hidden: jump out of the run (merged runs make last+1 visible or gone)
    auto it = std::upper_bound(hiddenRuns_.begin(), hiddenRuns_.end(), line,
                               [](int l, const HiddenRun& r) { return l < r.first; });
    if (it != hiddenRuns_.begin()) {
        --it;
        if (line <= it->last)
            return std::min(it->last + 1, docLines - 1);
    }
    return std::min(line + 1, docLines - 1); // unreached by construction
}

int CodeEditor::prevVisibleModelLine(int line) const
{
    if (line <= 0)
        return line; // at the document start: caller clamps
    const int s = visibleIndexOfModelLine(line);
    if (s >= 0) {
        if (s > 0)
            return modelLineForVisibleIndex(s - 1);
        return line;
    }
    auto it = std::upper_bound(hiddenRuns_.begin(), hiddenRuns_.end(), line,
                               [](int l, const HiddenRun& r) { return l < r.first; });
    if (it != hiddenRuns_.begin()) {
        --it;
        if (line <= it->last)
            return it->first - 1; // run.first - 1 is the fold's own start
    }
    return line - 1; // unreached by construction
}

int CodeEditor::slotOfModelLine(int line) const
{
    const int s = visibleIndexOfModelLine(line);
    return s < 0 ? 0 : s;
}

int CodeEditor::modelLineForY(int y) const
{
    if (y < padding_)
        return -1;
    return modelLineForVisibleIndex((y - padding_ + vScroll_) / lineHeight_);
}

int CodeEditor::foldZoneWidth() const
{
    if (lineNumbersMode_ == LineNumbersMode::Off)
        return 0; // no gutter chrome to host the zone (deliberately: the
                  // gutter Off switch IS the folding chrome's switch in v1)
    return std::max(10, qRound(2.2 * cellWidth_));
}

QVector<CodeEditor::FoldRange> CodeEditor::foldableRanges()
{
    // Exact public view of the paint cache: entries whose end the paint
    // left lazy (indent-rule starts) get their real end resolved here —
    // explicit callers (tests, bindings) pay that, the paint path never
    // comes through this function.
    QVector<FoldRange> out;
    out.reserve(foldables_.size());
    for (const FoldState& f : foldables_) {
        const int end = f.end > f.start ? f.end : foldEndForLine(f.start);
        if (end > f.start)
            out.push_back(FoldRange{f.start, end});
    }
    return out;
}

bool CodeEditor::isFoldable(int line) const
{
    // lower_bound (element-first comparator: the one order libstdc++ gets
    // right everywhere) + equality: the foldables cache is sorted by start.
    auto it = std::lower_bound(foldables_.begin(), foldables_.end(), line,
                               [](const FoldState& f, int l) { return f.start < l; });
    return it != foldables_.end() && it->start == line;
}

bool CodeEditor::isFolded(int line) const
{
    auto it = std::lower_bound(collapsed_.begin(), collapsed_.end(), line,
                               [](const FoldState& f, int l) { return f.start < l; });
    return it != collapsed_.end() && it->start == line;
}

bool CodeEditor::foldLine(int line)
{
    if (line < 0 || line >= buffer_.lineCount() || isFolded(line))
        return false;
    if (visibleIndexOfModelLine(line) < 0)
        return false; // hidden start: unfold the outer fold first (public
                      // contract; the mouse can never reach this state)
    const int end = foldEndForLine(line);
    if (end <= line || end >= buffer_.lineCount())
        return false;
    // Drop everything this fold subsumes (same-start residue included),
    // then insert at sort position and re-derive the map. Partially
    // overlapping residue (stale-edit leftovers) keeps its own headroom:
    // the run merge normalizes the visible result either way.
    for (int i = collapsed_.size() - 1; i >= 0; --i) {
        const FoldState& f = collapsed_[i];
        if (f.start == line || (f.start > line && f.end <= end))
            collapsed_.removeAt(i);
    }
    auto at = std::lower_bound(collapsed_.begin(), collapsed_.end(), FoldState{line, end},
                               [](const FoldState& a, const FoldState& b) {
                                   if (a.start != b.start)
                                       return a.start < b.start;
                                   return a.end > b.end;
                               });
    collapsed_.insert(at, FoldState{line, end});
    rebuildHiddenRuns();
    updateScrollBarRange(); // the slot space shrank: vBar + clamp follow
    update();
    return true;
}

bool CodeEditor::unfoldLine(int line)
{
    bool changed = false;
    for (int i = collapsed_.size() - 1; i >= 0; --i)
        if (collapsed_[i].start == line) {
            collapsed_.removeAt(i);
            changed = true;
        }
    if (!changed)
        return false;
    rebuildHiddenRuns();
    updateScrollBarRange();
    update();
    return true;
}

bool CodeEditor::toggleFold(int line)
{
    return isFolded(line) ? unfoldLine(line) : foldLine(line);
}

void CodeEditor::collapseAll()
{
    // Explicit whole-document pass (O(document) allowed here): engine
    // folds first (whole tree), then indent starts the engine does not
    // cover. Outer-first order (start asc, end desc) means every already
    // collapsed inner start is hidden when its turn comes — foldLine's
    // hidden-start rule then skips it, so nesting folds for free.
    QVector<FoldState> all;
    for (const auto& fr : highlight_->foldsFull())
        if (fr.endLine > fr.startLine)
            all.push_back(FoldState{fr.startLine, fr.endLine});
    const int docLines = buffer_.lineCount();
    if (indentFoldsActive()) { // grammar fold mode: engine folds are the whole
        for (int L = 0; L + 1 < docLines; ++L) { // story — no indent pseudo-
            const int ind = indentOfModelLine(L); // starts to add
            if (ind < 0)
                continue;
            const int next = indentOfModelLine(L + 1);
            if (next > ind
                && !std::any_of(all.begin(), all.end(),
                                [L](const FoldState& f) { return f.start == L; })) {
                all.push_back(FoldState{L, L}); // end resolved by foldLine below
            }
        }
    }
    std::sort(all.begin(), all.end(), [](const FoldState& a, const FoldState& b) {
        if (a.start != b.start)
            return a.start < b.start;
        return a.end > b.end;
    });
    bool changed = false;
    for (const FoldState& f : all)
        if (foldLine(f.start)) // foldLine re-resolves the exact end and
            changed = true;    // enforces every invariant (incl. dedupe)
    if (changed)
        update();
}

void CodeEditor::expandAll()
{
    if (collapsed_.isEmpty())
        return;
    collapsed_.clear();
    rebuildHiddenRuns();
    updateScrollBarRange();
    update();
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

namespace {

// M5 decoration ink: hardcoded VS Code dark-leaning constants. The
// set_theme pass (D3 theme row of the PLAN parity table) is where these
// move behind a palette; until then they must stay distinguishable on the
// default dark Base (#323232), which the numeric decoration tests rely on.
QRgb severityInk(CodeEditor::Severity s)
{
    switch (s) {
    case CodeEditor::Severity::Error:
        return QColor(241, 76, 76).rgb(); // red-dominant
    case CodeEditor::Severity::Warning:
        return QColor(255, 204, 0).rgb(); // yellow-dominant
    case CodeEditor::Severity::Information:
        return QColor(117, 190, 255).rgb(); // blue-dominant
    case CodeEditor::Severity::Hint:
        return QColor(127, 127, 127).rgb(); // muted gray
    }
    return QColor(241, 76, 76).rgb();
}

// Zigzag underline: 3px half-period, 2px amplitude sitting just above
// `yWave`. Point parity (even = baseline, odd = raised) is derived from
// the point index, so the run always alternates consistently. Short runs
// (a single cell) degenerate to a straight run so a zero-width diagnostic
// is still visible.
void drawWavyUnderline(QPainter& p, qreal x0, qreal x1, qreal yWave, QRgb ink)
{
    if (x1 <= x0)
        return;
    p.setPen(QPen(QColor::fromRgb(ink), 1.0));
    QVector<QPointF> pts;
    pts.reserve(int((x1 - x0) / 3) + 3);
    for (qreal x = x0; x < x1; x += 3.0)
        pts.append(QPointF(x, pts.size() % 2 ? yWave - 2.0 : yWave));
    pts.append(QPointF(x1, pts.size() % 2 ? yWave - 2.0 : yWave));
    p.drawPolyline(pts.data(), int(pts.size()));
}

// M6 match-quad ink (VS Code Dark+ findMatch family, fixed until
// set_theme): all matches muted orange, the current one bright orange.
// Both are red-dominant over the dark Base - the numeric paint test
// separates that band from the blue selection and the red diagnostic ink.
QRgb findMatchInk(bool current)
{
    return current ? QColor(246, 169, 0).rgb() : QColor(196, 136, 30).rgb();
}

// Shared by the gutter dot (M5) and the ruler marker band (M7): one
// fixed red constant until set_theme routes it.
QRgb breakpointInk()
{
    return QColor(220, 61, 66).rgb();
}

// M7 overview ruler chrome ink: FIXED COOL constants (R==G==B) — the
// ruler background/stripes/viewport frame must never satisfy the
// warm-quad or red-diagnostic numeric probe predicates; only marker
// layers reuse the warm/red inks above, by design.
QRgb rulerBackground()
{
    return QColor(22, 22, 22).rgb();
}
QColor rulerStripe()
{
    return QColor(140, 140, 140, 110); // muted cool gray over the dark bg
}

// M13 fold-triangle ink: a FIXED cool gray (R==G==B by construction) so
// the warm/red pixel predicates of the decoration probes stay green even
// when the fold zone is inside their scan window.
QRgb foldInk()
{
    return qRgb(150, 150, 150);
}

} // namespace

void CodeEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Base));
    p.setPen(palette().color(QPalette::Text));
    p.setFont(font_);

    const int lh = lineHeight_;
    // M13: paint walks VISIBLE SLOTS (the view-line map). first/last stay
    // MODEL lines — the engine's highlight window and every decoration
    // pass (bands, selections, matches, diagnostics, hints) keep their
    // model-contiguous semantics and simply skip hidden rows (their
    // visible index is -1). No folds: runs empty, slot == model line, and
    // every expression below reduces to its pre-M13 form.
    const int vt = std::max(1, visibleLineTotal()); // scroll-past-end can push
    const int firstSlot = std::clamp((vScroll_ - padding_) / lh, 0, vt - 1); // the slot past
    const int lastSlot = std::min(vt - 1, firstSlot + visibleLineCount());   // the last line
    const int first = modelLineForVisibleIndex(firstSlot);
    const int last = modelLineForVisibleIndex(lastSlot);
    const QFontMetricsF fmF(font_); // preedit advance measurement
    const int textLeft = contentLeft(); // M5: padding_ + gutter width

    // M9: the horizontal offset shifts every x in the text space (match
    // quads, selections, carets, preedit, squiggles, inlay hints — the
    // gutter and the ruler are chrome and never ride it). Line-space ->
    // widget-space conversion goes through xw() only; quads cull against
    // the text window [textLeft, winRight) and clamp their left edge, so
    // a scrolled-past band never bleeds into the gutter.
    const double hShift = hScroll_;
    const double win = std::max(1, horizontalViewportWidth());
    const double winRight = textLeft + win; // ruler chrome boundary
    const auto xw = [&](double lineSpaceX) { return textLeft + lineSpaceX - hShift; };

    // M4: bring the highlight engine current here, not on the key path
    // (the typing perf harness measures keyPressEvent logic; the reparse
    // rides the next paint, so one key press reparses at most once).
    // The visible window (model offsets) lets the engine's adaptive
    // policy scope parse+query to what is on screen on big documents.
    if (highlight_->active()) {
        const std::vector<int>& ls = highlight_->lineStarts();
        if (!ls.empty() && size_t(first) < ls.size()) {
            const int vpEnd =
                size_t(last) + 1 < ls.size() ? ls[size_t(last) + 1] : buffer_.charCount();
            highlight_->setViewport(ls[size_t(first)], vpEnd);
        }
    }
    highlight_->refreshIfNeeded();
    // M13: foldable starts for this paint's window, from the tree+spans
    // generation the line above just produced (same cadence, same window
    // — a fold query beyond the viewport is never paid here; see
    // recomputeFoldables for the union + lazy-end rules).
    recomputeFoldables(first, last);

    // M5 layer 2: whole-line bands under selections and text (qtmonaco
    // highlighted lines + optional current-line band). Full width on
    // purpose: the band rides under the gutter too, Monaco-style.
    QColor band = palette().color(QPalette::Highlight);
    band.setAlpha(45);
    if (highlightEnd_ >= 0) {
        for (int line = std::max(first, highlightStart_); line <= std::min(last, highlightEnd_); ++line)
            if (const int slot = visibleIndexOfModelLine(line); slot >= 0) // M13: hidden rows
                p.fillRect(QRect(0, padding_ + slot * lh - vScroll_, width(), lh), band);
    }
    if (currentLineHl_ && caret_.line >= first && caret_.line <= last
        && visibleIndexOfModelLine(caret_.line) >= 0) {
        QColor cur = palette().color(QPalette::Text);
        cur.setAlpha(16); // subtle wash: lighter on dark, darker on light
        p.fillRect(QRect(0, padding_ + slotOfModelLine(caret_.line) * lh - vScroll_, width(), lh),
                   cur);
    }

    // M6: match quads - all matches muted, current bright (they ride
    // decoration layer (3) conceptually; here the engine's own list).
    // matches_ is sorted: binary-search the first visible candidate and
    // walk only the window - thousands of matches cost the viewport only.
    // Quads sit above the bands and under the selection pass, so the
    // selected current match reads as match AND selection at once.
    findRepainting_ = true; // recompute skips its own update() inside a paint
    refreshFindMatches();
    findRepainting_ = false;
    if (!matches_.isEmpty()) {
        auto it = std::lower_bound(matches_.begin(), matches_.end(), first,
                                   [](const MatchRange& m, int line) {
                                       return m.startLine < line;
                                   });
        const MatchRange* cur = currentMatch_ >= 0
                && currentMatch_ < static_cast<int>(matches_.size())
            ? &matches_[currentMatch_]
            : nullptr;
        for (; it != matches_.end() && it->startLine <= last; ++it) {
            const MatchRange& m = *it;
            const int slot = visibleIndexOfModelLine(m.startLine); // M13 cull
            if (slot < 0)
                continue; // hidden match row: no pixels to paint
            const bool isCur = cur == &m;
            const GeoSlot* gs = hitLine(m.startLine);
            const double raw0 = xw(xInLine(gs, m.startCol));
            const double x1 = m.endCol > m.startCol
                ? xw(xInLine(gs, m.endCol))
                : raw0 + cellWidth_; // zero-width match: one phantom cell
            const double x0 = std::max(raw0, double(textLeft));
            if (x1 <= x0 || x0 >= winRight)
                continue; // M9 cull: quad fully outside the text window
            QColor ink = QColor::fromRgb(findMatchInk(isCur));
            ink.setAlpha(isCur ? 130 : 80);
            p.fillRect(QRectF(QPointF(x0, padding_ + slot * lh - vScroll_),
                              QSizeF(x1 - x0, lh)),
                       ink);
        }
    }

    // Selection pass after the bands: paint the background under the glyphs.
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
            const int slot = visibleIndexOfModelLine(line); // M13: a selection
            if (slot < 0)                                    // may span a fold:
                continue;                                    // paint the visible rows only
            const int startCol = (line == s.line) ? s.col : 0;
            // On fully covered lines the selection extends one phantom cell
            // past the text to visualize the selected newline; on the end
            // line it stops at the selection end.
            const int textLen = lineLength(line);
            const int endCol = (line == e.line) ? e.col : std::max(textLen, 1);
            const GeoSlot* gs = hitLine(line);
            const int x0 = qRound(xw(xInLine(gs, startCol)));
            const int x1 = qRound(xw(xInLine(gs, endCol)));
            const int cx0 = std::max(x0, textLeft); // M9: clamp into the window
            if (x1 > cx0 && cx0 < width()) {
                const int yTop = padding_ + slot * lh - vScroll_;
                p.fillRect(QRect(cx0, yTop, x1 - cx0, lh), selColor);
            }
        }
    }

    // M5 layer 1: the gutter - line numbers, breakpoint dots (in their
    // reserved slot) and diagnostic severity icons (in the digit's right
    // padding, so LSP diagnostics never change the gutter layout). Painted
    // after the bands (numbers ride on top of them) and before the text
    // loop; text starts at textLeft, so nothing overlaps the numbers.
    if (const int gw = textLeft - padding_; gw > 0) {
        const int pad = gutterInnerPad();
        const int fz = foldZoneWidth();   // M13 strip at the gutter's right edge
        const int numRight = textLeft - pad - fz; // right edge of the number column
        const int bpCx = padding_ + pad; // breakpoint slot center
        const int iconCx = numRight + pad / 2;
        // M13 fold-triangle geometry (fixed for the frame: the zone never
        // shifts, so neither does the icon, the numbers or the triangles):
        const int foldCx = textLeft - fz / 2; // zone center x
        const double fr = std::max(2.5, cellWidth_ * 0.38); // triangle half-extent
        // Column edge separator: a hairline against Base (VS Code style).
        QColor div = palette().color(QPalette::Text);
        div.setAlpha(45);
        p.setPen(QPen(div, 1.0));
        p.drawLine(textLeft - 1, 0, textLeft - 1, height());
        const QColor dim = palette().color(QPalette::PlaceholderText);
        const QColor bright = palette().color(QPalette::Text);
        auto dit = diagnostics_.begin(); // sorted by startLine: one forward walk
        for (int slot = firstSlot; slot <= lastSlot; ++slot) { // M13: SLOTS
            const int line = modelLineForVisibleIndex(slot);
            const int yTop = padding_ + slot * lh - vScroll_;
            const GeoSlot* gs = hitLine(line);
            const double baseline = yTop + (gs->valid ? gs->line.ascent() : ascent_);
            if (const QString num = lineNumberText(line); !num.isEmpty()) {
                p.setPen(line == caret_.line ? bright : dim);
                // Right-align: monospace digit advances are exactly
                // cellWidth_ (the fact the grid tests assert), so the
                // column width needs no text measurement.
                p.drawText(QPointF(numRight - num.size() * cellWidth_, baseline), num);
            }
            if (std::binary_search(breakpointLines_.begin(), breakpointLines_.end(), line)) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor::fromRgb(breakpointInk())); // fixed red: set_theme pass routes it
                const double r = std::max(3.0, cellWidth_ * 0.45);
                p.drawEllipse(QPointF(qreal(bpCx), yTop + lh / 2.0), r, r);
            }
            // M13: fold triangle in the zone — ▾ (points DOWN) when the
            // fold is open, ▸ (points RIGHT) once collapsed. Fixed cool
            // gray (foldInk): R==G==B, so warm/red decoration probes are
            // unaffected by zone ink.
            if (fz > 0) {
                const auto fit = std::lower_bound(
                    foldables_.begin(), foldables_.end(), line,
                    [](const FoldState& f, int l) { return f.start < l; });
                if (fit != foldables_.end() && fit->start == line) {
                    const double cy = yTop + lh / 2.0;
                    QPolygonF tri;
                    if (isFolded(line))
                        tri << QPointF(foldCx - fr, cy - fr) << QPointF(foldCx - fr, cy + fr)
                            << QPointF(foldCx + fr, cy); // ▸
                    else
                        tri << QPointF(foldCx - fr, cy - fr) << QPointF(foldCx + fr, cy - fr)
                            << QPointF(foldCx, cy + fr); // ▾
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor::fromRgb(foldInk()));
                    p.drawPolygon(tri);
                }
            }
            while (dit != diagnostics_.end() && dit->startLine < line)
                ++dit;
            int worst = int(CodeEditor::Severity::Hint) + 1; // Error sorts first
            for (auto j = dit; j != diagnostics_.end() && j->startLine == line; ++j)
                worst = std::min(worst, int(j->severity));
            if (worst <= int(CodeEditor::Severity::Hint)) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor::fromRgb(severityInk(CodeEditor::Severity(worst))));
                p.drawEllipse(QPointF(qreal(iconCx), yTop + lh / 2.0), 2.5, 2.5);
            }
        }
        p.setPen(palette().color(QPalette::Text));
    }

    // M13: the text loop walks VISIBLE SLOTS too — a collapsed fold's
    // lines have no row, so they are never visited, never laid out and
    // never painted (hidden lines cost nothing; that IS the fold's
    // performance story on top of the slot-shrunk scroll space).
    for (int slot = firstSlot; slot <= lastSlot; ++slot) {
        const int line = modelLineForVisibleIndex(slot);
        const int yTop = padding_ + slot * lh - vScroll_;
        const GeoSlot* gs = hitLine(line);

        // M9 whole-line fast path: once the line's right edge (the same
        // phantom-aware xInLine math the hit-test uses) has scrolled past
        // the window's left edge, NOTHING on the line can be visible — skip
        // carets, preedit and text in O(1). An invalid (empty) slot keeps
        // drawing: its xInLine fallback is 0, i.e. inside the window. The
        // hitLine visit feeds the ambient doc-width extent (noteWidth) even
        // for skipped lines, so the h-bar keeps growing while the user
        // visits the line.
        const bool scrolledPast = gs->valid && xInLine(gs, lineLength(line)) <= hShift;

        if (line == caret_.line && caretOn_ && !scrolledPast) {
            // The caret follows the very cluster advances the line below is
            // drawn with (same QTextLine), so emoji ZWJ sequences no longer
            // push the caret away from the glyphs. During composition the
            // visual caret trails the preedit (which is not in the buffer).
            double cx = xw(xInLine(gs, caret_.col));
            if (!preedit_.isEmpty())
                cx += fmF.horizontalAdvance(preedit_);
            // M9: the caret is text-space ink — cull it against the window
            // so a scrolled-past caret never paints a bar over the gutter
            // and never bleeds under the ruler chrome.
            if (cx + 2.0 > textLeft && cx < winRight)
                p.fillRect(QRect(qRound(cx), yTop, 2, lh), palette().color(QPalette::Text));
        }
        // D1: additional carets blink in sync with the primary (one shared
        // blink timer), drawn with the same 2px rule off the same layout.
        if (!scrolledPast) {
            for (const CaretSlot& slot : extraCarets_) {
                if (slot.caret.line != line || !caretOn_)
                    continue;
                const double cx = xw(xInLine(gs, slot.caret.col));
                if (cx + 2.0 > textLeft && cx < winRight)
                    p.fillRect(QRect(qRound(cx), yTop, 2, lh), palette().color(QPalette::Text));
            }
        }
        if (gs->valid && !scrolledPast) {
            // Draw with the cached QTextLine itself, not a fresh drawText:
            // glyphs, caret and hit-testing then share one layout.
            // MEASURED (numeric band scan, raster backing store = the only
            // engine our widgets use): draw's pos is the layout TOP-LEFT,
            // not a baseline anchor - the engine places glyphs at
            // pos.y + line.y() + line.ascent() on its own (first line has
            // y()==0). Passing baseline=yTop+ascent_ here drew every line
            // ~one line-height too low, an empty band at the top and a
            // caret visibly a line above its text.
            // M9: the origin rides the horizontal offset, and the painter
            // clip bounds the pass to the text window — glyphs tessellated
            // past the window's right edge (the layout budget is ~4
            // viewports wide, see geoMaxUnits) stay unlit, and neither the
            // gutter nor the ruler chrome can be bled into from the text
            // side. Both branches share origin+clip, so the one-layout
            // fmts/no-fmts bit-parity invariant holds.
            const QPointF origin(textLeft - hShift, yTop);
            const auto fmts = formatRangesForLine(line, gs);
            p.save();
            p.setClipRect(QRectF(textLeft, yTop, win, lh));
            if (fmts.isEmpty()) {
                gs->line.draw(&p, origin);
            } else {
                // Colored path: the SAME layout, painted through
                // QTextLayout::draw's FormatRange selections (foreground
                // overrides per range, everything else keeps the pen) -
                // introducing per-color grid math would break the
                // one-layout invariant.
                gs->layout.draw(&p, origin, fmts);
            }
            p.restore();
        }
        if (line == caret_.line && !preedit_.isEmpty() && !scrolledPast) {
            // Composition text at the caret with a dotted underline (Qt's
            // own line-edit convention) to show it is not committed yet.
            // drawText below IS baseline-anchored, so align it with the
            // layout engine's own baseline for the line above.
            const double px = xw(xInLine(gs, caret_.col));
            const double textBaseline = yTop + (gs->valid ? gs->line.ascent() : ascent_);
            p.drawText(QPointF(px, textBaseline), preedit_);
            p.setPen(QPen(palette().color(QPalette::Text), 0, Qt::DotLine));
            const double pw = fmF.horizontalAdvance(preedit_);
            p.drawLine(QPointF(px, yTop + lh - 2), QPointF(px + pw, yTop + lh - 2));
            p.setPen(palette().color(QPalette::Text));
        }
    }

    // M5 layer 3 on top of the text: diagnostic wavy underlines. The list
    // is sorted by startLine, so multi-line ranges clip per line and the
    // startLine>last break is safe. Counts are consumer-bounded (LSP
    // batches); M6 match highlights will need indexing if they ride here.
    for (const Diagnostic& d : diagnostics_) {
        if (d.startLine > last)
            break;
        if (d.endLine < first)
            continue;
        const QRgb ink = severityInk(d.severity);
        for (int line = std::max(first, d.startLine); line <= std::min(last, d.endLine); ++line) {
            const int slot = visibleIndexOfModelLine(line); // M13: multi-line diag
            if (slot < 0)                                   // spans a fold: the
                continue;                                   // hidden rows are skipped
            const int len = lineLength(line);
            int c0 = std::clamp(line == d.startLine ? d.startCol : 0, 0, len);
            int c1 = std::clamp(line == d.endLine ? d.endCol : len, 0, len);
            if (c1 <= c0) {
                // Zero-width marker (position-only diagnostic, LSP
                // style): paint one phantom cell so it stays visible.
                c1 = c0 + 1;
            }
            const GeoSlot* gs = hitLine(line);
            const double raw0 = xw(xInLine(gs, c0));
            const double x1 = std::min(xw(xInLine(gs, c1)), winRight);
            const double x0 = std::max(raw0, double(textLeft)); // M9 clamp+cull
            if (x1 <= x0)
                continue;
            const int yTop = padding_ + slot * lh - vScroll_;
            drawWavyUnderline(p, x0, x1, yTop + lh - 2.5, ink);
        }
    }

    // M5 layer 3: inlay hints on top, without re-flowing the text (the
    // caller anchors them into existing whitespace; re-flow later is a
    // rendering refinement, not an API change).
    if (!inlayHints_.isEmpty()) {
        p.setPen(palette().color(QPalette::PlaceholderText));
        for (const InlayHint& h : inlayHints_) {
            if (h.line < first)
                continue;
            if (h.line > last)
                break; // sorted by line
            const int slot = visibleIndexOfModelLine(h.line); // M13: hint inside
            if (slot < 0)                                     // a collapsed fold
                continue;                                     // has no row
            const GeoSlot* gs = hitLine(h.line);
            const double x = xw(xInLine(gs, std::max(h.col, 0)));
            if (x < textLeft || x >= winRight)
                continue; // M9: hint anchored outside the text window
            const int hTop = padding_ + slot * lh - vScroll_;
            const double baseline = hTop + (gs->valid ? gs->line.ascent() : ascent_);
            p.drawText(QPointF(x, baseline), h.text);
        }
        p.setPen(palette().color(QPalette::Text));
    }

    // M7: the overview ruler is the LAST pass — opaque chrome that
    // overpaints the text's right edge (the viewport is chrome-bounded,
    // VS Code-style). Cost rides the strip, never the document.
    paintOverviewRuler(p);
}

// M7: the strip is chrome, not a render of the document: sampled line
// stripes (step = ceil(1/pxPerLine), so O(strip height) stripes), marker
// layers from the already-sorted decoration vectors (subsampled to the
// strip when huge), and the viewport frame. Line reads are capped
// textRange windows — a 1M-unit line costs the strip one small slice,
// never a materialization (the E1 "no off-screen paint cost" DoD).
void CodeEditor::paintOverviewRuler(QPainter& p) const
{
    const QRect g = overviewRulerGeometry();
    if (g.isNull())
        return;
    p.fillRect(g, QColor::fromRgb(rulerBackground()));

    const int total = std::max(1, totalHeight());
    const int h = std::max(1, height());
    const auto rulerY = [&](int docPx) { return int(qint64(docPx) * h / total); };
    // one-line marker band, thickened so it survives the squashing.
    // M13: docPx rides the SLOT space (same totalHeight ratio every other
    // mapping uses); a marker on a hidden line has no row in that space —
    // the band is simply not drawn (the fold hides its stripes too,
    // VS Code-style).
    const auto band = [&](int line, const QColor& c, int minThick = 2) {
        const int slot = visibleIndexOfModelLine(line);
        if (slot < 0)
            return;
        const int y0 = rulerY(padding_ + slot * lineHeight_);
        const int y1 = rulerY(padding_ + (slot + 1) * lineHeight_);
        p.fillRect(QRect(g.left() + 3, y0, kOverviewRulerWidth - 6, qMax(minThick, y1 - y0)), c);
    };

    const int vt = std::max(1, visibleLineTotal()); // M13: sample SLOTS: a
    const double pxPerLine = double(h) * lineHeight_ / total; // collapsed run costs
    const int step = pxPerLine >= 1.0 ? 1 : int(std::ceil(1.0 / pxPerLine)); // zero stripes
    const int cap = kOverviewRulerWidth - 6; // stripe-precision budget in units

    p.setPen(Qt::NoPen);
    p.setBrush(rulerStripe());
    for (int slot = 0; slot < vt; slot += step) {
        const int line = modelLineForVisibleIndex(slot);
        const int off = buffer_.offsetOfLine(line);
        const QString chunk = buffer_.textRange(off, qMin(off + cap + 1, buffer_.charCount()));
        const int nl = chunk.indexOf(QLatin1Char('\n'));
        const int len = nl >= 0 ? nl : chunk.size(); // '\n' excluded; capped
        if (len <= 0)
            continue; // empty line: no stripe (Monaco draws nothing there)
        int indent = 0;
        while (indent < len && (chunk.at(indent) == QLatin1Char(' ') || chunk.at(indent) == QLatin1Char('\t')))
            ++indent; // tabs count as 1 unit: v1 approximation
        const int x0 = g.left() + 3 + qMin(indent, cap - 1);
        const int w = qMax(1, qMin(len, cap) - qMin(indent, cap - 1));
        const int y0 = rulerY(padding_ + slot * lineHeight_);
        const int y1 = rulerY(padding_ + (slot + 1) * lineHeight_);
        p.fillRect(QRect(x0, y0, w, qMax(1, y1 - y0)), rulerStripe());
    }

    // Marker layers, cool selection band first, then the warm/red state
    // inks (M5/M6 constants). Huge sets are stride-subsampled to a few
    // stripes per strip pixel — the paint never walks the whole vector.
    if (highlightEnd_ >= highlightStart_ && highlightStart_ >= 0) {
        const int y0 = rulerY(padding_ + highlightStart_ * lineHeight_);
        const int y1 = rulerY(padding_ + (highlightEnd_ + 1) * lineHeight_);
        p.fillRect(QRect(g.left() + 3, y0, kOverviewRulerWidth - 6, qMax(2, y1 - y0)),
                   QColor(96, 144, 196, 90)); // cool blue: passes every probe predicate
    }
    const auto strideFor = [h](int n) { return n > 4 * h ? n / (4 * h) + 1 : 1; };
    for (int i = 0; i < breakpointLines_.size(); i += strideFor(breakpointLines_.size()))
        band(breakpointLines_[i], QColor::fromRgb(breakpointInk()));
    for (int i = 0; i < diagnostics_.size(); i += strideFor(diagnostics_.size()))
        band(diagnostics_[i].startLine, QColor::fromRgb(severityInk(diagnostics_[i].severity)));
    for (int i = 0; i < matches_.size(); i += strideFor(matches_.size()))
        band(matches_[i].startLine, QColor::fromRgb(findMatchInk(false)), 1);
    if (currentMatch_ >= 0 && currentMatch_ < matches_.size())
        band(matches_[currentMatch_].startLine, QColor::fromRgb(findMatchInk(true)), 1);

    // Viewport frame last (the strip's own scroll state: state, not ink).
    const QRect vp = overviewRulerViewportRect();
    p.setBrush(QColor(210, 210, 210, 26)); // faint cool fill (R==G==B)
    p.setPen(QPen(QColor(190, 190, 190, 110), 1.0));
    p.drawRect(vp.marginsRemoved({0, 0, 1, 1}));
}

QVector<QTextLayout::FormatRange> CodeEditor::formatRangesForLine(int line, const GeoSlot* gs) const
{
    QVector<QTextLayout::FormatRange> out;
    if (!highlight_->active() || !gs->valid)
        return out;
    // The engine's line index + span list are the same tree generation
    // (paintEvent calls refreshIfNeeded() before this runs).
    const std::vector<int>& ls = highlight_->lineStarts();
    if (line < 0 || size_t(line) >= ls.size())
        return out;
    const int lineStart = ls[size_t(line)];
    const int lineEnd = size_t(line) + 1 < ls.size()
        ? ls[size_t(line) + 1] - 1 // the '\n' terminating this line
        : buffer_.charCount();     // last line: no terminator past it
    const auto& hs = highlight_->spans();
    auto it = std::upper_bound(hs.begin(), hs.end(), lineStart,
                               [](int v, const HighlightEngine::Span& s) { return v < s.end; });
    const int laidOut = static_cast<int>(gs->text.size()); // truncation-aware
    for (; it != hs.end() && it->start < lineEnd; ++it) {
        const QColor color = HighlightEngine::colorFor(it->kind);
        if (!color.isValid())
            continue;
        int a = std::max(it->start, lineStart) - lineStart;
        int b = std::min(it->end, lineEnd) - lineStart;
        b = std::min(b, laidOut);
        if (a >= b)
            continue; // span lies in the never-laid-out tail of a monster line
        a = snapToGraphemeForward(gs->text, a);
        b = snapToGraphemeForward(gs->text, b);
        if (a >= b)
            continue;
        QTextCharFormat fmt; // font untouched: monospace stays monospace
        fmt.setForeground(color);
        out.append(QTextLayout::FormatRange{a, b - a, fmt});
    }
    return out;
}

// --- M4 syntax highlighting public API ----------------------------------

void CodeEditor::setLanguage(const QString& language)
{
    highlight_->setLanguage(language);
    update(); // colors appear on the next paint (parse+query deferred there)
}

QString CodeEditor::language() const
{
    return highlight_->language();
}

QStringList CodeEditor::availableLanguages()
{
    return HighlightEngine::languageIds();
}

QVector<CodeEditor::HighlightSpan> CodeEditor::highlightSpans(int startOffset, int endOffset)
{
    QVector<HighlightSpan> out;
    const int n = buffer_.charCount();
    startOffset = std::clamp(startOffset, 0, n);
    endOffset = std::clamp(endOffset, 0, n);
    if (startOffset >= endOffset)
        return out;
    // Exactness contract: the returned spans cover the requested window
    // even when paint-driven refreshes only parsed the viewport (M4
    // adaptive policy) - spansFor may pay a synchronous re-query or, in
    // the rare programmatic whole-document dump, one full parse.
    const auto& hs = highlight_->spansFor(startOffset, endOffset);
    for (const auto& s : hs) {
        if (s.end <= startOffset)
            continue;
        if (s.start >= endOffset)
            break; // spans are sorted; nothing later can intersect
        out.push_back(HighlightSpan{std::max(s.start, startOffset),
                                    std::min(s.end, endOffset), s.kind});
    }
    return out;
}

// --- M5 decorations + gutter (public API section) ------------------------

int CodeEditor::gutterInnerPad() const
{
    return std::max(3, qRound(cellWidth_));
}

int CodeEditor::gutterWidth() const
{
    if (lineNumbersMode_ == LineNumbersMode::Off)
        return 0;
    // The widest displayed number decides the digit column: every mode
    // shows either the absolute line number or a distance smaller than
    // lineCount. Digits are exactly cellWidth_ wide (monospace), so the
    // column is digit-count * cellWidth; 9 -> 10 lines grows it one cell.
    int digits = 1;
    for (int n = buffer_.lineCount(); n >= 10; n /= 10)
        ++digits;
    const int pad = gutterInnerPad();
    int w = qRound(digits * cellWidth_) + 2 * pad;
    // M13: the fold zone rides the gutter's right edge ALWAYS (when the
    // gutter exists): constant width, so folding/unfolding never shifts a
    // single glyph and the M5 crossover/delta tests keep their exact math.
    w += foldZoneWidth();
    // One reserved glyph slot while any breakpoint exists: toggling a
    // breakpoint must not shift the already-drawn numbers. Diagnostic
    // icons fit into the right digit padding instead (Gate-2 LSP
    // diagnostics never change the layout).
    if (!breakpointLines_.isEmpty())
        w += 2 * pad;
    return w;
}

QString CodeEditor::lineNumberText(int line) const
{
    if (lineNumbersMode_ == LineNumbersMode::Off)
        return {};
    if (line < 0 || line >= buffer_.lineCount())
        return {};
    switch (lineNumbersMode_) {
    case LineNumbersMode::On:
        return QString::number(line + 1);
    case LineNumbersMode::Interval:
        // Monaco's interval behavior: the absolute number only on every
        // 10th (1-based) line; the rest of the column stays blank.
        return (line + 1) % 10 == 0 ? QString::number(line + 1) : QString();
    case LineNumbersMode::Relative:
        // Distance to the caret line; the caret line itself shows its
        // absolute number (Monaco's relative mode behavior).
        if (line == caret_.line)
            return QString::number(line + 1);
        return QString::number(std::abs(line - caret_.line));
    case LineNumbersMode::Off:
        break;
    }
    return {};
}

void CodeEditor::setLineNumbersMode(LineNumbersMode mode)
{
    if (mode == lineNumbersMode_)
        return;
    lineNumbersMode_ = mode;
    updateScrollBarRange(); // contentLeft moved: the h-window width (and the
                            // hBar geometry riding it) changed with the gutter
    update(); // contentLeft() shifts everything drawn at x
}

void CodeEditor::setBreakpointLines(const QVector<int>& lines)
{
    auto v = lines;
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    if (v == breakpointLines_)
        return;
    breakpointLines_ = std::move(v);
    update(); // the reserved slot may appear/disappear + the dots
}

void CodeEditor::setDiagnostics(const QVector<Diagnostic>& diagnostics)
{
    auto v = diagnostics;
    std::sort(v.begin(), v.end(), [](const Diagnostic& a, const Diagnostic& b) {
        if (a.startLine != b.startLine)
            return a.startLine < b.startLine;
        if (a.startCol != b.startCol)
            return a.startCol < b.startCol;
        if (a.endLine != b.endLine)
            return a.endLine < b.endLine;
        return a.endCol < b.endCol;
    });
    diagnostics_ = std::move(v);
    update();
}

void CodeEditor::setInlayHints(const QVector<InlayHint>& hints)
{
    auto v = hints;
    std::sort(v.begin(), v.end(), [](const InlayHint& a, const InlayHint& b) {
        return a.line != b.line ? a.line < b.line : a.col < b.col;
    });
    inlayHints_ = std::move(v);
    update();
}

void CodeEditor::setHighlightedLines(int startLine, int endLine)
{
    if (endLine < startLine)
        std::swap(startLine, endLine); // tolerant arg order
    highlightStart_ = startLine;
    highlightEnd_ = endLine;
    update();
}

void CodeEditor::clearHighlightedLines()
{
    if (highlightStart_ < 0)
        return;
    highlightStart_ = highlightEnd_ = -1;
    update();
}

void CodeEditor::setCurrentLineHighlight(bool enabled)
{
    if (enabled == currentLineHl_)
        return;
    currentLineHl_ = enabled;
    update();
}

// --- M6 find/replace (public API section) --------------------------------

namespace {

// The per-line match engine (the DoD math; verified through the public
// API). Literal queries are escaped into the same regex path. PCRE2 runs
// in UTF-16 code-unit mode (Qt never enables UTF mode): "." steps through
// a surrogate pair half-pair at a time, and an empty match would advance
// forever without a manual anchor walk. So per raw match:
//  - non-empty: a boundary that splits a pair snaps OUTWARD (the whole
//    pair enters or stays out: neither highlight nor replace ever splits
//    or orphans half a pair);
//  - empty: snaps only at a pair split, and then takes the WHOLE pair
//    (a lone half-cell would orphan a half on replace);
//  - a snapped range equal to the previous match dedups (a pair's two
//    "." raws collapse into one range);
//  - the scan anchor advances by the raw width, minimum 1 (an empty
//    match would otherwise loop).
// WholeWord wraps \b around the (escaped when literal) pattern: a lookaround
// start still sees the word context (\w(?=x) keeps the leading \b), and a
// trailing lookaround yields the \b{ws} zero-width match that snaps onto
// the word - the whole-word editor behavior.
QVector<CodeEditor::MatchRange> findMatchInLine(const QString& line, int lineIdx,
                                                const QRegularExpression& re)
{
    QVector<CodeEditor::MatchRange> out;
    int from = 0;
    while (from <= line.size()) {
        // Unanchored from an offset is equivalent here (and free of the
        // deprecated match options): PCRE2 reports the EARLIEST match at
        // or after the offset, which is exactly the global-scan step.
        const QRegularExpressionMatch m = re.match(line, from);
        if (!m.hasMatch())
            break;
        const int rawStart = m.capturedStart(0);
        const int rawEnd = m.capturedEnd(0);
        int start = rawStart, end = rawEnd;
        const auto splitsAt = [&](int at) {
            return at > 0 && at < line.size() && line.at(at - 1).isHighSurrogate()
                && line.at(at).isLowSurrogate();
        };
        if (rawStart == rawEnd) {
            if (splitsAt(rawStart)) {
                start = rawStart - 1;
                end = rawEnd + 1;
            }
        } else {
            if (splitsAt(start))
                --start;
            if (splitsAt(end))
                ++end;
        }
        if (out.isEmpty() || out.back().startCol != start || out.back().endCol != end)
            out.append(CodeEditor::MatchRange{lineIdx, start, lineIdx, end});
        // Scan advance: past the RAW match (never the snapped one: the
        // pair's second "." raw must still be seen to dedup), minimum
        // one unit per step. The outer max keeps progress even for the
        // exotic \K pattern, whose reported start can precede the offset.
        from = std::max(rawEnd > rawStart ? rawEnd : rawStart + 1, from + 1);
    }
    return out;
}

// The built-in find/replace bar. It talks to the editor purely through
// the public find API (the editor holds it as a plain QWidget*), so this
// class never crosses the public/Shiboken boundary. Two rows: toggles,
// query, count, prev/next, close; then the replace row. Enter in the
// query = next, Shift+Enter = previous, Enter in the replace field =
// replace current + advance. Replace All is button-ONLY - that is the
// D4 "confirm step": no key chord ever fires a whole-document mutation
// by accident, and the batch lands as one undoable D1 action.
class FindBar : public QWidget {
public:
    explicit FindBar(CodeEditor* editor)
        : QWidget(editor), editor_(editor)
    {
        // Named so tests (and later, consumers embedding their own chrome)
        // can locate the bar via findChild without exposing the class.
        setObjectName(QStringLiteral("qmn-find-bar"));
        setAutoFillBackground(true);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, pal.color(QPalette::Window).lighter(108));
        setPalette(pal);

        auto toggle = [this](const QString& text, const QString& tip) {
            auto* b = new QPushButton(text, this);
            b->setCheckable(true);
            b->setToolTip(tip);
            return b;
        };
        caseToggle_ = toggle(QStringLiteral("Aa"), tr("Match case"));
        wordToggle_ = toggle(QStringLiteral("ab|"), tr("Whole word"));
        regexToggle_ = toggle(QStringLiteral(".*"), tr("Regular expression"));

        queryEdit_ = new QLineEdit(this);
        queryEdit_->setClearButtonEnabled(true);
        queryEdit_->setMinimumWidth(120); // keeps the bar's natural width
                                          // inside a 600px editor: the
                                          // right-edge anchor is exact
        queryEdit_->setPlaceholderText(tr("Find"));
        replaceEdit_ = new QLineEdit(this);
        replaceEdit_->setPlaceholderText(tr("Replace"));
        countLabel_ = new QLabel(this);
        countLabel_->setTextFormat(Qt::PlainText);
        // Reserve the widest count so typing never reflows the bar.
        countLabel_->setText(QStringLiteral("9999/99999"));
        countLabel_->setMinimumWidth(countLabel_->sizeHint().width());
        countLabel_->setText(QString());

        // \u escapes, NOT UTF-8 byte hex escapes: QStringLiteral prefixes the
        // literal with u"", so \xE2... sign-extends into the wrong code
        // points (measured: the arrows and the X rendered as mojibake).
        prevButton_ = new QPushButton(QStringLiteral("\u25B2"), this); // ▲
        nextButton_ = new QPushButton(QStringLiteral("\u25BC"), this); // ▼
        replaceButton_ = new QPushButton(tr("Replace"), this);
        allButton_ = new QPushButton(tr("All"), this);
        close_ = new QPushButton(QStringLiteral("\u00D7"), this); // ×

        auto* row1 = new QHBoxLayout;
        row1->setContentsMargins(0, 0, 0, 0);
        row1->addWidget(caseToggle_);
        row1->addWidget(wordToggle_);
        row1->addWidget(regexToggle_);
        row1->addWidget(queryEdit_, 1);
        row1->addWidget(countLabel_);
        row1->addWidget(prevButton_);
        row1->addWidget(nextButton_);
        row1->addWidget(close_);
        auto* row2 = new QHBoxLayout;
        row2->setContentsMargins(0, 0, 0, 0);
        row2->addWidget(replaceEdit_, 1);
        row2->addWidget(replaceButton_);
        row2->addWidget(allButton_);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(6, 4, 6, 4);
        lay->addLayout(row1);
        lay->addLayout(row2);
        // A hard 300px floor on the BAR widget (the layout has no
        // setMinimumWidth): below the natural width the bar COMPRESSES
        // (documented v1 compromise, VS Code truncates too) instead of
        // having its resize refused by the widgets' own minimums - that
        // keeps the right-edge anchor exact at every editor width.
        setMinimumWidth(300);

        connect(queryEdit_, &QLineEdit::textChanged, this,
                [this](const QString& t) { editor_->setFindText(t); });
        for (QPushButton* t : {caseToggle_, wordToggle_, regexToggle_})
            connect(t, &QPushButton::toggled, this, [this] { pushFlags(); });
        connect(prevButton_, &QPushButton::clicked, this, [this] {
            editor_->findPrevious();
            updateCount();
        });
        connect(nextButton_, &QPushButton::clicked, this, [this] {
            editor_->findNext();
            updateCount();
        });
        connect(replaceButton_, &QPushButton::clicked, this, [this] {
            editor_->replaceNext(replaceEdit_->text());
            updateCount();
        });
        connect(allButton_, &QPushButton::clicked, this, [this] {
            editor_->replaceAll(replaceEdit_->text());
            updateCount();
        });
        connect(close_, &QPushButton::clicked, this, [this] { editor_->hideFindBar(); });

        // Esc closes from anywhere in the bar; Enter/Shift+Enter get their
        // find semantics BEFORE the line edits eat them (returnPressed
        // cannot tell Shift apart, hence the filter).
        queryEdit_->installEventFilter(this);
        replaceEdit_->installEventFilter(this);
    }

    void open(bool replaceField)
    {
        syncFromEditor();
        show();
        place();
        raise();
        QLineEdit* le = replaceField ? replaceEdit_ : queryEdit_;
        le->setFocus();
        le->selectAll(); // Monaco-style: the remembered query is selected,
                         // so the first typed character replaces it
    }

    // Position at the top-right of the editor, clamped to its width.
    void place()
    {
        QWidget* e = parentWidget();
        if (!e)
            return;
        // effective minimum hint (the 300px widget floor, see the ctor):
        // resize below it would be refused, breaking the right-edge math
        const int minW = qMax(minimumWidth(), 300);
        QSize s = sizeHint();
        s.rwidth() = qMax(minW, qMin(s.width(), e->width() - 8));
        resize(s);
        move(e->width() - 4 - s.width(), 4);
    }

    // Reflect the editor's query/flags without echoing back into setters.
    void syncFromEditor()
    {
        const QSignalBlocker bq(queryEdit_);
        queryEdit_->setText(editor_->findText());
        const QSignalBlocker bc(caseToggle_), bw(wordToggle_), br(regexToggle_);
        caseToggle_->setChecked(editor_->findFlags().testFlag(CodeEditor::FindFlag::MatchCase));
        wordToggle_->setChecked(editor_->findFlags().testFlag(CodeEditor::FindFlag::WholeWord));
        regexToggle_->setChecked(editor_->findFlags().testFlag(CodeEditor::FindFlag::UseRegex));
        updateCount();
    }

    // The count read goes through matchCount(), which refreshes a set the
    // editor's typing may have invalidated - so the label is never stale.
    void updateCount()
    {
        if (!editor_->findPatternValid()) {
            countLabel_->setText(tr("bad re"));
            for (QPushButton* b : {prevButton_, nextButton_, replaceButton_, allButton_})
                b->setEnabled(false);
            return;
        }
        const int n = editor_->matchCount();
        const int cur = editor_->currentMatch();
        countLabel_->setText(cur >= 0 ? QStringLiteral("%1/%2").arg(cur + 1).arg(n)
                                      : QString::number(n));
        const bool any = n > 0;
        prevButton_->setEnabled(any);
        nextButton_->setEnabled(any);
        replaceButton_->setEnabled(any);
        allButton_->setEnabled(any);
        allButton_->setText(n > 0 ? tr("All (%L1)").arg(n) : tr("All"));
    }

protected:
    bool eventFilter(QObject* watched, QEvent* ev) override
    {
        if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            const bool isEnter = ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter;
            if (ke->key() == Qt::Key_Escape) {
                editor_->hideFindBar();
                return true;
            }
            if (isEnter && watched == queryEdit_) {
                if (ke->modifiers() & Qt::ShiftModifier)
                    editor_->findPrevious();
                else
                    editor_->findNext();
                updateCount();
                return true;
            }
            if (isEnter && watched == replaceEdit_) {
                editor_->replaceNext(replaceEdit_->text());
                updateCount();
                return true;
            }
        }
        return QWidget::eventFilter(watched, ev);
    }

    void keyPressEvent(QKeyEvent* ke) override
    {
        if (ke->key() == Qt::Key_Escape) // focus sitting on a button
            editor_->hideFindBar();
        else
            QWidget::keyPressEvent(ke);
    }

private:
    void pushFlags()
    {
        CodeEditor::FindFlags f;
        if (caseToggle_->isChecked())
            f |= CodeEditor::FindFlag::MatchCase;
        if (wordToggle_->isChecked())
            f |= CodeEditor::FindFlag::WholeWord;
        if (regexToggle_->isChecked())
            f |= CodeEditor::FindFlag::UseRegex;
        editor_->setFindFlags(f); // recomputes, which re-syncs the label
    }

    CodeEditor* editor_;
    QPushButton* caseToggle_;
    QPushButton* wordToggle_;
    QPushButton* regexToggle_;
    QLineEdit* queryEdit_;
    QLineEdit* replaceEdit_;
    QLabel* countLabel_;
    QPushButton* prevButton_;
    QPushButton* nextButton_;
    QPushButton* replaceButton_;
    QPushButton* allButton_;
    QPushButton* close_;
};

} // namespace

void CodeEditor::recomputeFindMatches()
{
    findDirty_ = false;
    currentMatch_ = -1;
    matches_.clear();
    findPatternValid_ = true;
    if (findText_.isEmpty()) // an empty query never searches (Monaco too)
        return;

    const QString pattern = findFlags_.testFlag(FindFlag::UseRegex)
        ? findText_
        : QRegularExpression::escape(findText_);
    const QString wrapped = findFlags_.testFlag(FindFlag::WholeWord)
        ? QStringLiteral("\\b(?:%1)\\b").arg(pattern)
        : pattern;
    QRegularExpression::PatternOptions opts = QRegularExpression::UseUnicodePropertiesOption;
    if (!findFlags_.testFlag(FindFlag::MatchCase))
        opts |= QRegularExpression::CaseInsensitiveOption;
    const QRegularExpression re(wrapped, opts);
    if (!re.isValid()) { // "invalid" must be tellable from "no matches"
        findPatternValid_ = false;
        if (!findRepainting_)
            update();
        syncFindBarLabel();
        return;
    }

    const int lines = buffer_.lineCount();
    for (int l = 0; l < lines; ++l) {
        const auto lineMatches = findMatchInLine(buffer_.lineText(l), l, re);
        matches_.reserve(matches_.size() + lineMatches.size());
        matches_.append(lineMatches); // findMatchInLine keeps line order
    }
    if (!findRepainting_)
        update(); // highlights ride the repaint (entries outside a paint)
    syncFindBarLabel();
}

void CodeEditor::refreshFindMatches()
{
    if (findDirty_)
        recomputeFindMatches();
}

void CodeEditor::syncFindBarLabel() const
{
    if (findBar_)
        static_cast<FindBar*>(findBar_)->updateCount();
}

void CodeEditor::setFindText(const QString& text)
{
    if (text == findText_)
        return;
    findText_ = text;
    recomputeFindMatches();
}

void CodeEditor::setFindFlags(FindFlags flags)
{
    if (flags == findFlags_)
        return;
    findFlags_ = flags;
    recomputeFindMatches();
}

namespace {

// Document-order comparison for the match walk: matches_ is sorted by
// (startLine, startCol), so std::lower_bound over a (line, col) pair gives
// the first candidate at/after a position.
bool matchStartsBefore(const CodeEditor::MatchRange& m, const std::pair<int, int>& p)
{
    return m.startLine < p.first || (m.startLine == p.first && m.startCol < p.second);
}

// The current match counts as "current" only while the selection covers
// it exactly - a click elsewhere quietly drops the position, and the next
// F3 resumes from the caret (Monaco behavior), never from a stale index.
bool selectionOnMatch(const QVector<CodeEditor::MatchRange>& ms, int cur,
                      TextPosition selStart, TextPosition selEnd)
{
    if (cur < 0 || cur >= static_cast<int>(ms.size()))
        return false;
    const CodeEditor::MatchRange& m = ms[cur];
    return m.startLine == selStart.line && m.startCol == selStart.col
        && m.endLine == selEnd.line && m.endCol == selEnd.col;
}

} // namespace

int CodeEditor::matchCount()
{
    refreshFindMatches();
    return static_cast<int>(matches_.size());
}

// The EFFECTIVE current match: the stored index only answers while the
// selection still covers it (see selectionOnMatch). Read-only - the walk
// state itself is only moved by the actions, never by a paint or a label
// refresh reading through here.
int CodeEditor::currentMatch()
{
    refreshFindMatches();
    return selectionOnMatch(matches_, currentMatch_, selStart(), selEnd()) ? currentMatch_ : -1;
}

bool CodeEditor::gotoMatch(int index)
{
    refreshFindMatches();
    if (index < 0 || index >= static_cast<int>(matches_.size()))
        return false;
    const MatchRange& m = matches_[index];
    setSelection({m.startLine, m.startCol}, {m.endLine, m.endCol});
    currentMatch_ = index;
    ensureCaretVisible(); // a match far down must scroll into view
    update();
    syncFindBarLabel();
    return true;
}

bool CodeEditor::findNext()
{
    refreshFindMatches();
    if (matches_.isEmpty())
        return false;
    const int n = static_cast<int>(matches_.size());
    int idx;
    if (selectionOnMatch(matches_, currentMatch_, selStart(), selEnd())) {
        idx = currentMatch_ + 1 >= n ? 0 : currentMatch_ + 1; // cycle; the
                                                              // wrap
                                                              // re-selects
                                                              // the first
                                                              // match
    } else {
        const auto it = std::lower_bound(matches_.begin(), matches_.end(),
                                         std::pair(caret_.line, caret_.col),
                                         matchStartsBefore);
        idx = it == matches_.end() ? 0 : int(it - matches_.begin());
    }
    return gotoMatch(idx);
}

bool CodeEditor::findPrevious()
{
    refreshFindMatches();
    if (matches_.isEmpty())
        return false;
    const int n = static_cast<int>(matches_.size());
    int idx;
    if (selectionOnMatch(matches_, currentMatch_, selStart(), selEnd())) {
        idx = currentMatch_ == 0 ? n - 1 : currentMatch_ - 1;
    } else {
        const auto it = std::lower_bound(matches_.begin(), matches_.end(),
                                         std::pair(caret_.line, caret_.col),
                                         matchStartsBefore);
        idx = it == matches_.begin() ? n - 1 : int(it - matches_.begin()) - 1;
    }
    return gotoMatch(idx);
}

bool CodeEditor::replaceRange(const MatchRange& m, const QString& replacement)
{
    const UndoMark pre = captureEditMark();
    buffer_.beginEditGroup(); // bracketed: its own undo step, never coalesced
    const int start = buffer_.offsetOf(m.startLine, m.startCol);
    buffer_.removeRange(start, buffer_.offsetOf(m.endLine, m.endCol));
    if (!replacement.isEmpty())
        buffer_.insertAtOffset(start, replacement);
    buffer_.endEditGroup();
    // Single-caret operation (find semantics; D1 extras collapse), caret
    // at the replacement end. A multiline replacement spans lines, so the
    // caret position goes through the model.
    const auto atLc = buffer_.lineColAt(start + replacement.size());
    const TextPosition at{atLc.line, atLc.col};
    setSelection(at, at);
    reconcileUndoMarks(pre);
    return true;
}

bool CodeEditor::replaceNext(const QString& replacement)
{
    refreshFindMatches();
    if (matches_.isEmpty())
        return false;
    int idx;
    if (selectionOnMatch(matches_, currentMatch_, selStart(), selEnd())) {
        idx = currentMatch_;
    } else { // no current: the next at/after the caret, no wrap
             // (Monaco's Replace only acts on the selection or the
             // next occurrence ahead)
        const auto it = std::lower_bound(matches_.begin(), matches_.end(),
                                         std::pair(caret_.line, caret_.col),
                                         matchStartsBefore);
        if (it == matches_.end())
            return false;
        idx = int(it - matches_.begin());
    }
    const MatchRange target = matches_[idx]; // copy: replaceRange may shift
    if (!replaceRange(target, replacement))
        return false;
    recomputeFindMatches();
    // Position the walk for the next F3/replace: the first match strictly
    // after the replaced range (offsets are the post-edit ones now).
    const auto it = std::upper_bound(matches_.begin(), matches_.end(),
                                     std::pair(target.startLine, target.startCol),
                                     [](const std::pair<int, int>& p, const MatchRange& m) {
                                         return p.first < m.startLine
                                             || (p.first == m.startLine && p.second < m.startCol);
                                     });
    if (it != matches_.end())
        currentMatch_ = int(it - matches_.begin()); // NOT reselected: the
                                                    // caret stays at the
                                                    // replacement end
    return true;
}

int CodeEditor::replaceAll(const QString& replacement)
{
    refreshFindMatches();
    const int n = static_cast<int>(matches_.size());
    if (n == 0)
        return 0;
    const MatchRange first = matches_.front(); // its line/col survive: the
                                               // walk below runs bottom-up
    const UndoMark pre = captureEditMark();
    buffer_.beginEditGroup(); // ONE undo group for the whole batch (D1 DoD)
    for (int i = n - 1; i >= 0; --i) {
        const MatchRange& m = matches_[i];
        const int start = buffer_.offsetOf(m.startLine, m.startCol);
        buffer_.removeRange(start, buffer_.offsetOf(m.endLine, m.endCol));
        if (!replacement.isEmpty())
            buffer_.insertAtOffset(start, replacement);
    }
    buffer_.endEditGroup();
    const auto atLc = buffer_.lineColAt(
        buffer_.offsetOf(first.startLine, first.startCol) + replacement.size());
    const TextPosition at{atLc.line, atLc.col};
    setSelection(at, at);
    reconcileUndoMarks(pre);
    recomputeFindMatches();
    return n;
}

void CodeEditor::showFindBar()
{
    if (!findBar_)
        findBar_ = new FindBar(this);
    static_cast<FindBar*>(findBar_)->open(false);
}

void CodeEditor::showReplaceBar()
{
    if (!findBar_)
        findBar_ = new FindBar(this);
    static_cast<FindBar*>(findBar_)->open(true);
}

void CodeEditor::hideFindBar()
{
    if (!findBar_)
        return;
    findBar_->hide();
    setFocus(Qt::OtherFocusReason); // the query and highlights persist
}

bool CodeEditor::findBarVisible() const
{
    return findBar_ && findBar_->isVisible();
}

void CodeEditor::clearFind()
{
    findText_.clear();
    findFlags_ = FindFlag::MatchCase; // back to the documented default
                                       // (the same value a fresh editor
                                       // carries); the bar's toggles re-sync
    matches_.clear();
    currentMatch_ = -1;
    findDirty_ = false;
    findPatternValid_ = true;
    if (findBar_)
        static_cast<FindBar*>(findBar_)->syncFromEditor(); // empties fields
    update();
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

    // M6 find/replace shortcuts via standard-key matching: Find (Ctrl+F),
    // Replace (Ctrl+H), FindNext (F3), FindPrevious (Shift+F3). They act
    // with the bar closed too (the query persists, Monaco parity); when
    // focus sits in the bar these keys are consumed by its own widgets
    // and never reach this switch. Ctrl+F must stay ahead of the Ctrl
    // catch-all below, which swallows every other Control combo.
    if (event->matches(QKeySequence::Find)) {
        showFindBar();
        finish();
        return;
    }
    if (event->matches(QKeySequence::Replace)) {
        showReplaceBar();
        finish();
        return;
    }
    if (event->matches(QKeySequence::FindNext)) {
        findNext();
        finish();
        return;
    }
    if (event->matches(QKeySequence::FindPrevious)) {
        findPrevious();
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

    // M13 fold commands, VS Code's chord table. NOTE: `ctrl` masks Alt/Ctrl/
    // Meta only — Shift is a SEPARATE bool here — so Ctrl+Shift+[ arrives as
    // ctrl==ControlModifier && shift==true, while Ctrl+Alt+[ is
    // ctrl==(Control|Alt) && !shift. Real X11 keyboards sprinkle Meta onto
    // Ctrl/Alt chords (NumLock, AltGr), so the tests below mask Meta out;
    // both bracket key spellings are accepted (Key_BraceLeft shows up on
    // layouts that shift the bracket, and Shift+[ IS [ on US keyboards
    // before Qt maps it — depending on the platform either key can arrive).
    const bool foldKey = key == Qt::Key_BracketLeft || key == Qt::Key_BraceLeft;
    const bool unfoldKey = key == Qt::Key_BracketRight || key == Qt::Key_BraceRight;
    const bool ctrlOnly = (ctrl & Qt::ControlModifier) && !(ctrl & Qt::AltModifier);
    const bool ctrlAlt = (ctrl & (Qt::ControlModifier | Qt::AltModifier))
                         == (Qt::ControlModifier | Qt::AltModifier);
    if (foldKey && ctrlOnly && shift) {
        foldLine(caret_.line); // no-op when not foldable; caret untouched
        finish();
        return;
    }
    if (unfoldKey && ctrlOnly && shift) {
        unfoldLine(caret_.line); // reveals this fold; inner folds stay as-is
        finish();
        return;
    }
    if (foldKey && ctrlAlt && !shift) {
        collapseAll();
        finish();
        return;
    }
    if (unfoldKey && ctrlAlt && !shift) {
        expandAll();
        finish();
        return;
    }

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
        // M13: the step walks VISIBILITY slots, not model lines — at a
        // collapsed fold's header Down lands on the line after the hidden
        // run (the caret jumps the whole fold for free; the merged-run
        // invariant guarantees the target is visible).
        if (!extraCarets_.isEmpty()) {
            const bool up = key == Qt::Key_Up;
            moveCarets(
                [this, up](const CaretSlot& s) {
                    TextPosition p = s.caret;
                    if (up) {
                        const int t = prevVisibleModelLine(p.line);
                        if (t < p.line)
                            p.line = t;
                        else
                            p.col = 0;
                    } else {
                        const int t = nextVisibleModelLine(p.line);
                        if (t > p.line)
                            p.line = t;
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
            const int t = prevVisibleModelLine(pos.line);
            if (t < pos.line)
                pos.line = t;
            else
                pos.col = 0;
        } else {
            const int t = nextVisibleModelLine(pos.line);
            if (t > pos.line)
                pos.line = t;
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
        // M13: the page jump is slot arithmetic (page = viewport rows), so
        // a PageDown inside the viewport of a collapsed fold leaps over
        // the hidden run — the walk only ever lands on visible lines.
        auto pageStep = [this](int line, int delta) {
            const int slot = std::clamp(slotOfModelLine(line) + delta, 0,
                                        std::max(0, visibleLineTotal() - 1));
            return modelLineForVisibleIndex(slot);
        };
        if (!extraCarets_.isEmpty()) {
            const int page = std::max(1, visibleLineCount() - 2);
            const int delta = key == Qt::Key_PageUp ? -page : page;
            moveCarets(
                [this, delta, pageStep](const CaretSlot& s) {
                    TextPosition p = s.caret;
                    p.line = pageStep(p.line, delta);
                    return p; // clampPos inside moveCarets pins the edges
                },
                shift);
            finish();
            return;
        }
        TextPosition pos = caret_;
        const int page = std::max(1, visibleLineCount() - 2);
        pos.line = pageStep(pos.line, (key == Qt::Key_PageUp) ? -page : page);
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
        double x = contentLeft() + xInLine(hitLine(caret_.line), caret_.col);
        if (!preedit_.isEmpty())
            x += QFontMetricsF(font_).horizontalAdvance(preedit_);
        // M13: the caret row lives in slot space (hidden runs shift the y).
        const int y = padding_ + slotOfModelLine(caret_.line) * lineHeight_ - vScroll_;
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
    // M7 ruler gesture FIRST (left = click/drag scroll, middle too: paste
    // in the chrome strip would surprise): the strip scrolls, the caret
    // never moves. The scrollbar child already swallows its own area.
    if (rulerEnabled_ && overviewRulerGeometry().contains(event->position().toPoint())
        && (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)) {
        rulerDrag_ = true;
        rulerScrollTo(int(event->position().y()));
        event->accept();
        return;
    }
    // M13 fold-zone hit, BEFORE any caret placement: a left click inside
    // the strip at the gutter's right edge toggles that row's fold and
    // NEVER moves the caret (VS Code behavior). Alt+click keeps its
    // multi-caret meaning everywhere, so it skips this handler; rows with
    // no fold marker (or left-gutter clicks) fall through to the regular
    // caret placement unchanged.
    if (event->button() == Qt::LeftButton && !(event->modifiers() & Qt::AltModifier)) {
        const int fz = foldZoneWidth();
        const QPoint pt = event->position().toPoint();
        if (fz > 0 && pt.x() >= contentLeft() - fz && pt.x() < contentLeft()) {
            if (const int line = modelLineForY(pt.y()); line >= 0 && toggleFold(line)) {
                event->accept();
                return;
            }
        }
    }
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
    // M7 ruler drag: continuous scroll while the press started in the
    // strip (before any caret-selection drag logic).
    if (rulerDrag_) {
        rulerScrollTo(int(event->position().y()));
        event->accept();
        return;
    }
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
    // M9 horizontal twin: past the text window's left/right edges, 3 cells
    // per event (the hBar singleStep). The right edge is the ruler chrome
    // boundary; positions are mapped back into the shifted window so the
    // caret target keeps following the pointer (positionAt rides hScroll_).
    const int rightEdge = width() - overviewRulerWidth();
    if (pos.x() < contentLeft()) {
        setHScroll(hScroll_ - 3 * int(std::ceil(cellWidth_)));
        pos.setX(contentLeft());
    } else if (pos.x() > rightEdge) {
        setHScroll(hScroll_ + 3 * int(std::ceil(cellWidth_)));
        pos.setX(rightEdge);
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
    if (rulerDrag_
        && (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)) {
        rulerDrag_ = false; // end the ruler scroll gesture
        event->accept();
        return;
    }
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
    const int vLines = event->angleDelta().y() / 120 * 3;
    // M9: trackpads deliver a real angle.x; Shift+vertical wheel is the
    // classic mouse-only h-scroll gesture (3 notches*3 lines == 3 cells
    // per notch matches the hBar singleStep). Vertical motion goes to the
    // horizontal axis only under Shift (then it does NOT also scroll v).
    const int hCols = event->angleDelta().x() / 120 * 3
                      + (event->modifiers() & Qt::ShiftModifier ? vLines : 0);
    if (hCols != 0)
        setHScroll(hScroll_ - hCols * int(std::ceil(cellWidth_)));
    if (!(event->modifiers() & Qt::ShiftModifier) && vLines != 0)
        setVScroll(vScroll_ - vLines * lineHeight_);
    event->accept();
}

void CodeEditor::resizeEvent(QResizeEvent*)
{
    updateScrollBarRange();
    if (findBar_) // the bar hugs the top-right corner (Monaco placement)
        static_cast<FindBar*>(findBar_)->place();
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
