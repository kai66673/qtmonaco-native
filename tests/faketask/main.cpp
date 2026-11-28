// The TASK faketask (tests support binary, NOT a ctest entry): the
// fakeserver/dapfake sibling for GATE 4 car I4 — a QProcess-spawned,
// deterministic, compiler-SHAPED command whose stdout+stderr the IDE's task
// runner merges into its output pane and whose TEXT the problem matcher
// eats. The far side's EXIT CODE is the oracle (the dapfake law reused
// whole); its text is the fixture. NO PATH COMPILER IS EVER SPAWNED BY
// ctest — the matcher's subject is this text, and the real-compiler story
// stays a live ritual outside CI (PLAN's I4 opening record, the spawn law).
//
// Modes (argv[1]; every leg is pinned by a tst_ide slot against the
// window's own doors):
//   (none)   one stdout line, exit 0 — the plain success face.
//   cwd      prints CWD=<absolute working dir> and ARGS=<argv[2..] joined
//            by '|'> : the cwd + args-expansion face (${WORKSPACE_ROOT}
//            lands here as a literal expanded path). exit 0.
//   gcc      WRITES ./fake_src.cpp into its working dir (the file the
//            problems name must EXIST for the open-and-reveal face) and
//            prints gcc/clang-shaped diagnostics + the noise a real build
//            prints that must NOT become a row (an `In file included from`
//            header line, a caret line, `collect2: error: ld returned 1
//            exit status`, and ONE msvc-shaped line the gcc matcher must
//            refuse). Paths are RELATIVE: they resolve against the task's
//            cwd. One problem names gone.cpp — a file that does NOT exist
//            (the honest phantom row). exit 1 (a compiler reports failure).
//   msvc     the same written file, MSVC-shaped diagnostics, plus one
//            gcc-shaped line the msvc matcher must refuse. exit 1.
//   stderr   OUT-1 / ERR-1 / OUT-2 across the two streams: the pane keeps
//            ARRIVAL ORDER, and the stdout/stderr counts are summary faces.
//   big <n>  n stdout lines BIG-1..BIG-n — the pane's tail cap + the
//            hidden-line face + the paced-flush legs.
//   many <n>  n gcc-shaped rows against the file this mode WRITES (line numbers
//            cycling inside it) — the panel's ROW cap and the decoration FEED
//            cap, both with their "skipped" faces. exit 1.
//   slow <ms> SLOW-START, flush, sleep, SLOW-END — the pane shows output
//            while the run is STILL RUNNING.
//   hang     HANG-START then waits for a signal; SIGTERM (the runner's
//            terminate step) prints TERM-CAUGHT and exits 0 — the far-side
//            witness that the graceful stop step reached the child. argv[2]
//            == "stubborn" IGNORES SIGTERM, so the app must escalate to
//            kill(): that leg's witness is the app's own stopped face.
//   exit <c> prints EXITING, exits with code c (default 3) — the exit-code
//            summary face.
//
// The written file's CONTENT is byte-fixed, because the line/column numbers
// in the diagnostics below refer to it: an error at 3:10 must land exactly on
// `undeclared_thing`, and if fake and IDE ever drift, the pinned rows drift
// LOUDLY instead of silently agreeing.

#include <QDir>
#include <QFile>
#include <QString>
#include <QTextStream>

#include <csignal>
#include <cstdlib>

#include <unistd.h> // pause() — the hang mode waits for a signal

namespace {

constexpr char kFakeSourcePath[] = "fake_src.cpp";

// 1-based line map (the diagnostics below are written against THIS text):
//   1 // fake_src.cpp — written by tests/faketask (the I4 far side)
//   2 int widget_value(int x) {
//   3   return undeclared_thing + x;
//   4 }
//   5
//   6 static int unused_static_helper() { return 0; }
//   7
//   8 int main() { return widget_value(1); }
const char kFakeSource[] =
    "// fake_src.cpp \xe2\x80\x94 written by tests/faketask (the I4 far side)\n"
    "int widget_value(int x) {\n"
    "  return undeclared_thing + x;\n"
    "}\n"
    "\n"
    "static int unused_static_helper() { return 0; }\n"
    "\n"
    "int main() { return widget_value(1); }\n";

void writeFakeSource()
{
    QFile file(QString::fromLatin1(kFakeSourcePath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        // The far side says so on stderr (a real compiler cannot write its
        // own way either) and exits LOUD: every row the test pins hangs on
        // this file existing.
        QTextStream err(stderr);
        err << "FAKE-TASK-CANNOT-WRITE " << file.errorString() << Qt::endl;
        std::_Exit(2);
    }
    file.write(kFakeSource);
    file.close();
}

void sleepMs(int ms)
{
    constexpr int kUsPerMs = 1000;
    ::usleep(static_cast<unsigned>(ms) * kUsPerMs);
}

// SIGTERM is the runner's FIRST stop step (terminate, then kill after the
// grace), so the child gets a voice in that leg: the handler's line lands in
// the pane in arrival order and the exit is a clean 0.
void onTerm(int)
{
    QTextStream out(stdout);
    out << "TERM-CAUGHT" << Qt::endl; // endl flushes: the pane must see it
    std::_Exit(0);
}

int intArg(char* arg, int fallback)
{
    const QString text = QString::fromLocal8Bit(arg);
    bool ok = false;
    const int value = text.toInt(&ok);
    return ok ? value : fallback;
}

} // namespace

int main(int argc, char* argv[])
{
    const QString mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    QTextStream out(stdout);
    QTextStream err(stderr);

    if (mode.isEmpty()) {
        out << "FAKE-TASK-OK" << Qt::endl;
        return 0;
    }

    if (mode == QLatin1String("cwd")) {
        out << "CWD=" << QDir::currentPath() << Qt::endl;
        QString joined;
        for (int i = 2; i < argc; ++i) {
            if (i > 2)
                joined += QLatin1Char('|');
            joined += QString::fromLocal8Bit(argv[i]);
        }
        out << "ARGS=" << joined << Qt::endl;
        return 0;
    }

    if (mode == QLatin1String("gcc")) {
        writeFakeSource();
        out << "g++ -c fake_src.cpp -o fake_src.o" << Qt::endl;
        out << "In file included from fake_src.cpp:2:" << Qt::endl; // noise
        out << "fake_src.cpp:3:10: error: 'undeclared_thing' was not "
               "declared in this scope [-Wundeclared]"
            << Qt::endl;
        out << "    3 |   return undeclared_thing + x;" << Qt::endl; // noise
        out << "      |          ^" << Qt::endl;                     // noise
        out << "fake_src.cpp:6: warning: 'unused_static_helper' defined but "
               "not used [-Wunused-function]"
            << Qt::endl; // the NO-COLUMN shape
        out << "fake_src.cpp:8:5: note: while expanding 'widget_value'"
            << Qt::endl;
        out << "collect2: error: ld returned 1 exit status" << Qt::endl; // noise
        // The OTHER matcher's shape: a gcc-matcher run must refuse it, not
        // invent a row from it (the never-invent law).
        out << "fake_src.cpp(3,10): error C2065 : msvc shape under gcc"
            << Qt::endl;
        // The phantom: a compiler may name a file that is gone. The row is
        // the answer; the reveal is a separate truth.
        out << "gone.cpp:1:1: fatal error: no such file: compilation "
               "terminated"
            << Qt::endl;
        err << "fake task: build failed" << Qt::endl;
        return 1;
    }

    if (mode == QLatin1String("msvc")) {
        writeFakeSource();
        out << "cl /c fake_src.cpp" << Qt::endl;
        out << "fake_src.cpp(3,10): error C2065 : 'undeclared_thing' : "
               "undeclared identifier"
            << Qt::endl;
        out << "fake_src.cpp(6): warning C4505: unreferenced local function "
               "has been removed"
            << Qt::endl; // no-column shape
        out << "fake_src.cpp(8,5): note C1234 : a note shaped row"
            << Qt::endl;
        out << "fake_src.cpp:3:10: error: gcc shape under msvc" << Qt::endl;
        err << "cl : Command line warning D9002 : ignoring unknown option"
            << Qt::endl; // stderr noise that is NOT a row (no path shape)
        return 1;
    }

    if (mode == QLatin1String("stderr")) {
        out << "OUT-1" << Qt::endl;
        err << "ERR-1" << Qt::endl;
        out << "OUT-2" << Qt::endl;
        return 0;
    }

    if (mode == QLatin1String("big")) {
        const int lines = argc > 2 ? intArg(argv[2], 0) : 0;
        for (int i = 1; i <= lines; ++i)
            out << "BIG-" << i << Qt::endl;
        return 0;
    }

    if (mode == QLatin1String("many")) {
        // THE CAP legs (the skip-honesty faces, the law I2 froze for quick-open
        // now riding the problems panel and the decoration feed): N gcc-shaped
        // rows, ALL against the file this mode WRITES, cycling the line number
        // inside that file so a row never points past its end. `many 1200`
        // crosses the panel's ROW cap; `many 2500` crosses the FEED cap too. The
        // counts and both "skipped" faces are pinned in tst_ide. exit 1 — a
        // compiler that printed N errors never claims success.
        writeFakeSource();
        const int rows = argc > 2 ? intArg(argv[2], 0) : 0;
        for (int i = 1; i <= rows; ++i)
            out << "fake_src.cpp:" << (1 + (i % 8)) << ":1: error: seeded "
                << "problem " << i << " [-Wcapped]" << Qt::endl;
        return 1;
    }

    if (mode == QLatin1String("slow")) {
        const int ms = argc > 2 ? intArg(argv[2], 1000) : 1000;
        out << "SLOW-START" << Qt::endl; // Qt::endl flushes the pipe NOW: the
                                         // pane must show it mid-run
        sleepMs(ms);
        out << "SLOW-END" << Qt::endl;
        return 0;
    }

    if (mode == QLatin1String("hang")) {
        out << "HANG-START" << Qt::endl;
        if (argc > 2
            && QString::fromLocal8Bit(argv[2]) == QLatin1String("stubborn"))
            std::signal(SIGTERM, SIG_IGN);
        else
            std::signal(SIGTERM, onTerm);
        for (;;)
            ::pause();
    }

    if (mode == QLatin1String("exit")) {
        const int code = argc > 2 ? intArg(argv[2], 3) : 3;
        out << "EXITING" << Qt::endl;
        return code;
    }

    out << "FAKE-TASK-UNKNOWN-MODE " << mode << Qt::endl;
    return 4;
}
