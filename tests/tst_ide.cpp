// Drives the qmn-ide-demo app (GATE 4 car I0 skeleton + car I1 panel + car
// I2 quick-open/palette + car I3 LSP depth: definition, references, rename,
// code actions, formatting, the outline dock + car I4 tasks: the
// .qmn/tasks.json config door, the QProcess runner, the read-only output
// pane, the problem matcher feeding the SAME results panel and the view's
// M5 diagnostics door) fully offscreen through the
// SAME programmatic doors the dock widgets, popups and panel buttons call —
// no dialog exec, no file dialogs. THE SPAWN FACE (owner grant, 2026-11-28,
// PLAN's I3 opening record): the I3 depth drills DO spawn a server — the
// CI-built deterministic `fakeserver` via $<TARGET_FILE:fakeserver>, the
// dapfake class tst_demo's --dap slot already rides, its exit code the
// far-side oracle — armed by the window's own `armLspServer` door UNDER THE
// BUILT-IN SPEC NAME ("clangd" replaced whole), so PATH clangd is still
// NEVER spawned here (clangd is no CI guarantee; probe_clangd.sh stays its
// ritual). Every OTHER slot keeps the I0 law whole: a hub slot without an
// explicit spec path opens only UNCLAIMED languages.
// THE SPAWN FAMILY GROWS FOR CAR I4 ($QMN_TEST_FAKE_TASK, the same grant
// reused exactly): the task drills spawn the CI-built `faketask` through the
// IDE's OWN runner and the IDE's OWN config file — the slots write a real
// <root>/.qmn/tasks.json and the window reads it at openProject (the config
// FILE is the door; there is no programmatic task-registration face). NO
// PATH COMPILER IS EVER SPAWNED: the matcher's subject is that binary's
// compiler-SHAPED text, and the real-toolchain story is the close ritual's
// measured live run, never a ctest entry. The real-user
// `.qmn/lsp.json` fold leg is pinned end-to-end once.
// tst_ide never includes a glue, lsp or search header: IdeWindow.hpp keeps
// those types behind forward-decls, and the window's own read doors
// (searchSummary/outline*/jump*/rename*/format*/codeAction*/resultsProducer/
// searchResult*) are the observation channel. Assertions are on model/widget
// state, never on pixels.

#include "app/IdeWindow.hpp"

#include "view/CodeDocument.hpp" // car I4: the output pane is a REAL editor —
#include "view/CodeEditorOptions.hpp" // its document() and options() knobs are
                                      // observed directly (the M19 lock face)
#include "view/CodeEditor.hpp"

#include <QApplication>
#include <QClipboard> // car I4: the output pane's COPY face (the M19 lock keeps
                      // copy alive — that is the point of a build log)
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
        QCOMPARE(window_->commandRowCount(), 16); // the built-ins — car I3
        // appended SIX LSP commands at the END of the frozen registry order,
        // and car I4 appended SIX TASK commands after those (rows 0..3 below
        // are the I2 four, unmoved: an append never disturbs the tie law's
        // registry order — the slot i4TaskChordsAndPaletteAppendAreMaskFree
        // pins the new six)
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
        QCOMPARE(window_->commandRowCount(), 16); // I4's six ride it (append)

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
        QCOMPARE(window_->commandRowCount(), 16); // rows intact (I3's six and
                                                  // I4's six appended commands
                                                  // ride this face too)
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

    // ============================================ GATE 4 car I4: tasks, the
    // output pane, and the problem matcher.
    //
    // THE SPAWN FAMILY GROWS (the I3 grant reused exactly, PLAN's I4 opening
    // record): the task drills spawn `faketask` — the CI-built deterministic
    // fakeserver/dapfake sibling ($<TARGET_FILE:faketask>, a build-dependency
    // of this target, its EXIT CODE the far-side oracle and its TEXT the
    // fixture the matcher eats). The spawn goes through the IDE's OWN runner:
    // the slots write a real .qmn/tasks.json into the fixture root and the
    // window reads it at its own project door — the config FILE is the door;
    // there is no programmatic task-registration face. NO PATH COMPILER IS
    // EVER SPAWNED HERE (a real compiler is no CI guarantee; the real-toolchain
    // story is the close ritual's measured LIVE run, never a ctest entry).
    //
    // Wall-clock NEVER rides a pin: the walk rides the window's own doors +
    // waitUntil, and the far side's exit code/text is the oracle (the same law
    // the search walk and the DAP legs froze).

    // The config the window reads at openProject (the app's OWN file — the
    // shelf owns no task door). @FAKE@ is the CI-built faketask path.
    void i4WriteTasks(const QString& root, QString body) // BY VALUE: Qt's
        // replace() mutates, and a const-ref string could not carry the swap
    {
        const QString path = QDir(root).filePath(QStringLiteral(".qmn/tasks.json"));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
        const QString json
            = body.replace(QStringLiteral("@FAKE@"),
                           QStringLiteral(QMN_TEST_FAKE_TASK));
        file.write(json.toUtf8());
    } // VOID on purpose (the writeFile helper's law: a QVERIFY inside a
      // value-returning helper is the return-statement-with-no-value trap)
    void i4WriteTasksHere(const QString& body) { i4WriteTasks(temp_->path(), body); }

    static QString i4Clean(const QString& absolute) { return QDir::cleanPath(absolute); }

    bool i4RunFinished(int timeoutMs = 20000)
    {
        return waitUntil([this] { return !window_->taskRunning(); }, timeoutMs);
    }

    void i4TaskConfigReadsAtTheProjectDoor()
    {
        // FOUR tasks: the default, the msvc pair, the "none" face, and a cwd
        // face that EXPANDS ${WORKSPACE_ROOT}. Config order is the read-back's
        // order AND the picker's order — one truth, never a re-sort.
        writeFile(QStringLiteral("keep.txt"), "x\n");
        i4WriteTasksHere(QStringLiteral(R"({
            "version": 2.0,
            "tasks": [
              {"label": "build", "command": "@FAKE@", "args": ["gcc"],
               "problemMatcher": "gcc", "default": true, "group": "build",
               "presentation": {"reveal": "always"}},
              {"label": "msvc build", "command": "@FAKE@", "args": ["msvc"],
               "problemMatcher": "msvc"},
              {"label": "noisy", "command": "@FAKE@", "problemMatcher": "none"},
              {"label": "cwd face", "command": "@FAKE@",
               "args": ["cwd", "a", "b"], "cwd": "${WORKSPACE_ROOT}/sub",
               "env": {"CC": "gcc"}}
            ],
            "unknownTopLevelKey": {"nested": 1}
        })"));
        window_->openProject(temp_->path());
        const QString root = window_->projectRoot();
        QVERIFY(window_->tasksConfigured());
        QVERIFY2(window_->taskConfigSummary().isEmpty(),
                 qPrintable(window_->taskConfigSummary()));
        QCOMPARE(window_->taskCount(), 4);
        QCOMPARE(window_->taskLabelAt(0), QStringLiteral("build"));
        QCOMPARE(window_->taskLabelAt(3), QStringLiteral("cwd face"));
        QCOMPARE(window_->taskLabelAt(9), QString()); // the out-of-range face
        QVERIFY(window_->taskIsDefaultAt(0));
        QVERIFY(!window_->taskIsDefaultAt(1));
        QVERIFY(!window_->taskIsDefaultAt(9)); // out of range, honestly false
        QCOMPARE(window_->taskMatcherAt(0), QStringLiteral("gcc"));
        QCOMPARE(window_->taskMatcherAt(1), QStringLiteral("msvc"));
        QCOMPARE(window_->taskMatcherAt(2), QStringLiteral("none"));
        QCOMPARE(window_->taskMatcherAt(3), QStringLiteral("gcc")); // the
        // DECLARED default: no problemMatcher key at all
        // THE DECLARED FORWARDS-COMPATIBILITY FACE: unknown keys — top-level
        // (version, unknownTopLevelKey) AND per-task (group, presentation, env)
        // — are IGNORED, not refused. A config that carries VS Code's shape
        // still reads.
        // THE CWD DEFAULTS AND THE TOKEN EXPANSION (the lsp.json spelling):
        QCOMPARE(window_->taskWorkingDirAt(0), i4Clean(root));
        QCOMPARE(window_->taskWorkingDirAt(3), i4Clean(root + QStringLiteral("/sub")));
        QCOMPARE(window_->taskWorkingDirAt(42), QString()); // out of range
    }

    void i4TaskConfigRefusalsAreLoudAndWhole()
    {
        // A refused config reads as NOTHING — never a half-registry (the loud-
        // refusal law this window froze for the search query and the I3 gates)
        // — and the refusal's MACHINE reason rides the summary VERBATIM, onto
        // the ask faces too.
        window_->openProject(temp_->path());
        QCOMPARE(window_->taskCount(), 0); // no tasks.json at all: NOT a
        QVERIFY(window_->taskConfigSummary().isEmpty()); // refusal — an honest
        QVERIFY(!window_->tasksConfigured());            // nothing

        struct Leg { const char* name; const char* json; const char* phrase; };
        const Leg legs[] = {
            { "malformed-json", "{ \"tasks\": [ ", "задачи:" },
            { "tasks-not-a-list", "{\"tasks\": {}}", "\"tasks\"" },
            { "no-label", "{\"tasks\":[{\"command\":\"x\"}]}", "label" },
            { "no-command", "{\"tasks\":[{\"label\":\"a\"}]}", "command" },
            { "args-not-a-list",
              "{\"tasks\":[{\"label\":\"a\",\"command\":\"x\",\"args\":\"y\"}]}",
              "args" },
            { "cwd-not-a-string",
              "{\"tasks\":[{\"label\":\"a\",\"command\":\"x\",\"cwd\":5}]}", "cwd" },
            { "matcher-not-a-string",
              "{\"tasks\":[{\"label\":\"a\",\"command\":\"x\",\"problemMatcher\":[\"gcc\"]}]}",
              "problemMatcher" },
            { "unknown-matcher-name",
              "{\"tasks\":[{\"label\":\"a\",\"command\":\"x\",\"problemMatcher\":\"eslint\"}]}",
              "eslint" },
            { "default-not-a-bool",
              "{\"tasks\":[{\"label\":\"a\",\"command\":\"x\",\"default\":\"yes\"}]}",
              "default" },
            { "duplicate-label",
              "{\"tasks\":[{\"label\":\"a\",\"command\":\"x\"},{\"label\":\"a\","
              "\"command\":\"y\"}]}", "повторяющаяся" },
            { "two-defaults",
              "{\"tasks\":[{\"label\":\"a\",\"command\":\"x\",\"default\":true},"
              "{\"label\":\"b\",\"command\":\"y\",\"default\":true}]}", "по умолчанию" },
        };
        for (const Leg& leg : legs) {
            i4WriteTasksHere(QString::fromUtf8(leg.json));
            window_->openProject(temp_->path()); // the SAME door re-reads
            QVERIFY2(window_->taskCount() == 0,
                     QStringLiteral("%1: a refused config left %2 tasks behind")
                         .arg(leg.name)
                         .arg(window_->taskCount())
                         .toUtf8()
                         .constData());
            QVERIFY2(!window_->tasksConfigured(), leg.name);
            QVERIFY2(window_->taskConfigSummary().contains(QString::fromUtf8(leg.phrase)),
                     QStringLiteral("%1 -> %2")
                         .arg(leg.name, window_->taskConfigSummary())
                         .toUtf8()
                         .constData());
            QVERIFY2(window_->taskConfigSummary().contains(
                         QStringLiteral(".qmn/tasks.json")),
                     leg.name); // the refusal NAMES the file
            // The ask faces carry the refusal VERBATIM, never a shrug:
            QVERIFY(!window_->runTask(QStringLiteral("a")));
            QCOMPARE(window_->taskRunSummary(), window_->taskConfigSummary());
            QVERIFY(!window_->runDefaultTask());
            QCOMPARE(window_->taskRunSummary(), window_->taskConfigSummary());
            // And the picker shows the refusal instead of an empty list:
            window_->showTaskPicker();
            QCOMPARE(window_->taskPickerRowCount(), 0);
            QCOMPARE(window_->taskPickerSummary(), window_->taskConfigSummary());
            window_->closeOverlay();
        }
    }

    void i4TaskRunPrintsIntoThePaneAndCarriesTheExitFaces()
    {
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"plain","command":"@FAKE@"},
            {"label":"failing","command":"@FAKE@","args":["exit","3"]},
            {"label":"no-program","command":"/nonexistent/qmn-does-not-exist"},
            {"label":"cwd","command":"@FAKE@",
             "args":["cwd","a","${WORKSPACE_ROOT}/in-args"],
             "cwd":"${WORKSPACE_ROOT}/sub"}]})"));
        writeFile(QStringLiteral("sub/keep.txt"), "x\n"); // the cwd leg's dir
        window_->openProject(temp_->path());
        const QString root = window_->projectRoot();
        window_->showTaskOutput(true);
        QVERIFY(window_->taskOutputVisible());

        // THE NEVER-AUTO-RUN LAW at the door that READS the config: openProject
        // read four tasks and ran NONE of them.
        QVERIFY(!window_->taskRunning());
        QCOMPARE(window_->taskExitCode(), IdeWindow::kTaskExitPending);
        QCOMPARE(window_->taskRunLabel(), QString());
        QCOMPARE(window_->taskOutputLineCount(), 0);

        // A named run, and the plain success face:
        QVERIFY(window_->runTask(QStringLiteral("plain")));
        QVERIFY(window_->taskRunning());
        QCOMPARE(window_->taskRunLabel(), QStringLiteral("plain"));
        // NO exit code EXISTS while it runs — the pending sentinel, never a
        // borrowed zero:
        QCOMPARE(window_->taskExitCode(), IdeWindow::kTaskExitPending);
        QVERIFY2(i4RunFinished(), "the plain run never finished");
        QCOMPARE(window_->taskExitCode(), 0);
        QVERIFY2(window_->taskRunSummary().contains(QStringLiteral("успешно")),
                 qPrintable(window_->taskRunSummary()));
        QVERIFY2(window_->taskOutput().contains(QStringLiteral("FAKE-TASK-OK")),
                 qPrintable(window_->taskOutput()));
        QVERIFY(window_->taskElapsedMs() >= 0);
        QCOMPARE(window_->taskOutputHiddenLines(), 0);
        // ONE truth, two faces: the door's text and the pane's document agree.
        CodeEditor* pane = window_->outputPane();
        QVERIFY(pane);
        QCOMPARE(pane->toPlainText(), window_->taskOutput());
        QCOMPARE(pane->document()->lineCount(), window_->taskOutputLineCount());

        // A non-zero exit is a FACE, not a failure of the runner:
        QVERIFY(window_->runTask(QStringLiteral("failing")));
        QVERIFY2(i4RunFinished(), "the failing run never finished");
        QCOMPARE(window_->taskExitCode(), 3);
        QVERIFY2(window_->taskRunSummary().contains(QStringLiteral("код 3")),
                 qPrintable(window_->taskRunSummary()));
        // ONE run, one log: the new run CLEARED the last one's pane, so the
        // tail cap and the hidden count always describe a single run.
        QVERIFY2(!window_->taskOutput().contains(QStringLiteral("FAKE-TASK-OK")),
                 qPrintable(window_->taskOutput()));

        // Failed to start: NO exit code the child could have written — its own
        // negative sentinel, and the summary says it could not start.
        QVERIFY(window_->runTask(QStringLiteral("no-program")));
        QVERIFY2(i4RunFinished(), "the failed start never reported");
        QCOMPARE(window_->taskExitCode(), IdeWindow::kTaskExitFailedToStart);
        QVERIFY2(window_->taskRunSummary().contains(
                     QStringLiteral("не удалось запустить")),
                 qPrintable(window_->taskRunSummary()));

        // THE CWD + ARGS FACE, witnessed by the CHILD: the far side prints the
        // directory it was given and the argv it received, so ${WORKSPACE_ROOT}
        // expansion is the child's testimony, not our assertion.
        QVERIFY(window_->runTask(QStringLiteral("cwd")));
        QVERIFY2(i4RunFinished(), "the cwd run never finished");
        QVERIFY2(window_->taskOutput().contains(
                     QStringLiteral("CWD=") + i4Clean(root + QStringLiteral("/sub"))),
                 qPrintable(window_->taskOutput()));
        QVERIFY2(window_->taskOutput().contains(QStringLiteral("ARGS=a|") + root
                                                + QStringLiteral("/in-args")),
                 qPrintable(window_->taskOutput())); // the far side joins argv[2..]

        // The no-project refusal (before ANY project is open):
        IdeWindow bare;
        bare.show();
        QVERIFY(!bare.runTask(QStringLiteral("plain")));
        QVERIFY(bare.taskRunSummary().contains(QStringLiteral("проект")));
        QVERIFY(!bare.runDefaultTask());
        QVERIFY(bare.taskRunSummary().contains(QStringLiteral("проект")));
    }

    void i4PaneIsAReadOnlyLogThatStaysCopyable()
    {
        // THE STUB'S DOGFOOD BET, PINNED ON THE REAL EDITOR (the opening
        // audit's answer, and the M19 law quoted at CodeEditorOptions.hpp:
        // every USER edit gesture is refused while "caret movement, selection,
        // copy, find and programmatic document replacement keep working"):
        // the log LANDS through the unlocked programmatic door, a keystroke
        // changes nothing, and a locked pane is still copyable.
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"lines","command":"@FAKE@","args":["stderr"]}]})"));
        window_->openProject(temp_->path());
        const int tabsBefore = window_->editorCount();
        QVERIFY(window_->runTask(QStringLiteral("lines")));
        QVERIFY2(i4RunFinished(), "the run never finished");
        QCOMPARE(window_->editorCount(), tabsBefore); // the pane is NOT a tab,
        CodeEditor* pane = window_->outputPane();     // never a session document
        QVERIFY(pane);
        QVERIFY(pane->options()->isReadOnly()); // THE LOCK is on (the options
        QVERIFY(pane->options()->softWrapEnabled()); // door is the public face;
        const QString log = window_->taskOutput();  //   the view's own setter is
        QCOMPARE(pane->toPlainText(), log);         //   private — M19's rule)

        // A USER keystroke changes nothing (the front-door silence M19 froze):
        pane->setFocus();
        QTest::qWait(20);
        const int linesBefore = pane->document()->lineCount();
        QTest::keyClick(pane, Qt::Key_X);
        QTest::keyClick(pane, Qt::Key_Return);
        QTest::keyClick(pane, Qt::Key_Backspace);
        QTest::keyClick(pane, Qt::Key_Delete);
        QCOMPARE(pane->toPlainText(), log);
        QCOMPARE(pane->document()->lineCount(), linesBefore);
        // undo/redo are edit gestures too — they refuse, the log stands:
        QTest::keyClick(pane, Qt::Key_Z, Qt::ControlModifier);
        QTest::keyClick(pane, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(pane->toPlainText(), log);

        // AND IT IS COPYABLE: selection + copy yields the pane's own text (the
        // point of a build log). The offscreen platform carries a clipboard
        // (tst_codeeditor's clipboard legs ride the same platform).
        QVERIFY2(QApplication::clipboard() != nullptr,
                 "no clipboard on this platform: the copy face cannot be "
                 "witnessed — and a face that cannot be witnessed is never a pass");
        pane->selectAll();
        pane->copy();
        QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard), log);

        // THE TAIL FOLLOW, app-side arithmetic over public doors: after the
        // final flush the caret rides the log's LAST line (the reveal is the
        // view's own conditional edge-flush).
        QCOMPARE(pane->position().line, window_->taskOutputLineCount() - 1);

        // The clear door clears the PANE only (the pane and the problems are
        // two truths):
        window_->clearTaskOutput();
        QCOMPARE(window_->taskOutputLineCount(), 0);
        QCOMPARE(window_->taskOutputHiddenLines(), 0);
        QCOMPARE(pane->toPlainText(), QString());
    }

    void i4PaneTailCapHidesAndSaysSoAndStreamOrderSurvives()
    {
        // THE TAIL CAP + THE VISIBLE HIDDEN COUNT (the truncation-shows law I2
        // froze for quick-open, now riding the pane) — and ARRIVAL ORDER across
        // the two streams (the runner's merged-channel law, the documented
        // answer: separate channels carry NO cross-stream ordering guarantee).
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"big","command":"@FAKE@","args":["big","6000"]},
            {"label":"streams","command":"@FAKE@","args":["stderr"]}]})"));
        window_->openProject(temp_->path());
        QVERIFY(window_->runTask(QStringLiteral("big")));
        QVERIFY2(i4RunFinished(30000), "the big run never finished");
        const int visible = window_->taskOutputLineCount();
        QCOMPARE(visible, IdeWindow::kOutputMaxLines); // the NEWEST kept, exactly
        QCOMPARE(window_->taskOutputHiddenLines(), 6000 - visible);
        const QStringList lines = window_->taskOutput().split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), visible);
        // FIRST-KEPT and LAST are the far side's own numbering: a drift between
        // the cap, the count and the kept window shows up HERE, loudly.
        QCOMPARE(lines.first(), QStringLiteral("BIG-%1").arg(6000 - visible + 1));
        QCOMPARE(lines.last(), QStringLiteral("BIG-6000"));
        QVERIFY2(window_->taskRunSummary().contains(
                     QStringLiteral("(скрыто 4000)")),
                 qPrintable(window_->taskRunSummary())); // hidden rides the
                                                         // summary too
        CodeEditor* pane = window_->outputPane();
        QCOMPARE(pane->document()->lineCount(), visible);
        QCOMPARE(pane->toPlainText(), window_->taskOutput());

        // ARRIVAL ORDER: the faketask writes OUT-1 / ERR-1 / OUT-2 and the pane
        // shows exactly that sequence, ERR-1 in the MIDDLE — a compiler talking
        // on stderr is not an accident and never re-arrived at the end.
        QVERIFY(window_->runTask(QStringLiteral("streams")));
        QVERIFY2(i4RunFinished(), "the streams run never finished");
        QCOMPARE(window_->taskOutput(), QStringLiteral("OUT-1\nERR-1\nOUT-2"));
        // OUTPUT DURING A RUN is not a finish-only face: the same paced replace
        // serves a running task (the flush timer, not the exit, put it there).
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"slow","command":"@FAKE@","args":["slow","900"]}]})"));
        window_->openProject(temp_->path());
        QVERIFY(window_->runTask(QStringLiteral("slow")));
        QVERIFY2(waitUntil([this] {
                     return window_->taskOutput().contains(QStringLiteral("SLOW-START"));
                 }),
                 qPrintable(window_->taskOutput()));
        QVERIFY(window_->taskRunning()); // MID-RUN: the door already carries
        QVERIFY(!window_->outputPane()->toPlainText().contains(QStringLiteral("SLOW-END")));
        // THE PACE ITSELF IS A FACE: the model (the door) is AHEAD of the pane
        // by up to one flush interval — the replace is paced (one per burst),
        // so the pane CATCHES UP on its own timer while the run is still alive.
        QVERIFY2(waitUntil([this] {
                     return window_->outputPane()->toPlainText().contains(
                         QStringLiteral("SLOW-START"));
                 }),
                 qPrintable(window_->outputPane()->toPlainText()));
        QVERIFY(window_->taskRunning()); // STILL running when the pane showed
        QVERIFY(!window_->taskOutput().contains(QStringLiteral("SLOW-END")));
        QVERIFY2(i4RunFinished(), "the slow run never finished");
        QVERIFY(window_->taskOutput().contains(QStringLiteral("SLOW-END")));
        QCOMPARE(window_->outputPane()->toPlainText(), window_->taskOutput());
    }

    void i4StopLegsGracefulAndEscalation()
    {
        // TWO stop legs (the opening record's design): terminate() reaches a
        // child that HANDLES SIGTERM and the far side says so IN THE PANE in
        // arrival order — an honest far-side witness. A STUBBORN child gives no
        // witness (a kill has no voice): its witness is our own escalated face,
        // pinned as such, never as a fake exit code.
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"hang","command":"@FAKE@","args":["hang"],"default":true},
            {"label":"stubborn","command":"@FAKE@","args":["hang","stubborn"]}]})"));
        window_->openProject(temp_->path());

        // The refusal face BEFORE anything runs:
        QVERIFY(!window_->stopTask());
        QVERIFY2(window_->taskRunSummary().contains(
                     QStringLiteral("нет выполняющейся задачи")),
                 qPrintable(window_->taskRunSummary()));

        QVERIFY(window_->runDefaultTask()); // the DEFAULT door runs the marked
        QVERIFY2(waitUntil([this] {         // task (Ctrl+Shift+B's body)
                     return window_->taskOutput().contains(QStringLiteral("HANG-START"));
                 }),
                 qPrintable(window_->taskOutput()));
        QVERIFY(window_->taskRunning());
        // ONE RUN AT A TIME — a second ask is a LOUD refusal, never a queue:
        QVERIFY(!window_->runTask(QStringLiteral("stubborn")));
        QVERIFY2(window_->taskRunSummary().contains(QStringLiteral("ещё выполняется")),
                 qPrintable(window_->taskRunSummary()));
        QVERIFY(!window_->runDefaultTask()); // the same refusal, same face
        // The picker keeps its honesty while a run stands:
        window_->showTaskPicker();
        QVERIFY(window_->taskPickerVisible());
        QCOMPARE(window_->taskPickerRowCount(), 2); // CONFIG order
        QCOMPARE(window_->taskPickerLabelAt(0), QStringLiteral("hang"));
        QVERIFY(!window_->activateTask(1)); // a refusal KEEPS the popup open
        QVERIFY(window_->taskPickerVisible());
        window_->closeOverlay();

        QVERIFY(window_->stopTask());
        QVERIFY2(i4RunFinished(), "the stopped run never reported");
        QVERIFY2(window_->taskOutput().contains(QStringLiteral("TERM-CAUGHT")),
                 qPrintable(window_->taskOutput())); // THE FAR SIDE'S OWN
        QCOMPARE(window_->taskExitCode(), 0);         // WITNESS: terminate
        QVERIFY2(window_->taskRunSummary().contains(QStringLiteral("остановлена")),
                 qPrintable(window_->taskRunSummary())); // reached the child,
        QVERIFY(!window_->taskRunSummary().contains(QStringLiteral("убить")));
        // which answered and exited 0 by itself
        QVERIFY(!window_->taskRunSummary().contains(QStringLiteral("код 0")));
        // ("остановлена" is the state face; an exit code the STOP caused is
        // never dressed up as the child's own verdict)

        // The stubborn leg: the grace expires, the app escalates, the summary
        // NAMES the escalation instead of inventing a code.
        QVERIFY(window_->runTask(QStringLiteral("stubborn")));
        QVERIFY2(waitUntil([this] {
                     return window_->taskOutput().contains(QStringLiteral("HANG-START"));
                 }),
                 qPrintable(window_->taskOutput()));
        QVERIFY(window_->stopTask());
        QVERIFY2(i4RunFinished(), "the killed run never reported");
        QVERIFY2(window_->taskRunSummary().contains(QStringLiteral("убить")),
                 qPrintable(window_->taskRunSummary()));
        QCOMPARE(window_->taskExitCode(), IdeWindow::kTaskExitCrashed);
        QVERIFY(!window_->taskOutput().contains(QStringLiteral("TERM-CAUGHT")));
        QVERIFY(!window_->stopTask()); // the same honest refusal again
    }

    void i4ProjectSwitchKillsTheRunAndRetiresItsFaces()
    {
        // A task SURVIVES a tab close and DIES at a project switch (the pool-
        // replacement seam's process sibling). A kill()'d child cannot witness
        // its own death (SIGKILL has no voice — recorded honestly in PLAN, the
        // graceful stop leg above carries the far-side witness instead), so
        // this leg pins the app's OWN faces plus the generation law: nothing a
        // dead run emits afterwards can speak, and every face retires.
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"gcc","command":"@FAKE@","args":["gcc"]}]})"));
        window_->openProject(temp_->path());
        QVERIFY(window_->runTask(QStringLiteral("gcc")));
        QVERIFY2(i4RunFinished(), "the gcc run never finished");
        QCOMPARE(window_->problemCount(), 4); // (the matcher's rows — pinned in
                                              // the matcher slot below)
        writeFile(QStringLiteral("keep.txt"), "x\n");
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("keep.txt"))));
        const int tabsBefore = window_->editorCount();

        QDir().mkpath(temp_->filePath(QStringLiteral("other")));
        window_->openProject(temp_->filePath(QStringLiteral("other")));
        // A task from the OLD root never poses as this project's truth:
        QVERIFY(!window_->tasksConfigured());
        QCOMPARE(window_->taskCount(), 0);
        QCOMPARE(window_->problemCount(), -1); // the panel claim retires too
        QCOMPARE(window_->problemFeedCount(), 0);
        QCOMPARE(window_->taskOutputLineCount(), 0); // the pane retires
        QVERIFY(!window_->taskRunning());
        QCOMPARE(window_->editorCount(), tabsBefore); // a TAB CLOSE is not the
        window_->closeTabAt(0);                       // task's death face —
        QCOMPARE(window_->editorCount(), tabsBefore - 1); // a live run rides it
    }

    void i4SwitchKillsALiveRunAndNothingStraggles()
    {
        // THE SAME LAW ON A LIVE RUN: the switch KILLS, the faces say so, and
        // the dead run's straggler signals (generation-retired) can never speak
        // again — the next run's faces are its own, with the old output gone.
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"hang","command":"@FAKE@","args":["hang"]}]})"));
        window_->openProject(temp_->path());
        QVERIFY(window_->runTask(QStringLiteral("hang")));
        QVERIFY2(waitUntil([this] {
                     return window_->taskOutput().contains(QStringLiteral("HANG-START"));
                 }),
                 qPrintable(window_->taskOutput()));
        const int killedTabs = window_->editorCount();
        QDir().mkpath(temp_->filePath(QStringLiteral("other")));
        window_->openProject(temp_->filePath(QStringLiteral("other")));
        QVERIFY(!window_->taskRunning());
        QCOMPARE(window_->taskExitCode(), IdeWindow::kTaskExitCrashed); // WE
        QVERIFY2(window_->taskRunSummary().contains(QStringLiteral("прервана")),
                 qPrintable(window_->taskRunSummary()));               // ended
        QCOMPARE(window_->taskOutputLineCount(), 0);                    // it
        QCOMPARE(window_->editorCount(), killedTabs);

        // The next run, in the NEW root, with its own config and clean faces:
        i4WriteTasks(temp_->filePath(QStringLiteral("other")),
                     QStringLiteral(R"({"tasks":[
                         {"label":"plain","command":"@FAKE@"}]})"));
        window_->openProject(temp_->filePath(QStringLiteral("other")));
        QCOMPARE(window_->taskCount(), 1);
        QVERIFY(window_->runTask(QStringLiteral("plain")));
        QVERIFY2(i4RunFinished(), "the clean run never finished");
        QCOMPARE(window_->taskExitCode(), 0);
        QCOMPARE(window_->taskOutputLineCount(), 1);
        QVERIFY(!window_->taskOutput().contains(QStringLiteral("HANG-START")));
        QVERIFY(window_->taskOutput().contains(QStringLiteral("FAKE-TASK-OK")));
    }

    void i4GccMatcherRowsRideThePanelAndRefuseToInvent()
    {
        // THE PANEL'S THIRD PRODUCER over the engine's own currency, and the
        // NEVER-INVENT law: what the pane shows is OUTPUT; what becomes a ROW is
        // what the pattern actually matches. The far side's noise — an
        // `In file included from` header line, a caret line, `collect2:`, and
        // the OTHER matcher's shape — stays text.
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"gcc","command":"@FAKE@","args":["gcc"]}]})"));
        window_->openProject(temp_->path());
        const QString root = window_->projectRoot();
        // A standing SEARCH result first: the problems feed replaces the panel
        // ONLY when it has rows of its own (resultsProducer() names the owner).
        window_->runSearch(QStringLiteral("nothing-matches-this-text"));
        QVERIFY2(waitUntil([this] { return !window_->searchRunning(); }),
                 "the search never finished");
        QCOMPARE(window_->resultsProducer(), QStringLiteral("search"));
        QCOMPARE(window_->problemCount(), -1); // the problems face does not
                                               // exist under another producer

        QVERIFY(window_->runTask(QStringLiteral("gcc")));
        QVERIFY2(i4RunFinished(), "the gcc run never finished");
        QCOMPARE(window_->resultsProducer(), QStringLiteral("problems"));
        QCOMPARE(window_->problemCount(), 4);
        QCOMPARE(window_->problemFeedCount(), 4);
        // The far side WROTE the file it complains about, in the task's cwd —
        // the cwd face and the row's frame of reference, one witness:
        const QString fakeSource = i4Clean(root + QStringLiteral("/fake_src.cpp"));
        QVERIFY2(QFileInfo(fakeSource).exists(),
                 "the faketask did not write its own subject file");
        // GROUPING: files in FIRST-APPEARANCE order, rows inside a file in
        // EMISSION order (the panel's walk-order honesty, unchanged by a new
        // producer).
        QCOMPARE(window_->searchResultFileAt(0), fakeSource);
        QCOMPARE(window_->searchResultFileAt(1), fakeSource);
        QCOMPARE(window_->searchResultFileAt(2), fakeSource);
        QCOMPARE(window_->searchResultFileAt(3),
                 i4Clean(root + QStringLiteral("/gone.cpp")));
        // 0-based ENGINE lines (wire 3:10 -> line 2 col 9), and the column
        // faces: an explicit column is ONE cell; a MISSING column is the line's
        // start (the compiler pointed at a column, never an extent — the caret
        // line that WOULD show an extent is noise we refuse to parse).
        QCOMPARE(window_->searchResultLineAt(0), 2);
        QCOMPARE(window_->searchResultLineAt(1), 5);
        QCOMPARE(window_->searchResultLineAt(2), 7);
        QCOMPARE(window_->searchResultLineAt(3), 0);
        QCOMPARE(window_->problemSeverityAt(0), QStringLiteral("error"));
        QCOMPARE(window_->problemSeverityAt(1), QStringLiteral("warning"));
        QCOMPARE(window_->problemSeverityAt(2), QStringLiteral("information"));
        QCOMPARE(window_->problemSeverityAt(3), QStringLiteral("error"));
        // (the mapping at the door: error AND fatal error -> Error, warning ->
        // Warning, note -> Information)
        QCOMPARE(window_->problemSeverityAt(4), QString()); // out of range
        QCOMPARE(window_->problemSeverityAt(-1), QString());
        QVERIFY(window_->problemFileExistsAt(0));
        QVERIFY(!window_->problemFileExistsAt(3)); // THE PHANTOM ROW: a
        // compiler may name a gone file — the row IS the answer, the reveal is
        // a separate truth (openFile's own law)
        // THE ROW'S OWN TEXT (producer-governed — searchResultTextAt's law):
        // the compiler's message VERBATIM, its flag tail and all, riding the
        // LineMatch's own preview field (no new field anywhere).
        QVERIFY2(window_->searchResultTextAt(0).contains(
                     QStringLiteral("'undeclared_thing' was not declared")),
                 qPrintable(window_->searchResultTextAt(0)));
        QVERIFY2(window_->searchResultTextAt(0).contains(
                     QStringLiteral("[-Wundeclared]")),
                 qPrintable(window_->searchResultTextAt(0)));
        QVERIFY(window_->searchResultTextAt(2).contains(
            QStringLiteral("while expanding")));
        // The NOISE the pane shows and the panel refuses:
        QVERIFY(window_->taskOutput().contains(QStringLiteral("collect2: error:")));
        QVERIFY(window_->taskOutput().contains(QStringLiteral("In file included")));
        QVERIFY(window_->taskOutput().contains(
            QStringLiteral("msvc shape under gcc")));
        QCOMPARE(window_->problemCount(), 4); // unchanged by the noise
        QVERIFY(window_->searchSummary().contains(QStringLiteral("проблем")));
        // TWO FACES, ONE TRUTH EACH, decided by POST ORDER: finishTask posts the
        // RUN's summary AFTER buildProblemFeed handed the panel its own, so the
        // shared bar carries the run's face while the panel's truth is the door
        // searchSummary(). (The I3 lesson — a per-dock truth rides its OWN face,
        // not the shared bar — is honored here by never claiming the bar for the
        // problems feed.)
        QCOMPARE(window_->statusBar()->currentMessage(), window_->taskRunSummary());

        // REVEAL LAWS: the phantom refuses LOUDLY and keeps its row; a real row
        // opens the file and selects the span with the CARET AT THE END (the I1
        // panel law — the definition jump's START face is the other side of the
        // SAME door, no new door needed).
        QVERIFY(!window_->activateSearchResult(3));
        QCOMPARE(window_->problemCount(), 4); // the row STAYS visible
        QVERIFY(window_->activateSearchResult(0));
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        QCOMPARE(editor->document()->lineText(2),
                 QStringLiteral("  return undeclared_thing + x;"));
        QCOMPARE(editor->position(), (TextPosition{2, 10})); // caret at the
        QVERIFY(editor->hasSelection());                     // span END [9,10)
        QCOMPARE(editor->selectedText(), QStringLiteral("u"));
        QVERIFY(!window_->activateSearchResult(99)); // the frozen out-of-range
    }

    void i4ProblemFeedMergesWithAnotherProducerAndRestores()
    {
        // THE SHARED DECORATION CHANNEL, BOTH directions, pinned with no hub in
        // sight: the app rides the view's OWN M5 door with its producer tag,
        // merges over what the tag does not own, and RESTORES exactly the
        // foreign rows when its feed ends (the DapHub posture, app-side).
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"gcc","command":"@FAKE@","args":["gcc"]}]})"));
        window_->openProject(temp_->path());
        const QString fakeSource = i4Clean(window_->projectRoot()
                                           + QStringLiteral("/fake_src.cpp"));

        // CATCH-UP FACE: the run lands BEFORE the tab exists (the file is
        // written BY the run), and opening the file afterwards wears the
        // squiggles at once — no re-run asked.
        QVERIFY(window_->runTask(QStringLiteral("gcc")));
        QVERIFY2(i4RunFinished(), "the first run never finished");
        QVERIFY(window_->openFile(fakeSource));
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        QCOMPARE(editor->diagnostics().size(), 3);
        QStringList sources;
        QVector<CodeEditor::Severity> severities;
        for (const CodeEditor::Diagnostic& diag : editor->diagnostics()) {
            sources << diag.source;
            severities << diag.severity;
        }
        const QStringList taskTag{ QStringLiteral("qmn-tasks"),
                                   QStringLiteral("qmn-tasks"),
                                   QStringLiteral("qmn-tasks") }; // brace-init
        QCOMPARE(sources, taskTag); // lists inside QCOMPARE need PARENS (the
                                     // macro counts commas — the brace form
                                     // passes FOUR args and never compiles)
        // the severity mapping rides the view's enum, and the message carries
        // the MATCHER (the producer label is opaque to the painter, exactly
        // like the hover sliver's):
        QVERIFY(severities.contains(CodeEditor::Severity::Warning));
        QVERIFY(severities.contains(CodeEditor::Severity::Information));
        QVERIFY(severities.contains(CodeEditor::Severity::Error));
        for (const CodeEditor::Diagnostic& diag : editor->diagnostics())
            QVERIFY(diag.message.startsWith(QStringLiteral("[gcc] ")));

        // A FOREIGN producer writes its own table (ours kept, one row added) —
        // exactly what a hub-served document carries:
        QVector<CodeEditor::Diagnostic> withForeign = editor->diagnostics();
        CodeEditor::Diagnostic foreign;
        foreign.startLine = 0;
        foreign.startCol = 0;
        foreign.endLine = 0;
        foreign.endCol = 4;
        foreign.severity = CodeEditor::Severity::Hint;
        foreign.message = QStringLiteral("someone else's mark");
        foreign.source = QStringLiteral("hand-made");
        withForeign.append(foreign);
        editor->setDiagnostics(withForeign);
        QCOMPARE(editor->diagnostics().size(), 4);

        // A NEW run replaces OUR feed and NOTHING else:
        QVERIFY(window_->runTask(QStringLiteral("gcc")));
        QVERIFY2(i4RunFinished(), "the second run never finished");
        int mine = 0, theirs = 0;
        for (const CodeEditor::Diagnostic& diag : editor->diagnostics()) {
            if (diag.source == QStringLiteral("qmn-tasks"))
                ++mine;
            else if (diag.source == QStringLiteral("hand-made"))
                ++theirs;
        }
        QCOMPARE(mine, 3);
        QCOMPARE(theirs, 1); // a re-run NEVER erases another producer's marks

        // And taking our feed away puts back EXACTLY what the tag did not own:
        window_->clearTaskProblems();
        QCOMPARE(editor->diagnostics().size(), 1);
        QCOMPARE(editor->diagnostics().first().source, QStringLiteral("hand-made"));
        QCOMPARE(editor->diagnostics().first().message,
                 QStringLiteral("someone else's mark"));
        QCOMPARE(editor->diagnostics().first().severity, CodeEditor::Severity::Hint);
        QCOMPARE(window_->problemCount(), -1); // the panel claim retires too
        QVERIFY(window_->searchSummary().contains(QStringLiteral("проблемы сняты")));

        // THE PANEL NEVER STEALS ITSELF: a run whose matcher claims NOTHING
        // leaves a standing search result in place ("none" names nothing; an
        // empty feed claims no producer).
        window_->runSearch(QStringLiteral("int"));
        QVERIFY2(waitUntil([this] { return !window_->searchRunning(); }),
                 "the search never finished");
        const int searchRows = window_->searchResultRowCount();
        QVERIFY(searchRows > 0); // fake_src.cpp and keep.txt both carry "int"
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"quiet","command":"@FAKE@","problemMatcher":"none"}]})"));
        window_->openProject(temp_->path());
        QVERIFY(window_->runTask(QStringLiteral("quiet")));
        QVERIFY2(i4RunFinished(), "the quiet run never finished");
        QCOMPARE(window_->problemFeedCount(), 0); // "none" claims NOTHING: the
        QVERIFY(window_->taskOutput().contains(QStringLiteral("FAKE-TASK-OK")));
        // run still PRINTED...
        QCOMPARE(window_->resultsProducer(), QStringLiteral("search")); // ...and
        QCOMPARE(window_->searchResultRowCount(), searchRows);          // stole
        QCOMPARE(window_->problemCount(), -1);                          // nothing
    }

    void i4SecondMatcherDecodesMsvcAndRefusesTheGccShape()
    {
        // THE MATCHER PAIR refuses each other's shape — SYMMETRIC, decided by
        // the patterns' arithmetic, not by a name table: the msvc mode prints
        // ONE gcc-shaped line that must NOT become a row, and its own code
        // token (C2065 & friends) rides the message rather than being dropped.
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"msvc","command":"@FAKE@","args":["msvc"],
             "problemMatcher":"msvc"}]})"));
        window_->openProject(temp_->path());
        const QString fakeSource = i4Clean(window_->projectRoot()
                                           + QStringLiteral("/fake_src.cpp"));
        QVERIFY(window_->runTask(QStringLiteral("msvc")));
        QVERIFY2(i4RunFinished(), "the msvc run never finished");
        QCOMPARE(window_->problemCount(), 3); // NOT 4: the gcc-shaped line is
        QCOMPARE(window_->problemFeedCount(), 3); // refused by THIS matcher
        QCOMPARE(window_->searchResultFileAt(0), fakeSource);
        QCOMPARE(window_->searchResultLineAt(0), 2); // (3,10) -> line 2, col 9
        QCOMPARE(window_->searchResultLineAt(1), 5); // the no-column shape too
        QCOMPARE(window_->problemSeverityAt(0), QStringLiteral("error"));
        QCOMPARE(window_->problemSeverityAt(1), QStringLiteral("warning"));
        QCOMPARE(window_->problemSeverityAt(2), QStringLiteral("information"));
        QVERIFY2(window_->searchResultTextAt(0).contains(QStringLiteral("C2065")),
                 qPrintable(window_->searchResultTextAt(0))); // the code token
        QVERIFY(window_->taskOutput().contains(QStringLiteral("gcc shape under msvc")));
        QVERIFY(window_->taskOutput().contains(
            QStringLiteral("ignoring unknown option"))); // stderr noise, no row
        QCOMPARE(window_->problemCount(), 3);
    }

    void i4TaskPickerRanksRefusesAndRuns()
    {
        // THE THIRD POPUP on the I2 law whole (child-scoped Esc, close-then-
        // run, a retrigger never wiping the query, empty needle = CONFIG order,
        // score DESC with ties broken by CONFIG order) — qmn::fuzzy's THIRD
        // user, over LABELS this time.
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"build","command":"@FAKE@"},
            {"label":"build release","command":"@FAKE@"},
            {"label":"clean everything","command":"@FAKE@"},
            {"label":"rebuild","command":"@FAKE@"}]})"));
        window_->openProject(temp_->path());

        // NOTHING configured is a refusal, never a silent zero-row list:
        QDir().mkpath(temp_->filePath(QStringLiteral("empty")));
        window_->openProject(temp_->filePath(QStringLiteral("empty")));
        window_->showTaskPicker();
        QCOMPARE(window_->taskPickerRowCount(), 0);
        QVERIFY(window_->taskPickerSummary().contains(QStringLiteral("нет")));
        QVERIFY(!window_->activateTask(0));
        window_->closeOverlay();

        window_->openProject(temp_->path());
        window_->showTaskPicker();
        QVERIFY(window_->taskPickerVisible());
        QCOMPARE(window_->taskPickerRowCount(), 4); // EMPTY NEEDLE = CONFIG
        QCOMPARE(window_->taskPickerLabelAt(0), QStringLiteral("build")); // order
        QCOMPARE(window_->taskPickerLabelAt(3), QStringLiteral("rebuild"));
        QCOMPARE(window_->taskPickerLabelAt(9), QString()); // out of range
        QVERIFY(window_->taskPickerSummary().contains(QStringLiteral("задач")));
        window_->showTaskPicker(); // IDEMPOTENCE: a retrigger never wipes the
        QCOMPARE(window_->taskPickerRowCount(), 4); // query and never re-shows
        window_->typeTaskPickerQuery(QStringLiteral("build"));
        window_->showTaskPicker(); // a retrigger on an OPEN picker is a no-op:
        QCOMPARE(window_->taskPickerRowCount(), 3); // the query SURVIVES (the
        QCOMPARE(window_->taskPickerLabelAt(0), QStringLiteral("build")); // I2
        window_->closeOverlay();                   // law, third popup)

        // A needle ranks by SCORE, ties by CONFIG ORDER (a stable sort over a
        // config-order scan): `build` matches build / build release / rebuild;
        // the two `build*` labels score IDENTICALLY (same positions, same
        // boundary and adjacency seams) and keep config order between them,
        // while `rebuild` starts its match INSIDE a word and ranks below.
        window_->showTaskPicker();
        window_->typeTaskPickerQuery(QStringLiteral("build"));
        QCOMPARE(window_->taskPickerRowCount(), 3);
        QCOMPARE(window_->taskPickerLabelAt(0), QStringLiteral("build"));
        QCOMPARE(window_->taskPickerLabelAt(1), QStringLiteral("build release"));
        QCOMPARE(window_->taskPickerLabelAt(2), QStringLiteral("rebuild"));
        QCOMPARE(window_->taskPickerSummary(), QStringLiteral("Совпадений: 3"));
        // A NO-MATCH needle is LOUD:
        window_->typeTaskPickerQuery(QStringLiteral("zzzznothing"));
        QCOMPARE(window_->taskPickerRowCount(), 0);
        QVERIFY(window_->taskPickerSummary().contains(
            QStringLiteral("Нет совпадений")));
        // ACTIVATION is close-then-run, and the far side's text lands:
        window_->typeTaskPickerQuery(QStringLiteral("clean"));
        QCOMPARE(window_->taskPickerRowCount(), 1);
        QVERIFY(window_->activateTask(0));
        QVERIFY(!window_->taskPickerVisible());
        QVERIFY2(i4RunFinished(), "the picked task never finished");
        QVERIFY(window_->taskOutput().contains(QStringLiteral("FAKE-TASK-OK")));
        // A refusal keeps the popup open (the quick-open failure face's twin):
        window_->showTaskPicker();
        QVERIFY(!window_->activateTask(7)); // out of range
        QVERIFY(window_->taskPickerVisible());
        window_->closeOverlay();
        QVERIFY(!window_->taskPickerVisible());
    }

    void i4TaskChordsAndPaletteAppendAreMaskFree()
    {
        // THE CHORD-TABLE AUDIT AS A TRIPWIRE, TWO new legs (the I2/I3
        // precedent): the view's frozen keymap binds NO Key_B and NO Key_R
        // EVER, so Ctrl+Shift+B / Ctrl+Shift+R reach the WINDOW while the
        // EDITOR keeps focus and the document stays byte-identical. And the
        // palette's tie law rides REGISTRY ORDER, so the six task commands
        // APPEND after I2's four and I3's six — the older rows never move.
        writeFile(QStringLiteral("keep.py"), "keepme\n");
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"plain","command":"@FAKE@","default":true}]})"));
        window_->openProject(temp_->path());
        QVERIFY2(waitUntil([this] { return !window_->projectFilesWalking(); }),
                 "the pool walk never finished");
        QVERIFY(window_->openFile(temp_->filePath(QStringLiteral("keep.py"))));
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        const QString textBefore = editor->toPlainText();
        editor->setFocus();
        QTest::qWait(20);

        // THE REGISTRY: four (I2) + six (I3) + six (I4) — an append, and the
        // older rows in their frozen positions.
        window_->showCommandPalette();
        QCOMPARE(window_->commandRowCount(), 16);
        QCOMPARE(window_->commandLabelAt(0).contains(
            QStringLiteral("Быстрое открытие")), true); // I2's four unmoved
        QCOMPARE(window_->commandLabelAt(3).contains(
            QStringLiteral("Закрыть")), true);
        QCOMPARE(window_->commandLabelAt(4).contains(
            QStringLiteral("определени")), true);      // I3's six unmoved
        QCOMPARE(window_->commandLabelAt(10).contains(                   // I4's
            QStringLiteral("Запустить задачу по умолчанию")), true);     // six
        QCOMPARE(window_->commandLabelAt(11).contains(
            QStringLiteral("Выбрать задачу")), true);                    // append
        for (int row = 10; row < 16; ++row)
            QVERIFY2(window_->commandLabelAt(row).contains(QStringLiteral("адач")),
                     qPrintable(window_->commandLabelAt(row)));
        // A filtered row runs the RIGHT command (close-then-run):
        window_->typePaletteQuery(QStringLiteral("Остановить"));
        int stopRow = -1;
        for (int row = 0; row < window_->commandRowCount(); ++row) {
            if (window_->commandLabelAt(row).contains(QStringLiteral("Остановить")))
                stopRow = row;
        }
        QVERIFY(stopRow >= 0);
        QVERIFY(window_->runPaletteCommand(stopRow)); // nothing runs: the
        QVERIFY(!window_->commandPaletteVisible());   // refusal face is its own
        QVERIFY(window_->taskRunSummary().contains(   // honest truth
            QStringLiteral("нет выполняющейся задачи")));
        // And the SHOW-OUTPUT command really shows the pane:
        window_->showTaskOutput(false);
        window_->showCommandPalette();
        window_->typePaletteQuery(QStringLiteral("Показать вывод"));
        QVERIFY(window_->commandRowCount() >= 1);
        int showRow = -1;
        for (int row = 0; row < window_->commandRowCount(); ++row) {
            if (window_->commandLabelAt(row).contains(QStringLiteral("Показать вывод")))
                showRow = row;
        }
        QVERIFY(showRow >= 0);
        QVERIFY(window_->runPaletteCommand(showRow));
        QVERIFY(window_->taskOutputVisible());
        window_->closeOverlay();

        // Ctrl+Shift+B runs the DEFAULT task THROUGH THE WINDOW:
        QTest::keyClick(editor, Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY2(window_->taskRunSummary().contains(QStringLiteral("запущена")),
                 qPrintable(window_->taskRunSummary()));
        QCOMPARE(editor->toPlainText(), textBefore); // the view typed NOTHING
        QCOMPARE(editor, window_->currentEditor());  // and stole no focus
        QVERIFY2(i4RunFinished(), "the chorded task never finished");
        QVERIFY(window_->taskOutput().contains(QStringLiteral("FAKE-TASK-OK")));

        // Ctrl+Shift+R raises the PICKER through the WINDOW:
        QTest::keyClick(editor, Qt::Key_R, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(window_->taskPickerVisible());
        QCOMPARE(window_->taskPickerRowCount(), 1);
        QCOMPARE(editor->toPlainText(), textBefore);
        // Esc stays the POPUP's while its subtree has focus (child-scoped) and
        // the view's own frozen Escape chord stays the editor's (the I2 law,
        // now serving a third popup):
        QTest::qWait(20);
        QWidget* focused = QApplication::focusWidget();
        QVERIFY2(focused != nullptr, "the picker edit did not take focus");
        // The focus law's witness: the POPUP's edit holds focus (the picker
        // took it from the editor), which is what makes the CHILD-scoped Esc
        // the popup's and not the editor's. (An AMBIGUOUS Esc — two QActions
        // with the same shortcut on the same popup — was the bug this leg
        // caught: Qt matches neither, and the keyPress falls into the QLineEdit.
        // ONE Esc action per popup is the invariant, spelled at the ctor.)
        QVERIFY2(QString(focused->metaObject()->className())
                     == QLatin1String("QLineEdit"),
                 focused->metaObject()->className());
        QTest::keyClick(focused, Qt::Key_Escape);
        QVERIFY(!window_->taskPickerVisible());
        QCOMPARE(editor->toPlainText(), textBefore);

        // A chord with NO default task is a LOUD refusal, never silence:
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"plain","command":"@FAKE@","problemMatcher":"none"}]})"));
        window_->openProject(temp_->path());
        editor = window_->currentEditor(); // the keep.py tab rode the switch
        QVERIFY(editor);
        const QString stillOurs = editor->toPlainText();
        QTest::keyClick(editor, Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY2(window_->taskRunSummary().contains(QStringLiteral("по умолчанию")),
                 qPrintable(window_->taskRunSummary()));
        QVERIFY(!window_->taskRunning());
        QCOMPARE(editor->toPlainText(), stillOurs);
    }

    void i4TaskConfigOpensAsATabAndIsNeverCreated()
    {
        // THE DOGFOOD FACE: the IDE opens the file whose truth it just read —
        // and NEVER writes one (a config file the tool invents is a promise v1
        // did not earn). openTaskConfig IS openFile, so the one-tab law and the
        // openFile truth ride it for free.
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"plain","command":"@FAKE@"}]})"));
        window_->openProject(temp_->path());
        QVERIFY(window_->openTaskConfig());
        QCOMPARE(window_->editorCount(), 1);
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        QVERIFY(editor->toPlainText().contains(QStringLiteral("plain")));
        window_->openTaskConfig(); // the one-tab-per-path law rides this door
        QCOMPARE(window_->editorCount(), 1); // (it IS openFile's twin)
        // A root with NO tasks.json: openFile's honest refusal on the bar, and
        // the app creates nothing:
        QDir().mkpath(temp_->filePath(QStringLiteral("noconf")));
        window_->openProject(temp_->filePath(QStringLiteral("noconf")));
        QVERIFY(!window_->openTaskConfig());
        QVERIFY(window_->statusBar()->currentMessage().contains(
            QStringLiteral("Не удалось открыть")));
        QVERIFY(!QFileInfo(temp_->filePath(
                     QStringLiteral("noconf/.qmn/tasks.json"))).exists());
        // No project at all: the refusal is LOUD (the no-project law's twin):
        IdeWindow bare;
        bare.show();
        QVERIFY(!bare.openTaskConfig());
        QVERIFY(bare.taskRunSummary().contains(QStringLiteral("проект")));
    }

    void i4ProblemRowAndFeedCapsCountAndShowTheSkipped()
    {
        // THE SKIP-HONESTY LAW, riding the panel and the feed (the law I2 froze
        // for quick-open's rows): an excess is COUNTED and SHOWN, never silently
        // dropped and never silently truncated in the OTHER direction either —
        // the gutter truth is deliberately NOT capped by the row cap.
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"many1200","command":"@FAKE@","args":["many","1200"]},
            {"label":"many2500","command":"@FAKE@","args":["many","2500"]}]})"));
        window_->openProject(temp_->path());

        // Leg 1: the ROW cap (kProblemMaxRows=500) with the FEED whole. The
        // file opens AFTER the run that WRITES it (the far side's own law: it
        // ships the subject file it names), which is also how a real build
        // works — the file exists before the compiler names it.
        QVERIFY(window_->runTask(QStringLiteral("many1200")));
        QVERIFY2(i4RunFinished(), "the 1200-row run never finished");
        const QString fakeSource = i4Clean(window_->projectRoot()
                                           + QStringLiteral("/fake_src.cpp"));
        QVERIFY(window_->openFile(fakeSource)); // the FEED leg needs a live view
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        QCOMPARE(window_->problemFeedCount(), 1200); // the FEED is not capped
        QCOMPARE(window_->problemCount(), IdeWindow::kProblemMaxRows); // the PANEL is
        QVERIFY2(window_->searchSummary().contains(
                     QStringLiteral("ещё 700 не показаны")),
                 qPrintable(window_->searchSummary())); // 1200 - 500, LOUD
        QVERIFY(!window_->taskRunSummary().contains(
            QStringLiteral("матчер пропустил"))); // no FEED skip at 1200
        QCOMPARE(editor->diagnostics().size(), 1200); // THE GUTTER TRUTH STAYS
                                                      // WHOLE — the row cap is a
                                                      // PANEL cap, not a feed cap

        // Leg 2: the FEED cap (kProblemFeedMax=2000) — its face is the RUN
        // summary's "(матчер пропустил N)", and the panel's face still counts the
        // rows it could not show.
        QVERIFY(window_->runTask(QStringLiteral("many2500")));
        QVERIFY2(i4RunFinished(), "the 2500-row run never finished");
        QCOMPARE(window_->problemFeedCount(), IdeWindow::kProblemFeedMax);
        QCOMPARE(window_->problemCount(), IdeWindow::kProblemMaxRows);
        QVERIFY2(window_->taskRunSummary().contains(
                     QStringLiteral("(матчер пропустил 500)")), // 2500 - 2000
                 qPrintable(window_->taskRunSummary()));
        QVERIFY2(window_->searchSummary().contains(
                     QStringLiteral("ещё 1500 не показаны")), // 2000 - 500
                 qPrintable(window_->searchSummary()));
        QCOMPARE(editor->diagnostics().size(), IdeWindow::kProblemFeedMax);

        // And a run with FEWER rows than the caps shows every row, with neither
        // face present (the caps never invent a "skipped" that did not happen).
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"many12","command":"@FAKE@","args":["many","12"]}]})"));
        window_->openProject(temp_->path());
        QVERIFY(window_->runTask(QStringLiteral("many12")));
        QVERIFY2(i4RunFinished(), "the 12-row run never finished");
        QCOMPARE(window_->problemCount(), 12);
        QCOMPARE(window_->problemFeedCount(), 12);
        QVERIFY(!window_->searchSummary().contains(QStringLiteral("не показаны")));
        QVERIFY(!window_->taskRunSummary().contains(QStringLiteral("пропустил")));
    }

    void i4ARunNeverSavesOrRevertsADirtyDocument()
    {
        // LEDGER (f) AT THE DOOR IT WAS WRITTEN FOR: a run NEVER saves a buffer
        // and NEVER reverts one. The problems a task produces are the
        // COMPILER'S truth about the FILE on disk — so on a document the user
        // edited (dirty, unsaved), the row's coordinates stay the FILE's and the
        // buffer is left alone. The honest consequence is pinned AS the
        // divergence: the reveal lands on the buffer's line of the same number,
        // which after a top-inserted line is a DIFFERENT line of text. The app
        // never re-maps coordinates (no mirror face for a task — the task never
        // went through the syncer), and the IDE never saves on the user's behalf.
        i4WriteTasksHere(QStringLiteral(R"({"tasks":[
            {"label":"gcc","command":"@FAKE@","args":["gcc"]}]})"));
        window_->openProject(temp_->path());
        QVERIFY(window_->runTask(QStringLiteral("gcc")));
        QVERIFY2(i4RunFinished(), "the first run never finished");
        const QString fakeSource = i4Clean(window_->projectRoot()
                                           + QStringLiteral("/fake_src.cpp"));
        QVERIFY(window_->openFile(fakeSource));
        CodeEditor* editor = window_->currentEditor();
        QVERIFY(editor);
        const QString clean = editor->toPlainText();
        QVERIFY(clean.contains(QStringLiteral("return undeclared_thing + x;")));

        // DIRTY IT: a new blank line at the top (a one-line shift).
        editor->setPosition(TextPosition{0, 0});
        QTest::keyClick(editor, Qt::Key_Return);
        QVERIFY(editor->toPlainText() != clean); // dirty, and NOTHING saved it

        // The far side REWRITES the file (the same content every run — the file
        // is byte-fixed by design, which is what makes this leg readable).
        QVERIFY(window_->runTask(QStringLiteral("gcc")));
        QVERIFY2(i4RunFinished(), "the second run never finished");
        QVERIFY2(editor->toPlainText() != clean,
                 "the run SAVED or REVERTED the dirty buffer — the runner never "
                 "touches a document");
        QVERIFY2(editor->toPlainText().startsWith(QLatin1Char('\n')),
                 "the user's edit vanished anyway");
        QCOMPARE(window_->problemCount(), 4); // the rows are the FILE's truth

        // THE DIVERGENCE, PINNED: row 0 is the file's line 2 (0-based). In the
        // dirty buffer that line NUMBER now holds different text — and the reveal
        // rides the number, because a task's coordinates have no mirror face.
        QCOMPARE(window_->searchResultLineAt(0), 2);
        QVERIFY(window_->activateSearchResult(0));
        editor = window_->currentEditor();
        QCOMPARE(editor->position().line, 2); // the FILE's line number, applied
        QVERIFY(editor->hasSelection());      //   to the buffer as-is
        // (the buffer's line 2 is the file's old line 1 shifted down — the
        // compiler's column is a statement about the disk, not the editor)
        QVERIFY(!window_->taskRunSummary().contains(QStringLiteral("сохранен")));
        // the pane says NOTHING about saving: the law is silence + a refusal to
        // touch, not a warning the user has to read
    }
};

QTEST_MAIN(TestIde) // QTEST_MAIN (widget face, the tst_glue/tst_demo
                    // class): ctest runs it offscreen
#include "tst_ide.moc"
