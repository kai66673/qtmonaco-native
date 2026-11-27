// Drives the qmn-ide-demo app (GATE 4 car I0 skeleton + car I1 panel)
// fully offscreen through the SAME programmatic doors the dock widgets
// and panel buttons call — no dialog exec, no file dialogs, and NO
// server ever spawns (the --lsp door's tst_demo law rides whole: the
// one slot touching the hub opens only languages NO built-in spec
// claims). tst_ide never includes a glue or search header: IdeWindow.hpp
// keeps those types behind forward-decls, and the panel's own read doors
// (searchSummary/searchResultFileAt/…) are the observation channel.
// Assertions are on model/widget state, never on pixels.

#include "app/IdeWindow.hpp"

#include "view/CodeEditor.hpp"

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
