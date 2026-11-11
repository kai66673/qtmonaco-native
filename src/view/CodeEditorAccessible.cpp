#include "view/CodeEditorAccessible.hpp"

#include "view/CodeEditor.hpp"
#include "view/CodeEditorOptions.hpp" // the readOnly door (M19)

#include <algorithm>
#include <cmath>

namespace qmn::view {

namespace {

// The factory key is the CLASS NAME: QAccessible walks the object's
// metaObject chain (most-derived first) and hands every class name to every
// installed factory (installed factories run BEFORE the QtWidgets builtin
// fallback), so a key match plus a successful qobject_cast is the full
// identity check — CodeEditor SUBCLASSES hit the same entry because their
// chain passes the superclass name too. NOTE: Q_OBJECT in a namespace makes
// className() QUALIFIED ("qmn::view::CodeEditor"), so the key is taken from
// the metaObject itself, never spelled by hand.
QAccessibleInterface* codeEditorA11yFactory(const QString& key, QObject* object)
{
    if (key != QLatin1String(CodeEditor::staticMetaObject.className()))
        return nullptr; // not ours: the next factory decides (QWidget etc.)
    if (auto* editor = qobject_cast<CodeEditor*>(object))
        return new CodeEditorAccessible(editor);
    return nullptr;
}

} // namespace

CodeEditorAccessible::CodeEditorAccessible(CodeEditor* editor)
    : QAccessibleWidget(editor, QAccessible::EditableText), editor_(editor)
{
}

void CodeEditorAccessible::install()
{
    // ONE registration per process (function-local static init is
    // thread-safe since C++11); the CodeEditor ctor calls this, so the
    // factory is in place before any queryAccessibleInterface can ask.
    static const bool s_installed = [] {
        QAccessible::installFactory(&codeEditorA11yFactory);
        return true;
    }();
    Q_UNUSED(s_installed);
}

void* CodeEditorAccessible::interface_cast(QAccessible::InterfaceType type)
{
    if (type == QAccessible::TextInterface)
        return static_cast<QAccessibleTextInterface*>(this);
    if (type == QAccessible::EditableTextInterface)
        return static_cast<QAccessibleEditableTextInterface*>(this);
    return QAccessibleWidget::interface_cast(type);
}

QAccessible::State CodeEditorAccessible::state() const
{
    // The widget base already fills focusable/focused/enabled/visible…
    QAccessible::State st = QAccessibleWidget::state();
    st.selectable = true;
    st.selectableText = true;
    st.multiLine = true;
    const bool ro = editor_->options()->isReadOnly(); // M19 door (public)
    st.readOnly = ro;
    st.editable = !ro;
    return st;
}

QString CodeEditorAccessible::text(QAccessible::Text t) const
{
    if (t == QAccessible::Value)
        return editor_->toPlainText(); // whole-document Value convention
    return QAccessibleWidget::text(t); // Name = the editor's accessibleName
}

void CodeEditorAccessible::setText(QAccessible::Text t, const QString& value)
{
    if (t == QAccessible::Value) {
        editor_->setPlainText(value); // the PROGRAMMATIC door: alive even
        return;                       // in readOnly (the M19 contract)
    }
    QAccessibleWidget::setText(t, value); // Name/Description: properties
}

//
// --- QAccessibleTextInterface: selection + caret -------------------------
//

void CodeEditorAccessible::selection(int selectionIndex, int* startOffset,
                                     int* endOffset) const
{
    // NOTE the explicit ints: std::minmax over two call RESULTS yields a
    // pair of dangling references (the temporaries die with the statement),
    // so the values are materialized first.
    const int caretOff = editor_->buffer_.offsetOf(editor_->caret_.line, editor_->caret_.col);
    const int anchorOff = editor_->buffer_.offsetOf(editor_->anchor_.line, editor_->anchor_.col);
    if (selectionIndex != 0 || !editor_->hasSelection_) {
        *startOffset = *endOffset = -1; // Qt's no-such-selection frame
        return;
    }
    *startOffset = std::min(caretOff, anchorOff);
    *endOffset = std::max(caretOff, anchorOff);
}

int CodeEditorAccessible::selectionCount() const
{
    return editor_->hasSelection_ ? 1 : 0; // ONE primary selection (header)
}

void CodeEditorAccessible::addSelection(int startOffset, int endOffset)
{
    setSelection(0, startOffset, endOffset); // no per-slot AT semantics (D1)
}

void CodeEditorAccessible::removeSelection(int selectionIndex)
{
    if (selectionIndex == 0)
        editor_->clearSelection(); // public door: collapses to the caret
}

void CodeEditorAccessible::setSelection(int selectionIndex, int startOffset, int endOffset)
{
    if (selectionIndex != 0)
        return; // selection 0 is the primary; no extras exist here
    const int count = editor_->buffer_.charCount();
    const int a = std::clamp(std::min(startOffset, endOffset), 0, count);
    const int b = std::max(a, std::clamp(std::max(startOffset, endOffset), 0, count)); // caret
    const qmn::core::TextBuffer::LineCol sa = editor_->buffer_.lineColAt(a);
    const qmn::core::TextBuffer::LineCol sb = editor_->buffer_.lineColAt(b);
    // endOffset is the ACTIVE end (the caret lands on b, Qt convention)
    editor_->setSelection(TextPosition{sa.line, sa.col}, TextPosition{sb.line, sb.col});
}

int CodeEditorAccessible::cursorPosition() const
{
    return editor_->buffer_.offsetOf(editor_->caret_.line, editor_->caret_.col);
}

void CodeEditorAccessible::setCursorPosition(int position)
{
    const int clamped = std::clamp(position, 0, editor_->buffer_.charCount());
    const qmn::core::TextBuffer::LineCol p = editor_->buffer_.lineColAt(clamped);
    editor_->setPosition(TextPosition{p.line, p.col}); // collapses selection
}                                                       // + extra carets (D1)

QString CodeEditorAccessible::text(int startOffset, int endOffset) const
{
    return editor_->buffer_.textRange(startOffset, endOffset); // core clamps
}

int CodeEditorAccessible::characterCount() const
{
    return editor_->buffer_.charCount();
}

QRect CodeEditorAccessible::characterRect(int offset) const
{
    // Clamp, never fail: offset == charCount addresses the caret cell just
    // past the last char (lineColAt resolves it to the last line's EOL).
    const int clamped = std::clamp(offset, 0, editor_->buffer_.charCount());
    const qmn::core::TextBuffer::LineCol p = editor_->buffer_.lineColAt(clamped);
    // THE M13 view map decides where the line lives on screen (folded-away
    // lines clamp to their slot, exactly like the caret's own painting);
    // xForCol already carries contentLeft() and the live hScroll.
    const int slot = editor_->slotOfModelLine(p.line);
    const int x = int(std::floor(editor_->xForCol(p.line, p.col)));
    const int y = editor_->padding_ + slot * editor_->lineHeight_ - editor_->vScroll_;
    const QPoint global = editor_->mapToGlobal(QPoint(x, y)); // global frame
    const int w = qMax(1, int(std::ceil(editor_->cellWidth()))); // '\n'/EOL
    return QRect(global, QSize(w, editor_->lineHeight_));       // stays 1px
}

int CodeEditorAccessible::offsetAtPoint(const QPoint& point) const
{
    // point is GLOBAL (matches rect()/characterRect()); positionAt is the
    // click door — it clamps gutter/ruler edges to the nearest text edge.
    const TextPosition p = editor_->positionAt(editor_->mapFromGlobal(point));
    return editor_->buffer_.offsetOf(p.line, p.col);
}

void CodeEditorAccessible::scrollToSubstring(int startIndex, int endIndex)
{
    const int count = editor_->buffer_.charCount();
    const int a = std::clamp(std::min(startIndex, endIndex), 0, count);
    const int b = std::max(a, std::clamp(std::max(startIndex, endIndex), 0, count));
    const qmn::core::TextBuffer::LineCol pa = editor_->buffer_.lineColAt(a);
    const qmn::core::TextBuffer::LineCol pb = editor_->buffer_.lineColAt(b);
    // The M13 reveal rule first: a hidden (folded) line is unfolded before
    // anything addresses it — the same choke-point ensureCaretVisible uses.
    editor_->revealLine(pa.line);
    editor_->revealLine(pb.line);
    // The ensureCaretVisible vertical leg (flush-to-edge minimal scroll),
    // WITHOUT moving the caret: the substring range replaces the caret one.
    const int lh = editor_->lineHeight();
    const int topA = editor_->slotOfModelLine(pa.line) * lh;
    const int botB = editor_->slotOfModelLine(pb.line) * lh + lh;
    int target = editor_->vScroll_;
    if (topA - editor_->padding_ < editor_->vScroll_)
        target = topA - editor_->padding_;
    else if (botB + editor_->padding_ > editor_->vScroll_ + editor_->height())
        target = botB + editor_->padding_ - editor_->height();
    editor_->setVScroll(target); // clamped inside (never scrolls past content)
}

QString CodeEditorAccessible::attributes(int offset, int* startOffset, int* endOffset) const
{
    // Gate-1: no per-character AT role map yet (syntax roles stay paint-
    // only). Honest empty role map, span = the whole line (sans the '\n').
    const int count = editor_->buffer_.charCount();
    if (count == 0) {
        *startOffset = *endOffset = 0;
        return QString();
    }
    const int clamped = std::clamp(offset, 0, count - 1);
    const qmn::core::TextBuffer::LineCol p = editor_->buffer_.lineColAt(clamped);
    *startOffset = editor_->buffer_.offsetOfLine(p.line);
    *endOffset = *startOffset + editor_->buffer_.lineText(p.line).size();
    return QString();
}

//
// --- boundary walks over the LINE MODEL ----------------------------------
//

int CodeEditorAccessible::resolveOffset_(int offset) const
{
    // The Qt boundary convention (documented in the interface docs):
    // -1 == the document end, -2 == the current cursor position.
    if (offset == -1)
        return editor_->buffer_.charCount();
    if (offset == -2)
        return cursorPosition();
    return offset;
}

bool CodeEditorAccessible::isWordChar_(QChar ch)
{
    return ch.isLetterOrNumber() || ch == QLatin1Char('_'); // code words
}

namespace {

// Every "no such item" answer is Qt's frame: empty string, *s = *e = -1.
QString fail_2(int* startOffset, int* endOffset)
{
    *startOffset = *endOffset = -1;
    return QString();
}

QString span_(const qmn::core::TextBuffer& buf, int start, int end)
{
    return buf.textRange(start, end);
}

} // namespace

QString CodeEditorAccessible::textBeforeOffset(int offset, QAccessible::TextBoundaryType boundaryType,
                                               int* startOffset, int* endOffset) const
{
    offset = resolveOffset_(offset);
    const qmn::core::TextBuffer& buf = editor_->buffer_;
    const int count = buf.charCount();

    switch (boundaryType) {
    case QAccessible::CharBoundary:
        if (offset <= 0 || offset > count)
            return fail_2(startOffset, endOffset);
        *startOffset = offset - 1;
        *endOffset = offset;
        return span_(buf, *startOffset, *endOffset);
    case QAccessible::LineBoundary:
    case QAccessible::ParagraphBoundary: { // one paragraph == one line
        if (offset <= 0 || offset > count)
            return fail_2(startOffset, endOffset);
        // THE QT DEFAULT'S EXACT FRAME (line-by-line verified against the
        // 6.10 textLineBoundary helper): the span is the line terminated by
        // the LAST '\n' strictly before offset, INCLUDING that '\n' — so an
        // offset sitting ON a '\n', or at the start of the next line, both
        // still resolve to the line ABOVE it; the first line has nothing
        // "before". lineColAt puts an offset on a '\n' at its line's EOL,
        // which is exactly where the walk to line-1 starts.
        const int line = buf.lineColAt(offset).line;
        if (line == 0)
            return fail_2(startOffset, endOffset);
        *startOffset = buf.offsetOfLine(line - 1);
        *endOffset = buf.offsetOfLine(line); // == prev len + its '\n'
        return span_(buf, *startOffset, *endOffset);
    }
    case QAccessible::WordBoundary: {
        if (offset <= 0 || offset > count)
            return fail_2(startOffset, endOffset);
        // LINE-LOCAL word walk (header contract).
        const qmn::core::TextBuffer::LineCol p = buf.lineColAt(offset);
        const QString line = buf.lineText(p.line);
        int i = p.col - 1;
        if (i >= 0 && isWordChar_(line[i])) { // mid-word (or just past it):
            int s = i;                        // the span ends AT offset
            while (s > 0 && isWordChar_(line[s - 1]))
                --s;
            *startOffset = buf.offsetOf(p.line, s);
            *endOffset = buf.offsetOf(p.line, p.col);
            return span_(buf, *startOffset, *endOffset);
        }
        while (i >= 0 && !isWordChar_(line[i])) // jump the gap, take the
            --i;                                // whole previous word
        if (i < 0)
            return fail_2(startOffset, endOffset);
        int s = i;
        while (s > 0 && isWordChar_(line[s - 1]))
            --s;
        *startOffset = buf.offsetOf(p.line, s);
        *endOffset = buf.offsetOf(p.line, i + 1);
        return span_(buf, *startOffset, *endOffset);
    }
    case QAccessible::NoBoundary: // the whole prefix
        if (offset <= 0 || offset > count)
            return fail_2(startOffset, endOffset);
        *startOffset = 0;
        *endOffset = offset;
        return span_(buf, *startOffset, *endOffset);
    case QAccessible::SentenceBoundary:
        // Rare for code: pay the Qt default's whole-document walk (it is
        // correct over our text()/characterCount() — just not O(line)).
        return QAccessibleTextInterface::textBeforeOffset(offset, boundaryType,
                                                          startOffset, endOffset);
    }
    return fail_2(startOffset, endOffset); // unreachable today; forward-safe
}

QString CodeEditorAccessible::textAfterOffset(int offset, QAccessible::TextBoundaryType boundaryType,
                                              int* startOffset, int* endOffset) const
{
    offset = resolveOffset_(offset);
    const qmn::core::TextBuffer& buf = editor_->buffer_;
    const int count = buf.charCount();

    switch (boundaryType) {
    case QAccessible::CharBoundary:
        if (offset < 0 || offset >= count)
            return fail_2(startOffset, endOffset);
        *startOffset = offset;
        *endOffset = offset + 1;
        return span_(buf, *startOffset, *endOffset);
    case QAccessible::LineBoundary:
    case QAccessible::ParagraphBoundary: {
        // The line AFTER the one containing offset (the Qt default jumps
        // over the current line's remainder too); nothing after the last.
        if (offset < 0 || offset >= count)
            return fail_2(startOffset, endOffset);
        const int line = buf.lineColAt(offset).line;
        if (line + 1 >= buf.lineCount())
            return fail_2(startOffset, endOffset);
        *startOffset = buf.offsetOfLine(line + 1);
        *endOffset = *startOffset + buf.lineText(line + 1).size()
                     + (line + 2 < buf.lineCount() ? 1 : 0); // + the '\n'
        return span_(buf, *startOffset, *endOffset);
    }
    case QAccessible::WordBoundary: {
        if (offset < 0 || offset >= count)
            return fail_2(startOffset, endOffset);
        const qmn::core::TextBuffer::LineCol p = buf.lineColAt(offset);
        const QString line = buf.lineText(p.line);
        int i = p.col;
        if (i < line.size() && isWordChar_(line[i])) { // mid-word: the
            int e = i;                                 // remainder from here
            while (e < line.size() && isWordChar_(line[e]))
                ++e;
            *startOffset = buf.offsetOf(p.line, i);
            *endOffset = buf.offsetOf(p.line, e);
            return span_(buf, *startOffset, *endOffset);
        }
        while (i < line.size() && !isWordChar_(line[i])) // jump the gap
            ++i;
        if (i >= line.size())
            return fail_2(startOffset, endOffset); // LINE-LOCAL: a '\n' ends
        int e = i;                                 // the hunt (no cross-line
        while (e < line.size() && isWordChar_(line[e]))  // word pull)
            ++e;
        *startOffset = buf.offsetOf(p.line, i);
        *endOffset = buf.offsetOf(p.line, e);
        return span_(buf, *startOffset, *endOffset);
    }
    case QAccessible::NoBoundary: // the whole suffix
        if (offset < 0 || offset >= count)
            return fail_2(startOffset, endOffset);
        *startOffset = offset;
        *endOffset = count;
        return span_(buf, *startOffset, *endOffset);
    case QAccessible::SentenceBoundary:
        return QAccessibleTextInterface::textAfterOffset(offset, boundaryType,
                                                         startOffset, endOffset);
    }
    return fail_2(startOffset, endOffset);
}

QString CodeEditorAccessible::textAtOffset(int offset, QAccessible::TextBoundaryType boundaryType,
                                           int* startOffset, int* endOffset) const
{
    offset = resolveOffset_(offset);
    const qmn::core::TextBuffer& buf = editor_->buffer_;
    const int count = buf.charCount();

    switch (boundaryType) {
    case QAccessible::CharBoundary:
        // The char STARTING at offset; past the last char there is none
        // (exactly the Qt default's frame, offset == count included).
        if (count == 0 || offset < 0 || offset >= count)
            return fail_2(startOffset, endOffset);
        *startOffset = offset;
        *endOffset = offset + 1;
        return span_(buf, *startOffset, *endOffset);
    case QAccessible::LineBoundary:
    case QAccessible::ParagraphBoundary: {
        if (count == 0 || offset < 0 || offset > count)
            return fail_2(startOffset, endOffset);
        const int line = buf.lineColAt(offset).line; // offset at a '\n'
        *startOffset = buf.offsetOfLine(line);       // resolves before it
        *endOffset = *startOffset + buf.lineText(line).size()
                     + (line + 1 < buf.lineCount() ? 1 : 0); // + the '\n'
        return span_(buf, *startOffset, *endOffset);
    }
    case QAccessible::WordBoundary: {
        if (count == 0 || offset < 0 || offset > count)
            return fail_2(startOffset, endOffset);
        // The word STRADDLING offset: the char before or at offset is a
        // word char (an offset sitting inside a '\n' straddles the line's
        // last word exactly like the caret does at EOL).
        const qmn::core::TextBuffer::LineCol p = buf.lineColAt(offset);
        const QString line = buf.lineText(p.line);
        const bool prevWord = offset > 0 && isWordChar_(buf.textRange(offset - 1, offset).at(0));
        const bool atWord = offset < count && isWordChar_(buf.textRange(offset, offset + 1).at(0));
        if (!prevWord && !atWord)
            return fail_2(startOffset, endOffset);
        int s = p.col, e = p.col; // stay inside the line (line-local rule)
        while (s > 0 && isWordChar_(line[s - 1]))
            --s;
        while (e < line.size() && isWordChar_(line[e]))
            ++e;
        if (s == e)
            return fail_2(startOffset, endOffset); // e.g. the '\n' between
        *startOffset = buf.offsetOf(p.line, s);    // two lines: the straddle
        *endOffset = buf.offsetOf(p.line, e);      // lives on ONE side only
        return span_(buf, *startOffset, *endOffset);
    }
    case QAccessible::NoBoundary: // the whole document IS the item
        if (count == 0)
            return fail_2(startOffset, endOffset);
        *startOffset = 0;
        *endOffset = count;
        return span_(buf, *startOffset, *endOffset);
    case QAccessible::SentenceBoundary:
        return QAccessibleTextInterface::textAtOffset(offset, boundaryType,
                                                      startOffset, endOffset);
    }
    return fail_2(startOffset, endOffset);
}

//
// --- QAccessibleEditableTextInterface -------------------------------------
//

void CodeEditorAccessible::insertText(int offset, const QString& text)
{
    if (text.isEmpty() || editor_->options()->isReadOnly())
        return; // M19: the editable door IS a user-edit door
    const int clamped = std::clamp(offset, 0, editor_->buffer_.charCount());
    const qmn::core::TextBuffer::LineCol p = editor_->buffer_.lineColAt(clamped);
    editor_->setPosition(TextPosition{p.line, p.col}); // the caret travels
    editor_->insertTextAtCaret(text);                   // to the edit (header)
}

void CodeEditorAccessible::deleteText(int startOffset, int endOffset)
{
    if (editor_->options()->isReadOnly())
        return;
    const int count = editor_->buffer_.charCount();
    const int a = std::clamp(std::min(startOffset, endOffset), 0, count);
    const int b = std::max(a, std::clamp(std::max(startOffset, endOffset), 0, count));
    if (a == b)
        return;
    const qmn::core::TextBuffer::LineCol sa = editor_->buffer_.lineColAt(a);
    const qmn::core::TextBuffer::LineCol sb = editor_->buffer_.lineColAt(b);
    editor_->setSelection(TextPosition{sa.line, sa.col}, TextPosition{sb.line, sb.col});
    editor_->deleteSelection(); // surrogate-safe view-level delete
}

void CodeEditorAccessible::replaceText(int startOffset, int endOffset, const QString& text)
{
    if (editor_->options()->isReadOnly())
        return;
    if (text.isEmpty()) {
        deleteText(startOffset, endOffset); // empty replacement == delete
        return;
    }
    const int count = editor_->buffer_.charCount();
    const int a = std::clamp(std::min(startOffset, endOffset), 0, count);
    const int b = std::max(a, std::clamp(std::max(startOffset, endOffset), 0, count));
    const qmn::core::TextBuffer::LineCol sa = editor_->buffer_.lineColAt(a);
    const qmn::core::TextBuffer::LineCol sb = editor_->buffer_.lineColAt(b);
    editor_->setSelection(TextPosition{sa.line, sa.col}, TextPosition{sb.line, sb.col});
    editor_->insertTextAtCaret(text); // type-over-selection replaces (M2)
}

} // namespace qmn::view
