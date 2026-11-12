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

#include <QAccessible>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QImage>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QUrl>
#include <QWheelEvent>

#include "view/CodeEditor.hpp"
#include "view/CodeEditorOptions.hpp"

#include <algorithm> // std::min in the ruler mapping contract
#include <cmath> // std::ceil in the h-scroll exact-offset arithmetic
#include <functional> // std::function for the probe predicates

using qmn::view::CodeEditor;
using qmn::view::CodeEditorOptions;

// Human-readable TextPosition dumps: QCOMPARE failures then show the
// offending {line,col} values instead of <null> (QTest streams elements,
// so QVector<TextPosition> benefits automatically too).
inline QDebug operator<<(QDebug dbg, const qmn::view::TextPosition& p)
{
    return dbg.nospace() << '{' << p.line << ", " << p.col << '}';
}

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

    // Indentation + auto-indent (M14).
    void tabInsertsSpacesToNextStop();
    void tabRespectsCustomStopAndSelectionReplace();
    void blockIndentAndOutdentAcrossSelectionLines();
    void multiCaretTabIndentsEachLineOneUndoStep();
    void shiftTabNoOpAtColZeroRecordsNothing();
    void enterAutoIndentCopiesIndentAndAddsLevel();
    void enterSplitsEmptyPair();
    void autoIndentDisableFlag();
    void enterIsItsOwnUndoStepWithAutoIndent();
    void tabChangesFocusIgnoresKey();

    // Caret/mouse gestures (M15): smart Home, multi-click, word drag, Ctrl+D.
    void smartHomeTogglesIndentThenColZero();
    void doubleClickSelectsWordRun();
    void multiClickLinesSelectWholeLinesAndGrow();
    void wordDragExtendsByWholeWords();
    void ctrlDSelectsWordThenNextOccurrences();
    void ctrlDEdgeCases();

    // Context menu + text DnD (M16).
    void contextMenuPlacementKeepsOrMovesCaret();
    void contextMenuChromeGuardsSkipMenu();
    void contextMenuActionsMirrorChords();
    void pressInsideSelectionHoldsForDrag();
    void dragGestureOffscreenIsPayloadSafe();
    void dropTextInsertsAtDropPoint();
    void dropMoveMarkerMovesSelection();
    void dropIgnoresForeignAndCopyVariants();
    void dragMoveAutoscrolls();
    void dropCaretTracksDragTarget();
    void shiftAtDropCopiesInsteadOfMoves();

    // Code font + zoom (M17).
    void setCodeFontRebuildsMetricsAndGeometry();
    void zoomFactorClampsAndSyncsWidgetFont();
    void zoomChordsZoomAndReset();
    void ctrlWheelZoomsAndPlainWheelScrolls();
    void zoomScalesChromeAndHitTesting();
    void externalSetFontRebasesBaseAndZoom();

    // Auto-close pairs + long-line ruler + current-line mode (M18).
    void autoClosePairInsertsSkipsAndDeletesBoth();
    void autoCloseWrapsSelection();
    void autoCloseQuotesRespectWordBoundary();
    void autoCloseSkipStaysTextualAndClosersPlain();
    void autoCloseFansOutMixedSlotsAsOneUndoStep();
    void longLineRulerMapsToColumnAndRidesHScroll();
    void currentLineModeWashFrameAndDelegates();

    // Settings object (M19).
    void optionsMirrorsKnobsAndNotifies();
    void autoCloseToggleOnOptions();
    void readOnlyBlocksUserEdits();

    // Accessibility bridge (M11).
    void accessibleInterfaceMirrorsRoleStateAndText();
    void accessibleTextOffsetsMapDocumentAndDriveCaret();
    void accessibleBoundariesFollowTheLineModel();
    void accessibleCharRectAndPointHitRoundTrip();

    // Phase F parity doors (the qtmonaco insert_text/delete_line/set_cursor
    // backing) + the textChanged ping the Python shim mirrors.
    void parityInsertAndDeleteLineDoors();
    void parityDoorsAndEveryMutationPingTextChanged();
    void revealAtScrollsPerModeAndUnfoldsFolds();

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

    // Code folding (M13, Phase E2). DoD: visible-line counts after a fold,
    // map lookups across fold boundaries, the caret jumping over a fold,
    // unfolded content unchanged after expandAll, the ruler reflecting the
    // fold, and fold chrome (gutter triangles, keyboard chords, edit
    // bookkeeping) — all through state/numeric assertions plus one
    // triangle-shape pixel probe (no vision).
    void foldMapBasicsIndentMode();
    void caretJumpsOverFold();
    void keyboardFoldUnfoldChords();
    void revealOnProgrammaticMove();
    void mouseClickFoldZoneToggles();
    void foldTriangleInkProbe();
    void rulerReflectsFoldRanges();
    void foldEditBookkeepingAndReset();
    void grammarFoldsFollowBracketsNotIndent();
    void pythonGrammarFoldsAndCollapseAll();
    void jsonFoldHidesClosingBrace();
    void foldZoneGutterConstant();
    void foldAutoExpandOnSourceDeath();

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
    // M18: the current-line wash ships ON and is a non-Base band — the
    // probe pins glyph GEOMETRY, so switch the wash off and keep the
    // background uniform.
    editor->setCurrentLineMode(CodeEditor::CurrentLineMode::Off);
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

// --- M16 context-menu + DnD test plumbing ----------------------------------

// Test-side twin of the private move-marker codec in CodeEditor.cpp.
// CodeEditor has a single inheritance chain, so the widget address equals
// the `this` a real drag would stamp.
QByteArray moveMarkerFor(const CodeEditor* e, int startOff, int endOff)
{
    return QStringLiteral("%1:%2:%3")
        .arg(quint64(reinterpret_cast<quintptr>(e)), 0, 16)
        .arg(startOff)
        .arg(endOff)
        .toUtf8();
}

// Hand-built drag-protocol sends. Measured: offscreen QDrag::exec returns
// synchronously (NoAction) and NEVER delivers a drop, so the drop side is
// only reachable through synthetic events; sendEvent dispatches DragEnter/
// DragMove/Drop straight into the widget overrides. Returns whether the
// dragEnter stage was accepted (QEvent copies are not assignable). The
// modifiers ride BOTH stages' events (Shift-at-drop tests need them on
// the Drop, and the enter/move acceptance answers Shift too).
bool sendDropAt(CodeEditor* e, const QPoint& at, Qt::DropAction action, const QMimeData& md,
                Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QDragEnterEvent de(at, action, &md, Qt::LeftButton, modifiers);
    QTEST_ASSERT(QApplication::sendEvent(e, &de));
    const bool entered = de.isAccepted();
    QDropEvent dr(at, action, &md, Qt::LeftButton, modifiers);
    QTEST_ASSERT(QApplication::sendEvent(e, &dr));
    return entered;
}

// Exposes the M16 protected menu seam (contextMenuEvent itself ends in
// QMenu::exec, which would block an offscreen test forever - tests use the
// prepare/factory split instead, and only ever send real ContextMenu events
// at CHROME pixels, whose guards return before the exec).
class MenuProbe : public CodeEditor {
public:
    using CodeEditor::createContextMenu;
    using CodeEditor::prepareContextMenuAt;
};

QAction* menuAction(QMenu* menu, const QString& text)
{
    for (QAction* a : menu->actions()) {
        if (!a->isSeparator() && a->text() == text)
            return a;
    }
    return nullptr;
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

// --- M14 indentation + auto-indent -----------------------------------------

void TestCodeEditor::tabInsertsSpacesToNextStop()
{
    editor->setPlainText(QStringLiteral("x"));
    QCOMPARE(editor->tabStopWidth(), 4); // default stop (Monaco tabSize)
    QTest::keyClick(editor, Qt::Key_Tab); // col 0 -> stop at 4
    QCOMPARE(editor->toPlainText(), QStringLiteral("    x"));
    QTest::keyClick(editor, Qt::Key_Tab); // already ON a stop: full stop
    QCOMPARE(editor->toPlainText(), QStringLiteral("        x"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 8}));
    QVERIFY(editor->additionalCarets().isEmpty()); // Tab never conjures slots

    // The two unbracketed inserts coalesced into ONE burst step: undo
    // clears both and restores the caret where the burst began (col 0).
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("x"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));

    // Shift+Tab: no-op at col 0; from right of a stop boundary it removes
    // exactly one stop. Both spellings work (QTest sends Key_Backtab on
    // itself, real shift+Tab arrives as Key_Tab + the shift flag).
    QTest::keyClick(editor, Qt::Key_Backtab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("x"));
    QTest::keyClick(editor, Qt::Key_Tab);
    QTest::keyClick(editor, Qt::Key_Tab);
    QTest::keyClick(editor, Qt::Key_Backtab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("    x"));
    QTest::keyClick(editor, Qt::Key_Tab, Qt::ShiftModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("x"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));
}

void TestCodeEditor::tabRespectsCustomStopAndSelectionReplace()
{
    editor->setTabStopWidth(0); // clamped to 1 minimum
    QCOMPARE(editor->tabStopWidth(), 1);
    editor->setTabStopWidth(3);
    editor->setPlainText(QStringLiteral("x"));
    QTest::keyClick(editor, Qt::Key_Tab); // col 0 -> stop 3
    QCOMPARE(editor->toPlainText(), QStringLiteral("   x"));
    // From a NON-boundary column the pad only crosses the next boundary:
    // End parks the caret at visual col 4, stop grid 0/3/6 -> two spaces.
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("   x  "));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 6}));

    // Plain Tab REPLACES a single-line selection (typing semantics): the
    // pad is measured at the caret (active end at col 3 = a stop -> 3sp).
    editor->setPlainText(QStringLiteral("abcd"));
    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier); // anchor 1, caret 3
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("a   d"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));
    QVERIFY(!editor->hasSelection());
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcd")); // ONE undo step
    QVERIFY(!editor->canUndo());

    // Shift+Tab is VS Code's "unindent": the touched WHOLE line only. With
    // no leading whitespace it is a no-op and the selection SURVIVES.
    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier); // [2,3)
    QTest::keyClick(editor, Qt::Key_Backtab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcd"));
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("c"));

    // Mid-band stop math: col 1 with stop 3 pads to col 3 (two spaces).
    editor->clearSelection();
    editor->setPlainText(QStringLiteral("xy"));
    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("x  y"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));
}

void TestCodeEditor::blockIndentAndOutdentAcrossSelectionLines()
{
    // Selection (0,0)..(1,0): the caret sits at col 0 of line 1, so the
    // end-at-col-0 rule leaves line 1 untouched — only line 0 indents.
    editor->setPlainText(QStringLiteral("  two\nthree"));
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_Down, Qt::ShiftModifier);
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("      two\nthree"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));
    QVERIFY(!editor->hasSelection()); // the block action consumes selections
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("  two\nthree")); // ONE step

    // Deeper selection (caret at col > 0) touches BOTH lines; Shift+Tab
    // then drops each line to the next lower stop boundary: 6 -> 4 (the
    // mid-band overshoot only), 4 -> 0 (a full stop from a boundary).
    editor->setPlainText(QStringLiteral("      six\n    four"));
    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Down, Qt::ShiftModifier); // (0,2)..(1,2)
    QTest::keyClick(editor, Qt::Key_Backtab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("    six\nfour"));
    // Caret (1,2) sat INSIDE line 1's removed prefix: collapsed to col 0
    // of the edit, then shifted by line 0's delta (-2).
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));

    // End-at-col-0 convention again via selectAll: caret (2,1) at col > 0
    // DOES touch the last line.
    editor->setPlainText(QStringLiteral("a\nb\nc"));
    editor->selectAll(); // anchor (0,0) .. caret (2,1)
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("    a\n    b\n    c"));

    // A selection ending exactly at col 0 leaves that line alone (built by
    // Shift+Up from (2,0): the END on line 2 sits at col 0, the caret is
    // the (0,0) end — the exclusion looks at the end on the LAST line).
    editor->setPlainText(QStringLiteral("a\nb\nc"));
    editor->setPosition({2, 0});
    QTest::keyClick(editor, Qt::Key_Up, Qt::ShiftModifier);
    QTest::keyClick(editor, Qt::Key_Up, Qt::ShiftModifier);
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("    a\n    b\nc"));

    // The stop follows the live tabStopWidth setting.
    editor->setTabStopWidth(2);
    editor->selectAll(); // caret (2,1): col > 0 touches line 2 again
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("      a\n      b\n  c"));
    editor->setTabStopWidth(4); // back to the default for later slots
}

void TestCodeEditor::multiCaretTabIndentsEachLineOneUndoStep()
{
    editor->setPlainText(QStringLiteral("line one\nline two"));
    editor->setPosition({0, 1}); // primary caret (0,1)
    altClick(editor, 1, 1); // clicked caret becomes primary, (0,1) demotes
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(editor->toPlainText(),
             QStringLiteral("    line one\n    line two"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 5}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 5}}));
    QVERIFY(editor->canUndo());
    editor->undo(); // the whole multi-caret Tab is exactly ONE step
    QCOMPARE(editor->toPlainText(), QStringLiteral("line one\nline two"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 1}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 1}}));
    QVERIFY(!editor->canUndo()); // setPosition/alt+click record no history

    // Extra carets on ONE line still route through the block path: the
    // line is touched once (no double indent) and every caret shifts.
    editor->setPlainText(QStringLiteral("abcdefg"));
    editor->setPosition({0, 1});
    altClick(editor, 0, 4); // primary (0,4), extra (0,1)
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("    abcdefg"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 8}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 5}}));
}

void TestCodeEditor::shiftTabNoOpAtColZeroRecordsNothing()
{
    editor->setPlainText(QStringLiteral("x"));
    QTest::keyClick(editor, Qt::Key_Backtab);
    QVERIFY(!editor->canUndo()); // a no-op must not even create an empty group
    QVERIFY(!editor->canRedo());
}

void TestCodeEditor::enterAutoIndentCopiesIndentAndAddsLevel()
{
    // Enter on a whitespace-ended line copies the whitespace through.
    editor->setPlainText(QStringLiteral("foo "));
    QTest::keyClick(editor, Qt::Key_End); // setPlainText parks the caret at (0,0)
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("foo \n"));
    QTest::keyClick(editor, Qt::Key_Return); // the copied prefix repeats
    QCOMPARE(editor->toPlainText(), QStringLiteral("foo \n\n"));

    editor->setPlainText(QStringLiteral("def f():"));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClick(editor, Qt::Key_Return); // trailing ':' adds one stop
    QCOMPARE(editor->toPlainText(), QStringLiteral("def f():\n    "));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 4}));
    QTest::keyClicks(editor, "if x:");
    QTest::keyClick(editor, Qt::Key_Return); // nested colon: one more stop
    QCOMPARE(editor->toPlainText(), QStringLiteral("def f():\n    if x:\n        "));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 8}));

    // Copied leading whitespace, no opener: the indent just rides over.
    editor->setPlainText(QStringLiteral("        x = 1"));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("        x = 1\n        "));
    // Opener detection skips trailing whitespace before the caret.
    editor->setPlainText(QStringLiteral("foo {   "));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("foo {   \n    "));
    // `{|` typed at EOL is the pair-split case: the implied closer moves
    // onto its own line below the child line.
    editor->setPlainText(QStringLiteral("foo {"));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("foo {\n    \n}"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 4}));

    // Alt+Enter bypasses the policy entirely (documented escape hatch):
    // plain '\n' where plain Enter would have split the level in two.
    editor->setPlainText(QStringLiteral("def f():"));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClick(editor, Qt::Key_Return, Qt::AltModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("def f():\n"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));
}

void TestCodeEditor::enterSplitsEmptyPair()
{
    editor->setPlainText(QStringLiteral("foo {}"));
    editor->setPosition({0, 5}); // exactly inside `{`|`}` (`}` sits at col 5)
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("foo {\n    \n}"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 4}));
    QTest::keyClick(editor, Qt::Key_Return); // a bare child line copies
    QCOMPARE(editor->toPlainText(), QStringLiteral("foo {\n    \n    \n}"));
    // `[]` splits when the caret hugs the OPENER at EOL (the payload
    // conjures the closer); a caret BEHIND the closer is a plain Enter.
    editor->setPlainText(QStringLiteral("[]"));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("[]\n"));
    editor->setPlainText(QStringLiteral("["));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("[\n    \n]"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 4}));
    editor->setPlainText(QStringLiteral("(a)"));
    editor->setPosition({0, 2}); // caret NOT flush against '('
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("(a\n)"));
}

void TestCodeEditor::autoIndentDisableFlag()
{
    QVERIFY(editor->autoIndentEnabled()); // default ON
    editor->setAutoIndentEnabled(false);
    editor->setPlainText(QStringLiteral("def f():"));
    QTest::keyClick(editor, Qt::Key_End);
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("def f():\n")); // plain '\n'
    editor->setAutoIndentEnabled(true);
    QTest::keyClick(editor, Qt::Key_End); // caret (1,0): line 1 is EMPTY
    QTest::keyClick(editor, Qt::Key_Return);
    // Enter at col 0 of an empty line stays plain (its own indent belongs
    // to the line it sits on): the level does NOT materialize from above.
    QCOMPARE(editor->toPlainText(), QStringLiteral("def f():\n\n"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 0}));
}

void TestCodeEditor::enterIsItsOwnUndoStepWithAutoIndent()
{
    // M3 regression guard under the new payload: Enter must break the
    // typing chain and stay a standalone undo step.
    editor->setPlainText(QString());
    QTest::keyClicks(editor, "ab");
    QTest::keyClick(editor, Qt::Key_Return);
    QTest::keyClicks(editor, "cd");
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab\ncd"));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab\n"));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));
}

void TestCodeEditor::tabChangesFocusIgnoresKey()
{
    QVERIFY(!editor->tabChangesFocus()); // default OFF
    editor->setTabChangesFocus(true);
    editor->setPlainText(QStringLiteral("x"));
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier,
                  QStringLiteral("\t")); // real Tab text, like X11 sends
    QCoreApplication::sendEvent(editor, &tab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("x")); // NO spaces...
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0})); // ...no caret move
    QVERIFY(!editor->canUndo());
    QVERIFY(!tab.isAccepted()); // and the key rides Qt's focus chain
    editor->setTabChangesFocus(false);
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(editor->toPlainText(), QStringLiteral("    x"));
}

// --- caret/mouse gestures (M15) --------------------------------------------

void TestCodeEditor::smartHomeTogglesIndentThenColZero()
{
    editor->setPlainText(QStringLiteral("    hello world\nplain\n   \n  bb"));

    editor->setPosition({0, 8});
    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4})); // first non-blank
    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0})); // then col 0
    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4})); // position-derived toggle

    // Shift extends the selection across the hop.
    editor->setPosition({0, 15});
    QTest::keyClick(editor, Qt::Key_Home, Qt::ShiftModifier);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));
    QCOMPARE(editor->selectedText(), QStringLiteral("hello world"));

    // Unindented line: Home is plain and IDEMPOTENT (first == col == 0).
    editor->setPosition({1, 5});
    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));
    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));

    // Whitespace-only line: first non-blank is the EOL itself.
    editor->setPosition({2, 1});
    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 3}));
    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 0}));

    // Multi-caret: the policy applies per slot.
    editor->setPosition({0, 0});
    altClick(editor, 3, 4); // primary {3,4} (clicked), extra {0,0}
    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{3, 2}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 4}}));
    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{3, 0}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 0}}));
}

void TestCodeEditor::doubleClickSelectsWordRun()
{
    editor->setPlainText(QStringLiteral("let foo_bar = 42"));

    QTest::mouseDClick(editor, Qt::LeftButton, Qt::NoModifier, caretPixel(editor, 0, 5));
    QCOMPARE(editor->selectedText(), QStringLiteral("foo_bar"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 11}));

    // A far second double-click is a FRESH word (chain x-slop), never a
    // line selection: 11 cells >> max(8, ceil(cellWidth)).
    QTest::mouseDClick(editor, Qt::LeftButton, Qt::NoModifier, caretPixel(editor, 0, 14));
    QCOMPARE(editor->selectedText(), QStringLiteral("42"));
    QCOMPARE(editor->additionalCaretCount(), 0);

    // A hit on the space between words selects the PURE space run (the
    // right-pivoted single-class range, not "word ").
    QTest::mouseDClick(editor, Qt::LeftButton, Qt::NoModifier, caretPixel(editor, 0, 3));
    QCOMPARE(editor->selectedText(), QStringLiteral(" "));

    // Word-less spot (empty document): the double-click just re-places the
    // caret and selects nothing (Monaco parity).
    editor->setPlainText(QString());
    QTest::mouseDClick(editor, Qt::LeftButton, Qt::NoModifier, caretPixel(editor, 0, 0));
    QVERIFY(!editor->hasSelection());
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));
}

void TestCodeEditor::multiClickLinesSelectWholeLinesAndGrow()
{
    editor->setPlainText(QStringLiteral("alpha\nbeta gamma\ngamma2"));
    const QPoint p = caretPixel(editor, 1, 1);

    // Two chained double-clicks at the SAME pixel = triple-click semantics
    // (Qt folds a fast click run into DblClick events; the detection rides
    // timestamps + the landing spot).
    QTest::mouseDClick(editor, Qt::LeftButton, Qt::NoModifier, p);
    QCOMPARE(editor->selectedText(), QStringLiteral("beta")); // first: the word
    QTest::mouseDClick(editor, Qt::LeftButton, Qt::NoModifier, p);
    QCOMPARE(editor->selectedText(), QStringLiteral("beta gamma\n")); // whole line + newline
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 0}));
    QCOMPARE(editor->additionalCaretCount(), 0);

    // 4th chained click grows the span to the next line; the last line
    // stops at EOL (there is no trailing newline to include).
    QTest::mouseDClick(editor, Qt::LeftButton, Qt::NoModifier, p);
    QCOMPARE(editor->selectedText(), QStringLiteral("beta gamma\ngamma2"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 6}));

    // 5th: at the document bottom the growth FLIPS upward.
    QTest::mouseDClick(editor, Qt::LeftButton, Qt::NoModifier, p);
    QCOMPARE(editor->selectedText(), QStringLiteral("alpha\nbeta gamma\ngamma2"));

    // A plain re-press of a grown line selection ARMS the M16 press-hold
    // (the selection survives the press for a possible text drag)...
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier, p);
    QVERIFY(editor->hasSelection());
    // ...and the release without a drag finalizes the collapse: bare caret
    // at the release point, chain broken.
    QTest::mouseRelease(editor, Qt::LeftButton, Qt::NoModifier, p);
    QVERIFY(!editor->hasSelection());
    // So the next double-click starts over on the word (not the line).
    QTest::mouseDClick(editor, Qt::LeftButton, Qt::NoModifier, p);
    QCOMPARE(editor->selectedText(), QStringLiteral("beta"));
}

void TestCodeEditor::wordDragExtendsByWholeWords()
{
    editor->setPlainText(QStringLiteral("one two three four"));
    auto sendMove = [this](const QPoint& to) {
        const QPointF p(to);
        QMouseEvent move(QEvent::MouseMove, p, p, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QTEST_ASSERT(QApplication::sendEvent(editor, &move));
    };

    QTest::mouseDClick(editor, Qt::LeftButton, Qt::NoModifier, caretPixel(editor, 0, 1));
    QCOMPARE(editor->selectedText(), QStringLiteral("one"));

    // Drag right: crossing the space adds the WHOLE word at once.
    sendMove(caretPixel(editor, 0, 9)); // inside "three"
    QCOMPARE(editor->selectedText(), QStringLiteral("one two three"));

    // Back left across "three"'s edge: only whole words remain.
    sendMove(caretPixel(editor, 0, 5)); // inside "two"
    QCOMPARE(editor->selectedText(), QStringLiteral("one two"));
    // Inside the anchor word: the selection collapses back to the pins.
    sendMove(caretPixel(editor, 0, 1));
    QCOMPARE(editor->selectedText(), QStringLiteral("one"));

    const QPoint end = caretPixel(editor, 0, 1);
    const QPointF e(end);
    QMouseEvent release(QEvent::MouseButtonRelease, e, e, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QTEST_ASSERT(QApplication::sendEvent(editor, &release));
    QCOMPARE(editor->selectedText(), QStringLiteral("one"));

    // Release disarms the gesture: the next held move is CHAR-wise (a
    // mid-word stop, no snap to the word edge).
    sendMove(caretPixel(editor, 0, 1));
    QCOMPARE(editor->selectedText(), QStringLiteral("o"));
}

void TestCodeEditor::ctrlDSelectsWordThenNextOccurrences()
{
    editor->setPlainText(QStringLiteral("foo bar foo baz\nfoo"));
    editor->setPosition({0, 0});

    // First press: the caret CONVERTS into its own word's selection
    // (VS Code parity: no duplicate slot parked on top of the caret).
    QTest::keyClick(editor, Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(editor->selectedText(), QStringLiteral("foo"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));
    QCOMPARE(editor->additionalCaretCount(), 0);

    // Next press: the next occurrence becomes a NEW SELECTED slot, and
    // additionalCarets() reports the slot caret = the match END.
    QTest::keyClick(editor, Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(editor->additionalCarets(), (QVector<qmn::view::TextPosition>{{0, 11}}));
    QTest::keyClick(editor, Qt::Key_D, Qt::ControlModifier); // next line too: no wrap
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 11}, {1, 3}}));

    // Past the last occurrence: no-op WITH a selection (this was the last
    // match), never a wrap.
    QTest::keyClick(editor, Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(editor->additionalCaretCount(), 2);
    QCOMPARE(editor->selectedText(), QStringLiteral("foo"));

    // The new slots are REAL carets: typing replaces every selection in
    // one fan-out step...
    QTest::keyClick(editor, Qt::Key_X);
    QCOMPARE(editor->toPlainText(), QStringLiteral("x bar x baz\nx"));
    // ...which undo restores in one step.
    editor->undo();
    QCOMPARE(editor->toPlainText(), QStringLiteral("foo bar foo baz\nfoo"));
}

void TestCodeEditor::ctrlDEdgeCases()
{
    // Empty document: wordless caret, nothing to add, nothing crashes.
    editor->setPlainText(QString());
    QTest::keyClick(editor, Qt::Key_D, Qt::ControlModifier);
    QVERIFY(!editor->hasSelection());
    QCOMPARE(editor->additionalCaretCount(), 0);

    // Caret MID-word (not at the word start): the word becomes the search
    // target (one contiguous Word-class run is ONE word in the M2 model,
    // so "xfooz" would be a single word) and the next occurrence lands as
    // an extra slot; the primary caret itself stays unselected.
    editor->setPlainText(QStringLiteral("fooz fooz"));
    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(editor->additionalCarets(), (QVector<qmn::view::TextPosition>{{0, 9}}));
    QVERIFY(!editor->hasSelection());

    // Whitespace under the caret: the PURE space run is the word (right
    // pivot), and the caret at its start converts like any other word.
    editor->setPlainText(QStringLiteral("foo bar foo"));
    editor->setPosition({0, 3});
    QTest::keyClick(editor, Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(editor->selectedText(), QStringLiteral(" "));
    QCOMPARE(editor->additionalCaretCount(), 0);
    QTest::keyClick(editor, Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(editor->additionalCarets(), (QVector<qmn::view::TextPosition>{{0, 8}}));

    // Multi-line primary selection: v1 degrades to the caret's word; the
    // hit at the selection's own end line becomes an extra slot, and past
    // it Ctrl+D is a no-op.
    editor->setPlainText(QStringLiteral("aa foo\nfoo bb"));
    dragSelect(editor, caretPixel(editor, 0, 3), caretPixel(editor, 1, 0));
    QCOMPARE(editor->selectedText(), QStringLiteral("foo\n"));
    QTest::keyClick(editor, Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(editor->additionalCarets(), (QVector<qmn::view::TextPosition>{{1, 3}}));
    QTest::keyClick(editor, Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(editor->additionalCaretCount(), 1);

    // Alt+D is NOT Ctrl+D: the chord stays unclaimed chrome.
    editor->setPlainText(QStringLiteral("foo foo"));
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_D, Qt::AltModifier);
    QVERIFY(!editor->hasSelection());
    QCOMPARE(editor->additionalCaretCount(), 0);
}

// --- context menu + text DnD (M16) ------------------------------------------

void TestCodeEditor::contextMenuPlacementKeepsOrMovesCaret()
{
    MenuProbe probe;
    probe.resize(600, 400);
    probe.setPlainText(QStringLiteral("hello world"));
    dragSelect(&probe, caretPixel(&probe, 0, 0), caretPixel(&probe, 0, 5)); // "hello"
    QCOMPARE(probe.selectedText(), QStringLiteral("hello"));

    // Right-click STRICTLY inside the live selection: the selection stands,
    // the caret stays put (the menu then acts on it).
    probe.prepareContextMenuAt(caretPixel(&probe, 0, 2));
    QVERIFY(probe.hasSelection());
    QCOMPARE(probe.selectedText(), QStringLiteral("hello"));
    QCOMPARE(probe.position(), (qmn::view::TextPosition{0, 5}));

    // Also AT the selection's left edge: the menu gate is INCLUSIVE of both
    // ends (wider than the left-drag arm, which needs interior room).
    probe.prepareContextMenuAt(caretPixel(&probe, 0, 0));
    QVERIFY(probe.hasSelection());
    QCOMPARE(probe.selectedText(), QStringLiteral("hello"));

    // Outside the selection: the caret is re-placed first (Qt convention).
    probe.prepareContextMenuAt(caretPixel(&probe, 0, 9));
    QVERIFY(!probe.hasSelection());
    QCOMPARE(probe.position(), (qmn::view::TextPosition{0, 9}));
}

void TestCodeEditor::contextMenuChromeGuardsSkipMenu()
{
    editor->setPlainText(QStringLiteral("hello world"));
    editor->setPosition({0, 3});

    // These are the ONLY real context events an offscreen test may send: the
    // chrome guards return BEFORE QMenu::exec (which blocks forever here).
    // Overview ruler strip: like the ruler press guard, no caret, no menu.
    QPoint rulerPt(editor->width() - CodeEditor::kOverviewRulerWidth / 2,
                   editor->padding() + editor->lineHeight() / 2);
    QContextMenuEvent e1(QContextMenuEvent::Mouse, rulerPt, rulerPt);
    QVERIFY(QApplication::sendEvent(editor, &e1));
    QVERIFY(e1.isAccepted());
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3})); // unmoved
    QVERIFY(editor->findChildren<QMenu*>().isEmpty());

    // Fold zone at the gutter's right edge: same guard.
    const int fz = editor->foldZoneWidth();
    QVERIFY(fz > 0);
    QPoint zonePt(editor->contentLeft() - fz / 2,
                  editor->padding() + editor->lineHeight() / 2);
    QContextMenuEvent e2(QContextMenuEvent::Mouse, zonePt, zonePt);
    QVERIFY(QApplication::sendEvent(editor, &e2));
    QVERIFY(e2.isAccepted());
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));
    QVERIFY(editor->findChildren<QMenu*>().isEmpty());
}

void TestCodeEditor::contextMenuActionsMirrorChords()
{
    MenuProbe probe;
    probe.resize(600, 400);
    probe.setPlainText(QStringLiteral("abc\ndef"));
    probe.setPosition({0, 0});

    // init() left the clipboard empty: Paste gates on it, the rest stay
    // ENABLED without a selection (they do the whole-line cut/copy the Ctrl
    // chords already implement — M1b parity).
    QMenu* menu = probe.createContextMenu();
    QAction* cut = menuAction(menu, QStringLiteral("Cut"));
    QAction* copy = menuAction(menu, QStringLiteral("Copy"));
    QAction* paste = menuAction(menu, QStringLiteral("Paste"));
    QAction* all = menuAction(menu, QStringLiteral("Select All"));
    QVERIFY(cut && copy && paste && all);
    QVERIFY(cut->isEnabled() && copy->isEnabled() && all->isEnabled());
    QVERIFY(!paste->isEnabled());

    cut->trigger(); // no selection: line cut of "abc\n" (Monaco semantics)
    QCOMPARE(probe.toPlainText(), QStringLiteral("def"));
    QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard),
             QStringLiteral("abc\n"));
    delete menu;

    // Fresh menu: Paste is now offered; triggering it re-inserts the cut line.
    QMenu* menu2 = probe.createContextMenu();
    // Re-fetch ALL actions from menu2: delete menu destroyed menu1's
    // actions with it (QMenu owns them), so any stale QAction* would be a
    // dangling deref (freed-heap reuse only shows up as a crash, as the
    // paired-run segfault at QAction::activate proved).
    paste = menuAction(menu2, QStringLiteral("Paste"));
    QVERIFY(paste && paste->isEnabled());
    paste->trigger();
    QCOMPARE(probe.toPlainText(), QStringLiteral("abc\ndef"));
    QCOMPARE(probe.position(), (qmn::view::TextPosition{1, 0})); // caret after "abc\n"

    all = menuAction(menu2, QStringLiteral("Select All"));
    all->trigger();
    QCOMPARE(probe.selectedText(), probe.toPlainText());
    copy = menuAction(menu2, QStringLiteral("Copy"));
    QVERIFY(copy && copy->isEnabled());
    copy->trigger();
    QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard),
             probe.toPlainText());
    delete menu2;
}

void TestCodeEditor::pressInsideSelectionHoldsForDrag()
{
    editor->setPlainText(QStringLiteral("abcdefgh"));
    dragSelect(editor, caretPixel(editor, 0, 1), caretPixel(editor, 0, 4)); // [1,4) "bcd"
    QCOMPARE(editor->selectedText(), QStringLiteral("bcd"));

    // A plain press STRICTLY inside the live selection does not collapse it...
    const QPoint in = caretPixel(editor, 0, 2);
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier, in);
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("bcd"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4})); // caret untouched

    // ...moves under startDragDistance are swallowed (held, not resized)...
    const QPoint nudge = in + QPoint(2, 0); // 2 px: well under the drag start
    const QPointF np(nudge);
    QMouseEvent move(QEvent::MouseMove, np, np, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QVERIFY(QApplication::sendEvent(editor, &move));
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("bcd"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));

    // ...and a release that never grew into a drag collapses AT the release
    // point (still col 2: the nearest-edge walk-back floors the nudge).
    QTest::mouseRelease(editor, Qt::LeftButton, Qt::NoModifier, nudge);
    QVERIFY(!editor->hasSelection());
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));
}

void TestCodeEditor::dragGestureOffscreenIsPayloadSafe()
{
    editor->setPlainText(QStringLiteral("abcdefgh"));
    editor->show(); // the QDrag path mirrors the shown-widget measurement
    dragSelect(editor, caretPixel(editor, 0, 1), caretPixel(editor, 0, 4)); // [1,4) "bcd"
    QCOMPARE(editor->selectedText(), QStringLiteral("bcd"));

    const QPoint from = caretPixel(editor, 0, 2);
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier, from);
    QVERIFY(editor->hasSelection());

    // Past startDragDistance the real QDrag::exec runs; offscreen it returns
    // NoAction synchronously and delivers nothing (measured), so the editor
    // must come out unscathed: no edit, nothing undoable, and the SOURCE
    // selection survives (Copy is the effective action offscreen).
    const QPoint to = caretPixel(editor, 0, 7);
    const QPointF tp(to);
    QMouseEvent move(QEvent::MouseMove, tp, tp, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QVERIFY(QApplication::sendEvent(editor, &move));
    QTest::mouseRelease(editor, Qt::LeftButton, Qt::NoModifier, to);

    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefgh"));
    QVERIFY(!editor->canUndo());
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("bcd"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));
}

void TestCodeEditor::dropTextInsertsAtDropPoint()
{
    editor->setPlainText(QStringLiteral("abcdefgh"));

    QMimeData md;
    md.setText(QStringLiteral("P\r\nQ")); // CRLF must be sanitized (paste parity)
    QVERIFY(sendDropAt(editor, caretPixel(editor, 0, 4), Qt::CopyAction, md));
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdP\nQefgh"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 1})); // caret at inserted end

    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier); // one undo for the whole drop
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefgh"));

    // A urls payload IS text to Qt (QMimeData::text() mirrors the stored
    // uri-list), so the editor takes it — the classic fallback that lands
    // a file-manager drop's path as plain text.
    QMimeData urls;
    urls.setUrls({QUrl(QStringLiteral("https://qt.io"))});
    QVERIFY(sendDropAt(editor, caretPixel(editor, 0, 2), Qt::CopyAction, urls));
    QCOMPARE(editor->toPlainText(), QStringLiteral("abhttps://qt.iocdefgh"));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefgh"));

    // Empty text: the format IS present (hasText), so the drag is accepted,
    // but there is nothing to insert — no edit, not even an empty undo step.
    QMimeData empty;
    empty.setText(QString());
    QVERIFY(sendDropAt(editor, caretPixel(editor, 0, 2), Qt::CopyAction, empty));
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefgh"));
    QVERIFY(!editor->canUndo());

    // A drop over the gutter clamps to the nearest text edge, just like a
    // click does (positionAt), landing the insert at the line start.
    QMimeData hash;
    hash.setText(QStringLiteral("#"));
    sendDropAt(editor, QPoint(editor->contentLeft() - 30,
                              editor->padding() + editor->lineHeight() / 2),
               Qt::CopyAction, hash);
    QCOMPARE(editor->toPlainText(), QStringLiteral("#abcdefgh"));
}

void TestCodeEditor::dropMoveMarkerMovesSelection()
{
    QMimeData md;
    md.setText(QStringLiteral("bcd"));
    md.setData(QStringLiteral("application/x-qmn-selection"), moveMarkerFor(editor, 1, 4));
    const auto rearm = [this] {
        editor->setPlainText(QStringLiteral("abcdefgh"));
        dragSelect(editor, caretPixel(editor, 0, 1), caretPixel(editor, 0, 4)); // [1,4) "bcd"
    };

    // (a) move right (drop after the source): ONE bracketed step remaps
    // "a|bcd|efgh" -> "aef|bcd|gh", caret parked at the moved block's end.
    rearm();
    sendDropAt(editor, caretPixel(editor, 0, 6), Qt::MoveAction, md);
    QCOMPARE(editor->toPlainText(), QStringLiteral("aefbcdgh"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 6}));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier); // undo kills BOTH halves
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefgh"));

    // (b) move left (drop before the source): "bcdaefgh".
    rearm();
    sendDropAt(editor, caretPixel(editor, 0, 0), Qt::MoveAction, md);
    QCOMPARE(editor->toPlainText(), QStringLiteral("bcdaefgh"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));

    // (c) drop INSIDE the dragged range: the drop point is swallowed by the
    // source, so nothing moves — a text-preserving no-op whose caret lands
    // at the source start (the same landing boundary drops take via the
    // remap table; overlapping del+ins never reaches applyBlockEdits).
    rearm();
    sendDropAt(editor, caretPixel(editor, 0, 2), Qt::MoveAction, md);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefgh"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));
}

void TestCodeEditor::dropIgnoresForeignAndCopyVariants()
{
    const auto selectBcd = [this] {
        editor->setPlainText(QStringLiteral("abcdefgh"));
        dragSelect(editor, caretPixel(editor, 0, 1), caretPixel(editor, 0, 4)); // [1,4) "bcd"
    };
    editor->setPlainText(QStringLiteral("abcdefgh")); // make the widget address real
    selectBcd();

    // Foreign source id (a sibling editor, or a copy round-tripped through
    // an external app): those offsets belong to ANOTHER buffer, so the drop
    // degrades to a plain copy — nothing gets deleted.
    QMimeData foreign;
    foreign.setText(QStringLiteral("bcd"));
    foreign.setData(QStringLiteral("application/x-qmn-selection"),
                    QByteArrayLiteral("deadbeef:1:4"));
    sendDropAt(editor, caretPixel(editor, 0, 6), Qt::MoveAction, foreign);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefbcdgh"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 9})); // caret at copied end

    // Own marker + Copy action (Ctrl modified the drag): the action gate
    // makes it a copy even though the marker would pass.
    selectBcd();
    QMimeData own;
    own.setText(QStringLiteral("bcd"));
    own.setData(QStringLiteral("application/x-qmn-selection"), moveMarkerFor(editor, 1, 4));
    sendDropAt(editor, caretPixel(editor, 0, 6), Qt::CopyAction, own);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefbcdgh"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 9}));

    // Own marker + Move, but the live selection no longer matches the marker
    // (caret collapsed after the drag armed): the offsets cannot be trusted,
    // so this too degrades to a copy.
    selectBcd();
    editor->setPosition({0, 4}); // collapse the selection the marker names
    sendDropAt(editor, caretPixel(editor, 0, 6), Qt::MoveAction, own);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefbcdgh"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 9}));
}

void TestCodeEditor::dragMoveAutoscrolls()
{
    QStringList lines;
    for (int i = 0; i < 100; ++i)
        lines << QStringLiteral("line %1").arg(i, 3);
    editor->setPlainText(lines.join(QLatin1Char('\n')));

    QMimeData md;
    md.setText(QStringLiteral("x"));
    const int lh = editor->lineHeight();

    // QDragMoveEvent/DropEvent are routed by QApplication::notify to the
    // QDragManager's CURRENT TARGET — which only an accepted DragEnter
    // sets (and a Drop clears). So synthetic drag-moves MUST be preceded
    // by a DragEnter send, or notify discards them before the widget ever
    // sees them (measured with Qt 6.12).
    QDragEnterEvent enter(QPointF(editor->width() - 1.0, editor->height() + 5.0),
                          Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
    QVERIFY(QApplication::sendEvent(editor, &enter));
    QVERIFY(enter.isAccepted());

    // Past the bottom edge: one line of scroll per drag-move event, and the
    // payload stays accepted — dragMoveEvent rides the same edge logic the
    // selection drag uses (edgeAutoScroll).
    QDragMoveEvent down(QPointF(editor->width() - 1.0, editor->height() + 5.0),
                        Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
    QVERIFY(QApplication::sendEvent(editor, &down));
    QVERIFY(down.isAccepted());
    QCOMPARE(editor->verticalScrollOffset(), lh);

    // Past the top edge: it scrolls back out.
    QDragMoveEvent up(QPointF(10, -5), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
    QVERIFY(QApplication::sendEvent(editor, &up));
    QVERIFY(up.isAccepted());
    QCOMPARE(editor->verticalScrollOffset(), 0);

    // A non-text payload is refused even at the move stage (dragEnter parity).
    QMimeData bin;
    bin.setData(QStringLiteral("application/x-qmn-binary"), QByteArray("1"));
    QDragMoveEvent noText(QPointF(10, 10), Qt::CopyAction, &bin, Qt::LeftButton, Qt::NoModifier);
    QVERIFY(QApplication::sendEvent(editor, &noText));
    QVERIFY(!noText.isAccepted());

    // DragLeave ends the gesture and (via notify) clears the manager's
    // target, so no stale routing state leaks into later tests.
    QDragLeaveEvent leave;
    QApplication::sendEvent(editor, &leave);
}

void TestCodeEditor::dropCaretTracksDragTarget()
{
    // Numeric pixel proof of the drop-target caret (no vision here): park
    // it on an EMPTY line whose band holds zero other ink (the real caret
    // never leaves {0,0}), and scan the band for exact-Text-color pixels:
    // a solid 2px x lh bar, unblinking by construction, so grabs are
    // deterministic. Lines 1+2 of the doc are empty for exactly that.
    editor->setPlainText(QStringLiteral("abcdefgh\n\n"));
    editor->setPosition({0, 0});
    const int pad = editor->padding();
    const int lh = editor->lineHeight();
    const QRgb ink = editor->palette().color(QPalette::Text).rgb();
    const int scanL = editor->contentLeft(); // the bar at col 0 lands here
    const int scanR = scanL + int(6 * editor->cellWidth());
    auto inkInBand = [&](int line) { // {count, leftmostX} over the line band
        const QImage img = editor->grab().toImage();
        int n = 0, left = -1;
        for (int x = scanL; x < scanR; ++x)
            for (int y = pad + line * lh; y < pad + (line + 1) * lh; ++y)
                if (img.pixel(x, y) == ink) {
                    ++n;
                    if (left < 0)
                        left = x;
                }
        return std::make_pair(n, left);
    };
    QCOMPARE(inkInBand(1).first, 0); // baseline: clean bands, no mark
    QCOMPARE(inkInBand(2).first, 0);

    QMimeData md;
    md.setText(QStringLiteral("Z"));

    // DragEnter already shows the mark: full-height bar at the positionAt
    // clamp of the drag point (empty line -> col 0 -> contentLeft).
    const QPointF at1(caretPixel(editor, 1, 0));
    QDragEnterEvent enter(at1, Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
    QVERIFY(QApplication::sendEvent(editor, &enter));
    QVERIFY(enter.isAccepted());
    QCOMPARE(inkInBand(1).first, 2 * lh);
    QCOMPARE(inkInBand(1).second, scanL);

    // A drag-move RELOCATES it (never doubles it): old band clean again.
    const QPointF at2(caretPixel(editor, 2, 0));
    QDragMoveEvent move(at2, Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
    QVERIFY(QApplication::sendEvent(editor, &move));
    QVERIFY(move.isAccepted());
    QCOMPARE(inkInBand(1).first, 0);
    QCOMPARE(inkInBand(2).first, 2 * lh);
    QCOMPARE(inkInBand(2).second, scanL);

    // Drag off: the dragLeaveEvent override clears the mark (QWidget's
    // default impl ignored the event, and the bar would linger forever).
    QDragLeaveEvent leave;
    QVERIFY(QApplication::sendEvent(editor, &leave));
    QVERIFY(leave.isAccepted());
    QCOMPARE(inkInBand(2).first, 0);

    // And the drop itself clears it too — proven with an ACCEPTED-but-
    // EMPTY-text payload (hasText, nothing to insert) so no text ink
    // pollutes the band and the model stays untouched for the undo check.
    QMimeData empty;
    empty.setText(QString());
    QVERIFY(sendDropAt(editor, caretPixel(editor, 2, 0), Qt::CopyAction, empty));
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefgh\n\n"));
    QVERIFY(!editor->canUndo());
    QCOMPARE(inkInBand(2).first, 0);
}

void TestCodeEditor::shiftAtDropCopiesInsteadOfMoves()
{
    // Shift held at drop forces a COPY even when the drop carries THIS
    // editor's own live move marker (the marker+Move shape that
    // dropMoveMarkerMovesSelection moves). Offscreen exec never delivers
    // a drop, so the source-side delete can never arm in tests — the
    // model-level copy IS the observable half of the feature.
    editor->setPlainText(QStringLiteral("abcdefgh")); // make THIS address the marked one
    QMimeData md;
    md.setText(QStringLiteral("bcd"));
    md.setData(QStringLiteral("application/x-qmn-selection"), moveMarkerFor(editor, 1, 4));
    const auto rearm = [this] {
        editor->setPlainText(QStringLiteral("abcdefgh"));
        dragSelect(editor, caretPixel(editor, 0, 1), caretPixel(editor, 0, 4)); // [1,4) "bcd"
    };

    // Own marker + Move + Shift -> copy: source stays put, one undo step.
    rearm();
    QVERIFY(sendDropAt(editor, caretPixel(editor, 0, 6), Qt::MoveAction, md, Qt::ShiftModifier));
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefbcdgh"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 9})); // caret at copied end
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefgh"));

    // The very same events WITHOUT Shift: still a MOVE (the new gate must
    // not leak into the normal path).
    rearm();
    sendDropAt(editor, caretPixel(editor, 0, 6), Qt::MoveAction, md);
    QCOMPARE(editor->toPlainText(), QStringLiteral("aefbcdgh"));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefgh"));

    // Own marker + Copy-proposed + Shift: the shape Qt's X11/Wayland
    // sources already invert to for Shift drags — stays a plain copy.
    rearm();
    sendDropAt(editor, caretPixel(editor, 0, 6), Qt::CopyAction, md, Qt::ShiftModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefbcdgh"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 9}));

    // Shift cannot arm a move the marker never could (foreign id -> copy
    // with or without it).
    rearm();
    QMimeData foreign;
    foreign.setText(QStringLiteral("bcd"));
    foreign.setData(QStringLiteral("application/x-qmn-selection"),
                    QByteArrayLiteral("deadbeef:1:4"));
    sendDropAt(editor, caretPixel(editor, 0, 6), Qt::MoveAction, foreign, Qt::ShiftModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abcdefbcdgh"));
}

// --- M17 code font + zoom ----------------------------------------------------

void TestCodeEditor::setCodeFontRebuildsMetricsAndGeometry()
{
    editor->setPlainText(QStringLiteral("hello world"));

    const double cw1 = editor->cellWidth();
    const int lh1 = editor->lineHeight();
    QVERIFY(cw1 > 0.0);
    QVERIFY(lh1 > 0);

    // A font exactly 2x the shipped pixel size: codeFont() reports the BASE,
    // QWidget::font() mirrors the effective font, and the no-hinting design
    // advances make the cell width scale (essentially) linearly.
    QFont dbl = editor->codeFont();
    QVERIFY(dbl.pixelSize() >= 8);
    dbl.setPixelSize(dbl.pixelSize() * 2);
    editor->setCodeFont(dbl);
    QCOMPARE(editor->codeFont().pixelSize(), dbl.pixelSize());
    QCOMPARE(editor->font().pixelSize(), dbl.pixelSize());
    const double cw2 = editor->cellWidth();
    const int lh2 = editor->lineHeight();
    QVERIFY(cw2 > cw1 * 1.5);
    QVERIFY(qAbs(cw2 - 2.0 * cw1) < 0.05); // exact linear scaling of advances
    QVERIFY(lh2 >= lh1 * 2 - 2 && lh2 <= lh1 * 2 + 2);

    // Geometry proof: the caret pixel of a DEEP column round-trips through
    // positionAt with the NEW metrics. A stale geo cache (the font is NOT in
    // the cache key!) or skipped re-measurement lands on the wrong column.
    const int deep = 20;
    editor->resize(800, 400);
    editor->setPlainText(QString(deep + 5, QLatin1Char('a')));
    QCOMPARE(editor->positionAt(caretPixel(editor, 0, deep)).col, deep);
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier,
                      caretPixel(editor, 0, deep));
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, deep);

    // Undoing the font change restores the OLD metrics exactly — the same
    // QFont measures deterministically, and the caches get wiped again.
    QFont base = editor->codeFont();
    base.setPixelSize(base.pixelSize() / 2);
    editor->setCodeFont(base);
    QCOMPARE(editor->cellWidth(), cw1);
    QCOMPARE(editor->lineHeight(), lh1);
    QCOMPARE(editor->positionAt(caretPixel(editor, 0, deep)).col, deep);
}

void TestCodeEditor::zoomFactorClampsAndSyncsWidgetFont()
{
    editor->setCodeFont([&] { QFont f = editor->codeFont();
                                f.setPixelSize(20); return f; }());
    QCOMPARE(editor->codeFont().pixelSize(), 20);
    QCOMPARE(editor->zoomFactor(), 1.0);
    QCOMPARE(editor->font().pixelSize(), 20);

    // Steps multiply by 5/4 — exact in binary from 1.0, so these QCOMPAREs
    // of doubles are safe (no 0.8-style round-trips in this chain).
    editor->zoomIn();
    QCOMPARE(editor->zoomFactor(), 1.25);
    QCOMPARE(editor->font().pixelSize(), 25); // qRound(20*1.25)
    editor->zoomIn();
    QCOMPARE(editor->zoomFactor(), 1.5625);
    QCOMPARE(editor->font().pixelSize(), 31); // qRound(31.25)
    editor->zoomOut();
    QCOMPARE(editor->zoomFactor(), 1.25);
    QCOMPARE(editor->zoomFactor(), CodeEditor::kZoomFactorStep);
    editor->resetZoom();
    QCOMPARE(editor->zoomFactor(), 1.0);
    QCOMPARE(editor->font().pixelSize(), 20);

    // Clamping: out-of-range requests clamp to the published bounds and the
    // single-step chords stop at them too.
    editor->setZoomFactor(99.0);
    QCOMPARE(editor->zoomFactor(), CodeEditor::kZoomFactorMax);
    QCOMPARE(editor->font().pixelSize(), 80); // qRound(20*4)
    editor->zoomIn();
    QCOMPARE(editor->zoomFactor(), CodeEditor::kZoomFactorMax);
    editor->setZoomFactor(0.001);
    QCOMPARE(editor->zoomFactor(), CodeEditor::kZoomFactorMin);
    QCOMPARE(editor->font().pixelSize(), 5); // qRound(20*0.25)
    editor->zoomOut();
    QCOMPARE(editor->zoomFactor(), CodeEditor::kZoomFactorMin);
    editor->zoomIn();
    QCOMPARE(editor->zoomFactor(), 0.3125); // 0.25*1.25, exact

    // Zooming NEVER touches the base: codeFont() keeps telling the truth.
    QCOMPARE(editor->codeFont().pixelSize(), 20);

    // A font step that rounds to the SAME effective pixels lands us at the
    // clamp boundary anyway; the early-return is about the FACTOR being
    // unchanged, and applyCodeFont's comment documents why a same-rounded
    // factor change is safe to skip (QWidget fires no FontChange then).
    editor->resetZoom();
    QCOMPARE(editor->font().pixelSize(), 20);
    QCOMPARE(editor->zoomFactor(), 1.0);
    QCOMPARE(editor->codeFont().pixelSize(), 20);
}

void TestCodeEditor::zoomChordsZoomAndReset()
{
    editor->setPlainText(QStringLiteral("plain text"));
    editor->resetZoom();
    const double f1 = editor->zoomFactor();
    QCOMPARE(f1, 1.0);

    // The canonical Ctrl+= ...
    QTest::keyClick(editor, Qt::Key_Equal, Qt::ControlModifier);
    QCOMPARE(editor->zoomFactor(), 1.25);
    // ... the Shifted spelling Ctrl++ that real keyboards deliver ...
    QTest::keyClick(editor, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(editor->zoomFactor(), 1.5625);
    // ... Ctrl+- down ...
    QTest::keyClick(editor, Qt::Key_Minus, Qt::ControlModifier);
    QCOMPARE(editor->zoomFactor(), 1.25);
    // ... and the Shifted Ctrl+_ spelling.
    QTest::keyClick(editor, Qt::Key_Underscore, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(editor->zoomFactor(), 1.0);

    // Ctrl+0 resets from wherever the zoom stood (Key_0 and the numpad's
    // Key_Period code — Qt has no Kp* constants, the numpad reuses these).
    QTest::keyClick(editor, Qt::Key_Equal, Qt::ControlModifier);
    QTest::keyClick(editor, Qt::Key_Equal, Qt::ControlModifier);
    QVERIFY(editor->zoomFactor() > 1.0);
    QTest::keyClick(editor, Qt::Key_0, Qt::ControlModifier);
    QCOMPARE(editor->zoomFactor(), 1.0);
    QTest::keyClick(editor, Qt::Key_Equal, Qt::ControlModifier);
    QTest::keyClick(editor, Qt::Key_Period, Qt::ControlModifier);
    QCOMPARE(editor->zoomFactor(), 1.0);

    // Keypad modifier sprinkles (NumLock) must not break the chords.
    QTest::keyClick(editor, Qt::Key_Equal, Qt::ControlModifier | Qt::KeypadModifier);
    QCOMPARE(editor->zoomFactor(), 1.25);
    QTest::keyClick(editor, Qt::Key_Period,
                    Qt::ControlModifier | Qt::KeypadModifier);
    QCOMPARE(editor->zoomFactor(), 1.0);

    // Unclaimed variants fall through untouched: Ctrl+Alt combos stay free
    // chrome, and plain Ctrl+= without Alt claims, but the bare keys keep
    // TYPING (a lone '=' must land in the document, not zoom).
    QTest::keyClick(editor, Qt::Key_Equal, Qt::ControlModifier | Qt::AltModifier);
    QCOMPARE(editor->zoomFactor(), 1.0);
    QTest::keyClick(editor, Qt::Key_Minus, Qt::ControlModifier | Qt::AltModifier);
    QCOMPARE(editor->zoomFactor(), 1.0);
    editor->setPlainText(QString());
    QTest::keyClick(editor, Qt::Key_Equal); // no Ctrl: types '='
    QCOMPARE(editor->toPlainText(), QStringLiteral("="));
    QCOMPARE(editor->zoomFactor(), 1.0);
}

void TestCodeEditor::ctrlWheelZoomsAndPlainWheelScrolls()
{
    editor->resize(500, 300);
    QString text;
    for (int i = 0; i < 200; ++i)
        text += QStringLiteral("line %1\n").arg(i);
    editor->setPlainText(text);
    editor->resetZoom();

    auto wheel = [&](int dy, Qt::KeyboardModifiers mods) {
        const QPointF p(200, 150);
        // Qt 6.12 has ONE public ctor (the old Orientation overload is gone,
        // measured): pass phase+inverted and let source/device default —
        // an explicit null device SEGFAULTS the QSinglePointEvent ctor on
        // offscreen (measured), the primary pointing device is a dummy there.
        QWheelEvent ev(p, p, QPoint(0, 0), QPoint(0, dy), Qt::NoButton, mods,
                       Qt::ScrollUpdate, false);
        QApplication::sendEvent(editor, &ev);
    };

    // Ctrl+wheel UP zooms the FONT in, one notch = one step, and the
    // viewport must not scroll (this is the whole point of the guard).
    const int scroll0 = editor->verticalScrollOffset();
    wheel(120, Qt::ControlModifier);
    QCOMPARE(editor->zoomFactor(), 1.25);
    QCOMPARE(editor->verticalScrollOffset(), scroll0);
    wheel(120, Qt::ControlModifier);
    QCOMPARE(editor->zoomFactor(), 1.5625);
    QCOMPARE(editor->verticalScrollOffset(), scroll0);
    // Ctrl+wheel DOWN mirrors it.
    wheel(-120, Qt::ControlModifier);
    QCOMPARE(editor->zoomFactor(), 1.25);
    QCOMPARE(editor->verticalScrollOffset(), scroll0);
    editor->resetZoom();
    QCOMPARE(editor->zoomFactor(), 1.0);

    // Sub-notch trackpad deltas ACCUMULATE to whole steps.
    wheel(24, Qt::ControlModifier);
    wheel(24, Qt::ControlModifier);
    wheel(24, Qt::ControlModifier); // 72: no step yet
    QCOMPARE(editor->zoomFactor(), 1.0);
    wheel(24, Qt::ControlModifier); // 96
    wheel(24, Qt::ControlModifier); // 120: ONE step
    QCOMPARE(editor->zoomFactor(), 1.25);

    // A plain wheel RESETS the accumulator (the leftover 0..119 must not
    // leak across into the next ctrl gesture).
    wheel(60, Qt::ControlModifier); // acc 60
    wheel(-120, Qt::NoModifier);    // plain: scrolls, resets acc
    QVERIFY(editor->verticalScrollOffset() > scroll0);
    const double fAcc = editor->zoomFactor();
    wheel(80, Qt::ControlModifier); // would have hit 140 without the reset
    QCOMPARE(editor->zoomFactor(), fAcc);
    wheel(40, Qt::ControlModifier); // 80+40 == 120: exactly one step now
    QCOMPARE(editor->zoomFactor(), fAcc * 1.25);

    // Runaway-gesture cap: one event, however many notches, steps at most 5
    // — and the factor clamp catches it anyway (1.25^6 of this chain > 4).
    wheel(12000, Qt::ControlModifier);
    QCOMPARE(editor->zoomFactor(), CodeEditor::kZoomFactorMax);

    // Plain wheel scrolls the view and zooms NOTHING.
    editor->resetZoom();
    const int before = editor->verticalScrollOffset();
    wheel(-120, Qt::NoModifier);
    QCOMPARE(editor->zoomFactor(), 1.0);
    QVERIFY(editor->verticalScrollOffset() > before);
}

void TestCodeEditor::zoomScalesChromeAndHitTesting()
{
    // Ten lines -> the gutter's digit column is exactly 2 cells wide; all
    // chrome geometry derives LIVE from cellWidth_, no extra invalidation.
    QString text;
    for (int i = 0; i < 12; ++i)
        text += QStringLiteral("0123456789\n");
    editor->setPlainText(text);
    editor->resetZoom();
    editor->resize(600, 400);

    const double cw1 = editor->cellWidth();
    const int gutter1 = editor->gutterWidth();
    const int zone1 = editor->foldZoneWidth();
    QCOMPARE(zone1, std::max(10, qRound(2.2 * cw1))); // the live formula
    const int left1 = editor->contentLeft();
    QCOMPARE(left1, editor->padding() + gutter1);

    editor->zoomIn();
    editor->zoomIn();
    QCOMPARE(editor->zoomFactor(), 1.5625);
    const double cw2 = editor->cellWidth();
    QVERIFY(cw2 > cw1);
    // The fold zone re-derives from the NEW cell width (the old constant
    // would be a stale-geometry tripwire); the gutter and the content edge
    // follow it, and the caret grid follows the gutter: deep-column clicks
    // still round-trip after zoom.
    QCOMPARE(editor->foldZoneWidth(), std::max(10, qRound(2.2 * cw2)));
    QVERIFY(editor->gutterWidth() > gutter1);
    QCOMPARE(editor->contentLeft(), editor->padding() + editor->gutterWidth());
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier,
                      caretPixel(editor, 0, 9));
    QCOMPARE(editor->position().col, 9);
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier,
                      caretPixel(editor, 5, 3));
    QCOMPARE(editor->position().line, 5);
    QCOMPARE(editor->position().col, 3);

    editor->resetZoom();
    QCOMPARE(editor->cellWidth(), cw1);
    QCOMPARE(editor->gutterWidth(), gutter1);
    QCOMPARE(editor->foldZoneWidth(), zone1);
}

void TestCodeEditor::externalSetFontRebasesBaseAndZoom()
{
    editor->setPlainText(QStringLiteral("hello"));
    editor->setCodeFont([&] { QFont f = editor->codeFont();
                                f.setPixelSize(14); return f; }());
    const double cw14 = editor->cellWidth();
    editor->zoomIn();
    editor->zoomIn();
    QCOMPARE(editor->zoomFactor(), 1.5625);
    QCOMPARE(editor->font().pixelSize(), qRound(14 * 1.5625)); // 22
    const double cwZoomed = editor->cellWidth();

    // Raw QWidget::setFont (app chrome code, a style nudge): the FOREIGN
    // font wins and REBASES — codeFont() reports it, zoom returns to 1.0,
    // and the metrics track the foreign font, not our old scaled one.
    QFont foreign = editor->codeFont();
    foreign.setPixelSize(20);
    editor->setFont(foreign);
    QCOMPARE(editor->zoomFactor(), 1.0);
    QCOMPARE(editor->codeFont().pixelSize(), 20);
    QCOMPARE(editor->font().pixelSize(), 20);
    const double cw20 = editor->cellWidth();
    QVERIFY(cw20 > cw14);
    QVERIFY(cw20 < cwZoomed); // 20px < the old 22px effective font
    // (the caret grid rebuilt under the foreign font):
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier,
                      caretPixel(editor, 0, 4));
    QCOMPARE(editor->position().col, 4);

    // And the zoom ladder now scales from the NEW base:
    editor->zoomIn();
    QCOMPARE(editor->zoomFactor(), 1.25);
    QCOMPARE(editor->font().pixelSize(), 25); // qRound(20*1.25), not 22*1.25

    // setCodeFont re-arms the BASE while the zoom factor keeps applying
    // (the documented one-knob-each model): 14px base under factor 1.25
    // draws qRound(17.5) == 18px, and resetZoom lands back on EXACT cw14.
    editor->setCodeFont([&] { QFont f = editor->codeFont();
                                f.setPixelSize(14); return f; }());
    QCOMPARE(editor->zoomFactor(), 1.25);
    QCOMPARE(editor->codeFont().pixelSize(), 14);
    QCOMPARE(editor->font().pixelSize(), 18);
    editor->resetZoom();
    QCOMPARE(editor->cellWidth(), cw14);
}

// --- M18 auto-close pairs + long-line ruler + current-line mode ------------

void TestCodeEditor::autoClosePairInsertsSkipsAndDeletesBoth()
{
    // VS Code parity pair set: bare caret inserts the pair with the caret
    // parked BETWEEN the two chars, the matching closer merely hops over,
    // and one backspace inside an empty pair kills BOTH sides.
    struct Case { Qt::Key open; Qt::Key close; const char* text; };
    const Case cases[] = {
        { Qt::Key_ParenLeft, Qt::Key_ParenRight, "()" },
        { Qt::Key_BracketLeft, Qt::Key_BracketRight, "[]" },
        { Qt::Key_BraceLeft, Qt::Key_BraceRight, "{}" },
        { Qt::Key_QuoteDbl, Qt::Key_QuoteDbl, "\"\"" },
        { Qt::Key_Apostrophe, Qt::Key_Apostrophe, "''" },
        { Qt::Key_QuoteLeft, Qt::Key_QuoteLeft, "``" },
    };
    for (const Case& c : cases) {
        editor->setPlainText(QString());
        QTest::keyClick(editor, c.open);
        QCOMPARE(editor->toPlainText(), QString::fromLatin1(c.text));
        QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1})); // BETWEEN
        QTest::keyClick(editor, c.close); // the closer already sits at the
        QCOMPARE(editor->toPlainText(), QString::fromLatin1(c.text));   // caret:
        QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));  // hop only
        editor->setPosition({0, 1});
        QTest::keyClick(editor, Qt::Key_Backspace); // caret inside the pair:
        QCOMPARE(editor->toPlainText(), QString());                     // BOTH die
        QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));
    }

    // The hop is motion-only: no undo entry, no chain freeze — pair, skip
    // and letter stay ONE coalesced typing chunk.
    editor->setPlainText(QString());
    QTest::keyClick(editor, Qt::Key_ParenLeft);
    QTest::keyClick(editor, Qt::Key_ParenRight); // skip: caret motion only
    QTest::keyClick(editor, Qt::Key_X);
    QCOMPARE(editor->toPlainText(), QStringLiteral("()x"));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QString());
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));
}

void TestCodeEditor::autoCloseWrapsSelection()
{
    // Open the selected text, caret ends up after the close, selection gone.
    editor->setPlainText(QStringLiteral("hello"));
    editor->selectAll();
    QTest::keyClick(editor, Qt::Key_ParenLeft);
    QCOMPARE(editor->toPlainText(), QStringLiteral("(hello)"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 7}));
    QVERIFY(!editor->hasSelection());

    // Quote wrap IGNORES the word-boundary guard (VS Code: the guard only
    // guards bare-caret pairing, never a deliberate wrap). One bracketed
    // undo step brings the selection back whole.
    editor->setPlainText(QStringLiteral("xhello"));
    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier); // select "hello"
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("hello"));
    QTest::keyClick(editor, Qt::Key_QuoteDbl);
    QCOMPARE(editor->toPlainText(), QStringLiteral("x\"hello\""));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 8}));
    QVERIFY(!editor->hasSelection());
    QVERIFY(editor->canUndo());
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("xhello"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 6}));
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("hello"));

    // Multi-line selection wrap: the payload spans lines, the caret parks
    // after the close on the final line.
    editor->setPlainText(QStringLiteral("a\nb"));
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_Down, Qt::ShiftModifier);
    QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier); // select "a\nb"
    QTest::keyClick(editor, Qt::Key_BraceLeft);
    QCOMPARE(editor->toPlainText(), QStringLiteral("{a\nb}"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 2}));
}

void TestCodeEditor::autoCloseQuotesRespectWordBoundary()
{
    // The apostrophe case: a quote typed right after word ink is prose,
    // not a pair opener — it stays a single plain char.
    editor->setPlainText(QStringLiteral("don"));
    editor->setPosition({0, 3}); // EOL, right after word ink
    QTest::keyClick(editor, Qt::Key_Apostrophe);
    QCOMPARE(editor->toPlainText(), QStringLiteral("don'"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));

    // Word ink on the RIGHT of a bare caret guards too.
    editor->setPlainText(QStringLiteral("t"));
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_Apostrophe);
    QCOMPARE(editor->toPlainText(), QStringLiteral("'t"));

    // '_' counts as wordish (identifier context).
    editor->setPlainText(QStringLiteral("a_1"));
    editor->setPosition({0, 3});
    QTest::keyClick(editor, Qt::Key_QuoteDbl);
    QCOMPARE(editor->toPlainText(), QStringLiteral("a_1\""));

    // Neutral context pairs normally — the guard is contextual, not global.
    editor->setPlainText(QString());
    QTest::keyClick(editor, Qt::Key_QuoteDbl);
    QCOMPARE(editor->toPlainText(), QStringLiteral("\"\""));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));

    // Brackets never guard: word context is irrelevant for them.
    editor->setPlainText(QStringLiteral("foo"));
    editor->setPosition({0, 3});
    QTest::keyClick(editor, Qt::Key_ParenLeft);
    QCOMPARE(editor->toPlainText(), QStringLiteral("foo()"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));
}

void TestCodeEditor::autoCloseSkipStaysTextualAndClosersPlain()
{
    // Skip only hops the caret: the model never sees an edit.
    editor->setPlainText(QStringLiteral("[]"));
    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_BracketRight);
    QCOMPARE(editor->toPlainText(), QStringLiteral("[]"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));

    // A closer typed where its char does NOT sit at the caret is ordinary
    // typing (the full pair never "resonates" against unrelated ink).
    editor->setPlainText(QStringLiteral("ab"));
    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_ParenRight);
    QCOMPARE(editor->toPlainText(), QStringLiteral("a)b"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));

    // Closer over a selection: plain replace — Skip needs no selection.
    editor->setPlainText(QStringLiteral("ab"));
    editor->selectAll();
    QTest::keyClick(editor, Qt::Key_BracketRight);
    QCOMPARE(editor->toPlainText(), QStringLiteral("]"));

    // At EOL nothing sits at the caret (the '\n'/EOF probe never matches),
    // so right after an empty pair the closer types for real.
    editor->setPlainText(QStringLiteral("()"));
    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_ParenRight);
    QCOMPARE(editor->toPlainText(), QStringLiteral("())"));
}

void TestCodeEditor::autoCloseFansOutMixedSlotsAsOneUndoStep()
{
    // Mixed slots ride ONE bracketed fan-out group. altClick promotes the
    // clicked slot to primary (D1), so primary identity follows the click.
    editor->setPlainText(QStringLiteral(")ab"));
    editor->setPosition({0, 0});
    altClick(editor, 0, 3); // primary {0,3} (Plain), extra {0,0} (Skip)
    QTest::keyClick(editor, Qt::Key_ParenRight);
    QCOMPARE(editor->toPlainText(), QStringLiteral(")ab)"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));
    QCOMPARE(editor->additionalCarets(),
             (QVector<qmn::view::TextPosition>{{0, 1}})); // the Skip slot
                                                          // parked past its
                                                          // re-inserted closer
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral(")ab")); // ONE step for
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3})); // the whole batch
    QCOMPARE(editor->additionalCarets(), (QVector<qmn::view::TextPosition>{{0, 0}}));

    // All slots Pair: still one bracketed group per press (D1 typing rule).
    editor->setPlainText(QStringLiteral("xy"));
    editor->setPosition({0, 0});
    altClick(editor, 0, 2);
    QTest::keyClick(editor, Qt::Key_ParenLeft);
    QCOMPARE(editor->toPlainText(), QStringLiteral("()xy()"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 5})); // inside
                                                                   // the pair
                                                                   // it opened
    QCOMPARE(editor->additionalCarets(), (QVector<qmn::view::TextPosition>{{0, 1}}));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("xy"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));
    QCOMPARE(editor->additionalCarets(), (QVector<qmn::view::TextPosition>{{0, 0}}));

    // All-Skip is pure motion: no text, no undo entry, no chain freeze —
    // the next Ctrl+Z still pops the earlier Pair group.
    editor->setPlainText(QStringLiteral("()\n()"));
    editor->setPosition({0, 1});
    altClick(editor, 1, 1);
    QTest::keyClick(editor, Qt::Key_ParenLeft); // both slots bare carets:
    QCOMPARE(editor->toPlainText(), QStringLiteral("(())\n(())")); // Pair fan-out
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 2}));
    QCOMPARE(editor->additionalCarets(), (QVector<qmn::view::TextPosition>{{0, 2}}));
    QTest::keyClick(editor, Qt::Key_ParenRight); // both now face a ')': all-Skip
    QCOMPARE(editor->toPlainText(), QStringLiteral("(())\n(())"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 3}));
    QCOMPARE(editor->additionalCarets(), (QVector<qmn::view::TextPosition>{{0, 3}}));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier); // pops the Pair
    QCOMPARE(editor->toPlainText(), QStringLiteral("()\n()")); // group only
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 1}));
    QCOMPARE(editor->additionalCarets(), (QVector<qmn::view::TextPosition>{{0, 1}}));

    // All-Plain bails out to the ordinary typing fan-out (the guard fired
    // on both slots: wordish char before every caret).
    editor->setPlainText(QStringLiteral("a\nb"));
    editor->setPosition({0, 1});
    altClick(editor, 1, 1);
    QTest::keyClick(editor, Qt::Key_Apostrophe);
    QCOMPARE(editor->toPlainText(), QStringLiteral("a'\nb'"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 2}));
    QCOMPARE(editor->additionalCarets(), (QVector<qmn::view::TextPosition>{{0, 2}}));
}

void TestCodeEditor::longLineRulerMapsToColumnAndRidesHScroll()
{
    // Pixel-differential (no vision): the ruler is exactly ONE lit column at
    // contentLeft + qRound(col*cw) - hScroll, full height, and nothing else
    // in the frame changes. Pin the current-line wash off so the backdrop
    // stays uniform Base.
    editor->setCurrentLineMode(CodeEditor::CurrentLineMode::Off);
    QCOMPARE(editor->longLineRulerColumn(), 0); // off by default
    editor->setLongLineRulerColumn(-7);
    QCOMPARE(editor->longLineRulerColumn(), 0); // negatives clamp to off

    const double cw = editor->cellWidth();
    const int x0 = editor->contentLeft();
    const int h = editor->height();
    editor->setPlainText(QStringLiteral("ab"));
    editor->setPosition({0, 0}); // caret col 0, far from the probe column
    const QImage base = editor->grab().toImage();

    auto diffAt = [h](const QImage& a, const QImage& b, int x) {
        int n = 0;
        for (int y = 0; y < h; ++y)
            if (a.pixelColor(x, y) != b.pixelColor(x, y))
                ++n;
        return n;
    };
    auto totalDiff = [w = editor->width(), h](const QImage& a, const QImage& b) {
        int n = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (a.pixelColor(x, y) != b.pixelColor(x, y))
                    ++n;
        return n;
    };

    editor->setLongLineRulerColumn(10);
    QCOMPARE(editor->longLineRulerColumn(), 10);
    const QImage lit = editor->grab().toImage();
    const int want = x0 + qRound(10 * cw);
    QVERIFY(diffAt(base, lit, want) > 5); // lit column exists
    for (int d = 1; d <= 3; ++d) {        // no AA bleed into neighbours
        QCOMPARE(diffAt(base, lit, want - d), 0);
        QCOMPARE(diffAt(base, lit, want + d), 0);
    }
    QCOMPARE(totalDiff(base, lit), diffAt(base, lit, want)); // exactly one column

    // Off-screen far right: clipped away, frame stays pixel-identical.
    editor->setLongLineRulerColumn(100000);
    QCOMPARE(editor->longLineRulerColumn(), 100000); // setter does not clamp up
    QCOMPARE(totalDiff(base, editor->grab().toImage()), 0);

    // Rides horizontal scroll: the guide shifts left by exactly the offset.
    editor->setLongLineRulerColumn(0);
    editor->setPlainText(QString(200, QLatin1Char(' '))); // wide, all blank
    editor->setPosition({0, 0});
    editor->setHorizontalScrollOffset(37);
    QCOMPARE(editor->horizontalScrollOffset(), 37);
    const QImage baseH = editor->grab().toImage();
    editor->setLongLineRulerColumn(20);
    const QImage litH = editor->grab().toImage();
    const int wantH = x0 + qRound(20 * cw) - 37;
    QVERIFY(diffAt(baseH, litH, wantH) > 5);
    QCOMPARE(diffAt(baseH, litH, x0 + qRound(20 * cw)), 0); // unscrolled x is dead
    editor->setHorizontalScrollOffset(0);
    editor->setLongLineRulerColumn(0);
}

void TestCodeEditor::currentLineModeWashFrameAndDelegates()
{
    // The M18 enum replaces the old bool; the default is the ON wash.
    // Pixel-differential checks: the wash covers exactly one line band in
    // exact Text-ink@30-over-Base blend; the Frame mode adds exactly the
    // band outline in Text@90-over-wash. Wash on, caret line empty -> the
    // band interior is pure blend (the caret block cancels between grabs).
    QCOMPARE(editor->currentLineMode(), CodeEditor::CurrentLineMode::Background);
    QVERIFY(editor->currentLineHighlight()); // the bool delegate reads ON

    editor->setPlainText(QStringLiteral("abc\n"));
    editor->setPosition({1, 0});
    const int pad = editor->padding();
    const int lh = editor->lineHeight();
    const int bandTop = pad + lh; // caret line 1, no v-scroll, no folds
    const int xL = editor->contentLeft();
    const int xR = editor->width() - editor->overviewRulerWidth() - 3;
    const int h = editor->height();
    const QColor baseC = editor->palette().color(QPalette::Base);
    const QColor ink = editor->palette().color(QPalette::Text);
    auto blendOver = [&](const QColor& under, const QColor& over, int a) {
        const qreal f = a / 255.0;
        return QColor(qRound(under.red() + f * (over.red() - under.red())),
                      qRound(under.green() + f * (over.green() - under.green())),
                      qRound(under.blue() + f * (over.blue() - under.blue())));
    };

    editor->setCurrentLineMode(CodeEditor::CurrentLineMode::Off);
    const QImage off = editor->grab().toImage();
    editor->setCurrentLineMode(CodeEditor::CurrentLineMode::Background);
    const QImage bg = editor->grab().toImage();

    const QColor wash = blendOver(baseC, ink, 30);
    auto washLike = [&](const QColor& c) {
        return qAbs(c.red() - wash.red()) <= 4 && qAbs(c.green() - wash.green()) <= 4
            && qAbs(c.blue() - wash.blue()) <= 4;
    };
    int changed = 0, inBand = 0, exact = 0, outside = 0;
    for (int y = 0; y < h; ++y)
        for (int x = xL; x < xR; ++x) {
            if (off.pixelColor(x, y) == bg.pixelColor(x, y))
                continue;
            ++changed;
            if (y >= bandTop && y < bandTop + lh) {
                ++inBand;
                if (washLike(bg.pixelColor(x, y)))
                    ++exact;
            } else {
                ++outside;
            }
        }
    QCOMPARE(outside, 0);   // the wash paints nothing outside its band
    QVERIFY(inBand > 30);   // …and plenty of it (minus caret-column cancels)
    QCOMPARE(exact, inBand); // every wash pixel is the exact Text@30 blend

    // BackgroundFrame = Background + the band outline. Measured rows of
    // Qt's cosmetic 1px outline of band.adjusted(0,0,-1,-1): bandTop and
    // bandTop+lh-1; the vertical edges sit at widget x 0 / width-2, both
    // outside the scanned window.
    editor->setCurrentLineMode(CodeEditor::CurrentLineMode::BackgroundFrame);
    const QImage fr = editor->grab().toImage();
    const QColor frameInk = blendOver(wash, ink, 90);
    auto frameLike = [&](const QColor& c) {
        return qAbs(c.red() - frameInk.red()) <= 4 && qAbs(c.green() - frameInk.green()) <= 4
            && qAbs(c.blue() - frameInk.blue()) <= 4;
    };
    int frameChanged = 0, frameExact = 0, frameRows[2] = {0, 0}, strays = 0;
    for (int y = 0; y < h; ++y)
        for (int x = xL + 6; x < xR; ++x) { // xL+6: skip the caret block columns
            if (bg.pixelColor(x, y) == fr.pixelColor(x, y))
                continue;
            ++frameChanged;
            if (y == bandTop)
                ++frameRows[0];
            else if (y == bandTop + lh - 1)
                ++frameRows[1];
            else
                ++strays;
            if (frameLike(fr.pixelColor(x, y)))
                ++frameExact;
        }
    QCOMPARE(strays, 0);       // outline rows only — no interior repaint
    QVERIFY(frameRows[0] > 30 && frameRows[1] > 30);
    QCOMPARE(frameChanged, frameRows[0] + frameRows[1]);
    QCOMPARE(frameExact, frameChanged);

    // Bool delegates round-trip over the enum in both directions.
    editor->setCurrentLineHighlight(false);
    QCOMPARE(editor->currentLineMode(), CodeEditor::CurrentLineMode::Off);
    QVERIFY(!editor->currentLineHighlight());
    editor->setCurrentLineHighlight(true);
    QCOMPARE(editor->currentLineMode(), CodeEditor::CurrentLineMode::Background);
    QVERIFY(editor->currentLineHighlight());
    editor->setCurrentLineMode(CodeEditor::CurrentLineMode::BackgroundFrame);
    QVERIFY(editor->currentLineHighlight());
    editor->setCurrentLineMode(CodeEditor::CurrentLineMode::Off);
    QVERIFY(!editor->currentLineHighlight());
}

// --- M19 settings object ----------------------------------------------------

void TestCodeEditor::optionsMirrorsKnobsAndNotifies()
{
    // ONE stable, parented child; two doors (this object OR the flat
    // CodeEditor setters) into the SAME room. The editor keeps the state,
    // so options getters can never drift stale.
    CodeEditorOptions* opt = editor->options();
    QVERIFY(opt);
    QCOMPARE(editor->options(), opt); // stable instance
    QCOMPARE(opt->parent(), static_cast<QObject*>(editor)); // owned child

    // Getters mirror the live editor state without setting anything.
    QCOMPARE(opt->lineNumbersMode(), editor->lineNumbersMode());
    QCOMPARE(opt->currentLineMode(), editor->currentLineMode());
    QCOMPARE(opt->currentLineHighlight(), editor->currentLineHighlight());
    QCOMPARE(opt->longLineRulerColumn(), editor->longLineRulerColumn());
    QCOMPARE(opt->tabStopWidth(), editor->tabStopWidth());
    QCOMPARE(opt->autoIndentEnabled(), editor->autoIndentEnabled());
    QCOMPARE(opt->tabChangesFocus(), editor->tabChangesFocus());
    QCOMPARE(opt->overviewRulerEnabled(), editor->overviewRulerEnabled());
    QCOMPARE(opt->scrollBeyondLastLineEnabled(),
             editor->scrollBeyondLastLineEnabled());
    QVERIFY(opt->autoCloseEnabled()); // M19-only knobs: default ON…
    QVERIFY(!opt->isReadOnly());      // …and OFF

    // Change through options -> editor sees it, ONE signal with the new
    // value; same-value sets are silent; the flat door fires the SAME spy.
    QSignalSpy lnSpy(opt, &CodeEditorOptions::lineNumbersModeChanged);
    const auto lnBase = opt->lineNumbersMode();
    const auto lnOther = lnBase == CodeEditor::LineNumbersMode::On
        ? CodeEditor::LineNumbersMode::Off : CodeEditor::LineNumbersMode::On;
    opt->setLineNumbersMode(lnOther);
    QCOMPARE(editor->lineNumbersMode(), lnOther);
    QCOMPARE(lnSpy.count(), 1);
    QCOMPARE(lnSpy.at(0).at(0).value<CodeEditor::LineNumbersMode>(), lnOther);
    opt->setLineNumbersMode(lnOther); // same value: no re-emit
    QCOMPARE(lnSpy.count(), 1);
    editor->setLineNumbersMode(lnBase); // the flat setter rides this spy too
    QCOMPARE(opt->lineNumbersMode(), lnBase);
    QCOMPARE(lnSpy.count(), 2);

    // Pin the base, then walk the enum through every door.
    editor->setCurrentLineMode(CodeEditor::CurrentLineMode::Background);
    QSignalSpy clSpy(opt, &CodeEditorOptions::currentLineModeChanged);
    opt->setCurrentLineMode(CodeEditor::CurrentLineMode::BackgroundFrame);
    QCOMPARE(editor->currentLineMode(), CodeEditor::CurrentLineMode::BackgroundFrame);
    QCOMPARE(clSpy.count(), 1);
    opt->setCurrentLineHighlight(false); // the bool delegate rides the enum:
    QCOMPARE(editor->currentLineMode(), CodeEditor::CurrentLineMode::Off); // exactly ONE emit
    QCOMPARE(clSpy.count(), 2);
    QVERIFY(!opt->currentLineHighlight());
    opt->setCurrentLineHighlight(true);
    QCOMPARE(opt->currentLineMode(), CodeEditor::CurrentLineMode::Background);
    QCOMPARE(clSpy.count(), 3);

    QSignalSpy rulerSpy(opt, &CodeEditorOptions::longLineRulerColumnChanged);
    opt->setLongLineRulerColumn(40);
    QCOMPARE(editor->longLineRulerColumn(), 40);
    QCOMPARE(rulerSpy.count(), 1);
    opt->setLongLineRulerColumn(-3); // the editor's clamp lives in the
    QCOMPARE(opt->longLineRulerColumn(), 0); // setter, so options inherit it…
    QCOMPARE(rulerSpy.at(1).at(0).toInt(), 0); // …and notifies with the CLAMPED value
    QCOMPARE(rulerSpy.count(), 2);
    opt->setLongLineRulerColumn(0); // same (clamped) value: silent
    QCOMPARE(rulerSpy.count(), 2);

    QSignalSpy tabSpy(opt, &CodeEditorOptions::tabStopWidthChanged);
    opt->setTabStopWidth(10);
    QCOMPARE(editor->tabStopWidth(), 10);
    QCOMPARE(tabSpy.at(0).at(0).toInt(), 10);
    QCOMPARE(tabSpy.count(), 1);
    opt->setTabStopWidth(0); // >=1 clamp, reported as 1
    QCOMPARE(opt->tabStopWidth(), 1);
    QCOMPARE(tabSpy.count(), 2);
    QCOMPARE(tabSpy.at(1).at(0).toInt(), 1);

    // The four plain-bool mirrors, one door each (invert whatever the
    // current state is — the test must not bake in the shipped defaults).
    QSignalSpy aiSpy(opt, &CodeEditorOptions::autoIndentEnabledChanged);
    editor->setAutoIndentEnabled(!opt->autoIndentEnabled());
    QCOMPARE(opt->autoIndentEnabled(), editor->autoIndentEnabled());
    QCOMPARE(aiSpy.count(), 1);
    QSignalSpy tfSpy(opt, &CodeEditorOptions::tabChangesFocusChanged);
    const bool tfWant = !editor->tabChangesFocus();
    opt->setTabChangesFocus(tfWant);
    QCOMPARE(editor->tabChangesFocus(), tfWant);
    QCOMPARE(tfSpy.count(), 1);
    QSignalSpy orSpy(opt, &CodeEditorOptions::overviewRulerEnabledChanged);
    const bool orWant = !editor->overviewRulerEnabled();
    opt->setOverviewRulerEnabled(orWant);
    QCOMPARE(editor->overviewRulerEnabled(), orWant);
    QCOMPARE(orSpy.count(), 1);
    QSignalSpy sbSpy(opt, &CodeEditorOptions::scrollBeyondLastLineEnabledChanged);
    editor->setScrollBeyondLastLineEnabled(!opt->scrollBeyondLastLineEnabled());
    QCOMPARE(opt->scrollBeyondLastLineEnabled(), editor->scrollBeyondLastLineEnabled());
    QCOMPARE(sbSpy.count(), 1);
    editor->setScrollBeyondLastLineEnabled(!opt->scrollBeyondLastLineEnabled());
    QCOMPARE(sbSpy.count(), 2);
    opt->setScrollBeyondLastLineEnabled(opt->scrollBeyondLastLineEnabled()); // same: silent
    QCOMPARE(sbSpy.count(), 2);

    // The two options-only knobs also notify; the behavioral half of the
    // contract is exercised by autoCloseToggleOnOptions/readOnlyBlocksUserEdits.
    QSignalSpy acSpy(opt, &CodeEditorOptions::autoCloseEnabledChanged);
    QSignalSpy roSpy(opt, &CodeEditorOptions::readOnlyChanged);
    opt->setAutoCloseEnabled(false);
    opt->setReadOnly(true);
    QCOMPARE(acSpy.count(), 1);
    QVERIFY(!acSpy.at(0).at(0).toBool());
    QCOMPARE(roSpy.count(), 1);
    QVERIFY(roSpy.at(0).at(0).toBool());
    opt->setReadOnly(true); // same: silent
    QCOMPARE(roSpy.count(), 1);
    opt->setAutoCloseEnabled(true); // leave the room as we found it
    opt->setReadOnly(false);
    QCOMPARE(acSpy.count(), 2);
    QCOMPARE(roSpy.count(), 2);
}

// The auto-close toggle is M19-options-only (no flat setter on purpose):
// OFF must degrade EVERY pairing behavior (Pair/Wrap/Skip) to plain
// single-char typing, while the textual "backspace between an empty pair
// removes both" rule stays alive (it never claims pairing, it just reads
// the text — documented contract in CodeEditorOptions.hpp).
void TestCodeEditor::autoCloseToggleOnOptions()
{
    CodeEditorOptions* opt = editor->options();
    QVERIFY(opt->autoCloseEnabled()); // default ON = the M18 behavior
    QSignalSpy spy(opt, &CodeEditorOptions::autoCloseEnabledChanged);

    opt->setAutoCloseEnabled(false);
    QVERIFY(!opt->autoCloseEnabled());
    QCOMPARE(spy.count(), 1);
    opt->setAutoCloseEnabled(false); // same value: silent
    QCOMPARE(spy.count(), 1);

    // OFF: the opener is one plain char — no pair, no wrap, no skip.
    editor->setPlainText(QString());
    QTest::keyClick(editor, Qt::Key_ParenLeft);
    QCOMPARE(editor->toPlainText(), QStringLiteral("("));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));
    QTest::keyClick(editor, Qt::Key_QuoteDbl); // quote: same, plain
    QCOMPARE(editor->toPlainText(), QStringLiteral("(\""));
    QTest::keyClick(editor, Qt::Key_ParenRight); // the closer would have
    QCOMPARE(editor->toPlainText(), QStringLiteral("(\")")); // inserted plain anyway

    // Wrap degrades to a plain selection-replace with the single char.
    editor->setPlainText(QStringLiteral("hello"));
    editor->selectAll();
    QTest::keyClick(editor, Qt::Key_ParenLeft);
    QCOMPARE(editor->toPlainText(), QStringLiteral("("));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));

    // The textual backspace rule is NOT gated: two plain inserts spelling
    // "()" still die as a pair between the empty pair.
    editor->setPlainText(QString());
    QTest::keyClick(editor, Qt::Key_ParenLeft);
    QTest::keyClick(editor, Qt::Key_ParenRight);
    QCOMPARE(editor->toPlainText(), QStringLiteral("()")); // two real inserts
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));
    editor->setPosition({0, 1});
    QTest::keyClick(editor, Qt::Key_Backspace);
    QCOMPARE(editor->toPlainText(), QString());
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));

    // Back ON: every M18 behavior resumes.
    opt->setAutoCloseEnabled(true);
    QCOMPARE(spy.count(), 2);
    editor->setPlainText(QString());
    QTest::keyClick(editor, Qt::Key_ParenLeft);
    QCOMPARE(editor->toPlainText(), QStringLiteral("()")); // Pair again
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 1}));
    QTest::keyClick(editor, Qt::Key_ParenRight); // Skip again: motion only
    QCOMPARE(editor->toPlainText(), QStringLiteral("()"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 2}));
}

// Read-only mode (options-only knob): EVERY user edit gesture is refused;
// caret motion, selection, copy, find, and programmatic setPlainText stay
// fully alive. Clipboard content is deliberately NOT asserted (offscreen
// clipboard flakiness) — only the model state matters.
void TestCodeEditor::readOnlyBlocksUserEdits()
{
    CodeEditorOptions* opt = editor->options();
    QVERIFY(!opt->isReadOnly()); // default OFF
    QSignalSpy spy(opt, &CodeEditorOptions::readOnlyChanged);

    // Seed an undoable edit BEFORE the lock: if any gate leaked, the
    // frozen-text comparisons below would catch the mutation.
    editor->setPlainText(QStringLiteral("hello world"));
    editor->setPosition({0, 5});
    QTest::keyClick(editor, Qt::Key_X); // allowed while unlocked
    QVERIFY(editor->canUndo());

    opt->setReadOnly(true);
    QVERIFY(opt->isReadOnly());
    QCOMPARE(spy.count(), 1);
    opt->setReadOnly(true); // same value: silent
    QCOMPARE(spy.count(), 1);
    const QString frozen = editor->toPlainText(); // "hellox world"
    QCOMPARE(frozen, QStringLiteral("hellox world"));

    // Refused gestures — typing, IME commit, deletes (incl. the Ctrl word
    // variant), Enter, Tab/Shift+Tab indentation, auto-close, replace
    // actions, undo/redo, cut/paste, DnD drop.
    QTest::keyClick(editor, Qt::Key_Y);
    QTest::keyClick(editor, Qt::Key_Backspace);
    QTest::keyClick(editor, Qt::Key_Delete);
    QTest::keyClick(editor, Qt::Key_Delete, Qt::ControlModifier);
    QTest::keyClick(editor, Qt::Key_Return);
    QTest::keyClick(editor, Qt::Key_Tab);
    QTest::keyClick(editor, Qt::Key_Backtab);
    QTest::keyClick(editor, Qt::Key_ParenLeft); // auto-close stays out too…
    QTest::keyClick(editor, Qt::Key_Apostrophe); // …and no skip/pair motion
    QInputMethodEvent ime(QStringLiteral("\u4F60"), // IME commit = typing
                          {});
    QApplication::sendEvent(editor, &ime);
    // The refuses are inert: not one of them even moved the caret.
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 6}));
    QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier); // selection OK…
    QVERIFY(editor->hasSelection()); // …selection gestures stay alive…
    QTest::keyClick(editor, Qt::Key_Q); // …but type-over-selection is refused
    editor->undo();
    editor->redo();
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier); // chord too
    editor->cut();
    editor->paste();
    QMimeData md;
    md.setText(QStringLiteral("DROPPED"));
    QDragEnterEvent de(editor->rect().center(), Qt::CopyAction, &md,
                       Qt::LeftButton, Qt::NoModifier); // (deprecated QPoint
    QApplication::sendEvent(editor, &de);               // ctor: the in-repo
    QVERIFY(!de.isAccepted()); // helper spells it the same way)
    // readOnly refuses at the drag-enter door — the real protocol never
    // reaches dragMove/drop. (The dropEvent guard is the belt to this
    // braces; sendDropAt's QTEST_ASSERT would fire precisely because the
    // Drop event stays ignored — that refusal is the assertion here.)
    QCOMPARE(editor->toPlainText(), frozen); // NONE of it touched the model
    QVERIFY(editor->canUndo()); // the pre-lock edit is still pending

    // Find stays fully alive; only the replace ACTIONS refuse.
    editor->setFindText(QStringLiteral("o"));
    QCOMPARE(editor->matchCount(), 2);
    // (replaceRange is the private replace-bar internal; the public door
    // is replaceAll — refused, returns 0 matches acted on.)
    QCOMPARE(editor->replaceAll(QStringLiteral("0")), 0);
    QCOMPARE(editor->toPlainText(), frozen);

    // The programmatic door and the read side of the widget are untouched.
    editor->setPlainText(QStringLiteral("replaced")); // allowed by contract
    QCOMPARE(editor->toPlainText(), QStringLiteral("replaced"));
    editor->setPosition({0, 3});
    QTest::keyClick(editor, Qt::Key_Right);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 4}));
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier);
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(), QStringLiteral("aced")); // copy side alive
    editor->setPosition({0, 8});

    // Unlock: edits resume exactly where they stopped.
    opt->setReadOnly(false);
    QCOMPARE(spy.count(), 2);
    QTest::keyClick(editor, Qt::Key_Backspace);
    QCOMPARE(editor->toPlainText(), QStringLiteral("replace"));
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), QStringLiteral("replaced"));
}

// --- M11 accessibility bridge (QAccessible) -------------------------------

void TestCodeEditor::accessibleInterfaceMirrorsRoleStateAndText()
{
    editor->setPlainText(QStringLiteral("alpha\nbeta"));

    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(editor);
    QVERIFY(iface); // the CodeEditor ctor installed the factory: no app wiring
    QVERIFY(iface->role() == QAccessible::EditableText);
    QVERIFY(iface->textInterface());
    QVERIFY(iface->editableTextInterface());

    // Factory + cache: ONE interface object per editor (the second query
    // hits QAccessible's cache, it is not a fresh allocation).
    QCOMPARE(QAccessible::queryAccessibleInterface(editor), iface);

    // Name rides the widget property set in the ctor; Value is the whole
    // document (Qt's multiline-editor convention, the QAccessibleTextEdit
    // model).
    QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("code editor"));
    QCOMPARE(iface->text(QAccessible::Value), editor->toPlainText());

    // The state flags that make a screen reader treat the widget as an
    // editable multiline text area.
    QAccessible::State st = iface->state();
    QVERIFY(st.selectableText);
    QVERIFY(st.multiLine);
    QVERIFY(st.editable);
    QVERIFY(!st.readOnly);

    // The M19 lock reaches the state through the options door…
    editor->options()->setReadOnly(true);
    st = iface->state();
    QVERIFY(st.readOnly);
    QVERIFY(!st.editable);

    // …while the Value WRITE door (the programmatic setPlainText contract
    // the M19 suite froze) survives the lock.
    iface->setText(QAccessible::Value, QStringLiteral("gamma"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("gamma"));
    editor->options()->setReadOnly(false);
}

void TestCodeEditor::accessibleTextOffsetsMapDocumentAndDriveCaret()
{
    const QString doc = QStringLiteral("abc\nde\nfghi"); // 11 units, LFs at 3, 6
    editor->setPlainText(doc);
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(editor);
    QAccessibleTextInterface* ti = iface->textInterface();
    QAccessibleEditableTextInterface* et = iface->editableTextInterface();
    QVERIFY(ti);
    QVERIFY(et);

    QCOMPARE(ti->characterCount(), 11);
    // The read door IS TextBuffer::textRange verbatim (document frame with
    // the '\n' separators; core clamps both ends).
    QCOMPARE(ti->text(1, 4), QStringLiteral("bc\n"));
    QCOMPARE(ti->text(-5, 100), doc);

    // Caret: offsets resolve through lineColAt — offset 3 sits ON the '\n'
    // after "abc", and the buffer maps that position to {0, 3} (BEFORE the
    // separator, the core convention surfacing unchanged here); offset 4 is
    // then line 1's start.
    ti->setCursorPosition(7);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 0}));
    QCOMPARE(ti->cursorPosition(), 7);
    ti->setCursorPosition(3);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));
    ti->setCursorPosition(4);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));
    ti->setCursorPosition(999); // clamped to the document end
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 4}));
    ti->setCursorPosition(-7); // and to the beginning (no -1 magic here:
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0})); // that
    // -1/-2 convention belongs to the boundary queries, test 3 proves it.

    int a = -9, b = -9;
    ti->setSelection(0, 1, 4);
    QCOMPARE(ti->selectionCount(), 1); // Gate-1: the ONE primary selection
    ti->selection(0, &a, &b);
    QCOMPARE(a, 1);
    QCOMPARE(b, 4);
    QCOMPARE(editor->selectedText(), QStringLiteral("bc\n"));
    QCOMPARE(ti->cursorPosition(), 4); // endOffset is the ACTIVE end
    ti->selection(1, &a, &b); // index 1 does not exist (extras are AT-invisible)
    QCOMPARE(a, -1);
    QCOMPARE(b, -1);

    ti->setSelection(0, 9, 2); // reversed ends sort; caret rides endOffset
    ti->selection(0, &a, &b);
    QCOMPARE(a, 2);
    QCOMPARE(b, 9);
    QCOMPARE(ti->cursorPosition(), 9);

    ti->addSelection(6, 8); // add == set(0,…) here (no per-slot AT semantics)
    ti->selection(0, &a, &b);
    QCOMPARE(a, 6);
    QCOMPARE(b, 8);
    ti->removeSelection(0);
    QCOMPARE(ti->selectionCount(), 0);
    QVERIFY(!editor->hasSelection());
    ti->setSelection(3, 0, 4); // idx != 0: ignored, never a second selection
    QCOMPARE(ti->selectionCount(), 0);

    // Editable doors route through the editor's caret doors: the caret
    // travels to every edit, undo stays intact, cross-line works.
    ti->setCursorPosition(0);
    et->insertText(0, QStringLiteral("X"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("Xabc\nde\nfghi"));
    QCOMPARE(ti->cursorPosition(), 1);
    et->replaceText(2, 5, QStringLiteral("Y")); // "bc\n" -> "Y" (spans a line)
    QCOMPARE(editor->toPlainText(), QStringLiteral("XaYde\nfghi"));
    et->deleteText(1, 3); // "aY"
    QCOMPARE(editor->toPlainText(), QStringLiteral("Xde\nfghi"));
    QVERIFY(editor->canUndo()); // rode the editor's own undo doors

    // readOnly turns every editable door into a no-op (the M19 user-edit
    // contract)…
    editor->options()->setReadOnly(true);
    et->insertText(0, QStringLiteral("NO"));
    et->deleteText(0, 1);
    et->replaceText(0, 1, QStringLiteral("Z"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("Xde\nfghi"));
    // …while caret/selection stay alive (navigation is not an edit).
    ti->setSelection(0, 0, 2);
    QCOMPARE(ti->selectionCount(), 1);
    ti->setCursorPosition(0);
    QCOMPARE(ti->cursorPosition(), 0);
    editor->options()->setReadOnly(false);

    // Read-side freshness: the interface caches nothing — plain typing is
    // immediately visible in every query.
    ti->setCursorPosition(0);
    const int before = ti->characterCount();
    QTest::keyClick(editor, Qt::Key_Z);
    QCOMPARE(ti->characterCount(), before + 1);
    QCOMPARE(ti->text(ti->cursorPosition() - 1, ti->cursorPosition()),
             QStringLiteral("z")); // (keyClick lands lowercase)
}

void TestCodeEditor::accessibleBoundariesFollowTheLineModel()
{
    editor->setPlainText(QStringLiteral("hello world\nsecond line")); // 23 units
    QAccessibleTextInterface* ti =
        QAccessible::queryAccessibleInterface(editor)->textInterface();
    QCOMPARE(ti->characterCount(), 23);
    editor->setPosition({1, 3}); // offset 15 is the cursor for the -2 probes

    enum Which { Before, After, At };
    auto call = [ti](Which w, int off, QAccessible::TextBoundaryType bt,
                     int* s, int* e) -> QString {
        switch (w) {
        case Before: return ti->textBeforeOffset(off, bt, s, e);
        case After:  return ti->textAfterOffset(off, bt, s, e);
        case At:     return ti->textAtOffset(off, bt, s, e);
        }
        return QString();
    };
    auto item = [call](Which w, int off, QAccessible::TextBoundaryType bt,
                       const QString& want, int ws, int we) {
        int s = -9, e = -9;
        QCOMPARE(call(w, off, bt, &s, &e), want);
        QCOMPARE(s, ws);
        QCOMPARE(e, we);
    };
    auto fail = [call](Which w, int off, QAccessible::TextBoundaryType bt) {
        int s = 9, e = 9;
        QVERIFY(call(w, off, bt, &s, &e).isEmpty());
        QCOMPARE(s, -1); // Qt's failure frame
        QCOMPARE(e, -1);
    };

    // Char: native, single code units. -1 == the document end convention.
    item(Before, 1, QAccessible::CharBoundary, QStringLiteral("h"), 0, 1);
    fail(Before, 0, QAccessible::CharBoundary);
    item(Before, -1, QAccessible::CharBoundary, QStringLiteral("e"), 22, 23);
    item(After, 22, QAccessible::CharBoundary, QStringLiteral("e"), 22, 23);
    fail(After, 23, QAccessible::CharBoundary);
    item(At, 0, QAccessible::CharBoundary, QStringLiteral("h"), 0, 1);
    item(At, 22, QAccessible::CharBoundary, QStringLiteral("e"), 22, 23);
    fail(At, 23, QAccessible::CharBoundary); // past the last char: none

    // Word: LINE-LOCAL runs (letter/number + '_'): before mid-word is the
    // partial span ending AT the offset, before at a gap jumps to the whole
    // previous word, at = the word straddling the offset.
    item(Before, 4, QAccessible::WordBoundary, QStringLiteral("hell"), 0, 4);
    item(Before, 5, QAccessible::WordBoundary, QStringLiteral("hello"), 0, 5);
    item(Before, 6, QAccessible::WordBoundary, QStringLiteral("hello"), 0, 5);
    item(Before, 22, QAccessible::WordBoundary, QStringLiteral("lin"), 19, 22);
    item(Before, -2, QAccessible::WordBoundary, QStringLiteral("sec"), 12, 15);
    fail(Before, 12, QAccessible::WordBoundary); // no word before line 1 start
    item(After, 0, QAccessible::WordBoundary, QStringLiteral("hello"), 0, 5);
    item(After, 4, QAccessible::WordBoundary, QStringLiteral("o"), 4, 5);
    item(After, 5, QAccessible::WordBoundary, QStringLiteral("world"), 6, 11);
    item(After, 18, QAccessible::WordBoundary, QStringLiteral("line"), 19, 23);
    fail(After, 11, QAccessible::WordBoundary); // a '\n' ends the line-local hunt
    item(At, 3, QAccessible::WordBoundary, QStringLiteral("hello"), 0, 5);
    item(At, 0, QAccessible::WordBoundary, QStringLiteral("hello"), 0, 5);
    item(At, 11, QAccessible::WordBoundary, QStringLiteral("world"), 6, 11); // the '\n' offset resolves to the line's last word
    item(At, 18, QAccessible::WordBoundary, QStringLiteral("second"), 12, 18);
    item(At, 22, QAccessible::WordBoundary, QStringLiteral("line"), 19, 23);

    // Line: EXACTLY the Qt default frame, spans include the trailing '\n':
    // before = the line terminated by the LAST '\n' strictly before the
    // offset (so offset ON the '\n' fails for line 0, and offsets anywhere
    // inside/after line 0's terminator yield line 0 — first-line fail);
    // after skips to the NEXT line; at = the line containing the offset.
    fail(Before, 5, QAccessible::LineBoundary); // nothing before line 0
    fail(Before, 11, QAccessible::LineBoundary); // offset ON the '\n': no LF yet
    item(Before, 12, QAccessible::LineBoundary, QStringLiteral("hello world\n"), 0, 12);
    item(Before, 15, QAccessible::LineBoundary, QStringLiteral("hello world\n"), 0, 12);
    item(Before, -1, QAccessible::LineBoundary, QStringLiteral("hello world\n"), 0, 12);
    item(After, 5, QAccessible::LineBoundary, QStringLiteral("second line"), 12, 23);
    item(After, 11, QAccessible::LineBoundary, QStringLiteral("second line"), 12, 23);
    fail(After, 12, QAccessible::LineBoundary); // nothing after the last line
    fail(After, 22, QAccessible::LineBoundary);
    item(At, 5, QAccessible::LineBoundary, QStringLiteral("hello world\n"), 0, 12);
    item(At, 11, QAccessible::LineBoundary, QStringLiteral("hello world\n"), 0, 12);
    item(At, 12, QAccessible::LineBoundary, QStringLiteral("second line"), 12, 23);
    // Paragraph == Line (one paragraph per line).
    item(At, 5, QAccessible::ParagraphBoundary, QStringLiteral("hello world\n"), 0, 12);
    item(Before, 15, QAccessible::ParagraphBoundary, QStringLiteral("hello world\n"), 0, 12);
    item(After, 5, QAccessible::ParagraphBoundary, QStringLiteral("second line"), 12, 23);

    // NoBoundary: the whole prefix / suffix / document.
    item(Before, 5, QAccessible::NoBoundary, QStringLiteral("hello"), 0, 5);
    item(After, 12, QAccessible::NoBoundary, QStringLiteral("second line"), 12, 23);
    item(At, 7, QAccessible::NoBoundary, QStringLiteral("hello world\nsecond line"), 0, 23);

    // attributes: empty role map (Gate-1), span = the line sans its '\n'.
    int as = -9, ae = -9;
    QVERIFY(ti->attributes(15, &as, &ae).isEmpty());
    QCOMPARE(as, 12);
    QCOMPARE(ae, 23);
}

void TestCodeEditor::accessibleCharRectAndPointHitRoundTrip()
{
    editor->setPlainText(QStringLiteral("0123456789\nabcdefghij"));
    QAccessibleTextInterface* ti =
        QAccessible::queryAccessibleInterface(editor)->textInterface();

    const int lh = editor->lineHeight();
    const double cw = editor->cellWidth();
    // Offset 12 == line 1, col 1 ('b'): the rect must sit EXACTLY where the
    // renderer puts that cell — the M13 slot map for y, xForCol for x —
    // in the GLOBAL frame rect() uses.
    const QPoint wantTopLeft = editor->mapToGlobal(
        QPoint(int(std::floor(editor->xForCol(1, 1))),
               editor->padding() + 1 * lh - editor->verticalScrollOffset()));
    const QRect r = ti->characterRect(12);
    QCOMPARE(r.topLeft(), wantTopLeft);
    QCOMPARE(r.width(), qMax(1, int(std::ceil(cw)))); // EOL/'\n' cells stay 1px
    QCOMPARE(r.height(), lh);

    // offsetAtPoint is the inverse (mapFromGlobal + positionAt): a point
    // just inside the cell maps back to the same offset…
    QCOMPARE(ti->offsetAtPoint(r.topLeft() + QPoint(int(0.3 * cw), lh / 2)), 12);
    // …and a probe left of the text (gutter) clamps to the line start,
    // exactly like a click does.
    QCOMPARE(ti->offsetAtPoint(editor->mapToGlobal(
                 QPoint(2, editor->padding() + lh + lh / 2))), 12 - 1);

    // scrollToSubstring over a far-below substring: the ensureCaretVisible
    // math WITHOUT a caret move — minimal flush, the range's bottom edge
    // flush against the viewport bottom (minus padding).
    QStringList lines;
    for (int i = 0; i < 100; ++i)
        lines << (QStringLiteral("line") + QString::number(i));
    const QString big = lines.join(QLatin1Char('\n'));
    editor->setPlainText(big); // scroll + caret reset with the document
    const qmn::view::TextPosition caretBefore = editor->position();
    const int at = big.indexOf(QStringLiteral("line50"));
    QVERIFY(at > 0);
    QCOMPARE(editor->verticalScrollOffset(), 0);
    ti->scrollToSubstring(at, at + 6);
    const int slot = editor->visibleIndexOfModelLine(50);
    QCOMPARE(slot, 50); // no folds: the M13 map is the identity
    const int vs = editor->verticalScrollOffset();
    QVERIFY(vs > 0);
    QCOMPARE(vs, (slot + 1) * lh + editor->padding() - editor->height());
    const int yTop = editor->padding() + slot * lh - vs;
    QVERIFY(yTop >= 0);
    QVERIFY(yTop + lh <= editor->height()); // fully visible, flush bottom
    QCOMPARE(editor->position(), caretBefore); // the caret NEVER moved
}

// --- Phase F parity doors (qtmonaco insert_text/delete_line) ---------------

void TestCodeEditor::parityInsertAndDeleteLineDoors()
{
    // insertTextAt mid-line: the caret lands just PAST the insert (Monaco's
    // executeEdits behavior, the contract behind qtmonaco's insert_text).
    editor->setPlainText(QStringLiteral("hello world"));
    editor->insertTextAt({0, 5}, QStringLiteral("!!"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("hello!! world"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 7}));

    // On another line: the caret travels there first.
    editor->setPlainText(QStringLiteral("aa\nbb"));
    editor->insertTextAt({1, 0}, QStringLiteral("x"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("aa\nxbb"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 1}));

    // Inserted text itself carries a '\n': the caret normalizes onto the
    // last inserted row (the insertTextAtCaret newline math rides along).
    editor->setPlainText(QStringLiteral("aa\nbb"));
    editor->insertTextAt({0, 2}, QStringLiteral("\n--")); // BEFORE the existing \n
    QCOMPARE(editor->toPlainText(), QStringLiteral("aa\n--\nbb"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 2}));

    // Empty document (lineCount() >= 1 always): an insert at {0,0} works.
    editor->setPlainText(QString());
    QCOMPARE(editor->lineCount(), 1);
    editor->insertTextAt({0, 0}, QStringLiteral("abc"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("abc"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 3}));

    // Out-of-range clamps FIRST (like setPosition), THEN the door inserts:
    // col 99 on a real line clamps to its EOL; line 9 clamps to the last.
    editor->setPlainText(QStringLiteral("one\ntwo\nthree"));
    editor->insertTextAt({2, 99}, QStringLiteral("!"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("one\ntwo\nthree!"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 6}));
    editor->insertTextAt({9, 9}, QStringLiteral("z"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("one\ntwo\nthree!z"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 7}));

    // Empty text is a no-op — and the caret stays put too (the door returns
    // before its setPosition).
    editor->insertTextAt({0, 0}, QString());
    QCOMPARE(editor->toPlainText(), QStringLiteral("one\ntwo\nthree!z"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{2, 7}));

    // The doors ride undo: a non-adjacent insert is its own group.
    editor->insertTextAt({0, 0}, QStringLiteral("Q"));
    QVERIFY(editor->canUndo());
    editor->undo();
    QCOMPARE(editor->toPlainText(), QStringLiteral("one\ntwo\nthree!z"));

    // deleteLineAt: a middle line goes with its '\n'.
    editor->setPlainText(QStringLiteral("a\nb\nc"));
    editor->deleteLineAt(1);
    QCOMPARE(editor->toPlainText(), QStringLiteral("a\nc"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0})); // parked

    // Last line: its separator goes too, leaving a real EMPTY last line.
    editor->setPlainText(QStringLiteral("a\nb"));
    editor->deleteLineAt(1);
    QCOMPARE(editor->toPlainText(), QStringLiteral("a\n"));
    QCOMPARE(editor->lineCount(), 2);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{1, 0}));

    // First line:
    editor->setPlainText(QStringLiteral("a\nb"));
    editor->deleteLineAt(0);
    QCOMPARE(editor->toPlainText(), QStringLiteral("b"));
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));

    // Solo line: the document ends up empty, lineCount() stays 1.
    editor->setPlainText(QStringLiteral("only"));
    editor->deleteLineAt(0);
    QCOMPARE(editor->toPlainText(), QString());
    QCOMPARE(editor->lineCount(), 1);
    QCOMPARE(editor->position(), (qmn::view::TextPosition{0, 0}));

    // Out of range: silent no-op, nothing recorded on the undo stack.
    editor->setPlainText(QStringLiteral("x"));
    QVERIFY(!editor->canUndo());
    editor->deleteLineAt(-1);
    editor->deleteLineAt(1);
    QCOMPARE(editor->toPlainText(), QStringLiteral("x"));
    QVERIFY(!editor->canUndo());

    // One delete_line call = one undo step.
    editor->setPlainText(QStringLiteral("a\nb\nc"));
    editor->deleteLineAt(1);
    QVERIFY(editor->canUndo());
    editor->undo();
    QCOMPARE(editor->toPlainText(), QStringLiteral("a\nb\nc"));

    // M19 readOnly locks the doors completely: no text change, no undo
    // entry (the ValueError lives in the Python shim, the C++ door is mute).
    editor->setPlainText(QStringLiteral("a\nb\nc"));
    const auto caretBefore = editor->position(); // {0,0} from setPlainText
    editor->options()->setReadOnly(true);
    editor->insertTextAt({0, 1}, QStringLiteral("X")); // caret must NOT travel
    editor->deleteLineAt(1);
    QCOMPARE(editor->toPlainText(), QStringLiteral("a\nb\nc"));
    QCOMPARE(editor->position(), caretBefore);
    QVERIFY(!editor->canUndo());
    editor->options()->setReadOnly(false);
}

void TestCodeEditor::parityDoorsAndEveryMutationPingTextChanged()
{
    QSignalSpy spy(editor, &CodeEditor::textChanged);
    QVERIFY(spy.isValid());

    // setPlainText is ONE primitive mutation -> exactly one ping…
    editor->setPlainText(QStringLiteral("abc"));
    QCOMPARE(spy.count(), 1);
    // …and re-setting the SAME value pings too: the core setText fires
    // whenever the old document was non-empty (oldChars > 0) — there is no
    // value-equality no-op at that level. Frozen frame, shim echo-guard
    // (last-value cache) lives in Python instead, like qtmonaco's.
    editor->setPlainText(QStringLiteral("abc"));
    QCOMPARE(spy.count(), 2);

    // Primitive user gestures: one ping each (caret sits at {0,0}).
    QTest::keyClick(editor, Qt::Key_X); // types lowercase 'x'
    QCOMPARE(spy.count(), 3);
    QTest::keyClick(editor, Qt::Key_Backspace);
    QCOMPARE(spy.count(), 4);

    // undo/redo replay the ops -> one ping per replayed op.
    editor->undo();
    QCOMPARE(spy.count(), 5);
    editor->redo();
    QCOMPARE(spy.count(), 6);

    // Caret motion / selection NEVER pings.
    int before = spy.count();
    editor->setPosition({0, 2});
    editor->selectAll();
    QTest::keyClick(editor, Qt::Key_Left);
    QCOMPARE(spy.count(), before);

    // The parity doors: exactly one ping per successful call.
    editor->setPlainText(QStringLiteral("a\nb\nc"));
    before = spy.count();
    editor->insertTextAt({1, 1}, QStringLiteral("Q"));
    QCOMPARE(spy.count(), before + 1);
    before = spy.count();
    editor->deleteLineAt(0);
    QCOMPARE(spy.count(), before + 1);
    // Refused door calls stay mute (no mutation, no ping).
    before = spy.count();
    editor->insertTextAt({0, 0}, QString());
    editor->deleteLineAt(99);
    QCOMPARE(spy.count(), before);

    // Consecutive typing pings per KEY PRESS (each press is its own
    // primitive mutation) — but tryCoalesce rewrites the chain into ONE
    // merged op, so one undo/redo call replays one op = one ping. Caret
    // sits at {0,0} on "ab": 'a' lands at 0, 'b' at 1 -> "abab".
    editor->setPlainText(QStringLiteral("ab"));
    before = spy.count();
    QTest::keyClick(editor, Qt::Key_A); // "aab"
    QTest::keyClick(editor, Qt::Key_B); // "abab" — coalesced chain
    QCOMPARE(spy.count(), before + 2);
    before = spy.count();
    editor->undo();
    QCOMPARE(spy.count(), before + 1);
    QCOMPARE(editor->toPlainText(), QStringLiteral("ab"));
    before = spy.count();
    editor->redo();
    QCOMPARE(spy.count(), before + 1);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abab"));

    // A genuinely MULTI-OP group: typing over a selection is a compound
    // delete+insert step under one bracket — undo STILL pings per replayed
    // primitive op, pinging per mutation, not per group.
    editor->selectAll(); // selects all of "abab"
    before = spy.count();
    QTest::keyClick(editor, Qt::Key_Q); // type-over-selection: 2 mutations
    QCOMPARE(spy.count(), before + 2);
    before = spy.count();
    editor->undo();
    QCOMPARE(spy.count(), before + 2);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abab"));

    // readOnly: the doors ping zero (M19), and undo is refused too.
    editor->options()->setReadOnly(true);
    before = spy.count();
    editor->insertTextAt({0, 0}, QStringLiteral("no"));
    editor->deleteLineAt(0);
    editor->undo();
    editor->redo();
    QCOMPARE(spy.count(), before);
    QCOMPARE(editor->toPlainText(), QStringLiteral("abab"));
    editor->options()->setReadOnly(false);
}

void TestCodeEditor::revealAtScrollsPerModeAndUnfoldsFolds()
{
    QStringList lines;
    for (int i = 0; i < 100; ++i)
        lines << (QStringLiteral("line") + QString::number(i));
    editor->setPlainText(lines.join(QLatin1Char('\n')));

    const int lh = editor->lineHeight();
    const int pad = editor->padding();
    const int h = editor->height();
    const auto caretHome = editor->position(); // {0,0}: setPlainText reset it
    QCOMPARE(editor->verticalScrollOffset(), 0);

    // Center: the line's cell middle at the viewport middle. No clamp here:
    // the doc has plenty of lines below and scroll-past-end is OFF.
    editor->revealAt({50, 2}, CodeEditor::RevealMode::Center);
    QCOMPARE(editor->verticalScrollOffset(), 50 * lh + lh / 2 + pad - h / 2);
    QCOMPARE(editor->position(), caretHome); // reveal-ONLY: caret never moves

    // NearTop on a line hidden BELOW: flush at the top edge.
    editor->revealAt({70, 2}, CodeEditor::RevealMode::NearTop);
    QCOMPARE(editor->verticalScrollOffset(), 70 * lh - pad);
    // NearTop on an ALREADY-VISIBLE line (72 sits inside the viewport that
    // just opened, lines ~71..94): still re-flushes to the top edge, exactly
    // like Monaco's revealPositionNearTop (unconditional; the conditional
    // variants carry ...IfOutsideViewport in their names). Tripwire.
    editor->revealAt({72, 2}, CodeEditor::RevealMode::NearTop);
    QCOMPARE(editor->verticalScrollOffset(), 72 * lh - pad);

    // Minimal on a line hidden ABOVE: travel to the top edge only.
    editor->revealAt({10, 2}, CodeEditor::RevealMode::Minimal);
    QCOMPARE(editor->verticalScrollOffset(), 10 * lh - pad);

    // Minimal on a line hidden BELOW: flush against the bottom edge.
    editor->revealAt({60, 2}, CodeEditor::RevealMode::Minimal);
    QCOMPARE(editor->verticalScrollOffset(), 61 * lh + pad - h);
    // Minimal on an already-visible line (50 sits inside the viewport that
    // flush just opened, lines ~38..61): unlike NearTop it does NOT re-scroll
    // — the conditional counterpart of the NearTop tripwire above.
    editor->revealAt({50, 2}, CodeEditor::RevealMode::Minimal);
    QCOMPARE(editor->verticalScrollOffset(), 61 * lh + pad - h);
    // Visible: Minimal is a no-op.
    editor->revealAt({60, 2}, CodeEditor::RevealMode::Minimal);
    QCOMPARE(editor->verticalScrollOffset(), 61 * lh + pad - h);

    // Positions clamp through clampPos FIRST: line 999 -> the last line,
    // and Minimal flushes its bottom edge (below maxScroll: no clamp).
    editor->revealAt({999, 99}, CodeEditor::RevealMode::Minimal);
    QCOMPARE(editor->verticalScrollOffset(), 100 * lh + pad - h);

    // setVScroll's edge clamp backstops the formulas: line 0 wants NEGATIVE
    // scroll for NearTop, and lands at 0 instead.
    editor->revealAt({0, 0}, CodeEditor::RevealMode::NearTop);
    QCOMPARE(editor->verticalScrollOffset(), 0);
    QCOMPARE(editor->position(), caretHome); // still untouched throughout

    // M13 reveal contract: the target of a reveal is never meant to be
    // hidden, so a collapsed fold containing it unfolds first. Rebuild the
    // doc with an indented block (grammar-less mode -> indent fallback).
    lines[40] = QStringLiteral("block:");
    for (int i = 41; i <= 45; ++i)
        lines[i] = QStringLiteral("  ") + lines[i]; // deeper than the header
    editor->setPlainText(lines.join(QLatin1Char('\n')));
    // Fold DISCOVERY (the foldables cache) is paint-fed, so probing via
    // isFoldable needs an event-loop pump; foldLine resolves the lazy
    // indent end itself — call the door directly.
    editor->revealAt({40, 0}, CodeEditor::RevealMode::NearTop);
    QCOMPARE(editor->verticalScrollOffset(), 40 * lh - pad);
    QVERIFY(editor->foldLine(40));
    QCOMPARE(editor->visibleIndexOfModelLine(43), -1); // hidden inside the fold
    editor->revealAt({43, 4}, CodeEditor::RevealMode::Center);
    QVERIFY(!editor->isFolded(40));
    QVERIFY(editor->visibleIndexOfModelLine(43) != -1);
    // With the map unfolded again, slot 43 is model 43; Center ALWAYS
    // scrolls, so the exact math also pins the auto-unfold having happened.
    QCOMPARE(editor->verticalScrollOffset(), 43 * lh + lh / 2 + pad - h / 2);
    QCOMPARE(editor->position(), caretHome);
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

    // Current-line band defaults ON since M18 (Background wash; the mode
    // enum replaced the old bool, whose setter/getter are delegates).
    QVERIFY(editor->currentLineHighlight());
    QCOMPARE(editor->currentLineMode() == CodeEditor::CurrentLineMode::Background, true);
    editor->setCurrentLineHighlight(false);
    QVERIFY(!editor->currentLineHighlight());
    editor->setCurrentLineMode(CodeEditor::CurrentLineMode::BackgroundFrame);
    QVERIFY(editor->currentLineHighlight());
    editor->setCurrentLineMode(CodeEditor::CurrentLineMode::Off);
    QVERIFY(!editor->currentLineHighlight());

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
    // M18: the selection-blend / caret-ink halves of this probe compare
    // against an exact Base backdrop; the current-line wash (ON by default
    // since M18) would blend under them, so pin the wash off.
    editor->setCurrentLineMode(CodeEditor::CurrentLineMode::Off);
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

// --- M13 folding (Phase E2) ------------------------------------------------
// Fixture for the view-side indent rule (no grammar loaded): line 0 heads a
// depth-2 indent block spanning lines 1..3; "next"/"last" sit back at col 0.
static QString foldDoc()
{
    return QStringLiteral("root\n  child1\n  child2\n  child3\nnext\nlast");
}

void TestCodeEditor::foldMapBasicsIndentMode()
{
    editor->setPlainText(foldDoc());
    editor->grab(); // paint pass warms the viewport foldable cache
    QVERIFY(editor->isFoldable(0));
    QVERIFY(!editor->isFoldable(1)); // child1/child2 share the indent
    QVERIFY(!editor->isFolded(0));
    const auto ranges = editor->foldableRanges();
    QCOMPARE(ranges.size(), 1);
    QCOMPARE(ranges[0].startLine, 0);
    QCOMPARE(ranges[0].endLine, 3); // the indent scan stops at "next"

    QVERIFY(editor->foldLine(0));
    QVERIFY(editor->isFolded(0));
    QCOMPARE(editor->lineCount(), 6); // the model never learns about folds
    QCOMPARE(editor->visibleLineTotal(), 3);
    // map lookups across the fold boundary
    QCOMPARE(editor->visibleIndexOfModelLine(0), 0);
    QCOMPARE(editor->visibleIndexOfModelLine(1), -1);
    QCOMPARE(editor->visibleIndexOfModelLine(3), -1);
    QCOMPARE(editor->visibleIndexOfModelLine(4), 1);
    QCOMPARE(editor->visibleIndexOfModelLine(5), 2);
    QCOMPARE(editor->visibleIndexOfModelLine(6), -1); // out of document
    QCOMPARE(editor->modelLineForVisibleIndex(0), 0);
    QCOMPARE(editor->modelLineForVisibleIndex(1), 4);
    QCOMPARE(editor->modelLineForVisibleIndex(2), 5);
    QCOMPARE(editor->modelLineForVisibleIndex(9), 5); // clamped both ways
    QCOMPARE(editor->modelLineForVisibleIndex(-2), 0);
    // positionAt rides the map: a click on the second row gets line 4, never
    // a hidden line (DoD: map lookups across fold boundaries).
    const int lh = editor->lineHeight();
    QCOMPARE(editor->positionAt(QPoint(editor->contentLeft() + 2,
                                       editor->padding() + lh + lh / 2)).line,
             4);

    QVERIFY(!editor->foldLine(1)); // not a fold start: no state change
    QCOMPARE(editor->visibleLineTotal(), 3);

    QVERIFY(editor->unfoldLine(0));
    QVERIFY(!editor->isFolded(0));
    QCOMPARE(editor->visibleLineTotal(), 6);
    for (int i = 0; i < 6; ++i) // identity map restored
        QCOMPARE(editor->visibleIndexOfModelLine(i), i);
    QCOMPARE(editor->toPlainText(), foldDoc()); // DoD: unfolded content unchanged
    // toggle round trip
    QVERIFY(editor->toggleFold(0));
    QCOMPARE(editor->visibleLineTotal(), 3);
    QVERIFY(editor->toggleFold(0));
    QCOMPARE(editor->visibleLineTotal(), 6);

    // collapseAll/expandAll over the whole document
    editor->setPlainText(QStringLiteral("root\n  a\n  b\nroot2\n  c"));
    editor->grab();
    editor->collapseAll();
    QCOMPARE(editor->visibleLineTotal(), 2);
    QVERIFY(editor->isFolded(0));
    QVERIFY(editor->isFolded(3));
    editor->expandAll();
    QCOMPARE(editor->visibleLineTotal(), 5);
    QCOMPARE(editor->toPlainText(), QStringLiteral("root\n  a\n  b\nroot2\n  c"));

    // The scrollbar space rides the map: on a tall doc the vBar clamp uses
    // SLOT count, not model lines (asserted through the public offset
    // getter + the public totalHeight formula recomputed from
    // visibleLineTotal()), and every hit test lands on a VISIBLE line.
    QStringList doc;
    doc << QStringLiteral("root") << QStringLiteral("  a") << QStringLiteral("  b")
        << QStringLiteral("  c");
    for (int i = 0; i < 200; ++i)
        doc << QStringLiteral("tail%1").arg(i);
    editor->setPlainText(doc.join(QLatin1Char('\n')));
    editor->grab();
    QVERIFY(editor->foldLine(0));
    const int pad = editor->padding();
    const int maxScroll = 2 * pad + editor->visibleLineTotal() * lh - editor->height();
    QVERIFY(maxScroll > 0); // tall enough to actually scroll
    QWheelEvent wheel(QPointF(300, 200), QPointF(300, 200), QPoint(0, 0),
                      QPoint(0, -120 * 1000), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QVERIFY(QApplication::sendEvent(editor, &wheel)); // deep wheel: clamps to max
    QCOMPARE(editor->verticalScrollOffset(), maxScroll); // slot-space max, exactly
    for (int y = 0; y < editor->height(); y += 5) { // no hit ever addresses a hidden line
        const int l = editor->positionAt(QPoint(editor->contentLeft() + 2, y)).line;
        QVERIFY2(editor->visibleIndexOfModelLine(l) >= 0,
                 qPrintable(QStringLiteral("hit y=%1 landed on hidden line %2")
                                .arg(y).arg(l)));
    }
}

void TestCodeEditor::caretJumpsOverFold()
{
    editor->setPlainText(foldDoc());
    editor->grab();
    QVERIFY(editor->foldLine(0));

    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Down); // slot-space step: lands on line 4
    QCOMPARE(editor->position().line, 4);
    QTest::keyClick(editor, Qt::Key_Down); // 5
    QCOMPARE(editor->position().line, 5);
    QTest::keyClick(editor, Qt::Key_Down); // last line: clamps to EOL
    QCOMPARE(editor->position().line, 5);
    QCOMPARE(editor->position().col, 4);
    QTest::keyClick(editor, Qt::Key_Up); // back to 4, col re-clamped to len("next")
    QCOMPARE(editor->position().line, 4);
    QCOMPARE(editor->position().col, 4);
    QTest::keyClick(editor, Qt::Key_Up); // fold header 0 (col 4 survives the walk,
    QCOMPARE(editor->position().line, 0); //   len("root") == 4 keeps it exact)
    QCOMPARE(editor->position().col, 4);
    QTest::keyClick(editor, Qt::Key_Up); // already first: col 0
    QCOMPARE(editor->position().col, 0);

    // Shift+Down extends the selection across the hidden run in ONE step
    // (the fan-out target is the map's next visible line).
    editor->setPosition({0, 2});
    QTest::keyClick(editor, Qt::Key_Down, Qt::ShiftModifier);
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(),
             QStringLiteral("ot\n  child1\n  child2\n  child3\nne"));

    // PageDown is slot arithmetic too: a page walk NEVER lands on a hidden
    // line (the DoD property; the exact stride semantics predate M13).
    editor->setPosition({0, 0});
    for (int i = 0; i < 12; ++i) {
        QTest::keyClick(editor, Qt::Key_PageDown);
        QVERIFY2(editor->visibleIndexOfModelLine(editor->position().line) >= 0,
                 qPrintable(QStringLiteral("PageDown landed on hidden line %1")
                                .arg(editor->position().line)));
    }
    QCOMPARE(editor->position().line, 5); // saturated at the last visible slot
    for (int i = 0; i < 12; ++i) {
        QTest::keyClick(editor, Qt::Key_PageUp);
        QVERIFY(editor->visibleIndexOfModelLine(editor->position().line) >= 0);
    }
    QCOMPARE(editor->position().line, 0);

    // Multi-caret navigation walks the same map. Alt+Click on the SECOND
    // visible row adds a caret on model line 4 — the click can never
    // address a hidden line — and one Down then steps BOTH carets.
    editor->setPosition({0, 0});
    const int lh = editor->lineHeight();
    QTest::mousePress(editor, Qt::LeftButton, Qt::AltModifier,
                      QPoint(editor->contentLeft() + 5,
                             editor->padding() + lh + lh / 2));
    QTest::mouseRelease(editor, Qt::LeftButton, Qt::AltModifier,
                        QPoint(editor->contentLeft() + 5,
                               editor->padding() + lh + lh / 2));
    QCOMPARE(editor->additionalCaretCount(), 1);
    QTest::keyClick(editor, Qt::Key_Down);
    QVector<int> lines;
    lines << editor->position().line;
    const QVector<qmn::view::TextPosition> extras = editor->additionalCarets();
    for (const qmn::view::TextPosition& t : extras)
        lines << t.line;
    std::sort(lines.begin(), lines.end());
    QCOMPARE(lines, QVector<int>({4, 5})); // {0,4} -> slot-step -> {4,5}
    for (int l : lines)
        QVERIFY(editor->visibleIndexOfModelLine(l) >= 0);

    // A caret that a fold swallows is never stranded: the arrow steps and
    // the programmatic setPosition both route through the reveal rule.
    editor->setPlainText(foldDoc());
    editor->grab();
    editor->setPosition({2, 2}); // caret INSIDE what is about to collapse
    QVERIFY(editor->foldLine(0)); // allowed: folds under the caret
    QCOMPARE(editor->visibleIndexOfModelLine(2), -1); // caret's row is now hidden
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_Down); // map step: header -> line 4
    QVERIFY(editor->visibleIndexOfModelLine(editor->position().line) >= 0);
    editor->setPosition({2, 2}); // programmatic move INTO a hidden line
    QVERIFY(!editor->isFolded(0)); // reveal opened the fold first
    QCOMPARE(editor->position().line, 2);
}

void TestCodeEditor::keyboardFoldUnfoldChords()
{
    editor->setPlainText(foldDoc());
    editor->grab();
    editor->setPosition({0, 0});

    // Ctrl+Shift+[ folds at the caret; the caret is NOT moved.
    QTest::keyClick(editor, Qt::Key_BracketLeft, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(editor->isFolded(0));
    QCOMPARE(editor->visibleLineTotal(), 3);
    QCOMPARE(editor->position().line, 0);
    QCOMPARE(editor->position().col, 0);
    // Re-fold on an already-collapsed start: no-op (state + map unchanged).
    QTest::keyClick(editor, Qt::Key_BracketLeft, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(editor->isFolded(0));
    QCOMPARE(editor->visibleLineTotal(), 3);

    // Ctrl+Shift+] unfolds exactly this fold.
    QTest::keyClick(editor, Qt::Key_BracketRight, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(!editor->isFolded(0));
    QCOMPARE(editor->visibleLineTotal(), 6);

    // Ctrl+Alt+[ / Ctrl+Alt+]: document-wide collapse/expand.
    QTest::keyClick(editor, Qt::Key_BracketLeft, Qt::ControlModifier | Qt::AltModifier);
    QVERIFY(editor->isFolded(0));
    QCOMPARE(editor->visibleLineTotal(), 3);
    QTest::keyClick(editor, Qt::Key_BracketRight, Qt::ControlModifier | Qt::AltModifier);
    QVERIFY(!editor->isFolded(0));
    QCOMPARE(editor->visibleLineTotal(), 6);
    QCOMPARE(editor->toPlainText(), foldDoc()); // chords never touch the model

    // The brace spellings (Key_BraceLeft/Right) work identically.
    QTest::keyClick(editor, Qt::Key_BraceLeft, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(editor->isFolded(0));
    QTest::keyClick(editor, Qt::Key_BraceRight, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(!editor->isFolded(0));

    // Non-foldable caret line: the chord is consumed but changes nothing.
    editor->setPosition({4, 1});
    QTest::keyClick(editor, Qt::Key_BracketLeft, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(!editor->isFolded(4));
    QCOMPARE(editor->visibleLineTotal(), 6);
}

void TestCodeEditor::revealOnProgrammaticMove()
{
    editor->setPlainText(foldDoc());
    editor->grab();
    QVERIFY(editor->foldLine(0));

    // Mouse DRAG across a fold is always map-mapped (positionAt can never
    // address a hidden line): the fold STAYS closed and the selection
    // simply spans it, unfolded content included.
    const int lh = editor->lineHeight();
    const QPointF p0(editor->contentLeft() + 2, editor->padding() + lh / 2);
    const QPointF p1(editor->contentLeft() + 2, editor->padding() + lh + lh / 2);
    QMouseEvent pr(QEvent::MouseButtonPress, p0, p0, Qt::LeftButton, Qt::LeftButton,
                   Qt::NoModifier);
    QMouseEvent mv(QEvent::MouseMove, p1, p1, Qt::LeftButton, Qt::LeftButton,
                   Qt::NoModifier);
    QMouseEvent rl(QEvent::MouseButtonRelease, p1, p1, Qt::LeftButton, Qt::LeftButton,
                   Qt::NoModifier);
    QVERIFY(QApplication::sendEvent(editor, &pr));
    QVERIFY(QApplication::sendEvent(editor, &mv));
    QVERIFY(QApplication::sendEvent(editor, &rl));
    QVERIFY(editor->isFolded(0)); // drag did NOT open the fold
    QVERIFY(editor->hasSelection());
    QCOMPARE(editor->selectedText(),
             QStringLiteral("root\n  child1\n  child2\n  child3\n")); // hidden text included
    QCOMPARE(editor->position().line, 4); // released row = model line 4

    // setPosition INTO a hidden line reveals the fold instead of stranding.
    editor->setPosition({2, 2});
    QVERIFY(!editor->isFolded(0));
    QCOMPARE(editor->position().line, 2);
    QCOMPARE(editor->position().col, 2);

    // The reveal choke point is ONE guard (ensureCaretVisible) that every
    // caret route funnels through — but the map-routed public entries
    // (mouse, keyboard, drag) can never even NAME a hidden line, so
    // setPosition is the only public door that must reveal, and it does.
}

void TestCodeEditor::mouseClickFoldZoneToggles()
{
    editor->setPlainText(foldDoc());
    editor->grab();
    const int lh = editor->lineHeight();
    const int fz = editor->foldZoneWidth();
    QVERIFY(fz > 0);
    const int zoneX = editor->contentLeft() - fz / 2; // middle of the strip
    auto zoneY = [&](int slot) { return editor->padding() + slot * lh + lh / 2; };

    // A marker-row click folds — and NEVER moves the caret (VS Code).
    editor->setPosition({5, 2});
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, QPoint(zoneX, zoneY(0)));
    QVERIFY(editor->isFolded(0));
    QCOMPARE(editor->visibleLineTotal(), 3);
    QCOMPARE(editor->position().line, 5);
    QCOMPARE(editor->position().col, 2);
    // The SAME point now sits on line 0 again (slot 0) and unfolds.
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, QPoint(zoneX, zoneY(0)));
    QVERIFY(!editor->isFolded(0));
    QCOMPARE(editor->visibleLineTotal(), 6);
    QCOMPARE(editor->position().line, 5);
    QCOMPARE(editor->position().col, 2);

    // A zone click on a NON-foldable row falls through to plain caret
    // placement (col clamps to 0 inside the gutter).
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, QPoint(zoneX, zoneY(1)));
    QVERIFY(!editor->isFolded(1));
    QCOMPARE(editor->position().line, 1);
    QCOMPARE(editor->position().col, 0);

    // Alt+Click keeps its multi-caret meaning in the zone: no fold happens.
    editor->setPosition({5, 2});
    QTest::mouseClick(editor, Qt::LeftButton, Qt::AltModifier, QPoint(zoneX, zoneY(0)));
    QVERIFY(!editor->isFolded(0));
    QCOMPARE(editor->additionalCaretCount(), 1);

    // Left gutter (the numbers area) keeps caret behavior, marker row or not.
    const int leftX = editor->padding() + 2;
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, QPoint(leftX, zoneY(0)));
    QVERIFY(!editor->isFolded(0));
    QCOMPARE(editor->position().line, 0);

    // With the fold intact, clicking the row BELOW the collapsed run
    // addresses the map: slot 1 is model line 4 even mid-fold.
    QVERIFY(editor->foldLine(0));
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(editor->contentLeft() + 2, zoneY(1)));
    QCOMPARE(editor->position().line, 4);
}

void TestCodeEditor::foldTriangleInkProbe()
{
    // No-vision shape probe: ▾ (open) tapers top-to-bottom, ▸ (closed)
    // tapers left-to-right, in both cases ONLY foldInk pixels (150,150,150,
    // R==G==B) exist inside the zone band — the numbers stay left of it.
    editor->setPlainText(foldDoc());
    editor->grab();
    editor->setPosition({5, 2}); // current-line band away from row 0
    const int lh = editor->lineHeight();
    const int fz = editor->foldZoneWidth();
    const int zx0 = editor->contentLeft() - fz;
    const int zx1 = editor->contentLeft();
    const int y0 = editor->padding();
    const QColor ink(150, 150, 150);

    auto rowCounts = [&](const QImage& im) {
        QVector<int> out;
        for (int y = y0; y < y0 + lh; ++y) {
            int c = 0;
            for (int x = zx0; x < zx1; ++x)
                if (im.pixelColor(x, y) == ink)
                    ++c;
            out << c;
        }
        return out;
    };
    auto colCounts = [&](const QImage& im) {
        QVector<int> out;
        for (int x = zx0; x < zx1; ++x) {
            int c = 0;
            for (int y = y0; y < y0 + lh; ++y)
                if (im.pixelColor(x, y) == ink)
                    ++c;
            out << c;
        }
        return out;
    };
    auto firstLast = [](const QVector<int>& prof, int& first, int& last) {
        first = -1;
        last = -1;
        for (int i = 0; i < prof.size(); ++i) {
            if (prof[i] == 0)
                continue;
            if (first < 0)
                first = i;
            last = i;
        }
    };

    // OPEN fold: ▾ down-triangle. Rows: wide base up top, point at bottom.
    QImage open = editor->grab().toImage();
    const QVector<int> rows = rowCounts(open);
    int f = -1, l = -1;
    firstLast(rows, f, l);
    QVERIFY2(f >= 0, "no fold-triangle ink in the zone on a foldable row");
    QVERIFY2(rows[f] >= 5, QStringLiteral("▾ base only %1 px wide").arg(rows[f]).toUtf8().constData());
    QVERIFY2(rows[l] <= rows[f] - 3, "▾ does not taper downward");

    // CLOSED fold: ▸ right-triangle. Columns: full-height edge on the left,
    // point on the right.
    QVERIFY(editor->foldLine(0));
    QImage closed = editor->grab().toImage();
    const QVector<int> cols = colCounts(closed);
    firstLast(cols, f, l);
    QVERIFY2(f >= 0, "no fold-triangle ink on a collapsed fold row");
    QVERIFY2(cols[f] >= 4, QStringLiteral("▸ back edge only %1 px tall").arg(cols[f]).toUtf8().constData());
    QVERIFY2(cols[l] <= cols[f] - 3, "▸ does not taper rightward");

    // Markers only exist on foldable rows: every OTHER slot's zone band is
    // ink-free (the numbers sit left of the zone; fold ink never bleeds).
    auto bandInkOutside = [&](const QImage& im, int skipSlot) {
        int stray = 0;
        for (int y = editor->padding() + (skipSlot + 1) * lh; y < editor->height(); ++y)
            for (int x = zx0; x < zx1; ++x)
                if (im.pixelColor(x, y) == ink)
                    ++stray;
        return stray;
    };
    QCOMPARE(bandInkOutside(open, 0), 0);
    QCOMPARE(bandInkOutside(closed, 0), 0);
}

void TestCodeEditor::rulerReflectsFoldRanges()
{
    // DoD: the ruler rides the SAME one map — its mapping contract is the
    // M7 formula over SLOT space + modelLineForVisibleIndex, never model
    // arithmetic, and it can never address a hidden line.
    QStringList doc;
    doc << QStringLiteral("root") << QStringLiteral("  a") << QStringLiteral("  b")
        << QStringLiteral("  c");
    for (int i = 0; i < 200; ++i)
        doc << QStringLiteral("tail%1").arg(i);
    editor->setPlainText(doc.join(QLatin1Char('\n')));
    editor->grab();
    const int lh = editor->lineHeight();
    const int pad = editor->padding();
    const int h = editor->height();
    QCOMPARE(editor->lineCount(), 204);

    // Unfolded baseline mid-strip value (identity map, M7 contract):
    const int totalU = 2 * pad + 204 * lh;
    const int midU = editor->rulerLineAt(h / 2);
    QCOMPARE(midU, (int(qint64(h / 2) * totalU / h) - pad) / lh);

    QVERIFY(editor->foldLine(0)); // hides lines 1..3 of 204
    const int vt = editor->visibleLineTotal();
    QCOMPARE(vt, 201);
    const int totalF = 2 * pad + vt * lh; // the strip ratio re-derives on slots
    int prev = -1;
    for (int y = 0; y < h; ++y) {
        const int docPx = int(qint64(y) * totalF / h);
        const int got = editor->rulerLineAt(y);
        QCOMPARE(got, editor->modelLineForVisibleIndex((docPx - pad) / lh));
        QVERIFY2(got >= prev, qPrintable(QStringLiteral("mapping not monotonic at y=%1").arg(y)));
        prev = got;
        QVERIFY2(editor->visibleIndexOfModelLine(got) >= 0, // NEVER a hidden line
                 qPrintable(QStringLiteral("ruler y=%1 mapped to hidden line %2")
                                .arg(y).arg(got)));
    }
    QCOMPARE(editor->rulerLineAt(0), 0);
    QCOMPARE(editor->rulerLineAt(h - 1), 203); // last line still reachable
    QVERIFY(editor->rulerLineAt(h / 2) != midU); // strip geometry actually moved
    // Hidden lines cost NO strip rows anymore (the fold is visible IN the
    // strip: the run simply does not exist there):
    for (int y = 0; y < h; ++y) {
        const int got = editor->rulerLineAt(y);
        QVERIFY2(got != 1 && got != 2 && got != 3,
                 qPrintable(QStringLiteral("strip row %1 shows hidden line %2").arg(y).arg(got)));
    }

    // The viewport rect rides the same slot-space total (vpWant math with
    // visibleLineTotal()): deep wheel lands the rect at the strip bottom.
    QWheelEvent wheel(QPointF(300, 200), QPointF(300, 200), QPoint(0, 0),
                      QPoint(0, -120 * 1000), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QVERIFY(QApplication::sendEvent(editor, &wheel));
    const int v = editor->verticalScrollOffset();
    QCOMPARE(v, totalF - h);
    const int y1 = int(qint64(std::min(totalF, v + h)) * h / totalF);
    QCOMPARE(editor->overviewRulerViewportRect().bottom(), h - 1); // flush at strip end
    QVERIFY(y1 >= h);
    // Ruler click-to-scroll now addresses slots too: click strip bottom and
    // the top row is the LAST slot's line (clamped), never a hidden line.
    const int w = editor->width();
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(w - CodeEditor::kOverviewRulerWidth / 2, 2));
    QVERIFY(editor->visibleIndexOfModelLine(editor->firstVisibleLine()) >= 0);
    QCOMPARE(editor->firstVisibleLine(), 0); // strip top = slot 0 = model line 0
}

void TestCodeEditor::foldEditBookkeepingAndReset()
{
    // Reaching a fold BODY row with the caret is impossible (the reveal rule
    // opens the fold under any programmatic move; mouse/keys ride the map),
    // so the bookkeeping paths an edit can actually trigger are: an edit
    // entirely ABOVE a fold (whole fold shifts), a join AT the header EOL
    // (the END shifts; the fold survives shorter) and deletes collapsing
    // end <= start (the fold dies). Each path gets its own paragraph.

    // --- edit entirely above the fold: the whole fold slides down --------
    editor->setPlainText(QStringLiteral("head\nroot\n  child1\n  child2\n  child3\nend"));
    editor->grab();
    QVERIFY(editor->foldLine(1)); // {1,4}: "root" heads the block, "end" ends it
    QCOMPARE(editor->visibleLineTotal(), 3); // slots: head, root, end

    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_Return); // a NEW line above the fold start
    QCOMPARE(editor->lineCount(), 7);
    QVERIFY(!editor->isFolded(1));
    QVERIFY(editor->isFolded(2)); // {1,4} -> {2,5}: the whole fold shifted
    QCOMPARE(editor->visibleLineTotal(), 4); // one more visible row, same body
    QCOMPARE(editor->visibleIndexOfModelLine(0), 0); // "" (caret's new line)
    QCOMPARE(editor->visibleIndexOfModelLine(1), 1); // "head"
    QCOMPARE(editor->visibleIndexOfModelLine(2), 2); // "root" (fold header)
    QCOMPARE(editor->visibleIndexOfModelLine(3), -1); // hidden body 3..5
    QCOMPARE(editor->visibleIndexOfModelLine(5), -1);
    QCOMPARE(editor->visibleIndexOfModelLine(6), 3); // "end": last slot

    // Undo re-shifts the fold back (fold state itself is not an undoable
    // action — documented v1 stance — but edits keep it consistent).
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->lineCount(), 6);
    QVERIFY(editor->isFolded(1)); // {2,5} -> {1,4}
    QCOMPARE(editor->visibleLineTotal(), 3);
    QCOMPARE(editor->visibleIndexOfModelLine(2), -1);
    QCOMPARE(editor->visibleIndexOfModelLine(5), 2); // "end" back at slot 2

    // --- delete spanning the hidden rows: the fold dies -------------------
    // (header through the line after the body — the only keyboard-reachable
    // selection spanning hidden rows — collapses end below the start)
    editor->setPlainText(foldDoc());
    editor->grab();
    QVERIFY(editor->foldLine(0)); // {0,3}
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_Down, Qt::ShiftModifier); // map step to (4,0)
    QVERIFY(editor->hasSelection());
    QTest::keyClick(editor, Qt::Key_Delete);
    QCOMPARE(editor->lineCount(), 2);
    QVERIFY(!editor->isFolded(0));
    QCOMPARE(editor->visibleLineTotal(), 2);
    QCOMPARE(editor->toPlainText(), QStringLiteral("next\nlast"));

    // --- join AT the fold header's EOL: the END shifts, fold survives -----
    // Delete at (0,4) eats the newline between the VISIBLE header and the
    // hidden body: child1's text lands on the header row, and the fold's
    // anchor (its start line) never moved — only the end shifts by delta.
    editor->setPlainText(foldDoc());
    editor->grab();
    QVERIFY(editor->foldLine(0)); // {0,3}, hidden run 1..3
    editor->setPosition({0, 4}); // EOL of "root" (the header, visible)
    QTest::keyClick(editor, Qt::Key_Delete); // anchor 0, delta -1: {0,3} -> {0,2}
    QCOMPARE(editor->lineCount(), 5);
    QVERIFY(editor->isFolded(0)); // the fold SURVIVED the join
    QCOMPARE(editor->visibleLineTotal(), editor->lineCount() - 2); // 5 - 2 hidden
    QCOMPARE(editor->toPlainText(),
             QStringLiteral("root  child1\n  child2\n  child3\nnext\nlast"));

    // setPlainText resets ALL fold state (fresh doc, fresh view).
    editor->setPlainText(foldDoc());
    editor->grab();
    QVERIFY(editor->foldLine(0));
    editor->setPlainText(QStringLiteral("x"));
    QVERIFY(!editor->isFolded(0));
    QCOMPARE(editor->visibleLineTotal(), 1);
    QCOMPARE(editor->lineCount(), 1);
}

void TestCodeEditor::pythonGrammarFoldsAndCollapseAll()
{
    editor->setPlainText(QStringLiteral(
        "def outer():\n"
        "    x = 1\n"
        "    def inner():\n"
        "        return x\n"
        "    return inner\n"
        "print(outer())\n"));
    editor->setLanguage(QStringLiteral("python"));
    editor->grab(); // paint warms the windowed fold cache (paint-only pass)

    // tree-sitter @fold captures, NOT the indent heuristic: the inner def
    // is foldable even though the indent rule would also agree, and the
    // OUTER capture's end is the node's last body line (line 4) — the
    // print statement at line 5 is outside the function_definition node.
    QVERIFY(editor->isFoldable(0));
    QVERIFY(editor->isFoldable(2));
    QVERIFY(!editor->isFoldable(1));
    QVERIFY(!editor->isFoldable(4));
    const auto ranges = editor->foldableRanges();
    QCOMPARE(ranges.size(), 2);
    QCOMPARE(ranges[0].startLine, 0);
    QCOMPARE(ranges[0].endLine, 4);
    QCOMPARE(ranges[1].startLine, 2);
    QCOMPARE(ranges[1].endLine, 3);

    // Same-start collision rule (widest capture wins = outermost fold).
    QVERIFY(editor->foldLine(0)); // hides 1..4
    QCOMPARE(editor->visibleLineTotal(), 3); // lines 0, 5, 6 (7 incl. EOL line)
    QCOMPARE(editor->visibleIndexOfModelLine(4), -1);
    QCOMPARE(editor->visibleIndexOfModelLine(5), 1);

    // collapseAll: outer folds first, the inner start is then HIDDEN, and
    // foldLine's hidden-start rule skips it — nesting without a fold tree.
    editor->expandAll();
    QCOMPARE(editor->visibleLineTotal(), 7);
    editor->collapseAll();
    QCOMPARE(editor->visibleLineTotal(), 3);
    QVERIFY(editor->isFolded(0));
    QVERIFY(!editor->isFolded(2)); // inner skipped: its start row is gone
    editor->expandAll();
    QCOMPARE(editor->visibleLineTotal(), 7);
    QCOMPARE(editor->lineCount(), 7);
}

void TestCodeEditor::jsonFoldHidesClosingBrace()
{
    // Grammar ends make the closing brace part of the fold: json object
    // nodes END ON the `}` line, so (start, end] hides it — the standard
    // editor behavior the header doc promises.
    const QString doc = QStringLiteral("{\n  \"a\": [\n    1,\n    2\n  ]\n}");
    editor->setPlainText(doc);
    editor->setLanguage(QStringLiteral("json"));
    editor->grab();
    const auto ranges = editor->foldableRanges();
    QCOMPARE(ranges.size(), 2); // object {0,5} + array {1,4}
    QCOMPARE(ranges[0].startLine, 0);
    QCOMPARE(ranges[0].endLine, 5);
    QCOMPARE(ranges[1].startLine, 1);
    QCOMPARE(ranges[1].endLine, 4);

    QVERIFY(editor->foldLine(0));
    QCOMPARE(editor->visibleLineTotal(), 1); // ONLY line 0 remains
    QCOMPARE(editor->visibleIndexOfModelLine(5), -1); // the `}` is hidden too
    QCOMPARE(editor->firstVisibleLine(), 0);
    QVERIFY(editor->unfoldLine(0));
    QCOMPARE(editor->visibleLineTotal(), 6);
    QCOMPARE(editor->toPlainText(), doc); // DoD: model untouched by folding
}

void TestCodeEditor::foldZoneGutterConstant()
{
    editor->setPlainText(foldDoc());
    editor->grab();
    const double cw = editor->cellWidth();
    const int fz = std::max(10, qRound(2.2 * cw));
    QCOMPARE(editor->foldZoneWidth(), fz);

    // The zone width is a constant chrome budget: independent of document
    // size, line-number width and fold state (the pre-M13 gutter tests
    // already pin digit/crossover math — this pins the fold-zone term).
    QStringList many;
    for (int i = 0; i < 500; ++i)
        many << QStringLiteral("line %1").arg(i);
    editor->setPlainText(many.join(QLatin1Char('\n')));
    editor->grab();
    QCOMPARE(editor->foldZoneWidth(), fz);
    const int wide = editor->gutterWidth();
    QVERIFY(editor->foldLine(0) == false); // "line 0" is not foldable here
    QCOMPARE(editor->gutterWidth(), wide); // fold state never resizes the gutter

    // Gutter Off IS the fold-chrome switch: the zone disappears from the
    // layout, the API reports 0, and folding itself keeps working.
    editor->setPlainText(foldDoc());
    editor->grab();
    const int wOn = editor->gutterWidth();
    QVERIFY(wOn >= fz);
    editor->setLineNumbersMode(CodeEditor::LineNumbersMode::Off);
    QCOMPARE(editor->foldZoneWidth(), 0);
    QCOMPARE(editor->gutterWidth(), 0); // Off = no gutter chrome at all
    editor->grab();
    QVERIFY(editor->foldLine(0)); // folding works without the chrome
    QCOMPARE(editor->visibleLineTotal(), 3);
    editor->expandAll();
    editor->setLineNumbersMode(CodeEditor::LineNumbersMode::On);
    QCOMPARE(editor->gutterWidth(), wOn); // zone back, same constant
}

// Owner-reported M13 fallout: the indent fallback fired in GRAMMAR modes
// too (the always-union design), so js/json got python-style pseudo-blocks
// following leading whitespace. Fix: the indent scan is a fallback for
// grammar-LESS modes (or a language whose fold query failed to compose);
// in grammar modes nesting is only what the brackets/syntax say. Bracket
// structures that js/json users expect to fold (multiline calls, python
// literals) therefore had to JOIN the grammar node lists.
void TestCodeEditor::grammarFoldsFollowBracketsNotIndent()
{
    // --- js: a deeper-indented line is NOT a block -----------------------
    editor->setPlainText(QStringLiteral(
        "a();\n"
        "    b();\n"
        "c(\n"
        "  x\n"
        ");\n"));
    editor->setLanguage(QStringLiteral("javascript"));
    editor->grab();
    QVERIFY(!editor->isFoldable(0)); // "b();" deeper: python nests, js must not
    // The multiline CALL still folds — now via the grammar's `arguments`
    // node (bracket semantics, VS Code-style), not via the indent scan:
    const auto ranges = editor->foldableRanges();
    QCOMPARE(ranges.size(), 1);
    QCOMPARE(ranges[0].startLine, 2);
    QCOMPARE(ranges[0].endLine, 4);
    QVERIFY(editor->foldLine(2));
    QCOMPARE(editor->visibleLineTotal(), editor->lineCount() - 2);
    editor->expandAll();
    QCOMPARE(editor->visibleLineTotal(), editor->lineCount());

    // Grammar mode: foldLine on an indent-only artifact finds nothing.
    QVERIFY(!editor->foldLine(0));
    QVERIFY(!editor->isFolded(0));

    // --- json: jagged key indents never create depth --------------------
    editor->setPlainText(QStringLiteral("{\n\"a\": 1,\n    \"b\": 2\n}"));
    editor->setLanguage(QStringLiteral("json"));
    editor->grab();
    QVERIFY(!editor->isFoldable(1)); // the deeper key line: no pseudo-fold
    const auto jr = editor->foldableRanges();
    QCOMPARE(jr.size(), 1); // the grammar object {0,3} is the ONLY fold
    QCOMPARE(jr[0].startLine, 0);
    QCOMPARE(jr[0].endLine, 3);

    // --- python: brackets own the multiline spans, the indent scan is gone
    editor->setPlainText(QStringLiteral("d = {\n  1: 2\n}\nif d:\n    pass"));
    editor->setLanguage(QStringLiteral("python"));
    editor->grab();
    const auto pr = editor->foldableRanges();
    QCOMPARE(pr.size(), 2);
    QCOMPARE(pr[0].startLine, 0);
    QCOMPARE(pr[0].endLine, 2); // dictionary node
    QCOMPARE(pr[1].startLine, 3);
    QCOMPARE(pr[1].endLine, 4); // if_statement node

    // The indent fallback stays alive exactly where the PLAN E2 spec put
    // it: grammar-LESS modes (here: the same js text with no language).
    editor->setLanguage(QString());
    editor->grab();
    QVERIFY(editor->isFoldable(0)); // plain text: the indent scan returns
    QVERIFY(editor->foldLine(0));
    QVERIFY(editor->isFolded(0));
}

void TestCodeEditor::foldAutoExpandOnSourceDeath()
{
    // Owner-requested symmetry (2026-11-11): an edit that kills a fold's
    // SOURCE auto-expands a COLLAPSED fold, exactly like the same edit
    // simply drops the marker of an unfolded one (whose line then gets no
    // foldable row at all). The check rides the paint-time fold-cache
    // recompute (grammar validation cannot run per keystroke on big docs),
    // so each edit below needs one paint (grab) to take effect.

    // --- indent mode, source DEAD: re-typing the header line with an
    // indent kills the pair rule (next line no longer DEEPER than the
    // header), while the edit bookkeeping (delta 0, alive end) happily
    // keeps {0,3} — the old stale-marker branch kept ▸ + hidden rows
    // forever; now only the source check decides, and it opens the fold.
    editor->setPlainText(foldDoc());
    editor->grab();
    QVERIFY(editor->foldLine(0)); // {0,3}, hidden run 1..3
    QCOMPARE(editor->visibleLineTotal(), 3);
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier); // select "root"
    QTest::keyClicks(editor, QStringLiteral("  root")); // indented header
    QVERIFY(editor->isFolded(0)); // bookkeeping alone: still collapsed
    editor->grab(); // the paint path runs the fold-source re-check
    QVERIFY(!editor->isFolded(0)); // ...and auto-expands the dead fold
    QCOMPARE(editor->lineCount(), 6);
    QCOMPARE(editor->visibleLineTotal(), 6); // the body came back
    QVERIFY(!editor->isFoldable(0)); // marker gone, like the twin case
    QVERIFY(!editor->foldLine(0));
    QCOMPARE(editor->toPlainText(),
             QStringLiteral("  root\n  child1\n  child2\n  child3\nnext\nlast"));

    // --- indent mode, source ALIVE: a blank line under the header must
    // not fake a dead pair (the blank skip is part of the foldEndForLine
    // source rule this probe mirrors).
    editor->setPlainText(QStringLiteral("root\n\n  child\nnext"));
    editor->grab();
    QVERIFY(editor->foldLine(0)); // {0,2}: born through the blank skip
    QCOMPARE(editor->visibleLineTotal(), 2);
    editor->setPosition({0, 4}); // EOL of "root" (the visible header)
    QTest::keyClicks(editor, QStringLiteral("!"));
    editor->grab();
    QVERIFY(editor->isFolded(0)); // pair (0 -> first NON-BLANK line 2) alive
    QCOMPARE(editor->visibleLineTotal(), 2);
    QVERIFY(editor->unfoldLine(0));

    // --- grammar mode, source DEAD: the @fold capture IS the source.
    // Replacing the header text kills the function_definition node (the
    // orphaned indent block lands in ERROR nodes, which no fold capture
    // matches) while delta bookkeeping keeps {0,2} intact — the grammar
    // re-check opens it, and useIndent=false means no indent second
    // opinion rescues it (grammar modes fold on syntax, not whitespace).
    editor->setPlainText(QStringLiteral("def f():\n    x = 1\n    y = 2\nz = 3\n"));
    editor->setLanguage(QStringLiteral("python"));
    editor->grab();
    QVERIFY(editor->isFoldable(0));
    QVERIFY(editor->foldLine(0)); // function_definition {0,2}, hides 1..2
    QCOMPARE(editor->visibleLineTotal(), 3); // 5 model lines - 2 hidden
    editor->setPosition({0, 0});
    QTest::keyClick(editor, Qt::Key_End, Qt::ShiftModifier); // select header
    QTest::keyClicks(editor, QStringLiteral("z = 3"));
    QVERIFY(editor->isFolded(0)); // bookkeeping kept it...
    editor->grab(); // ...the capture re-check opens it
    QVERIFY(!editor->isFolded(0));
    QCOMPARE(editor->lineCount(), 5);
    QCOMPARE(editor->visibleLineTotal(), 5);
    QVERIFY(!editor->isFoldable(0)); // no grammar capture starts line 0 now

    // --- grammar mode, source ALIVE: a harmless header edit (whitespace
    // after '{') moves no structure: the object capture still starts on
    // line 0, the fold SURVIVES — auto-expand only fires on source death.
    editor->setPlainText(QStringLiteral("{\n  \"a\": 1\n}"));
    editor->setLanguage(QStringLiteral("json"));
    editor->grab();
    QVERIFY(editor->foldLine(0)); // object {0,2}
    QCOMPARE(editor->visibleLineTotal(), 1); // 3 model lines - 2 hidden
    editor->setPosition({0, 1}); // right after '{'
    QTest::keyClicks(editor, QStringLiteral("  "));
    editor->grab();
    QVERIFY(editor->isFolded(0)); // capture alive: the fold stays closed
    QCOMPARE(editor->visibleLineTotal(), 1);
    QVERIFY(editor->unfoldLine(0));
    QCOMPARE(editor->visibleLineTotal(), 3);
}

QTEST_MAIN(TestCodeEditor)
#include "tst_codeeditor.moc"
