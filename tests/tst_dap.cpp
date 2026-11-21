// The 7th ctest entry (PLAN car 4, machine D1): the EnvelopeCodec seam.
// Like tst_lsp this binary GROWS machine by machine (D2 landed src/dap +
// DapClient here, D3 the DapHub glue, D4 the demo door), and D1's half of
// the tripwire is OUTSIDE this file: the ENTIRE pre-existing tst_lsp
// binary passes with ZERO slot edits — that is the "JsonRpcCodec keeps LSP
// traffic byte-identical" claim made literal. What lives HERE is the other
// half: the codec surface itself (builder byte pins against the pre-seam
// wire), and RpcClient driven through a SECOND envelope — a test-local
// DAP-shape codec (the blueprint the real src/dap DapCodec grew from in
// D2): request_seq matching, event->notification normalization,
// success:false reading, the cancel-as-request shape and the
// disconnect-without-exit stop barrier. Machine D2 appended the production
// DapCodec byte pins and the DapClient lifecycle slots over the dapfake
// adapter (tests/dapfake), keeping every D1 slot and the test-local codec
// untouched — the D1 slots keep proving the MECHANISM is policy-free.
//
// Guiless no more: machine D3 landed the DapHub slots over REAL editors
// and flipped the main to QTEST_MAIN + offscreen, exactly as tst_lsp's
// machine 4 predicted. The D1/D2 slots keep their shape untouched — D3
// only APPENDS hub machines (and one test-side spawn/exit helper).

#include <QtTest>

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVector>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include "dap/DapClient.hpp"
#include "dap/DapCodec.hpp"
#include "glue/DapHub.hpp"
#include "glue/ProjectSession.hpp"
#include "protocol/EnvelopeCodec.hpp"
#include "protocol/JsonFraming.hpp"
#include "protocol/JsonRpcCodec.hpp"
#include "protocol/ProcessTransport.hpp"
#include "protocol/RpcClient.hpp"
#include "protocol/TcpTransport.hpp"
#include "view/CodeDocument.hpp"
#include "view/CodeEditor.hpp"

using namespace qmn::protocol;
using namespace qmn::dap;
using qmn::glue::DapHub;
using qmn::glue::ProjectSession;
using qmn::view::CodeDocument;
using qmn::view::CodeEditor;

namespace {

QByteArray wire(const QJsonObject& message)
{
    return QJsonDocument(message).toJson(QJsonDocument::Compact);
}

QJsonObject parsed(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

// Server-side stand-in (the tst_lsp harness shape, adapted: DAP frames
// carry `command`, not `method`, so both counters exist).
class FakePeer : public QObject {
public:
    explicit FakePeer(QObject* parent = nullptr) : QObject(parent)
    {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            client_ = server_.nextPendingConnection();
            connect(client_, &QTcpSocket::readyRead, this, [this] {
                framing_.feed(client_->readAll());
                QByteArray payload;
                while (!framing_.hasError() && framing_.poll(&payload)) {
                    const QJsonDocument doc = QJsonDocument::fromJson(payload);
                    if (doc.isObject())
                        messages.append(doc.object());
                }
            });
            connect(client_, &QTcpSocket::disconnected, this,
                    [this] { clientGone_ = true; });
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return server_.serverPort(); }
    bool haveClient() { return client_ != nullptr && !clientGone_; }
    bool clientGone() const { return clientGone_; }

    void send(const QJsonObject& message)
    {
        if (client_ && !clientGone_)
            client_->write(JsonFraming::encode(wire(message)));
    }
    void abortClient()
    {
        if (client_)
            client_->abort();
        clientGone_ = true;
    }

    int indexOf(std::function<bool(const QJsonObject&)> predicate) const
    {
        for (int i = 0; i < messages.size(); ++i)
            if (predicate(messages.at(i)))
                return i;
        return -1;
    }
    int commandCount(const QString& command) const
    {
        int n = 0;
        for (const QJsonObject& m : messages)
            if (m.value(QStringLiteral("command")).toString() == command)
                ++n;
        return n;
    }

    QVector<QJsonObject> messages;

private:
    QTcpServer server_;
    QTcpSocket* client_ = nullptr;
    bool clientGone_ = false;
    JsonFraming framing_;
};

// The test-local DAP-shape envelope: the seam's SECOND policy, and the
// blueprint machine D2's real src/dap DapCodec grows from (the production
// codec will add its spec-shape argument/body rules; what matters HERE is
// that RpcClient's mechanism rides it with zero client-side changes).
class TestDapCodec : public EnvelopeCodec {
public:
    QJsonObject buildRequest(qint64 seq, const QString& command,
                             const QJsonValue& arguments) const override
    {
        QJsonObject m;
        m.insert(QStringLiteral("type"), QStringLiteral("request"));
        m.insert(QStringLiteral("seq"), seq);
        m.insert(QStringLiteral("command"), command);
        if (!arguments.isUndefined())
            m.insert(QStringLiteral("arguments"), arguments);
        return m;
    }
    QJsonObject buildNotification(const QString&, const QJsonValue&,
                                  qint64) const override
    {
        return {}; // This test-local envelope has no outbound one-way —
                   // deliberately kept from D1 to pin RpcClient's
                   // empty-answer skip (the production DapCodec DOES have
                   // one: the handshake's initialized EVENT, pinned by
                   // machine D2's handshake slots instead)
    }
    QJsonObject buildResponse(const QJsonObject& incoming, const QJsonValue& result,
                              qint64 selfSeq) const override
    {
        QJsonObject m = baseResponse(incoming, selfSeq);
        m.insert(QStringLiteral("success"), true);
        if (!result.isUndefined())
            m.insert(QStringLiteral("body"), result);
        return m;
    }
    QJsonObject buildErrorResponse(const QJsonObject& incoming, int code,
                                   const QString& message, const QJsonValue&,
                                   qint64 selfSeq) const override
    {
        QJsonObject m = baseResponse(incoming, selfSeq);
        m.insert(QStringLiteral("success"), false);
        QJsonObject error;
        error.insert(QStringLiteral("id"), code);
        error.insert(QStringLiteral("format"), message);
        m.insert(QStringLiteral("error"), error);
        return m;
    }
    QJsonObject buildUnregisteredAnswer(const QJsonObject& request,
                                        qint64 selfSeq) const override
    {
        const QString command
            = request.value(QStringLiteral("command")).toString();
        QJsonObject m = baseResponse(request, selfSeq);
        m.insert(QStringLiteral("success"), false);
        QJsonObject error;
        error.insert(QStringLiteral("id"), 0); // DAP has no code for this
        error.insert(QStringLiteral("format"),
                     QStringLiteral("Unknown request: %1").arg(command));
        m.insert(QStringLiteral("error"), error);
        return m;
    }
    QJsonObject buildInvalidIdAnswer(const QJsonObject&) const override
    {
        return {}; // no numeric request_seq to answer INTO: classify already
                   // routed such frames to NotEnvelope; the empty answer
                   // (writeAnswer) writes nothing — the lenient path
    }
    QJsonObject buildCancel(qint64 targetId, qint64 selfSeq) const override
    {
        QJsonObject args;
        args.insert(QStringLiteral("requestId"), targetId);
        return buildRequest(selfSeq, QStringLiteral("cancel"), args);
    }
    bool cancelIsRequest() const override { return true; }
    bool responsesCarrySeq() const override { return true; }

    QString shutdownCommand() const override { return QStringLiteral("disconnect"); }
    QJsonValue shutdownParams() const override
    {
        QJsonObject args;
        args.insert(QStringLiteral("restart"), false);
        return args;
    }
    bool hasExitNotification() const override { return false; }

    Incoming classify(const QJsonObject& message) const override
    {
        Incoming in;
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("event")) {
            // The machine-D1 normalization: an event lands on the SAME
            // dispatch as a JSON-RPC notification (unknown events ignored).
            in.kind = Incoming::Kind::Notification;
            in.method = message.value(QStringLiteral("event")).toString();
            in.params = message.value(QStringLiteral("body"));
            return in;
        }
        if (type == QStringLiteral("request")) {
            const QJsonValue seq = message.value(QStringLiteral("seq"));
            if (!seq.isDouble()) {
                in.kind = Incoming::Kind::NotEnvelope;
                in.why = QStringLiteral("request has no numeric seq");
                return in;
            }
            in.kind = Incoming::Kind::Request;
            in.id = seq;
            in.method = message.value(QStringLiteral("command")).toString();
            in.params = message.value(QStringLiteral("arguments"));
            return in;
        }
        if (type == QStringLiteral("response")) {
            in.kind = Incoming::Kind::Response;
            return in;
        }
        in.kind = Incoming::Kind::NotEnvelope;
        in.why = QStringLiteral("not a DAP envelope: type member missing or unknown");
        return in;
    }
    bool responseShapeOk(const QJsonObject& message, QString* why) const override
    {
        if (!message.value(QStringLiteral("success")).isBool()) {
            *why = QStringLiteral("response without success boolean");
            return false;
        }
        return true;
    }
    bool responseMatchId(const QJsonObject& message, qint64* id,
                         QString* why) const override
    {
        const QJsonValue seq = message.value(QStringLiteral("request_seq"));
        if (!seq.isDouble()) {
            *why = QStringLiteral("response has no numeric request_seq");
            return false;
        }
        *id = static_cast<qint64>(seq.toDouble());
        return true;
    }
    bool decodeReply(const QJsonObject& message, EnvelopeReply* out,
                     QString*) const override
    {
        if (!message.value(QStringLiteral("success")).toBool()) {
            out->isError = true;
            const QJsonObject error = message.value(QStringLiteral("error")).toObject();
            out->code = error.value(QStringLiteral("id")).toInt(); // 0 when absent
            out->message = error.value(QStringLiteral("format"))
                               .toString(QStringLiteral("(no message)"));
            out->data = message.value(QStringLiteral("error")); // the whole table
            return true;
        }
        out->isError = false;
        out->result = message.value(QStringLiteral("body"));
        return true;
    }

private:
    static QJsonObject baseResponse(const QJsonObject& incoming, qint64 selfSeq)
    {
        QJsonObject m;
        m.insert(QStringLiteral("type"), QStringLiteral("response"));
        m.insert(QStringLiteral("seq"), selfSeq);
        m.insert(QStringLiteral("request_seq"), incoming.value(QStringLiteral("seq")));
        m.insert(QStringLiteral("command"), incoming.value(QStringLiteral("command")));
        return m;
    }
};

// The live-session harness for machine D2: DapClient + the dapfake factory.
// Same shape as tst_lsp's M2 harness: the factory counts spawns (the NO-
// AUTO-RESTART decision's oracle: spawnCount must stay 1 across a failed
// handshake and across a mid-session adapter death) and records each
// process's exit code at its closed() (dapfake's exit code is its wire-side
// verdict; recording inside the signal is the only safe read — the session
// teardown deletes the transport as soon as the child is reaped).
struct DapHarness {
    struct Spawn {
        ProcessTransport* transport = nullptr; // may die: never dereferenced
        int exitCode = -1;                     // -1 until recorded
    };

    QString program;
    QStringList modeArgs;
    int spawnCount = 0;
    QVector<Spawn> spawned;
    std::unique_ptr<DapClient> client;

    explicit DapHarness(const QString& modeArg)
        : program(QString::fromLatin1(QMN_TEST_DAP_SERVER))
        , modeArgs{modeArg}
    {
        client.reset(new DapClient(
            [this](QObject* parent) -> Transport* {
                auto* transport
                    = new ProcessTransport(program, modeArgs, parent);
                ++spawnCount;
                const int index = spawned.size();
                spawned.append(Spawn{transport, -1});
                QObject::connect(transport, &Transport::closed, transport,
                                 [this, index, transport] {
                                     spawned[index].exitCode = transport->exitCode();
                                 });
                return transport;
            },
            QStringLiteral("dapfake"))); // the adapterID initialize fills in
    }

    int exitCodeOf(int index) const
    {
        return index < spawned.size() ? spawned.at(index).exitCode : -1;
    }
};

// D3's spawn/exit oracle for the HUB slots: the DapHarness idea moved onto
// the hub's transport-factory seam (the hub builds its own client, so the
// test injects there — the very seam the D3 header promises stays honest).
// LIFETIME RULE: declare this BEFORE the hub it installs onto — the hub's
// dtor pump may still fire a closed() lambda, and it must find the record
// alive. The dapfake far-side audits (exit codes 2/3) carry the rest.
struct HubSpawns {
    QVector<int> exitCodes; // one entry per spawn, -1 until recorded

    void install(DapHub& hub)
    {
        hub.setTransportFactory([this](const QString& program, const QStringList& args,
                                       QObject* parent) -> Transport* {
            auto* transport = new ProcessTransport(program, args, parent);
            exitCodes.append(-1);
            const int index = exitCodes.size() - 1;
            QObject::connect(transport, &Transport::closed, transport,
                             [this, index, transport] {
                                 exitCodes[index] = transport->exitCode();
                             });
            return transport;
        });
    }

    int exitCodeOf(int index) const
    {
        return index < exitCodes.size() ? exitCodes.at(index) : -1;
    }
};

} // namespace

class TstDap : public QObject {
    Q_OBJECT
private:
    void openSession(FakePeer& peer, TcpTransport& transport, RpcClient& client)
    {
        transport.start();
        QTRY_VERIFY_WITH_TIMEOUT(peer.haveClient(), 3000);
        QVERIFY(!client.isGone());
    }

private slots:
    // ================================================== machine D1: codec surface
    // The JSON-RPC half: byte pins against the PRE-SEAM wire (these exact
    // compact frames are what RpcClient.cpp itself emitted before D1; the
    // fakeserver's transcript oracles in tst_lsp pin the same shapes live).
    void jsonRpcBuildersPinThePreSeamWire()
    {
        const JsonRpcCodec codec;
        QCOMPARE(wire(codec.buildRequest(7, QStringLiteral("m"), QJsonObject{{"a", 1}})),
                 QByteArray(R"({"id":7,"jsonrpc":"2.0","method":"m","params":{"a":1}})"));
        // Only UNDEFINED params are omitted; explicit null travels (the LSP
        // "params may be null" doors) — both pre-seam rules, unchanged:
        QCOMPARE(wire(codec.buildNotification(
                     QStringLiteral("exit"), QJsonValue(QJsonValue::Undefined), 0)),
                 QByteArray(R"({"jsonrpc":"2.0","method":"exit"})"));
        QCOMPARE(wire(codec.buildNotification(
                     QStringLiteral("m"), QJsonValue(QJsonValue::Null), 0)),
                 QByteArray(R"({"jsonrpc":"2.0","method":"m","params":null})"));
        QCOMPARE(wire(codec.buildResponse(parsed(R"({"id":5})"),
                                          QJsonObject{{"x", 1}}, 0)),
                 QByteArray(R"({"id":5,"jsonrpc":"2.0","result":{"x":1}})"));
        QCOMPARE(wire(codec.buildErrorResponse(
                     parsed(R"({"id":5})"), -32601, QStringLiteral("oops"),
                     QJsonObject{{"d", 1}}, 0)),
                 QByteArray(R"({"error":{"code":-32601,"data":{"d":1},"message":"oops"},)"
                            R"("id":5,"jsonrpc":"2.0"})"));
        QCOMPARE(wire(codec.buildCancel(9, 0)),
                 QByteArray(R"({"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":9}})"));
        // The stop-barrier vocabulary (phase 1 request + phase 2 exit):
        QCOMPARE(codec.shutdownCommand(), QStringLiteral("shutdown"));
        QVERIFY(codec.shutdownParams().isNull()); // explicit JSON null
        QVERIFY(codec.hasExitNotification());
        QCOMPARE(codec.exitCommand(), QStringLiteral("exit"));
        QVERIFY(!codec.cancelIsRequest());
        QVERIFY(!codec.responsesCarrySeq()); // LSP ids must NOT drift: byte-identity
    }

    void jsonRpcMechanicalAnswersPin()
    {
        // The M1 tripwire shapes: these answers are built WITHOUT any
        // openIncoming_ entry (the respond() guard would refuse them).
        const JsonRpcCodec codec;
        QCOMPARE(wire(codec.buildUnregisteredAnswer(
                     parsed(R"({"id":7,"method":"foo"})"), 0)),
                 QByteArray(R"({"error":{"code":-32601,"message":"method not found: foo"},)"
                            R"("id":7,"jsonrpc":"2.0"})"));
        QCOMPARE(wire(codec.buildInvalidIdAnswer(parsed(R"({"id":true,"method":"foo"})"))),
                 QByteArray(R"({"error":{"code":-32600,"message":"invalid request id type"},)"
                            R"("id":null,"jsonrpc":"2.0"})")); // JSON-RPC's id:null rule
    }

    void jsonRpcClassifyRoutesEveryShape()
    {
        const JsonRpcCodec codec;
        // (None of these frames carries "jsonrpc" — the lenient-inbound rule
        // the pre-seam code had, now the codec's call.)
        auto req = codec.classify(parsed(R"({"method":"m","id":3})"));
        QCOMPARE(int(req.kind), int(EnvelopeCodec::Incoming::Kind::Request));
        QCOMPARE(req.method, QStringLiteral("m"));
        QVERIFY(req.id.isDouble());

        auto note = codec.classify(parsed(R"({"method":"m"})"));
        QCOMPARE(int(note.kind), int(EnvelopeCodec::Incoming::Kind::Notification));
        QVERIFY(note.params.isUndefined());
        auto nullId = codec.classify(parsed(R"({"method":"m","id":null})"));
        QCOMPARE(int(nullId.kind), int(EnvelopeCodec::Incoming::Kind::Notification));

        auto resp = codec.classify(parsed(R"({"id":3,"result":1})"));
        QCOMPARE(int(resp.kind), int(EnvelopeCodec::Incoming::Kind::Response));

        auto junk = codec.classify(parsed(R"({"x":1})"));
        QCOMPARE(int(junk.kind), int(EnvelopeCodec::Incoming::Kind::NotEnvelope));
        QCOMPARE(junk.why, QStringLiteral("no method and no id in message"));
    }

    void jsonRpcResponseReadingPins()
    {
        const JsonRpcCodec codec;
        QString why;
        // The pinned ORDER the machine-1 tests grew up with: a frame whose
        // payload is broken reports THAT first, whatever its id looks like.
        QVERIFY(!codec.responseShapeOk(parsed(R"({"id":1})"), &why));
        QCOMPARE(why, QStringLiteral("response with neither result nor error"));
        qint64 id = 0;
        QVERIFY(!codec.responseMatchId(parsed(R"({"id":"s","result":1})"), &id, &why));
        QCOMPARE(why, QStringLiteral("response id is not the numeric id we sent"));

        EnvelopeReply reply;
        QVERIFY(codec.decodeReply(
            parsed(R"({"id":1,"error":{"code":-32001,"message":"boom","data":7}})"),
            &reply, &why));
        QVERIFY(reply.isError);
        QCOMPARE(reply.code, -32001);
        QCOMPARE(reply.message, QStringLiteral("boom"));
        QVERIFY(reply.data.isDouble());

        QVERIFY(codec.decodeReply(parsed(R"({"id":1,"result":{"ok":true}})"),
                                  &reply, &why));
        QVERIFY(!reply.isError);
        QVERIFY(reply.result.toObject().value(QStringLiteral("ok")).toBool());

        // The never-conflated banding: the codec's wire codes ARE RpcClient's
        // kWire* constants (one set of numbers, two spellings), and the DAP
        // codec's POSITIVE error.id table (slot below: 8) lands in neither
        // the kWire* negatives nor the local kErr* -33xxx band.
        QCOMPARE(int(JsonRpcCodec::kMethodNotFound), RpcClient::kWireMethodNotFound);
        QCOMPARE(int(JsonRpcCodec::kInvalidRequest), RpcClient::kWireInvalidRequest);
    }

    // The DAP half, pure-function side: the SECOND envelope rides the same
    // interface (this codec is test-local until machine D2 promotes a real
    // DapCodec into src/dap).
    void testDapCodecEnvelopeUnits()
    {
        const TestDapCodec codec;
        QCOMPARE(wire(codec.buildRequest(3, QStringLiteral("next"),
                                         QJsonObject{{"threadId", 1}})),
                 QByteArray(R"({"arguments":{"threadId":1},"command":"next","seq":3,"type":"request"})"));
        QVERIFY(codec.buildNotification(QStringLiteral("x"), QJsonValue(), 0).isEmpty());
        QVERIFY(codec.buildInvalidIdAnswer(parsed(R"({"command":"x"})")).isEmpty());

        auto ev = codec.classify(
            parsed(R"({"type":"event","seq":7,"event":"output","body":{"output":"hi"}})"));
        QCOMPARE(int(ev.kind), int(EnvelopeCodec::Incoming::Kind::Notification));
        QCOMPARE(ev.method, QStringLiteral("output")); // event name = dispatch key
        QCOMPARE(wire(ev.params.toObject()), QByteArray(R"({"output":"hi"})")); // body = params

        auto rev = codec.classify(
            parsed(R"({"type":"request","seq":42,"command":"runInTerminal"})"));
        QCOMPARE(int(rev.kind), int(EnvelopeCodec::Incoming::Kind::Request));
        QCOMPARE(rev.id.toInt(), 42); // adapter-side seq is the inbound id
        QCOMPARE(rev.method, QStringLiteral("runInTerminal")); // command = dispatch key

        auto noSeq = codec.classify(parsed(R"({"type":"request","command":"x"})"));
        QCOMPARE(int(noSeq.kind), int(EnvelopeCodec::Incoming::Kind::NotEnvelope));
        auto foreign = codec.classify(parsed(R"({"jsonrpc":"2.0","method":"x","id":1})"));
        QCOMPARE(int(foreign.kind), int(EnvelopeCodec::Incoming::Kind::NotEnvelope));
        QCOMPARE(foreign.why,
                 QStringLiteral("not a DAP envelope: type member missing or unknown"));

        QString why;
        QVERIFY(!codec.responseShapeOk(parsed(R"({"type":"response","request_seq":4})"), &why));
        QCOMPARE(why, QStringLiteral("response without success boolean"));
        qint64 id = 0;
        QVERIFY(!codec.responseMatchId(parsed(R"({"type":"response","success":true})"),
                                       &id, &why));
        QCOMPARE(why, QStringLiteral("response has no numeric request_seq"));

        // success:false + the OPTIONAL positive error.id table: code 8 when
        // present, 0 when the adapter omits it (never a local kErr* mask).
        EnvelopeReply reply;
        QVERIFY(codec.decodeReply(parsed(R"({"type":"response","request_seq":4,"success":false,)"
                                         R"("error":{"id":8,"format":"no such thread"}})"),
                                  &reply, &why));
        QVERIFY(reply.isError);
        QCOMPARE(reply.code, 8);
        QCOMPARE(reply.message, QStringLiteral("no such thread"));
        QVERIFY(reply.data.isObject()); // the WHOLE error table crosses (DAP has no "data")
        QVERIFY(codec.decodeReply(parsed(R"({"type":"response","request_seq":4,"success":false,)"
                                         R"("error":{}})"),
                                  &reply, &why));
        QCOMPARE(reply.code, 0); // omitted error.id stays 0, NOT a local code

        QCOMPARE(codec.shutdownCommand(), QStringLiteral("disconnect"));
        QCOMPARE(codec.shutdownParams().toObject().value(QStringLiteral("restart")).toBool(),
                 false);
        QVERIFY(!codec.hasExitNotification()); // DAP has no exit message
        QVERIFY(codec.cancelIsRequest());
        QVERIFY(codec.responsesCarrySeq());
    }

    // ================================= machine D1: the mechanism over a DAP wire
    // Everything below is RpcClient UNMODIFIED — id allocation, the pending
    // map, exactly-once, budgets, the two-phase stop, the gone-latch — driven
    // through the second codec. That is the seam claim made executable.
    void dapRequestResponseMatchesOnRequestSeq()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        TestDapCodec codec;
        RpcClient client(&transport, &codec);
        openSession(peer, transport, client);

        QVector<RpcClient::Reply> replies;
        client.request(QStringLiteral("next"), QJsonObject{{"threadId", 1}},
                       [&](const RpcClient::Reply& r) { replies.append(r); });
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("next")), 1, 3000);
        // Our first outbound message takes seq 1 (the counter is RpcClient's
        // own; responsesCarrySeq() only matters for OUR responses, slot
        // registeredReverseRequest...):
        QCOMPARE(wire(peer.messages.first()),
                 QByteArray(R"({"arguments":{"threadId":1},"command":"next","seq":1,"type":"request"})"));

        peer.send(parsed(R"({"type":"response","seq":100,"request_seq":1,"success":true,)"
                         R"("command":"next","body":{"allThreadsContinued":false}})"));
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QVERIFY(!replies.first().isError);
        QCOMPARE(wire(replies.first().result.toObject()),
                 QByteArray(R"({"allThreadsContinued":false})")); // body = the result
        QCOMPARE(client.pendingCount(), 0);
    }

    void dapWireErrorReadsSuccessFalseAndErrorId()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        TestDapCodec codec;
        RpcClient client(&transport, &codec);
        openSession(peer, transport, client);

        QVector<RpcClient::Reply> replies;
        client.request(QStringLiteral("stackTrace"), QJsonObject{{"threadId", 1}},
                       [&](const RpcClient::Reply& r) { replies.append(r); });
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("stackTrace")), 1, 3000);
        peer.send(parsed(R"({"type":"response","request_seq":1,"success":false,)"
                         R"("command":"stackTrace","error":{"id":8,"format":"no such thread"}})"));
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QVERIFY(replies.first().isError);
        QCOMPARE(replies.first().code, 8); // the adapter's POSITIVE error.id, verbatim
        QCOMPARE(replies.first().message, QStringLiteral("no such thread"));
        QVERIFY(replies.first().data.isObject());
    }

    void dapEventsRideTheNotificationPath()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        TestDapCodec codec;
        RpcClient client(&transport, &codec);
        openSession(peer, transport, client);

        QJsonValue captured(QJsonValue::Undefined);
        client.setNotificationHandler(QStringLiteral("output"),
                                      [&](const QJsonValue& params) { captured = params; });
        peer.send(parsed(R"({"type":"event","seq":7,"event":"output",)"
                         R"("body":{"category":"console","output":"hi"}})"));
        QTRY_VERIFY_WITH_TIMEOUT(!captured.isUndefined(), 3000);
        QCOMPARE(wire(captured.toObject()),
                 QByteArray(R"({"category":"console","output":"hi"})"));

        // The unknown-notification-ignored rule carried across unchanged:
        // an UNHANDLED event is silently dropped, the stream stays usable.
        peer.send(parsed(R"({"type":"event","seq":8,"event":"terminated"})"));
        QVector<RpcClient::Reply> replies;
        client.request(QStringLiteral("ping"), QJsonValue(QJsonValue::Undefined),
                       [&](const RpcClient::Reply& r) { replies.append(r); });
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("ping")), 1, 3000);
        peer.send(parsed(R"({"type":"response","request_seq":1,"success":true,"command":"ping"})"));
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QVERIFY(!replies.first().isError);
    }

    void unregisteredDapCommandAnsweredHonestly()
    {
        // "Always answer, never lie" rides the codec: the DAP shape is a
        // success:false response echoing command + request_seq, NOT -32601.
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        TestDapCodec codec;
        RpcClient client(&transport, &codec);
        openSession(peer, transport, client);

        peer.send(parsed(R"({"type":"request","seq":42,"command":"runInTerminal",)"
                         R"("arguments":{"kind":"local"}})"));
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("runInTerminal")), 1, 3000);
        QCOMPARE(wire(peer.messages.first()),
                 QByteArray(R"({"command":"runInTerminal",)"
                            R"("error":{"format":"Unknown request: runInTerminal","id":0},)"
                            R"("request_seq":42,"seq":1,"success":false,"type":"response"})"));
        QCOMPARE(client.pendingCount(), 0); // mechanical answers never touch the pending map
    }

    void registeredReverseRequestAnswersEchoingRequestSeq()
    {
        // The relay path machine D2's terminal/startDebugging relays ride:
        // handler by `command`, id := the adapter's seq, and the answer
        // ECHOES the full inbound request (DAP needs command + request_seq,
        // which is why openIncoming_ now stores the whole request object).
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        TestDapCodec codec;
        RpcClient client(&transport, &codec);
        openSession(peer, transport, client);

        bool answered = false;
        bool secondRefused = false;
        client.setRequestHandler(QStringLiteral("startDebugging"),
                                 [&](const QJsonObject& request) {
                                     answered = client.respond(
                                         request.value(QStringLiteral("seq")),
                                         QJsonObject{{"ok", true}});
                                     secondRefused = client.respond(
                                         request.value(QStringLiteral("seq")),
                                         QJsonValue(QJsonValue::Null));
                                 });
        peer.send(parsed(R"({"type":"request","seq":42,"command":"startDebugging"})"));
        QTRY_VERIFY_WITH_TIMEOUT(answered, 3000);
        QVERIFY(!secondRefused); // double answer must be refused
        QCOMPARE(wire(peer.messages.first()),
                 QByteArray(R"({"body":{"ok":true},"command":"startDebugging",)"
                            R"("request_seq":42,"seq":1,"success":true,"type":"response"})"));
        // responsesCarrySeq(): OUR response consumed seq 1 — the next
        // outbound continues at 2 (the per-direction counter DAP expects).
        client.request(QStringLiteral("configurationDone"),
                       QJsonValue(QJsonValue::Undefined), nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("configurationDone")), 1, 3000);
        QCOMPARE(peer.messages.last().value(QStringLiteral("seq")).toInt(), 2);
    }

    void dapCancelIsARequestAndLateResponsesIgnored()
    {
        // Seam-mechanics pin (NOT D2's product contract): the WIRE shape of
        // cancel is the codec's — DAP's `cancel` REQUEST with its own seq,
        // its arguments{requestId} pointing at the target. The cancel's own
        // response lands on the unmatched-ignore path. The LOCAL fulfillment
        // code is JSON-RPC-shaped (-32802) — cancel() speaks it; D2's real
        // DAP cancel rides the request door instead (documented there).
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        TestDapCodec codec;
        RpcClient client(&transport, &codec);
        openSession(peer, transport, client);

        QVector<RpcClient::Reply> replies;
        const qint64 target = client.request(
            QStringLiteral("stackTrace"), QJsonObject{{"threadId", 1}},
            [&](const RpcClient::Reply& r) { replies.append(r); }, 5000);
        QCOMPARE(target, qint64(1));
        client.cancel(target);

        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000); // local, immediate
        QVERIFY(replies.first().isError);
        QCOMPARE(replies.first().code, RpcClient::kWireRequestCancelled);
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("cancel")), 1, 3000);
        QCOMPARE(wire(peer.messages.last()),
                 QByteArray(R"({"arguments":{"requestId":1},"command":"cancel","seq":2,"type":"request"})"));

        // Late adapter responses to BOTH the target and the cancel request:
        // no pending entry, silently ignored, no second fire.
        peer.send(parsed(R"({"type":"response","request_seq":1,"success":true,)"
                         R"("command":"stackTrace","body":{"stackFrames":[]}})"));
        peer.send(parsed(R"({"type":"response","request_seq":2,"success":true,"command":"cancel"})"));
        QTest::qWait(200);
        QCOMPARE(replies.size(), 1);
    }

    void timeoutCancelSpeaksCodecShape()
    {
        // PLAN: timeout CANCELS remote work before eviction — the rule is
        // envelope-agnostic, the shape is the codec's. $/cancelRequest must
        // NEVER appear on a DAP wire.
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        TestDapCodec codec;
        RpcClient client(&transport, &codec);
        openSession(peer, transport, client);

        QVector<RpcClient::Reply> replies;
        client.request(QStringLiteral("slow"), QJsonValue(QJsonValue::Undefined),
                       [&](const RpcClient::Reply& r) { replies.append(r); }, 100);
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QCOMPARE(replies.first().code, RpcClient::kErrTimeout); // LOCAL code, not a wire lie
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("cancel")), 1, 3000);
        QCOMPARE(peer.messages.last().value(QStringLiteral("arguments"))
                     .toObject().value(QStringLiteral("requestId")).toInt(), 1);
        QCOMPARE(peer.indexOf([](const QJsonObject& m) {
                     return m.value(QStringLiteral("method")).toString()
                            == QLatin1String("$/cancelRequest");
                 }), -1);
    }

    void dapStopBarrierDisconnectsAndNeverExits()
    {
        // The two-phase barrier, codec-shaped vocabulary: phase 1 is the
        // `disconnect` REQUEST (restart:false), phase 2 has NO final
        // message (LSP's exit does not exist for DAP) — then the transport
        // teardown (the kill escalation is the Transport's).
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        TestDapCodec codec;
        RpcClient client(&transport, &codec);
        openSession(peer, transport, client);

        client.stop();
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("disconnect")), 1, 3000);
        QCOMPARE(int(client.stopPhase()), int(RpcClient::StopPhase::Stopping));
        QCOMPARE(wire(peer.messages.first()),
                 QByteArray(R"({"arguments":{"restart":false},"command":"disconnect",)"
                            R"("seq":1,"type":"request"})"));
        peer.send(parsed(R"({"type":"response","request_seq":1,"success":true,"command":"disconnect"})"));
        QTRY_VERIFY_WITH_TIMEOUT(int(client.stopPhase()) == int(RpcClient::StopPhase::Stopped), 3000);

        // NO exit message, ever, and no `method` member anywhere: this wire
        // never spoke JSON-RPC.
        QCOMPARE(peer.commandCount(QStringLiteral("exit")), 0);
        QCOMPARE(peer.indexOf([](const QJsonObject& m) {
                     return m.contains(QStringLiteral("method"));
                 }), -1);
        QTRY_VERIFY_WITH_TIMEOUT(peer.clientGone(), 3000); // the barrier tears the transport down
    }

    void wrongEnvelopeSurfacedStreamStaysUsable()
    {
        // RpcClient no longer sniffs "method"/"id" itself — an LSP-shaped
        // frame on a DAP session is the codec's NotEnvelope: surfaced via
        // malformedMessage (never swallowed), the transport stays open.
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        TestDapCodec codec;
        RpcClient client(&transport, &codec);
        openSession(peer, transport, client);

        QSignalSpy malformedSpy(&client, &RpcClient::malformedMessage);
        peer.send(parsed(R"({"jsonrpc":"2.0","method":"x","id":1})"));
        QTRY_COMPARE_WITH_TIMEOUT(malformedSpy.count(), 1, 3000);
        QCOMPARE(malformedSpy.at(0).at(0).toString(),
                 QStringLiteral("not a DAP envelope: type member missing or unknown"));

        QVector<RpcClient::Reply> replies;
        client.request(QStringLiteral("ping"), QJsonValue(QJsonValue::Undefined),
                       [&](const RpcClient::Reply& r) { replies.append(r); });
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("ping")), 1, 3000);
        peer.send(parsed(R"({"type":"response","request_seq":1,"success":true,"command":"ping"})"));
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QVERIFY(!replies.first().isError);
    }

    void localFailureCodesStayLocalOnTheDapWire()
    {
        // The iide anti-pattern rule, extended to the second envelope: a
        // transport death is the LOCAL kErrTransportClosed band — never a
        // made-up DAP error.id (the positive table belongs to the adapter).
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        TestDapCodec codec;
        RpcClient client(&transport, &codec);
        openSession(peer, transport, client);

        QSignalSpy goneSpy(&client, &RpcClient::transportGone);
        QVector<RpcClient::Reply> replies;
        client.request(QStringLiteral("next"), QJsonObject{{"threadId", 1}},
                       [&](const RpcClient::Reply& r) { replies.append(r); });
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("next")), 1, 3000);
        peer.abortClient();
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QCOMPARE(replies.first().code, RpcClient::kErrTransportClosed);
        QCOMPARE(goneSpy.count(), 1);
    }

    // ================================================= machine D2: src/dap
    // The production DapCodec (the spec-shaped upgrade of the D1 blueprint)
    // and DapClient: the pinned handshake, the NO-auto-restart lifecycle,
    // the event relay, the reverse-request relay with honest defaults, and
    // the inbound-cancel policy — over the dapfake adapter (whose exit code
    // is the far-side oracle for everything the client cannot audit).

    void dapCodecWireShapesMatchTheSpec()
    {
        const DapCodec codec;
        // Undefined arguments/body members are OMITTED — the shape real
        // adapters emit:
        QCOMPARE(wire(codec.buildRequest(1, QStringLiteral("next"),
                                         QJsonObject{{QStringLiteral("threadId"), 1}})),
                 QByteArray(R"({"arguments":{"threadId":1},"command":"next","seq":1,"type":"request"})"));
        QCOMPARE(wire(codec.buildRequest(2, QStringLiteral("configurationDone"),
                                         QJsonValue(QJsonValue::Undefined))),
                 QByteArray(R"({"command":"configurationDone","seq":2,"type":"request"})"));
        // The outbound one-way is the EVENT frame (the D2 correction to the
        // D1 note about empty one-ways — the production codec HAS one):
        QCOMPARE(wire(codec.buildNotification(QStringLiteral("initialized"),
                                              QJsonObject{}, 2)),
                 QByteArray(R"({"body":{},"event":"initialized","seq":2,"type":"event"})"));
        QCOMPARE(wire(codec.buildNotification(QStringLiteral("progress"),
                                              QJsonValue(QJsonValue::Undefined), 5)),
                 QByteArray(R"({"event":"progress","seq":5,"type":"event"})"));
        QVERIFY(codec.buildNotification(QString(), QJsonObject{}, 6).isEmpty());
        // Responses echo the pairing key (request_seq) AND the command:
        const QJsonObject rev
            = parsed(R"({"type":"request","seq":42,"command":"stackTrace"})");
        QCOMPARE(wire(codec.buildResponse(rev,
                                          QJsonObject{{QStringLiteral("stackFrames"), QJsonArray{}}}, 3)),
                 QByteArray(R"({"body":{"stackFrames":[]},"command":"stackTrace","request_seq":42,"seq":3,"success":true,"type":"response"})"));
        QCOMPARE(wire(codec.buildResponse(rev, QJsonValue(QJsonValue::Undefined), 3)),
                 QByteArray(R"({"command":"stackTrace","request_seq":42,"seq":3,"success":true,"type":"response"})"));
        // success:false carries error{id,format}; an OBJECT data MERGES into
        // the error table (DAP has no nested "data" member); a non-object
        // data has no legal spelling here and is dropped:
        QCOMPARE(wire(codec.buildErrorResponse(
                     parsed(R"({"type":"request","seq":9,"command":"next"})"), 0,
                     QStringLiteral("no such thread"),
                     QJsonObject{{QStringLiteral("showCrash"), true}}, 4)),
                 QByteArray(R"({"command":"next","error":{"format":"no such thread","id":0,"showCrash":true},"request_seq":9,"seq":4,"success":false,"type":"response"})"));
        QCOMPARE(wire(codec.buildErrorResponse(
                     parsed(R"({"type":"request","seq":9,"command":"next"})"), 3,
                     QStringLiteral("boom"), QStringLiteral("nope"), 4)),
                 QByteArray(R"({"command":"next","error":{"format":"boom","id":3},"request_seq":9,"seq":4,"success":false,"type":"response"})"));
        // The mechanical answer: honest success:false, error.id 0 (the
        // POSITIVE table is the adapter's — we invent no code):
        QCOMPARE(wire(codec.buildUnregisteredAnswer(
                     parsed(R"({"type":"request","seq":8,"command":"bogus"})"), 5)),
                 QByteArray(R"({"command":"bogus","error":{"format":"Unknown request: bogus","id":0},"request_seq":8,"seq":5,"success":false,"type":"response"})"));
        QVERIFY(codec.buildInvalidIdAnswer(parsed(R"({"type":"request","command":"x"})")).isEmpty());
        // cancel is a REQUEST; its target lives in arguments.requestId:
        QCOMPARE(wire(codec.buildCancel(7, 3)),
                 QByteArray(R"({"arguments":{"requestId":7},"command":"cancel","seq":3,"type":"request"})"));
        QCOMPARE(codec.cancelTargetId(parsed(R"({"type":"request","seq":1,"command":"cancel","arguments":{"requestId":42}})")), qint64(42));
        QCOMPARE(codec.cancelTargetId(parsed(R"({"type":"request","seq":1,"command":"cancel"})")), qint64(0)); // absent -> "nothing to cancel"
        QCOMPARE(codec.cancelCommand(), QStringLiteral("cancel")); // NOT $/cancelRequest
        QVERIFY(codec.cancelIsRequest());
        QVERIFY(codec.responsesCarrySeq());
        // The stop vocabulary: disconnect(restart:false), NO exit frame
        // (the base's empty exitCommand stays empty):
        QCOMPARE(codec.shutdownCommand(), QStringLiteral("disconnect"));
        QCOMPARE(wire(codec.shutdownParams().toObject()), QByteArray(R"({"restart":false})"));
        QVERIFY(!codec.hasExitNotification());
        QVERIFY(codec.exitCommand().isEmpty());

        // classify + response reading on the PRODUCTION codec (the D1 unit
        // slot pinned the blueprint; these pin the real one agrees):
        auto ev = codec.classify(parsed(R"({"type":"event","seq":7,"event":"output","body":{"output":"hi"}})"));
        QCOMPARE(int(ev.kind), int(EnvelopeCodec::Incoming::Kind::Notification));
        QCOMPARE(ev.method, QStringLiteral("output"));
        auto req = codec.classify(parsed(R"({"type":"request","seq":42,"command":"runInTerminal"})"));
        QCOMPARE(int(req.kind), int(EnvelopeCodec::Incoming::Kind::Request));
        QCOMPARE(req.id.toInt(), 42);
        auto noSeq = codec.classify(parsed(R"({"type":"request","command":"x"})"));
        QCOMPARE(int(noSeq.kind), int(EnvelopeCodec::Incoming::Kind::NotEnvelope));
        auto foreign = codec.classify(parsed(R"({"jsonrpc":"2.0","method":"x","id":1})"));
        QCOMPARE(int(foreign.kind), int(EnvelopeCodec::Incoming::Kind::NotEnvelope));
        QString why;
        QVERIFY(!codec.responseShapeOk(parsed(R"({"type":"response","request_seq":4})"), &why));
        QCOMPARE(why, QStringLiteral("response without success boolean"));
        qint64 id = 0;
        QVERIFY(!codec.responseMatchId(parsed(R"({"type":"response","success":true})"), &id, &why));
        EnvelopeReply reply;
        QVERIFY(codec.decodeReply(parsed(R"({"type":"response","request_seq":4,"success":false,)"
                                         R"("error":{"id":8,"format":"no such thread"}})"), &reply, &why));
        QVERIFY(reply.isError);
        QCOMPARE(reply.code, 8); // the adapter's POSITIVE error.id, verbatim
        QVERIFY(codec.decodeReply(parsed(R"({"type":"response","request_seq":4,"success":false,"error":{}})"),
                                  &reply, &why));
        QCOMPARE(reply.code, 0); // omitted id stays 0, NEVER a local kErr* mask
        QVERIFY(codec.decodeReply(parsed(R"({"type":"response","request_seq":4,"success":true})"),
                                  &reply, &why));
        QVERIFY(!reply.isError);
        QVERIFY(reply.result.isUndefined()); // body-less command: honest Undefined
    }

    void dapHandshakeSpeaksInitializeThenInitializedEvent()
    {
        // The pinned handshake at wire-byte level over the FakePeer (the
        // dapfake slots pin the ORDER far-side: a lying order exits 2):
        // initialize REQUEST -> success -> initialized EVENT -> Ready.
        FakePeer peer;
        QVERIFY(peer.listen());
        DapClient client([&peer](QObject* parent) -> Transport* {
            return new TcpTransport(QStringLiteral("127.0.0.1"), peer.port(), parent);
        }, QStringLiteral("testadapter"));
        QSignalSpy readySpy(&client, &DapClient::ready);
        QVector<std::pair<QString, QJsonValue>> events;
        QObject::connect(&client, &DapClient::dapEvent,
                         [&](const QString& name, const QJsonValue& body) {
                             events.append({name, body});
                         });
        client.start(QJsonObject{{QStringLiteral("pathFormat"), QStringLiteral("path")}});
        QTRY_VERIFY_WITH_TIMEOUT(peer.haveClient(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("initialize")), 1, 3000);
        // The gate is CLOSED before the handshake answers:
        QCOMPARE(client.state(), DapClient::State::Starting);
        QCOMPARE(readySpy.count(), 0);
        // The initialize ARGUMENTS (the PLAN D2 bundle): identity keys
        // filled when absent (the clientInfo-is-configured precedent from
        // the LSP hub), the 1-BASED PAIR declared — it is the spec default
        // and the hub's ±1 conversions lean on it always being true — and
        // NO clientCapabilities member at all (honest-absence style). The
        // locale/clientOS values are host-shaped, so this pin reads the
        // shape, not the bytes:
        const QJsonObject initArgs
            = peer.messages.first().value(QStringLiteral("arguments")).toObject();
        QCOMPARE(initArgs.value(QStringLiteral("adapterID")).toString(),
                 QStringLiteral("testadapter"));
        QCOMPARE(initArgs.value(QStringLiteral("clientID")).toString(),
                 QStringLiteral("qtmonaco-native"));
        QCOMPARE(initArgs.value(QStringLiteral("clientName")).toString(),
                 QStringLiteral("qtmonaco-native"));
        QVERIFY(!initArgs.value(QStringLiteral("clientOS")).toString().isEmpty());
        QVERIFY(!initArgs.value(QStringLiteral("locale")).toString().isEmpty());
        QVERIFY(initArgs.value(QStringLiteral("linesStartAt1")).toBool());
        QVERIFY(initArgs.value(QStringLiteral("columnsStartAt1")).toBool());
        QVERIFY(!initArgs.contains(QStringLiteral("clientCapabilities")));
        QCOMPARE(initArgs.value(QStringLiteral("pathFormat")).toString(),
                 QStringLiteral("path")); // the caller's keys ride along

        peer.send(parsed(R"({"type":"response","seq":1,"request_seq":1,"success":true,)"
                         R"("command":"initialize","body":{"supportsConfigurationDoneRequest":true}})"));
        QTRY_COMPARE_WITH_TIMEOUT(peer.messages.size(), 2, 3000);
        // The SECOND handshake half: the initialized EVENT frame, on our OWN
        // seq (2 — every DAP message consumes one, and responsesCarrySeq()
        // is why notify() takes it):
        QCOMPARE(wire(peer.messages.at(1)),
                 QByteArray(R"({"body":{},"event":"initialized","seq":2,"type":"event"})"));
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 3000);
        QCOMPARE(client.state(), DapClient::State::Ready);
        // On this wire the capabilities ARE the initialize result (no
        // nested "capabilities" member):
        QVERIFY(client.adapterCapabilities()
                    .value(QStringLiteral("supportsConfigurationDoneRequest"))
                    .toBool());

        // Commands keep the single outbound counter (3 = next, 4 = the
        // event() door's frame), and a normal reply decodes:
        QVector<RpcClient::Reply> replies;
        client.request(QStringLiteral("next"), QJsonObject{{QStringLiteral("threadId"), 1}},
                       [&](const RpcClient::Reply& r) { replies.append(r); });
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("next")), 1, 3000);
        QCOMPARE(peer.messages.last().value(QStringLiteral("seq")).toInt(), 3);
        client.event(QStringLiteral("progressTick"), QJsonObject{{QStringLiteral("v"), 1}});
        QTRY_COMPARE_WITH_TIMEOUT(peer.messages.size(), 4, 3000);
        QCOMPARE(wire(peer.messages.at(3)),
                 QByteArray(R"({"body":{"v":1},"event":"progressTick","seq":4,"type":"event"})"));
        peer.send(parsed(R"({"type":"response","request_seq":3,"success":true,"command":"next","body":{"allThreadsContinued":false}})"));
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QVERIFY(!replies.first().isError);
        QVERIFY(!replies.first().result.toObject()
                     .value(QStringLiteral("allThreadsContinued")).toBool());

        // The `capabilities` EVENT RE-STORES the bag (adapters publish
        // revisions that way) — and the event still rides the dapEvent
        // relay, hub-side:
        peer.send(parsed(R"({"type":"event","seq":9,"event":"capabilities",)"
                         R"("body":{"supportsTerminateRequest":true}})"));
        QTRY_VERIFY_WITH_TIMEOUT(client.adapterCapabilities()
                                     .value(QStringLiteral("supportsTerminateRequest"))
                                     .toBool(), 3000);
        QVERIFY(!client.adapterCapabilities().contains(
            QStringLiteral("supportsConfigurationDoneRequest"))); // RE-stored, not merged
        QCOMPARE(events.last().first, QStringLiteral("capabilities"));

        // The stop barrier through the real client: the disconnect REQUEST
        // (restart:false, seq 5), NO exit frame, then the teardown:
        client.stop();
        QTRY_COMPARE_WITH_TIMEOUT(peer.commandCount(QStringLiteral("disconnect")), 1, 3000);
        QCOMPARE(wire(peer.messages.last()),
                 QByteArray(R"({"arguments":{"restart":false},"command":"disconnect","seq":5,"type":"request"})"));
        peer.send(parsed(R"({"type":"response","request_seq":5,"success":true,"command":"disconnect"})"));
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), DapClient::State::Stopped, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(peer.clientGone(), 3000);
        QCOMPARE(peer.indexOf([](const QJsonObject& m) {
                     return m.value(QStringLiteral("command")).toString()
                                == QLatin1String("exit");
                 }), -1); // DAP has no exit message, the barrier never sends one
    }

    void dapCommandsBeforeReadyAnswerOnceLocally()
    {
        // The state gate, WITHOUT any transport: the feature door's contract
        // is "answers exactly once" (queued, never inside the call), with
        // the LOCAL kErrNotRunning code — and the factory was never touched.
        int spawns = 0;
        DapClient client([&spawns](QObject*) -> Transport* {
            ++spawns;
            return nullptr;
        }, QStringLiteral("never-spawned"));
        QCOMPARE(client.state(), DapClient::State::NotStarted);

        QVector<RpcClient::Reply> replies;
        const qint64 id = client.request(QStringLiteral("threads"), QJsonObject{},
                                         [&](const RpcClient::Reply& r) { replies.append(r); });
        QCOMPARE(id, qint64(-1));
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QVERIFY(replies.first().isError);
        QCOMPARE(replies.first().code, RpcClient::kErrNotRunning);
        QTest::qWait(200);
        QCOMPARE(replies.size(), 1);
        QCOMPARE(spawns, 0);

        // stop() from NotStarted is a bare honest terminal:
        client.stop();
        QCOMPARE(client.state(), DapClient::State::Stopped);
    }

    void dapBasicSessionRunsHandshakeEventsAndCleanRestart()
    {
        DapHarness h(QStringLiteral("basic"));
        QSignalSpy readySpy(h.client.get(), &DapClient::ready);
        QSignalSpy stderrSpy(h.client.get(), &DapClient::serverStderr);
        QVector<std::pair<QString, QJsonValue>> events;
        QObject::connect(h.client.get(), &DapClient::dapEvent,
                         [&](const QString& name, const QJsonValue& body) {
                             events.append({name, body});
                         });
        h.client->start();
        // The budgets here ride 30000, NOT the usual 5000: these slots spawn
        // children, and a starved host (the tst_lsp lesson of 2026-11-21)
        // must read as slow, not as broken.
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 30000);
        QCOMPARE(h.client->state(), DapClient::State::Ready);
        QCOMPARE(h.spawnCount, 1);
        QCOMPARE(h.client->adapterCapabilities().value(QStringLiteral("dapfakeMode")).toString(),
                 QStringLiteral("basic"));
        QVERIFY(h.client->adapterCapabilities()
                    .value(QStringLiteral("supportsCancelRequest")).toBool());

        // The channel split, exercised from the far side (the PLAN-pinned
        // PROTOCOL-vs-STDERR rule carries across cars): the dapfake's stderr
        // line reached serverStderr BEFORE the handshake, its PROTOCOL
        // marker reached dapEvent AFTER — and nothing crossed over.
        QTRY_VERIFY_WITH_TIMEOUT(
            std::any_of(events.begin(), events.end(),
                        [](const std::pair<QString, QJsonValue>& e) {
                            return e.first == QLatin1String("output");
                        }), 5000);
        bool sawStderrMarker = false;
        for (const auto& args : stderrSpy)
            if (args.at(0).toString().contains(QLatin1String("DAPOUT-STDERR-MARKER")))
                sawStderrMarker = true;
        QVERIFY(sawStderrMarker);
        for (const auto& e : events)
            QVERIFY(!e.second.toObject()
                         .value(QStringLiteral("output")).toString()
                         .contains(QLatin1String("DAPOUT-STDERR-MARKER")));

        // Handshake-gated commands answer normally:
        QVector<RpcClient::Reply> replies;
        h.client->request(QStringLiteral("next"), QJsonObject{{QStringLiteral("threadId"), 1}},
                          [&](const RpcClient::Reply& r) { replies.append(r); });
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 30000);
        QVERIFY(!replies.first().isError);
        QVERIFY(!replies.first().result.toObject()
                     .value(QStringLiteral("allThreadsContinued")).toBool());

        // restartServer() in Ready is a NO-OP: no silent mid-session
        // respawn (the no-auto-restart decision's other half).
        h.client->restartServer();
        QTest::qWait(300);
        QCOMPARE(h.spawnCount, 1);
        QCOMPARE(h.client->state(), DapClient::State::Ready);

        // The clean stop: the disconnect barrier; dapfake exits 0, which IS
        // its audit (it exits 2 on a broken handshake order, a missing
        // initialized event, a non-monotonic client seq or a bogus command).
        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(), DapClient::State::Stopped, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0, 5000);

        // The restart door from Stopped: a FRESH session, the stored
        // initialize arguments replayed (ready() fires on EVERY re-ready —
        // D3's hub re-pushes breakpoints on exactly this signal).
        h.client->restartServer();
        QTRY_COMPARE_WITH_TIMEOUT(h.spawnCount, 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 2, 30000);
        QCOMPARE(h.client->state(), DapClient::State::Ready);
        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(), DapClient::State::Stopped, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(1), 0, 5000);
    }

    void dapInitializeFailureMeansFailedWithNoRestart()
    {
        DapHarness h(QStringLiteral("initfail"));
        QSignalSpy failedSpy(h.client.get(), &DapClient::failed);
        QSignalSpy readySpy(h.client.get(), &DapClient::ready);
        h.client->start();
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 30000);
        QCOMPARE(h.client->state(), DapClient::State::Failed);
        QCOMPARE(readySpy.count(), 0);
        QVERIFY(failedSpy.at(0).at(0).toString()
                    .contains(QStringLiteral("dapfake cannot start")));
        // NO restart budget exists for adapters: the spawn count stays 1.
        QTest::qWait(300);
        QCOMPARE(h.spawnCount, 1);
        // enterFailed still shut the pipe honestly: disconnect answered,
        // dapfake exits 0 (and it exits 2 if an initialized event ever
        // arrived — none did).
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0, 5000);
        // The Failed-state feature door keeps answering locally:
        QVector<RpcClient::Reply> replies;
        h.client->request(QStringLiteral("threads"), QJsonObject{},
                          [&](const RpcClient::Reply& r) { replies.append(r); });
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QCOMPARE(replies.first().code, RpcClient::kErrNotRunning);
    }

    void dapDeafHandshakeHitsTheInitializeBudget()
    {
        // Slow by DESIGN: this slot waits out the handshake budget (the SAME
        // kInitializeTimeoutMs PLAN pinned for LSP — DapClient requests
        // initialize with it, not the feature default) and reads the
        // failure honestly.
        DapHarness h(QStringLiteral("deaf"));
        QSignalSpy failedSpy(h.client.get(), &DapClient::failed);
        h.client->start();
        QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() >= 1, 20000);
        QCOMPARE(h.client->state(), DapClient::State::Failed);
        QTest::qWait(300);
        QCOMPARE(h.spawnCount, 1);
        // Timeout CANCELS remote work before eviction (envelope-agnostic):
        // the cancel frame on this wire was a `cancel` REQUEST — dapfake
        // tolerates exactly that and exits 2 on anything else; it then
        // answered our disconnect and exited 0.
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0, 5000);
    }

    void dapAdapterDeathMeansFailedAndSpawnCountStaysOne()
    {
        DapHarness h(QStringLiteral("dead"));
        QSignalSpy readySpy(h.client.get(), &DapClient::ready);
        QSignalSpy failedSpy(h.client.get(), &DapClient::failed);
        h.client->start();
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 30000);
        // The adapter dies mid-session (dapfake _exit(7) right after the
        // handshake): the LspClient restart-budget branch deliberately has
        // NO twin — a crashed adapter killed the USER'S debug session, and
        // a silent respawn would resurrect a process whose whole
        // breakpoint/thread world is gone (PLAN-decided for car 4).
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
        QCOMPARE(h.client->state(), DapClient::State::Failed);
        QTest::qWait(300);
        QCOMPARE(h.spawnCount, 1);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 7, 5000);
        // The restart door is the EXPLICIT one (and D4's demo shows it):
        // Failed -> a fresh session, ready() fires again...
        h.client->restartServer();
        QTRY_COMPARE_WITH_TIMEOUT(h.spawnCount, 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 2, 30000);
        // ...and the same mode kills the second session too:
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 2, 5000);
        QCOMPARE(h.client->state(), DapClient::State::Failed);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(1), 7, 5000);
        QCOMPARE(h.spawnCount, 2); // died once per manual attempt, nothing in between
    }

    void dapEveryEventRelaysWithBodyOrWithout()
    {
        DapHarness h(QStringLiteral("events"));
        QSignalSpy readySpy(h.client.get(), &DapClient::ready);
        QVector<std::pair<QString, QJsonValue>> events;
        QObject::connect(h.client.get(), &DapClient::dapEvent,
                         [&](const QString& name, const QJsonValue& body) {
                             events.append({name, body});
                         });
        h.client->start();
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 30000);
        // The generic relay: the OUTPUT event with its body verbatim...
        QTRY_COMPARE_WITH_TIMEOUT(events.size(), 2, 5000);
        QCOMPARE(events.at(0).first, QStringLiteral("output"));
        QCOMPARE(events.at(0).second.toObject()
                     .value(QStringLiteral("output")).toString(),
                 QStringLiteral("dapfake-out"));
        // ...and the TERMINATED event with NO body member — relayed as an
        // Undefined QJsonValue (the DapHub reads terminated from this relay).
        QCOMPARE(events.at(1).first, QStringLiteral("terminated"));
        QVERIFY(events.at(1).second.isUndefined());
        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(), DapClient::State::Stopped, 5000);
        // Exit 0 doubles as "dapfake spoke nothing else on the wire" (a
        // stray bogus command exits 2 there).
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0, 5000);
    }

    void dapReverseRelayHonestDefaultAndGlueFirstTurn()
    {
        DapHarness h(QStringLiteral("reverse"));
        QSignalSpy readySpy(h.client.get(), &DapClient::ready);
        QStringList relayed;
        bool glueAnswered = false;
        QObject::connect(h.client.get(), &DapClient::dapRequest,
                         [&](const QJsonObject& request) {
                             const QString command
                                 = request.value(QStringLiteral("command")).toString();
                             relayed.append(command);
                             if (command == QLatin1String("startDebugging")) {
                                 // Glue's FIRST turn wins against the honest
                                 // default (first answer wins, RpcClient
                                 // refuses the second): answer DURING the
                                 // signal, keyed by the request's own seq.
                                 glueAnswered = h.client->respond(
                                     request.value(QStringLiteral("seq")),
                                     QJsonObject{{QStringLiteral("ok"), true}});
                             }
                         });
        h.client->start();
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 30000);
        QTRY_COMPARE_WITH_TIMEOUT(relayed.size(), 2, 5000);
        QCOMPARE(relayed.at(0), QStringLiteral("runInTerminal"));
        // runInTerminal: the TEST never answered — dapfake validated the
        // layer's honest in-envelope failure (success:false, error.id 0,
        // format naming "not handled: runInTerminal") and only then sent...
        QCOMPARE(relayed.at(1), QStringLiteral("startDebugging"));
        QVERIFY(glueAnswered);
        // dapfake validated the glue's success:true body{ok:true}. Its
        // THIRD battery leg — progressStart — must NOT appear in `relayed`:
        // the static answer shelf ({} — real progress UI is out of car 4)
        // outranks the relay, and dapfake validated the {} shape itself.
        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(), DapClient::State::Stopped, 5000);
        QCOMPARE(relayed.size(), 2); // the shelf answer never reached the relay
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0, 5000);
    }

    void dapInboundCancelFulfillsLocalPendingExactlyOnce()
    {
        DapHarness h(QStringLiteral("cancelreq"));
        QSignalSpy readySpy(h.client.get(), &DapClient::ready);
        h.client->start();
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 30000);

        // The generous timeout keeps the LOCAL timeout race out of this
        // slot: the ONLY cancel allowed on this wire travels from dapfake
        // (a client-side timeout would emit an outbound cancel and dapfake
        // exits 2 — so cancel must win well before 20 s).
        QVector<RpcClient::Reply> replies;
        h.client->request(QStringLiteral("slow"), QJsonObject{},
                          [&](const RpcClient::Reply& r) { replies.append(r); },
                          20000);
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 30000);
        QVERIFY(replies.first().isError);
        // The LOCAL code, not a wire lie and not a fabricated success:
        QCOMPARE(replies.first().code, RpcClient::kErrCancelledByPeer);

        // dapfake validated BOTH cancel answers itself: the bogus-id one
        // got the honest "cannot be cancelled" failure, the live one got
        // success:true — and then it answered the cancelled `slow` request
        // LATE (spec-best-effort). The client must swallow that silently:
        // exactly-once fulfilment already fired above.
        QTest::qWait(300);
        QCOMPARE(replies.size(), 1);

        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(), DapClient::State::Stopped, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0, 5000);
    }

    // ================================================== machine D3: the hub
    // The DapHub over REAL editors (offscreen), dapfake as the far side, and
    // the HubSpawns seam as the near-side spawn/exit oracle. Every slot ends
    // with the far-side contract dapfake reads (a wire lie exits 2, a hang
    // exits 3) — the same two-oracle sandwich tst_lsp's hub machines ride.

    void dapHubRefusesEverythingWithoutAnAdapter()
    {
        // The no-session honesty sweep (no process is ever spawned here —
        // and the absence itself is pinned by every door's return):
        DapHub hub;
        qmn::view::CodeEditor ed; // NO document uri, NO project attached

        QVERIFY(!hub.setBreakpoints(&ed, {0})); // a breakpoint needs an identity
        QVERIFY(hub.requestedBreakpoints(&ed).isEmpty());
        QVERIFY(hub.breakpointMap(&ed).isEmpty());
        QCOMPARE(hub.client(), nullptr);
        QVERIFY(!hub.isDebugging());
        QVERIFY(!hub.isReady());
        QVERIFY(hub.adapterCapabilities().isEmpty());

        hub.configurationDone(); // honest no-ops on a session-less hub:
        hub.stopDebugging();
        QVERIFY(!hub.terminateDebugging());
        QCOMPARE(hub.request(QStringLiteral("ping"),
                             QJsonValue(QJsonValue::Undefined)), qint64(-1));
        QVERIFY(!hub.startDebugging(QString())); // an empty program refuses
        hub.attachProject(nullptr);              // legal: QPointer unattach
    }

    void dapHubBreakpointsVerifyMoveResendAndClear()
    {
        ProjectSession project;
        CodeEditor ed;
        const QString path = QStringLiteral("/dapfake/project/prog.cpp");
        QVERIFY(project.openDocument(&ed, path));
        ed.setPlainText(QStringLiteral("line one\nline two\nline three\n"
                                       "line four\nline five\nline six\n"
                                       "line seven\nline eight\nline nine\n"
                                       "line ten\nline eleven\nline twelve\n"));

        HubSpawns spawns; // BEFORE the hub (the dtor-pump lifetime rule)
        DapHub hub;
        spawns.install(hub);
        hub.attachProject(&project);

        QSignalSpy startedSpy(&hub, &DapHub::sessionStarted);
        QSignalSpy endedSpy(&hub, &DapHub::sessionEnded);
        QStringList relayedNames;
        QObject::connect(&hub, &DapHub::dapEvent,
                         [&relayedNames](const QString& name, const QJsonValue&) {
                             relayedNames.append(name);
                         });

        auto dapDiag = [&ed](int line, const QString& needle) {
            for (const CodeEditor::Diagnostic& d : ed.diagnostics())
                if (d.startLine == line && d.source == QLatin1String("dap")
                    && d.severity == CodeEditor::Severity::Warning
                    && d.message.contains(needle))
                    return true;
            return false;
        };

        // The store-BEFORE-ready half of the door: setBreakpoints lands its
        // truth now, the wire push rides the ready() re-push:
        QVERIFY(hub.setBreakpoints(&ed, {2, 4, 8})); // wire lines 3, 5, 9
        QCOMPARE(hub.requestedBreakpoints(&ed), (QVector<int>{2, 4, 8}));
        QVERIFY(ed.breakpointLines().isEmpty()); // nothing painted pre-ready

        QVERIFY(hub.startDebugging(QString::fromLatin1(QMN_TEST_DAP_SERVER),
                                   {QStringLiteral("breakpoints"), path},
                                   QJsonObject{{QStringLiteral("program"),
                                               QStringLiteral("/bin/true")}},
                                   QStringLiteral("dapfake")));
        QVERIFY(hub.isDebugging());
        // ONE session at a time (the PLAN shape): a second start refuses —
        // dapfake never sees a double handshake (its seq audit would exit 2):
        QVERIFY(!hub.startDebugging(QString::fromLatin1(QMN_TEST_DAP_SERVER),
                                    {QStringLiteral("breakpoints"), path},
                                    QJsonObject{}, QStringLiteral("dapfake")));
        QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 30000);
        QVERIFY(hub.isReady());

        // The capabilities PROXY (dapfake says configurationDone is NOT
        // supported, explicitly): the door must stay SILENT — a stray
        // configurationDone exits 2 far-side, so the exit-0 below pins it.
        QVERIFY(hub.adapterCapabilities()
                    .contains(QStringLiteral("supportsConfigurationDoneRequest")));
        QVERIFY(!hub.adapterCapabilities()
                    .value(QStringLiteral("supportsConfigurationDoneRequest")).toBool());
        hub.configurationDone();

        QTRY_VERIFY_WITH_TIMEOUT(relayedNames.contains(QStringLiteral("breakpoint")), 5000);

        // The re-push landed on Ready AND the id-keyed `breakpoint` EVENT
        // (dapfake pulls the first call's FIRST id = wire 3 = model 2) lands
        // in the same frame batch — the "all verified" intermediate NEVER
        // settles, so the slot pins the MERGED post-event truth: wire 9
        // VERIFIED-MOVED to 11 (dot on model 10, DESIRED 8 in the map),
        // wire 5 unverified ("cannot stop"), model 2's dot PULLED AWAY by
        // the event (second Warning), the DESIRED store untouched:
        QTRY_VERIFY_WITH_TIMEOUT(dapDiag(4, QStringLiteral("cannot stop on line 5"))
                                     && dapDiag(2, QStringLiteral("pulled the dot")), 5000);
        QCOMPARE(ed.breakpointLines(), (QVector<int>{10}));
        QCOMPARE(ed.diagnostics().size(), 2); // only the two unverified rows
        const QMap<int, int> moved = hub.breakpointMap(&ed);
        QCOMPARE(moved.value(8), 10); // the desired->ACTUAL map
        QCOMPARE(moved.size(), 1);    // unverified (5) AND pulled (2) stay OUT
        QCOMPARE(hub.requestedBreakpoints(&ed), (QVector<int>{2, 4, 8}));

        // The LINE-ANCHORED resend: editing the bound document re-pushes
        // through the kBreakpointResendMs debounce; dapfake's pass-2 message
        // is the far-side proof the SECOND setBreakpoints really arrived
        // (and dapfake exits 2 if the disconnect ever outran it):
        ed.setPlainText(ed.toPlainText() + QStringLiteral("// touched\n"));
        QTRY_VERIFY_WITH_TIMEOUT(
            dapDiag(4, QStringLiteral("pass 2"))
                && ed.breakpointLines() == (QVector<int>{2, 10}), 5000);

        // The legal CLEAR: an empty array answers an empty array — every
        // dap decoration goes, the store empties, the DAP ids die:
        QVERIFY(hub.setBreakpoints(&ed, {}));
        QTRY_VERIFY_WITH_TIMEOUT(ed.breakpointLines().isEmpty()
                                     && ed.diagnostics().isEmpty(), 5000);
        QVERIFY(hub.requestedBreakpoints(&ed).isEmpty());
        QVERIFY(hub.breakpointMap(&ed).isEmpty());

        // The stop door rides the disconnect barrier; the clean-stop edge
        // (DapClient::stopped) is what carries sessionEnded("stopped"):
        hub.stopDebugging();
        QTRY_COMPARE_WITH_TIMEOUT(endedSpy.count(), 1, 5000);
        QCOMPARE(endedSpy.at(0).at(0).toString(), QStringLiteral("stopped"));
        QVERIFY(!hub.isDebugging());
        QTRY_COMPARE_WITH_TIMEOUT(spawns.exitCodeOf(0), 0, 5000); // dapfake's
        // contract: handshake order + the launch audit + >=2 setBreakpoints
        // calls + no stray configurationDone — ALL paid with this exit 0.
    }

    void dapHubStoppedFeedPaintsStackAndValueHints()
    {
        ProjectSession project;
        CodeEditor ed;
        const QString path = QStringLiteral("/dapfake/project/main.cpp");
        QVERIFY(project.openDocument(&ed, path));
        ed.setPlainText(QStringLiteral("top line\n"
                                       "    int y = 8\n"
                                       "    return x + y\n"
                                       "bottom line\n"));

        HubSpawns spawns;
        DapHub hub;
        spawns.install(hub);
        hub.attachProject(&project);
        QSignalSpy startedSpy(&hub, &DapHub::sessionStarted);
        QSignalSpy endedSpy(&hub, &DapHub::sessionEnded);
        QSignalSpy stoppedSpy(&hub, &DapHub::debugStopped);
        QSignalSpy continuedSpy(&hub, &DapHub::debugContinued);
        QSignalSpy unresolvedSpy(&hub, &DapHub::sourceUnresolved);

        // The frame arrives at 1-based line 3 / column 6 (argv-pinned):
        QVERIFY(hub.startDebugging(QString::fromLatin1(QMN_TEST_DAP_SERVER),
                                   {QStringLiteral("steps"), path,
                                    QStringLiteral("3"), QStringLiteral("6")},
                                   QJsonObject{{QStringLiteral("name"),
                                               QStringLiteral("tst")}},
                                   QStringLiteral("dapfake")));
        QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 30000);
        QVERIFY(hub.adapterCapabilities()
                    .value(QStringLiteral("supportsTerminateRequest")).toBool());

        // The EXPLICIT configurationDone (caps say true here): dapfake
        // answers and fires the `stopped` event (threadId 7).
        hub.configurationDone();
        QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 5000);
        QCOMPARE(stoppedSpy.at(0).at(0).toInt(), 7); // the v1 single-thread leg
        QCOMPARE(stoppedSpy.at(0).at(1).toString(), QStringLiteral("breakpoint"));

        // The stack feed on the RESOLVED editor (the ONLY frame-resolution
        // route is the G2 uri map — no disk was touched, the path is a
        // virtual /dapfake/... path nothing can open):
        QTRY_COMPARE_WITH_TIMEOUT(ed.highlightedLinesStart(), 2, 5000); // the band
        QCOMPARE(ed.highlightedLinesEnd(), 2);
        QCOMPARE(ed.position().line, 2); // frame (3, 6) 1-based -> model (2, 5)
        QCOMPARE(ed.position().col, 5);
        QCOMPARE(unresolvedSpy.count(), 0);

        // The value hints: the FIRST top scope only (scope 22 is POISONED —
        // dapfake exits 2 if it is ever asked, so a wrong-scope feed dies
        // at the exit code, never as a printed "= WRONG"), first-occurrence
        // per name (the duplicate "x"/WRONG-DUP never paints), no hint for
        // names off the line (zzUnused), anchored at the word-END column:
        QTRY_COMPARE_WITH_TIMEOUT(ed.inlayHints().size(), 2, 5000);
        const QVector<CodeEditor::InlayHint> hints = ed.inlayHints();
        QCOMPARE(hints.at(0).line, 2);
        QCOMPARE(hints.at(0).col, 12); // one past "x"
        QCOMPARE(hints.at(0).text, QStringLiteral(" = 42"));
        QCOMPARE(hints.at(1).col, 16); // one past "y"
        QCOMPARE(hints.at(1).text, QStringLiteral(" = 13"));

        // `continued` (fired through the hub's request door as a raw DAP
        // command) clears the band AND the hints:
        QVERIFY(hub.request(QStringLiteral("continue"),
                             QJsonValue(QJsonValue::Undefined)) >= 1);
        QTRY_COMPARE_WITH_TIMEOUT(continuedSpy.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(ed.highlightedLinesStart(), -1, 5000);
        QVERIFY(ed.inlayHints().isEmpty()); // cleared in the SAME call chain

        // terminateDebugging: the capability is TRUE here, so the terminate
        // REQUEST leads and the barrier follows (dapfake exits 2 if the
        // disconnect ever outran terminate):
        QVERIFY(hub.terminateDebugging());
        QTRY_COMPARE_WITH_TIMEOUT(endedSpy.count(), 1, 5000);
        QCOMPARE(endedSpy.at(0).at(0).toString(), QStringLiteral("stopped"));
        QVERIFY(!hub.isDebugging());
        QTRY_COMPARE_WITH_TIMEOUT(spawns.exitCodeOf(0), 0, 5000);
    }

    void dapHubUnresolvedFrameRelaysSourceAndFrame()
    {
        ProjectSession project;
        CodeEditor ed;
        QVERIFY(project.openDocument(&ed, QStringLiteral("/dapfake/project/other.cpp")));
        ed.setPlainText(QStringLiteral("x = 1\n"));

        HubSpawns spawns;
        DapHub hub;
        spawns.install(hub);
        hub.attachProject(&project);
        QSignalSpy startedSpy(&hub, &DapHub::sessionStarted);
        QSignalSpy endedSpy(&hub, &DapHub::sessionEnded);
        QSignalSpy unresolvedSpy(&hub, &DapHub::sourceUnresolved);

        // steps mode with the BOGUS frame path: the session's open
        // documents do NOT cover it — and the hub NEVER reads disk, NEVER
        // opens anything of its own:
        QVERIFY(hub.startDebugging(QString::fromLatin1(QMN_TEST_DAP_SERVER),
                                   {QStringLiteral("steps"), QStringLiteral("BOGUS"),
                                    QStringLiteral("1"), QStringLiteral("1")},
                                   QJsonObject{}, QStringLiteral("dapfake")));
        QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 30000);
        hub.configurationDone();

        QTRY_COMPARE_WITH_TIMEOUT(unresolvedSpy.count(), 1, 5000);
        const QJsonObject source = unresolvedSpy.at(0).at(0).toJsonObject();
        const QJsonObject frame = unresolvedSpy.at(0).at(1).toJsonObject();
        QVERIFY(source.value(QStringLiteral("path")).toString()
                    .contains(QStringLiteral("definitely-not-open")));
        QCOMPARE(frame.value(QStringLiteral("name")).toString(),
                 QStringLiteral("dapfake-top"));
        // Nothing was painted anywhere (the app's call to open an editor;
        // the NEXT feed would resolve it — no silent I/O):
        QCOMPARE(ed.highlightedLinesStart(), -1);
        QVERIFY(ed.inlayHints().isEmpty());
        QCOMPARE(ed.position().line, 0);

        QVERIFY(hub.terminateDebugging());
        QTRY_COMPARE_WITH_TIMEOUT(endedSpy.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(spawns.exitCodeOf(0), 0, 5000);
    }

    void dapHubTerminateWithoutCapabilityFallsBackToStop()
    {
        // basic mode's caps carry NO supportsTerminateRequest: the door
        // answers the honest false AND still takes the plain barrier
        // (dapfake answers the disconnect and exits 0 either way):
        HubSpawns spawns;
        DapHub hub;
        spawns.install(hub);
        QSignalSpy startedSpy(&hub, &DapHub::sessionStarted);
        QSignalSpy endedSpy(&hub, &DapHub::sessionEnded);

        QVERIFY(hub.startDebugging(QString::fromLatin1(QMN_TEST_DAP_SERVER),
                                   {QStringLiteral("basic")}, QJsonObject{},
                                   QStringLiteral("dapfake")));
        QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 30000);
        QVERIFY(!hub.adapterCapabilities()
                    .contains(QStringLiteral("supportsTerminateRequest")));

        QVERIFY(!hub.terminateDebugging()); // the honest "no terminate here"...
        QTRY_COMPARE_WITH_TIMEOUT(endedSpy.count(), 1, 5000); // ...and the
        QCOMPARE(endedSpy.at(0).at(0).toString(), QStringLiteral("stopped")); // barrier rode anyway
        QVERIFY(!hub.isDebugging());
        QCOMPARE(hub.request(QStringLiteral("ping"),
                             QJsonValue(QJsonValue::Undefined)), qint64(-1));
        QTRY_COMPARE_WITH_TIMEOUT(spawns.exitCodeOf(0), 0, 5000);
    }

    void dapHubAdapterCrashClearsFeedsAndRelaysSessionEnded()
    {
        ProjectSession project;
        CodeEditor ed;
        QVERIFY(project.openDocument(&ed, QStringLiteral("/dapfake/project/dead.cpp")));
        ed.setPlainText(QStringLiteral("alpha\nbeta\n"));

        HubSpawns spawns;
        DapHub hub;
        spawns.install(hub);
        hub.attachProject(&project);
        QSignalSpy endedSpy(&hub, &DapHub::sessionEnded);

        // A stored breakpoint (the ready() re-push would race the crash —
        // whichever order the frames land in, the sessionGen_ guard makes
        // the feed's final state deterministic once sessionEnded fires):
        QVERIFY(hub.setBreakpoints(&ed, {1}));
        QVERIFY(hub.startDebugging(QString::fromLatin1(QMN_TEST_DAP_SERVER),
                                   {QStringLiteral("dead")}, QJsonObject{},
                                   QStringLiteral("dapfake")));

        // dead mode: dapfake _exit(7)s right after the handshake. The
        // client's failed() merges into the hub's sessionEnded, feeds all
        // cleared, and the DESIRED breakpoint truth survives the death —
        // the next session's ready() re-pushes it (the recorded shape):
        QTRY_COMPARE_WITH_TIMEOUT(endedSpy.count(), 1, 30000);
        QVERIFY(!endedSpy.at(0).at(0).toString().isEmpty());
        QVERIFY(!hub.isDebugging());
        QVERIFY(!hub.isReady());
        QVERIFY(ed.breakpointLines().isEmpty());
        QVERIFY(ed.diagnostics().isEmpty()); // the feed EMPTIES on death — it
        // never repaints as warnings: a dead session gets to say nothing
        QCOMPARE(hub.requestedBreakpoints(&ed), (QVector<int>{1}));
        QCOMPARE(hub.request(QStringLiteral("next"),
                             QJsonValue(QJsonValue::Undefined)), qint64(-1));
        QTRY_COMPARE_WITH_TIMEOUT(spawns.exitCodeOf(0), 7, 5000);
    }
};

QTEST_MAIN(TstDap) // machine D3 dragged real CodeEditors in, exactly as
                   // tst_lsp's machine 4 predicted (ctest runs this entry
                   // with QT_QPA_PLATFORM=offscreen)
#include "tst_dap.moc"
