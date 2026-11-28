// Drives the qmn-ide-demo app (GATE 4 car I0 skeleton + car I1 panel + car
// I2 quick-open/palette + car I3 LSP depth: definition, references, rename,
// code actions, formatting, the outline dock) fully offscreen through the
// SAME programmatic doors the dock widgets, popups and panel buttons call —
// no dialog exec, no file dialogs. THE SPAWN FACE (owner grant, 2026-11-28,
// PLAN's I3 opening record): the I3 depth drills DO spawn a server — the
// CI-built deterministic `fakeserver` via $<TARGET_FILE:fakeserver>, the
// dapfake class tst_demo's --dap slot already rides, its exit code the
// far-side oracle — armed by the window's own `armLspServer` door UNDER THE
// BUILT-IN SPEC NAME ("clangd" replaced whole), so PATH clangd is still
// NEVER spawned here (clangd is no CI guarantee; probe_clangd.sh stays its
// ritual). Every OTHER slot keeps the I0 law whole: a hub slot without an
// explicit spec path opens only UNCLAIMED languages. The real-user
// `.qmn/lsp.json` fold leg is pinned end-to-end once.
// tst_ide never includes a glue, lsp or search header: IdeWindow.hpp keeps
// those types behind forward-decls, and the window's own read doors
// (searchSummary/outline*/jump*/rename*/format*/codeAction*/resultsProducer/
// searchResult*) are the observation channel. Assertions are on model/widget
// state, never on pixels.

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

QString i3MarkerSource()
{
    // The fake's marker protocol (see tests/fakeserver's
    // featureDocumentSymbol law): a Class with two children, one
    // EOF-CLOSED root Function, one marker-free line. The outline
    // witness below is derived from that protocol BY HAND and byte-pinned
    // — if fake and test ever drift, this string drifts LOUDLY.
    return QStringLiteral(
        "SYM cl Widget | class Widget\n"
        "  SYM mth paint | void paint(Gfx*)\n"
        "  END\n"
        "  SYM fld count | int count\n"
        "  END\n"
        "END\n"
        "SYM fn helper | int helper()\n"
        "plain text line\n");
}

// The spawn arm, in one place: an explicit program path + the far side's
// flags + the language it claims. THE TENTH PROVIDER RIDES THE DEFAULT ARM
// on purpose: an outline ask cycle that FINISHED is the window's own proof
// that a live server ANSWERED, and that is the readiness gate every depth
// drill needs — the belt-and-braces alternative is polling a dozen feature
// summaries for "not sent yet", which is what the first drafts did and what
// made the wire look red when it was merely young. (A NEGATIVE CONTROL at
// the close proved the legs are load-bearing: with the advertise flag gone
// the hub's door refuses LOCALLY, -32601 capability "documentSymbolProvider"
// is not offered, and the outline drill fails.) The spec name "clangd" REPLACES the
// built-in claim whole (setServerSpec replaces the whole record) — the only
// deterministic arm over c/cpp, and PATH clangd stays UNSPAWNED, which is
// the I0 law intact. NO .qmn/lsp.json here (that real-user path gets its own
// slot).
void armTheFake(IdeWindow* window, const QStringList& extraFlags)
{
    window->armLspServer(QStringLiteral("clangd"),
                         QStringLiteral(QMN_TEST_LSP_SERVER),
                         QStringList{QStringLiteral("sync"),
                                     QStringLiteral("utf-16"),
                                     QStringLiteral("features"),
                                     QStringLiteral("documentsymbol")}
                             + extraFlags,
                         {QStringLiteral("cpp")});
}

// THE WINDOW'S OWN READINESS GATE: an outline ask cycle that FINISHED means
// a live server ANSWERED (an empty-symbol answer is still an answer). An
// empty summary / "не подключён" / "поднимается" / "запрошены" all mean
// not-yet. The drill gates ride this — no slot sleeps on a hope, and the
// tenth door is the gate exactly because it is the window's own ask.
bool outlineCycleFinished(IdeWindow* window)
{
    const QString s = window->outlineSummary();
    return !s.isEmpty()
        && !s.contains(QStringLiteral(
            "LSP-\u0441\u0435\u0440\u0432\u0435\u0440 \u043d\u0435 "
            "\u043f\u043e\u0434\u043a\u043b\u044e\u0447\u0451\u043d"))
        && !s.contains(QStringLiteral(
            "\u043f\u043e\u0434\u043d\u0438\u043c\u0430\u0435\u0442\u0441\u044f"))
        && !s.contains(QStringLiteral(
            "\u0437\u0430\u043f\u0440\u043e\u0448\u0435\u043d\u044b"));
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
        QCOMPARE(window_->commandRowCount(), 10); // the built-ins — car I3
        // appended SIX LSP commands at the END of the frozen registry order
        // (rows 0..3 below are the I2 four, unmoved: an append never
        // disturbs the tie law's registry order)
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
        QCOMPARE(window_->commandRowCount(), 10);

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
        QCOMPARE(window_->commandRowCount(), 10); // rows intact (I3's six
                                                  // appended commands ride
                                                  // this face too)
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
    // ============================================ GATE 4 car I3: the LSP
    // depth drills. THE SPAWN GRANT (owner decision, 2026 11 28, recorded
    // in PLAN's I3 opening record): these slots DO spawn a server — the
    // deterministic CI-built `fakeserver`, the dapfake class tst_demo's
    // --dap slot already rides ($<TARGET_FILE:fakeserver>, a build-dependency
    // of this target, its exit code the far-side oracle). That grant does
    // NOT touch the I0 law for every OTHER slot: no slot without an explicit
    // spec path opens a claimed language, and PATH clangd is never spawned
    // here (clangd is no CI guarantee). The arm door is IdeWindow's own
    // (armLspServer) — the same hub/session path a .qmn/lsp.json fold builds,
    // spelled programmatically; the JSON fold leg is pinned by the real-user
    // path in i3ConfigFoldLegArmsTheSameServerFromDisk.
    //
    // What these slots pin is the WINDOW: the chords reach the window and
    // not the view, the answers land in the panel/outline/summaries the docks
    // read, the reveal faces carry their caret law, and refusals are LOUD and
    // local. The wire itself is tst_lsp's turf.

    void i3OutlineDrillReadsTheMarkerTreeFarSide()
    {
        writeFile(QStringLiteral("proj/a.cpp"),
                  i3MarkerSource().toUtf8()); // the fixture is ASCII: the
                                              // byte view IS the model view
        window_->openProject(temp_->filePath(QStringLiteral("proj")));
        armTheFake(window_.get(), {});
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("proj/a.cpp"))));
        // THE STILL-STARTING FACE is part of this slot: the first ask fires
        // while the handshake is still running (openFile beats the Ready
        // door), the outline SAYS "сервер ещё поднимается", and the bounded
        // re-arm lands the tree seconds later — no user retry, no stale tree.
        QVERIFY2(waitUntil([this] { return window_->outlineRowCount() == 4; }),
                 qPrintable(window_->outlineSummary()));
        QCOMPARE(window_->outlineTextAt(0),
                 QStringLiteral("class Widget \u00b7 class Widget"));
        QCOMPARE(window_->outlineTextAt(1),
                 QStringLiteral("method paint \u00b7 void paint(Gfx*)"));
        QCOMPARE(window_->outlineTextAt(2),
                 QStringLiteral("field count \u00b7 int count"));
        QCOMPARE(window_->outlineTextAt(3),
                 QStringLiteral("function helper \u00b7 int helper()"));
        QCOMPARE(window_->outlineTextAt(4), QString()); // out of range is
                                                        // empty, never a
                                                        // wrap-around
        // NESTING, the far side's own (the fake nests by the marker stack):
        QCOMPARE(window_->outlineParentAt(0), -1); // Widget: a root
        QCOMPARE(window_->outlineParentAt(1), 0);  // paint under Widget
        QCOMPARE(window_->outlineParentAt(2), 0);  // count under Widget
        QCOMPARE(window_->outlineParentAt(3), -1); // helper: its own root
        QCOMPARE(window_->outlineParentAt(99), -2); // out of range is its
                                                    // own face
        QVERIFY2(window_->outlineSummary().contains(
                     QStringLiteral("\u00b7 \u0438\u0435\u0440\u0430\u0440\u0445\u0438\u044f")),
                 qPrintable(window_->outlineSummary())); // the hierarchy
                                                         // shape, said out
        // The activation face: caret at the token END (the panel's face; a
        // JUMP is the START face — the two are pinned apart).
        QVERIFY(window_->outlineActivate(1));
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        QCOMPARE(editor->position(), (TextPosition{1, 15}));
        QCOMPARE(editor->selectedText(), QStringLiteral("paint"));
        QVERIFY(!window_->outlineActivate(9)); // out of range: false, no
                                               // reveal, no crash
        // THE EDIT-DEBOUNCE LEG of the refresh law (the policy is the
        // outline's: the tenth door never auto-pulls): an edit re-arms the
        // app-side timer and the ask that follows answers about the NEW text
        // — the newest ask winning is also the generation guard's visible
        // face (a late answer about the old text would leave FOUR rows).
        editor->insertTextAt(TextPosition{0, 0},
                             QStringLiteral("SYM var counter | int counter\n"
                                            "END\n"));
        QVERIFY2(waitUntil([this] { return window_->outlineRowCount() == 5; }),
                 qPrintable(window_->outlineSummary()));
        QCOMPARE(window_->outlineTextAt(0),
                 QStringLiteral("variable counter \u00b7 int counter"));
        QCOMPARE(window_->outlineParentAt(1), -1); // Widget: still a root,
                                                   // now at row 1
        QCOMPARE(window_->outlineParentAt(2), 1);  // paint under Widget
        QCOMPARE(window_->outlineParentAt(4), -1); // helper, EOF-closed root
    }

    void i3OutlineFlatAnswerRebuildsByContainerName()
    {
        // The sloppy-server class DESPITE our hierarchicalDocumentSymbolSupport
        // claim: SymbolInformation[] + containerName. tst_lsp pins the door
        // carries that whole shape; THIS slot pins the app-side REBUILD —
        // same four rows, same parents, and the summary ADMITS the rebuild
        // (the skip-honesty law: the user is told the shape was not given).
        writeFile(QStringLiteral("proj/a.cpp"), i3MarkerSource().toUtf8());
        window_->openProject(temp_->filePath(QStringLiteral("proj")));
        armTheFake(window_.get(), {QStringLiteral("documentsymbol"), QStringLiteral("symflat")});
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("proj/a.cpp"))));
        QVERIFY2(waitUntil([this] { return window_->outlineRowCount() == 4; }),
                 qPrintable(window_->outlineSummary()));
        QCOMPARE(window_->outlineTextAt(0), QStringLiteral("class Widget"));
        QCOMPARE(window_->outlineTextAt(1), QStringLiteral("method paint"));
        QCOMPARE(window_->outlineTextAt(3), QStringLiteral("function helper"));
        QCOMPARE(window_->outlineParentAt(1), 0);
        QCOMPARE(window_->outlineParentAt(2), 0);
        QCOMPARE(window_->outlineParentAt(3), -1);
        QVERIFY2(window_->outlineSummary().contains(
                     QStringLiteral("containerName")),
                 qPrintable(window_->outlineSummary()));
        // THE NAMESAKE LEG of the rebuild law ("the LAST earlier row of that
        // name adopts"): the fake's containerName IS the parent's NAME, so two
        // nested Widgets make the child's "Widget" ambiguous. The flat list is
        // preorder, so the last earlier namesake is the INNER Widget — which is
        // exactly the innermost containing symbol. A first-match rule would
        // nest the child one level too high and fail HERE. (Second document,
        // so the ask is fresh and the generation guard is not in play.)
        writeFile(QStringLiteral("proj/b.cpp"),
                  "SYM cl Widget | class Widget\n"
                  "  SYM cl Widget | class Widget\n"
                  "    SYM mth deep | void deep()\n"
                  "    END\n"
                  "  END\n"
                  "END\n");
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("proj/b.cpp"))));
        QVERIFY2(waitUntil([this] { return window_->outlineRowCount() == 3; }),
                 qPrintable(window_->outlineSummary()));
        QCOMPARE(window_->outlineTextAt(1), QStringLiteral("class Widget"));
        QCOMPARE(window_->outlineParentAt(1), 0); // inner Widget rides the outer
        QCOMPARE(window_->outlineTextAt(2), QStringLiteral("method deep"));
        QCOMPARE(window_->outlineParentAt(2), 1); // the tie-break: LAST earlier,
                                                  // the inner one
        QCOMPARE(window_->outlineParentAt(0), -1);
    }

    void i3DefinitionJumpSelectsTheTokenWithCaretAtStart()
    {
        writeFile(QStringLiteral("proj/a.cpp"),
                  "alpha beta alpha\n  beta tail\n");
        window_->openProject(temp_->filePath(QStringLiteral("proj")));
        armTheFake(window_.get(), {});
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("proj/a.cpp"))));
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        editor->setPosition(TextPosition{0, 6}); // the caret lands on "beta"
        QVERIFY2(waitUntil([this] {
                     return window_->jumpToDefinition();
                 }),
                 qPrintable(window_->jumpSummary())); // the ask can only
                                                      // cross once the server
                                                      // is Ready — waiting
                                                      // for the ask to be
                                                      // SENT is the honest
                                                      // face of a live wire
        QVERIFY2(waitUntil([this] {
                     return window_->jumpSummary().startsWith(
                         QStringLiteral("\u043f\u0435\u0440\u0435\u0445\u043e\u0434: a.cpp"));
                 }),
                 qPrintable(window_->jumpSummary()));
        // THE JUMP'S CARET FACE: the selection covers the token and the
        // CARET sits at its START (the panel/outline reveal is the END face —
        // both ride the ONE public setSelection door, the I1 widening).
        QCOMPARE(editor->position(), (TextPosition{0, 6}));
        QCOMPARE(editor->selectedText(), QStringLiteral("beta"));
        QCOMPARE(editor->toPlainText(),
                 QStringLiteral("alpha beta alpha\n  beta tail\n")); // a jump
                                                                     // moves
                                                                     // the
                                                                     // caret,
                                                                     // never
                                                                     // the
                                                                     // text
    }

    void i3ReferencesFillTheSamePanelWithFileMatchRows()
    {
        writeFile(QStringLiteral("proj/a.cpp"), "alpha beta alpha\n\ngamma alpha\n");
        window_->openProject(temp_->filePath(QStringLiteral("proj")));
        armTheFake(window_.get(), {});
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("proj/a.cpp"))));
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        editor->setPosition(TextPosition{0, 0}); // the caret on the FIRST
                                                 // "alpha" (the fake's own
                                                 // declaration)
        QVERIFY2(waitUntil([this] {
                     return outlineCycleFinished(window_.get());
                 }),
                 qPrintable(window_->outlineSummary())); // the Readiness gate
        QVERIFY(window_->findReferences());
        QVERIFY2(waitUntil([this] { return window_->searchResultRowCount() == 3; }),
                 qPrintable(window_->searchSummary()));
        QCOMPARE(window_->resultsProducer(), QStringLiteral("references"));
        // The SERVER's order is the row order, grouped per file in first-
        // appearance order (the panel's I1 honesty, the second producer
        // riding it unchanged):
        const QString target = temp_->filePath(QStringLiteral("proj/a.cpp"));
        for (int row = 0; row < 3; ++row)
            QCOMPARE(window_->searchResultFileAt(row), target);
        QCOMPARE(window_->searchResultLineAt(0), 0);
        QCOMPARE(window_->searchResultLineAt(1), 0);
        QCOMPARE(window_->searchResultLineAt(2), 2);
        // A reference row activates through the SAME door as a search row —
        // the row currency is the engine's own FileMatch/LineMatch:
        QVERIFY(window_->activateSearchResult(2));
        QCOMPARE(editor->position(), (TextPosition{2, 11})); // caret at the
                                                             // END of the hit
        QCOMPARE(editor->selectedText(), QStringLiteral("alpha"));
        QVERIFY2(window_->searchSummary().contains(
                     QStringLiteral("\u0421\u0441\u044b\u043b\u043e\u043a: 3")),
                 qPrintable(window_->searchSummary()));
        // The producer switch back: a fresh search owns the panel again.
        window_->runSearch(QStringLiteral("beta"), false, false, false);
        QCOMPARE(window_->resultsProducer(), QStringLiteral("search"));
        QVERIFY2(waitUntil([this] { return window_->searchResultRowCount() == 1; }),
                 qPrintable(window_->searchSummary()));
    }

    void i3RenameAppliesOneUndoGroupAndRefusesWorkspaceShapes()
    {
        writeFile(QStringLiteral("proj/a.cpp"), "alpha beta alpha\n\ngamma alpha\n");
        window_->openProject(temp_->filePath(QStringLiteral("proj")));
        armTheFake(window_.get(), {});
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("proj/a.cpp"))));
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        editor->setPosition(TextPosition{0, 2}); // inside the first "alpha"
        // The prefill law: no selection → the word AT the caret, app-side math
        // over the model (no new view door).
        QVERIFY(window_->openRenameOverlay());
        QVERIFY(window_->renameOverlayVisible());
        QCOMPARE(window_->renamePrefill(), QStringLiteral("alpha"));
        const QString before = editor->toPlainText();
        QVERIFY2(waitUntil([this] {
                     return outlineCycleFinished(window_.get());
                 }),
                 qPrintable(window_->outlineSummary())); // the Readiness gate
        window_->commitRename(QStringLiteral("zeta"));
        QVERIFY2(waitUntil([this] {
                     return window_->renameSummary().contains(
                         QStringLiteral("\u043f\u0440\u0438\u043c\u0435\u043d\u0435\u043d\u043e"));
                 }),
                 qPrintable(window_->renameSummary()));
        QCOMPARE(editor->toPlainText(),
                 QStringLiteral("zeta beta zeta\n\ngamma zeta\n")); // ALL
                                                                    // three
                                                                    // hits,
                                                                    // the
                                                                    // fake's
                                                                    // WorkspaceEdit
        QVERIFY(!window_->renameOverlayVisible()); // success CLOSES the
                                                   // overlay
        // ONE undo group per document (the applier's frozen law, pinned here
        // through the app's door): ONE undo restores the WHOLE rename.
        editor->undo();
        QCOMPARE(editor->toPlainText(), before);
        // The refusal face keeps the overlay OPEN (the next name may yet be
        // the truth) and a blank name never goes on the wire at all:
        QVERIFY(window_->openRenameOverlay());
        window_->commitRename(QStringLiteral("   "));
        QVERIFY2(window_->renameSummary().contains(
                     QStringLiteral("\u043f\u0443\u0441\u0442\u043e\u0435 \u0438\u043c\u044f")),
                 qPrintable(window_->renameSummary()));
        QVERIFY(window_->renameOverlayVisible());
        window_->closeOverlay();
        // The workspace-shape refusal: the fake's RENAMEMULTI answer edits a
        // document this hub does not hold open, so the applier refuses ALL of
        // it — the current document's own edits included, which is exactly
        // what makes a half-applied rename impossible.
        writeFile(QStringLiteral("proj/multi.cpp"), "alpha RENAMEMULTI\nalpha\n");
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("proj/multi.cpp"))));
        CodeEditor* multi = window_->currentEditor();
        QVERIFY(multi);
        multi->setPosition(TextPosition{0, 2});
        QVERIFY2(waitUntil([this] {
                     return outlineCycleFinished(window_.get());
                 }),
                 qPrintable(window_->outlineSummary())); // the SECOND document
                                                         // opened: its outline
                                                         // cycle proves the
                                                         // didOpen reached the
                                                         // same live client
        const QString multiBefore = multi->toPlainText();
        window_->commitRename(QStringLiteral("omega"));
        QVERIFY2(waitUntil([this] {
                     return window_->renameSummary().contains(
                         QStringLiteral("\u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u043e"));
                 }),
                 qPrintable(window_->renameSummary()));
        QCOMPARE(multi->toPlainText(), multiBefore); // NOTHING half-applied
    }

    void i3FormattingRunsThroughTheApplier()
    {
        writeFile(QStringLiteral("proj/a.cpp"), "head line\ntail line\n");
        window_->openProject(temp_->filePath(QStringLiteral("proj")));
        armTheFake(window_.get(), {});
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("proj/a.cpp"))));
        QVERIFY2(waitUntil([this] {
                     return outlineCycleFinished(window_.get());
                 }),
                 qPrintable(window_->outlineSummary())); // the Readiness gate
        QVERIFY(window_->formatDocument());
        QVERIFY2(waitUntil([this] {
                     return window_->formatSummary().contains(
                         QStringLiteral("\u043f\u0440\u0438\u043c\u0435\u043d\u0435\u043d\u043e"));
                 }),
                 qPrintable(window_->formatSummary()));
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        QVERIFY2(editor->toPlainText().startsWith(QStringLiteral("# fmt\n")),
                 qPrintable(editor->toPlainText()));
        QVERIFY2(editor->toPlainText().endsWith(QStringLiteral(" // fmt")),
                 qPrintable(editor->toPlainText()));
    }

    void i3CodeActionsRowsRunOrRefuseThroughTheApplier()
    {
        writeFile(QStringLiteral("proj/a.cpp"), "head line\ntail line\n");
        window_->openProject(temp_->filePath(QStringLiteral("proj")));
        armTheFake(window_.get(), {});
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("proj/a.cpp"))));
        QVERIFY2(waitUntil([this] {
                     return outlineCycleFinished(window_.get());
                 }),
                 qPrintable(window_->outlineSummary())); // the Readiness gate
        QVERIFY(window_->showCodeActions());
        QVERIFY2(waitUntil([this] { return window_->codeActionRowCount() == 4; }),
                 qPrintable(window_->codeActionsSummary()));
        // The fake's four shapes, in the answer's order (the row order is the
        // SERVER's, the panel law again):
        QCOMPARE(window_->codeActionLabelAt(0), QStringLiteral("indent the head"));
        QCOMPARE(window_->codeActionLabelAt(1), QStringLiteral("append the tail"));
        QCOMPARE(window_->codeActionLabelAt(2), QStringLiteral("create a sidecar"));
        QCOMPARE(window_->codeActionLabelAt(3), QStringLiteral("nothing to apply"));
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        // Row 1 FIRST (clangd's hidden-in-command shape) because the pinned
        // sequence never replays a stale range: rows 1 and 0 anchor at the
        // document's two OPPOSITE ends, so either order is drift-free — and
        // the sequence below says which one, LOUDLY, instead of leaving it to
        // a lucky accident.
        QVERIFY(window_->runCodeAction(1));
        QVERIFY2(editor->toPlainText().endsWith(QStringLiteral(" // cmd")),
                 qPrintable(editor->toPlainText()));
        // v1 never executes a server's arbitrary command and never creates a
        // file: BOTH non-applicable shapes refuse through the APPLIER, with
        // its reasons riding the summary verbatim (the honest VISIBLE row is
        // the point — the user sees the offer and reads why it did not run).
        QVERIFY(!window_->runCodeAction(2));
        QVERIFY2(window_->codeActionsSummary().contains(
                     QStringLiteral("\u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u043e")),
                 qPrintable(window_->codeActionsSummary()));
        const QString afterRow1 = editor->toPlainText();
        QVERIFY(!window_->runCodeAction(3));
        QCOMPARE(editor->toPlainText(), afterRow1);
        QVERIFY(!window_->runCodeAction(99)); // out of range: false, and
                                              // nothing ran
    }

    void i3NewChordsRideTheWindowNotTheViewAndRefuseLocally()
    {
        // The chord-mask tripwire GROWS (the I2 law, five more legs): with NO
        // hub raised the five new chords reach the WINDOW, every door refuses
        // LOUDLY and LOCALLY (no server can spawn — no spec, no wire), and
        // the document stays byte-identical.
        writeFile(QStringLiteral("a.cpp"), "alpha\n");
        window_->openProject(temp_->path());
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("a.cpp"))));
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        const QString textBefore = editor->toPlainText();
        editor->setFocus();
        QTest::qWait(20);

        QTest::keyClick(editor, Qt::Key_F12);
        QVERIFY2(window_->jumpSummary().contains(
                     QStringLiteral("LSP")),
                 qPrintable(window_->jumpSummary()));
        QTest::keyClick(editor, Qt::Key_F12, Qt::ShiftModifier);
        QVERIFY(window_->resultsProducer().isEmpty()); // the references door
                                                       // refused BEFORE it
                                                       // ever claimed the
                                                       // panel
        QTest::keyClick(editor, Qt::Key_F2);
        QVERIFY(!window_->renameOverlayVisible()); // no hub: the overlay
                                                   // never opens
        QTest::keyClick(editor, Qt::Key_F, Qt::ShiftModifier | Qt::AltModifier);
        QVERIFY2(window_->formatSummary().contains(
                     QStringLiteral("LSP")),
                 qPrintable(window_->formatSummary()));
        QTest::keyClick(editor, Qt::Key_Period, Qt::ControlModifier);
        QCOMPARE(window_->codeActionRowCount(), 0);
        QCOMPARE(editor->toPlainText(), textBefore); // the VIEW typed
                                                     // NOTHING through any
                                                     // of the five chords
        QCOMPARE(editor, window_->currentEditor());
        QVERIFY(window_->outlineRowCount() == 0); // the outline dock exists
                                                  // and is honestly empty
        QVERIFY2(window_->outlineSummary().contains(
                     QStringLiteral("LSP")),
                 qPrintable(window_->outlineSummary()));
    }

    void i3LspOffRetiresTheOutlineAndEverySummary()
    {
        writeFile(QStringLiteral("proj/a.cpp"), i3MarkerSource().toUtf8());
        window_->openProject(temp_->filePath(QStringLiteral("proj")));
        armTheFake(window_.get(), {QStringLiteral("documentsymbol")});
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("proj/a.cpp"))));
        QVERIFY2(waitUntil([this] { return window_->outlineRowCount() == 4; }),
                 qPrintable(window_->outlineSummary()));
        window_->setLspEnabled(false);
        // A stale tree never poses as current: the key going DOWN empties
        // the dock LOUDLY and retires every feature summary with it.
        QCOMPARE(window_->outlineRowCount(), 0);
        QVERIFY2(window_->outlineSummary().contains(
                     QStringLiteral("LSP-\u0441\u0435\u0440\u0432\u0435\u0440 "
                                    "\u043d\u0435 "
                                    "\u043f\u043e\u0434\u043a\u043b\u044e\u0447\u0451\u043d")),
                 qPrintable(window_->outlineSummary()));
        const QString retired
            = QStringLiteral("LSP-\u043a\u043b\u044e\u0447 \u043e\u043f\u0443\u0448\u0435\u043d");
        QVERIFY2(window_->jumpSummary().contains(retired),
                 qPrintable(window_->jumpSummary()));
        QVERIFY2(window_->renameSummary().contains(retired),
                 qPrintable(window_->renameSummary()));
        QVERIFY2(window_->formatSummary().contains(retired),
                 qPrintable(window_->formatSummary()));
        QVERIFY2(window_->codeActionsSummary().contains(retired),
                 qPrintable(window_->codeActionsSummary()));
        // And the outline's re-ask after the key is down stays EMPTY (no
        // wire to ask on, no crash, no phantom rows):
        window_->refreshOutline();
        QCOMPARE(window_->outlineRowCount(), 0);
    }


    void i3DeadSpecWaitsBoundedlyThenRefusesLoudly()
    {
        // THE BOUNDED FACE of the outline's still-starting law: a spec whose
        // program cannot spawn is indistinguishable from a slow server at the
        // hub's door (the same -32002 no-live-server refusal), so the outline
        // WAITS — bounded at 10 tries — says so at every try, and then tells
        // the plain refusal. No spin, no phantom rows, no crash, and the next
        // ask after the cap refuses straight away (the wait is not re-litigated
        // per keystroke).
        writeFile(QStringLiteral("proj/a.cpp"), "int main() { return 0; }\n");
        window_->openProject(temp_->filePath(QStringLiteral("proj")));
        window_->armLspServer(QStringLiteral("clangd"),
                              temp_->filePath(QStringLiteral("no-such-program")),
                              {QStringLiteral("sync")}, {QStringLiteral("cpp")});
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("proj/a.cpp"))));
        QVERIFY2(waitUntil([this] {
                     return window_->outlineSummary().contains(
                         QStringLiteral("\u043f\u043e\u043f\u044b\u0442\u043a\u0430 10"));
                 }, 12000),
                 qPrintable(window_->outlineSummary())); // the cap: ten tries,
                                                         // out loud
        QCOMPARE(window_->outlineRowCount(), 0);
        QVERIFY2(waitUntil([this] {
                     return window_->outlineSummary().contains(
                         QStringLiteral("\u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u044b"));
                 }, 5000),
                 qPrintable(window_->outlineSummary())); // the plain refusal
                                                         // face arrives
        QCOMPARE(window_->outlineRowCount(), 0);
        const QString refused = window_->outlineSummary();
        window_->refreshOutline(); // past the cap: the SAME refusal, with no
                                   // second bounded wait
        QVERIFY2(window_->outlineSummary().contains(
                     QStringLiteral("\u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u044b")),
                 qPrintable(window_->outlineSummary()));
        QVERIFY(!window_->outlineSummary().contains(
            QStringLiteral("\u043f\u043e\u043f\u044b\u0442\u043a\u0430")));
        QCOMPARE(window_->outlineSummary(), refused); // past the cap the SAME
                                                      // refusal, verbatim: the
                                                      // wait is not re-
                                                      // negotiated per ask
    }

    void i3ConfigFoldLegArmsTheSameServerFromDisk()
    {
        // The REAL user path (the .qmn/lsp.json fold), pinned once end-to-end
        // with the CI-built far side: the same server the arm door installs
        // arrives through the config file the IDE reads at openProject.
        writeFile(QStringLiteral("proj/.qmn/lsp.json"),
                  QByteArray("{\"servers\": {\"fakecpp\": {\"program\": \"")
                      + QByteArray(QMN_TEST_LSP_SERVER)
                      + QByteArray("\", \"args\": [\"sync\", \"utf-16\", "
                                   "\"features\", \"documentsymbol\"], "
                                   "\"languages\": [\"cpp\"]}}}"));
        writeFile(QStringLiteral("proj/a.cpp"), i3MarkerSource().toUtf8());
        window_->setLspEnabled(true); // the hub FIRST: openProject replays
                                      // the already-read config into it (the
                                      // ide_main order, pinned here too)
        window_->openProject(temp_->filePath(QStringLiteral("proj")));
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("proj/a.cpp"))));
        QVERIFY2(waitUntil([this] { return window_->outlineRowCount() == 4; }),
                 qPrintable(window_->outlineSummary()));
        // The fold's OWN truth: the outline named the language through the
        // CONFIG's spec, not the arm door — the same drill, the real path.
        QVERIFY(!window_->projectRoot().isEmpty());
    }
};

QTEST_MAIN(TestIde) // QTEST_MAIN (widget face, the tst_glue/tst_demo
                    // class): ctest runs it offscreen
#include "tst_ide.moc"
