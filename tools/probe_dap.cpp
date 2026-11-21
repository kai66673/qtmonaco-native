// Car 4 close-car ritual (PLAN, D4 record): the LIVE-ADAPTER DAP probe.
// Never a CI gate (CI ships no debuggers, adapters drift) — but car 4 may
// not CLOSE without a green run, recorded in the CHANGELOG. The order is
// PLAN-decided: lldb-dap first, gdb -i=dap second (the host ships both).
//
// What the ritual pins (and dapfake never can): agreement with an adapter
// that computes its own answers — a REAL breakpoint verified at a real
// source line, a `stopped` EVENT, a stack frame whose line agrees with the
// hub's painted dot, a VARIABLE VALUE read back through scopes/variables
// (41 is the probe's, not the adapter's memory), then `continue` and an
// honest program-exit (`terminated` -> sessionEnded + the feeds cleared).
//
// State checks only (no vision): every step prints PASS, the run ends in
// PROBE-OK (exit 0) or PROBE-FAIL <step> (exit 1); teardown rides the
// hub's stop barrier like any destructor. Build+run lines live in
// tools/probe_dap.sh (ad-hoc g++ like tools/bench_model.cpp).
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <functional>

#include "dap/DapClient.hpp"
#include "glue/DapHub.hpp"
#include "glue/ProjectSession.hpp"
#include "protocol/RpcClient.hpp"
#include "view/CodeEditor.hpp"

namespace {

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}
QTextStream& err()
{
    static QTextStream s(stderr);
    return s;
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!predicate()) {
        if (clock.elapsed() > timeoutMs)
            return predicate();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return true;
}

// The reply carrier, the same FeatureProbe shape as the clangd probe —
// but riding DapClient's OWN handler door (the hub's request() answer
// route is dapEvent-only by design; client() is the C++-only door).
struct DapReplyProbe {
    bool arrived = false;
    qmn::protocol::RpcClient::Reply reply;

    qmn::protocol::RpcClient::ReplyHandler handler()
    {
        return [this](const qmn::protocol::RpcClient::Reply& r) {
            arrived = true;
            reply = r;
        };
    }
};

QString dumpValue(const QJsonValue& value)
{
    if (value.isObject())
        return QString::fromUtf8(
            QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    if (value.isArray())
        return QString::fromUtf8(
            QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    const QString wrapped = QString::fromUtf8(
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
    return wrapped.mid(1, wrapped.size() - 2); // [x] -> x
}

// Own join for the STRING-separator cases: the inline QStringList::join
// overload for const char*/QStringLiteral separators loads &QString::empty
// for its "empty list" fast path — a copy relocation a NON-PIE binary (the
// probe's -no-pie, same reason as tools/bench_model.cpp) may not get under
// current binutils ("non-copyable protected symbol _ZN7QString6_emptyE").
// The QChar/QLatin1Char overloads stay out-of-line and are proven safe here.
QString joinAll(const QStringList& list, const QString& sep)
{
    QString out;
    for (const QString& part : list) {
        if (!out.isEmpty())
            out += sep;
        out += part;
    }
    return out;
}

QString joinLines(const QVector<int>& lines)
{
    QStringList out;
    for (int line : lines)
        out << QString::number(line);
    return joinAll(out, QStringLiteral(","));
}

int g_step = 0;
bool step(const QString& what)
{
    out() << "PASS " << ++g_step << ": " << what << Qt::endl;
    return true;
}

// Compiles the drill document. cc first, gcc second — absence is an
// honest PROBE-FAIL (the drill's truth is REAL debug info, nothing here
// may be faked).
bool compileDrill(const QString& sourcePath, const QString& binaryPath)
{
    for (const QString& compiler : {QStringLiteral("cc"),
                                    QStringLiteral("gcc"),
                                    QStringLiteral("clang")}) {
        QProcess cc;
        cc.start(compiler, {QStringLiteral("-g"), QStringLiteral("-O0"),
                            QStringLiteral("-o"), binaryPath, sourcePath});
        if (cc.error() == QProcess::FailedToStart)
            continue; // not on PATH
        if (!cc.waitForFinished(30000) || cc.exitCode() != 0) {
            err() << "PROBE-FAIL compile: " << compiler << ": "
                  << QString::fromUtf8(cc.readAllStandardError()).trimmed()
                  << Qt::endl;
            return false;
        }
        out() << "compiled with " << compiler << Qt::endl;
        return true;
    }
    err() << "PROBE-FAIL compile: no cc/gcc/clang on PATH\n";
    return false;
}

bool runProbe(qmn::view::CodeEditor& editor,
              qmn::glue::ProjectSession& project,
              qmn::glue::DapHub& hub,
              const QString& sourcePath,
              const QString& binaryPath,
              const QString& adapterProgram,
              const QStringList& adapterArgs)
{
    // The drill document. The BREAKPOINT lands on model line 4
    // ("int extra = answer + 1"): by then `answer` HAS been assigned, so
    // the value read-back below is a fact the adapter must answer from a
    // real stopped thread, not from a guess. Positions are 0-based model
    // lines; the wire is 1-based (the hub converts, total by contract).
    const QString text = QStringLiteral(
        "// qtmonaco-native live-DAP drill document\n"
        "int main(void)\n"
        "{\n"
        "    int answer = 41;\n"
        "    int extra = answer + 1;\n"
        "    return extra - 42;\n"
        "}\n");
    editor.setPlainText(text);
    editor.setLanguage(QStringLiteral("c"));
    if (!project.openDocument(&editor, sourcePath))
        return err() << "PROBE-FAIL open: the session refused the document\n",
               false;

    // store-BEFORE-ready (the hub's documented legal order):
    if (!hub.setBreakpoints(&editor, {4}))
        return err() << "PROBE-FAIL breakpoints: the door refused locally\n",
               false;

    int stoppedThreadId = -1;
    QStringList stoppedReasons;
    QStringList continuedFlags;
    QStringList eventNames;
    QStringList endReasons;
    QStringList unresolvedFrames;
    QStringList refusals;
    QObject::connect(&hub, &qmn::glue::DapHub::debugStopped,
                     [&](int threadId, const QString& reason) {
                         stoppedThreadId = threadId;
                         stoppedReasons << reason;
                     });
    QObject::connect(&hub, &qmn::glue::DapHub::launchRejected,
                     [&](const QString& message) { refusals << message; });
    QObject::connect(&hub, &qmn::glue::DapHub::sourceUnresolved,
                     [&](const QJsonObject& source, const QJsonObject& frame) {
                         unresolvedFrames << QStringLiteral("source=")
                                          + dumpValue(source)
                                          + QStringLiteral(" frame={line:")
                                          + QString::number(
                                                frame.value(QStringLiteral("line")).toInt())
                                          + QStringLiteral(",name:")
                                          + frame.value(QStringLiteral("name")).toString()
                                          + QStringLiteral("}");
                     });
    QObject::connect(&hub, &qmn::glue::DapHub::debugContinued,
                     [&] { continuedFlags << QStringLiteral("continued"); });
    QObject::connect(&hub, &qmn::glue::DapHub::sessionEnded,
                     [&](const QString& reason) { endReasons << reason; });
    QObject::connect(&hub, &qmn::glue::DapHub::dapEvent,
                     [&](const QString& name, const QJsonValue&) {
                         eventNames << name;
                     });

    if (!hub.startDebugging(adapterProgram, adapterArgs,
                            QJsonObject{{QStringLiteral("program"),
                                        binaryPath}}))
        return err() << "PROBE-FAIL start: the hub refused ("
                     << adapterProgram << ")\n",
               false;
    if (!waitUntil([&] { return hub.isReady(); }, 60000))
        return err() << "PROBE-FAIL handshake: the adapter never reached "
                     << "Ready (" << adapterProgram << ")\n",
               false;
    step(QStringLiteral("handshake: %1 Ready (caps: %2 fields)")
             .arg(adapterProgram)
             .arg(hub.adapterCapabilities().size()));
    hub.configurationDone(); // the app's setup-over word (the demo's shape)

    // A REAL breakpoint at a real source line: the dot must verify IN
    // place (the adapter saw the debug info and accepted the line).
    if (!waitUntil([&] { return !editor.breakpointLines().isEmpty(); }, 30000)) {
        QString diagText;
        for (const auto& d : editor.diagnostics())
            diagText += QStringLiteral("[line %1] %2; ").arg(d.startLine).arg(d.message);
        return err() << "PROBE-FAIL breakpoints: no dot was ever verified. "
                     << "diags: " << diagText
                     << (refusals.isEmpty()
                             ? QString()
                             : QStringLiteral("launch refused: %1").arg(joinAll(refusals, QStringLiteral("; "))))
                     << Qt::endl,
               false;
    }
    QStringList dots;
    for (int line : editor.breakpointLines())
        dots << QString::number(line);
    step(QStringLiteral("setBreakpoints: verified dot(s) at model line(s) [%1]")
             .arg(dots.join(QLatin1Char(','))));

    // The stop: a `stopped` EVENT with a threadId (the hub's feed keys by
    // it). Programs that run to a breakpoint legitimately differ on WHY
    // (breakpoint vs step) — the probe prints, pins the SHAPE only.
    if (!waitUntil([&] { return !stoppedReasons.isEmpty(); }, 60000))
        return err() << "PROBE-FAIL stopped: no debugStopped ever arrived "
                     << "(events: " << eventNames.join(QLatin1Char(','))
                     << ")\n",
               false;
    step(QStringLiteral("stopped: thread %1, reason \"%2\"")
             .arg(stoppedThreadId)
             .arg(stoppedReasons.constFirst()));

    // The CARET feed (D3's stackTrace chain): the hub's own chain runs on
    // the stopped event and lands LATER (an async round trip) — WAIT for
    // the caret to agree with the painted dot before pinning (a caret
    // checked at stopped-time is the probe's race, not the hub's bug).
    const bool caretLanded = waitUntil(
        [&] {
            return editor.breakpointLines().contains(editor.position().line)
                || !unresolvedFrames.isEmpty();
        },
        15000);
    const int caretLine = editor.position().line;
    if (!caretLanded || !editor.breakpointLines().contains(caretLine))
        return err() << "PROBE-FAIL caret feed: caret on model line "
                     << caretLine << ", dots at ["
                     << joinLines(editor.breakpointLines()) << "] "
                     << (unresolvedFrames.isEmpty()
                             ? QStringLiteral("(no sourceUnresolved: the "
                                              "hub's stackTrace chain answered "
                                              "error/empty)")
                             : joinAll(unresolvedFrames, QStringLiteral(" | ")))
                     << Qt::endl,
               false;
    step(QStringLiteral("caret feed: caret on model line %1 agrees with "
                        "the verified dot")
             .arg(caretLine));

    // stackTrace -> scopes -> variables: the frame id the hub ALREADY
    // knows (its feed ran this chain) is re-walked here by hand, because
    // the value read-back is the probe's own demand, not the hub's.
    qmn::dap::DapClient* client = hub.client();
    if (!client)
        return err() << "PROBE-FAIL client door: null client on a Ready "
                     << "session\n",
               false;
    {
        DapReplyProbe probe;
        client->request(QStringLiteral("stackTrace"),
                        QJsonObject{{QStringLiteral("threadId"),
                                    stoppedThreadId},
                                    {QStringLiteral("startFrame"), 0},
                                    {QStringLiteral("levels"), 1}},
                        probe.handler());
        if (!waitUntil([&] { return probe.arrived; }, 30000)
            || probe.reply.isError)
            return err() << "PROBE-FAIL stackTrace: "
                         << (probe.arrived ? probe.reply.message
                                           : QStringLiteral("never answered"))
                         << Qt::endl,
                   false;
        const QJsonArray frames
            = probe.reply.result.toObject().value(QStringLiteral("stackFrames"))
                  .toArray();
        if (frames.isEmpty())
            return err() << "PROBE-FAIL stackTrace: zero frames on a "
                         << "stopped thread\n",
                   false;
        const QJsonObject frame = frames.at(0).toObject();
        if (frame.value(QStringLiteral("line")).toInt() - 1 != caretLine)
            return err() << "PROBE-FAIL stackTrace: frame line "
                         << frame.value(QStringLiteral("line")).toInt()
                         << " disagrees with the caret/dot model line "
                         << caretLine << "\n",
                   false;
        if (!frame.contains(QStringLiteral("source"))
            || frame.value(QStringLiteral("source")).toObject()
                       .value(QStringLiteral("path"))
                       .toString()
                       .compare(sourcePath, Qt::CaseInsensitive) != 0)
            out() << "note: frame source is "
                  << dumpValue(frame.value(QStringLiteral("source"))).left(200)
                  << Qt::endl; // printed honestly; not all adapters echo path
        const int frameId = frame.value(QStringLiteral("id")).toInt();
        step(QStringLiteral("stackTrace: frame %1 at line %2 (name \"%3\")")
                 .arg(frameId)
                 .arg(frame.value(QStringLiteral("line")).toInt())
                 .arg(frame.value(QStringLiteral("name")).toString()));

        DapReplyProbe scopes;
        client->request(QStringLiteral("scopes"),
                        QJsonObject{{QStringLiteral("frameId"), frameId}},
                        scopes.handler());
        if (!waitUntil([&] { return scopes.arrived; }, 30000)
            || scopes.reply.isError)
            return err() << "PROBE-FAIL scopes: "
                         << (scopes.arrived ? scopes.reply.message
                                            : QStringLiteral("never answered"))
                         << Qt::endl,
                   false;
        int variablesReference = -1;
        for (const QJsonValue& s :
             scopes.reply.result.toObject()
                 .value(QStringLiteral("scopes"))
                 .toArray()) {
            const QJsonObject obj = s.toObject();
            if (obj.value(QStringLiteral("variablesReference")).toInt() > 0) {
                variablesReference
                    = obj.value(QStringLiteral("variablesReference")).toInt();
                break; // the first real scope is enough for the value hunt
            }
        }
        if (variablesReference <= 0)
            return err() << "PROBE-FAIL scopes: no live variablesReference: "
                         << dumpValue(scopes.reply.result).left(300)
                         << Qt::endl,
                   false;

        DapReplyProbe vars;
        client->request(QStringLiteral("variables"),
                        QJsonObject{{QStringLiteral("variablesReference"),
                                     variablesReference}},
                        vars.handler());
        if (!waitUntil([&] { return vars.arrived; }, 30000) || vars.reply.isError)
            return err() << "PROBE-FAIL variables: "
                         << (vars.arrived ? vars.reply.message
                                          : QStringLiteral("never answered"))
                         << Qt::endl,
                   false;
        bool answer41 = false;
        QString printed;
        for (const QJsonValue& v :
             vars.reply.result.toObject().value(QStringLiteral("variables"))
                 .toArray()) {
            const QJsonObject obj = v.toObject();
            printed += QStringLiteral("%1=%2 ").arg(
                obj.value(QStringLiteral("name")).toString(),
                obj.value(QStringLiteral("value")).toString());
            if (obj.value(QStringLiteral("name")).toString()
                    == QLatin1String("answer")
                && obj.value(QStringLiteral("value")).toString().trimmed()
                       == QLatin1String("41"))
                answer41 = true;
        }
        if (!answer41)
            return err() << "PROBE-FAIL variables: no answer==41 in ["
                         << printed << "]\n",
                   false;
        step(QStringLiteral("variables: answer=41 read back through ref %1 "
                            "[%2]")
                 .arg(variablesReference)
                 .arg(printed.trimmed()));
    }

    // The release: continue (DAP may answer WITHOUT a continued EVENT when
    // allThreadsContinued is true — the hub's D2 `stopped()` edge does not
    // apply to the continued side, so the pin is the REPLY + the honest
    // program-exit that follows, not the signal).
    {
        DapReplyProbe probe;
        client->request(QStringLiteral("continue"),
                        QJsonObject{{QStringLiteral("threadId"),
                                    stoppedThreadId}},
                        probe.handler());
        if (!waitUntil([&] { return probe.arrived; }, 30000)
            || probe.reply.isError)
            return err() << "PROBE-FAIL continue: "
                         << (probe.arrived ? probe.reply.message
                                           : QStringLiteral("never answered"))
                         << Qt::endl,
                   false;
    }
    // The program EXITS for real (return extra-42 -> status -1/255): the
    // adapter must send `terminated`; the hub relays sessionEnded AND
    // clears its feeds (the recorded terminated rule).
    if (!waitUntil([&] { return !endReasons.isEmpty(); }, 60000))
        return err() << "PROBE-FAIL exit: no sessionEnded after continue "
                     << "(events: " << eventNames.join(QLatin1Char(','))
                     << ")\n",
               false;
    if (!editor.breakpointLines().isEmpty() || !editor.diagnostics().isEmpty())
        return err() << "PROBE-FAIL exit: the terminated feed-clear left ["
                     << joinLines(editor.breakpointLines()) << "] dots / "
                     << editor.diagnostics().size() << " diags\n",
               false;
    step(QStringLiteral("exit: sessionEnded(\"%1\") rode the real program "
                        "death, feeds cleared (events: %2)")
             .arg(endReasons.constFirst())
             .arg(eventNames.join(QLatin1Char(','))));
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    if (argc < 2) {
        err() << "usage: probe_dap <adapter-program> [adapter-args...]\n";
        return 2;
    }
    const QString adapterProgram = QString::fromLocal8Bit(argv[1]);
    QStringList adapterArgs;
    for (int i = 2; i < argc; ++i)
        adapterArgs << QString::fromLocal8Bit(argv[i]);

    QApplication app(argc, argv);

    QTemporaryDir dir; // the drill lives here: the binary must be runnable,
    if (!dir.isValid()) { // so no /proc-pinning trick is used, just a real cc
        err() << "PROBE-FAIL setup: no temporary dir\n";
        return 1;
    }
    const QString sourcePath = dir.filePath(QStringLiteral("drill.c"));
    const QString binaryPath = dir.filePath(QStringLiteral("drill"));
    {
        QFile file(sourcePath);
        if (!file.open(QIODevice::WriteOnly)) {
            err() << "PROBE-FAIL setup: cannot write " << sourcePath << "\n";
            return 1;
        }
        file.write("// qtmonaco-native live-DAP drill document\n"
                   "int main(void)\n"
                   "{\n"
                   "    int answer = 41;\n"
                   "    int extra = answer + 1;\n"
                   "    return extra - 42;\n"
                   "}\n");
        file.close();
    }
    if (!compileDrill(sourcePath, binaryPath))
        return 1;

    qmn::view::CodeEditor editor;   // declared BEFORE the hub: the dtor
    qmn::glue::ProjectSession project; // order on the shelf matters
    qmn::glue::DapHub hub;
    hub.attachProject(&project);

    const bool ok = runProbe(editor, project, hub, sourcePath, binaryPath,
                             adapterProgram, adapterArgs);
    // Teardown rides the scope: stopDebugging inside ~DapHub pumps the
    // disconnect barrier (bounded). If the session already ended via
    // terminated this is the client's disconnect + the adapter's exit.
    if (ok)
        out() << "PROBE-OK (" << g_step << " steps, adapter "
              << adapterProgram
              << (adapterArgs.isEmpty() ? QString()
                                        : QStringLiteral(" %1")
                                              .arg(adapterArgs.join(' ')))
              << ")" << Qt::endl;
    return ok ? 0 : 1;
}
