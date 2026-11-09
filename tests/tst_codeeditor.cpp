// Smoke tests for the widget: keyboard edits and selection semantics must
// reach the model. Rendering itself is not asserted here (screenshot tests
// come later).

#include <QtTest>

#include <QClipboard>

#include "view/CodeEditor.hpp"

using qmn::view::CodeEditor;

class TestCodeEditor : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        editor = new CodeEditor();
        editor->resize(600, 400);
        if (QClipboard* cb = QApplication::clipboard()) {
            cb->clear(QClipboard::Clipboard);
            cb->clear(QClipboard::Selection); // keep primary state isolated too
        }
    }
    void cleanup()
    {
        delete editor;
    }

    void typeAtStart();
    void typeAtEndViaEnd();
    void enterThenBackspaceMergeLines();
    void deleteForwardMergesLines();
    void arrowNavigationClamps();
    void mousePlacesCaret();
    void cellWidthMatchesGlyphEngineAdvance();

    void shiftSelectsToLineEnd();
    void typingReplacesSelection();
    void enterReplacesSelection();
    void backspaceDeletesSelectionOnly();
    void deleteRemovesSelectionOnly();
    void shiftHomeAndDownSelectAcrossLines();
    void shiftClickExtendsSelection();
    void mouseDragSelects();
    void mouseDragSelectsBackwards();
    void selectAllThenTypeReplacesAll();
    void escapeClearsSelection();
    void graphemeSafeBackspaceAndDelete();

    void copyThenPasteRoundTrip();
    void copyWithoutSelectionGrabsWholeLine();
    void copyLastLineHasNoTrailingNewline();
    void cutSelectionRemovesIt();
    void cutWithoutSelectionRemovesWholeLine();
    void cutLastLineTakesPreviousNewline();
    void cutSoleLineEmptiesDocument();
    void pasteReplacesSelection();
    void pasteNormalizesCRLF();
    void emptyClipboardPasteIsNoOp();
    void pasteKeepsSurrogatePairIntact();
    void clipboardAlternateKeyChains();
    void ctrlAltCopyDoesNotFireClipboard();
    void primarySelectionMirrorsAndClears();
    void middleClickPastesPrimarySelection();
    void middleClickFallsBackToClipboard();

private:
    CodeEditor* editor = nullptr;
};

void TestCodeEditor::typeAtStart()
{
    editor->setPlainText(QStringLiteral("ab"));
    QTest::keyClicks(editor, "cd");
    QCOMPARE(editor->toPlainText(), QStringLiteral("cdab"));
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 2);
}

void TestCodeEditor::typeAtEndViaEnd()
{
    editor->setPlainText(QStringLiteral("ab"));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClicks(editor, "cd");
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcd"));
    QCOMPARE(editor->position().col, 4);
}

void TestCodeEditor::enterThenBackspaceMergeLines()
{
    editor->setPlainText(QStringLiteral("ab"));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClick(editor, Qt::Key_Return);
    QTest::keyClicks(editor, "c");
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab\nc"));
    QCOMPARE(editor->position().line, 1);
    QCOMPARE(editor->position().col, 1);

    QTest::keyClick(editor, Qt::Key_Backspace); // deletes 'c'
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab\n"));
    QCOMPARE(editor->position().line, 1);
    QCOMPARE(editor->position().col, 0);

    QTest::keyClick(editor, Qt::Key_Backspace); // joins lines
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab"));
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 2);
    QCOMPARE(editor->lineCount(), 1);
}

void TestCodeEditor::deleteForwardMergesLines()
{
    editor->setPlainText(QStringLiteral("ab\ncde"));
    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Delete);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcde"));
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 2);
}

void TestCodeEditor::arrowNavigationClamps()
{
    editor->setPlainText(QStringLiteral("ab\ncde"));
    editor->setPosition({0, 2});

    QTest::keyClick(editor, Qt::Key_Right); // past EOL wraps to next line
    QCOMPARE(editor->position().line, 1);
    QCOMPARE(editor->position().col, 0);

    QTest::keyClick(editor, Qt::Key_Right);
    QCOMPARE(editor->position().col, 1);

    QTest::keyClick(editor, Qt::Key_Up); // col clamped to shorter line
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 1);

    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->position().col, 0);
    QTest::keyClick(editor, Qt::Key_End);
    QCOMPARE(editor->position().col, 2);

    QTest::keyClick(editor, Qt::Key_Left); // before line start wraps up
    QTest::keyClick(editor, Qt::Key_Left);
    QTest::keyClick(editor, Qt::Key_Left);
    QTest::keyClick(editor, Qt::Key_Left);
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 0);
    QTest::keyClick(editor, Qt::Key_Left); // stays at 0,0
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 0);
}

void TestCodeEditor::mousePlacesCaret()
{
    editor->setPlainText(QStringLiteral("abcdef\nxyz"));
    const int pad = editor->padding();
    const double cw = editor->cellWidth();
    const int lh = editor->lineHeight();

    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(pad + int(3 * cw + cw / 2), pad + lh / 2));
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 3);

    // far below the last line clamps to the last line
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier, QPoint(pad, pad + 100 * lh));
    QCOMPARE(editor->position().line, 1);
    // far right clamps to line length
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier, QPoint(pad + int(90 * cw), pad + lh / 2));
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 6);
}

void TestCodeEditor::cellWidthMatchesGlyphEngineAdvance()
{
    // Regression guard for caret drift: the model-side cell width must match
    // the cumulative advance the glyph engine uses, within half a pixel over
    // a full 120-column line.
    const int cols = 120;
    QFontMetricsF fm(editor->font());
    const double engineAdvance = fm.horizontalAdvance(QString(cols, QLatin1Char('0')));
    const double modelAdvance = cols * editor->cellWidth();
    QVERIFY2(qAbs(engineAdvance - modelAdvance) < 0.5,
             qPrintable(QStringLiteral("engine=%1 model=%2")
                            .arg(engineAdvance)
                            .arg(modelAdvance)));
}

void TestCodeEditor::shiftSelectsToLineEnd()
{
    editor->setPlainText(QStringLiteral("abcdef\nxyz"));
    editor->setPosition({0, 2});
    QVERIFY(!editor->hasSelection());

    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier);
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 6);
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("cdef"));
    QCOMPARE(editor->selectedLength(), 4);

    // Shift+Left shrinks from the active end; anchor stays.
    QTest::keyClick(editor, Qt::Key_Left, Qt::ShiftModifier);
    QCOMPARE(editor->selectedText(), QStringLiteral("cde"));
}

void TestCodeEditor::typingReplacesSelection()
{
    editor->setPlainText(QStringLiteral("abcdef"));
    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(editor->selectedText(), QStringLiteral("cd"));

    QTest::keyClicks(editor, "X");
    QCOMPARE(editor->toPlainText(), QStringLiteral("abXef"));
    QCOMPARE(editor->position().col, 3);
    QVERIFY(!editor->hasSelection());
}

void TestCodeEditor::enterReplacesSelection()
{
    editor->setPlainText(QStringLiteral("ab\ncd\nef"));
    editor->setPosition({0, 2}); // at EOL of line 0
    QTest::keyClick(editor, Qt::Key_Down, Qt::ShiftModifier); // select "\ncd"
    QCOMPARE(editor->selectedText(), QStringLiteral("\ncd"));

    QTest::keyClick(editor, Qt::Key_Return);
    // Replacing "\ncd" with a newline leaves "ab" + "\n" + "\n" + "ef".
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab\n\nef"));
    QCOMPARE(editor->position().line, 1);
    QCOMPARE(editor->position().col, 0);
}

void TestCodeEditor::backspaceDeletesSelectionOnly()
{
    editor->setPlainText(QStringLiteral("abcdef"));
    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier);
    QTest::keyClick(editor, Qt::Key_Backspace);
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab"));
    QCOMPARE(editor->position().col, 2);
    QVERIFY(!editor->hasSelection());
}

void TestCodeEditor::deleteRemovesSelectionOnly()
{
    editor->setPlainText(QStringLiteral("abcdef"));
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_Down, Qt::ShiftModifier); // whole document
    QTest::keyClick(editor, Qt::Key_Delete);
    QCOMPARE(editor->toPlainText(), QString());
    QCOMPARE(editor->lineCount(), 1);
    QVERIFY(!editor->hasSelection());
}

void TestCodeEditor::shiftHomeAndDownSelectAcrossLines()
{
    editor->setPlainText(QStringLiteral("ab\ncde\nfgh"));
    editor->setPosition({1, 3});

    QTest::keyClick(editor, Qt::Key_Home, Qt::ShiftModifier);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));
    QCOMPARE(editor->selectedText(), QStringLiteral("cde"));

    QTest::keyClick(editor, Qt::Key_Home, Qt::ShiftModifier); // already at start: no-op
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));
    QCOMPARE(editor->selectedText(), QStringLiteral("cde"));

    QTest::keyClick(editor, Qt::Key_Escape); // clears selection, keeps caret
    QVERIFY(!editor->hasSelection());
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));

    QTest::keyClick(editor, Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 0}));
    QCOMPARE(editor->selectedText(), QStringLiteral("cde\n"));

    // Ctrl+Shift+End extends the existing selection to the document end.
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier | Qt::ControlModifier);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 3}));
    QCOMPARE(editor->selectedText(), QStringLiteral("cde\nfgh"));
}

void TestCodeEditor::shiftClickExtendsSelection()
{
    editor->setPlainText(QStringLiteral("abcdef\nxyz"));
    editor->setPosition({0, 2});
    const int pad = editor->padding();
    const double cw = editor->cellWidth();
    const int lh = editor->lineHeight();

    QTest::mousePress(editor, Qt::LeftButton, Qt::ShiftModifier,
                      QPoint(pad + int(5 * cw + cw / 2), pad + lh / 2));
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 5);
    QCOMPARE(editor->selectedText(), QStringLiteral("cde"));

    // Plain click collapses at the new position.
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier, QPoint(pad, pad + lh));
    QVERIFY(!editor->hasSelection());
    QCOMPARE(editor->position().line, 1);
}

namespace {

// Synthesize a drag: press, move (with the button held per Qt's event state),
// release. QTest::mouseMove does not carry button state, so the events are
// constructed manually. NOTE: never wrap the send calls in Q_ASSERT — in
// release builds Q_ASSERT strips the whole expression, events included.
void dragSelect(QWidget* w, const QPoint& from, const QPoint& to)
{
    const QPointF f(from), t(to);
    QMouseEvent press(QEvent::MouseButtonPress, f, f, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, t, t, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, t, t, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QTEST_ASSERT(QApplication::sendEvent(w, &press));
    QTEST_ASSERT(QApplication::sendEvent(w, &move));
    QTEST_ASSERT(QApplication::sendEvent(w, &release));
}

} // namespace

namespace {

// X11 mirrors selections to the primary selection; Wayland and the
// offscreen platform do not (measured: supportsSelection() is false
// offscreen, so Selection-mode setText is silently dropped). Any test
// asserting Selection-mode behavior must gate on this, exactly like
// CodeEditor::updatePrimarySelection() does.
bool primarySelectionAvailable()
{
    QClipboard* cb = QApplication::clipboard();
    return cb && cb->supportsSelection();
}

} // namespace

void TestCodeEditor::mouseDragSelects()
{
    editor->setPlainText(QStringLiteral("abcdef\nxyz"));
    const int pad = editor->padding();
    const double cw = editor->cellWidth();
    const int lh = editor->lineHeight();

    dragSelect(editor, QPoint(pad + 1, pad + lh / 2),
               QPoint(pad + qRound(3 * cw + cw / 2), pad + lh / 2));
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("abc"));
    QCOMPARE(editor->position().col, 3); // caret at the drag end (active)

    // Drag across lines: press at col 4, release at col 1 of line 1.
    dragSelect(editor, QPoint(pad + qRound(4 * cw) + 1, pad + lh / 2),
               QPoint(pad + qRound(1.5 * cw), pad + lh + lh / 2));
    QCOMPARE(editor->selectedText(), QStringLiteral("ef\nx"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 1}));
}

void TestCodeEditor::mouseDragSelectsBackwards()
{
    editor->setPlainText(QStringLiteral("abcdef"));
    const int pad = editor->padding();
    const double cw = editor->cellWidth();
    const int lh = editor->lineHeight();

    dragSelect(editor, QPoint(pad + qRound(4 * cw) + 1, pad + lh / 2),
               QPoint(pad + qRound(1.5 * cw), pad + lh / 2));
    QCOMPARE(editor->position().col, 1); // caret at the drag end
    QCOMPARE(editor->selectedText(), QStringLiteral("bcd"));
}

void TestCodeEditor::selectAllThenTypeReplacesAll()
{
    editor->setPlainText(QStringLiteral("ab\ncd"));
    QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("ab\ncd"));
    QCOMPARE(editor->selectedLength(), 5);

    QTest::keyClicks(editor, "Z");
    QCOMPARE(editor->toPlainText(), QStringLiteral("Z"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));
    QVERIFY(!editor->hasSelection());
}

void TestCodeEditor::escapeClearsSelection()
{
    editor->setPlainText(QStringLiteral("abcdef"));
    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier);
    QVERIFY(editor->hasSelection());
    QTest::keyClick(editor, Qt::Key_Escape);
    QVERIFY(!editor->hasSelection());
    QCOMPARE(editor->position().col, 6);
    // Without a selection Escape propagates (ignored) — smoke: nothing breaks.
    QTest::keyClick(editor, Qt::Key_Escape);
    QVERIFY(!editor->hasSelection());
}

void TestCodeEditor::graphemeSafeBackspaceAndDelete()
{
    // 'a' + U+1F600 (surrogate pair, cols 1..2) + 'b'
    editor->setPlainText(QString::fromUtf8("a\U0001F600" "b"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));

    editor->setPosition({0, 3}); // right after the pair
    QTest::keyClick(editor, Qt::Key_Backspace); // removes the whole pair
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));

    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_Delete); // removes the pair again
    QCOMPARE(editor->toPlainText(), QStringLiteral("a"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));
}

// --- Clipboard (M1b) ----------------------------------------------------

void TestCodeEditor::copyThenPasteRoundTrip()
{
    editor->setPlainText(QStringLiteral("hello world"));
    editor->setPosition({0, 0});
    for (int i = 0; i < 5; ++i) // select "hello"
        QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(editor->selectedText(), QStringLiteral("hello"));
    QTest::keyClick(editor, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard),
             QStringLiteral("hello"));
    // Copy keeps the selection and caret where they are.
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 5}));

    editor->setPosition({0, 5}); // collapses; pasting inserts between words
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("hellohello world"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 10}));
    // Ctrl+V must not disturb the clipboard itself.
    QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard),
             QStringLiteral("hello"));
}

void TestCodeEditor::copyWithoutSelectionGrabsWholeLine()
{
    editor->setPlainText(QStringLiteral("one\ntwo\nthree"));
    editor->setPosition({1, 1}); // caret mid-line, no selection
    QTest::keyClick(editor, Qt::Key_C, Qt::ControlModifier);
    // Monaco semantics: the non-last line comes with its trailing newline.
    QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard),
             QStringLiteral("two\n"));
    QVERIFY(!editor->hasSelection());
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 1})); // copy moves nothing

    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("otwo\nne\ntwo\nthree"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0})); // after "two\n"
}

void TestCodeEditor::copyLastLineHasNoTrailingNewline()
{
    editor->setPlainText(QStringLiteral("a\nb"));
    editor->setPosition({1, 0});
    QTest::keyClick(editor, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard),
             QStringLiteral("b"));
}

void TestCodeEditor::cutSelectionRemovesIt()
{
    editor->setPlainText(QStringLiteral("hello world"));
    editor->setPosition({0, 6});
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier); // "world"
    QTest::keyClick(editor, Qt::Key_X, Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard),
             QStringLiteral("world"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("hello "));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 6}));
    QVERIFY(!editor->hasSelection());
}

void TestCodeEditor::cutWithoutSelectionRemovesWholeLine()
{
    editor->setPlainText(QStringLiteral("one\ntwo\nthree"));
    editor->setPosition({1, 2});
    QTest::keyClick(editor, Qt::Key_X, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("one\nthree"));
    QCOMPARE(editor->lineCount(), 2);
    QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard),
             QStringLiteral("two\n"));
    // Caret waits at the start of the slid-up line, ready for another Ctrl+X.
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));
}

void TestCodeEditor::cutLastLineTakesPreviousNewline()
{
    editor->setPlainText(QStringLiteral("a\nb"));
    editor->setPosition({1, 0});
    QTest::keyClick(editor, Qt::Key_X, Qt::ControlModifier);
    // The trailing line is removed together with the '\n' that preceded it,
    // so no phantom empty line survives (VS Code line-cut semantics); the
    // clipboard holds exactly what was deleted.
    QCOMPARE(editor->toPlainText(), QStringLiteral("a"));
    QCOMPARE(editor->lineCount(), 1);
    QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard),
             QStringLiteral("\nb"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));
}

void TestCodeEditor::cutSoleLineEmptiesDocument()
{
    editor->setPlainText(QStringLiteral("abc"));
    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_X, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QString());
    QCOMPARE(editor->lineCount(), 1); // always at least one line
    QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard),
             QStringLiteral("abc"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));
}

void TestCodeEditor::pasteReplacesSelection()
{
    editor->setPlainText(QStringLiteral("abcdef"));
    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier); // "cd"
    QApplication::clipboard()->setText(QStringLiteral("XY"), QClipboard::Clipboard);
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abXYef"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));
    QVERIFY(!editor->hasSelection());
}

void TestCodeEditor::pasteNormalizesCRLF()
{
    QClipboard* cb = QApplication::clipboard();
    cb->setText(QStringLiteral("a\r\nb\rc"), QClipboard::Clipboard);
    editor->setPlainText(QStringLiteral("x"));
    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    // The buffer only knows '\n' separators; both '\r\n' and lone '\r'
    // arriving from foreign clipboards are normalized on the way in. The
    // first pasted line joins the current one (no column-aware pasting yet).
    QCOMPARE(editor->toPlainText(), QStringLiteral("xa\nb\nc"));
    QCOMPARE(editor->lineCount(), 3);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 1}));
}

void TestCodeEditor::emptyClipboardPasteIsNoOp()
{
    editor->setPlainText(QStringLiteral("abc"));
    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier); // select "bc"
    // Clipboard was cleared in init(): an empty paste must not delete the
    // selection either — it simply does nothing.
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abc"));
    QVERIFY(editor->hasSelection());
}

void TestCodeEditor::pasteKeepsSurrogatePairIntact()
{
    const QString pairText = QString::fromUtf8("a\U0001F600" "b");
    QApplication::clipboard()->setText(pairText, QClipboard::Clipboard);
    editor->setPlainText(QString());
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), pairText); // no unit splitting anywhere
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4})); // 4 UTF-16 units

    editor->setPosition({0, 3}); // right after the pair, before the 'b'
    QTest::keyClick(editor, Qt::Key_Backspace); // removes the whole pair
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));
}

void TestCodeEditor::clipboardAlternateKeyChains()
{
    QClipboard* cb = QApplication::clipboard();
    editor->setPlainText(QStringLiteral("ab\ncd"));
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier); // "ab"
    QTest::keyClick(editor, Qt::Key_Insert, Qt::ControlModifier); // Ctrl+Ins = copy
    if (cb->text(QClipboard::Clipboard) != QStringLiteral("ab"))
        QSKIP("platform key bindings do not map Ctrl+Ins to copy");

    editor->setPosition({1, 2}); // end of the second line
    QTest::keyClick(editor, Qt::Key_Insert, Qt::ShiftModifier); // Shift+Ins = paste
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab\ncdab"));

    QTest::keyClick(editor, Qt::Key_Delete, Qt::ShiftModifier); // Shift+Del = cut
    // Last-line cut takes the preceding newline, as with Ctrl+X.
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab"));
    QCOMPARE(cb->text(QClipboard::Clipboard), QStringLiteral("\ncdab"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));
}

void TestCodeEditor::ctrlAltCopyDoesNotFireClipboard()
{
    // matches() checks the modifier set exactly: Ctrl+Alt+C is free for a
    // future app-level shortcut and must not copy, nor type 'c'.
    editor->setPlainText(QStringLiteral("abc"));
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_Down, Qt::ShiftModifier);
    QTest::keyClick(editor, Qt::Key_C, Qt::ControlModifier | Qt::AltModifier);
    QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard), QString());
    QCOMPARE(editor->toPlainText(), QStringLiteral("abc"));
    QVERIFY(editor->hasSelection()); // state untouched
}

void TestCodeEditor::primarySelectionMirrorsAndClears()
{
    if (!primarySelectionAvailable())
        QSKIP("platform has no X11-style primary selection");
    QClipboard* cb = QApplication::clipboard();
    editor->setPlainText(QStringLiteral("abcdef"));
    const int pad = editor->padding();
    const double cw = editor->cellWidth();
    const int lh = editor->lineHeight();

    dragSelect(editor, QPoint(pad + 1, pad + lh / 2),
               QPoint(pad + qRound(3 * cw + cw / 2), pad + lh / 2));
    QTRY_COMPARE(cb->text(QClipboard::Selection), QStringLiteral("abc"));

    QTest::keyClick(editor, Qt::Key_Escape); // selection gone: primary released
    QTRY_COMPARE(cb->text(QClipboard::Selection), QString());

    // Keyboard selection mirrors too.
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier);
    QTRY_COMPARE(cb->text(QClipboard::Selection), QStringLiteral("def"));
}

void TestCodeEditor::middleClickPastesPrimarySelection()
{
    if (!primarySelectionAvailable())
        QSKIP("platform has no X11-style primary selection");
    editor->setPlainText(QStringLiteral("abcdef"));
    QClipboard* cb = QApplication::clipboard();
    cb->setText(QStringLiteral("CB"), QClipboard::Clipboard);
    cb->setText(QStringLiteral("PR"), QClipboard::Selection);

    const int pad = editor->padding();
    const double cw = editor->cellWidth();
    const int lh = editor->lineHeight();
    QTest::mousePress(editor, Qt::MiddleButton, Qt::NoModifier,
                      QPoint(pad + int(2 * cw + cw / 2), pad + lh / 2));
    QCOMPARE(editor->toPlainText(), QStringLiteral("abPRcdef"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));
}

void TestCodeEditor::middleClickFallsBackToClipboard()
{
    // Runs on every platform: offscreen has no primary selection at all, and
    // X11 without one also hands back an empty string — in both cases the
    // documented fallback pastes the regular clipboard.
    editor->setPlainText(QStringLiteral("abcdef"));
    QClipboard* cb = QApplication::clipboard();
    cb->clear(QClipboard::Selection);
    cb->setText(QStringLiteral("ZZ"), QClipboard::Clipboard);

    const int pad = editor->padding();
    const double cw = editor->cellWidth();
    const int lh = editor->lineHeight();
    QTest::mousePress(editor, Qt::MiddleButton, Qt::NoModifier,
                      QPoint(pad + int(2 * cw + cw / 2), pad + lh / 2));
    QCOMPARE(editor->toPlainText(), QStringLiteral("abZZcdef"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));
}

QTEST_MAIN(TestCodeEditor)
#include "tst_codeeditor.moc"
