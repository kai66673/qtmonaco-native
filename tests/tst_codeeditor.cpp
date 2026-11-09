// Smoke tests for the widget: keyboard edits and selection semantics must
// reach the model. Rendering itself is not asserted here (screenshot tests
// come later).

#include <QtTest>

#include <QClipboard>
#include <QImage>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>

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
    void caretXFollowsClusterAdvances();
    void caretXPinnedAcrossUnrelatedEdits();
    void mouseClickSnapsToClusterEdges();
    void caretPixelTracksLayoutNotGrid();
    void emptyLineGeometry();
    void textGlyphsShareCaretBands();

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

    void graphemeArrowKeysMoveOneCluster();
    void graphemeBackspaceCombiningMarks();
    void graphemeDeleteMovesWholeFamilyAndFlag();
    void caretInsideClusterSnapsWithoutOrphans();
    void wordNavigationRightSteps();
    void wordNavigationLeftSteps();
    void wordNavigationCrossesLines();
    void ctrlDeleteWordRight();
    void ctrlDeleteNeverSplitsClusters();

    void imeCommitInsertsText();
    void imePreeditStaysOutOfBufferUntilCommit();
    void imeQueriesTrackCaretAndPreedit();

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

void TestCodeEditor::caretXFollowsClusterAdvances()
{
    // The caret-drift regression guard: xForCol must follow glyph-engine
    // cluster advances, keep EXACT col*cellWidth positions on plain ASCII,
    // and snap mid-cluster columns to a cluster edge (engine out-param).
    const QString family = QString::fromUtf8("\U0001F468\u200D\U0001F469\u200D\U0001F467");
    editor->setPlainText(QStringLiteral("x") + family + QStringLiteral("y")
                         + QStringLiteral("\n0123456789"));
    const double cw = editor->cellWidth();
    const int pad = editor->padding();
    QFontMetricsF fm(editor->font());

    // ASCII line: identical to the grid model down to the last epsilon.
    for (int c = 0; c <= 10; ++c)
        QVERIFY2(qAbs(editor->xForCol(1, c) - (pad + c * cw)) < 1e-9,
                 qPrintable(QStringLiteral("col %1: %2 vs %3")
                                .arg(c).arg(editor->xForCol(1, c)).arg(pad + c * cw)));

    // The family spans units 1..9 but occupies the advance of ONE cluster:
    // ~1.8 cells on the measured font, never the 8 cells the grid claimed.
    const double famStart = editor->xForCol(0, 1);
    const double famEnd = editor->xForCol(0, 9);
    const double engineFamily = fm.horizontalAdvance(family);
    QVERIFY2(qAbs((famEnd - famStart) - engineFamily) < 0.5,
             qPrintable(QStringLiteral("cluster span=%1 engine=%2")
                            .arg(famEnd - famStart).arg(engineFamily)));
    QVERIFY(famEnd - famStart < 4.0 * cw); // grid said 8 cells; drift is dead

    // Mid-cluster caret columns all report the same edge position.
    for (int c = 2; c <= 8; ++c)
        QVERIFY2(qAbs(editor->xForCol(0, c) - famEnd) < 1e-9,
                 qPrintable(QStringLiteral("mid-cluster col %1 drifted").arg(c)));

    // 'y' after the family: one real cell further, not nine grid cells.
    QVERIFY(qAbs(editor->xForCol(0, 10) - (famEnd + cw)) < 1e-9);
    // xForCol clamps past the line end (phantom cells are paint-only).
    QCOMPARE(editor->xForCol(0, 99), editor->xForCol(0, 10));
}

void TestCodeEditor::caretXPinnedAcrossUnrelatedEdits()
{
    // The layout cache keys on (line, text): an edit elsewhere must not
    // re-shape the geometry of untouched lines, and an edit on the line
    // itself must shift only what precedes it.
    const QString family = QString::fromUtf8("\U0001F468\u200D\U0001F469\u200D\U0001F467");
    editor->setPlainText(QStringLiteral("x") + family + QStringLiteral("y")
                         + QStringLiteral("\nsecond"));
    const double famEnd = editor->xForCol(0, 9);
    const double eol = editor->xForCol(0, 10);

    editor->setPosition({1, 0});
    QTest::keyClick(editor, Qt::Key_Q); // insert on the OTHER line
    QCOMPARE(editor->xForCol(0, 9), famEnd); // bit-identical: same text, no reflow
    QCOMPARE(editor->xForCol(0, 10), eol);

    const double cw = editor->cellWidth();
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_A); // 'A' enters the emoji line's front
    // Everything after the insert slides right by exactly one real cell.
    QVERIFY(qAbs(editor->xForCol(0, 10) - famEnd - cw) < 1e-9);
    QVERIFY(qAbs(editor->xForCol(0, 11) - eol - cw) < 1e-9);
}

void TestCodeEditor::mouseClickSnapsToClusterEdges()
{
    // Hit-testing keeps floor semantics (click inside a cell -> that cell's
    // start), and inside a wide cluster x floors to the cluster start.
    const QString family = QString::fromUtf8("\U0001F468\u200D\U0001F469\u200D\U0001F467");
    editor->setPlainText(QStringLiteral("x") + family + QStringLiteral("y"));
    const int pad = editor->padding();
    const int lh = editor->lineHeight();
    const double cw = editor->cellWidth();
    const double famStart = editor->xForCol(0, 1);
    const double famEnd = editor->xForCol(0, 9);
    const int y = pad + lh / 2;

    auto clickAt = [&](double x) {
        QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier, QPoint(qRound(x), y));
        return editor->position();
    };

    // Middle of the family ink -> the cluster's LEFT edge (floor, like every
    // editor does inside a cell).
    QCOMPARE(clickAt((famStart + famEnd) / 2.0).col, 1);
    // One pixel short of the cluster end still floors to the cluster start...
    QCOMPARE(clickAt(famEnd - 1.0).col, 1);
    // ...one pixel past it is the next position.
    QCOMPARE(clickAt(famEnd + 1.0).col, 9);
    // 'y' occupies one real cell after the cluster; its middle is col 9.
    QCOMPARE(clickAt(famEnd + cw / 2.0).col, 9);
    // Way past the line end clamps to EOL (col 10), as the grid model did.
    QCOMPARE(clickAt(famEnd + 500.0 * cw).col, 10);
    QCOMPARE(clickAt(-100.0).col, 0);

    // And the caret placed mid-cluster by a click then Backspace deletes the
    // WHOLE cluster (view-level cluster safety, M2 rule, unchanged):
    QCOMPARE(clickAt((famStart + famEnd) / 2.0).col, 1);
    QTest::keyClick(editor, Qt::Key_Right); // one whole cluster over the family
    QCOMPARE(editor->position().col, 9);
}

void TestCodeEditor::caretPixelTracksLayoutNotGrid()
{
    // Numeric pixel assertion (no vision): render the widget, scan for the
    // solid caret fill, and require it at the layout position - not the old
    // col*cellWidth grid position, which sat ~50px away on this very line.
    const QString family = QString::fromUtf8("\U0001F468\u200D\U0001F469\u200D\U0001F467");
    editor->setPlainText(QStringLiteral("x") + family + QStringLiteral("y"));
    editor->setPosition({0, 10}); // EOL: no glyph overlaps the caret block
    const int lh = editor->lineHeight();
    const int pad = editor->padding();
    const double gridX = pad + 10 * editor->cellWidth(); // the BUGGY position
    const int layoutX = qRound(editor->xForCol(0, 10)); // what we draw with

    QImage img = editor->grab().toImage();
    const QColor textCol = editor->palette().color(QPalette::Text);
    QVector<int> solidCols;
    for (int x = 0; x < img.width(); ++x) {
        // Longest vertical run of exact Text color in the first line's band.
        // The caret is a 2px wide, line-height-tall solid fill; glyph
        // columns antialias and never reach the full line height.
        int run = 0, best = 0;
        for (int y = qMax(0, pad - 2); y <= pad + lh + 2; ++y) {
            if (img.pixelColor(x, y) == textCol) {
                ++run;
                best = qMax(best, run);
            } else {
                run = 0;
            }
        }
        if (best >= lh - 1)
            solidCols.append(x);
    }
    QVERIFY2(!solidCols.isEmpty(), "no solid caret column found in render");
    // The caret is 2px wide, so solid columns come as a single tight group
    // (glyph strokes below the line-height threshold stay excluded).
    for (int x : solidCols)
        QVERIFY2(qAbs(x - layoutX) <= 3,
                 qPrintable(QStringLiteral("solid column %1 far from layout caret x=%2")
                                .arg(x).arg(layoutX)));
    // Anti-drift tripwire: the old grid would have put the caret far right
    // (emoji advance << 8 code-unit cells); the render must NOT be there.
    QVERIFY(qAbs(layoutX - qRound(gridX)) > 20);
}

void TestCodeEditor::emptyLineGeometry()
{
    // The cache never hands out a dead QTextLine (measured: touching one
    // SEGFAULTS); empty lines resolve at the line start with phantom-cell
    // extrapolation only for xForCol(== 0-length lines: any col clamps).
    editor->setPlainText(QStringLiteral("abc\n\ndef"));
    const int pad = editor->padding();
    const int lh = editor->lineHeight();
    const double cw = editor->cellWidth();

    QVERIFY(qAbs(editor->xForCol(1, 0) - pad) < 1e-12);
    QCOMPARE(editor->xForCol(1, 5), editor->xForCol(1, 0)); // clamped to EOL
    QCOMPARE(editor->xForCol(99, 3), editor->xForCol(0, 0)); // bad line -> line start x

    // A click anywhere on an empty line lands at its start.
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(pad + int(30 * cw), pad + lh + lh / 2));
    QCOMPARE(editor->position().line, 1);
    QCOMPARE(editor->position().col, 0);
}

void TestCodeEditor::textGlyphsShareCaretBands()
{
    // Numeric band tripwire (no vision): the glyph ink of every line must
    // land INSIDE that line's band [yTop, yTop+lh). A regression had
    // QTextLine::draw's pos treated as a baseline anchor, but the raster
    // engine adds its own line.y()+ascent() on top - each text line then
    // rendered ~one line-height below its caret/selection band, leaving a
    // fake "empty line" at the top. Lowercase-only glyphs keep the vertical
    // extent unambiguous (no ascender/cap-height font dependency), and the
    // caret parks on the last line at col 1 so its solid block stays out of
    // the scanned x window.
    editor->setPlainText(QStringLiteral("x\ny\nz\nw\nv"));
    editor->setPosition({4, 1});
    const int pad = editor->padding();
    const int lh = editor->lineHeight();
    const QImage img = editor->grab().toImage();
    const QColor base = editor->palette().color(QPalette::Base);
    const int x1 = pad + int(editor->cellWidth()) - 1; // exclude caret cols

    for (int i = 0; i < 5; ++i) {
        const int yTop = pad + i * lh;
        int inBand = 0, below = 0;
        for (int x = 0; x <= x1 && x < img.width(); ++x) {
            for (int y = yTop; y < yTop + lh && y < img.height(); ++y)
                if (img.pixelColor(x, y) != base) { ++inBand; break; }
            // First two rows of the NEXT band: fixed geometry cannot have
            // ink this early (lowercase tops start ~x-height below the band
            // top); the shifted-draw bug lands its own line's ink here.
            for (int y = yTop + lh; y <= yTop + lh + 2 && y < img.height(); ++y)
                if (img.pixelColor(x, y) != base) { ++below; break; }
        }
        QVERIFY2(inBand >= 4, qPrintable(QStringLiteral("line %1 has ink %2")
                                             .arg(i).arg(inBand)));
        QVERIFY2(below == 0, qPrintable(QStringLiteral("line %1 ink spills %2 cols "
                                                       "into the next band")
                                            .arg(i).arg(below)));
    }
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

// --- M2 graphemes and word motion --------------------------------------

void TestCodeEditor::graphemeArrowKeysMoveOneCluster()
{
    // x | 👨‍👩‍👧 (ZWJ family, 8 units) | y | 🇺🇸 (RI flag, 4 units)
    // | e+U+0301 (combining, 2 units) = 16 UTF-16 units, 5 clusters.
    const QString text = QString::fromUtf8(
        "x\U0001F468\u200D\U0001F469\u200D\U0001F467"
        "y\U0001F1FA\U0001F1F8" "e\u0301");
    editor->setPlainText(text);
    QCOMPARE(editor->toPlainText().size(), qsizetype(16));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));

    const int rightStops[] = {1, 9, 10, 14, 16};
    for (int col : rightStops) {
        QTest::keyClick(editor, Qt::Key_Right);
        QCOMPARE(editor->position(), (qmn::view::TextPosition{0, col}));
    }
    QTest::keyClick(editor, Qt::Key_Right); // at end: stays
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 16}));

    const int leftStops[] = {14, 10, 9, 1, 0};
    for (int col : leftStops) {
        QTest::keyClick(editor, Qt::Key_Left);
        QCOMPARE(editor->position(), (qmn::view::TextPosition{0, col}));
    }
}

void TestCodeEditor::graphemeBackspaceCombiningMarks()
{
    // "e + U+0301" then 'x': three clusters worth of backspace, plus the
    // mid-cluster caret case (only reachable via setPlainText/setPosition).
    editor->setPlainText(QString::fromUtf8("e\u0301x"));
    editor->setPosition({0, 3});

    QTest::keyClick(editor, Qt::Key_Backspace); // removes 'x' only
    QCOMPARE(editor->toPlainText(), QString::fromUtf8("e\u0301"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));

    QTest::keyClick(editor, Qt::Key_Backspace); // removes the whole e+mark cluster
    QCOMPARE(editor->toPlainText(), QString()); // nothing left, no dangling mark
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));

    editor->setPlainText(QString::fromUtf8("e\u0301x"));
    editor->setPosition({0, 1}); // INSIDE the cluster (between base and mark)
    QTest::keyClick(editor, Qt::Key_Backspace); // still removes the whole cluster
    QCOMPARE(editor->toPlainText(), QStringLiteral("x")); // no dangling mark
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));
}

void TestCodeEditor::graphemeDeleteMovesWholeFamilyAndFlag()
{
    const QString family = QString::fromUtf8("\U0001F468\u200D\U0001F469\u200D\U0001F467");
    const QString flag = QString::fromUtf8("\U0001F1FA\U0001F1F8");
    editor->setPlainText(family + "a" + flag);
    QCOMPARE(editor->toPlainText().size(), qsizetype(13));

    QTest::keyClick(editor, Qt::Key_Delete); // whole family in one press
    QCOMPARE(editor->toPlainText(), QStringLiteral("a") + flag);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));

    QTest::keyClick(editor, Qt::Key_Delete); // then the plain 'a'
    QCOMPARE(editor->toPlainText(), flag);

    QTest::keyClick(editor, Qt::Key_Delete); // whole flag in one press
    QCOMPARE(editor->toPlainText(), QString());
    QCOMPARE(editor->lineCount(), 1);
}

void TestCodeEditor::caretInsideClusterSnapsWithoutOrphans()
{
    // 'a' + U+1F600 (cols 1..2) + 'b': a mouse click or setPosition can put
    // the caret at col 2, INSIDE the surrogate pair. Arrows must snap out;
    // edits must take the whole cluster and leave no orphan half behind.
    editor->setPlainText(QString::fromUtf8("a\U0001F600" "b"));

    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Right); // snap right: past the pair
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));

    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Left); // snap left: before the pair
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));

    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Delete); // deletes the whole pair
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab")); // no orphan low
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));

    editor->setPlainText(QString::fromUtf8("a\U0001F600" "b"));
    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Backspace); // same from the other side
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab")); // no orphan high
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));
}

void TestCodeEditor::wordNavigationRightSteps()
{
    editor->setPlainText(QStringLiteral("foo_bar baz!! qux"));
    editor->setPosition({0, 0});
    // One press per class run (VS Code cursorWordRight); targets are the
    // ends of word / space / punctuation runs.
    const int stops[] = {7, 8, 11, 13, 14, 17};
    for (int col : stops) {
        QTest::keyClick(editor, Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(editor->position(), (qmn::view::TextPosition{0, col}));
    }
    QTest::keyClick(editor, Qt::Key_Right, Qt::ControlModifier); // at EOL, single line: stays
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 17}));
}

void TestCodeEditor::wordNavigationLeftSteps()
{
    editor->setPlainText(QStringLiteral("foo_bar baz!! qux"));
    editor->setPosition({0, 17});
    // Word run, then space+run combined, alternating like cursorWordLeft.
    const int stops[] = {14, 11, 8, 0};
    for (int col : stops) {
        QTest::keyClick(editor, Qt::Key_Left, Qt::ControlModifier);
        QCOMPARE(editor->position(), (qmn::view::TextPosition{0, col}));
    }
    QTest::keyClick(editor, Qt::Key_Left, Qt::ControlModifier); // at 0, single line: stays
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));

    // Ctrl+Shift+Left extends the selection to the word target.
    editor->setPosition({0, 8});
    QTest::keyClick(editor, Qt::Key_Left, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));
    QCOMPARE(editor->selectedText(), QStringLiteral("foo_bar "));
}

void TestCodeEditor::wordNavigationCrossesLines()
{
    editor->setPlainText(QStringLiteral("ab\ncde"));
    editor->setPosition({0, 2}); // at EOL of line 0
    QTest::keyClick(editor, Qt::Key_Right, Qt::ControlModifier); // wraps to next line start
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));
    QTest::keyClick(editor, Qt::Key_Left, Qt::ControlModifier); // wraps back to prev line end
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));
}

void TestCodeEditor::ctrlDeleteWordRight()
{
    editor->setPlainText(QStringLiteral("foo bar baz"));
    editor->setPosition({0, 0});

    QTest::keyClick(editor, Qt::Key_Delete, Qt::ControlModifier); // "foo"
    QCOMPARE(editor->toPlainText(), QStringLiteral(" bar baz"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));

    QTest::keyClick(editor, Qt::Key_Delete, Qt::ControlModifier); // the space run
    QCOMPARE(editor->toPlainText(), QStringLiteral("bar baz"));

    QTest::keyClick(editor, Qt::Key_Delete, Qt::ControlModifier); // "bar"
    QCOMPARE(editor->toPlainText(), QStringLiteral(" baz"));
    QVERIFY(!editor->hasSelection());

    // At EOL Ctrl+Del joins the lines, like plain Delete.
    editor->setPlainText(QStringLiteral("ab\ncd"));
    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Delete, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcd"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));

    // With a selection, Ctrl+Del removes the selection (like Delete).
    editor->setPlainText(QStringLiteral("hello world"));
    editor->setPosition({0, 6});
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier);
    QTest::keyClick(editor, Qt::Key_Delete, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("hello "));
}

void TestCodeEditor::ctrlDeleteNeverSplitsClusters()
{
    // "e + U+0301" then "x y": the word run ends mid-cluster at the mark;
    // Ctrl+Del must take the whole cluster, not orphan the mark.
    editor->setPlainText(QString::fromUtf8("e\u0301x y"));
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_Delete, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("x y"));
    QVERIFY(!editor->toPlainText().contains(QChar(0x0301)));

    // ZWJ family: all units are punctuation-class, the run ends exactly at
    // the cluster's outer boundary, so the family dies whole in one press.
    const QString family = QString::fromUtf8("\U0001F468\u200D\U0001F469\u200D\U0001F467");
    editor->setPlainText(family + "a b");
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_Delete, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("a b"));
}

// --- IME composition (Phase C) ------------------------------------------

void TestCodeEditor::imeCommitInsertsText()
{
    editor->setPlainText(QStringLiteral("x"));
    editor->setPosition({0, 1});
    // "こんにちは" — 5 UTF-16 units, a typical Japanese IME commit.
    const QString japanese = QString::fromUtf8("こんにちは");
    // (Qt 6.12 shape: preedit+attributes in the ctor, commit via setter.)
    QInputMethodEvent commit;
    commit.setCommitString(japanese);
    QVERIFY(QApplication::sendEvent(editor, &commit));
    QCOMPARE(editor->toPlainText(), QStringLiteral("x") + japanese);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 6}));
    // Committed CJK text survives cluster-safe editing as one unit per
    // kana (each is its own cluster here).
    QTest::keyClick(editor, Qt::Key_Backspace);
    QCOMPARE(editor->toPlainText(), QStringLiteral("x") + japanese.left(4));
    // The trailing は (U+306F) went as one cluster; the remaining kana each
    // stayed a cluster of their own.
}

void TestCodeEditor::imePreeditStaysOutOfBufferUntilCommit()
{
    editor->setPlainText(QStringLiteral("a"));
    editor->setPosition({0, 1});

    QInputMethodEvent preedit1(QStringLiteral("nihon"), {});
    QVERIFY(QApplication::sendEvent(editor, &preedit1));
    // Preedit is display-only: nothing reached the model or the caret.
    QCOMPARE(editor->toPlainText(), QStringLiteral("a"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));

    // Typical IME step: replace preedit with a candidate plus a new preedit
    // tail ("日本" committed, "go" still composing). Only the commit hits
    // the buffer.
    QInputMethodEvent step(QStringLiteral("go"), {});
    step.setCommitString(QStringLiteral("日本"));
    QVERIFY(QApplication::sendEvent(editor, &step));
    QCOMPARE(editor->toPlainText(), QStringLiteral("a日本"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));

    // Composition cancelled (empty commit + empty preedit): buffer keeps
    // exactly what was committed, no leftovers.
    QInputMethodEvent cancel; // empty commit + empty preedit
    QVERIFY(QApplication::sendEvent(editor, &cancel));
    QCOMPARE(editor->toPlainText(), QStringLiteral("a日本"));

    // A deliberate caret move also ends any stale preedit display: preedit,
    // then Left arrow, then another preedit must not stack on the old one.
    QInputMethodEvent preedit2(QStringLiteral("old"), {});
    preedit2.setCommitString(QStringLiteral("x"));
    QVERIFY(QApplication::sendEvent(editor, &preedit2));
    QTest::keyClick(editor, Qt::Key_Left); // moveCaret clears preedit_
    // caret is back at col 3 and no stale preedit trails it, so the
    // candidate anchor must sit at the plain caret column again. The
    // expectation reads the widget's own layout truth (CJK glyphs are wider
    // than one monospace cell; the old col*cellWidth formula WAS the drift).
    QInputMethodQueryEvent queries(Qt::ImCursorRectangle);
    QVERIFY(QApplication::sendEvent(editor, &queries));
    QCOMPARE(queries.value(Qt::ImCursorRectangle).toRect().left(),
             qRound(editor->xForCol(0, 3)));
}

void TestCodeEditor::imeQueriesTrackCaretAndPreedit()
{
    editor->setPlainText(QStringLiteral("ab"));
    editor->setPosition({0, 2});

    QInputMethodQueryEvent enabled(Qt::ImEnabled);
    QVERIFY(QApplication::sendEvent(editor, &enabled));
    QVERIFY(enabled.value(Qt::ImEnabled).toBool());

    QInputMethodQueryEvent q1(Qt::ImCursorRectangle);
    QVERIFY(QApplication::sendEvent(editor, &q1));
    const QRect anchor = q1.value(Qt::ImCursorRectangle).toRect();
    QCOMPARE(anchor.left(), editor->padding() + qRound(2 * editor->cellWidth()));
    QCOMPARE(anchor.top(), editor->padding()); // line 0, unscrolled
    QCOMPARE(anchor.height(), editor->lineHeight());

    // The candidate anchor must trail the preedit, not the buffer caret.
    QInputMethodEvent preedit(QStringLiteral("xy"), {});
    QVERIFY(QApplication::sendEvent(editor, &preedit));
    QInputMethodQueryEvent q2(Qt::ImCursorRectangle);
    QVERIFY(QApplication::sendEvent(editor, &q2));
    QCOMPARE(q2.value(Qt::ImCursorRectangle).toRect().left(),
             editor->padding() + qRound(4 * editor->cellWidth()));

    // The IM may want the current selection to operate on.
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier); // select "ab"
    QInputMethodQueryEvent q3(Qt::ImCurrentSelection);
    QVERIFY(QApplication::sendEvent(editor, &q3));
    QCOMPARE(q3.value(Qt::ImCurrentSelection).toString(), QStringLiteral("ab"));
}

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
