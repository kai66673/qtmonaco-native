// The DAP fake adapter (tests support binary, NOT a ctest entry; PLAN car
// 4 machine D2): the dapfake analog of the fakeserver — a QProcess-spawned
// stdio adapter speaking DAP frames over our own JsonFraming (identical
// Content-Length framing on both wires; tests/CMakeLists wires tst_dap's
// QMN_TEST_DAP_SERVER to this target). PROTOCOL rides stdout, markers ride
// stderr — the same channel-split exercise from the far side of the pipe.
//
// Modes (argv[1], pinned with the machine-D2 tst_dap slots):
//   basic    the handshake (initialize VALIDATES arguments.adapterID is a
//            non-empty string — a structural lie exits 2), then the
//            initialized EVENT is mandatory as the very next client
//            message (spec order, a missing/early one exits 2); then
//            commands: `next` -> body{allThreadsContinued:false}, `ping`
//            -> body{pong:true}; an EVENT "output" carrying
//            DAPOUT-PROTOCOL-MARKER rides right after the handshake while
//            a DAPOUT-STDERR-MARKER line went to stderr at startup (the
//            two-channel relay pin). `disconnect` -> answer + exit 0.
//            Unknown commands exit 2 (harness drift).
//   initfail initialize answers success:false error{id:1,format:"dapfake
//            cannot start"} — the client must go FAILED with NO
//            initialized event (an early one exits 2) and NO restart
//            (spawn counting is the tst side); the disconnect that
//            enterFailed triggers is answered, exit 0.
//   dead     answers the handshake, WAITS for the initialized event, then
//            _exit(7): the mid-session death; the no-auto-restart decision
//            is pinned by the tst-side spawnCount staying 1.
//   deaf     NEVER answers initialize (the RpcClient 10 s handshake budget
//            is what has to fail it) but answers the disconnect honestly;
//            exit 0. A pre-answer initialized event from the client is a
//            lie, exit 2.
//   events   handshake, then two events: output{category,console,
//            "dapfake-out"} and terminated (NO body member — the
//            Undefined-body relay pin). Waits for the disconnect, exit 0.
//   reverse  handshake, then the reverse-request battery: a
//            runInTerminal REQUEST — the client's answer must be
//            success:false with error.format naming "not handled" and
//            error.id 0 (the honest default this test never overrides);
//            then a startDebugging REQUEST the TEST answers success:true
//            body{ok:true} through the respond door (glue's first turn);
//            then a progressStart REQUEST — the DapClient's STATIC shelf
//            must answer success:true body{} (a shelf answer never reaches
//            the reverse relay; the tst side pins the relay saw exactly TWO
//            commands, and HERE the {} answer shape is validated).
//            Every answer is validated HERE (request_seq match + shape);
//            any lie exits 2 with a stderr explanation, all matched ->
//            waits for the disconnect, exit 0.
//   cancelreq handshake, then a `cancel` REQUEST for requestId 424242
//            (nothing to cancel: the answer must be success:false naming
//            "cannot be cancelled") — then waits for the client's "slow"
//            command, sends `cancel` for ITS seq (the answer must be
//            success:true), then answers the slow request LATE (a response
//            the client must swallow silently: exactly-once fulfilment is
//            pinned tst-side). A client-originated "cancel" command exits
//            2 (the test never sends one). Waits for the disconnect,
//            exit 0.
//
// Every inbound REQUEST must carry a strictly increasing numeric `seq`
// (all modes): the per-direction seq monotonicity is pinned HERE, on the
// far side of the pipe, where the client cannot audit itself.
//
// The exit code is the oracle for everything the client side cannot see;
// tst_dap asserts exitCode 0 where a mode's contract ends in a clean exit.
// A HUNG dapfake dies LOUD (watchdog exit 3 + stderr), never hangs ctest —
// same philosophy as the fakeserver watchdog (and its 2026-11-21
// starved-host lesson: budgets stay far above every legitimate door).

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

#include <unistd.h>

#include "protocol/JsonFraming.hpp"

using qmn::protocol::JsonFraming;

namespace {

QString g_mode;
JsonFraming g_framing;

qint64 g_outSeq = 0;      // OUR seq (every message we write consumes one)
qint64 g_lastClientSeq = 0; // audits the client's outbound monotonicity
bool g_handshakeDone = false;
qint64 g_outstandingReq = -1; // the seq of our one outstanding request
int g_reverseStep = 0;        // reverse mode: 0 runInTerminal, 1 startDebugging
qint64 g_slowSeq = -1;        // cancelreq: the client's slow command's seq
int g_cancelStep = 0;         // cancelreq: 0 bogus-id, 1 real-id

[[noreturn]] void fail(const QString& what)
{
    std::fprintf(stderr, "DAPFAKE-FAIL %s\n", qPrintable(what));
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

void respondOk(const QJsonObject& request, const QJsonValue& body)
{
    QJsonObject m;
    m.insert(QStringLiteral("type"), QStringLiteral("response"));
    m.insert(QStringLiteral("seq"), ++g_outSeq);
    m.insert(QStringLiteral("request_seq"), request.value(QStringLiteral("seq")));
    m.insert(QStringLiteral("command"), request.value(QStringLiteral("command")));
    m.insert(QStringLiteral("success"), true);
    if (!body.isUndefined())
        m.insert(QStringLiteral("body"), body);
    send(m);
}

void respondFail(const QJsonObject& request, int id, const QString& format)
{
    QJsonObject m;
    m.insert(QStringLiteral("type"), QStringLiteral("response"));
    m.insert(QStringLiteral("seq"), ++g_outSeq);
    m.insert(QStringLiteral("request_seq"), request.value(QStringLiteral("seq")));
    m.insert(QStringLiteral("command"), request.value(QStringLiteral("command")));
    m.insert(QStringLiteral("success"), false);
    m.insert(QStringLiteral("error"),
             QJsonObject{{QStringLiteral("id"), id},
                         {QStringLiteral("format"), format}});
    send(m);
}

void sendEvent(const QString& name, const QJsonValue& body)
{
    QJsonObject m;
    m.insert(QStringLiteral("type"), QStringLiteral("event"));
    m.insert(QStringLiteral("seq"), ++g_outSeq);
    m.insert(QStringLiteral("event"), name);
    if (!body.isUndefined())
        m.insert(QStringLiteral("body"), body);
    send(m);
}

void sendRequest(const QString& command, const QJsonObject& arguments)
{
    QJsonObject m;
    m.insert(QStringLiteral("type"), QStringLiteral("request"));
    m.insert(QStringLiteral("seq"), ++g_outSeq);
    m.insert(QStringLiteral("command"), command);
    m.insert(QStringLiteral("arguments"), arguments);
    g_outstandingReq = m.value(QStringLiteral("seq")).toVariant().toLongLong();
    send(m);
}

// The per-mode "handshake completed" hooks (fired on the initialized
// event, i.e. only modes whose initialize was ANSWERED get here).
void afterHandshake()
{
    g_handshakeDone = true;
    if (g_mode == QLatin1String("basic")) {
        sendEvent(QStringLiteral("output"),
                  QJsonObject{{QStringLiteral("category"),
                               QStringLiteral("console")},
                              {QStringLiteral("output"),
                               QStringLiteral("DAPOUT-PROTOCOL-MARKER")}});
    } else if (g_mode == QLatin1String("dead")) {
        _exit(7);
    } else if (g_mode == QLatin1String("events")) {
        sendEvent(QStringLiteral("output"),
                  QJsonObject{{QStringLiteral("category"),
                               QStringLiteral("console")},
                              {QStringLiteral("output"), QStringLiteral("dapfake-out")}});
        sendEvent(QStringLiteral("terminated"),
                  QJsonValue(QJsonValue::Undefined)); // NO body member
    } else if (g_mode == QLatin1String("reverse")) {
        sendRequest(QStringLiteral("runInTerminal"),
                    QJsonObject{{QStringLiteral("kind"),
                                 QStringLiteral("local")},
                                {QStringLiteral("args"), QJsonArray{}}});
    } else if (g_mode == QLatin1String("cancelreq")) {
        sendRequest(QStringLiteral("cancel"),
                    QJsonObject{{QStringLiteral("requestId"), 424242}});
    }
}

// The response to OUR outstanding request (reverse/cancelreq batteries).
void checkAnswer(const QJsonObject& message)
{
    const QJsonValue requestSeq = message.value(QStringLiteral("request_seq"));
    if (!requestSeq.isDouble()
        || qint64(requestSeq.toDouble()) != g_outstandingReq)
        fail(QStringLiteral("answer keyed by request_seq %1, our outstanding "
                            "request was %2")
                 .arg(requestSeq.toVariant().toString())
                 .arg(g_outstandingReq));
    const bool success = message.value(QStringLiteral("success")).toBool();
    const QJsonObject error
        = message.value(QStringLiteral("error")).toObject();

    if (g_mode == QLatin1String("reverse")) {
        if (g_reverseStep == 0) {
            // runInTerminal: the honest in-envelope failure (nobody in the
            // TEST wired glue for this one — the DapClient default must
            // have answered).
            if (success)
                fail(QStringLiteral("runInTerminal was answered success:true "
                                    "by an unwired client"));
            const QString format = error.value(QStringLiteral("format"))
                                       .toString();
            if (!format.contains(QLatin1String("not handled"))
                || !format.contains(QLatin1String("runInTerminal")))
                fail(QStringLiteral("runInTerminal honest answer says \"%1\"")
                         .arg(format));
            if (error.value(QStringLiteral("id")).toInt() != 0)
                fail(QStringLiteral("runInTerminal honest answer invented an "
                                    "error.id (%1)").arg(error.value(QStringLiteral("id")).toInt()));
            g_reverseStep = 1;
            sendRequest(QStringLiteral("startDebugging"),
                        QJsonObject{{QStringLiteral("name"),
                                     QStringLiteral("child")}});
        } else if (g_reverseStep == 1) {
            // startDebugging: the TEST's glue answered through the respond
            // door while the signal was live.
            if (!success)
                fail(QStringLiteral("startDebugging answer was not success:true: %1")
                         .arg(error.value(QStringLiteral("format")).toString()));
            if (!message.value(QStringLiteral("body")).toObject()
                     .value(QStringLiteral("ok")).toBool())
                fail(QStringLiteral("startDebugging body.ok was not true"));
            // Third leg: the static answer shelf (DapClient registers
            // progressStart/Update/End -> {} BEFORE any relay exists), so
            // this request must be answered AND must never reach the
            // dapRequest signal (pinned tst-side by the relay's count).
            g_reverseStep = 2;
            sendRequest(QStringLiteral("progressStart"),
                        QJsonObject{{QStringLiteral("title"),
                                     QStringLiteral("dapfake-progress")}});
        } else {
            // progressStart: the shelf's honest empty result.
            if (!success)
                fail(QStringLiteral("progressStart shelf answer was a "
                                    "failure: %1")
                         .arg(error.value(QStringLiteral("format")).toString()));
            const QJsonValue body = message.value(QStringLiteral("body"));
            if (!body.isObject() || !body.toObject().isEmpty())
                fail(QStringLiteral("progressStart shelf answer body was not "
                                    "an empty object (got %1)")
                         .arg(QString::fromUtf8(
                             QJsonDocument(body.toObject()).toJson(
                                 QJsonDocument::Compact))));
            g_outstandingReq = -1; // battery complete; only the disconnect remains
        }
    } else if (g_mode == QLatin1String("cancelreq")) {
        if (g_cancelStep == 0) {
            if (success)
                fail(QStringLiteral("cancel of unknown requestId 424242 was "
                                    "answered success:true (a lie)"));
            if (!error.value(QStringLiteral("format")).toString()
                     .contains(QLatin1String("cannot be cancelled")))
                fail(QStringLiteral("unknown-id cancel answer says \"%1\"")
                         .arg(error.value(QStringLiteral("format")).toString()));
            g_cancelStep = 1; // now the real-id cancel may travel (or wait
                              // for the slow request below, either order)
            if (g_slowSeq >= 0)
                sendRequest(QStringLiteral("cancel"),
                            QJsonObject{{QStringLiteral("requestId"), g_slowSeq}});
        } else {
            if (!success)
                fail(QStringLiteral("cancel of the live slow request failed: %1")
                         .arg(error.value(QStringLiteral("format")).toString()));
            // The late answer to the cancelled request: honest per spec
            // ("best effort"), and the client must swallow it silently —
            // the exactly-once pin lives tst-side.
            if (g_slowSeq >= 0) {
                QJsonObject m;
                m.insert(QStringLiteral("type"), QStringLiteral("response"));
                m.insert(QStringLiteral("seq"), ++g_outSeq);
                m.insert(QStringLiteral("request_seq"), g_slowSeq);
                m.insert(QStringLiteral("command"), QStringLiteral("slow"));
                m.insert(QStringLiteral("success"), true);
                m.insert(QStringLiteral("body"), QJsonObject{});
                send(m);
                g_slowSeq = -1;
            }
            g_outstandingReq = -1;
        }
    } else {
        fail(QStringLiteral("client response arrived with no outstanding "
                            "request (mode %1)").arg(g_mode));
    }
}

// One inbound frame (all modes share the audits, dispatch by mode).
void handle(const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("response")) {
        checkAnswer(message);
        return;
    }
    if (type == QLatin1String("event")) {
        const QString event = message.value(QStringLiteral("event")).toString();
        if (g_handshakeDone)
            fail(QStringLiteral("client sent event \"%1\" mid-session "
                                "(none of these modes legitimately does)")
                     .arg(event));
        if (g_mode != QLatin1String("basic")
            && g_mode != QLatin1String("dead")
            && g_mode != QLatin1String("events")
            && g_mode != QLatin1String("reverse")
            && g_mode != QLatin1String("cancelreq"))
            fail(QStringLiteral("initialized arrived although this mode's "
                                "handshake never completed (%1)").arg(g_mode));
        if (event != QLatin1String("initialized"))
            fail(QStringLiteral("first post-handshake message was event "
                                "\"%1\", not initialized").arg(event));
        afterHandshake();
        return;
    }
    if (type != QLatin1String("request"))
        fail(QStringLiteral("frame is not a DAP message (type \"%1\")").arg(type));

    const QJsonValue seq = message.value(QStringLiteral("seq"));
    if (!seq.isDouble())
        fail(QStringLiteral("client request without a numeric seq"));
    const qint64 seqValue = qint64(seq.toDouble());
    if (seqValue <= g_lastClientSeq)
        fail(QStringLiteral("client seq %1 did not increase past %2")
                 .arg(seqValue).arg(g_lastClientSeq));
    g_lastClientSeq = seqValue;

    const QString command = message.value(QStringLiteral("command")).toString();

    if (command == QLatin1String("initialize")) {
        if (g_mode == QLatin1String("deaf"))
            return; // deafness is specific to the handshake
        const QJsonObject args
            = message.value(QStringLiteral("arguments")).toObject();
        if (args.value(QStringLiteral("adapterID")).toString().isEmpty())
            fail(QStringLiteral("initialize without a non-empty adapterID"));
        if (g_mode == QLatin1String("initfail")) {
            respondFail(message, 1, QStringLiteral("dapfake cannot start"));
            return;
        }
        respondOk(message, QJsonObject{
            {QStringLiteral("supportsConfigurationDoneRequest"), true},
            {QStringLiteral("supportsCancelRequest"), true},
            {QStringLiteral("dapfakeMode"), g_mode}});
        return;
    }
    if (command == QLatin1String("disconnect")) {
        respondOk(message, QJsonValue(QJsonValue::Undefined));
        _exit(0); // every mode ends its contract here (dead already _exited)
    }
    if (command == QLatin1String("cancel")) {
        // A CLIENT-originated cancel is legitimate in exactly one place:
        // deaf mode, where the handshake's 10 s timeout cancels the remote
        // work first (the rule is envelope-agnostic). Silently ignored: we
        // never answer that initialize anyway, and DAP's cancel is
        // best-effort (the client does not track the cancel's own answer).
        if (g_mode == QLatin1String("deaf"))
            return;
        fail(QStringLiteral("a client-originated cancel arrived outside "
                            "deaf mode (the TEST never calls cancel())"));
    }
    if (!g_handshakeDone)
        fail(QStringLiteral("command \"%1\" arrived before the initialized "
                            "event").arg(command));

    if (g_mode == QLatin1String("basic")) {
        if (command == QLatin1String("next")) {
            respondOk(message, QJsonObject{{QStringLiteral("allThreadsContinued"),
                                            false}});
            return;
        }
        if (command == QLatin1String("ping")) {
            respondOk(message,
                      QJsonObject{{QStringLiteral("pong"), true}});
            return;
        }
    } else if (g_mode == QLatin1String("cancelreq")
               && command == QLatin1String("slow")) {
        // The slow request may arrive BEFORE or AFTER the bogus-id cancel's
        // answer was checked (both orders are legal timing): store the seq,
        // and send the real cancel whenever BOTH facts are in.
        g_slowSeq = seqValue;
        if (g_cancelStep == 1)
            sendRequest(QStringLiteral("cancel"),
                        QJsonObject{{QStringLiteral("requestId"), g_slowSeq}});
        return; // the LATE answer travels from checkAnswer
    }
    fail(QStringLiteral("unexpected command \"%1\" in mode %2")
             .arg(command, g_mode));
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
                handle(message);
            }
        } else if (n == 0) {
            _exit(0); // stdin closed by the client: honest shutdown
        } else if (errno != EINTR) {
            fail(QStringLiteral("stdin read failed: %1")
                     .arg(QString::fromUtf8(std::strerror(errno))));
        }
    });
    QCoreApplication::exec();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    g_mode = args.size() > 1 ? args.at(1) : QString();
    if (g_mode != QLatin1String("basic") && g_mode != QLatin1String("initfail")
        && g_mode != QLatin1String("dead") && g_mode != QLatin1String("deaf")
        && g_mode != QLatin1String("events") && g_mode != QLatin1String("reverse")
        && g_mode != QLatin1String("cancelreq"))
        fail(QStringLiteral("unknown mode \"%1\"").arg(g_mode));

    if (g_mode == QLatin1String("basic")) {
        // The STDERR channel marker fires BEFORE the handshake completes:
        // it must reach serverStderr, never a dapEvent.
        std::fprintf(stderr, "DAPOUT-STDERR-MARKER\n");
        std::fflush(stderr);
    }

    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        std::fprintf(stderr, "DAPFAKE-FAIL watchdog timeout (mode %s)\n",
                     qPrintable(g_mode));
        std::fflush(stderr);
        _exit(3);
    });
    watchdog.start(g_mode == QLatin1String("deaf") ? 60'000 : 120'000);

    runLoop();
    return 0; // never reached: every path ends in _exit()
}
