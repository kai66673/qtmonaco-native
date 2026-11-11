#pragma once

// M11 (Phase E3): the QAccessible bridge. One custom QAccessibleInterface
// over CodeEditor, registered through a QAccessible::installFactory factory
// (installed once, from the CodeEditor constructor — a drop-in of
// Chromium-Monaco must be accessible without the app remembering to wire
// anything). Role EditableText + QAccessibleTextInterface +
// QAccessibleEditableTextInterface: screen readers (AT-SPI/Native) read the
// document, its caret and its selection, and navigate by char/word/line.
//
// Offset contract: every offset the interface speaks is a DOCUMENT UTF-16
// code-unit offset [start, end) INCLUDING the '\n' separators between lines
// — exactly the core TextBuffer frame (charCount/offsetOf/textRange), so
// the interface never keeps its own bookkeeping and can never drift from
// the model. (This matches Qt's own convention: its LineBoundary spans
// include the trailing newline.)
//
// Boundary queries (textBeforeOffset/textAfterOffset/textAtOffset) are
// implemented over the LINE MODEL (no whole-document materialization —
// the Qt default implementation copies the entire text() for every query,
// unacceptable on big files). Word spans are LINE-LOCAL: a word never
// crosses a '\n' (a code editor convention; the Qt default walks across
// lines). ParagraphBoundary == LineBoundary (one paragraph per line, the
// Qt default makes the same distinction-free choice). SentenceBoundary is
// delegated to the Qt default (it is genuinely rare for code, and the cost
// of the whole-document fallback is then paid only by that query).
// Boundary offsets follow the Qt convention: -1 means the document end,
// -2 means the current cursor position; a missing item returns an empty
// string with *start = *end = -1.
//
// Selection contract (Gate-1): ONE primary selection (selectionCount is
// 0 or 1) — the same selection the editor paints and what setSelection/
// addSelection write, so the read and write doors can never disagree. The
// D1 extra carets are an editor-level feature, deliberately invisible to
// AT here (multi-caret a11y is a post-v1 refinement; the write side has no
// per-slot semantics to map onto anyway).
//
// Event emission lives in CodeEditor itself (the constructor wires the
// editor's cursorMoved/selectionChanged signals and the core EditEvent
// observer into QAccessible::updateAccessibility, guarded by
// QAccessible::isActive() so an idle app pays ~one bool). TextRemoved
// events carry a POSITION only (the core EditEvent observer fires after
// the mutation, so the removed TEXT is not in its payload; ATs re-read,
// which is what the payload is optimized for anyway). The state queries are
// the testable contract in the offscreen suite — event DELIVERY goes to
// the platform bridge and is not observable without one.
//
// Shiboken contract: the binding wraps CodeEditor; this class is internal
// glue (never crosses the Python API boundary), so multiple inheritance is
// fine here — only the editor's own surface must stay wrappable.

#include <QAccessibleWidget>
#include <QAccessible>

namespace qmn::view {

class CodeEditor;

class CodeEditorAccessible final : public QAccessibleWidget,
                                   public QAccessibleTextInterface,
                                   public QAccessibleEditableTextInterface
{
public:
    explicit CodeEditorAccessible(CodeEditor* editor);

    // Registers the factory that answers queryAccessibleInterface for
    // CodeEditor objects. Idempotent (one static call, thread-safe init);
    // the CodeEditor constructor calls it, consumers never need to.
    static void install();

    // --- QAccessibleInterface ----------------------------------------
    void* interface_cast(QAccessible::InterfaceType type) override;
    QAccessible::Role role() const override { return QAccessible::EditableText; }
    QAccessible::State state() const override;
    // Value == the whole document text (Qt's multiline-editor convention,
    // the QAccessibleTextEdit/QLineEdit model). Name rides the widget's
    // accessibleName ("code editor", set in the editor ctor).
    QString text(QAccessible::Text t) const override;
    void setText(QAccessible::Text t, const QString& value) override;

    // --- QAccessibleTextInterface -------------------------------------
    // selection(): the PRIMARY selection only (sorted ends). False + (-1,-1)
    // when there is no selection or the index is out of range.
    void selection(int selectionIndex, int* startOffset, int* endOffset) const override;
    int selectionCount() const override;
    void addSelection(int startOffset, int endOffset) override; // == setSelection(0,..)
    void removeSelection(int selectionIndex) override;          // clears the primary
    void setSelection(int selectionIndex, int startOffset, int endOffset) override;
    int cursorPosition() const override;
    void setCursorPosition(int position) override; // moves the PRIMARY caret (collapses
                                                   // the selection + extra carets, the
                                                   // setPosition door semantics)
    QString text(int startOffset, int endOffset) const override; // document frame
    int characterCount() const override;
    // GLOBAL screen rect (the QAccessibleWidget rect convention) of the
    // glyph cell for the character AT offset (offset==charCount addresses
    // the caret position past the last char). Fold-aware: the M13 view map
    // decides the y, xForCol the x — the same geometry the caret paints at.
    QRect characterRect(int offset) const override;
    // Inverse of characterRect: GLOBAL point -> nearest document offset
    // (positionAt clamps gutter/ruler edges exactly like a click).
    int offsetAtPoint(const QPoint& point) const override;
    // Scrolls (unfolding hidden lines via the M13 reveal rule first) so
    // both ends land inside the viewport — the ensureCaretVisible math,
    // without moving the caret.
    void scrollToSubstring(int startIndex, int endIndex) override;
    // Empty role map (Gate-1: per-character syntax roles to AT are future
    // work); the reported span is the whole line containing offset.
    QString attributes(int offset, int* startOffset, int* endOffset) const override;
    // Line-model boundary walks (contract in the file header above).
    QString textBeforeOffset(int offset, QAccessible::TextBoundaryType boundaryType,
                             int* startOffset, int* endOffset) const override;
    QString textAfterOffset(int offset, QAccessible::TextBoundaryType boundaryType,
                            int* startOffset, int* endOffset) const override;
    QString textAtOffset(int offset, QAccessible::TextBoundaryType boundaryType,
                         int* startOffset, int* endOffset) const override;

    // --- QAccessibleEditableTextInterface ------------------------------
    // All three route through the editor's caret doors (setPosition +
    // insertTextAtCaret / deleteSelection), so caret normalization, undo
    // bracketing and the M19 read-only refusal behave EXACTLY as if the
    // user performed the edit — including the refusal: read-only mode
    // turns these into no-ops. Consequence: they move the primary caret
    // to the touched range (documented; there is no offset-addressed user
    // edit door that would keep it where it was).
    void deleteText(int startOffset, int endOffset) override;
    void insertText(int offset, const QString& text) override;
    void replaceText(int startOffset, int endOffset, const QString& text) override;

private:
    // Qt boundary-offset convention: -1 -> document end, -2 -> current
    // cursor position; every other value passes through.
    int resolveOffset_(int offset) const;
    // Line-local word walk helpers: a "word" run is [A-Za-z0-9_-ish] —
    // letter-or-number plus '_' (the code-editor word convention; see the
    // file header for the line-local rule).
    static bool isWordChar_(QChar ch);

    CodeEditor* editor_; // lifetime: the interface object is created and
                         // cached by QAccessible FOR that QObject, so the
                         // editor always outlives it (cache dies with the
                         // object's destroyed signal)
};

} // namespace qmn::view
