// Smoke tests for the widget: keyboard edits and selection semantics must
// reach the model. Rendering itself is not asserted here (screenshot tests
// come later).

#include <QtTest>

#include "view/CodeEditor.hpp"

using qmn::view::CodeEditor;

class TestCodeEditor : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        editor = new CodeEditor();
        editor->resize(600, 400);
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

QTEST_MAIN(TestCodeEditor)
#include "tst_codeeditor.moc"
