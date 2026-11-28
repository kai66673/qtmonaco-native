// Drives the qmn-ide-demo app (GATE 4 car I0 skeleton + car I1 panel +
// car I2 quick-open/palette) fully offscreen through the SAME
// programmatic doors the dock widgets, popups and panel buttons call —
// no dialog exec, no file dialogs, and NO
// server ever spawns (the --lsp door's tst_demo law rides whole: the
// one slot touching the hub opens only languages NO built-in spec
// claims). tst_ide never includes a glue or search header: IdeWindow.hpp
// keeps those types behind forward-decls, and the panel's own read doors
// (searchSummary/searchResultFileAt/…) are the observation channel.
// Assertions are on model/widget state, never on pixels.

#include "app/IdeWindow.hpp"

#include "view/CodeEditor.hpp"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include <functional>

using qmn::app::IdeWindow;
using qmn::view::CodeEditor;
using qmn::view::TextPosition;

namespace {

bool waitUntil(const std::function<bool()>& cond, int timeoutMs = 15000)
{
    for (int waited = 0; !cond(); waited += 10) {
        if (waited > timeoutMs)
            return false;
        QTest::qWait(10);
    }
    return true;
}

} // namespace

class TestIde : public QObject {
    Q_OBJECT
private:
    std::unique_ptr<IdeWindow> window_;
    std::unique_ptr<QTemporaryDir> temp_;

    void writeFile(const QString& name, const QByteArray& content)
    {
        const QString path = temp_->filePath(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
        file.write(content);
    } // VOID on purpose: a QVERIFY inside a value-returning helper is the
      // return-statement-with-no-value trap — callers spell the path with
      // temp_->filePath(name) themselves

private slots:
    void init()
    {
        temp_ = std::make_unique<QTemporaryDir>();
        QVERIFY(temp_->isValid());
        window_ = std::make_unique<IdeWindow>(); // fresh per slot: the
                                                 // session/panel state is
                                                 // slot-local by
                                                 // construction
        // The overlays are CHILD widgets: their visibility doors read
        // true only under a shown window (Qt's own visibility law) — so
        // the window is shown for EVERY slot (offscreen, no exec, the
        // no-dialog law untouched).
        window_->show();
        window_->activateWindow();
        QTest::qWait(20);
    }
    void cleanup()
    {
        window_.reset(); // the window (hub/session/search) dies BEFORE
                         // the temp dir it may have open tabs in
        temp_.reset();
    }

    // ------------------------------------------------------- I0: skeleton

    void projectDoorRoundTrips()
    {
        QVERIFY(window_->projectRoot().isEmpty()); // NO project until the
                                                   // door is called
        window_->openProject(temp_->filePath(QLatin1String("sub/../"))); // the
        // root door normalizes lexically (QDir absolutePath) — the same
        // dir the writes below land in:
        QCOMPARE(window_->projectRoot(), QDir(temp_->path()).absolutePath());
        QVERIFY(window_->statusBar()->currentMessage().contains(
            QStringLiteral("\u041f\u0440\u043e\u0435\u043a\u0442:")));
    }

    void openFileTabFaces()
    {
        window_->openProject(temp_->path());
        writeFile(QStringLiteral("a.py"), "sum = 1\n");
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("a.py"))));
        QCOMPARE(window_->editorCount(), 1);
        QCOMPARE(window_->currentEditor()->language(), QStringLiteral("python")); // the
        // session's suffix table wired the grammar at the tab door (G2
        // shape unchanged)
        writeFile(QStringLiteral("note.txt"), "plain\n");
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("note.txt"))));
        QCOMPARE(window_->editorCount(), 2);
        QCOMPARE(window_->currentEditor()->language(), QString()); // unmapped
                                                                   // suffix:
                                                                   // honest
                                                                   // plain

        // A failed read is FALSE + a non-modal status-bar message (the
        // demo's drivability law): no dialog to block ctest.
        QVERIFY(!window_->openFile(temp_->filePath(QLatin1String("nope.txt"))));
        QCOMPARE(window_->editorCount(), 2);
        QVERIFY(window_->statusBar()->currentMessage().contains(
            QStringLiteral("\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c")));
    }

    void reopeningPathFocusesTheOneTab()
    {
        window_->openProject(temp_->path());
        writeFile(QStringLiteral("a.py"), "sum = 1\n");
        const QString py = temp_->filePath(QStringLiteral("a.py"));
        QVERIFY(window_->openFile(py));
        QVERIFY(window_->openFile(py)); // the ONE-tab law: a re-open
                                        // FOCUSES, never a second tab,
                                        // never a second registration
        // A lexical variant of the SAME path focuses the SAME tab (the
        // tab's key is the normalized absolute path):
        QVERIFY(window_->openFile(QFileInfo(py).absoluteDir().filePath(
            QStringLiteral("./a.py"))));
        QCOMPARE(window_->editorCount(), 1);
        QVERIFY(window_->statusBar()->currentMessage().contains(
            QStringLiteral("\u0444\u043e\u043a\u0443\u0441"))); // "focus"
                                                               // feedback,
                                                               // not silence
    }

    void closeTabThenReopenWorks()
    {
        window_->openProject(temp_->path());
        writeFile(QStringLiteral("a.py"), "sum = 1\n");
        const QString py = temp_->filePath(QStringLiteral("a.py"));
        QVERIFY(window_->openFile(py));
        window_->closeTabAt(0);
        QCOMPARE(window_->editorCount(), 0);
        QVERIFY(!window_->currentEditor());
        // The session drop rode closeTabAt (closeDocument), so the
        // re-open is a CLEAN registration — the duplicate-uri face must
        // NOT fire here (that face stays tst_glue's turf; this pin says
        // the IDE's close order leaves no squatter):
        QVERIFY(window_->openFile(py));
        QCOMPARE(window_->editorCount(), 1);
    }

    // --------------------------------------------------- I1: the panel

    void searchPanelRowsRideWalkOrder()
    {
        // ONE root dir, codepoint-sorted Name order (the engine's sort
        // is locale-independent): a.py (2 matched lines), b.py (1), and
        // c.txt scanned-clean (no match, still COUNTED as scanned).
        window_->openProject(temp_->path());
        writeFile(QStringLiteral("a.py"), "sum = 1\nx sum y\n");
        writeFile(QStringLiteral("b.py"), "nothing here\nsum\n");
        writeFile(QStringLiteral("c.txt"), "nothing to see\n");
        const QString a = temp_->filePath(QStringLiteral("a.py"));
        const QString b = temp_->filePath(QStringLiteral("b.py"));

        window_->runSearch(QStringLiteral("sum"));
        QVERIFY(window_->searchRunning()); // the walk is event-loop-paced
        QVERIFY2(waitUntil([this] { return !window_->searchRunning(); }),
                 "the walk never finished");

        // The summary face: the ENGINE's truth (3 matches / 2 files / 3
        // scanned) in the panel's Russian spelling — one truth two faces.
        QVERIFY(window_->searchSummary().contains(QStringLiteral("3")));
        QVERIFY(window_->searchSummary().contains(QStringLiteral("\u041d\u0430\u0439\u0434\u0435\u043d\u043e")));
        QVERIFY(window_->statusBar()->currentMessage() == window_->searchSummary());

        // The flattened row model (file-major, per MATCHED LINE):
        QCOMPARE(window_->searchResultRowCount(), 3);
        QCOMPARE(window_->searchResultFileAt(0), QFileInfo(a).absoluteFilePath());
        QCOMPARE(window_->searchResultLineAt(0), 0);
        QCOMPARE(window_->searchResultFileAt(1), QFileInfo(a).absoluteFilePath());
        QCOMPARE(window_->searchResultLineAt(1), 1);
        QCOMPARE(window_->searchResultFileAt(2), QFileInfo(b).absoluteFilePath());
        QCOMPARE(window_->searchResultLineAt(2), 1);
        QCOMPARE(window_->searchResultFileAt(3), QString()); // honest out-
        QCOMPARE(window_->searchResultLineAt(3), -1);        // of-range
    }

    void activateRevealsCaretAndSelection()
    {
        window_->openProject(temp_->path());
        writeFile(QStringLiteral("a.py"), "sum = 1\nx sum y\n");
        writeFile(QStringLiteral("b.py"), "nothing here\nsum\n");
        window_->runSearch(QStringLiteral("sum"));
        QVERIFY2(waitUntil([this] { return !window_->searchRunning(); }),
                 "the walk never finished");
        QCOMPARE(window_->searchResultRowCount(), 3);

        // Row 2 = b.py line 1 (col 0-3). The reveal rides the PUBLIC
        // doors only (the GATE 4 I1 LEDGER widening — setSelection went
        // public for exactly this): open/focus the tab, caret at the
        // span's END (the active end), the WHOLE span selected.
        QVERIFY(window_->activateSearchResult(2));
        QCOMPARE(window_->editorCount(), 1);
        QCOMPARE(window_->currentEditor()->language(), QStringLiteral("python"));
        QCOMPARE(window_->currentEditor()->position(), (TextPosition{1, 3}));
        QVERIFY(window_->currentEditor()->hasSelection());
        QCOMPARE(window_->currentEditor()->selectedText(), QStringLiteral("sum"));

        // Row 0 = a.py line 0 — focusing the OTHER file is a NEW tab
        // (the one-tab law was never about hiding files, about not
        // duplicating them):
        QVERIFY(window_->activateSearchResult(0));
        QCOMPARE(window_->editorCount(), 2);
        QCOMPARE(window_->currentEditor()->position(), (TextPosition{0, 3}));
        QCOMPARE(window_->currentEditor()->selectedText(), QStringLiteral("sum"));

        // Re-activating row 1 (a.py, line 1, cols 2-5 of "x sum y")
        // FOCUSES the a.py tab — no third tab — and moves the
        // caret+selection there; the reveal is idempotent against focus:
        QVERIFY(window_->activateSearchResult(1));
        QCOMPARE(window_->editorCount(), 2);
        QCOMPARE(window_->currentEditor()->position(), (TextPosition{1, 5}));
        QCOMPARE(window_->currentEditor()->selectedText(), QStringLiteral("sum"));

        // Out of range: an honest false, nothing moved.
        const TextPosition before = window_->currentEditor()->position();
        QVERIFY(!window_->activateSearchResult(99));
        QVERIFY(!window_->activateSearchResult(-1));
        QCOMPARE(window_->currentEditor()->position(), before);
    }

    void searchRefusalFacesAreLoud()
    {
        window_->openProject(temp_->filePath(QLatin1String("missing-dir")));

        // A bad regex: the refusal lands SYNCHRONOUSLY (the engine's
        // connect-before-start law is paid by the panel's CTOR
        // connections — the summary is written the moment runSearch
        // returns), the row set is empty, and the walk never ran.
        window_->runSearch(QStringLiteral("(sum"), false, false, true);
        QVERIFY(!window_->searchRunning());
        QCOMPARE(window_->searchResultRowCount(), 0);
        QVERIFY(window_->searchSummary().startsWith(
            QStringLiteral("\u041f\u043e\u0438\u0441\u043a \u043e\u0442\u043a\u043b\u043e\u043d\u0451\u043d"))); // "refused"
        // The engine's MACHINE reason rides verbatim inside the human
        // text — the panel never erases the truth:
        QVERIFY(window_->searchSummary().contains(QStringLiteral("query:")));

        // A root the engine refuses: the "root:" prefix rides through too.
        window_->runSearch(QStringLiteral("sum"));
        QVERIFY(window_->searchSummary().contains(QStringLiteral("root:")));

        // NO project at all: the PANEL refuses before the engine — an
        // honest summary, never a silent zero (fresh window_, no door
        // was ever opened on it):
        window_ = std::make_unique<IdeWindow>();
        window_->runSearch(QStringLiteral("sum"));
        QVERIFY(!window_->searchRunning());
        QVERIFY(window_->searchSummary().contains(
            QStringLiteral("\u043e\u0442\u043a\u0440\u043e\u0439\u0442\u0435"))); // "open
        // a project first"
    }

    void cancelDoorStopsHonestly()
    {
        window_->openProject(temp_->path());
        for (int i = 0; i < 120; ++i) // wide enough that the full walk
                                      // would span many chunks
            writeFile(QStringLiteral("f%1.py").arg(i, 3, 10, QLatin1Char('0')),
                      "sum line\nmore lines here\n");

        window_->runSearch(QStringLiteral("sum"));
        // runSearch → start() (no pump tick without the event loop) and
        // cancelSearch lands BEFORE the first chunk: the honest stop
        // face is "cancelled · partial", never the completed spelling,
        // and the zero rows are the CANCELLED zero, not a clean none.
        window_->cancelSearch();
        QVERIFY2(waitUntil([this] { return !window_->searchRunning(); }),
                 "the cancelled walk never finished");
        QVERIFY(window_->searchSummary().contains(QStringLiteral("\u043e\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d"))); // "stopped"
        QVERIFY(!window_->searchSummary().contains(QStringLiteral("\u041d\u0430\u0439\u0434\u0435\u043d\u043e"))); // NOT
                                                                                                              // the
                                                                                                              // completed
                                                                                                              // face
    }

    // --------------------------------------------- car I2: quick open + palette

    void quickOpenPoolRidesTheListFace()
    {
        writeFile(QStringLiteral("a.py"), "sum = 1\n");
        writeFile(QStringLiteral("z.py"), "sum = 2\n");
        writeFile(QStringLiteral("sub/b.py"), "sum = 3\n");
        writeFile(QStringLiteral(".hidden.txt"), "sum\n"); // filtered OUT
        writeFile(QStringLiteral(".git/obj.txt"), "sum\n"); // subtree OUT
        window_->openProject(temp_->path());
        QVERIFY(window_->projectFilesWalking()); // the LIST-face walk rode
                                                 // the project door
        QVERIFY2(waitUntil([this] { return !window_->projectFilesWalking(); }),
                 "the pool walk never finished");
        QCOMPARE(window_->projectFileCount(), 3); // hidden/git never listed

        window_->showQuickOpen();
        QCOMPARE(window_->quickOpenRowCount(), 3);
        // EMPTY needle = the POOL WALK ORDER (files of a dir before its
        // subdirs, per-dir codepoint sort — the engine's own order, pool
        // untouched by the scorer):
        QCOMPARE(window_->quickOpenPathAt(0), temp_->filePath(QStringLiteral("a.py")));
        QCOMPARE(window_->quickOpenPathAt(1), temp_->filePath(QStringLiteral("z.py")));
        QCOMPARE(window_->quickOpenPathAt(2), temp_->filePath(QStringLiteral("sub/b.py")));
        QCOMPARE(window_->quickOpenPathAt(3), QString()); // honest out-
        QCOMPARE(window_->quickOpenPathAt(-1), QString()); // of-range
        QVERIFY(window_->quickOpenSummary().contains(QStringLiteral("3")));
        QVERIFY(window_->quickOpenSummary().contains(
            QStringLiteral("\u0412\u0441\u0435\u0433\u043e"))); // "total"
    }

    void quickOpenFuzzyRankIsScoreThenPath()
    {
        // The frozen ranking law on a hand-computed score table (the
        // qmn::fuzzy law at src/fuzzy/FuzzyMatcher.hpp):
        //   foo/bar.cpp     "fb": f@0 (start 4) + b@4 ('/' seam 4) = 8
        //   a/fb2.txt       "fb": 4 + (1 + adjacency 2) = 7 — TIED
        //   b/fb1.txt       "fb": 7                                     —
        //   fb.md           "fb": 7          ties resolve by DISPLAYED
        //   src/fizzbuzz.txt"fb": f@4 (4) + b@8 (base 1) = 5  PATH asc
        writeFile(QStringLiteral("foo/bar.cpp"), "// x\n");
        writeFile(QStringLiteral("fb.md"), "x\n");
        writeFile(QStringLiteral("src/fizzbuzz.txt"), "x\n");
        writeFile(QStringLiteral("a/fb2.txt"), "x\n");
        writeFile(QStringLiteral("b/fb1.txt"), "x\n");
        window_->openProject(temp_->path());
        QVERIFY2(waitUntil([this] { return !window_->projectFilesWalking(); }),
                 "the pool walk never finished");
        QCOMPARE(window_->projectFileCount(), 5);

        window_->showQuickOpen();
        window_->typeQuickOpenQuery(QStringLiteral("fb"));
        QCOMPARE(window_->quickOpenRowCount(), 5);
        const QStringList want {
            QStringLiteral("foo/bar.cpp"), QStringLiteral("a/fb2.txt"),
            QStringLiteral("b/fb1.txt"), QStringLiteral("fb.md"),
            QStringLiteral("src/fizzbuzz.txt")};
        QStringList got;
        for (int r = 0; r < 5; ++r)
            got << QDir(temp_->path()).relativeFilePath(window_->quickOpenPathAt(r));
        QCOMPARE(got, want);

        // Case-insensitive needle, and a needle that NO file embeds:
        window_->typeQuickOpenQuery(QStringLiteral("FB"));
        QCOMPARE(window_->quickOpenRowCount(), 5); // same table, same order
        window_->typeQuickOpenQuery(QStringLiteral("qqqzzz"));
        QCOMPARE(window_->quickOpenRowCount(), 0); // the scorer says no —
        QVERIFY(window_->quickOpenSummary().contains(
            QStringLiteral("\u041d\u0435\u0442 \u0441\u043e\u0432\u043f\u0430\u0434\u0435\u043d\u0438\u0439"))); // "no matches", LOUD
    }

    void quickOpenActivationFaces()
    {
        writeFile(QStringLiteral("x.py"), "first tab\n");
        writeFile(QStringLiteral("y.py"), "second tab\n");
        window_->openProject(temp_->path());
        QVERIFY2(waitUntil([this] { return !window_->projectFilesWalking(); }),
                 "the pool walk never finished");

        window_->showQuickOpen();
        QVERIFY(window_->quickOpenVisible());
        // Row 1 = y.py (walk order): activation opens the tab AND closes
        // the overlay:
        QVERIFY(window_->activateQuickOpen(1));
        QCOMPARE(window_->editorCount(), 1);
        QCOMPARE(window_->currentEditor()->toPlainText(), QStringLiteral("second tab\n"));
        QVERIFY(!window_->quickOpenVisible());

        // Out of range: honest false, the popup STAYS open (another row
        // may still be the truth):
        window_->showQuickOpen();
        QVERIFY(window_->quickOpenVisible());
        QVERIFY(!window_->activateQuickOpen(99));
        QVERIFY(!window_->activateQuickOpen(-1));
        QVERIFY(window_->quickOpenVisible());
        QCOMPARE(window_->editorCount(), 1);

        // The file vanished after the list: false + the openFile truth
        // on the bar, the popup stays, the tab count stands:
        QVERIFY(QFile::remove(temp_->filePath(QStringLiteral("x.py"))));
        QVERIFY(!window_->activateQuickOpen(0));
        QVERIFY(window_->quickOpenVisible());
        QCOMPARE(window_->editorCount(), 1);
        QVERIFY(window_->statusBar()->currentMessage().contains(
            QStringLiteral("\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c")));
        // …and the OTHER row still works (the panel is not dead):
        QVERIFY(window_->activateQuickOpen(1)); // one-tab law: y re-Focuses
        QCOMPARE(window_->editorCount(), 1);
        QVERIFY(!window_->quickOpenVisible());
    }

    void quickOpenRefusalIsLoud()
    {
        // NO project ever opened (fresh window_ from init): the quick-open
        // refuses with words, never a silent zero-hit list (the search
        // panel's law, sibling face).
        window_->showQuickOpen();
        QCOMPARE(window_->quickOpenRowCount(), 0);
        QCOMPARE(window_->quickOpenPathAt(0), QString());
        QVERIFY(window_->quickOpenSummary().contains(
            QStringLiteral("\u043e\u0442\u043a\u0440\u043e\u0439\u0442\u0435"))); // "open a project first"
    }

    void quickOpenTruncationIsVisible()
    {
        for (int i = 0; i < 60; ++i) // past the row cap ON PURPOSE: the
            writeFile(QStringLiteral("f%1.py").arg(i, 3, 10, QLatin1Char('0')),
                      "x\n");       // cap must SHOW itself (skip-honesty)
        window_->openProject(temp_->path());
        QVERIFY2(waitUntil([this] { return !window_->projectFilesWalking(); }),
                 "the pool walk never finished");
        QCOMPARE(window_->projectFileCount(), 60);

        window_->showQuickOpen(); // empty needle: walk order, capped
        QCOMPARE(window_->quickOpenRowCount(), IdeWindow::kQuickOpenMaxRows);
        QVERIFY(window_->quickOpenSummary().contains(
            QStringLiteral("\u041f\u043e\u043a\u0430\u0437\u0430\u043d\u044b \u043f\u0435\u0440\u0432\u044b\u0435 50"))); // "first 50 shown"
        QVERIFY(window_->quickOpenSummary().contains(QStringLiteral("60")));

        window_->typeQuickOpenQuery(QStringLiteral("f")); // ALL 60 embed
        QCOMPARE(window_->quickOpenRowCount(), IdeWindow::kQuickOpenMaxRows);
        QVERIFY(window_->quickOpenSummary().contains(
            QStringLiteral("\u0441\u043a\u0440\u044b\u0442\u043e"))); // "hidden [10 more]"
        QVERIFY(window_->quickOpenSummary().contains(QStringLiteral("10")));
    }

    void poolReplacesOnProjectSwitchSeam()
    {
        // The cancel→restart seam: two openProject calls WITHOUT an
        // event-loop beat between them. The first walk is cancelled (its
        // files never reach the pool), the restart lands in onPoolFinished
        // (the engine retired walk_ before the emit — the sanctioned
        // restart face). The final pool is project B's set, WHOLE and
        // ONLY B's.
        for (int i = 0; i < 2; ++i)
            writeFile(QStringLiteral("A/f%1.py").arg(i), "x\n");
        for (int i = 0; i < 3; ++i)
            writeFile(QStringLiteral("B/g%1.py").arg(i), "x\n");
        const QString aRoot = temp_->filePath(QLatin1String("A"));
        const QString bRoot = temp_->filePath(QLatin1String("B"));

        window_->openProject(aRoot); // starts the walk
        window_->openProject(bRoot); // mid-walk: pending restart, no
                                     // second session, no leak
        QVERIFY2(waitUntil([this] { return !window_->projectFilesWalking(); }),
                 "the replaced pool walk never finished");
        QCOMPARE(window_->projectFileCount(), 3);
        window_->showQuickOpen();
        QCOMPARE(window_->quickOpenRowCount(), 3);
        for (int r = 0; r < 3; ++r)
            QVERIFY(window_->quickOpenPathAt(r).startsWith(bRoot));
    }

    void paletteRegistryFilterAndRun()
    {
        window_->showCommandPalette();
        QCOMPARE(window_->commandRowCount(), 4); // the built-ins,
        QCOMPARE(window_->commandLabelAt(0).contains(
            QStringLiteral("\u0411\u044b\u0441\u0442\u0440\u043e\u0435 \u043e\u0442\u043a\u0440\u044b\u0442\u0438\u0435")), true); // REGISTRY
        QCOMPARE(window_->commandLabelAt(1).contains(
            QStringLiteral("\u041f\u0430\u043b\u0438\u0442\u0440\u0430")), true);      // ORDER for the
        QCOMPARE(window_->commandLabelAt(3).contains(
            QStringLiteral("\u0417\u0430\u043a\u0440\u044b\u0442\u044c")), true);      // empty needle
        QCOMPARE(window_->commandLabelAt(99), QString()); // out of range

        window_->typePaletteQuery(QStringLiteral("\u043f\u0430\u043b\u0438\u0442\u0440")); // "palitr" (lower
        QCOMPARE(window_->commandRowCount(), 1);                                        // folds onto the
        QVERIFY(window_->commandLabelAt(0).contains(                                     // capital П)
            QStringLiteral("\u041f\u0430\u043b\u0438\u0442\u0440\u0430")));
        window_->typePaletteQuery(QString()); // back to registry order
        QCOMPARE(window_->commandRowCount(), 4);

        // Run the CLOSE-TAB command through the palette (real effect —
        // the registry is not a poster):
        writeFile(QStringLiteral("a.py"), "x\n");
        window_->openProject(temp_->path());
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("a.py"))));
        QCOMPARE(window_->editorCount(), 1);
        QVERIFY(window_->runPaletteCommand(3)); // "Закрыть активную
        QCOMPARE(window_->editorCount(), 0);    // вкладку" at registry
        QVERIFY(!window_->commandPaletteVisible()); // row 3 — overlay
                                                     // closed BEFORE run
        // The close→run ORDER face: the quick-open command run from the
        // palette leaves the PALETTE hidden and QUICK-OPEN up — a
        // post-run hide would have killed it:
        window_->showCommandPalette();
        QVERIFY(window_->runPaletteCommand(0));
        QVERIFY(!window_->commandPaletteVisible());
        QVERIFY(window_->quickOpenVisible());
        // Out of range: false and NOTHING ran:
        window_->closeOverlay();
        window_->showCommandPalette();
        QVERIFY(!window_->runPaletteCommand(99));
        QCOMPARE(window_->commandRowCount(), 4); // rows intact
        QVERIFY(window_->commandPaletteVisible()); // palette stays
    }

    void paletteChordsRideTheWindowNotTheView()
    {
        // The chord-table audit as a TRIPWIRE (the I2-stub law): the IDE's
        // palette chords live on the WINDOW (QShortcut, WindowShortcut)
        // while the VIEW's frozen keymap never sees them — the editor
        // keeps focus, the document stays byte-identical, and the tabs
        // are untouched. (The chords the VIEW binds — Ctrl+A/C/V/X/Z/Y/D,
        // Tab/BackTab, Escape, F7/Shift+F7, the nav set — contain NO
        // Ctrl+P / Ctrl+Shift+P / Ctrl+W: this slot IS the mask face.)
        writeFile(QStringLiteral("a.py"), "keepme\n");
        window_->openProject(temp_->path());
        QVERIFY2(waitUntil([this] { return !window_->projectFilesWalking(); }),
                 "the pool walk never finished");
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("a.py"))));
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        const QString textBefore = editor->toPlainText();
        editor->setFocus();
        QTest::qWait(20);

        QTest::keyClick(editor, Qt::Key_P, Qt::ControlModifier);
        QVERIFY(window_->quickOpenVisible());
        QVERIFY(!window_->commandPaletteVisible());
        QCOMPARE(editor->toPlainText(), textBefore); // the view typed NOTHING
        QCOMPARE(editor, window_->currentEditor());  // and stole no focus

        window_->closeOverlay();
        QTest::keyClick(editor, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(window_->commandPaletteVisible());
        QVERIFY(!window_->quickOpenVisible());
        QCOMPARE(editor->toPlainText(), textBefore);

        window_->closeOverlay();
        QTest::keyClick(editor, Qt::Key_W, Qt::ControlModifier);
        QCOMPARE(window_->editorCount(), 0); // the close-tab chord runs
                                             // through the SAME command
                                             // body (deleteLater has not
                                             // spun — no touching the
                                             // dying editor beyond this)
        // Esc closes the popup while ITS subtree has focus (child-scoped
        // shortcut — the view's frozen Escape stays the editor's own):
        window_->showQuickOpen();
        QVERIFY(window_->quickOpenVisible());
        QTest::qWait(20); // let the focus law settle offscreen
        QWidget* focused = QApplication::focusWidget();
        QVERIFY2(focused != nullptr, "the overlay edit did not take focus");
        QTest::keyClick(focused, Qt::Key_Escape);
        QVERIFY(!window_->quickOpenVisible());
    }

    // --------------------------------------------- the --lsp door (I0)

    void lspDoorOffByDefaultNeverSpawns()
    {
        // The tst_demo law rides whole (AGENTS: clangd is not a CI
        // guarantee): this is the ONE ide slot touching the hub, and it
        // only opens languages NO built-in spec claims — python and txt
        // stay plain editors even with the hub armed, so NOTHING spawns.
        window_->openProject(temp_->path());
        QVERIFY(!window_->lspEnabled());
        writeFile(QStringLiteral("plain.txt"), "x");
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("plain.txt"))));
        QVERIFY(!window_->lspEnabled()); // openFile never turns the door on

        window_->setLspEnabled(true);
        QVERIFY(window_->lspEnabled());
        writeFile(QStringLiteral("unclaimed.py"), "x = 1\n");
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("unclaimed.py"))));
        QCOMPARE(window_->editorCount(), 2);
        window_->closeTabAt(0);
        QCOMPARE(window_->editorCount(), 1);
        window_->setLspEnabled(false);
        QVERIFY(!window_->lspEnabled());
        window_->setLspEnabled(true); // idempotent both ways: never a
        window_->setLspEnabled(true); // second hub, never an early death
        window_->setLspEnabled(false);
        QVERIFY(!window_->lspEnabled());
    }
};

QTEST_MAIN(TestIde) // QTEST_MAIN (widget face, the tst_glue/tst_demo
                    // class): ctest runs it offscreen
#include "tst_ide.moc"
