// CodeEditor widget tests: keyboard edits, selection, clipboard, undo,
// multiple carets and syntax highlighting must reach the model. Rendering
// is asserted ONLY through numeric pixel-band scans (the raster engine is
// the only platform here, and no vision is available) - never screenshots.
//
// M4 pixel facts (measured, this environment): offscreen Base palette is
// dark (#323232); green-dominant INK on the fixture line comes ONLY from
// the Comment (#6A9955) and Number (#B5CEA8) colors - Keyword (#569CD6)
// is blue-dominant and the rest neutral.

#include <QtTest>

#include <QClipboard>
#include <QImage>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QWheelEvent>

#include "view/CodeEditor.hpp"

#include <algorithm> // std::min in the ruler mapping contract
#include <cmath> // std::ceil in the h-scroll exact-offset arithmetic
#include <functional> // std::function for the probe predicates

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

    void undoTypingBurstIsOneStep();
    void undoEnterBreaksTypingChain();
    void undoCaretMoveBreaksChain();
    void undoOverSelectionTypingRestoresTextAndSelection();
    void undoPasteIsOneStep();
    void undoCutRestoresCutLine();
    void undoBackspaceChainIsOneChunk();
    void redoViaCtrlShiftZAndCtrlY();
    void newEditTruncatesRedoBranch();
    void setPlainTextClearsUndoHistory();
    void undoOnEmptyHistoryIsNoOp();

    // Multiple carets (Phase D1).
    void altClickAddsCaretsInDocOrder();
    void altClickInsideSelectionFollowsRules();
    void altDragSelectsExtraAndPrimary();
    void plainClickAndEscapeCollapseCarets();
    void programmaticPathsCollapseCarets();
    void navigationFoldDuplicatesAndKeepSelections();
    void typingFansOutToOneEditPerCaret();
    void typingOverSelectionsFansOut();
    void enterFansOutAsOneUndoStep();
    void backspaceAndDeleteFansOut();
    void backspaceOnExtraAtDocStartIsSkipped();
    void overlappingEditsKeepRightDropLeft();
    void ctrlDeleteWordFansOut();
    void caretKeysAndShiftNavFanOut();
    void copyWithCaretsJoinsLines();
    void cutSelectionsAtCaretsDeletesAll();
    void cutLinesAtCaretsJoinsClipboard();
    void pasteFansOutToAllCarets();
    void undoRestoresAllCaretsOneStepPerPress();

    // Syntax highlighting (M4, Phase D2).
    void setLanguageUnknownFallsBackToPlain();
    void availableLanguagesShipsPinnedGrammars();
    void pythonSpansMapToModelOffsets();
    void jsonSpansMapToModelOffsets();
    void javascriptSpansMapToModelOffsets();
    void jsonKeyQueryPatternWinsOverPlainString();
    void queryPredicatesFilterRefinedRoles();
    void spansClipToRequestedWindow();
    void stringSpanCoversSurrogatePairWhole();
    void typedQuotesCloseStringIncrementally();
    void coloredPaintChangesCommentInk();
    void coloredPaintKeepsGlyphGeometry();
    void largeDocViewportPolicyKeepsSpansExact();

    // Decorations + gutter (M5, Phase D3).
    void gutterDefaultAndOffCollapsesGeometry();
    void gutterWidthGrowsAtLineCountCrossover();
    void gutterLineNumbersModeMath();
    void decorationsRoundTripUpdateRemove();
    void gutterShiftsTextGeometryAndHitTest();
    void diagnosticUnderlinePaintsSeverityInk();

    // Find & replace (M6, Phase D4). DoD: match ranges as model ranges
    // (numeric assertions incl. surrogate safety); replace-all is ONE
    // undoable D1 action with a confirm step; match highlights ride the
    // decoration-style paint pass (viewport-only scans).
    void findLiteralRangesCaseFlagAndEscapedMeta();
    void findWholeWordFlag();
    void findRegexFlagInvalidPatternAndPerLineEngine();
    void findMatchesSnapOverSurrogatePairs();
    void findNextPreviousCycleWrapAndCurrentMatch();
    void replaceNextOwnUndoStepAndCaretLanding();
    void replaceAllIsOneUndoableAction();
    void findSetRefreshesLiveThroughTyping();
    void findBarOpenClosePersistsAndDrivesEngine();
    void replaceAllConfirmStepIsButtonOnly();
    void findBarStaysTopRightOnResize();
    void paintMatchQuadsNumericProbe();
    void paintZeroWidthMatchPhantomCell();

    // Overview ruler (M7, Phase E1). DoD: ruler geometry state tests
    // (numeric mapping contract), the click/drag scroll gesture (state
    // assertions), and O(strip) marker paint proven by pixel probes
    // bounded to the ruler's x window (no off-screen document cost).
    void overviewRulerGeometryAndMapping();
    void overviewRulerScrollGesture();
    void overviewRulerMarkerPaintProbe();

    // Horizontal scroll (M9, Phase E1). DoD: numeric viewport assertions —
    // the ambient extent/clamp contract, caret scroll-into-view on long
    // lines, wheel/click mapping through the offset, the
    // set_scroll_beyond_last_line_enabled parity flag (ruler total rides
    // it), and a pixel-band probe proving painted ink lands at the shifted
    // widget x (windowed paint, no off-screen glyph cost).
    void horizontalScrollExtentAndClamps();
    void caretKeptVisibleHorizontallyOnLongLines();
    void shiftWheelAndClickMapThroughOffset();
    void scrollBeyondLastLineFlagAndRulerTotal();
    void paintClusterWindowXOffsetProbe();

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
                      QPoint(editor->contentLeft() + int(3 * cw + cw / 2), pad + lh / 2));
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 3);

    // far below the last line clamps to the last line
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier, QPoint(pad, pad + 100 * lh));
    QCOMPARE(editor->position().line, 1);
    // far right clamps to line length
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(editor->contentLeft() + int(90 * cw), pad + lh / 2));
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

    // ASCII line: identical to the grid model down to the last epsilon
    // (contentLeft: M5 puts the gutter left of the text area).
    for (int c = 0; c <= 10; ++c)
        QVERIFY2(qAbs(editor->xForCol(1, c) - (editor->contentLeft() + c * cw)) < 1e-9,
                 qPrintable(QStringLiteral("col %1: %2 vs %3")
                                .arg(c).arg(editor->xForCol(1, c)).arg(editor->contentLeft() + c * cw)));

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
    const double gridX = editor->contentLeft() + 10 * editor->cellWidth(); // the BUGGY position
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

    QVERIFY(qAbs(editor->xForCol(1, 0) - editor->contentLeft()) < 1e-12);
    QCOMPARE(editor->xForCol(1, 5), editor->xForCol(1, 0)); // clamped to EOL
    QCOMPARE(editor->xForCol(99, 3), editor->xForCol(0, 0)); // bad line -> line start x

    // A click anywhere on an empty line lands at its start.
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(editor->contentLeft() + int(30 * cw), pad + lh + lh / 2));
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
    const int x0 = editor->contentLeft(); // M5: scan the TEXT area only;
    const int x1 = x0 + int(editor->cellWidth()) - 1; // exclude caret cols
    const QImage img = editor->grab().toImage();
    const QColor base = editor->palette().color(QPalette::Base);

    for (int i = 0; i < 5; ++i) {
        const int yTop = pad + i * lh;
        int inBand = 0, below = 0;
        for (int x = x0; x <= x1 && x < img.width(); ++x) {
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
                      QPoint(editor->contentLeft() + int(5 * cw + cw / 2), pad + lh / 2));
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 5);
    QCOMPARE(editor->selectedText(), QStringLiteral("cde"));

    // Plain click collapses at the new position.
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(editor->contentLeft(), pad + lh));
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

    dragSelect(editor, QPoint(editor->contentLeft() + 1, pad + lh / 2),
               QPoint(editor->contentLeft() + qRound(3 * cw + cw / 2), pad + lh / 2));
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("abc"));
    QCOMPARE(editor->position().col, 3); // caret at the drag end (active)

    // Drag across lines: press at col 4, release at col 1 of line 1.
    dragSelect(editor, QPoint(editor->contentLeft() + qRound(4 * cw) + 1, pad + lh / 2),
               QPoint(editor->contentLeft() + qRound(1.5 * cw), pad + lh + lh / 2));
    QCOMPARE(editor->selectedText(), QStringLiteral("ef\nx"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 1}));
}

void TestCodeEditor::mouseDragSelectsBackwards()
{
    editor->setPlainText(QStringLiteral("abcdef"));
    const int pad = editor->padding();
    const double cw = editor->cellWidth();
    const int lh = editor->lineHeight();

    dragSelect(editor, QPoint(editor->contentLeft() + qRound(4 * cw) + 1, pad + lh / 2),
               QPoint(editor->contentLeft() + qRound(1.5 * cw), pad + lh / 2));
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
    QCOMPARE(anchor.left(), editor->contentLeft() + qRound(2 * editor->cellWidth()));
    QCOMPARE(anchor.top(), editor->padding()); // line 0, unscrolled
    QCOMPARE(anchor.height(), editor->lineHeight());

    // The candidate anchor must trail the preedit, not the buffer caret.
    QInputMethodEvent preedit(QStringLiteral("xy"), {});
    QVERIFY(QApplication::sendEvent(editor, &preedit));
    QInputMethodQueryEvent q2(Qt::ImCursorRectangle);
    QVERIFY(QApplication::sendEvent(editor, &q2));
    QCOMPARE(q2.value(Qt::ImCursorRectangle).toRect().left(),
             editor->contentLeft() + qRound(4 * editor->cellWidth()));

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

    dragSelect(editor, QPoint(editor->contentLeft() + 1, pad + lh / 2),
               QPoint(editor->contentLeft() + qRound(3 * cw + cw / 2), pad + lh / 2));
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
                      QPoint(editor->contentLeft() + int(2 * cw + cw / 2), pad + lh / 2));
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
                      QPoint(editor->contentLeft() + int(2 * cw + cw / 2), pad + lh / 2));
    QCOMPARE(editor->toPlainText(), QStringLiteral("abZZcdef"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));
}

// --- undo/redo (M3) ------------------------------------------------------

void TestCodeEditor::undoTypingBurstIsOneStep()
{
    editor->setPlainText(QStringLiteral("ab"));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClicks(editor, "cdef"); // one continuous typing burst -> one group
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdef"));

    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab")); // the whole burst goes at once
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2})); // caret before the burst
    QVERIFY(!editor->canUndo());
    QVERIFY(editor->canRedo());

    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdef"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 6})); // caret as it stood when undone
}

void TestCodeEditor::undoEnterBreaksTypingChain()
{
    editor->setPlainText(QString());
    QTest::keyClicks(editor, "ab");
    QTest::keyClick(editor, Qt::Key_Return);
    QTest::keyClicks(editor, "cd");
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab\ncd"));

    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab\n")); // "cd" alone steps back
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab")); // Enter is its own step
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QString());
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));
    QVERIFY(!editor->canUndo());
}

void TestCodeEditor::undoCaretMoveBreaksChain()
{
    editor->setPlainText(QStringLiteral("abc"));
    editor->setPosition({0, 0});
    QTest::keyClicks(editor, "X"); // "Xabc"
    editor->setPosition({0, 1});  // deliberate move freezes the chain
    QTest::keyClicks(editor, "Y"); // adjacent to X, but must be its own step
    QCOMPARE(editor->toPlainText(), QStringLiteral("XYabc"));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("Xabc")); // only Y reverts
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));

    // The same freeze via a mouse click landing on the adjacent edge:
    const int pad = editor->padding();
    const double cw = editor->cellWidth();
    const int lh = editor->lineHeight();
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(editor->contentLeft() + int(cw + cw / 2), pad + lh / 2)); // col 1
    QTest::keyClicks(editor, "W");
    QCOMPARE(editor->toPlainText(), QStringLiteral("XWabc"));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("Xabc")); // only W reverts
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abc"));
}

void TestCodeEditor::undoOverSelectionTypingRestoresTextAndSelection()
{
    editor->setPlainText(QStringLiteral("abcdef"));
    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier); // anchor {0,1}, caret {0,3}: "bc"
    QSignalSpy selSpy(editor, &CodeEditor::selectionChanged);
    QTest::keyClicks(editor, "X");
    QCOMPARE(editor->toPlainText(), QStringLiteral("aXdef"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));
    QVERIFY(!editor->hasSelection());
    const int afterType = selSpy.count(); // the replace dropped the selection
    QVERIFY(afterType >= 1);

    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdef"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));
    QVERIFY(editor->hasSelection()); // the pre-action selection comes back
    QCOMPARE(editor->selectedText(), QStringLiteral("bc"));
    QVERIFY(selSpy.count() > afterType); // and the change is announced

    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("aXdef"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));
    QVERIFY(!editor->hasSelection());
}

void TestCodeEditor::undoPasteIsOneStep()
{
    editor->setPlainText(QStringLiteral("ab"));
    editor->setPosition({0, 2});
    QClipboard* cb = QApplication::clipboard();
    cb->setText(QStringLiteral("XY\nZW"), QClipboard::Clipboard);
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abXY\nZW"));
    cb->setText(QStringLiteral("Q"), QClipboard::Clipboard);
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abXY\nZWQ"));

    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abXY\nZW")); // pastes never chain
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 2}));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));

    // Paste over a selection is ONE compound step (nested brackets commit at
    // the outer paste bracket), and undo brings the selection back.
    editor->setPlainText(QStringLiteral("abcdef"));
    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier); // "cd"
    cb->setText(QStringLiteral("XY"), QClipboard::Clipboard);
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abXYef"));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdef"));
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("cd"));
}

void TestCodeEditor::undoCutRestoresCutLine()
{
    editor->setPlainText(QStringLiteral("abc\ndef"));
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_X, Qt::ControlModifier); // whole-line cut
    QCOMPARE(editor->toPlainText(), QStringLiteral("def"));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abc\ndef"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));
}

void TestCodeEditor::undoBackspaceChainIsOneChunk()
{
    editor->setPlainText(QStringLiteral("abcd"));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClick(editor, Qt::Key_Backspace); // deletes d
    QTest::keyClick(editor, Qt::Key_Backspace); // deletes c
    QTest::keyClick(editor, Qt::Key_Backspace); // deletes b
    QCOMPARE(editor->toPlainText(), QStringLiteral("a"));
    QVERIFY(editor->canUndo());

    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcd")); // the chain is one chunk
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4})); // caret before the first Backspace
    QVERIFY(!editor->canUndo());
    QVERIFY(editor->canRedo());
}

void TestCodeEditor::redoViaCtrlShiftZAndCtrlY()
{
    editor->setPlainText(QStringLiteral("ab"));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClicks(editor, "c");

    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab"));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abc"));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab"));

    // Qt's generic Redo sequence holds only Ctrl+Shift+Z (measured on Qt
    // 6.12 offscreen); the view binds Ctrl+Y explicitly, editor-style.
    QTest::keyClick(editor, Qt::Key_Y, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abc"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));
}

void TestCodeEditor::newEditTruncatesRedoBranch()
{
    editor->setPlainText(QString());
    QTest::keyClicks(editor, "ab");
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QString());
    QVERIFY(editor->canRedo());

    QTest::keyClicks(editor, "Q"); // a fresh edit truncates the redo branch
    QVERIFY(!editor->canRedo());
    editor->redo();
    QCOMPARE(editor->toPlainText(), QStringLiteral("Q"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));
}

void TestCodeEditor::setPlainTextClearsUndoHistory()
{
    editor->setPlainText(QString());
    QTest::keyClicks(editor, "abc");
    editor->setPosition({0, 1}); // break the chain: two groups exist
    QTest::keyClicks(editor, "d");
    QVERIFY(editor->canUndo());

    editor->setPlainText(QStringLiteral("fresh"));
    QVERIFY(!editor->canUndo());
    QVERIFY(!editor->canRedo());
    editor->undo(); // refused outright against the emptied history
    editor->redo();
    QCOMPARE(editor->toPlainText(), QStringLiteral("fresh"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));
}

void TestCodeEditor::undoOnEmptyHistoryIsNoOp()
{
    editor->setPlainText(QStringLiteral("x"));
    QVERIFY(!editor->canUndo());
    QVERIFY(!editor->canRedo());
    editor->undo();
    editor->redo();
    QCOMPARE(editor->toPlainText(), QStringLiteral("x"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));
}

// --- multiple carets (Phase D1) -------------------------------------------

namespace {

// Pixel for a (line, col) hit in the monospace test font: the cell-center
// convention of mousePlacesCaret, plus a y nudge so the click never lands
// inside the glyph of an adjacent cell.
QPoint caretPixel(CodeEditor* e, int line, int col, double xNudge = 0.4)
{
    const int pad = e->padding();
    const double cw = e->cellWidth();
    const int lh = e->lineHeight();
    return QPoint(e->contentLeft() + int(col * cw + cw / 2 + xNudge), pad + line * lh + lh / 2);
}

void altClick(CodeEditor* e, int line, int col)
{
    QTest::mousePress(e, Qt::LeftButton, Qt::AltModifier, caretPixel(e, line, col));
}

// Alt+press at `from`, drag to `to`, release. Button state rides the move
// event (QTest::mouseMove would drop it); never Q_ASSERT the sends.
void altDrag(CodeEditor* e, const QPoint& from, const QPoint& to)
{
    const QPointF f(from), t(to);
    QMouseEvent press(QEvent::MouseButtonPress, f, f, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    QMouseEvent move(QEvent::MouseMove, t, t, Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, t, t, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    QTEST_ASSERT(QApplication::sendEvent(e, &press));
    QTEST_ASSERT(QApplication::sendEvent(e, &move));
    QTEST_ASSERT(QApplication::sendEvent(e, &release));
}

} // namespace

void TestCodeEditor::altClickAddsCaretsInDocOrder()
{
    editor->setPlainText(QStringLiteral("aa bb cc"));
    editor->setPosition({0, 0});

    altClick(editor, 0, 6); // adds at 6; the clicked caret becomes PRIMARY
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 6}));
    QCOMPARE(editor->additionalCaretCount(), 1);
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}})); // old primary demoted

    altClick(editor, 0, 3);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));
    QCOMPARE(editor->additionalCaretCount(), 2);
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}, {0, 6}})); // document order

    // Alt+click exactly on the primary caret adds nothing (it arms a drag).
    altClick(editor, 0, 3);
    QCOMPARE(editor->additionalCaretCount(), 2);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));

    // Same on an extra caret's spot: that slot is dragged, no new caret.
    altClick(editor, 0, 0);
    QCOMPARE(editor->additionalCaretCount(), 2);

    // A plain click collapses the whole set back to one caret.
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier, caretPixel(editor, 0, 1));
    QCOMPARE(editor->additionalCaretCount(), 0);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));
}

void TestCodeEditor::altClickInsideSelectionFollowsRules()
{
    editor->setPlainText(QStringLiteral("abcdefgh"));
    editor->setPosition({0, 0});

    altDrag(editor, caretPixel(editor, 0, 0), caretPixel(editor, 0, 5));
    QVERIFY(editor->hasSelection()); // primary owns [0,5)
    QCOMPARE(editor->additionalCaretCount(), 0);

    altClick(editor, 0, 7); // adds, demoting the [0,5) selection into an extra
    QCOMPARE(editor->additionalCaretCount(), 1);

    // Alt+click strictly inside the extra's selection: the added caret is
    // PRIMARY, so the extra holding the enclosing selection loses its slot
    // (the primary caret always survives).
    altClick(editor, 0, 3);
    QCOMPARE(editor->additionalCaretCount(), 1);
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 7}}));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));
    QVERIFY(!editor->hasSelection());
}

void TestCodeEditor::altDragSelectsExtraAndPrimary()
{
    editor->setPlainText(QStringLiteral("abcdefgh"));
    editor->setPosition({0, 0});

    altClick(editor, 0, 6); // primary 6, extra at 0
    altDrag(editor, caretPixel(editor, 0, 0), caretPixel(editor, 0, 3));
    // The dragged EXTRA got [0,3); the click landed on that slot's caret.
    QCOMPARE(editor->additionalCaretCount(), 1);
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 3}})); // caret rode the drag
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 6})); // primary kept

    altDrag(editor, caretPixel(editor, 0, 6), caretPixel(editor, 0, 4));
    // Alt+press on the PRIMARY caret drags it; the anchor stays pinned at 6.
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("ef")); // [4,6)
    QCOMPARE(editor->additionalCaretCount(), 1); // no overlap with [0,3): survives

    // Partial overlap: drag the primary selection [4,6) LEFT across [0,3).
    // R2: the LEFT-starting extra [0,3) keeps its range, the primary
    // collapses to a bare caret at 1. The chained R3 then finds that caret
    // strictly inside the surviving [0,3): the caret is PRIMARY, so the
    // extra slot dies too. v1 outcome: one bare primary caret.
    altDrag(editor, caretPixel(editor, 0, 4), caretPixel(editor, 0, 1));
    QVERIFY(!editor->hasSelection());
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));
    QCOMPARE(editor->additionalCaretCount(), 0);
}

void TestCodeEditor::plainClickAndEscapeCollapseCarets()
{
    editor->setPlainText(QStringLiteral("aa bb cc"));
    editor->setPosition({0, 0});
    altClick(editor, 0, 3);
    altClick(editor, 0, 6);
    QCOMPARE(editor->additionalCaretCount(), 2);

    // Two-stage Escape: the extras go first...
    QTest::keyClick(editor, Qt::Key_Escape);
    QCOMPARE(editor->additionalCaretCount(), 0);
    QVERIFY(!editor->hasSelection());
    // ...the primary selection is stage two. Alt+Drag adds a caret on press
    // (the old primary demotes), so Escape first drops THAT extra, then the
    // selection on the next press.
    altDrag(editor, caretPixel(editor, 0, 2), caretPixel(editor, 0, 4));
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->additionalCaretCount(), 1);
    QTest::keyClick(editor, Qt::Key_Escape);
    QCOMPARE(editor->additionalCaretCount(), 0);
    QVERIFY(editor->hasSelection()); // selection survives stage one
    QTest::keyClick(editor, Qt::Key_Escape);
    QVERIFY(!editor->hasSelection()); // stage two clears it
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));
    // Nothing left: a further Escape is passed through (event->ignore()).
    QTest::keyClick(editor, Qt::Key_Escape);

    // Middle click collapses too (X11 paste gesture targets one caret).
    editor->setPosition({0, 0});
    altClick(editor, 0, 3);
    altClick(editor, 0, 6);
    QCOMPARE(editor->additionalCaretCount(), 2);
    QTest::mousePress(editor, Qt::MiddleButton, Qt::NoModifier, caretPixel(editor, 0, 1));
    QCOMPARE(editor->additionalCaretCount(), 0);
}

void TestCodeEditor::programmaticPathsCollapseCarets()
{
    editor->setPlainText(QStringLiteral("ab\ncd"));
    altClick(editor, 1, 0); // primary is now the clicked caret
    QCOMPARE(editor->additionalCaretCount(), 1);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));
    editor->clearAdditionalCarets();
    QCOMPARE(editor->additionalCaretCount(), 0);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0})); // primary kept

    altClick(editor, 0, 0); // add: primary moves, (1,0) demotes
    editor->setPosition({1, 1});
    QCOMPARE(editor->additionalCaretCount(), 0);

    altClick(editor, 1, 0);
    editor->selectAll(); // public single-caret selection (routes setSelection)
    QCOMPARE(editor->additionalCaretCount(), 0);
    QVERIFY(editor->hasSelection());

    altClick(editor, 1, 0);
    editor->setPlainText(QStringLiteral("fresh"));
    QCOMPARE(editor->additionalCaretCount(), 0);
}

void TestCodeEditor::navigationFoldDuplicatesAndKeepSelections()
{
    editor->setPlainText(QStringLiteral("aa bb cc"));
    altClick(editor, 0, 3);
    altClick(editor, 0, 6);

    // Ctrl+Home drives every caret to (0,0): the stacked extras fold (R1).
    QTest::keyClick(editor, Qt::Key_Home, Qt::ControlModifier);
    QCOMPARE(editor->additionalCaretCount(), 0);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));

    // Rebuild carets, then Shift+End extends EVERY slot: three collinear
    // right-to-left selections normalize to the single outermost one, and
    // the survivor belongs to the primary (caret on top).
    altClick(editor, 0, 3);
    altClick(editor, 0, 6);
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier);
    QCOMPARE(editor->additionalCaretCount(), 0);
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("cc")); // caret at EOD
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 8}));
}

void TestCodeEditor::typingFansOutToOneEditPerCaret()
{
    editor->setPlainText(QStringLiteral("hello world"));
    altClick(editor, 0, 6); // primary at the 'w', extra at (0,0)

    QTest::keyClick(editor, Qt::Key_X);
    QCOMPARE(editor->toPlainText(), QStringLiteral("xhello xworld"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 8}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 1}}));

    // One key press = one bracketed undo group (no cross-press coalescing).
    // (keyClick Key_Y delivers lowercase 'y', hence the lower-case joins.)
    QTest::keyClick(editor, Qt::Key_Y);
    QCOMPARE(editor->toPlainText(), QStringLiteral("xyhello xyworld"));
    editor->undo();
    QCOMPARE(editor->toPlainText(), QStringLiteral("xhello xworld"));
    // The caret state is restored for EVERY slot, not just the primary.
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 8}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 1}}));
}

void TestCodeEditor::typingOverSelectionsFansOut()
{
    editor->setPlainText(QStringLiteral("abcdefg"));
    altDrag(editor, caretPixel(editor, 0, 0), caretPixel(editor, 0, 2)); // primary [0,2)
    altClick(editor, 0, 4); // primary moves to 4, the [0,2) slot demotes
    altDrag(editor, caretPixel(editor, 0, 4), caretPixel(editor, 0, 6)); // primary [4,6)

    // [0,2) and [4,6) are disjoint; both slots replace their selections.
    QTest::keyClick(editor, Qt::Key_Q); // keyClick delivers lowercase 'q'
    QCOMPARE(editor->toPlainText(), QStringLiteral("qcdqg"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4})); // after the primary's 'q'
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 1}}));

    editor->undo();
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefg")); // ONE step for both edits
    QCOMPARE(editor->additionalCaretCount(), 1); // pre-press selection pair restored
}

void TestCodeEditor::enterFansOutAsOneUndoStep()
{
    editor->setPlainText(QStringLiteral("ab\ncd"));
    altClick(editor, 1, 2); // primary at EOL of line 1, extra at (0,0)

    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("\nab\ncd\n"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{3, 0}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{1, 0}}));

    editor->undo(); // the whole Enter is ONE step across both carets
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab\ncd"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 2}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}}));
    QVERIFY(!editor->canUndo());
}

void TestCodeEditor::backspaceAndDeleteFansOut()
{
    editor->setPlainText(QStringLiteral("abc\ndef"));
    altClick(editor, 1, 1); // primary mid line 1, extra at (0,0)
    QTest::keyClick(editor, Qt::Key_Backspace);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abc\nef")); // only the primary could act
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}}));

    editor->setPlainText(QStringLiteral("ab\nab\nab"));
    altClick(editor, 1, 2);
    altClick(editor, 2, 2); // primary EOL line 2, extras EOL 0 and 1
    QTest::keyClick(editor, Qt::Key_Delete);
    // Delete is FORWARD: (0,0) eats its line's first char (the caret lands
    // at the now-empty line start), (1,2) joins the next line, the doc-end
    // caret edits nothing but rides both deletes to (1,4).
    QCOMPARE(editor->toPlainText(), QStringLiteral("b\nabab"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 4}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}, {1, 2}}));
}

void TestCodeEditor::backspaceOnExtraAtDocStartIsSkipped()
{
    editor->setPlainText(QStringLiteral("abc\ndef"));
    altClick(editor, 1, 1); // primary (1,1); the extra sits at the doc start

    QTest::keyClick(editor, Qt::Key_Backspace);
    QCOMPARE(editor->additionalCaretCount(), 1); // the no-op extra survives
    editor->undo();
    QCOMPARE(editor->toPlainText(), QStringLiteral("abc\ndef"));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}})); // restored at its spot
    QVERIFY(!editor->canUndo()); // the whole press was ONE bracketed step
}

void TestCodeEditor::overlappingEditsKeepRightDropLeft()
{
    editor->setPlainText(QStringLiteral("abcdef"));
    altClick(editor, 0, 2); // primary at 2, extra at 0

    // Ctrl+Delete runs word deletes from both carets; "abcdef" is ONE word,
    // so the primary's [2,6) edit lands first and the extra's [0,6) edit
    // clashes with it and is DROPPED (documented v1 divergence vs VS Code's
    // merge). The extra's caret at 0 is left of the cut and rides along.
    QTest::keyClick(editor, Qt::Key_Delete, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}}));

    editor->undo(); // dropped and applied: still ONE undo step
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdef"));
    QVERIFY(!editor->canUndo());
}

void TestCodeEditor::ctrlDeleteWordFansOut()
{
    editor->setPlainText(QStringLiteral("ab cd ef gh"));
    altClick(editor, 0, 3); // three carets end up at 6 (primary), 3 and 0

    altClick(editor, 0, 6);

    QTest::keyClick(editor, Qt::Key_Delete, Qt::ControlModifier);
    // Word runs deleted at every caret: [0,2) "ab", [3,5) "cd", [6,8) "ef".
    QCOMPARE(editor->toPlainText(), QStringLiteral("   gh"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}, {0, 1}}));
}

void TestCodeEditor::caretKeysAndShiftNavFanOut()
{
    editor->setPlainText(QStringLiteral("abcd\nefgh\nijkl"));
    altClick(editor, 1, 2);
    altClick(editor, 2, 3); // primary (2,3), extras (0,0) and (1,2)

    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}, {1, 0}}));
    QTest::keyClick(editor, Qt::Key_End);
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 4}, {1, 4}}));
    QTest::keyClick(editor, Qt::Key_Up);
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}, {0, 4}})); // (0,4) pinned at col 0
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 4})); // top clamp

    // Shift+Left extends every slot: the (0,0) caret cannot move and stays
    // bare, the other two pick up one character each.
    QTest::keyClick(editor, Qt::Key_Left, Qt::ShiftModifier);
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("h"));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}, {0, 3}}));

    // Ctrl+Shift+Right word-extends: the bare (0,0) caret sweeps the whole
    // line into a [0,4) selection. The (0,3) slot's REVERSE selection
    // [3,4) extends right past its own anchor and collapses to an empty
    // selection (anchor == caret == 4), so it survives normalization next
    // to the sweeping one — v1 semantics, pinned here on purpose. The
    // primary's [8,9) extension walks right back onto its anchor (1,4)
    // and likewise collapses to zero-length.
    QTest::keyClick(editor, Qt::Key_Right, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(!editor->hasSelection()); // anchor == caret: zero-length extend
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 4}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 4}, {0, 4}}));
}

void TestCodeEditor::copyWithCaretsJoinsLines()
{
    QClipboard* cb = QApplication::clipboard();
    QVERIFY(cb);
    editor->setPlainText(QStringLiteral("aa\nbb\ncc"));
    altClick(editor, 1, 1); // primary mid line 1, extra at (0,0)

    cb->setText(QStringLiteral("sentinel"), QClipboard::Clipboard);
    editor->copy(); // no selections: distinct caret lines joined by \n
    QCOMPARE(cb->text(QClipboard::Clipboard), QStringLiteral("aa\nbb"));

    // With selections the join covers EVERY slot: Shift+Right fanned out,
    // so both carets selected a character and both texts are joined.
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier);
    editor->copy();
    QCOMPARE(cb->text(QClipboard::Clipboard), QStringLiteral("a\nb"));
}

void TestCodeEditor::cutSelectionsAtCaretsDeletesAll()
{
    QClipboard* cb = QApplication::clipboard();
    QVERIFY(cb);
    editor->setPlainText(QStringLiteral("abcdefg"));
    altDrag(editor, caretPixel(editor, 0, 0), caretPixel(editor, 0, 2)); // primary [0,2)
    altClick(editor, 0, 4); // [0,2) demotes to an extra
    altDrag(editor, caretPixel(editor, 0, 4), caretPixel(editor, 0, 6)); // primary [4,6)

    cb->setText(QStringLiteral("sentinel"), QClipboard::Clipboard);
    QTest::keyClick(editor, Qt::Key_X, Qt::ControlModifier);
    QCOMPARE(cb->text(QClipboard::Clipboard), QStringLiteral("ab\nef")); // doc-order join
    QCOMPARE(editor->toPlainText(), QStringLiteral("cdg")); // BOTH selections cut
}

void TestCodeEditor::cutLinesAtCaretsJoinsClipboard()
{
    QClipboard* cb = QApplication::clipboard();
    QVERIFY(cb);
    editor->setPlainText(QStringLiteral("aa\nbb\ncc"));
    altClick(editor, 1, 1); // carets on lines 0 and 1, none selected

    cb->setText(QStringLiteral("sentinel"), QClipboard::Clipboard);
    QTest::keyClick(editor, Qt::Key_X, Qt::ControlModifier);
    QCOMPARE(cb->text(QClipboard::Clipboard), QStringLiteral("aa\nbb")); // distinct lines
    QCOMPARE(editor->toPlainText(), QStringLiteral("cc")); // whole-line cut per caret
    // Both carets land at the start of the merged deleted block and stack-
    // fold (documented v1: stacked landings collapse in one action).
    QCOMPARE(editor->additionalCaretCount(), 0);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));
}

void TestCodeEditor::pasteFansOutToAllCarets()
{
    QClipboard* cb = QApplication::clipboard();
    QVERIFY(cb);
    editor->setPlainText(QStringLiteral("hello world"));
    altClick(editor, 0, 6); // primary at 'w', extra at (0,0)

    // v1 divergence from VS Code: the SAME text lands at every caret; the
    // multi-line clipboard is never distributed one line per caret.
    cb->setText(QStringLiteral("12\n34"), QClipboard::Clipboard);
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("12\n34hello 12\n34world"));
    // The two inserts are 11 code units apart, so the carets land at (1,2)
    // (extra) and (2,2) (primary) — no stack fold this deep.
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 2}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{1, 2}}));
    editor->undo();
    QCOMPARE(editor->toPlainText(), QStringLiteral("hello world")); // ONE step
    QCOMPARE(editor->additionalCaretCount(), 1); // the pre-action pair is back
}

void TestCodeEditor::undoRestoresAllCaretsOneStepPerPress()
{
    editor->setPlainText(QStringLiteral("ab\ncd"));
    altClick(editor, 1, 2); // primary (1,2), extra at (0,0)
    QTest::keyClick(editor, Qt::Key_X); // 'x' lands at 5 and 0
    QTest::keyClick(editor, Qt::Key_Return); // two presses = two undo steps
    QCOMPARE(editor->toPlainText(), QStringLiteral("x\nab\ncdx\n"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{3, 0}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{1, 0}}));

    editor->undo(); // the Enter step only
    QCOMPARE(editor->toPlainText(), QStringLiteral("xab\ncdx"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 3})); // caret before the Enter
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 1}}));

    editor->undo(); // the X step
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab\ncd"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 2}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}}));
    QVERIFY(!editor->canUndo()); // exactly ONE step per multi-caret press
}

// --- M4 syntax highlighting (Phase D2) -------------------------------------

namespace {

using TK = CodeEditor::TokenKind;
using Span = CodeEditor::HighlightSpan;

QVector<Span> allSpans(CodeEditor* ed)
{
    return ed->highlightSpans(0, 1 << 20); // window clamps to the document
}

int countKind(const QVector<Span>& spans, TK kind)
{
    int n = 0;
    for (const Span& s : spans)
        if (s.kind == kind)
            ++n;
    return n;
}

} // namespace

void TestCodeEditor::setLanguageUnknownFallsBackToPlain()
{
    editor->setPlainText(QStringLiteral("def f(x):\n    return 1  # add\n"));
    QVERIFY(editor->language().isEmpty()); // default: no grammar attached
    QVERIFY(allSpans(editor).isEmpty());

    editor->setLanguage(QStringLiteral("python"));
    QCOMPARE(editor->language(), QStringLiteral("python"));
    QVERIFY(!allSpans(editor).isEmpty());

    // Unknown ids (with case/whitespace noise) return the widget to the
    // pre-M4 plain rendering path and clear previous spans.
    editor->setLanguage(QStringLiteral("  COBOL  "));
    QVERIFY(editor->language().isEmpty());
    QVERIFY(allSpans(editor).isEmpty());

    editor->setLanguage(QString());
    QVERIFY(editor->language().isEmpty());
}

void TestCodeEditor::availableLanguagesShipsPinnedGrammars()
{
    // Registry order = HighlightEngine::languageIds() order; new pinned
    // grammars must be appended here (VENDORED.md lists the upstream pins).
    QCOMPARE(CodeEditor::availableLanguages(),
             QStringList({QStringLiteral("python"), QStringLiteral("json"),
                          QStringLiteral("javascript")}));
}

void TestCodeEditor::pythonSpansMapToModelOffsets()
{
    const QString text = QStringLiteral("def f(x):\n    return 1  # add\n");
    editor->setPlainText(text);
    editor->setLanguage(QStringLiteral("python"));

    // UTF-16 code-unit offsets, [start, end), the model's coordinate space.
    // `f` is Function: the query's function_definition pattern refines the
    // (identifier) @variable catch-all, and under our later-pattern-wins
    // merge (tree-sitter master's convention) the refined role wins over
    // the identical-span catch-all. `x` stays Variable: the pinned python
    // query has no parameter-role capture, so only the catch-all matches
    // it. The engine is verified against the committed queries, not an
    // idealized theme; customizing .scm files is a later milestone (see
    // VENDORED.md).
    const QVector<Span> expected = {
        {0, 3, TK::Keyword},            // def
        {4, 5, TK::Function},           // f
        {6, 7, TK::Variable},           // x
        {14, 20, TK::Keyword},          // return
        {21, 22, TK::Number},           // 1
        {24, 29, TK::Comment},          // # add
    };
    QVERIFY(allSpans(editor) == expected);

    // Sorted and non-overlapping is the engine's merge invariant; the
    // paint path consumes it as-is.
    const auto spans = allSpans(editor);
    for (int i = 1; i < spans.size(); ++i)
        QVERIFY(spans[i - 1].end <= spans[i].start);
}

void TestCodeEditor::jsonSpansMapToModelOffsets()
{
    const QString text = QStringLiteral("{\"a\": 1, \"b\": null}");
    editor->setPlainText(text);
    editor->setLanguage(QStringLiteral("json"));

    const QVector<Span> expected = {
        {1, 4, TK::StringSpecialKey},   // "a" (pair key)
        {6, 7, TK::Number},             // 1
        {9, 12, TK::StringSpecialKey},  // "b"
        {14, 18, TK::ConstantBuiltin},  // null
    };
    QVERIFY(allSpans(editor) == expected);
}

void TestCodeEditor::javascriptSpansMapToModelOffsets()
{
    const QString text = QStringLiteral("function f(x) { return x + 1; } // c\n");
    editor->setPlainText(text);
    editor->setLanguage(QStringLiteral("javascript"));

    const QVector<Span> expected = {
        {0, 8, TK::Keyword},                  // function
        {9, 10, TK::Function},                // f: the query's function_
        {10, 11, TK::PunctuationBracket},     // declaration pattern refines
        {11, 12, TK::Variable},               // the (identifier) @variable
        {12, 13, TK::PunctuationBracket},     // catch-all (later-wins merge)
        {14, 15, TK::PunctuationBracket},     // {
        {16, 22, TK::Keyword},                // return
        {23, 24, TK::Variable},               // x
        {25, 26, TK::Operator},               // +
        {27, 28, TK::Number},                 // 1
        {28, 29, TK::PunctuationDelimiter},   // ;
        {30, 31, TK::PunctuationBracket},     // }
        {32, 36, TK::Comment},                // // c
    };
    QVERIFY(allSpans(editor) == expected);
}

void TestCodeEditor::jsonKeyQueryPatternWinsOverPlainString()
{
    // The json query captures the pair-key node TWICE: (string) and, with
    // a LATER pattern, (_) @string.special.key - identical [start, end)
    // nodes, a genuine query conflict. The merge resolves it tree-sitter
    // master's way: the LATER pattern wins, which is why the locally
    // reordered json highlights.scm (VENDORED.md) puts the key capture
    // after (string). Flip the merge's pattern tie-break and the keys
    // degrade to plain String while everything else stays green - this is
    // that tripwire, the expectations are not arbitrary theme picks.
    const QString text = QStringLiteral("{\"k\": \"v\"}");
    editor->setPlainText(text);
    editor->setLanguage(QStringLiteral("json"));
    const QVector<Span> expected = {
        {1, 4, TK::StringSpecialKey},  // key wins the conflict
        {6, 9, TK::String},            // plain value string (offsets: the
    };                                 // colon+space sit at 4..6, NOT 7..9)
    QVERIFY(allSpans(editor) == expected);
}

void TestCodeEditor::queryPredicatesFilterRefinedRoles()
{
    // The tree-sitter runtime does NOT evaluate query predicates - the
    // cursor emits every syntactically matching capture (probe-verified
    // against the v0.27.2 pinned runtime). The engine applies #eq?/#match?
    // itself; this test is that tripwire. Without filtering, the js #eq?
    // "require" pattern would fire on EVERY call-function identifier
    // (log -> FunctionBuiltin) and the #match? role patterns would fire
    // on every capitalized name everywhere, stompable only by even later
    // patterns - i.e. colors would collapse exactly the way the pre-M4.1
    // first-wins merge hid them.
    editor->setPlainText(QStringLiteral("const TokenCounter = 1;\nclass Stock {}\nconsole.log(1);\nrequire(\"x\");\n"));
    editor->setLanguage(QStringLiteral("javascript"));
    const QVector<Span> js = allSpans(editor);
    // Selected roles only; punctuation/strings are asserted elsewhere.
    QVERIFY(js.contains(Span{6, 18, TK::Constructor}));  // #match? "^[A-Z]"
    QVERIFY(js.contains(Span{30, 35, TK::Constructor})); // class name
    QVERIFY(js.contains(Span{39, 46, TK::VariableBuiltin})); // #match? console
    QVERIFY(js.contains(Span{47, 50, TK::FunctionMethod}));  // plain method
    QVERIFY(js.contains(Span{55, 62, TK::FunctionBuiltin})); // #eq? require
    QVERIFY(!js.contains(Span{47, 50, TK::FunctionBuiltin})); // log != require

    editor->setPlainText(QStringLiteral("len(x)\nprint(y)\nZED = 1\n"));
    editor->setLanguage(QStringLiteral("python"));
    const QVector<Span> py = allSpans(editor);
    // py #match? builtin list: len/print pass it; ZED hits the constant
    // "^[A-Z][A-Z_]*$" pattern. Predicates IGNORED, EVERY py/js call name
    // would come out FunctionBuiltin and every capitalized identifier
    // Constructor - so these exact full tables pin the filtering itself.
    const QVector<Span> pyExpected = {
        {0, 3, TK::FunctionBuiltin},    // len (#match? builtin list)
        {4, 5, TK::Variable},           // x
        {7, 12, TK::FunctionBuiltin},   // print
        {13, 14, TK::Variable},         // y
        {16, 19, TK::Constant},         // ZED (#match? "^[A-Z][A-Z_]*$")
        {20, 21, TK::Operator},         // =
        {22, 23, TK::Number},           // 1
    };
    QVERIFY(py == pyExpected);
}

void TestCodeEditor::spansClipToRequestedWindow()
{
    const QString text = QStringLiteral("def f(x):\n    return 1  # add\n");
    editor->setPlainText(text);
    editor->setLanguage(QStringLiteral("python"));

    // Window [18, 22): intersects `return` (14..20) and `1` (21..22) only.
    const QVector<Span> expected = {
        {18, 20, TK::Keyword},
        {21, 22, TK::Number},
    };
    QVERIFY(editor->highlightSpans(18, 22) == expected);

    // Out-of-range windows clamp; degenerate ranges are empty, not the
    // spans shifted into the window.
    QVERIFY(editor->highlightSpans(-5, 1 << 20) == allSpans(editor));
    QVERIFY(editor->highlightSpans(1000, 2000).isEmpty());
    QVERIFY(editor->highlightSpans(5, 5).isEmpty());
    QVERIFY(editor->highlightSpans(7, 3).isEmpty());
}

void TestCodeEditor::stringSpanCoversSurrogatePairWhole()
{
    const QString text = QStringLiteral("x = \"\U0001F600\"\n");
    editor->setPlainText(text);
    editor->setLanguage(QStringLiteral("python"));
    QVERIFY(QChar(text[5]).isHighSurrogate()); // the pair really is 2 units
    QVERIFY(QChar(text[6]).isLowSurrogate());

    const auto spans = allSpans(editor);
    bool stringFound = false;
    for (const Span& s : spans) {
        if (s.kind == TK::String) {
            stringFound = true;
            QVERIFY(s.start <= 5 && s.end >= 7); // covers BOTH surrogates
        }
        // No token edge may cut INSIDE the pair at offset 6: tree-sitter
        // scans UTF-16LE and emits the string as one node. (Painting snaps
        // run edges to grapheme boundaries on top of this; the model must
        // not split it in the first place.)
        QVERIFY(s.start != 6);
        QVERIFY(s.end != 6);
    }
    QVERIFY(stringFound);
}

void TestCodeEditor::typedQuotesCloseStringIncrementally()
{
    // The ts_tree_edit-per-event + deferred-reparse pipeline must keep the
    // tree current across edits AND undos: a String span exists exactly
    // while the text contains a closed string.
    editor->setPlainText(QStringLiteral("s = abc"));
    editor->setLanguage(QStringLiteral("python"));
    const QVector<Span> baseline = allSpans(editor);
    QCOMPARE(baseline.size(), 3); // s, =, abc - no string yet
    QCOMPARE(countKind(baseline, TK::String), 0);

    // setPosition() deliberately breaks the undo chain (proven by
    // undoCaretMoveBreaksChain), so each keyClicks segment below is one
    // undo step. Typing an OPEN quote must not produce a String span...
    editor->setPosition({0, 7});
    QTest::keyClicks(editor, "\"");
    // Unterminated string: no String node (the other spans stay as-is).
    QCOMPARE(countKind(allSpans(editor), TK::String), 0);
    editor->setPosition({0, 8});
    QTest::keyClicks(editor, "abc");
    QCOMPARE(countKind(allSpans(editor), TK::String), 0); // still open
    editor->setPosition({0, 11});
    QTest::keyClicks(editor, "\""); // closes it
    QCOMPARE(editor->toPlainText(), QStringLiteral("s = abc\"abc\""));
    const auto closed = allSpans(editor);
    QVERIFY(countKind(closed, TK::String) == 1);
    for (const Span& s : closed)
        if (s.kind == TK::String) {
            QCOMPARE(s.start, 7); // the "abc" run incl. both quotes
            QCOMPARE(s.end, 12);
        }

    editor->undo(); // closing-quote chunk
    QCOMPARE(editor->toPlainText(), QStringLiteral("s = abc\"abc"));
    QCOMPARE(countKind(allSpans(editor), TK::String), 0);
    editor->undo(); // "abc" chunk
    QCOMPARE(editor->toPlainText(), QStringLiteral("s = abc\""));
    QCOMPARE(countKind(allSpans(editor), TK::String), 0);
    editor->undo(); // opening-quote chunk
    QCOMPARE(editor->toPlainText(), QStringLiteral("s = abc"));
    QVERIFY(allSpans(editor) == baseline);
}

void TestCodeEditor::coloredPaintChangesCommentInk()
{
    // Numeric pixel tripwire for the colored paint path (the only probe
    // available - no vision): green-dominant ink on the `return 1  # add`
    // line comes ONLY from Comment (#6A9955) and Number (#B5CEA8); the
    // Keyword blue and neutral inks are never green-dominant. Plain path
    // must draw ZERO green-dominant pixels there, colored path some
    // (measured 185 here; threshold stays far below to survive CI fonts).
    editor->setPlainText(QStringLiteral("def f(x):\n    return 1  # add\n"));
    const int lh = editor->lineHeight();
    const int pad = editor->padding();
    const int yTop = pad + lh; // line 1 band
    auto greenInLine1 = [&]() {
        const QImage img = editor->grab().toImage();
        int n = 0;
        for (int x = 0; x < img.width(); ++x)
            for (int y = yTop; y < yTop + lh && y < img.height(); ++y) {
                const QRgb px = img.pixel(x, y);
                if (qGreen(px) > qRed(px) + 10 && qGreen(px) > qBlue(px) + 10)
                    ++n;
            }
        return n;
    };
    QCOMPARE(greenInLine1(), 0);
    editor->setLanguage(QStringLiteral("python"));
    QVERIFY(greenInLine1() >= 20);
}

void TestCodeEditor::coloredPaintKeepsGlyphGeometry()
{
    // The colored path paints the SAME cached layout through
    // QTextLayout::draw(FormatRanges) instead of gs->line.draw - it must
    // change COLORS only, never glyph placement. Comparing whole bands
    // pixel-for-pixel cannot work (every non-empty python line receives
    // some capture, measured: even a lone `plain` becomes Variable), so
    // compare INK PROFILES instead: the set of columns containing ink
    // and the rows the ink spans must be identical before/after enabling
    // the grammar. Caret is parked past the text end and scanned out of
    // the window (grab() paints it; blink state could differ per grab).
    editor->setPlainText(QStringLiteral("def f(x):\n"));
    editor->setPosition({0, 12}); // caret past the text end (text is 10 wide)
    const QImage before = editor->grab().toImage();
    editor->setLanguage(QStringLiteral("python"));
    const QImage after = editor->grab().toImage();
    const int lh = editor->lineHeight();
    const int pad = editor->padding();
    const int caretX = pad + int(editor->cellWidth() * 12);

    const QRgb base = before.pixel(0, 0); // padding corner = Base fill
    QVector<bool> colB(caretX, false), colA(caretX, false);
    QVector<bool> rowB(lh, false), rowA(lh, false);
    int changed = 0;
    for (int y = 0; y < lh; ++y) {
        const int yy = pad + y;
        QVERIFY2(yy < before.height() && yy < after.height(), "band outside grab");
        for (int x = 0; x < caretX; ++x) {
            const QRgb pb = before.pixel(x, yy);
            const QRgb pa = after.pixel(x, yy);
            if (pb != base)
                colB[x] = rowB[y] = true;
            if (pa != base)
                colA[x] = rowA[y] = true;
            if (pb != pa)
                ++changed;
        }
    }
    QVERIFY(colB == colA); // same ink columns
    QVERIFY(rowB == rowA); // same vertical ink extent
    QVERIFY(changed > 0);  // colors really changed (not a vacuous pass)
}

void TestCodeEditor::largeDocViewportPolicyKeepsSpansExact()
{
    // The D2 adaptive policy (engine constants in HighlightEngine, bench
    // in tools/bench_ts_split.cpp) scopes parse+query to the visible
    // window once a document is big (>= 16k units) and a viewport has
    // been painted. This test pins the CONTRACT: the policy must be
    // invisible to the public highlightSpans() API. Every request window
    // - inside the viewport, inside the parsed tree, or beyond both -
    // returns exactly the spans the never-painted (full-tree) reference
    // editor produces.
    auto buildDoc = [] {
        QString text;
        for (int b = 0; b < 400; ++b)
            text += QStringLiteral("def func%1(arg_a, arg_b):\n"
                                   "    total = arg_a + arg_b  # accumulate func%1\n"
                                   "    if total > 100:\n"
                                   "        return \"value-%1\" % total\n"
                                   "    return [x for x in range(arg_a, arg_b)]\n\n")
                        .arg(b);
        return text;
    };
    const QString text = buildDoc();
    QVERIFY2(text.size() >= 16 * 1024, "fixture below the policy size floor (kViewportMinUnits)");

    // Reference through the SAME public API but the full-tree path: a
    // never-painted editor never feeds a viewport, so the engine stays
    // unscoped for it.
    CodeEditor ref;
    ref.setPlainText(text);
    ref.setLanguage(QStringLiteral("python"));
    const QVector<Span> reference = allSpans(&ref);
    QVERIFY(reference.size() > 1000); // sanity: the doc really produces lots of ink

    editor->setPlainText(text);
    editor->setLanguage(QStringLiteral("python"));

    // Block b occupies lines 6b..6b+4 plus a blank line at 6b+5; its
    // comment marker sits on line 6b+1.
    auto commentAt = [&](int b) {
        const QString marker = QStringLiteral("# accumulate func%1\n").arg(b);
        const int p = text.indexOf(marker);
        Q_ASSERT(p >= 0);
        return Span{p, p + int(marker.size()) - 1, TK::Comment}; // span ends before '\n'
    };
    auto hasSpan = [](const QVector<Span>& spans, const Span& want) {
        return spans.contains(want);
    };

    // 1) Cold first paint: one full parse (tree covers the whole doc),
    // but the capture walk is already window-scoped (the policy's query
    // half needs only a viewport, not the parse-mode flip).
    editor->grab(); // synchronous paint feeds the viewport [line0 .. line ~23]

    const Span top = commentAt(0); // first block: inside the first window
    QVERIFY(hasSpan(editor->highlightSpans(top.start, top.end + 5), top));

    const Span tail = commentAt(399); // far outside: served by a re-query
    // on the still-current full tree (coverage, not escalation).
    QVERIFY(hasSpan(editor->highlightSpans(tail.start, tail.end + 5), tail));

    // 2) One wheel throw + repaint: the engine (in Viewport parse mode by
    // now - python's incremental full reparse busts the budget at this
    // size) re-parses ONLY the new window anchored at the nearest col-0
    // line above. Block 200 (comment line 1201) must come out of that
    // window-parsed tree byte-exact vs the reference.
    QWheelEvent wheel(QPointF(300, 200), QPointF(300, 200), QPoint(0, 0),
                      QPoint(0, -120 * 400), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false); // one throw down 1200 lines
    QVERIFY(QApplication::sendEvent(editor, &wheel));
    QVERIFY(editor->firstVisibleLine() > 1000); // the throw landed deep in the doc
    editor->grab();

    const Span mid = commentAt(200);
    QVERIFY(editor->firstVisibleLine() <= 6 * 200 + 1);
    QVERIFY(editor->firstVisibleLine() + editor->visibleLineCount() >= 6 * 200 + 1);
    QVERIFY(hasSpan(editor->highlightSpans(mid.start, mid.end + 5), mid));

    // Strongest window check: EVERY span of block 200 equals the
    // reference's spans over the same byte-exact block bounds.
    const int blockStart = text.indexOf(QStringLiteral("def func200("));
    const int blockEnd = text.indexOf(QStringLiteral("def func201("));
    QVector<Span> refBlock;
    for (const Span& s : reference)
        if (s.start >= blockStart && s.end <= blockEnd)
            refBlock.push_back(s);
    QVERIFY(editor->highlightSpans(blockStart, blockEnd) == refBlock);

    // 3) Typing inside the window: the ts_tree_edit pipeline plus the
    // viewport reparse must keep the comment's geometry exact. setPosition
    // breaks the undo chain (undoCaretMoveBreaksChain), so one Ctrl+Z
    // reverts the whole "# hi" burst.
    const int markerLine = 6 * 200 + 1;
    const int markerCol = mid.start - (text.lastIndexOf('\n', mid.start - 1) + 1);
    editor->setPosition({markerLine, markerCol + 1}); // just after the '#'
    QTest::keyClicks(editor, "# hi");
    editor->grab(); // paints the dirty window: incremental viewport reparse
    const Span grown{mid.start, mid.end + 4, TK::Comment};
    QVERIFY(hasSpan(editor->highlightSpans(grown.start, grown.end + 5), grown));

    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), text); // burst is ONE undo step
    editor->grab();
    QVERIFY(hasSpan(editor->highlightSpans(mid.start, mid.end + 5), mid));

    // 4) Escalation exactness: a whole-document dump can't be served by
    // any window tree, so spansFor pays one synchronous full pass; the
    // result must equal the full-tree reference element-for-element.
    QVERIFY(allSpans(editor) == reference);
}

// --- decorations + gutter (M5, Phase D3) ---------------------------------

void TestCodeEditor::gutterDefaultAndOffCollapsesGeometry()
{
    // Monaco parity: gutter is On by default, and contentLeft() is the x
    // anchor all text geometry converges on.
    QCOMPARE(editor->lineNumbersMode(), CodeEditor::LineNumbersMode::On);
    editor->setPlainText(QStringLiteral("abc"));
    QVERIFY(editor->gutterWidth() > 0);
    QVERIFY(editor->contentLeft() > editor->padding());

    // Off mode collapses the pre-M5 geometry bit-for-bit: the gutter
    // leaves no width and xForCol lands exactly on the old padding edge.
    editor->setLineNumbersMode(CodeEditor::LineNumbersMode::Off);
    QCOMPARE(editor->gutterWidth(), 0);
    QCOMPARE(editor->contentLeft(), editor->padding());
    QCOMPARE(editor->xForCol(0, 0), double(editor->padding()));
    QCOMPARE(editor->lineNumberText(0), QString());
}

void TestCodeEditor::gutterWidthGrowsAtLineCountCrossover()
{
    const double cw = editor->cellWidth();
    editor->setPlainText(QStringLiteral("1\n2\n3\n4\n5\n6\n7\n8\n9")); // 9 lines: 1 digit
    const int w9 = editor->gutterWidth();
    QVERIFY(w9 > 0); // default On
    editor->setPlainText(QStringLiteral("1\n2\n3\n4\n5\n6\n7\n8\n9\n10")); // 10: 2 digits
    const int w10 = editor->gutterWidth();
    // DoD: the digit column is qRound(digits * cw), so the 9->10 line
    // crossover (1 digit -> 2 digits) grows the gutter by one digit step.
    QCOMPARE(w10 - w9, qRound(2 * cw) - qRound(cw));

    // Same digit width in every numbering mode (relative distances and
    // interval absolutes never exceed lineCount digits).
    editor->setLineNumbersMode(CodeEditor::LineNumbersMode::Relative);
    QCOMPARE(editor->gutterWidth(), w10);
    editor->setLineNumbersMode(CodeEditor::LineNumbersMode::Interval);
    QCOMPARE(editor->gutterWidth(), w10);
    editor->setLineNumbersMode(CodeEditor::LineNumbersMode::On);

    // A reserved breakpoint slot appears while ANY breakpoint exists and
    // then stays constant: adding/moving a dot must not shift the digits.
    editor->setBreakpointLines({3});
    const int wBp = editor->gutterWidth();
    QVERIFY(wBp > w10);
    editor->setBreakpointLines({5});
    QCOMPARE(editor->gutterWidth(), wBp);
    editor->setBreakpointLines({});
    QCOMPARE(editor->gutterWidth(), w10);
}

void TestCodeEditor::gutterLineNumbersModeMath()
{
    QStringList rows;
    for (int i = 1; i <= 30; ++i)
        rows << QString::number(i);
    editor->setPlainText(rows.join(QLatin1Char('\n'))); // exactly 30 lines

    // On: 1-based display of the absolute number (the widget stays 0-based).
    editor->grab(); // smoke: On-mode number painting
    QCOMPARE(editor->lineNumberText(0), QStringLiteral("1"));
    QCOMPARE(editor->lineNumberText(29), QStringLiteral("30"));
    QCOMPARE(editor->lineNumberText(30), QString()); // out of range: nothing drawn

    // Relative: distance to the caret line; the caret line itself shows
    // its absolute number (Monaco's relative-mode behavior).
    editor->setLineNumbersMode(CodeEditor::LineNumbersMode::Relative);
    editor->setPosition({15, 0});
    QCOMPARE(editor->lineNumberText(15), QStringLiteral("16")); // the caret line
    QCOMPARE(editor->lineNumberText(13), QStringLiteral("2"));
    QCOMPARE(editor->lineNumberText(20), QStringLiteral("5"));
    // The math follows the caret (the gutter repaints with it).
    editor->setPosition({29, 0});
    QCOMPARE(editor->lineNumberText(28), QStringLiteral("1"));
    QCOMPARE(editor->lineNumberText(29), QStringLiteral("30"));

    // Interval: the absolute number only on 1-based multiples of 10
    // (Monaco's interval behavior); elsewhere nothing is drawn.
    editor->setLineNumbersMode(CodeEditor::LineNumbersMode::Interval);
    editor->grab(); // smoke: sparse interval numbers + blank rows paint
    QCOMPARE(editor->lineNumberText(9), QStringLiteral("10"));
    QCOMPARE(editor->lineNumberText(19), QStringLiteral("20"));
    QCOMPARE(editor->lineNumberText(0), QString());
    QCOMPARE(editor->lineNumberText(5), QString());
}

void TestCodeEditor::decorationsRoundTripUpdateRemove()
{
    editor->setPlainText(QStringLiteral("aaa\nbbb\nccc\nddd"));
    using Diag = CodeEditor::Diagnostic;
    // add = set: the getter returns the items sorted by start position.
    const Diag d1{2, 1, 2, 4, CodeEditor::Severity::Warning, QStringLiteral("w")};
    const Diag d0{0, 0, 0, 3, CodeEditor::Severity::Error, QStringLiteral("e")};
    editor->setDiagnostics({d1, d0});
    QVERIFY2(editor->diagnostics() == (QVector<Diag>{d0, d1}), "diagnostics not sorted round-trip");
    // update = set again with changed values (message survives round-trip).
    Diag d1b = d1;
    d1b.severity = CodeEditor::Severity::Hint;
    d1b.message = QStringLiteral("later");
    editor->setDiagnostics({d1b});
    QVERIFY(editor->diagnostics() == (QVector<Diag>{d1b}));
    // remove = set without the item.
    editor->setDiagnostics({});
    QVERIFY(editor->diagnostics().isEmpty());
    // Out-of-document items are stored verbatim (lineCount moves under
    // edits without losing consumer state); they just never paint.
    const Diag far{99, 0, 99, 2, CodeEditor::Severity::Information, {}};
    editor->setDiagnostics({far});
    QVERIFY(editor->diagnostics() == (QVector<Diag>{far}));

    // Breakpoints: stored sorted + deduplicated, cleared by empty set.
    editor->setBreakpointLines({5, 1, 3, 1});
    QCOMPARE(editor->breakpointLines(), (QVector<int>{1, 3, 5}));
    editor->setBreakpointLines({});
    QVERIFY(editor->breakpointLines().isEmpty());

    // Inlay hints sort by (line, col) and round-trip with their text.
    using Hint = CodeEditor::InlayHint;
    const Hint h1{1, 3, QStringLiteral(": int")};
    const Hint h0{0, 8, QStringLiteral(" // x")};
    editor->setInlayHints({h1, h0});
    QVERIFY(editor->inlayHints() == (QVector<Hint>{h0, h1}));
    editor->setInlayHints({});
    QVERIFY(editor->inlayHints().isEmpty());

    // Whole-line bands: swapped args are tolerated; clear reports -1.
    editor->setHighlightedLines(3, 1);
    QCOMPARE(editor->highlightedLinesStart(), 1);
    QCOMPARE(editor->highlightedLinesEnd(), 3);
    editor->clearHighlightedLines();
    QCOMPARE(editor->highlightedLinesStart(), -1);
    QCOMPARE(editor->highlightedLinesEnd(), -1);

    // Current-line band defaults off (the set_theme pass picks the
    // shipped default, D3 theme row).
    QVERIFY(!editor->currentLineHighlight());
    editor->setCurrentLineHighlight(true);
    QVERIFY(editor->currentLineHighlight());

    // Paint smoke: every layer live in one frame (the state checks above
    // stay pixel-free; this only proves the composite paint path runs -
    // bands, gutter glyphs, wavy underlines and hints together).
    editor->setBreakpointLines({0, 2});
    editor->setInlayHints({h1});
    editor->setHighlightedLines(1, 2);
    editor->setDiagnostics({d0, d1});
    editor->setLineNumbersMode(CodeEditor::LineNumbersMode::Relative);
    QVERIFY(!editor->grab().toImage().isNull());
}

void TestCodeEditor::gutterShiftsTextGeometryAndHitTest()
{
    editor->setPlainText(QStringLiteral("abcdef\nxyz"));
    const int lh = editor->lineHeight();
    const double cw = editor->cellWidth();
    const int x0 = editor->contentLeft();
    QVERIFY(x0 > editor->padding()); // default On: text sits right of the gutter

    // xForCol anchors on contentLeft and keeps the ASCII grid cell-per-col.
    QCOMPARE(editor->xForCol(0, 0), double(x0));
    QVERIFY(qAbs(editor->xForCol(0, 3) - (x0 + 3 * cw)) < 1e-9);

    // A click inside the gutter clamps to col 0 of that line.
    const auto gp = editor->positionAt(QPoint(editor->padding() + 1, editor->padding() + lh + lh / 2));
    QCOMPARE(gp.line, 1);
    QCOMPARE(gp.col, 0);

    // A cell-center click (the editor's hit convention) round-trips
    // through the shifted coordinate frame.
    const auto cp = editor->positionAt(
        QPoint(qRound(editor->xForCol(0, 3) + cw / 2), editor->padding() + lh / 2));
    QCOMPARE(cp.line, 0);
    QCOMPARE(cp.col, 3);
}

void TestCodeEditor::diagnosticUnderlinePaintsSeverityInk()
{
    // Numeric pixel probe (no vision): red-dominant ink must appear in
    // the bottom band of line 1 only after an Error diagnostic arrives,
    // and the severity switch must move the ink off the red band.
    // Warning ink (255,204,0) stays below the red-dominant threshold
    // even when antialiased toward the dark Base (#323232).
    editor->setPlainText(QStringLiteral("plain\nmarked up\n"));
    const int lh = editor->lineHeight();
    const int pad = editor->padding();
    const int yTop = pad + lh; // line 1 band
    const int x0 = editor->contentLeft();
    const int x1 = x0 + int(9 * editor->cellWidth());
    const auto reddish = [](const QColor& c) {
        return c.red() > c.green() + 60 && c.red() > c.blue() + 60;
    };
    auto redInk = [&] {
        const QImage img = editor->grab().toImage();
        int n = 0;
        for (int x = x0; x < x1 && x < img.width(); ++x)
            for (int y = yTop; y < yTop + lh && y < img.height(); ++y)
                if (reddish(img.pixelColor(x, y)))
                    ++n;
        return n;
    };

    QCOMPARE(redInk(), 0); // plain text rows: zero red-dominant ink

    using Diag = CodeEditor::Diagnostic;
    editor->setDiagnostics({Diag{1, 0, 1, 9, CodeEditor::Severity::Error, QStringLiteral("boom")}});
    const int n = redInk();
    QVERIFY2(n >= 10, qPrintable(QStringLiteral("wavy underline drew only %1 red pixels").arg(n)));

    editor->setDiagnostics({Diag{1, 0, 1, 9, CodeEditor::Severity::Warning, QStringLiteral("hmm")}});
    QCOMPARE(redInk(), 0); // Warning is yellow-dominant: out of the red band
}

// ---- M6 find & replace (Phase D4) ---------------------------------------
//
// M6 pixel facts (measured, this environment): the match-family ink is a
// fixed VS Code-dark constant (the renderer's findMatchInk): all matches
// (196,136,30) at alpha 80, the current one (246,169,0) at alpha 130.
// Premultiplied over the dark Base #323232 they land near (96,77,44) and
// (150,111,25): red-dominant AND warm (G clearly above B). Diagnostic
// error ink (241,76,76) over Base is (110,58,58) - G == B, cool. So the
// warm probe below never counts the M5 diagnostic band, and the M5 probe
// (red > green+60) never counts quads (their red-minus-green stays <= 40).
// Neutral gray text/gutter ink and blue selection fail the warm test
// outright - both probes stay exclusive on the same rows by construction.

namespace qmn::view {
// Streamed for QCOMPARE failure output: ADL must see it, so it lives in
// the type's namespace (a global operator would be invisible to QtTest's
// toString). The defaulted operator== is what the DoD assertions compare.
QDebug operator<<(QDebug d, const CodeEditor::MatchRange& m)
{
    return d << "MatchRange(" << m.startLine << ", " << m.startCol << ", "
             << m.endLine << ", " << m.endCol << ')';
}
} // namespace qmn::view

namespace {

using MR = CodeEditor::MatchRange;

QLineEdit* barField(CodeEditor* e, const QString& placeholder)
{
    for (QLineEdit* le : e->findChildren<QLineEdit*>())
        if (le->placeholderText() == placeholder)
            return le;
    return nullptr;
}

QPushButton* barButton(CodeEditor* e, const QString& text, bool prefix = false)
{
    for (QPushButton* b : e->findChildren<QPushButton*>())
        if (prefix ? b->text().startsWith(text) : b->text() == text)
            return b;
    return nullptr;
}

QWidget* findBarOf(CodeEditor* e)
{
    return e->findChild<QWidget*>(QStringLiteral("qmn-find-bar"));
}

// Count warm match-quad ink in a row window, right of the gutter. The
// negative control (no matches -> exactly 0) makes every other pixel on
// those rows provably non-warm, so no per-case exclusion is needed.
int warmQuadPixels(const QImage& img, int xFrom, int yFrom, int yTo)
{
    int n = 0;
    for (int y = yFrom; y < yTo && y < img.height(); ++y)
        for (int x = xFrom; x < img.width(); ++x) {
            const QRgb px = img.pixel(x, y);
            if (qRed(px) >= qGreen(px) + 10 && qGreen(px) >= qBlue(px) + 10
                && qRed(px) <= 200 && qGreen(px) >= 55)
                ++n;
        }
    return n;
}

} // namespace

void TestCodeEditor::findLiteralRangesCaseFlagAndEscapedMeta()
{
    editor->setPlainText(QStringLiteral("foo Foo FOO xfoox"));
    QVERIFY(editor->findText().isEmpty());
    QCOMPARE(editor->matchCount(), 0); // an empty query never searches
    QVERIFY(editor->findPatternValid());
    QVERIFY(editor->findFlags() == CodeEditor::FindFlag::MatchCase); // default

    editor->setFindText(QStringLiteral("foo"));
    QCOMPARE(editor->matchCount(), 2); // substring hits, but case-sensitive
    QCOMPARE(editor->matches(),
             (QVector<MR>{MR{0, 0, 0, 3}, MR{0, 13, 0, 16}}));

    editor->setFindFlags(CodeEditor::FindFlags()); // MatchCase off
    QCOMPARE(editor->matchCount(), 4);
    editor->setFindFlags(CodeEditor::FindFlag::MatchCase);
    QCOMPARE(editor->matchCount(), 2);

    // Literal mode escapes regex metacharacters into the same engine.
    editor->setFindText(QStringLiteral("o.o")); // would hit "ooo" as regex
    QCOMPARE(editor->matchCount(), 0);
    editor->setFindText(QStringLiteral("FOO xfoo")); // a space is literal too
    QCOMPARE(editor->matchCount(), 1);

    editor->setFindText(QString()); // empty clears the set
    QCOMPARE(editor->matchCount(), 0);
    QVERIFY(editor->findPatternValid());
}

void TestCodeEditor::findWholeWordFlag()
{
    editor->setPlainText(QStringLiteral("foobar foo barfoo foo_bar foo-bar"));
    editor->setFindText(QStringLiteral("foo"));
    // The word boundary is regex-CLASS based (\w: underscore included):
    // the documented v1 approximation of VS Code's Unicode-aware \b.
    editor->setFindFlags(CodeEditor::FindFlag::MatchCase | CodeEditor::FindFlag::WholeWord);
    QCOMPARE(editor->matchCount(), 2);
    QCOMPARE(editor->matches(),
             (QVector<MR>{MR{0, 7, 0, 10}, MR{0, 26, 0, 29}}));

    editor->setFindFlags(CodeEditor::FindFlag::MatchCase);
    QCOMPARE(editor->matchCount(), 5); // foobar/barfoo/foo_bar hits come back
}

void TestCodeEditor::findRegexFlagInvalidPatternAndPerLineEngine()
{
    editor->setPlainText(QStringLiteral("a12 bb x9"));
    editor->setFindFlags(CodeEditor::FindFlag::MatchCase | CodeEditor::FindFlag::UseRegex);
    editor->setFindText(QStringLiteral("\\d+|x+|b+"));
    QCOMPARE(editor->matchCount(), 4);
    QCOMPARE(editor->matches(),
             (QVector<MR>{MR{0, 1, 0, 3}, MR{0, 4, 0, 6},
                          MR{0, 7, 0, 8}, MR{0, 8, 0, 9}}));

    // Capture groups: the reported range is the FULL match (group 0),
    // not the groups (which only refine what counts as a match).
    editor->setFindText(QStringLiteral("(\\d)(\\d)"));
    QCOMPARE(editor->matchCount(), 1);
    QCOMPARE(editor->matches().at(0), (MR{0, 1, 0, 3}));

    // An invalid pattern must be tellable from "no matches".
    editor->setFindText(QStringLiteral("(unclosed"));
    QCOMPARE(editor->matchCount(), 0);
    QVERIFY(!editor->findPatternValid());
    editor->setFindText(QStringLiteral("x"));
    QCOMPARE(editor->matchCount(), 1);
    QVERIFY(editor->findPatternValid());

    // The engine is PER-LINE (UseRegex stays on here): alternation
    // enumerates across lines, but a match (and a pattern mentioning a
    // literal newline) never crosses a line break.
    editor->setPlainText(QStringLiteral("a\nb"));
    editor->setFindText(QStringLiteral("a.b"));
    QCOMPARE(editor->matchCount(), 0);
    editor->setFindText(QStringLiteral("a\nb")); // literal, escaped, still 0
    QCOMPARE(editor->matchCount(), 0);
    editor->setFindText(QStringLiteral("a|b"));
    QCOMPARE(editor->matchCount(), 2);
    QCOMPARE(editor->matches(), (QVector<MR>{MR{0, 0, 0, 1}, MR{1, 0, 1, 1}}));
}

void TestCodeEditor::findMatchesSnapOverSurrogatePairs()
{
    editor->setPlainText(QStringLiteral("a\U0001F600" "b")); // a H L b (4 units)
    QVERIFY(QChar(editor->toPlainText()[1]).isHighSurrogate()); // pair guard
    QVERIFY(QChar(editor->toPlainText()[2]).isLowSurrogate());

    editor->setFindFlags(CodeEditor::FindFlag::MatchCase | CodeEditor::FindFlag::UseRegex);
    editor->setFindText(QStringLiteral("."));
    // PCRE2 in code-unit mode would report FOUR half-cell dots on this
    // 4-unit line; the engine snaps every boundary outward and dedups the
    // snapped pair matches: exactly the three code points, whole.
    QCOMPARE(editor->matchCount(), 3);
    QCOMPARE(editor->matches(),
             (QVector<MR>{MR{0, 0, 0, 1}, MR{0, 1, 0, 3}, MR{0, 3, 0, 4}}));

    editor->replaceAll(QStringLiteral("X"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("XXX"));
    const QString after = editor->toPlainText();
    for (int i = 0; i < after.size(); ++i) { // no orphan halves anywhere
        QVERIFY(!QChar(after[i]).isHighSurrogate());
        QVERIFY(!QChar(after[i]).isLowSurrogate());
    }

    // A literal query holding the pair matches the pair whole (and only
    // the pair): the DoD replacement-safety case.
    editor->setPlainText(QStringLiteral("x\U0001F600\U0001F600y\U0001F600"));
    editor->setFindFlags(CodeEditor::FindFlag::MatchCase);
    editor->setFindText(QStringLiteral("\U0001F600"));
    QCOMPARE(editor->matchCount(), 3);
    QCOMPARE(editor->matches(),
             (QVector<MR>{MR{0, 1, 0, 3}, MR{0, 3, 0, 5}, MR{0, 6, 0, 8}}));
    QCOMPARE(editor->replaceAll(QStringLiteral("p")), 3);
    QCOMPARE(editor->toPlainText(), QStringLiteral("xppyp"));
}

void TestCodeEditor::findNextPreviousCycleWrapAndCurrentMatch()
{
    editor->setPlainText(QStringLiteral("a\naa\naaa"));
    editor->setFindText(QStringLiteral("a"));
    QCOMPARE(editor->matchCount(), 6);
    QCOMPARE(editor->currentMatch(), -1); // a fresh set has no current match

    QVERIFY(editor->findNext()); // no current: first match at/after the caret
    QCOMPARE(editor->currentMatch(), 0);
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("a"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1})); // caret = end

    editor->findNext();
    editor->findNext();
    QCOMPARE(editor->currentMatch(), 2); // (1,1..2)

    // A click elsewhere drops the current match (the selection no longer
    // covers it); the next F3 resumes from the caret, Monaco-style.
    editor->setPosition({1, 0});
    QCOMPARE(editor->currentMatch(), -1);
    QVERIFY(editor->findNext());
    QCOMPARE(editor->currentMatch(), 1); // (1,0..1) at the caret

    QVERIFY(editor->findPrevious());
    QCOMPARE(editor->currentMatch(), 0);
    QVERIFY(editor->findPrevious()); // cycle wraps to the last match
    QCOMPARE(editor->currentMatch(), 5); // (2,2..3)
    QCOMPARE(editor->selectedText(), QStringLiteral("a"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 3}));
    QVERIFY(editor->findNext()); // wraps back to the first
    QCOMPARE(editor->currentMatch(), 0);

    QVERIFY(!editor->gotoMatch(-1));
    QVERIFY(!editor->gotoMatch(6));
    QVERIFY(editor->gotoMatch(5));
    QCOMPARE(editor->currentMatch(), 5);

    editor->setFindText(QStringLiteral("nope"));
    QCOMPARE(editor->currentMatch(), -1);
    QVERIFY(!editor->findNext());
    QVERIFY(!editor->findPrevious());
}

void TestCodeEditor::replaceAllIsOneUndoableAction()
{
    editor->setPlainText(QStringLiteral("foo foo foo\nfoo"));
    editor->setFindText(QStringLiteral("foo"));
    QCOMPARE(editor->matchCount(), 4);
    QVERIFY(!editor->canUndo()); // setPlainText reset the history

    QCOMPARE(editor->replaceAll(QStringLiteral("X")), 4); // D4 DoD core
    QCOMPARE(editor->toPlainText(), QStringLiteral("X X X\nX"));
    QCOMPARE(editor->matchCount(), 0); // recompute after the batch
    QVERIFY(editor->canUndo()); // the batch pushed...
    QVERIFY(!editor->canRedo()); // ...exactly ONE group (no redo branch yet)
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1})); // first end

    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier); // one Ctrl+Z
    QCOMPARE(editor->toPlainText(), QStringLiteral("foo foo foo\nfoo"));
    QVERIFY(!editor->canUndo()); // the WHOLE batch was one action
    QVERIFY(editor->canRedo());
    QCOMPARE(editor->matchCount(), 4); // the set followed the undo
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("X X X\nX")); // one redo

    // An empty replacement deletes every match (still one action).
    editor->setFindText(QStringLiteral("X"));
    QCOMPARE(editor->replaceAll(QString()), 4);
    QCOMPARE(editor->toPlainText(), QStringLiteral("  \n")); // the two separators survive
    QVERIFY(editor->canUndo()); // still ONE group for the empty batch too
    QVERIFY(!editor->canRedo());
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("X X X\nX"));
}

void TestCodeEditor::replaceAllConfirmStepIsButtonOnly()
{
    // D4 "confirm step": no key chord fires the whole-document mutation -
    // Enter-family keys only navigate; the All button is the only trigger.
    editor->setPlainText(QStringLiteral("foo foo foo"));
    editor->showFindBar();
    QLineEdit* q = barField(editor, QStringLiteral("Find"));
    QLineEdit* r = barField(editor, QStringLiteral("Replace"));
    QVERIFY(q && r);
    q->setText(QStringLiteral("foo")); // the bar drives the engine directly
    r->setText(QStringLiteral("X"));
    QCOMPARE(editor->matchCount(), 3);
    QVERIFY(!editor->canUndo());

    QTest::keyClick(q, Qt::Key_Return); // Enter = findNext
    QTest::keyClick(q, Qt::Key_Return, Qt::ShiftModifier); // = findPrevious
    QTest::keyClick(q, Qt::Key_Return, Qt::ControlModifier); // still find
    QCOMPARE(editor->currentMatch(), 0);
    QCOMPARE(editor->toPlainText(), QStringLiteral("foo foo foo"));
    QVERIFY(!editor->canUndo()); // no key replaced ANYTHING

    QPushButton* all = barButton(editor, QStringLiteral("All"), true);
    QVERIFY(all);
    QVERIFY(all->isEnabled());
    QVERIFY(all->text().startsWith(QStringLiteral("All"))); // "All (3)"
    QTest::mouseClick(all, Qt::LeftButton, Qt::NoModifier, all->rect().center());
    QCOMPARE(editor->toPlainText(), QStringLiteral("X X X")); // button-only
    QVERIFY(editor->canUndo()); // one D1 action for the whole batch
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("foo foo foo"));
    QCOMPARE(editor->matchCount(), 3);
}

void TestCodeEditor::findSetRefreshesLiveThroughTyping()
{
    editor->setPlainText(QStringLiteral("foo bar\n"));
    editor->setFindText(QStringLiteral("bar"));
    QCOMPARE(editor->matchCount(), 1);

    // Typing marks the set stale (the single edit observer); the next
    // refresh-on-read recomputes it. Crucially: NO paint runs between
    // the edits (QTest delivers synchronously, no event loop runs), so
    // a stale-set failure here reads 1, not 2.
    editor->setPosition({0, 0});
    QTest::keyClicks(editor, "bar");
    QCOMPARE(editor->toPlainText(), QStringLiteral("barfoo bar\n"));
    QCOMPARE(editor->matchCount(), 2);
    QCOMPARE(editor->matches().last(), (MR{0, 7, 0, 10}));

    // Deleting INSIDE a match shifts its end (nothing "was the match"),
    // so the shifted ranges themselves are the proof of a live recompute.
    QTest::keyClick(editor, Qt::Key_Delete, Qt::ControlModifier); // "foo" tail
    QCOMPARE(editor->toPlainText(), QStringLiteral("bar bar\n"));
    QCOMPARE(editor->matchCount(), 2);
    QCOMPARE(editor->matches(), (QVector<MR>{MR{0, 0, 0, 3}, MR{0, 4, 0, 7}}));
}

void TestCodeEditor::replaceNextOwnUndoStepAndCaretLanding()
{
    editor->setPlainText(QStringLiteral("foo foo bar"));
    editor->setFindText(QStringLiteral("foo"));
    editor->setPosition({0, 0});
    QVERIFY(!editor->canUndo());

    // No current match: the next one at/after the caret is replaced
    // (never a wrap-around — Replace never mutates behind your back).
    QVERIFY(editor->replaceNext(QStringLiteral("X")));
    QCOMPARE(editor->toPlainText(), QStringLiteral("X foo bar"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1})); // repl end
    QVERIFY(editor->canUndo()); // its own bracketed D1 step

    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("foo foo bar"));
    QVERIFY(!editor->canUndo());
    QVERIFY(editor->canRedo()); // exactly one group was on the stack
    QCOMPARE(editor->matchCount(), 2); // the set followed the undo

    QVERIFY(editor->replaceNext(QStringLiteral("Y")));
    QCOMPARE(editor->toPlainText(), QStringLiteral("Y foo bar"));

    // Nothing at/after the caret: false, no mutation.
    editor->setPosition({0, 99}); // clamps to the line end
    QVERIFY(!editor->replaceNext(QStringLiteral("Z")));
    QCOMPARE(editor->toPlainText(), QStringLiteral("Y foo bar"));
}

void TestCodeEditor::findBarOpenClosePersistsAndDrivesEngine()
{
    // findBarVisible() reads QWidget::isVisible(), which is false while any
    // ancestor is hidden - so this test needs the editor shown (the engine
    // tests above deliberately never show it).
    editor->show();
    editor->setPlainText(QStringLiteral("foo bar foo baz foo"));
    editor->setFindText(QStringLiteral("foo"));
    QCOMPARE(editor->matchCount(), 3);
    QVERIFY(!editor->findBarVisible());

    // Ctrl+F opens the bar with the remembered query (drop-in users keep
    // their muscle memory). The find row must be reachable: locate it by
    // its field/button identities instead of painting it.
    QTest::keyClick(editor, Qt::Key_F, Qt::ControlModifier);
    QVERIFY(editor->findBarVisible());
    QLineEdit* q = barField(editor, QStringLiteral("Find"));
    QVERIFY(q);
    QCOMPARE(q->text(), QStringLiteral("foo"));

    // The X button hides the bar; the query and the match set persist.
    // (The glyph texts also lock the \u-escape form of the bar's unicode
    // buttons: UTF-8 byte escapes in QStringLiteral would read as mojibake.)
    QVERIFY(barButton(editor, QStringLiteral("\u25B2"))); // ▲ prev
    QVERIFY(barButton(editor, QStringLiteral("\u25BC"))); // ▼ next
    QPushButton* x = barButton(editor, QStringLiteral("\u00D7"));
    QVERIFY(x);
    QTest::mouseClick(x, Qt::LeftButton, Qt::NoModifier, x->rect().center());
    QVERIFY(!editor->findBarVisible());
    QCOMPARE(editor->findText(), QStringLiteral("foo"));
    QCOMPARE(editor->matchCount(), 3);

    // F3/Shift+F3 work with the bar CLOSED (the query persists, Monaco).
    QTest::keyClick(editor, Qt::Key_F3);
    QCOMPARE(editor->currentMatch(), 0);
    QCOMPARE(editor->selectedText(), QStringLiteral("foo"));
    QTest::keyClick(editor, Qt::Key_F3, Qt::ShiftModifier);
    QCOMPARE(editor->currentMatch(), 2); // wraps to the last match
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 19})); // caret = match end

    // Ctrl+H reopens (single bar, replace row included). Esc in the
    // field closes from inside; again the query persists.
    QTest::keyClick(editor, Qt::Key_H, Qt::ControlModifier);
    QVERIFY(editor->findBarVisible());
    QVERIFY(barField(editor, QStringLiteral("Replace")));
    QTest::keyClick(q, Qt::Key_Escape);
    QVERIFY(!editor->findBarVisible());
    QCOMPARE(editor->findText(), QStringLiteral("foo"));

    // Typing into the field drives the engine through textChanged (works
    // with the bar hidden too, but reopen it to assert clearFind's
    // "does not hide" half of the contract).
    editor->showFindBar();
    QVERIFY(editor->findBarVisible());
    q->setText(QStringLiteral("bar"));
    QCOMPARE(editor->findText(), QStringLiteral("bar"));
    QCOMPARE(editor->matchCount(), 1);

    // clearFind: query + flags + matches + the field, bar untouched.
    editor->setFindFlags(CodeEditor::FindFlag::MatchCase | CodeEditor::FindFlag::UseRegex);
    editor->clearFind();
    QCOMPARE(editor->findText(), QString());
    QVERIFY(editor->findFlags() == CodeEditor::FindFlag::MatchCase); // the default state
    QCOMPARE(editor->matchCount(), 0);
    QVERIFY(editor->findPatternValid());
    QCOMPARE(q->text(), QString());
    QVERIFY(editor->findBarVisible()); // clearFind does not hide
}

void TestCodeEditor::findBarStaysTopRightOnResize()
{
    // The bar anchors to the editor's top-right with a 4px margin and
    // CLAMPS (never clips) at narrow widths: place() runs from open() and
    // from resizeEvent, so the invariant must hold after every resize.
    editor->show(); // a shown editor gives isVisible() its real semantics
    editor->showFindBar();
    QWidget* bar = findBarOf(editor);
    QVERIFY(bar);
    QVERIFY(bar->isVisible());
    QCOMPARE(bar->y(), 4);
    const int wideRight = bar->geometry().right();
    QCOMPARE(wideRight, editor->width() - 5); // flush at the 4px margin
    QVERIFY(bar->width() <= editor->width() - 8);
    QVERIFY(bar->x() >= 4);

    editor->resize(340, 400);
    QCoreApplication::processEvents(); // the resize event places the bar
    QCOMPARE(bar->geometry().right(), 340 - 5); // tracks the right edge
    QVERIFY2(bar->x() >= 0, "bar clipped at the left edge when narrow");

    editor->resize(316, 400); // below the 300px floor: clamp, no overflow
    QCoreApplication::processEvents();
    QCOMPARE(bar->geometry().right(), 316 - 5);
    QVERIFY(bar->width() >= 300); // controls compress, the bar never vanishes
    QVERIFY(bar->x() >= 0);

    editor->resize(600, 400);
    QCoreApplication::processEvents();
    QCOMPARE(bar->y(), 4);
    QCOMPARE(bar->geometry().right(), 600 - 5);
    QVERIFY(bar->width() > 300); // back above the floor at the wide size
}

void TestCodeEditor::paintMatchQuadsNumericProbe()
{
    // Numeric pixel probe (no vision): match quads must paint warm ink on
    // the matching lines ONLY, and the current match (bright family ink +
    // the blue selection over it) must leave the warm band — the quads
    // are distinguishable state, not decoration noise.
    editor->setPlainText(QStringLiteral("foo bar foo baz\nzzz foo qux\nbar"));
    const int lh = editor->lineHeight();
    const int pad = editor->padding();
    const int x0 = editor->contentLeft();
    auto warm = [&](int yFrom, int yTo) {
        return warmQuadPixels(editor->grab().toImage(), x0, yFrom, yTo);
    };

    QCOMPARE(warm(pad, pad + 3 * lh), 0); // negative control: no query set

    editor->setFindText(QStringLiteral("foo"));
    QCOMPARE(editor->matchCount(), 3);
    QCOMPARE(editor->currentMatch(), -1);
    const int allInk = warm(pad, pad + 3 * lh);
    QVERIFY2(allInk >= 300,
             qPrintable(QStringLiteral("match quads drew only %1 warm pixels").arg(allInk)));

    // The per-line window walk must not bleed ink into non-matching rows
    // (line 1 has ONE match at cols 4..7; the whole row is still mostly
    // cold - the exact per-line, viewport-only invariant).
    editor->setFindText(QStringLiteral("qux"));
    QCOMPARE(warm(pad, pad + lh), 0); // line 0: no match, no ink
    const int oneQuad = warm(pad + lh, pad + 2 * lh);
    QVERIFY2(oneQuad >= 100, qPrintable(QStringLiteral("single quad drew only %1").arg(oneQuad)));

    // Selecting the current match overlays the selection (blue) on that
    // quad: warm pixels strictly decrease while the OTHER quads keep the
    // band alive - current vs all is a distinct rendered state.
    editor->setFindText(QStringLiteral("foo"));
    QVERIFY(editor->findNext());
    const int withCurrent = warm(pad, pad + 3 * lh);
    QVERIFY2(allInk - withCurrent >= 60,
             qPrintable(QStringLiteral("current+selection did not part the warm band: %1 -> %2")
                            .arg(allInk).arg(withCurrent)));
    QVERIFY2(withCurrent >= 150, "the non-current quads vanished");
}

void TestCodeEditor::paintZeroWidthMatchPhantomCell()
{
    // A zero-width match (regex "b*" on "ab" hits twice around the b)
    // still owns one phantom cell of ink: empty highlights must not
    // vanish from the viewport paint pass.
    editor->setPlainText(QStringLiteral("ab"));
    editor->setFindFlags(CodeEditor::FindFlag::MatchCase | CodeEditor::FindFlag::UseRegex);
    editor->setFindText(QStringLiteral("b*"));
    QCOMPARE(editor->matchCount(), 3);
    QCOMPARE(editor->matches(),
             (QVector<MR>{MR{0, 0, 0, 0}, MR{0, 1, 0, 2}, MR{0, 2, 0, 2}}));

    const int lh = editor->lineHeight();
    const int pad = editor->padding();
    const int x0 = editor->contentLeft();
    QVERIFY2(warmQuadPixels(editor->grab().toImage(), x0, pad, pad + lh) >= 60,
             "zero-width quads did not paint phantom cells");

    editor->setFindText(QString()); // control: every quad gone
    QCOMPARE(warmQuadPixels(editor->grab().toImage(), x0, pad, pad + lh), 0);
}

// --- Overview ruler (M7, Phase E1) ---------------------------------------

void TestCodeEditor::overviewRulerGeometryAndMapping()
{
    // Ruler GEOMETRY STATE (the E1 DoD entry): strip rect, the linear
    // doc-pixel mapping, and the viewport rectangle. The test duplicates
    // the ONE ratio formula (doc px space [0, 2*padding + lineCount*lh)
    // onto [0, height())): implementation and test must agree exactly,
    // because the click gesture, the viewport frame and the paint all
    // ride that single mapping.
    const int h = editor->height();
    const int w = editor->width();
    const int lh = editor->lineHeight();
    const int pad = editor->padding();

    QVERIFY(editor->overviewRulerEnabled()); // default on: minimap parity
    QCOMPARE(CodeEditor::kOverviewRulerWidth, 44);
    QCOMPARE(editor->overviewRulerWidth(), CodeEditor::kOverviewRulerWidth);
    QCOMPARE(editor->overviewRulerGeometry(),
             QRect(w - CodeEditor::kOverviewRulerWidth, 0, CodeEditor::kOverviewRulerWidth, h));

    QString doc;
    for (int i = 0; i < 200; ++i) {
        if (i)
            doc += QLatin1Char('\n'); // NO trailing newline: lineCount is exact
        doc += QStringLiteral("line %1").arg(i);
    }
    editor->setPlainText(doc);
    const int lineCount = editor->lineCount();
    QCOMPARE(lineCount, 200);
    const int total = 2 * pad + lineCount * lh; // duplicated contract constant

    // One strip row at a time: rulerLineAt == the same integer math
    // (floor(y*total/h), then (docPx - pad)/lh clamped to the document).
    int prev = -1;
    QVector<bool> hit(lineCount, false);
    for (int y = 0; y < h; ++y) {
        const int yy = y < h - 1 ? y : h - 1;
        const int docPx = int(qint64(yy) * total / h);
        int want = (docPx - pad) / lh;
        want = want < 0 ? 0 : (want > lineCount - 1 ? lineCount - 1 : want);
        const int got = editor->rulerLineAt(y);
        QCOMPARE(got, want);
        QVERIFY2(got >= prev, qPrintable(QStringLiteral("mapping not monotonic at y=%1").arg(y)));
        prev = got;
        hit[got] = true;
    }
    QCOMPARE(editor->rulerLineAt(0), 0);
    QCOMPARE(editor->rulerLineAt(h - 1), lineCount - 1);
    // 200 lines on a 400px strip: ~2 strip px per line, so NO line may be
    // skipped by the sampling (each doc line owns >= 1 strip row).
    for (int line = 0; line < lineCount; ++line)
        QVERIFY2(hit[line], qPrintable(QStringLiteral("line %1 got no strip row").arg(line)));

    // Viewport rectangle: the strip's own scroll STATE, exact formula.
    const auto vpWant = [&](int vScroll) {
        const int y0 = int(qint64(vScroll) * h / total);
        const int y1 = int(qint64(std::min(total, vScroll + h)) * h / total);
        const int thick = y1 - y0;
        return QRect(w - CodeEditor::kOverviewRulerWidth, y0, CodeEditor::kOverviewRulerWidth,
                     thick < 1 ? 1 : thick);
    };
    QCOMPARE(editor->verticalScrollOffset(), 0);
    QCOMPARE(editor->overviewRulerViewportRect(), vpWant(0));

    QWheelEvent wheel(QPointF(300, 200), QPointF(300, 200), QPoint(0, 0), QPoint(0, -360),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QVERIFY(QApplication::sendEvent(editor, &wheel));
    const int scrolled = editor->verticalScrollOffset();
    QVERIFY(scrolled > 0); // the wheel throw moved the strip state
    QCOMPARE(editor->overviewRulerViewportRect(), vpWant(scrolled));
    QVERIFY(editor->overviewRulerViewportRect().top() > vpWant(0).top());

    // Disabled = the set_minimap_enabled(False) path: every geometry API
    // reads empty/zero/-1 (the binding never sees a phantom strip).
    editor->setOverviewRulerEnabled(false);
    QVERIFY(!editor->overviewRulerEnabled());
    QCOMPARE(editor->overviewRulerWidth(), 0);
    QVERIFY(editor->overviewRulerGeometry().isEmpty());
    QVERIFY(editor->overviewRulerViewportRect().isEmpty());
    QCOMPARE(editor->rulerLineAt(h / 2), -1);
    editor->setOverviewRulerEnabled(true);
    QCOMPARE(editor->overviewRulerGeometry(),
             QRect(w - CodeEditor::kOverviewRulerWidth, 0, CodeEditor::kOverviewRulerWidth, h));

    // The strip follows the right edge across a resize.
    editor->resize(700, h);
    QCOMPARE(editor->overviewRulerGeometry(),
             QRect(700 - CodeEditor::kOverviewRulerWidth, 0, CodeEditor::kOverviewRulerWidth, h));
    editor->resize(w, h); // keep the geometry contract stable for later slots
}

void TestCodeEditor::overviewRulerScrollGesture()
{
    // Strip clicks/drag scroll and NEVER touch the caret or the selection
    // (the strip is chrome, not a hit-test surface). Click-to-center uses
    // the same mapping math asserted in the geometry slot.
    QString doc;
    for (int i = 0; i < 500; ++i) {
        if (i)
            doc += QLatin1Char('\n');
        doc += QStringLiteral("line %1").arg(i);
    }
    editor->setPlainText(doc);
    QCOMPARE(editor->lineCount(), 500);
    QCOMPARE(editor->verticalScrollOffset(), 0);
    const int h = editor->height();
    const int w = editor->width();
    // Inside the strip, left of the vBar_ overlay (it only covers the
    // strip's right edge; direct sendEvent bypasses child hit-testing,
    // this x just keeps the gesture honest w.r.t. real mouse routing).
    const int stripX = w - CodeEditor::kOverviewRulerWidth + 4;

    const auto clickStrip = [&](int y, Qt::MouseButton button) {
        const QPointF p(stripX, y);
        QMouseEvent press(QEvent::MouseButtonPress, p, p, button, button, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, p, p, button, Qt::NoButton, Qt::NoModifier);
        QVERIFY(QApplication::sendEvent(editor, &press));
        QVERIFY(QApplication::sendEvent(editor, &release));
    };

    clickStrip(h - 2, Qt::LeftButton); // strip bottom: the doc end centers
    QVERIFY(editor->firstVisibleLine() > 450);
    QVERIFY(editor->verticalScrollOffset() > 0);

    clickStrip(2, Qt::LeftButton); // strip top: back to the doc start
    QCOMPARE(editor->verticalScrollOffset(), 0);
    QCOMPARE(editor->firstVisibleLine(), 0);

    // Drag pans continuously while the button is held (manual events:
    // QTest::mouseMove carries no button state).
    const QPointF p1(stripX, 30), p2(stripX, 250);
    QMouseEvent press(QEvent::MouseButtonPress, p1, p1, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, p2, p2, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, p2, p2, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QVERIFY(QApplication::sendEvent(editor, &press));
    const int afterPress = editor->firstVisibleLine();
    QVERIFY(afterPress > 5); // the press scrolled (near-top line centered)
    QVERIFY(QApplication::sendEvent(editor, &move));
    const int afterMove = editor->firstVisibleLine();
    QVERIFY2(afterMove > afterPress,
             qPrintable(QStringLiteral("ruler drag did not pan: %1 -> %2").arg(afterPress).arg(afterMove)));
    QVERIFY(QApplication::sendEvent(editor, &release));
    QCOMPARE(editor->firstVisibleLine(), afterMove); // release ends, does not move

    // Middle click inside the strip: scroll, but NO X11 paste and no caret
    // placement (the gesture intercept runs before the paste branch).
    clickStrip(h - 2, Qt::MiddleButton);
    QVERIFY(editor->firstVisibleLine() > 450);
    QCOMPARE(editor->position(), qmn::view::TextPosition(0, 0)); // caret never moved
    QVERIFY(!editor->hasSelection());
    QCOMPARE(editor->toPlainText(), doc); // no paste landed in the doc

    // Guard the other half of the contract: OUTSIDE the strip a normal
    // click still places the caret (the intercept is region-bounded).
    const QPointF outside(400, 200);
    QMouseEvent np(QEvent::MouseButtonPress, outside, outside, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent nr(QEvent::MouseButtonRelease, outside, outside, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QVERIFY(QApplication::sendEvent(editor, &np));
    QVERIFY(QApplication::sendEvent(editor, &nr));
    QVERIFY(editor->position().line > 400); // deep-scroll state: hit-test tracks it
}

void TestCodeEditor::overviewRulerMarkerPaintProbe()
{
    // Numeric strip-only probe (no vision): x bounded to the ruler window,
    // so every sampled pixel is ruler chrome. The chrome palette is cool
    // (R==G==B bg/stripes/viewport frame), hence all three predicates are
    // EXACTLY 0 as negative controls; each marker layer then lights its own
    // predicate and keeps the other two dark. Small doc: maxScroll == 0,
    // vBar_ hidden, nothing else paints into the window.
    editor->setPlainText(QStringLiteral("alpha\nbeta gamma\ndelta"));
    QCOMPARE(editor->lineCount(), 3);
    const int h = editor->height();
    const int w = editor->width();
    const int xs = w - CodeEditor::kOverviewRulerWidth;
    const auto countIf = [&](const std::function<bool(const QColor&)>& pred) {
        const QImage img = editor->grab().toImage();
        int n = 0;
        for (int y = 0; y < h; ++y)
            for (int x = xs; x < w; ++x)
                if (pred(img.pixelColor(x, y)))
                    ++n;
        return n;
    };
    const std::function<bool(const QColor&)> reddish = [](const QColor& c) {
        return c.red() > c.green() + 60 && c.red() > c.blue() + 60;
    };
    const std::function<bool(const QColor&)> warmish = [](const QColor& c) {
        return c.red() >= c.green() + 10 && c.green() >= c.blue() + 10 && c.red() <= 200
            && c.green() >= 55;
    };
    const std::function<bool(const QColor&)> coolBlue = [](const QColor& c) {
        return c.blue() > c.red() + 20 && c.blue() > c.green() + 5;
    };

    QCOMPARE(countIf(reddish), 0); // negative controls: chrome is cool
    QCOMPARE(countIf(warmish), 0);
    QCOMPARE(countIf(coolBlue), 0);

    editor->setBreakpointLines({1}); // M5 ink reuse: the gutter red dot is
    QVERIFY(countIf(reddish) >= 50); // OUTSIDE the window; only the ruler
    QCOMPARE(countIf(warmish), 0);   // band may light the predicates here
    QCOMPARE(countIf(coolBlue), 0);
    editor->setBreakpointLines({});
    QCOMPARE(countIf(reddish), 0);

    editor->setHighlightedLines(1, 1); // the M5 highlighted-line band
    QVERIFY(countIf(coolBlue) >= 50);
    QCOMPARE(countIf(reddish), 0);
    editor->clearHighlightedLines();
    QCOMPARE(countIf(coolBlue), 0);

    // M6 match ink, all-matches only (no findNext): the current-match ink
    // (246,169,0) is deliberately invisible to the warm predicate (R>200),
    // and the all-match ink (196,136,30) invisible to reddish — the bands
    // are exactly as colour-separated as in the text viewport.
    editor->setFindText(QStringLiteral("lpha")); // line 0
    QVERIFY(editor->matchCount() >= 1);
    QVERIFY(countIf(warmish) >= 20);
    QCOMPARE(countIf(reddish), 0);
    editor->clearFind();
    QCOMPARE(countIf(warmish), 0); // ruler ink follows the live find state

    // Disabled ruler = zero chrome: the probes stay dead even with every
    // marker layer armed (the paint pass is skipped, not recoloured).
    editor->setFindText(QStringLiteral("lpha"));
    editor->setBreakpointLines({0});
    editor->setOverviewRulerEnabled(false);
    QCOMPARE(countIf(reddish), 0);
    QCOMPARE(countIf(warmish), 0);
    QCOMPARE(countIf(coolBlue), 0);
    editor->setOverviewRulerEnabled(true);
    QVERIFY(countIf(warmish) >= 20); // and the band is back, live state intact
}

// --- Horizontal scroll (M9, Phase E1) ---

void TestCodeEditor::horizontalScrollExtentAndClamps()
{
    // The clamp contract in one line: max = docWidth + padding - window,
    // and the doc width is AMBIENT (public docs on maxHorizontalScroll):
    // it only exists once a line visit (paint/caret/hit-test) has measured
    // it. Before the first visit the bar must not budge even though the
    // document holds thousands of units.
    const double cw = editor->cellWidth();
    const int pad = editor->padding();
    editor->setPlainText(QStringLiteral("Z") + QString(3000, QLatin1Char('0')));
    QCOMPARE(editor->lineCount(), 1);
    QCOMPARE(editor->maxHorizontalScroll(), 0);
    QCOMPARE(editor->horizontalScrollOffset(), 0);

    const int win = editor->horizontalViewportWidth();
    QCOMPARE(win, editor->width() - editor->contentLeft() - editor->overviewRulerWidth());
    QVERIFY(win > 0);

    // Any caret visit re-seeds the extent: the visit measures the WHOLE
    // line (phantom-aware xInLine past the layout budget - ASCII monospace
    // is exactly col*cw), not just the caret column.
    QTest::keyClick(editor, Qt::Key_Right);
    const int docWidth = int(std::ceil(3001.0 * cw));
    QCOMPARE(editor->maxHorizontalScroll(), docWidth + pad - win);

    // setHorizontalScrollOffset is the one clamping writer (arrows, wheel,
    // the bar and the caret all ride it): overshoot lands EXACTLY on the
    // contract, negatives clamp to 0, an in-range value round-trips.
    editor->setHorizontalScrollOffset(999999);
    QCOMPARE(editor->horizontalScrollOffset(), docWidth + pad - win);
    editor->setHorizontalScrollOffset(-50);
    QCOMPARE(editor->horizontalScrollOffset(), 0);
    editor->setHorizontalScrollOffset(12345);
    QCOMPARE(editor->horizontalScrollOffset(), 12345);

    // xForCol is the widget-space (as-seen) x: it shifts left with the
    // offset by exactly col*cw on monospace (the M3 grid invariant).
    const double xe = editor->contentLeft() + 50 * cw - 12345;
    const double got = editor->xForCol(0, 50);
    QVERIFY2(std::abs(got - xe) < 0.01, qPrintable(QStringLiteral("xForCol %1 vs %2")
                                                      .arg(got).arg(xe)));

    // A document replace resets BOTH sides of the contract (offset and the
    // ambient extent), so a fresh doc starts its own bar growth.
    editor->setPlainText(QStringLiteral("one"));
    QCOMPARE(editor->horizontalScrollOffset(), 0);
    QCOMPARE(editor->maxHorizontalScroll(), 0);
}

void TestCodeEditor::caretKeptVisibleHorizontallyOnLongLines()
{
    // Monospace invariant: caret px is EXACTLY col*cw (the M3 grid test's
    // own finding), so the DoD "caret scroll-into-view" is assertable as a
    // closed form: the right-edge condition fires only at
    // ceil(caretX - win + 1) > hScroll, and Right is monotone.
    const double cw = editor->cellWidth();
    editor->setPlainText(QString(400, QLatin1Char('0')));
    const int win = editor->horizontalViewportWidth();
    const double lineEnd = 400.0 * cw;
    QVERIFY(lineEnd > win + 40 * cw); // far enough that the bar MUST move

    for (int k = 1; k <= 120; ++k) {
        QTest::keyClick(editor, Qt::Key_Right);
        const int expected = std::max(0, int(std::ceil(k * cw - win + 1.0)));
        QVERIFY2(editor->horizontalScrollOffset() == expected,
                 qPrintable(QStringLiteral("k=%1: h=%2 expected=%3")
                                .arg(editor->horizontalScrollOffset())
                                .arg(expected).arg(k)));
        // The caret is inside the window at every single step (that IS the
        // DoD): never past the right edge, never behind the left one.
        const double cx = editor->xForCol(0, k);
        QVERIFY(cx >= editor->contentLeft() - 0.01
                && cx < editor->contentLeft() + win + 0.01);
    }

    // End: flush right - and the 3px tail slack is exactly the padding_
    // term of the clamp formula (caret flushes at win-1 while maxScroll
    // keeps padding more px past the line end).
    QTest::keyClick(editor, Qt::Key_End);
    QCOMPARE(editor->position().col, 400);
    QCOMPARE(editor->horizontalScrollOffset(),
             editor->maxHorizontalScroll() - editor->padding() + 1);

    // Home walks the offset straight back to 0 (left-edge flush).
    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->horizontalScrollOffset(), 0);

    // Scroll the caret into the middle, then Left near the start must NOT
    // shift the bar back (the caret is still inside the window).
    QTest::keyClick(editor, Qt::Key_End);
    editor->setHorizontalScrollOffset(editor->maxHorizontalScroll());
    for (int k = 0; k < 10; ++k)
        QTest::keyClick(editor, Qt::Key_Left);
    QVERIFY(editor->horizontalScrollOffset() > 0); // caret far from the bar

    // Backspace on the scrolled monster line: one char gone, the extent
    // reseeds on the edit visit and the clamp pulls the offset back with
    // it (never stranded past the new max) — flush at the new end.
    const int before = editor->horizontalScrollOffset();
    QTest::keyClick(editor, Qt::Key_Backspace);
    QCOMPARE(editor->toPlainText().size(), 399); // one char gone (single line)
    QVERIFY(editor->horizontalScrollOffset() <= before);
    QCOMPARE(editor->horizontalScrollOffset(), editor->maxHorizontalScroll());
}

void TestCodeEditor::shiftWheelAndClickMapThroughOffset()
{
    const double cw = editor->cellWidth();
    const int cellPx = int(std::ceil(cw)); // setHScroll steps in ceil(cw) px
    QStringList doc;
    doc << QString(400, QLatin1Char('0')); // long line 0 (h-extent source)
    for (int i = 0; i < 60; ++i)
        doc << QStringLiteral("line %1").arg(i); // vertical bar is real too
    editor->setPlainText(doc.join(QLatin1Char('\n')));
    QTest::keyClick(editor, Qt::Key_Right); // seeds the ambient extent
    QVERIFY(editor->maxHorizontalScroll() > 0);

    const auto wheel = [&](QPoint angle, Qt::KeyboardModifiers mods) {
        QWheelEvent ev(QPointF(300, 200), QPointF(300, 200), QPoint(0, 0), angle,
                       Qt::NoButton, mods, Qt::NoScrollPhase, false);
        QVERIFY(QApplication::sendEvent(editor, &ev));
    };

    // Shift + one notch down = 3 cells right (mirrors the hBar singleStep),
    // and vertical must NOT also move under Shift (the axis swaps, it does
    // not stack).
    wheel(QPoint(0, -120), Qt::ShiftModifier);
    QCOMPARE(editor->horizontalScrollOffset(), 3 * cellPx);
    QCOMPARE(editor->firstVisibleLine(), 0);

    // A trackpad's native horizontal axis needs no modifier at all: one
    // notch right rolls the offset back (angle.x > 0 scrolls content left).
    wheel(QPoint(120, 0), Qt::NoModifier);
    QCOMPARE(editor->horizontalScrollOffset(), 0);

    // Plain vertical is vertical ONLY (no accidental h-shift).
    wheel(QPoint(0, -120), Qt::NoModifier);
    QCOMPARE(editor->horizontalScrollOffset(), 0);
    QVERIFY(editor->firstVisibleLine() > 0);
    wheel(QPoint(0, 120), Qt::NoModifier);
    QCOMPARE(editor->firstVisibleLine(), 0);

    // The click hit-test maps THROUGH the offset: with h = 100 the visible
    // position of model col 25 is contentLeft + 25cw - 100 (+half a cell so
    // the monospace floor picks col 25, the established grid semantics).
    editor->setHorizontalScrollOffset(100);
    const QPoint click(editor->contentLeft() + int(25 * cw + cw / 2.0) - 100,
                       editor->padding() + editor->lineHeight() / 2);
    const qmn::view::TextPosition p = editor->positionAt(click);
    QCOMPARE(p.line, 0);
    QCOMPARE(p.col, 25);

    // A real press+release goes through the same mapping.
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, click);
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 25);
}

void TestCodeEditor::scrollBeyondLastLineFlagAndRulerTotal()
{
    // Parity hook (qtmonaco set_scroll_beyond_last_line_enabled): on, the
    // doc pixel space gains height()-lineHeight_ so the LAST line can sit
    // at the viewport TOP. Everything (vBar clamp, firstVisibleLine, the
    // ruler mapping) rides the same totalHeight() formula.
    QStringList doc;
    for (int i = 0; i < 30; ++i)
        doc << QStringLiteral("line %1").arg(i);
    editor->setPlainText(doc.join(QLatin1Char('\n')));
    const int lh = editor->lineHeight();
    const int pad = editor->padding();
    const int h = editor->height();
    const int n = editor->lineCount();
    QCOMPARE(n, 30);
    QVERIFY(!editor->scrollBeyondLastLineEnabled()); // deliberate default

    const int max0 = 2 * pad + n * lh - h; // flag-off max (positive here)
    QVERIFY(max0 > 0);
    const auto deepWheel = [&]() {
        QWheelEvent ev(QPointF(300, 200), QPointF(300, 200), QPoint(0, 0),
                       QPoint(0, -120 * 1000), Qt::NoButton, Qt::NoModifier,
                       Qt::NoScrollPhase, false);
        QVERIFY(QApplication::sendEvent(editor, &ev));
    };
    deepWheel();
    QCOMPARE(editor->firstVisibleLine(), (max0 - pad) / lh);

    editor->setScrollBeyondLastLineEnabled(true);
    const int max1 = max0 + (h - lh); // bonus = height - lineHeight
    deepWheel(); // clamp lands on the new max
    // The whole point of the flag: the LAST line flushes at the top.
    QCOMPARE(editor->firstVisibleLine(), n - 1);

    // Ruler viewport rect = the same linear rulerY mapping over the bonus
    // total (re-derived here — the DoD is ONE doc-space ratio everywhere).
    const int total = 2 * pad + n * lh + (h - lh);
    const int vs = max1;
    const QRect r = editor->overviewRulerViewportRect();
    QCOMPARE(r.y(), int(qint64(vs) * h / total));
    const int y1 = int(qint64(std::min(total, vs + h)) * h / total);
    QCOMPARE(r.height(), std::max(1, y1 - int(qint64(vs) * h / total)));

    // Flag OFF again: the clamp pulls vScroll back to the exact flag-off
    // max (one setScrollBeyond… call, no stale deep state).
    editor->setScrollBeyondLastLineEnabled(false);
    QCOMPARE(editor->firstVisibleLine(), (max0 - pad) / lh);
    deepWheel();
    QCOMPARE(editor->firstVisibleLine(), (max0 - pad) / lh);

    // H-scroll sanity across the toggle: horizontal offsets are untouched
    // by the vertical bonus.
    editor->setPlainText(doc.join(QLatin1Char('\n')) + QLatin1Char('\n')
                        + QString(500, QLatin1Char('7')));
    QTest::keyClick(editor, Qt::Key_End, Qt::ControlModifier); // last line =
    // the monster: the caret visit seeds the h-extent
    QVERIFY(editor->maxHorizontalScroll() > 0);
    editor->setHorizontalScrollOffset(100);
    editor->setScrollBeyondLastLineEnabled(true);
    QCOMPARE(editor->horizontalScrollOffset(), 100);
    editor->setScrollBeyondLastLineEnabled(false);
    QCOMPARE(editor->horizontalScrollOffset(), 100);
}

void TestCodeEditor::paintClusterWindowXOffsetProbe()
{
    // No-vision band probe (the DoD "caret scroll-into-view" and "no
    // off-screen paint cost" verified at the pixel level). Doc line 1 is
    // 200 spaces + "QQ" + 200 spaces: the Q cluster sits at 200*cw px,
    // OUTSIDE the text window at h=0 but inside the geoMaxUnits layout
    // budget - so the positive step below proves its glyphs exist and only
    // the windowed paint kept them unlit. The caret parks on line 0 so
    // line 1's band is caret-free for the zero-ink control.
    const double cw = editor->cellWidth();
    editor->setPlainText(QStringLiteral("ab\n") + QString(200, QLatin1Char(' '))
                        + QStringLiteral("QQ") + QString(200, QLatin1Char(' ')));
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_Right); // caretOn_ = true, deterministic
    editor->grab(); // paint pass visits both lines -> seeds the extent

    const int win = editor->horizontalViewportWidth();
    const int textLeft = editor->contentLeft();
    const int lh = editor->lineHeight();
    const int pad = editor->padding();
    const QColor base = editor->palette().color(QPalette::Base);
    const QColor textColor = editor->palette().color(QPalette::Text);
    QVERIFY(std::abs(textColor.redF() - base.redF()) + std::abs(textColor.blueF() - base.blueF()) > 0.3);
    const int winRight = textLeft + win;
    QCOMPARE(editor->maxHorizontalScroll(), int(std::ceil(402 * cw)) + pad - win);

    const auto inkCount = [&](const QImage& img, int x0, int x1, int yTop, int rows) {
        int n = 0;
        for (int x = x0; x < x1; ++x)
            for (int y = yTop; y < yTop + rows; ++y)
                if (img.pixelColor(x, y) != base)
                    ++n;
        return n;
    };
    auto countWhere = [&](const QImage& img, int x0, int x1, int yTop, int rows,
                          const std::function<bool(const QColor&)>& pred) {
        int n = 0;
        for (int x = x0; x < x1; ++x)
            for (int y = yTop; y < yTop + rows; ++y)
                if (pred(img.pixelColor(x, y)))
                    ++n;
        return n;
    };

    const QImage img0 = editor->grab().toImage();
    const int y1 = pad + lh; // band top of line 1
    QCOMPARE(inkCount(img0, textLeft, winRight, y1, lh), 0);

    // Bring the Q cluster 100px inside the window's left edge.
    const int H = int(std::floor(200 * cw)) - 100;
    editor->setHorizontalScrollOffset(H);
    const QImage imgH = editor->grab().toImage();
    const double qX = textLeft + 200 * cw - H; // predicted widget x of col 200

    // Leading spaces keep EXACT base up to the predicted Q edge (the
    // offset landed on an integer px; only the fractional cell residual
    // gets a 1px tolerance).
    QCOMPARE(inkCount(imgH, textLeft, int(std::floor(qX)) - 1, y1, lh), 0);
    // ...and the Q glyph lights exactly the 2-cell band at that predicted
    // x - the painted ink now lands at shifted widget x (the DoD).
    QVERIFY2(inkCount(imgH, int(std::floor(qX)) - 1, int(std::ceil(qX + 2 * cw)) + 1, y1, lh) >= 20,
             QStringLiteral("no Q ink at predicted shifted band (%1..%2)")
                 .arg(qX).arg(qX + 2 * cw).toUtf8().constData());
    QCOMPARE(inkCount(imgH, int(std::ceil(qX + 2 * cw)) + 3, winRight, y1, lh), 0);

    // Selection ink rides the same shift: park the caret at the Q start
    // (col 200 - caret-keep scrolls here, setHScroll(H) puts the window
    // back where the predictions below were made), then Shift+Right x7:
    // range 200..207 keeps the caret inside the window, so the offset
    // stays put while the selection paints. The deep right half of the
    // range is plain spaces after the Qs: an exact alpha-blend over Base,
    // no glyph AA inside.
    editor->setPosition({1, 200});
    editor->setHorizontalScrollOffset(H);
    for (int i = 0; i < 7; ++i) // one-by-one: reliable through QTest's queue
        QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(editor->horizontalScrollOffset(), H);
    QCOMPARE(editor->position().col, 207);
    const QColor selColor = editor->palette().color(QPalette::Highlight);
    auto selBlend = [&](const QColor& c) {
        const qreal a = 90.0 / 255.0;
        const QColor want(static_cast<int>(qRound(base.red() + a * (selColor.red() - base.red()))),
                          static_cast<int>(qRound(base.green() + a * (selColor.green() - base.green()))),
                          static_cast<int>(qRound(base.blue() + a * (selColor.blue() - base.blue()))),
                          base.alpha());
        const QColor got = c.rgb(); // strip any alpha channel of the store
        return qAbs(got.red() - want.red()) <= 4 && qAbs(got.green() - want.green()) <= 4
            && qAbs(got.blue() - want.blue()) <= 4;
    };
    const int selCx = textLeft + int(204.5 * cw - H); // deep cell, fully inside
    const QImage imgS = editor->grab().toImage();
    QVERIFY(countWhere(imgS, selCx - 2, selCx + 3, y1, lh, selBlend) >= 10);

    // The caret bar (deterministic caretOn_ right after the key press) at
    // the model position follows the same shift: exact Text ink in its 2
    // columns, exact Base right next to them.
    const double cX = textLeft + 207 * cw - H; // predicted caret widget x
    const int cxi = qRound(cX);
    int caretInk = 0;
    for (int y = y1; y < y1 + lh; ++y) {
        if (imgS.pixelColor(cxi, y) == textColor)
            ++caretInk;
        if (imgS.pixelColor(cxi + 1, y) == textColor)
            ++caretInk;
    }
    QCOMPARE(caretInk, 2 * lh);
    QCOMPARE(countWhere(imgS, cxi + 3, cxi + 6, y1, lh,
                        [&](const QColor& c) {
                            return c.red() == base.red() && c.green() == base.green()
                                && c.blue() == base.blue();
                        }), 3 * lh);
    // And the very same model x was invisible in the h=0 control grab (it
    // existed off-window then too - culling is what kept it dark, not the
    // absence of the geometry): no Text ink anywhere in line 1's window.
    QCOMPARE(countWhere(img0, textLeft, winRight, y1, lh,
                        [&](const QColor& c) { return c == textColor; }), 0);
}

QTEST_MAIN(TestCodeEditor)
#include "tst_codeeditor.moc"
