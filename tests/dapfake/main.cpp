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
//   breakpoints (machine D3; argv[2] = the expected source.path)
//            Handshake with supportsConfigurationDoneRequest EXPLICITLY
//            FALSE — the hub's configurationDone door must NOT send the
//            request when told false (a stray one lands in the exit-2
//            "unexpected command" arm). Then `launch` (arguments.request
//            == "launch" && noDebug == false — the hub's spec-fill audit),
//            then the setBreakpoints battery: source.path must EQUAL
//            argv[2] (the uri→localPath conversion pin, the exact string
//            the app opened the document with); the breakpoints member
//            must be an array; ids are assigned ACROSS calls, strictly
//            increasing. Wire rules: line 5 -> unverified, message
//            "dapfake cannot stop on line 5" (+ " (pass N)" from call 2
//            on — the resend oracle, read tst-side through the Diagnostic
//            message); line 9 -> VERIFIED line 11 (the MOVED breakpoint:
//            the hub's dot must land on the ACTUAL line); everything else
//            verified in place; an EMPTY array answers with an empty body
//            array (the clear door). After the FIRST call a `breakpoint`
//            EVENT (reason:"changed", the first call's first id, verified
//            false, "dapfake pulled the dot") rides — the event-keyed
//            store update pin. `disconnect` exits 2 unless AT LEAST TWO
//            setBreakpoints calls arrived (the edit-resend demand), else 0.
//   steps    (machine D3; argv[2] = frame source path or the literal
//            "BOGUS", argv[3]/argv[4] = the frame's 1-based line/column)
//            Handshake with supportsConfigurationDoneRequest:true AND
//            supportsTerminateRequest:true, the `launch` audit, then
//            configurationDone -> ok answer + a `stopped` EVENT
//            (reason:"breakpoint", threadId:7). The hub's stack chain:
//            `stackTrace` (threadId/startFrame/levels audited) answers ONE
//            frame {id:1, line/column from argv, source.path=argv[2] —
//            "BOGUS" swaps in a path NO open document can resolve, the
//            sourceUnresolved pin}; `scopes` (frameId==1 audited) answers
//            TWO scopes — variablesReference 21 (the ONE the hub must use)
//            and 22 (poison: asking it exits 2 HERE, so a hub that reads
//            the wrong scope can never print "= WRONG" tst-side);
//            `variables` (21 legal, anything else exits 2) answers pairs
//            including a DUPLICATE name (first-wins pin) and names absent
//            from the stopped line (no-hint pins). `continue` -> ok + a
//            `continued` EVENT. `terminate` -> ok + remembered;
//            `disconnect` exits 2 unless terminate arrived FIRST (the
//            terminateDebugging demand), else 0.
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
int g_setBpCount = 0;         // breakpoints: completed setBreakpoints calls
int g_bpIdCounter = 0;        // breakpoints: adapter-wide ids, strictly increasing
bool g_terminateSeen = false; // steps: terminate answered before the disconnect
QString g_argPath;            // breakpoints: the EXPECTED source.path; steps: the frame path
int g_frameLine = 3;          // steps: the frame's 1-based line (argv[3])
int g_frameCol = 6;           // steps: the frame's 1-based column (argv[4])

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
            && g_mode != QLatin1String("cancelreq")
            && g_mode != QLatin1String("breakpoints")
            && g_mode != QLatin1String("steps"))
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
        // Mode-shaped capabilities (the machine-D3 pins ride these): the
        // EXPLICIT false in breakpoints mode tells the hub's
        // configurationDone door to STAY SILENT (a stray request lands in
        // the exit-2 arm below); steps mode advertises the terminate pair.
        QJsonObject caps{
            {QStringLiteral("supportsCancelRequest"), true},
            {QStringLiteral("dapfakeMode"), g_mode}};
        if (g_mode == QLatin1String("steps")) {
            caps.insert(QStringLiteral("supportsConfigurationDoneRequest"), true);
            caps.insert(QStringLiteral("supportsTerminateRequest"), true);
        } else if (g_mode == QLatin1String("breakpoints")) {
            caps.insert(QStringLiteral("supportsConfigurationDoneRequest"), false);
        } else {
            caps.insert(QStringLiteral("supportsConfigurationDoneRequest"), true);
        }
        respondOk(message, caps);
        return;
    }
    if (command == QLatin1String("disconnect")) {
        if (g_mode == QLatin1String("steps") && !g_terminateSeen)
            fail(QStringLiteral("disconnect arrived before terminate (the "
                                "TEST promised terminateDebugging)"));
        if (g_mode == QLatin1String("breakpoints") && g_setBpCount < 2)
            fail(QStringLiteral("disconnect after %1 setBreakpoints call(s): "
                                "the edit-resend never arrived")
                     .arg(g_setBpCount));
        respondOk(message, QJsonValue(QJsonValue::Undefined));
        _exit(0); // every mode ends its contract here (dead already _exited)
    }
    if (command == QLatin1String("launch")
        && g_mode != QLatin1String("breakpoints")
        && g_mode != QLatin1String("steps")) {
        // Machine D3's hub ALWAYS fires `launch` once the session is Ready
        // (its spec-fill of request/noDebug is audited strictly in the two
        // D3 modes); the older D2 modes stay conversational and just answer
        // ok, so the hub slots riding basic/dead/events modes never trip
        // the "unexpected command" arm below.
        respondOk(message, QJsonObject{});
        return;
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
    } else if (g_mode == QLatin1String("breakpoints")
               || g_mode == QLatin1String("steps")) {
        // The machine-D3 hub battery. Both modes audit the hub's own
        // spec-fill of the launch arguments first:
        if (command == QLatin1String("launch")) {
            const QJsonObject args
                = message.value(QStringLiteral("arguments")).toObject();
            if (args.value(QStringLiteral("request")).toString()
                != QLatin1String("launch"))
                fail(QStringLiteral("launch arguments.request was not "
                                    "\"launch\" (%1)")
                         .arg(args.value(QStringLiteral("request"))
                                  .toVariant().toString()));
            if (args.value(QStringLiteral("noDebug")).toBool(true))
                fail(QStringLiteral("launch arguments.noDebug was not false"));
            respondOk(message, QJsonObject{});
            return;
        }

        if (g_mode == QLatin1String("breakpoints")) {
            if (command == QLatin1String("setBreakpoints")) {
                const QJsonObject args
                    = message.value(QStringLiteral("arguments")).toObject();
                const QString path = args.value(QStringLiteral("source"))
                                         .toObject()
                                         .value(QStringLiteral("path"))
                                         .toString();
                if (path.isEmpty())
                    fail(QStringLiteral("setBreakpoints arrived without a "
                                        "source.path"));
                if (!g_argPath.isEmpty() && path != g_argPath)
                    fail(QStringLiteral("setBreakpoints source.path is \"%1\", "
                                        "expected the opened path \"%2\"")
                             .arg(path, g_argPath)); // the uri->localPath pin
                if (!args.value(QStringLiteral("breakpoints")).isArray())
                    fail(QStringLiteral("setBreakpoints arrived without a "
                                        "breakpoints array"));
                ++g_setBpCount;
                const QJsonArray requested
                    = args.value(QStringLiteral("breakpoints")).toArray();
                const int firstIdOfCall = g_bpIdCounter + 1;
                QJsonArray returned;
                for (const QJsonValue& value : requested) {
                    const QJsonObject item = value.toObject();
                    if (!item.value(QStringLiteral("line")).isDouble())
                        fail(QStringLiteral("setBreakpoints entry without a "
                                            "numeric line"));
                    const int line = item.value(QStringLiteral("line")).toInt();
                    QJsonObject bp;
                    bp.insert(QStringLiteral("id"), ++g_bpIdCounter);
                    if (line == 5) {
                        bp.insert(QStringLiteral("verified"), false);
                        bp.insert(QStringLiteral("line"), line);
                        bp.insert(QStringLiteral("message"),
                                  g_setBpCount == 1
                                      ? QStringLiteral("dapfake cannot stop "
                                                      "on line 5")
                                      : QStringLiteral("dapfake cannot stop "
                                                      "on line 5 (pass %1)")
                                            .arg(g_setBpCount));
                    } else if (line == 9) {
                        bp.insert(QStringLiteral("verified"), true);
                        bp.insert(QStringLiteral("line"), 11); // THE MOVE
                    } else {
                        bp.insert(QStringLiteral("verified"), true);
                        bp.insert(QStringLiteral("line"), line);
                    }
                    returned.append(bp);
                }
                respondOk(message,
                          QJsonObject{{QStringLiteral("breakpoints"), returned}});
                if (g_setBpCount == 1 && !returned.isEmpty()) {
                    // The event-keyed store update: the first call's FIRST
                    // id flips unverified (id-keyed, spec reason "changed").
                    sendEvent(QStringLiteral("breakpoint"),
                              QJsonObject{{QStringLiteral("reason"),
                                           QStringLiteral("changed")},
                                          {QStringLiteral("breakpoint"),
                                           QJsonObject{
                                               {QStringLiteral("id"),
                                                firstIdOfCall},
                                               {QStringLiteral("verified"),
                                                false},
                                               {QStringLiteral("message"),
                                                QStringLiteral("dapfake pulled "
                                                              "the dot")}}}});
                }
                return;
            }
        } else { // steps
            if (command == QLatin1String("configurationDone")) {
                respondOk(message, QJsonValue(QJsonValue::Undefined));
                sendEvent(QStringLiteral("stopped"),
                          QJsonObject{{QStringLiteral("reason"),
                                       QStringLiteral("breakpoint")},
                                      {QStringLiteral("threadId"), 7},
                                      {QStringLiteral("allThreadsStopped"),
                                       true}});
                return;
            }
            if (command == QLatin1String("stackTrace")) {
                const QJsonObject args
                    = message.value(QStringLiteral("arguments")).toObject();
                if (args.value(QStringLiteral("threadId")).toInt() != 7)
                    fail(QStringLiteral("stackTrace asked threadId %1, the "
                                        "stopped event said 7")
                             .arg(args.value(QStringLiteral("threadId")).toInt()));
                if (args.value(QStringLiteral("startFrame")).toInt() != 0)
                    fail(QStringLiteral("stackTrace startFrame was not 0"));
                if (args.value(QStringLiteral("levels")).toInt() < 1)
                    fail(QStringLiteral("stackTrace levels was not >= 1"));
                QString path = g_argPath;
                if (path == QLatin1String("BOGUS"))
                    path = QStringLiteral("/dapfake/definitely-not-open.cpp");
                const QJsonObject frame{
                    {QStringLiteral("id"), 1},
                    {QStringLiteral("name"), QStringLiteral("dapfake-top")},
                    {QStringLiteral("source"),
                     QJsonObject{{QStringLiteral("name"),
                                  QStringLiteral("main.cpp")},
                                 {QStringLiteral("path"), path}}},
                    {QStringLiteral("line"), g_frameLine},
                    {QStringLiteral("column"), g_frameCol}};
                respondOk(message,
                          QJsonObject{{QStringLiteral("stackFrames"),
                                       QJsonArray{frame}},
                                      {QStringLiteral("totalFrames"), 1}});
                return;
            }
            if (command == QLatin1String("scopes")) {
                const QJsonObject args
                    = message.value(QStringLiteral("arguments")).toObject();
                if (args.value(QStringLiteral("frameId")).toInt() != 1)
                    fail(QStringLiteral("scopes asked frameId %1, the frame "
                                        "carried id 1")
                             .arg(args.value(QStringLiteral("frameId")).toInt()));
                respondOk(message, QJsonObject{{QStringLiteral("scopes"),
                                                QJsonArray{
                                                    QJsonObject{
                                                        {QStringLiteral("name"),
                                                         QStringLiteral("Locals")},
                                                        {QStringLiteral("variablesReference"),
                                                         21},
                                                        {QStringLiteral("expensive"),
                                                         false}},
                                                    QJsonObject{
                                                        {QStringLiteral("name"),
                                                         QStringLiteral("Globals poison")},
                                                        {QStringLiteral("variablesReference"),
                                                         22},
                                                        {QStringLiteral("expensive"),
                                                         false}}}}});
                return;
            }
            if (command == QLatin1String("variables")) {
                const QJsonObject args
                    = message.value(QStringLiteral("arguments")).toObject();
                const int ref = args.value(QStringLiteral("variablesReference"))
                                    .toInt();
                if (ref == 22)
                    fail(QStringLiteral("the hub read the SECOND (poisoned) "
                                        "scope — the PLAN feed is the first "
                                        "top-frame scope only"));
                if (ref != 21)
                    fail(QStringLiteral("variables asked reference %1, only "
                                        "21 was published").arg(ref));
                respondOk(message,
                          QJsonObject{{QStringLiteral("variables"),
                                       QJsonArray{
                                           QJsonObject{{QStringLiteral("name"),
                                                       QStringLiteral("x")},
                                                      {QStringLiteral("value"),
                                                       QStringLiteral("42")},
                                                      {QStringLiteral("variablesReference"),
                                                       0}},
                                           QJsonObject{{QStringLiteral("name"),
                                                       QStringLiteral("x")},
                                                      {QStringLiteral("value"),
                                                       QStringLiteral("WRONG-DUP")},
                                                      {QStringLiteral("variablesReference"),
                                                       0}},
                                           QJsonObject{{QStringLiteral("name"),
                                                       QStringLiteral("y")},
                                                      {QStringLiteral("value"),
                                                       QStringLiteral("13")},
                                                      {QStringLiteral("variablesReference"),
                                                       0}},
                                           QJsonObject{{QStringLiteral("name"),
                                                       QStringLiteral("zzUnused")},
                                                      {QStringLiteral("value"),
                                                       QStringLiteral("never-on-line")},
                                                      {QStringLiteral("variablesReference"),
                                                       0}}}}});
                return;
            }
            if (command == QLatin1String("continue")) {
                respondOk(message, QJsonObject{{QStringLiteral("allThreadsContinued"),
                                                true}});
                sendEvent(QStringLiteral("continued"),
                          QJsonObject{{QStringLiteral("allThreadsContinued"),
                                       true}});
                return;
            }
            if (command == QLatin1String("terminate")) {
                g_terminateSeen = true;
                respondOk(message, QJsonObject{});
                return;
            }
        }
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
        && g_mode != QLatin1String("cancelreq")
        && g_mode != QLatin1String("breakpoints")
        && g_mode != QLatin1String("steps"))
        fail(QStringLiteral("unknown mode \"%1\"").arg(g_mode));
    if (args.size() > 2)
        g_argPath = args.at(2);
    if (args.size() > 3)
        g_frameLine = args.at(3).toInt();
    if (args.size() > 4)
        g_frameCol = args.at(4).toInt();

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
