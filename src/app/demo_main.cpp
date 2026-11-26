#include "MainWindow.hpp"
#include "ShowcaseTour.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

namespace {

// S1-4 (--tour): the LIVE shape resolves its fixtures from the build tree
// (the ctest face gets the same paths as compile-time defines). The guess
// rides the shipped layout — build/src/app/qmn-demo, build/tests/<fixture>
// — and every one of them is overridable by env, so the ritual is runnable
// from any build dir and honest when a fixture is missing (the leg prints
// a SKIP, never a silent drop).
QString envOrPath(const char* envName, const QStringList& candidates)
{
    const QString fromEnv = qEnvironmentVariable(envName);
    if (!fromEnv.isEmpty())
        return fromEnv;
    for (const QString& candidate : candidates)
        if (QFileInfo::exists(candidate))
            return QFileInfo(candidate).absoluteFilePath();
    return QString();
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qmn-demo"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    // S1-1 --diff <original> <modified>. QCommandLineParser CANNOT carry
    // this shape: a value option collects ONE token (the next lands as a
    // positional — measured on 6.12: `--diff A B` yielded values=[A]), and
    // the four-arg braced addOption spelling that "looks" like two value
    // names is a SILENT misbind (QCommandLineOption's 4th argument is the
    // DEFAULT VALUE — the trio never opened and the exit stayed 0). The
    // door therefore PRE-SCANS the raw argv before the parser sees it:
    // `--diff` must be followed by exactly two non-flag tokens or the call
    // is LOUD (stderr + exit 1, never the silent-zero pre-fix shape).
    QStringList rawArgs;
    for (int i = 1; i < argc; ++i)
        rawArgs << QString::fromLocal8Bit(argv[i]);
    QStringList diffPair;
    const int diffAt = rawArgs.indexOf(QStringLiteral("--diff"));
    if (diffAt != -1) {
        if (diffAt + 2 >= rawArgs.size() || rawArgs.at(diffAt + 1).startsWith(u'-')
            || rawArgs.at(diffAt + 2).startsWith(u'-')) {
            fprintf(stderr, "qmn-demo: --diff needs EXACTLY two files "
                            "(original modified) — run --help\n");
            return 1;
        }
        diffPair << rawArgs.at(diffAt + 1) << rawArgs.at(diffAt + 2);
        rawArgs.remove(diffAt, 3); // the parser never sees the trio's three tokens
    }
    // QCommandLineParser::process(const QStringList&) skips the FIRST
    // element as the application name (measured on 6.12: passing the
    // bare trimmed list dropped `--exit-after-ms 300` — the option name
    // ate as argv[0], 300 landed as a positional, and with no timer the
    // app hung forever). argv[0] therefore rides the front of the list.
    rawArgs.prepend(QString::fromLocal8Bit(argv[0]));

    // The parser runs over the TRIMMED QStringList (the parser's argv-free
    // entry point; nothing of the original argv survives past the scan).
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("qtmonaco-native demo: tabbed editors + a settings dialog "
                       "(Файл->Открыть…, Настройки->Параметры редактора…). "
                       "--diff <original> <modified> opens a DIFF trio: both "
                       "sides as plain tabs PLUS a focused third diff tab "
                       "(F7 / Shift+F7 walk the hunks there)."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("files"),
                                 QStringLiteral("Text files to open as tabs"),
                                 QStringLiteral("[files...]"));
    parser.addOption({QStringLiteral("exit-after-ms"),
                      QStringLiteral("Quit automatically after the given delay "
                                     "(headless smoke runs)"),
                      QStringLiteral("ms")});
    parser.addOption({QStringLiteral("lsp"),
                      QStringLiteral("Enable the LSP hub: built-in specs "
                                     "(clangd for c/cpp) + .qmn/lsp.json, "
                                     "completion/hover popups on typing and "
                                     "idle caret moves")});
    parser.addOption({QStringLiteral("dap"),
                      QStringLiteral("Enable the DAP hub (car 4): start ONE debug "
                                     "session through the named <adapter-program> "
                                     "(lldb-dap, gdb -i=dap, ...; the adapter argv "
                                     "follows as repeatable --dap-arg). The session "
                                     "begins with the first opened tab."),
                      QStringLiteral("program")});
    parser.addOption({QStringLiteral("dap-arg"),
                      QStringLiteral("One argument for the adapter program "
                                     "(repeatable, order-preserving)"),
                      QStringLiteral("arg")});
    parser.addOption({QStringLiteral("dap-program"),
                      QStringLiteral("What to DEBUG (the launch 'program'; default: "
                                     "the first opened tab's own file)"),
                      QStringLiteral("path")});
    // S1-3: the control-door flags. They do NOT call the doors at startup
    // (that could only ever refuse — the session is async); they queue
    // presses into the session's own event stream, walking pause -> step
    // -> pause -> continue -> continued -> stop hands-off.
    parser.addOption({QStringLiteral("debug-step"),
                      QStringLiteral("Press Step (DAP `next`) at the first "
                                     "pause (needs --dap; S1-3)")});
    parser.addOption({QStringLiteral("debug-continue"),
                      QStringLiteral("Press Continue at the next pause after "
                                     "the ride's step (needs --dap; S1-3)")});
    parser.addOption({QStringLiteral("debug-stop"),
                      QStringLiteral("Press Stop (terminate-first where the "
                                     "caps allow) after the ride's continue, "
                                     "or at the first pause if nothing rides "
                                     "before it (needs --dap; S1-3)")});
    // S1-2 --grammar (+ companions). ONE value per occurrence each — the
    // S1-1 lesson says a Qt6 value option never collects two tokens, so
    // the door's optional tail rides as its OWN repeatable flags, zipped
    // by index below (occurrence N of --grammar pairs with occurrence N of
    // each companion; missing companions = the door's defaults).
    parser.addOption({QStringLiteral("grammar"),
                      QStringLiteral("Load a grammar library (.so) and register "
                                     "it (S1-2; repeatable). The id defaults to "
                                     "the library stem; --grammar-id/-symbol/-lang/"
                                     "-query/-suffix refine occurrence N."),
                      QStringLiteral("file")});
    parser.addOption({QStringLiteral("grammar-id"),
                      QStringLiteral("Registry id for the N-th --grammar"),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("grammar-symbol"),
                      QStringLiteral("Exported symbol for the N-th --grammar "
                                     "(default: tree_sitter_<stem>)"),
                      QStringLiteral("symbol")});
    parser.addOption({QStringLiteral("grammar-lang"),
                      QStringLiteral("Which tabs to re-assert after the N-th "
                                     "load (default: the id)"),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("grammar-query"),
                      QStringLiteral(".scm highlights source for the N-th "
                                     "--grammar (default: parse-only)"),
                      QStringLiteral("scm")});
    parser.addOption({QStringLiteral("grammar-suffix"),
                      QStringLiteral("Bind a file suffix to a loaded grammar "
                                     "id for FUTURE tabs (repeatable, '<suffix:"
                                     "<id>'; the session's table, G2)"),
                      QStringLiteral("sfx:id")});
    // S1-4: the tour flag. A BOOL option on the parser (--tour takes no
    // value — the fixture tail never reaches a Qt6 value option, the S1-1
    // lesson), and it must be registered here because Qt6's parser rejects
    // UNKNOWN options before any hand-written scan runs (measured: `--tour`
    // with only --diff pre-scanned died as "Unknown options: tour").
    parser.addOption({QStringLiteral("tour"),
                      QStringLiteral("Run the SHARED showcase pin table (S1-4) "
                                     "over this app's own doors and exit with "
                                     "its result (0 green / 1 first FAIL): "
                                     "boot, model, highlight, diff, settings, "
                                     "grammar, clangd, dapfake, the live-DAP "
                                     "walk, teardown. Fixtures come from the "
                                     "build tree next to this binary or from "
                                     "QMN_TOUR_* env overrides; a leg with no "
                                     "fixture prints an honest SKIP. Optional "
                                     "tail: --tour <fixture-dir> [dapfake [bogus "
                                     "grammar]] — exactly the shapes the "
                                     "QMN_TOUR_* names carry.")});
    parser.process(rawArgs); // the trimmed list (parser's argv-free entry)

    // --tour's optional POSITIONAL tail (the SAME meaning as the
    // QMN_TOUR_* env names; env wins when both speak):
    //   --tour [fixture-dir [dapfake [bogus-grammar]]]
    // The positionals are ONLY read when --tour is set: an interactive
    // `qmn-demo --tour bigfile.py` opens bigfile.py as a tab AND runs the
    // tour over it (the tabs the tour opens join them; the table's pins
    // speak their own names). Scoping the live legs rides the env
    // (QMN_TOUR_LLDBDAP/QMN_TOUR_GDB set a path, QMN_TOUR_NO_LIVE_DAP=1
    // prints them as skips) — a BOOL tail never reaches a value option.
    QString tourCorpusDir, tourDapfake, tourBogus;
    if (parser.isSet(QStringLiteral("tour"))) {
        const QStringList tail = parser.positionalArguments();
        if (tail.size() > 3) {
            fprintf(stderr, "qmn-demo: --tour takes AT MOST three tail paths "
                            "(fixture-dir [dapfake [bogus-grammar]]) — run "
                            "--help\n");
            return 1;
        }
        if (tail.size() >= 1)
            tourCorpusDir = tail.at(0);
        if (tail.size() >= 2)
            tourDapfake = tail.at(1);
        if (tail.size() >= 3)
            tourBogus = tail.at(2);
    }

    qmn::app::MainWindow window;
    if (parser.isSet(QStringLiteral("lsp")))
        window.setLspEnabled(true); // BEFORE the tabs: openPath wires them
    if (parser.isSet(QStringLiteral("dap")))
        window.setDapAdapter(parser.value(QStringLiteral("dap")),
                             parser.values(QStringLiteral("dap-arg")),
                             parser.value(QStringLiteral("dap-program")));
    // S1-3: the ride queue (the natural execution order, not the flag
    // order on the command line — a stop before a step would just stall
    // until the next stop anyway; the queue is a WALK). LOUD on flags
    // without --dap: they could only ever refuse (the S1-1/S1-2 loud
    // face — a door asked to do something that never happens must not
    // ride on silently).
    QStringList debugRide;
    if (parser.isSet(QStringLiteral("debug-step")))
        debugRide << QStringLiteral("step");
    if (parser.isSet(QStringLiteral("debug-continue")))
        debugRide << QStringLiteral("continue");
    if (parser.isSet(QStringLiteral("debug-stop")))
        debugRide << QStringLiteral("stop");
    if (!debugRide.isEmpty()) {
        if (!window.dapEnabled()) {
            fprintf(stderr, "qmn-demo: --debug-step/-continue/-stop need "
                            "--dap <adapter-program> — no adapter armed\n");
            return 1;
        }
        window.queueDebugRide(debugRide);
    }
    for (const QString& path : parser.positionalArguments())
        window.openPath(path); // CLI failures land in the status bar, like menu opens
    if (!diffPair.isEmpty())
        window.openDiffPair(diffPair.at(0), diffPair.at(1));

    // S1-2: zip the repeatable --grammar occurrences with their
    // companions BY INDEX (a companion list shorter than N contributes
    // the empty default). LOUD + exit 1 on a load failure — a demo that
    // was told to load a grammar and didn't must never ride on silently
    // (the S1-1 --diff face's rule). --grammar-suffix pairs ride LAST:
    // the binding only means anything once its id registered (a session
    // table naming an absent id is the view's honest plain-text state,
    // but the demo's order keeps the intent obvious).
    const QStringList grammarFiles = parser.values(QStringLiteral("grammar"));
    const QStringList grammarIds = parser.values(QStringLiteral("grammar-id"));
    const QStringList grammarSymbols = parser.values(QStringLiteral("grammar-symbol"));
    const QStringList grammarLangs = parser.values(QStringLiteral("grammar-lang"));
    const QStringList grammarQueries = parser.values(QStringLiteral("grammar-query"));
    auto companionAt = [](const QStringList& list, int index) {
        return index < list.size() ? list.at(index) : QString();
    };
    for (int i = 0; i < grammarFiles.size(); ++i) {
        if (!window.loadGrammarLibrary(grammarFiles.at(i),
                                       companionAt(grammarIds, i),
                                       companionAt(grammarLangs, i),
                                       companionAt(grammarSymbols, i),
                                       companionAt(grammarQueries, i))) {
            fprintf(stderr, "qmn-demo: --grammar failed on \u2018%s\u2019 "
                            "\u2014 see the status-bar message\n",
                    qPrintable(grammarFiles.at(i)));
            return 1;
        }
    }
    for (const QString& pair : parser.values(QStringLiteral("grammar-suffix"))) {
        const int sep = pair.indexOf(u':');
        if (sep <= 0 || sep == pair.size() - 1) {
            fprintf(stderr, "qmn-demo: --grammar-suffix wants '<suffix:<id>', "
                            "got \u2018%s\u2019\n", qPrintable(pair));
            return 1;
        }
        window.setLanguageForSuffix(pair.left(sep), pair.mid(sep + 1));
    }
    window.show();

    // S1-4: the tour runs OVER the tabs the CLI just opened (one window,
    // the app itself) and its result IS the process exit — the LIVE
    // ritual's face (never a ctest entry; tst_demo pins the TABLE through
    // runShowcaseTour with the live legs scoped off). Fixtures: the
    // QMN_TOUR_* env names win, the --tour positional tail rides, the
    // last fallback is the build tree next to THIS executable (the
    // shipped layout build/src/app/qmn-demo -> build/tests/<fixtures>);
    // a fixture that can't be found prints its leg as an honest SKIP.
    if (parser.isSet(QStringLiteral("tour"))) {
        qmn::app::TourConfig config;
        const QString exeDir = QFileInfo(QCoreApplication::arguments().value(0))
                                   .absoluteDir().absolutePath(); // <build>/src/app
        const QString buildDir = QFileInfo(exeDir + QStringLiteral("/../.."))
                                     .absoluteFilePath(); // <build>
        config.pyGrammarLib = envOrPath("QMN_TOUR_PYGRAMMAR",
            { buildDir + QStringLiteral("/tests/libqmn_pygrammar.so") });
        config.pyQueryScm = envOrPath("QMN_TOUR_PYQUERY",
            { QStringLiteral(QMN_TOUR_PYQUERY_DEFAULT) });
        config.fakeGrammarLib = tourBogus.isEmpty()
            ? envOrPath("QMN_TOUR_FAKEGRAMMAR",
                { buildDir + QStringLiteral("/tests/libbogus.so") }) // OUTPUT_NAME "bogus" (tests/CMakeLists.txt)
            : tourBogus;
        config.dapfakePath = tourDapfake.isEmpty()
            ? envOrPath("QMN_TOUR_DAPFAKE",
                { buildDir + QStringLiteral("/tests/dapfake") })
            : tourDapfake;
        // The live-DAP scoping (the owner's 2026-11-26 record): an
        // explicitly NAMED adapter path rides as-is and a missing named
        // path fails LOUD ("a missing named path fails loud" — plan's
        // words); otherwise PATH discovery, with a NO_LIVE_DAP kill
        // switch that prints those legs as skips.
        const QString namedLldb = qEnvironmentVariable("QMN_TOUR_LLDBDAP");
        const QString namedGdb = qEnvironmentVariable("QMN_TOUR_GDB");
        const bool noLive = qEnvironmentVariable("QMN_TOUR_NO_LIVE_DAP")
                            == QStringLiteral("1");
        if (!namedLldb.isEmpty()) {
            if (!QFileInfo::exists(namedLldb)) {
                fprintf(stderr, "qmn-demo: QMN_TOUR_LLDBDAP names a program "
                                "that does not exist: %s\n",
                        namedLldb.toLocal8Bit().constData());
                return 1;
            }
            config.lldbDapPath = namedLldb;
        }
        if (!namedGdb.isEmpty()) {
            if (!QFileInfo::exists(namedGdb)) {
                fprintf(stderr, "qmn-demo: QMN_TOUR_GDB names a program that "
                                "does not exist: %s\n",
                        namedGdb.toLocal8Bit().constData());
                return 1;
            }
            config.gdbPath = namedGdb;
        }
        if (namedLldb.isEmpty() && !noLive)
            config.lldbDapPath = QStandardPaths::findExecutable(QStringLiteral("lldb-dap"));
        if (namedGdb.isEmpty() && !noLive)
            config.gdbPath = QStandardPaths::findExecutable(QStringLiteral("gdb"));
        if (noLive)
            config.liveDapSkipReason = QStringLiteral(
                "%1: scoped off by the operator (QMN_TOUR_NO_LIVE_DAP=1) "
                "— the binary may well be installed, the leg just did not run");
        else
            config.liveDapSkipReason = QStringLiteral("%1 not found on PATH");
        // The clangd leg rides when clangd is on PATH (the ctest face
        // never resolves it — the --lsp no-guarantee law in force).
        config.clangdEnabled = !noLive
            && !QStandardPaths::findExecutable(QStringLiteral("clangd")).isEmpty();

        // The corpus home: the tail's fixture dir, a QMN_TOUR_OUT
        // directory (the operator's keep-the-corpus face), or a
        // temporary dir that dies with the process.
        QTemporaryDir tempDir;
        QString corpusDir = tourCorpusDir;
        if (corpusDir.isEmpty())
            corpusDir = qEnvironmentVariable("QMN_TOUR_OUT");
        bool corpusTemporary = false;
        if (corpusDir.isEmpty()) {
            if (!tempDir.isValid()) {
                fprintf(stderr, "qmn-demo: --tour could not create a "
                                "temporary corpus directory\n");
                return 1;
            }
            corpusDir = tempDir.path();
            corpusTemporary = true;
        }
        config.corpusDir = QFileInfo(corpusDir).absoluteFilePath();

        // The pin table rides qDebug() (the tst face captures it at the
        // message handler — same channel, same slot). But off a TTY Qt
        // routes debug logs to the JOURNAL, silently robbing a piped
        // ritual of its own table (measured: a piped `--tour` printed
        // zero pin lines on this host). So the CLI face installs its
        // own stderr printer for the duration — every message lands on
        // fd 2 flushed, order preserved — and restores afterwards. The
        // operator never needs QT_FORCE_STDERR_LOGGING.
        const QtMessageHandler previousHandler = qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext&, const QString& msg) {
                const QByteArray line = msg.toUtf8();
                std::fwrite(line.constData(), 1, size_t(line.size()), stderr);
                std::fputc('\n', stderr);
                std::fflush(stderr);
            });
        const bool green = qmn::app::runShowcaseTour(config, window);
        qInstallMessageHandler(previousHandler);
        if (corpusTemporary)
            fprintf(stderr, "qmn-demo: the tour's corpus lived in a temporary "
                            "directory (QMN_TOUR_OUT=<dir> keeps it, --tour "
                            "<dir> names one)\n");
        else
            fprintf(stderr, "qmn-demo: the tour's corpus is at %s\n",
                    config.corpusDir.toLocal8Bit().constData());
        return green ? 0 : 1;
    }

    const QString exitAfterMs = parser.value(QStringLiteral("exit-after-ms"));
    if (!exitAfterMs.isEmpty())
        QTimer::singleShot(exitAfterMs.toInt(), &app, &QApplication::quit);
    return app.exec();
}
