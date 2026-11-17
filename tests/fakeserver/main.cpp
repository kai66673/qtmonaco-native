// The LSP fakeserver (tests support binary, NOT a ctest entry): a QProcess-
// spawned stdio server speaking our own JsonFraming (tests/CMakeLists wires
// tst_lsp's QMN_TEST_LSP_SERVER to this target). PROTOCOL rides stdout,
// diagnostics/corpus-cannery checks ride stderr — the ProcessTransport keeps
// those channels separate, and fakeserver exercises that split from the far
// side of the pipe.
//
// Modes (argv[1], pinned with the machine-2 tst_lsp slots):
//   basic    handshake + post-ready notification channels + a
//            showMessageRequest (expects the client's null answer); unknown
//            REQUEST methods are echoed (the feature-door round-trip),
//            unknown NOTIFICATIONS must be ignored — the stream must stay
//            alive past them (the battery's final probe is the liveness
//            oracle, any stray client response is an id-mismatch exit 2).
//   stderr   stderr marker at startup + a PROTOCOL logMessage afterwards:
//            the two channels must never cross on the client.
//   crash    answers the handshake, then _exit(7): every session dies, so
//            the client's ONE auto-restart + FAILED + restartServer() arms
//            the whole crash/restart ladder.
//   deaf     starts and stays alive but NEVER answers initialize (the 10 s
//            handshake budget is what has to fail it; shutdown/exit are
//            answered, the deafness is specific to the handshake), exits 0
//            on the exit wire or on stdin EOF.
//   requests handshake, then the server->client battery (always-answer +
//            capability-registry oracles, see runBattery); validates every
//            client answer itself, exits 0 only when all answers matched,
//            exits 2 with a stderr explanation otherwise. "noapply" (argv[2])
//            makes the applyEdit step wait for the TEST's respondApplyEdit
//            door instead of the client answering itself.
//
// The exit code IS the oracle for the answers the client side cannot see
// (the battery's wire shapes); tst_lsp asserts exitCode 0 where the mode's
// contract ends in a clean exit.

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QStringList>
#include <QTimer>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include <unistd.h>

#include "protocol/JsonFraming.hpp"

using qmn::protocol::JsonFraming;

namespace {

QString g_mode;
bool g_noApply = false;
bool g_batteryDone = false;
QVector<QJsonObject> g_recorded; // every inbound message, for the checks

// Exactly ONE framing for stdin (the battery's blocking reads and the
// notifier callback never run concurrently: the battery runs inside a
// callback that is parked on the stack).
JsonFraming g_framing;

[[noreturn]] void fail(const QString& what)
{
    std::fprintf(stderr, "FAKESERVER-FAIL %s\n", qPrintable(what));
    std::fflush(stderr);
    _exit(2);
}

void send(const QJsonObject& message)
{
    const QByteArray framed
        = JsonFraming::encode(QJsonDocument(message).toJson(QJsonDocument::Compact));
    const char* bytes = framed.constData();
    int left = framed.size();
    while (left > 0) {
        const ssize_t n = ::write(1, bytes, size_t(left));
        if (n <= 0)
            fail(QStringLiteral("stdout write failed: %1")
                     .arg(QString::fromUtf8(std::strerror(errno))));
        bytes += n;
        left -= int(n);
    }
}

QJsonObject response(const QJsonValue& id, const QJsonValue& result)
{
    QJsonObject m;
    m.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("result"), result);
    return m;
}

void notify(const QString& method, const QJsonValue& params)
{
    QJsonObject m;
    m.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    m.insert(QStringLiteral("method"), method);
    if (!params.isUndefined())
        m.insert(QStringLiteral("params"), params);
    send(m);
}

void answerError(const QJsonObject& request, int code)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), QStringLiteral("server-side check"));
    QJsonObject m;
    m.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    m.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
    m.insert(QStringLiteral("error"), error);
    send(m);
}

// --------------------------------------------------------- battery oracles
// Every entry validates one CLIENT answer (the LspClient policy handlers run
// inside the tst process; their wire answers come back here as responses).
struct Step {
    QJsonObject request;
    std::function<void(const QJsonObject&)> checks; // fail() on a lie
};

// The battery shares the runLoop's framing (there is exactly one stdin
// stream; during the blocking battery read the notifier callback is parked,
// so messages that overtake the battery — the client's shutdown/exit — stay
// buffered in the SAME framing and are processed after it returns).
QJsonValue readNextMessage()
{
    for (;;) {
        QByteArray payload;
        while (g_framing.poll(&payload)) {
            const QJsonObject message
                = QJsonDocument::fromJson(payload).object();
            if (message.value(QStringLiteral("method")).isUndefined())
                return QJsonValue(message); // a response for the battery
            g_recorded.append(message); // stray inbound: record, keep reading
        }
        char buffer[4096];
        const ssize_t n = ::read(0, buffer, sizeof(buffer));
        if (n == 0)
            fail(QStringLiteral("EOF while the battery awaited an answer"));
        if (n < 0 && errno != EINTR)
            fail(QStringLiteral("stdin read failed: %1")
                     .arg(QString::fromUtf8(std::strerror(errno))));
        if (n > 0)
            g_framing.feed(QByteArray(buffer, int(n)));
    }
}

void expectResponseFor(const QJsonObject& stepRequest, const QJsonValue& got,
                       std::function<void(const QJsonObject&)> checks)
{
    if (!got.isObject())
        fail(QStringLiteral("battery step \"%1\" got no response")
                 .arg(stepRequest.value(QStringLiteral("method")).toString()));
    const QJsonObject message = got.toObject();
    if (message.value(QStringLiteral("id"))
        != stepRequest.value(QStringLiteral("id")))
        fail(QStringLiteral("battery answer id mismatch for \"%1\"")
                 .arg(stepRequest.value(QStringLiteral("method")).toString()));
    checks(message);
}

QJsonObject regRequest(qint64 id, const QString& methodName)
{
    QJsonObject registration;
    registration.insert(QStringLiteral("id"), QStringLiteral("r%1").arg(id));
    registration.insert(QStringLiteral("method"), methodName);
    QJsonArray registrations;
    registrations.append(registration);
    QJsonObject m;
    m.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("method"), QStringLiteral("client/registerCapability"));
    m.insert(QStringLiteral("params"),
             QJsonObject{{QStringLiteral("registrations"), registrations}});
    return m;
}

void runBattery()
{
    std::vector<Step> steps;

    steps.push_back(
        {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), 9001},
                     {QStringLiteral("method"),
                      QStringLiteral("window/showMessageRequest")},
                     {QStringLiteral("params"),
                      QJsonObject{{QStringLiteral("type"), 2},
                                  {QStringLiteral("message"),
                                   QStringLiteral("battery message")},
                                  {QStringLiteral("actions"),
                                   QJsonArray{QJsonObject{
                                       {QStringLiteral("title"),
                                        QStringLiteral("Fix it")}}}}}}},
         [](const QJsonObject& m) {
             if (!m.contains(QStringLiteral("result"))
                 || !m.value(QStringLiteral("result")).isNull())
                 fail(QStringLiteral("showMessageRequest answer is not null"));
         }});

    steps.push_back(
        {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), 9002},
                     {QStringLiteral("method"),
                      QStringLiteral("workspace/configuration")},
                     {QStringLiteral("params"),
                      QJsonObject{{QStringLiteral("items"),
                                   QJsonArray{QJsonObject{
                                       {QStringLiteral("section"),
                                        QStringLiteral("a")}},
                                   QJsonObject{{QStringLiteral("section"),
                                                QStringLiteral("b")}}}}}}},
         [](const QJsonObject& m) {
             const QJsonArray answers
                 = m.value(QStringLiteral("result")).toArray();
             if (answers.size() != 2 || !answers.at(0).isNull()
                 || !answers.at(1).isNull())
                 fail(QStringLiteral("configuration answer is not [null,null]"));
         }});

    steps.push_back(
        {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), 9003},
                     {QStringLiteral("method"),
                      QStringLiteral("workspace/workspaceFolders")}},
         [](const QJsonObject& m) {
             if (!m.contains(QStringLiteral("result"))
                 || !m.value(QStringLiteral("result")).isNull())
                 fail(QStringLiteral("workspaceFolders answer is not null"));
         }});

    steps.push_back(
        {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), 9004},
                     {QStringLiteral("method"),
                      QStringLiteral("workDoneProgress/create")},
                     {QStringLiteral("params"),
                      QJsonObject{{QStringLiteral("token"),
                                   QStringLiteral("work-1")}}}},
         [](const QJsonObject& m) {
             if (!m.contains(QStringLiteral("result"))
                 || !m.value(QStringLiteral("result")).isNull())
                 fail(QStringLiteral("workDoneProgress/create answer is not null"));
         }});

    // all-or-nothing: the invalid entry must veto the valid one in the SAME
    // request (the client answering -32803 while also registering hover
    // would be the half-applied state lie).
    {
        QJsonObject hover;
        hover.insert(QStringLiteral("method"), QStringLiteral("textDocument/hover"));
        QJsonObject bogus;
        bogus.insert(QStringLiteral("method"),
                     QStringLiteral("textDocument/magic"));
        QJsonArray both;
        both.append(hover);
        both.append(bogus);
        steps.push_back(
            {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 9005},
                         {QStringLiteral("method"),
                          QStringLiteral("client/registerCapability")},
                         {QStringLiteral("params"),
                          QJsonObject{{QStringLiteral("registrations"), both}}}},
             [](const QJsonObject& m) {
                 if (m.contains(QStringLiteral("result")))
                     fail(QStringLiteral("invalid registration was accepted"));
                 const int code = m.value(QStringLiteral("error"))
                                      .toObject()
                                      .value(QStringLiteral("code"))
                                      .toInt();
                 if (code != -32803)
                     fail(QStringLiteral("invalid registration answered %1, "
                                          "not -32803")
                              .arg(code));
             }});
    }

    // The registry trio: register hover (null + the client flips the gate),
    // unregister it (null even under an unknown id: idempotent no-op), and
    // the idempotency round (unregistering nothing still answers null).
    steps.push_back(
        {regRequest(9006, QStringLiteral("textDocument/hover")),
         [](const QJsonObject& m) {
             if (!m.contains(QStringLiteral("result"))
                 || !m.value(QStringLiteral("result")).isNull())
                 fail(QStringLiteral("register hover answer is not null"));
         }});
    {
        QJsonObject unreg;
        unreg.insert(QStringLiteral("id"), QStringLiteral("no-such-id"));
        unreg.insert(QStringLiteral("method"), QStringLiteral("textDocument/hover"));
        QJsonArray registrations;
        registrations.append(unreg);
        steps.push_back(
            {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 9007},
                         {QStringLiteral("method"),
                          QStringLiteral("client/unregisterCapability")},
                         {QStringLiteral("params"),
                          QJsonObject{{QStringLiteral("registrations"),
                                       registrations}}}},
             [](const QJsonObject& m) {
                 if (!m.contains(QStringLiteral("result"))
                     || !m.value(QStringLiteral("result")).isNull())
                     fail(QStringLiteral("unregister answer is not null"));
             }});
    }

    // The unknown client METHOD gets the mechanical -32601 (M1's mechanism
    // re-proved through the LspClient session).
    steps.push_back(
        {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), 9008},
                     {QStringLiteral("method"), QStringLiteral("client/nonsense")}},
         [](const QJsonObject& m) {
             const int code = m.value(QStringLiteral("error"))
                                  .toObject()
                                  .value(QStringLiteral("code"))
                                  .toInt();
             if (code != -32601)
                 fail(QStringLiteral("unknown client method answered %1, "
                                      "not -32601")
                          .arg(code));
         }});

    // applyEdit: NOT answered by the client itself — either the tst answers
    // via respondApplyEdit (noapply mode) or a self-answer/stream break is
    // the violation.
    if (g_noApply) {
        steps.push_back(
            {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 9009},
                         {QStringLiteral("method"),
                          QStringLiteral("workspace/applyEdit")},
                         {QStringLiteral("params"),
                          QJsonObject{{QStringLiteral("edit"),
                                       QJsonObject{{QStringLiteral("label"),
                                                    QStringLiteral("battery edit")}}}}}},
             [](const QJsonObject& m) {
                 if (m.value(QStringLiteral("result"))
                         .toObject()
                         .value(QStringLiteral("applied"))
                         .toBool()
                     != true)
                     fail(QStringLiteral("applyEdit door answer is not "
                                          "{applied:true}"));
             }});
    } else {
        notify(QStringLiteral("workspace/applyEdit"),
               QJsonObject{{QStringLiteral("label"), QStringLiteral("x")}});
        steps.push_back(
            {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 9009},
                         {QStringLiteral("method"), QStringLiteral("client/streamprobe")}},
             [](const QJsonObject& m) {
                 if (m.value(QStringLiteral("error"))
                         .toObject()
                         .value(QStringLiteral("code"))
                         .toInt()
                     != -32601)
                     fail(QStringLiteral("stream broke after applyEdit"));
             }});
    }

    // An unknown NOTIFICATION carries no id to answer against, and RpcClient
    // never answers notifications (machine 1 pins the silence). This step
    // proves the stream stayed ALIVE across it: the final probe must still
    // get the mechanical -32601, and any stray client response that had been
    // emitted mid-battery would land in readNextMessage as an id mismatch
    // (an honest exit-2 fail) before this point could pass.
    notify(QStringLiteral("some/unknownNotification"),
           QJsonValue(QJsonValue::Undefined));
    steps.push_back(
        {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), 9010},
                     {QStringLiteral("method"), QStringLiteral("client/finalprobe")}},
         [](const QJsonObject& m) {
             if (m.value(QStringLiteral("error"))
                     .toObject()
                     .value(QStringLiteral("code"))
                     .toInt()
                 != -32601)
                 fail(QStringLiteral("final probe did not get -32601"));
         }});

    for (const Step& step : steps) {
        send(step.request);
        expectResponseFor(step.request, readNextMessage(), step.checks);
    }
    g_batteryDone = true;
    std::fprintf(stderr, "BATTERY-PASS\n");
    std::fflush(stderr);
}

// ------------------------------------------------------------- the server
void handleInitialize(const QJsonObject& request)
{
    if (!request.value(QStringLiteral("params"))
             .toObject()
             .contains(QStringLiteral("capabilities")))
        fail(QStringLiteral("initialize params lack \"capabilities\""));
    QJsonObject capabilities;
    capabilities.insert(QStringLiteral("hoverProvider"), true);
    send(response(request.value(QStringLiteral("id")),
                  QJsonObject{{QStringLiteral("capabilities"), capabilities},
                              {QStringLiteral("positionEncoding"),
                               QStringLiteral("utf-16")},
                              {QStringLiteral("serverInfo"),
                               QJsonObject{{QStringLiteral("name"),
                                            QStringLiteral("qmn-fakeserver")}}}}));
}

void handleInitialized()
{
    if (g_mode == QLatin1String("crash")) {
        // The crash ladder: EVERY session dies right after the handshake
        // (the client's restart/rearm semantics ride the repetition).
        std::fprintf(stderr, "CRASHER: bye after handshake\n");
        std::fflush(stderr);
        _exit(7);
    }
    if (g_mode == QLatin1String("requests")) {
        runBattery();
        return;
    }
    if (g_mode == QLatin1String("basic")) {
        notify(QStringLiteral("window/showMessage"),
               QJsonObject{{QStringLiteral("type"), 3},
                           {QStringLiteral("message"), QStringLiteral("basic hello")}});
        notify(QStringLiteral("window/logMessage"),
               QJsonObject{{QStringLiteral("type"), 4},
                           {QStringLiteral("message"), QStringLiteral("basic log")}});
        notify(QStringLiteral("$/logTrace"),
               QJsonObject{{QStringLiteral("message"), QStringLiteral("basic trace")}});
        notify(QStringLiteral("$/progress"),
               QJsonObject{{QStringLiteral("token"), QStringLiteral("p-1")},
                           {QStringLiteral("value"),
                            QJsonObject{{QStringLiteral("kind"),
                                         QStringLiteral("incremental")},
                                        {QStringLiteral("value"), 7}}}});
        QJsonArray actions;
        actions.append(QJsonObject{{QStringLiteral("title"),
                                    QStringLiteral("Do the thing")}});
        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), QStringLiteral("s-req-1")},
                         {QStringLiteral("method"),
                          QStringLiteral("window/showMessageRequest")},
                         {QStringLiteral("params"),
                          QJsonObject{{QStringLiteral("type"), 1},
                                      {QStringLiteral("message"),
                                       QStringLiteral("basic request")},
                                      {QStringLiteral("actions"), actions}}}});
        // workDoneProgress/create is a REQUEST (spec): the client answers
        // null and re-emits the token as createWorkDoneProgress. Sending it
        // as a notification would leave the client's request handler unrouted.
        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), QStringLiteral("s-req-2")},
                         {QStringLiteral("method"),
                          QStringLiteral("workDoneProgress/create")},
                         {QStringLiteral("params"),
                          QJsonObject{{QStringLiteral("token"),
                                       QStringLiteral("w-1")}}}});
        notify(QStringLiteral("window/showMessage"),
               QJsonObject{{QStringLiteral("type"), 3},
                           {QStringLiteral("message"), QStringLiteral("second hello")}});
        notify(QStringLiteral("some/unknownNotification"),
               QJsonValue(QJsonValue::Undefined));
        return;
    }
    if (g_mode == QLatin1String("stderr")) {
        // The PROTOCOL channel marker; the STDERR marker was written before
        // the handshake. Neither may leak into the other client signal.
        notify(QStringLiteral("window/logMessage"),
               QJsonObject{{QStringLiteral("type"), 4},
                           {QStringLiteral("message"), QStringLiteral("PROTOCOL-MARKER")}});
    }
}

void runLoop()
{
    QSocketNotifier stdinReady(0, QSocketNotifier::Read);
    QObject::connect(&stdinReady, &QSocketNotifier::activated, [&] {
        char buffer[4096];
        const ssize_t n = ::read(0, buffer, sizeof(buffer));
        if (n > 0) {
            g_framing.feed(QByteArray(buffer, int(n)));
            QByteArray payload;
            while (g_framing.poll(&payload)) {
                const QJsonObject message
                    = QJsonDocument::fromJson(payload).object();
                if (message.isEmpty())
                    fail(QStringLiteral("frame is not a JSON object"));
                g_recorded.append(message);
                const QJsonValue methodValue
                    = message.value(QStringLiteral("method"));
                if (methodValue.isUndefined()) {
                    // A RESPONSE from the client (the battery consumes its
                    // own through readNextMessage; anything landing here is
                    // leftover traffic: recorded, ignored).
                    continue;
                }
                const QString method = methodValue.toString();
                const QJsonValue id = message.value(QStringLiteral("id"));
                const QJsonValue params = message.value(QStringLiteral("params"));

                if (method == QLatin1String("initialize")) {
                    if (g_mode != QLatin1String("deaf"))
                        handleInitialize(message);
                } else if (method == QLatin1String("initialized")) {
                    handleInitialized();
                } else if (method == QLatin1String("shutdown")) {
                    send(response(id, QJsonValue(QJsonValue::Null)));
                } else if (method == QLatin1String("exit")) {
                    if (g_mode != QLatin1String("requests"))
                        _exit(0);
                } else if (method == QLatin1String("$/cancelRequest")) {
                    // spec-silent
                } else if (!id.isUndefined()) {
                    if (g_mode == QLatin1String("basic")
                        || g_mode == QLatin1String("stderr"))
                        send(response(id, params)); // the echo round-trip
                    else
                        answerError(message, -32601);
                }
                // unknown NOTIFICATIONS (no id): ignored, per spec. A real
                // JSON-RPC client CANNOT answer a notification (it carries
                // no id to answer against); RpcClient's machine-1 test pins
                // the silence. The stream simply stays alive past it.
            }
        } else if (n == 0) {
            stdinReady.setEnabled(false); // Qt caveat: re-enabled notifiers
                                          // busy-loop at EOF
            // The battery may still be awaiting an answer: EOF now IS the
            // hang the watchdog would report, so report it precisely.
            if (!g_batteryDone && g_mode == QLatin1String("requests"))
                fail(QStringLiteral("EOF mid-battery"));
            _exit(0); // stdin closed by the client: honest server shutdown
        }
    });
    QCoreApplication::exec();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2)
        fail(QStringLiteral("usage: fakeserver <mode> [flags]"));
    g_mode = args.at(1);
    g_noApply = args.size() > 2 && args.at(2) == QLatin1String("noapply");
    if (g_mode != QLatin1String("basic") && g_mode != QLatin1String("stderr")
        && g_mode != QLatin1String("crash") && g_mode != QLatin1String("deaf")
        && g_mode != QLatin1String("requests"))
        fail(QStringLiteral("unknown mode \"%1\"").arg(g_mode));

    if (g_mode == QLatin1String("stderr")) {
        // The STDERR channel fires BEFORE the handshake completes: the
        // client must show it on serverStderr, never on serverLogMessage.
        std::fprintf(stderr, "STDERR-MARKER line one\n");
        std::fflush(stderr);
    }

    // A hung test must die LOUD (exit 3 + stderr) instead of hanging ctest.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        std::fprintf(stderr, "FAKESERVER-FAIL watchdog timeout (mode %s)\n",
                     qPrintable(g_mode));
        std::fflush(stderr);
        _exit(3);
    });
    watchdog.start(g_mode == QLatin1String("deaf") ? 25'000 : 20'000);

    runLoop();
    return 0; // never reached: every path ends in exit()/fail()
}
