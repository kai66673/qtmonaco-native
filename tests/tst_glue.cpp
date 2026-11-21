// The 6th ctest entry (G2 of the glue shelf, PLAN "the glue shelf +
// ProjectSession"): the document session over REAL CodeEditors — language
// resolution, the one-call open, the registry's lifecycle doors, the
// config fold feeding BOTH sides, and the hub attached-but-never-serving
// path. NO test here may open a language a built-in spec claims (clangd is
// not a CI guarantee, AGENTS): the claimed-server machinery lives in
// tst_lsp with the fakeserver; this binary proves the session's own shape.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "glue/LspHub.hpp"
#include "glue/ProjectSession.hpp"
#include "view/CodeEditor.hpp"

class TestGlue : public QObject {
    Q_OBJECT
private slots:
    void suffixTableGuessesTheFiveGrammars()
    {
        qmn::glue::ProjectSession session;
        QCOMPARE(session.languageForSuffix(QStringLiteral("py")),
                 QStringLiteral("python"));
        QCOMPARE(session.languageForSuffix(QStringLiteral("json")),
                 QStringLiteral("json"));
        for (const QString js : { QStringLiteral("js"), QStringLiteral("mjs"),
                                  QStringLiteral("cjs") })
            QCOMPARE(session.languageForSuffix(js), QStringLiteral("javascript"));
        QCOMPARE(session.languageForSuffix(QStringLiteral("c")), QStringLiteral("c"));
        for (const QString h : { QStringLiteral("h"), QStringLiteral("cpp"),
                                 QStringLiteral("cc"), QStringLiteral("cxx"),
                                 QStringLiteral("hpp"), QStringLiteral("hh"),
                                 QStringLiteral("hxx") })
            QCOMPARE(session.languageForSuffix(h), QStringLiteral("cpp"));

        // Unmapped stays honest, and lookup forgives case and a leading dot.
        QVERIFY(session.languageForSuffix(QStringLiteral("txt")).isEmpty());
        QVERIFY(session.languageForSuffix(QString()).isEmpty());
        QCOMPARE(session.languageForSuffix(QStringLiteral(".PY")),
                 QStringLiteral("python"));
        QCOMPARE(session.languageForSuffix(QStringLiteral("HPP")),
                 QStringLiteral("cpp"));
    }

    void overridesAndConfigRemapAndUnmap()
    {
        qmn::glue::ProjectSession session;

        session.setLanguageOverride(QStringLiteral("txt"),
                                    QStringLiteral("python"));
        QCOMPARE(session.languageForSuffix(QStringLiteral("txt")),
                 QStringLiteral("python"));
        session.setLanguageOverride(QStringLiteral(".TXT"), QString());
        QVERIFY(session.languageForSuffix(QStringLiteral("txt")).isEmpty());

        // An override may also DROP a built-in mapping (the honest "these
        // .h files are not C++" case) without touching its siblings.
        session.setLanguageOverride(QStringLiteral("h"), QString());
        QVERIFY(session.languageForSuffix(QStringLiteral("h")).isEmpty());
        QCOMPARE(session.languageForSuffix(QStringLiteral("hpp")),
                 QStringLiteral("cpp"));

        // The config body door: "languages" folds over the table, and a
        // config CAN unmap a built-in with an empty id.
        QJsonObject languages;
        languages.insert(QStringLiteral("myext"), QStringLiteral("json"));
        languages.insert(QStringLiteral("py"), QString());
        QJsonObject root;
        root.insert(QStringLiteral("languages"), languages);
        QVERIFY(session.loadConfigFromJson(root));
        QCOMPARE(session.languageForSuffix(QStringLiteral("myext")),
                 QStringLiteral("json"));
        QVERIFY(session.languageForSuffix(QStringLiteral("py")).isEmpty());
        // A body with nothing foldable is an honest false.
        QVERIFY(!session.loadConfigFromJson(QJsonObject {
            { QStringLiteral("languages"), QJsonObject {} } }));
    }

    void openRegistersResolvesAndNeverFights()
    {
        qmn::glue::ProjectSession session;
        QSignalSpy opened(&session, &qmn::glue::ProjectSession::documentOpened);
        QSignalSpy closed(&session, &qmn::glue::ProjectSession::documentClosed);

        // Virtual absolute paths: normalization is pure string math (no
        // FS touch), so the key is deterministic under ctest's cwd.
        const QString pyPath = QStringLiteral("/work/project/sample.py");

        qmn::view::CodeEditor py;
        QVERIFY(session.openDocument(&py, pyPath));
        QCOMPARE(session.openCount(), 1);
        QCOMPARE(session.openUris(), QStringList { session.uriForEditor(&py) });
        QVERIFY(session.uriForEditor(&py)
                == QUrl::fromLocalFile(pyPath).toString()); // the KEY is always the uri form
        QVERIFY(session.openUris().first().startsWith(QLatin1String("file://")));
        QCOMPARE(py.language(), QStringLiteral("python")); // setLanguage ran
        // The path door re-normalizes to the SAME key as the uri door.
        QCOMPARE(session.languageForUri(session.uriForEditor(&py)),
                 QStringLiteral("python"));
        QCOMPARE(session.languageForUri(pyPath), QStringLiteral("python"));
        QVERIFY(session.editorForUri(pyPath) == &py);
        QCOMPARE(opened.count(), 1);

        // Re-opening the SAME editor+uri is an idempotent true (no second
        // registration, no second signal).
        QVERIFY(session.openDocument(&py, session.uriForEditor(&py)));
        QCOMPARE(session.openCount(), 1);
        QCOMPARE(opened.count(), 1);

        // A DIFFERENT editor claiming the same uri: the honest refusal.
        qmn::view::CodeEditor squatter;
        QVERIFY(!session.openDocument(&squatter, pyPath));
        QCOMPARE(session.openCount(), 1);
        QVERIFY(session.editorForUri(pyPath) == &py);
        QVERIFY(session.uriForEditor(&squatter).isEmpty());

        // Bad inputs are false without a trace.
        QVERIFY(!session.openDocument(nullptr, pyPath));
        QVERIFY(!session.openDocument(&squatter, QString()));

        // Never fights the app: an UNmapped suffix leaves the editor's
        // language exactly as it was (and the session records "" for it).
        qmn::view::CodeEditor bin;
        bin.setLanguage(QStringLiteral("json"));
        QVERIFY(session.openDocument(&bin, QStringLiteral("/work/project/notes.bin")));
        QCOMPARE(bin.language(), QStringLiteral("json"));
        QVERIFY(session.languageForUri(QStringLiteral("/work/project/notes.bin")).isEmpty());
        QCOMPARE(session.openCount(), 2);

        // The languageOverride door BEATS the suffix table (.py would be
        // python; the caller said json) and reaches the editor.
        qmn::view::CodeEditor weird;
        QVERIFY(session.openDocument(&weird, QStringLiteral("/work/project/other.py"),
                                     QStringLiteral("json")));
        QCOMPARE(weird.language(), QStringLiteral("json"));
        QCOMPARE(session.openCount(), 3);

        // Close doors: by editor (orderly, forwards to the hub below) and
        // by uri; unregistered/bad closes stay silent no-ops.
        session.closeDocument(&weird);
        QCOMPARE(session.openCount(), 2);
        QVERIFY(session.closeUri(QStringLiteral("/work/project/notes.bin")));
        QVERIFY(!session.closeUri(QStringLiteral("/work/project/notes.bin")));
        session.closeDocument(&squatter); // unregistered: silent
        QCOMPARE(session.openCount(), 1);
        QCOMPARE(closed.count(), 2);
        QVERIFY(session.editorForUri(QStringLiteral("/work/project/notes.bin")) == nullptr);
        QVERIFY(session.languageForUri(QStringLiteral("/work/project/notes.bin")).isEmpty());

        // A RELATIVE path normalizes against the process cwd (documented:
        // the door takes paths as QFileInfo sees them).
        qmn::view::CodeEditor rel;
        QVERIFY(session.openDocument(&rel, QStringLiteral("rel.py")));
        QCOMPARE(session.uriForEditor(&rel),
                 QUrl::fromLocalFile(QFileInfo(QStringLiteral("rel.py"))
                                         .absoluteFilePath())
                     .toString());
        QVERIFY(session.closeUri(QStringLiteral("rel.py")));
        QCOMPARE(session.openCount(), 1);

        // The popups-on-open knob round-trips (default ON, the demo shape).
        QVERIFY(session.popupsOnOpen());
        session.setPopupsOnOpen(false);
        QVERIFY(!session.popupsOnOpen());
    }

    void editorDeathDropsTheDocument()
    {
        qmn::glue::ProjectSession session;
        QSignalSpy closed(&session, &qmn::glue::ProjectSession::documentClosed);

        auto* doomed = new qmn::view::CodeEditor;
        QVERIFY(session.openDocument(doomed, QStringLiteral("doomed.py")));
        const QString uri = session.uriForEditor(doomed);
        QVERIFY(!uri.isEmpty());
        QCOMPARE(session.openCount(), 1);

        delete doomed; // destroyed() fires synchronously in the dtor
        QCOMPARE(session.openCount(), 0);
        QCOMPARE(closed.count(), 1);
        QCOMPARE(closed.at(0).at(0).toString(), uri);
        QVERIFY(session.editorForUri(uri) == nullptr);
    }

    void configFileFoldsBothSidesAndReplaysOnLateAttach()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral(".qmn")));
        {
            QFile file(dir.path() + QStringLiteral("/.qmn/lsp.json"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(R"JSON({
  "languages": { "myext": "json", "h": "" },
  "servers": { "rust-analyzer": { "program": "rust-analyzer",
                                  "languages": ["rust"] } }
})JSON");
        }

        qmn::glue::ProjectSession session;
        qmn::glue::LspHub hub; // NEVER started in this slot (clangd rule)
        session.setWorkspaceRoot(dir.path());
        QCOMPARE(session.workspaceRoot(), dir.path());

        // The fold happens session-side first: NO hub attached yet, so the
        // servers were read but nobody holds them (the late-attach replay
        // below is what makes that honest, not lossy).
        QVERIFY(session.loadConfig());
        QCOMPARE(session.languageForSuffix(QStringLiteral("myext")),
                 QStringLiteral("json"));
        QVERIFY(session.languageForSuffix(QStringLiteral("h")).isEmpty()); // UNMAPPED
        QCOMPARE(session.languageForSuffix(QStringLiteral("hpp")),
                 QStringLiteral("cpp")); // siblings survive
        QVERIFY(hub.specNameForLanguage(QStringLiteral("rust")).isEmpty());

        session.attachHub(&hub); // the LATE attach
        QCOMPARE(hub.workspaceRoot(), dir.path()); // the root forwarded
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("rust")),
                 QStringLiteral("rust-analyzer")); // the replay landed
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("cpp")),
                 QStringLiteral("clangd")); // built-ins stand under the merge

        // A hub dying mid-session simply unattaches.
        {
            qmn::glue::LspHub transient;
            session.attachHub(&transient);
            QVERIFY(session.hub() == &transient);
        }
        QVERIFY(session.hub() == nullptr);

        // Missing/malformed config: honest false, tables untouched.
        qmn::glue::ProjectSession nowhere;
        QVERIFY(!nowhere.loadConfig()); // no root: silent
        QTemporaryDir empty;
        QVERIFY(empty.isValid());
        nowhere.setWorkspaceRoot(empty.path());
        QVERIFY(!nowhere.loadConfig()); // no file: silent
        QCOMPARE(nowhere.languageForSuffix(QStringLiteral("py")),
                 QStringLiteral("python"));
        QVERIFY(QDir(empty.path()).mkpath(QStringLiteral(".qmn")));
        {
            QFile bad(empty.path() + QStringLiteral("/.qmn/lsp.json"));
            QVERIFY(bad.open(QIODevice::WriteOnly));
            bad.write("{ not json");
        }
        QVERIFY(!nowhere.loadConfig()); // malformed: the WHOLE file refused
    }

    void attachedHubServesOnlyClaimedLanguagesAndNeverSpawns()
    {
        qmn::glue::ProjectSession session;
        qmn::glue::LspHub hub;
        session.attachHub(&hub);

        // python: no built-in spec claims it, so the hub's openDocument is
        // its documented false no-op — the document lives session-side and
        // NO server ever lazy-starts.
        qmn::view::CodeEditor py;
        QVERIFY(session.openDocument(&py, QStringLiteral("unclaimed.py")));
        QCOMPARE(py.language(), QStringLiteral("python"));
        QVERIFY(hub.syncerFor(&py) == nullptr);
        QVERIFY(hub.clientForSpec(QStringLiteral("clangd")) == nullptr);

        // Resolution still reports cpp for .cpp — but NO openDocument on a
        // cpp doc here: the built-in clangd spec would lazy-start (AGENTS:
        // clangd is not a CI guarantee; the claimed path is tst_lsp's
        // fakeserver turf).
        QCOMPARE(session.languageForSuffix(QStringLiteral("cpp")),
                 QStringLiteral("cpp"));

        // Closing an editor the hub never served: the hub-side no-op rides
        // along silently, the session-side close is real.
        session.closeDocument(&py);
        QCOMPARE(session.openCount(), 0);
        QVERIFY(hub.syncerFor(&py) == nullptr);
    }
};

QTEST_MAIN(TestGlue)
#include "tst_glue.moc"
