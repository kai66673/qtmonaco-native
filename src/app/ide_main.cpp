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

// THE CLI TASK FACE'S EXIT MAPPING (decided at the I4 close, see the site at
// the end of main): the shell's exit slot holds 0…255, and the runner's own
// sentinels — IdeWindow::kTaskExitCrashed (signal death / killed),
// kTaskExitFailedToStart, kTaskExitPending (which never reaches this mapping:
// the poll only calls it once the run ENDED) — plus any code over 255 do not
// fit, so those all become 1 ("not success") while the printed summary carries
// WHICH kind. A real child code 1…255 rides VERBATIM: the far side's exit code
// is the oracle (the fakeserver/dapfake/faketask law).
int cliTaskExitStatus(int taskExitCode)
{
    if (taskExitCode == 0)
        return 0;
    if (taskExitCode > 0 && taskExitCode <= 255)
        return taskExitCode;
    return 1;
}

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
    parser.addOption({QStringLiteral("task"),
                      QStringLiteral("Run ONE configured task from .qmn/tasks.json "
                                     "and quit when it finishes (car I4). Explicit "
                                     "by construction — a task NEVER auto-runs; "
                                     "--exit-after-ms is a cap here, not the "
                                     "lifetime."),
                      QStringLiteral("label")});
    parser.addOption({QStringLiteral("task-default"),
                      QStringLiteral("Run the \"default\": true task instead of "
                                     "naming one (same I4 face as Ctrl+Shift+B)")});
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

    // ---- car I4: the task face (the CLI door of the runner) --------------
    // Explicit by construction: a task NEVER auto-runs, and the CLI asks
    // LOUDLY when it is asked to run one with no project to read the config
    // from (the tour law's CLI sibling). The MEASURED face prints to stderr
    // when the run ENDS — that is what the repo-build ritual's numbers ride
    // (PLAN's opening record: a live ritual, never a ctest entry, and NO
    // wall-clock ever pinned).
    const QString taskLabel = parser.value(QStringLiteral("task"));
    const bool askDefault = parser.isSet(QStringLiteral("task-default"));
    if (!taskLabel.isEmpty() && askDefault) {
        fprintf(stderr, "qmn-ide-demo: --task and --task-default are two asks — "
                        "pick one\n");
        return 1;
    }
    const bool askTask = !taskLabel.isEmpty() || askDefault;
    int taskRc = 0; // the CLI's own exit code when a task was asked (see the
                    // mapping helper above; a never-finishing run leaves 0)
    if (askTask) {
        if (positionals.isEmpty()) {
            fprintf(stderr, "qmn-ide-demo: --task needs a project directory "
                            "(the config lives in <root>/.qmn/tasks.json)\n");
            return 1;
        }
        const bool started = askDefault ? window.runDefaultTask()
                                        : window.runTask(taskLabel);
        if (!started) {
            fprintf(stderr, "qmn-ide-demo: задача отклонена: %s\n",
                    window.taskRunSummary().toLocal8Bit().constData());
            return 1;
        }
        auto* poll = new QTimer(&app);
        poll->setInterval(25);
        QObject::connect(poll, &QTimer::timeout, &app, [&window, poll, &taskRc] {
            if (window.taskRunning())
                return; // the run's own state is the only face polled (NO
            poll->stop(); //  wall-clock is ever asserted — the far side's
            taskRc = cliTaskExitStatus(window.taskExitCode());
            fprintf(stderr, "%s\n", //  exit code is the oracle)
                    window.taskRunSummary().toLocal8Bit().constData());
            QApplication::quit();
        });
        poll->start();
    }

    const QString exitAfterMs = parser.value(QStringLiteral("exit-after-ms"));
    if (!exitAfterMs.isEmpty())
        QTimer::singleShot(exitAfterMs.toInt(), &app, &QApplication::quit);
    app.exec();
    // THE CLI'S EXIT CODE IS THE TASK'S (decided at the I4 close): the face is
    // "run this task and tell me the truth", and a build wrapper that reports 0
    // for a failing build is a LIE. The mapping and its honest limit:
    //   0            -> 0
    //   1…255        -> the child's own code, VERBATIM (the far-side oracle law)
    //   anything else-> 1 — the shell's exit slot cannot hold a signal death,
    //                   a failed spawn or a code over 255, so the process says
    //                   "not success" and the printed summary carries WHICH
    //                   kind (crashed / killed / could not start). Never a fake
    //                   0, never an invented child number.
    // A run that never finished (the --exit-after-ms cap quit the app first)
    // leaves taskRc at 0 for the APP's own success — the cap is the smoke door,
    // and the run's face stays in the window's doors, not in the exit code.
    return taskRc;
}
