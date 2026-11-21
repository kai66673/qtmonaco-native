// The G1 glue-shelf consumer smoke (PLAN "The glue shelf + ProjectSession"):
// an EXTERNAL project that does find_package(qmnGlue) against the INSTALLED
// shelf — proves the second export set, qmnGlueConfig (its find_dependency
// (qmn) load order), the glue header layout and the qmn::glue namespace all
// hold outside the build tree. NOTHING SPAWNS: the default hub's only
// built-in is clangd (c/cpp), so the doors exercised here are the ones that
// answer WITHOUT a server — clangd is not a CI guarantee (AGENTS). The D4
// DapHub doors ride the SAME smoke (the PLAN D4 record: doors compile,
// NOTHING spawns — startDebugging is never touched, its default factory
// WOULD spawn). Headless; exit 0 = every check passed.

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <cstdio>

#include "glue/DapHub.hpp"
#include "glue/LspHub.hpp"
#include "glue/ProjectSession.hpp"
#include "view/CodeEditor.hpp"

namespace {
int failures = 0;
} // namespace

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAILED: %s (line %d)\n", #cond, __LINE__);\
            ++failures;                                                    \
        }                                                                  \
    } while (false)

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    qmn::glue::LspHub hub;

    // The built-in matrix stands out of the box: clangd claims c AND cpp,
    // nothing claims python.
    CHECK(hub.specNameForLanguage(QStringLiteral("cpp")) == QStringLiteral("clangd"));
    CHECK(hub.specNameForLanguage(QStringLiteral("python")).isEmpty());

    // The config body door (no file IO): a deep-merged new spec.
    QJsonObject rust;
    rust.insert(QStringLiteral("program"), QStringLiteral("rust-analyzer"));
    rust.insert(QStringLiteral("languages"),
                QJsonArray{QStringLiteral("rust")});
    QJsonObject root;
    root.insert(QStringLiteral("servers"),
                QJsonObject{{QStringLiteral("rust-analyzer"), rust}});
    CHECK(hub.loadConfigFromJson(root));
    CHECK(hub.specNameForLanguage(QStringLiteral("rust"))
          == QStringLiteral("rust-analyzer"));
    CHECK(hub.serverSpec(QStringLiteral("rust-analyzer"))
              .value(QStringLiteral("program"))
              .toString()
          == QStringLiteral("rust-analyzer"));

    // The lazy-start contract: a spec nobody opened has NO client yet.
    CHECK(hub.clientForSpec(QStringLiteral("rust-analyzer")) == nullptr);

    // The honest no-op door: no spec claims python, so opening is FALSE and
    // nothing spawns (the .txt-file contract, AGENTS: the hub speaks ids).
    qmn::view::CodeEditor editor;
    editor.setLanguage(QStringLiteral("python"));
    CHECK(!hub.openDocument(&editor, QStringLiteral("file:///smoke.py"),
                            QStringLiteral("python")));
    CHECK(hub.syncerFor(&editor) == nullptr);

    // Plain lifecycle doors round-trip.
    hub.setWorkspaceRoot(QStringLiteral("/tmp"));
    CHECK(hub.workspaceRoot() == QStringLiteral("/tmp"));
    hub.setDebounceMs(120);
    CHECK(hub.debounceMs() == 120);
    hub.closeDocument(&editor); // unregistered: silent, the door exists

    // The G2 session doors ride the SAME shelf (the header installed under
    // qmn/glue): attached to the very same hub, a .py open resolves the
    // language through the session table, registers session-side, and the
    // hub's unclaimed-language no-op keeps the whole smoke spawn-free.
    qmn::glue::ProjectSession session;
    session.attachHub(&hub);
    CHECK(session.languageForSuffix(QStringLiteral(".PY")) == QStringLiteral("python"));
    CHECK(session.openDocument(&editor, QStringLiteral("/work/anything.py")));
    CHECK(editor.language() == QStringLiteral("python")); // the one-call setLanguage
    CHECK(session.openCount() == 1);
    CHECK(session.editorForUri(QStringLiteral("/work/anything.py")) == &editor);
    CHECK(session.closeUri(QStringLiteral("/work/anything.py")));
    CHECK(session.openCount() == 0);
    CHECK(hub.clientForSpec(QStringLiteral("clangd")) == nullptr); // never started

    // The shelf's THIRD member (D3/D4): the DapHub doors
    // compile+link across the SAME export boundary (qmn_glue PUBLIC-links
    // qmn_dap, so the DapClient*-naming client() door resolves) and NOTHING
    // SPAWNS — every door answers honestly on a session-less hub, and
    // startDebugging is deliberately untouched (its default factory builds
    // a real ProcessTransport; an empty program refuses BEFORE the factory
    // is ever reached, which is the spawn-free half pinned below).
    qmn::glue::DapHub dap;
    dap.attachProject(&session);
    CHECK(dap.client() == nullptr);
    CHECK(!dap.isDebugging());
    CHECK(!dap.isReady());
    CHECK(dap.adapterCapabilities().isEmpty());
    CHECK(!dap.setBreakpoints(&editor, {2})); // the doc was CLOSED above:
    CHECK(dap.requestedBreakpoints(&editor).isEmpty()); // no identity, refused
    CHECK(dap.breakpointMap(&editor).isEmpty());
    CHECK(!dap.startDebugging(QString())); // empty program: honest false,
    CHECK(dap.client() == nullptr);        // the factory NEVER touched
    CHECK(!dap.terminateDebugging());
    CHECK(dap.request(QStringLiteral("ping"),
                      QJsonValue(QJsonValue::Undefined)) == -1);
    dap.configurationDone(); // honest no-ops on a session-less hub:
    dap.stopDebugging();     // the doors exist and answer
    // The transport-factory seam compiles across the boundary (C++-only,
    // mirrors LspHub's; NEVER invoked here):
    dap.setTransportFactory([](const QString&, const QStringList& args,
                               QObject* parent) -> qmn::protocol::Transport* {
        Q_UNUSED(args); Q_UNUSED(parent);
        return nullptr; // nothing ever starts this hub
    });

    if (failures) {
        std::fprintf(stderr, "qmn glue consumer smoke: %d check(s) FAILED\n",
                     failures);
        return 1;
    }
    std::printf("qmn glue consumer smoke OK (installed shelf, no server and no adapter spawned)\n");
    return 0;
}
