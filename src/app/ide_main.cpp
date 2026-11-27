// qmn-ide-demo's thin main (GATE 4 car I0): ALL window/panel logic lives
// in the qmn_ide_app static lib (IdeWindow) so tst_ide drives the exact
// doors the UI calls, offscreen (the qmn-demo shape, mirrored).
//
// The CLI face: one positional DIRECTORY = the project root (a file is a
// LOUD refusal on stderr + exit 1 — the tour law's CLI sibling: a door
// asked to do something that never happens never rides on silently);
// --exit-after-ms is the headless smoke door; --lsp arms the hub door
// (default OFF — tst_ide never spawns a server, and CI has no clangd
// guarantee, the demo law rides whole).

#include "IdeWindow.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qmn-ide-demo"));
    // Configured, never typed (the clientInfo version law from the 1.8.0
    // close audit): CMake hands this target QMN_PACKAGE_VERSION.
    QCoreApplication::setApplicationVersion(QStringLiteral(QMN_PACKAGE_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("qtmonaco-native IDE (GATE 4 dogfood): project tree + "
                       "editor tabs + Find-in-Files over qmn::search. "
                       "Positional: ONE project directory."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("project"),
                                 QStringLiteral("Project directory to open"),
                                 QStringLiteral("<project-dir>"));
    parser.addOption({QStringLiteral("exit-after-ms"),
                      QStringLiteral("Quit automatically after the given delay "
                                     "(headless smoke runs)"),
                      QStringLiteral("ms")});
    parser.addOption({QStringLiteral("lsp"),
                      QStringLiteral("Arm the LSP hub (built-in specs + "
                                     ".qmn/lsp.json under the project root) — "
                                     "off by default, like the demo's door")});
    parser.process(app);

    const QStringList positionals = parser.positionalArguments();
    if (positionals.size() > 1) {
        fprintf(stderr, "qmn-ide-demo: exactly ONE project directory "
                        "expected, got %d paths — run --help\n",
                positionals.size());
        return 1;
    }

    qmn::app::IdeWindow window;
    if (!positionals.isEmpty()) {
        const QFileInfo info(positionals.first());
        if (!info.isDir()) {
            fprintf(stderr, "qmn-ide-demo: the project must be a DIRECTORY "
                            "(got '%s') — run --help\n",
                    positionals.first().toLocal8Bit().constData());
            return 1;
        }
        // The root FIRST, the hub SECOND: openProject folds .qmn/lsp.json
        // into the session and setLspEnabled replays the already-read
        // config (the demo reads config at ctor time; the IDE knows its
        // root earlier and folds at the root door).
        window.openProject(info.absoluteFilePath());
    }
    if (parser.isSet(QStringLiteral("lsp")))
        window.setLspEnabled(true);
    window.show();

    const QString exitAfterMs = parser.value(QStringLiteral("exit-after-ms"));
    if (!exitAfterMs.isEmpty())
        QTimer::singleShot(exitAfterMs.toInt(), &app, &QApplication::quit);
    return app.exec();
}
