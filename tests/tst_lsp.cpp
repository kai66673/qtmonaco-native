// The 5th ctest entry (M8 machine 1): JsonFraming + the three transports +
// RpcClient, headless/guiless — the PLAN.md M8 layering contract made
// literal ("the protocol layers NEVER know CodeEditor/CodeDocument"). The
// in-process QTcpServer fake peer plays the server side for RpcClient;
// the smart text-model fakeserver (machines 2/3) joins this same binary.

#include <QtTest>

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVector>

#include <functional>
#include <memory>

#include "lsp/DocumentSyncer.hpp"
#include "lsp/LspClient.hpp"
#include "lsp/PositionConverter.hpp"
#include "protocol/JsonFraming.hpp"
#include "protocol/ProcessTransport.hpp"
#include "protocol/RpcClient.hpp"
#include "protocol/TcpTransport.hpp"

#include <csignal>

using namespace qmn::protocol;

namespace {

// Server-side stand-in: captures every framed message the client sends and
// answers through the same framing the client uses.
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
    bool haveClient() const { return client_ != nullptr && !clientGone_; }
    bool clientGone() const { return clientGone_; }

    void send(const QJsonObject& message)
    {
        if (client_ && !clientGone_)
            client_->write(JsonFraming::encode(
                QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }
    void sendRaw(const QByteArray& framed)
    {
        if (client_ && !clientGone_)
            client_->write(framed);
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
    int methodCount(const QString& method) const
    {
        int n = 0;
        for (const QJsonObject& m : messages)
            if (m.value(QStringLiteral("method")).toString() == method)
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

// NOTE the explicit Undefined defaults: Qt 6's QJsonValue() is a JSON
// NULL, not "member omitted" (measured on 6.12), and these helpers use
// isUndefined() to decide whether to write the params member at all.
QJsonObject peerRequest(const QString& method, const QJsonValue& id,
                        const QJsonValue& params
                            = QJsonValue(QJsonValue::Undefined))
{
    QJsonObject m;
    m.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    m.insert(QStringLiteral("method"), method);
    m.insert(QStringLiteral("id"), id);
    if (!params.isUndefined())
        m.insert(QStringLiteral("params"), params);
    return m;
}

QJsonObject peerNotification(const QString& method,
                             const QJsonValue& params
                                 = QJsonValue(QJsonValue::Undefined))
{
    QJsonObject m;
    m.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    m.insert(QStringLiteral("method"), method);
    if (!params.isUndefined())
        m.insert(QStringLiteral("params"), params);
    return m;
}

QJsonObject peerResponse(qint64 id, const QJsonValue& result)
{
    QJsonObject m;
    m.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("result"), result);
    return m;
}

// ------------------------------------------------------------ M2 harness
// LspClient + the fakeserver factory: the factory counts spawns (the
// restart-budget oracle) and records each process's exit code at its
// closed() (the exit code is the fakeserver's wire-side verdict; recording
// it inside the signal is the only safe read — the session teardown deletes
// the transport as soon as its child is reaped).
struct TestHarness {
    struct Spawn {
        ProcessTransport* transport = nullptr; // may die: never dereferenced
        bool opened = false;                   // the pipe was live
        int exitCode = -1;                     // -1 until recorded
    };

    QString program;
    QStringList modeArgs;
    bool crashFirstSessionOnly = false; // see the factory: harness-side
                                        // crashonce for spawn #0 only
    int spawnCount = 0;
    QVector<Spawn> spawned;
    std::unique_ptr<qmn::lsp::LspClient> client;

    explicit TestHarness(const QString& modeArg)
        : program(QString::fromLatin1(QMN_TEST_LSP_SERVER))
        , modeArgs{modeArg}
    {
        client.reset(new qmn::lsp::LspClient(
            [this](QObject* parent) {
                ++spawnCount;
                QStringList args = modeArgs;
                if (crashFirstSessionOnly && !spawned.isEmpty())
                    // "crashonce" is a PER-PROCESS flag in the fakeserver
                    // (globals reset with each spawn), so the one-shot
                    // session semantics live HERE: only spawn #0 carries the
                    // flag; the auto-restart (#1 and beyond) starts clean.
                    args.removeAll(QStringLiteral("crashonce"));
                auto* transport = new ProcessTransport(program, args, parent);
                return attachTransport(transport);
            }));
    }

    Transport* attachTransport(ProcessTransport* transport)
    {
        const int index = spawned.size();
        spawned.append(Spawn{transport, false, -1});
        QObject::connect(transport, &Transport::opened, transport,
                         [this, index] { spawned[index].opened = true; });
        QObject::connect(transport, &Transport::closed, transport,
                         [this, index] {
                             // Fires from ProcessTransport::onFinished:
                             // the exit code is defined right here.
                             if (spawned[index].opened)
                                 spawned[index].exitCode
                                     = spawned[index].transport->exitCode();
                         });
        return static_cast<Transport*>(transport);
    }

    // The LSP initialize params every real client must carry (the fakeserver
    // exits 2 if "capabilities" is missing: the handshake shape is pinned).
    static QJsonObject initParams()
    {
        return QJsonObject{
            {QStringLiteral("processId"), double(QCoreApplication::applicationPid())},
            {QStringLiteral("clientInfo"),
             QJsonObject{{QStringLiteral("name"), QStringLiteral("qmn-tst")}}},
            {QStringLiteral("locale"), QStringLiteral("en")},
            {QStringLiteral("rootUri"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("capabilities"), QJsonObject{}},
            {QStringLiteral("workspaceFolders"),
             QJsonValue(QJsonValue::Undefined)}};
    }

    // -1 while the process has not been reaped yet (QTRY against 0/7/...).
    int exitCodeOf(int spawnIndex) const { return spawned.at(spawnIndex).exitCode; }
};

// The battery's server-side verdict marker (stderr channel). A test that
// stops a "requests"-mode server must see it FIRST: stopping earlier puts
// stdin's EOF in front of the battery's blocking reads, and the fakeserver
// honestly fails that hang (exit 2) instead of a clean exit 0.
bool batteryPassed(const QSignalSpy& stderrSpy)
{
    for (const QList<QVariant>& args : stderrSpy)
        if (args.at(0).toString() == QLatin1String("BATTERY-PASS"))
            return true;
    return false;
}

// The machine-3 verdict door: asks the sync-mode fakeserver to compare its
// INDEPENDENT text model against the expected text (the test's own naive
// QString oracle) and version. A mismatch comes back as an honest ok:false
// (server model included in the reply for the failure dump), never an exit 2
// — a shared decoder would cancel client and server bugs into a green test.
QJsonObject assertSyncModel(qmn::lsp::LspClient* client,
                            const QString& expectedText, qint64 expectedVersion)
{
    bool answered = false;
    QJsonObject result;
    client->request(QStringLiteral("test/assertModel"),
                    QJsonObject{{QStringLiteral("expectedText"), expectedText},
                                {QStringLiteral("expectedVersion"),
                                 double(expectedVersion)}},
                    [&answered, &result](const RpcClient::Reply& reply) {
                        answered = true;
                        if (!reply.isError)
                            result = reply.result.toObject();
                    });
    // Explicit pump, not a QTRY macro: QTRY's failure path RETURNS, which a
    // value-returning helper cannot do. No answer means the fakeserver died
    // on a structural lie (exit 2) — surfaced as an honest ok:false verdict
    // with the reason, so the QVERIFY2 dump below shows it.
    for (int waited = 0; waited < 5000 && !answered; waited += 50)
        QTest::qWait(50); // the request rides the SAME stream as the
                          // didChange whose effect it checks: ordering IS the
                          // synchronization, no extra waits
    if (!answered) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("note"),
                            QStringLiteral("test/assertModel never answered "
                                           "(structural fakeserver fail: look "
                                           "for FAKESERVER-FAIL on stderr)")}};
    }
    return result;
}

QString modelDump(const QJsonObject& verdict)
{
    // Only ever formatted on a fail: the server-side model text inside the
    // verdict is the diagnostic that makes a desync debuggable straight from
    // the ctest log.
    return QStringLiteral("server model verdict: %1")
        .arg(QString::fromUtf8(
            QJsonDocument(verdict).toJson(QJsonDocument::Compact).constData()));
}

} // namespace

class TstLsp : public QObject {
    Q_OBJECT
private:
    // The slot has already listen()ed (the port feeds the transport ctor);
    // this starts the client side and waits for the connection.
    void openSession(FakePeer& peer, TcpTransport& transport, RpcClient& client)
    {
        transport.start();
        QTRY_VERIFY_WITH_TIMEOUT(peer.haveClient(), 3000);
        QVERIFY(!client.isGone());
    }

private slots:
    // ------------------------------------------------------------- framing
    void encodeExactWireForm()
    {
        const QByteArray framed = JsonFraming::encode("{\"a\":1}");
        QCOMPARE(framed, QByteArray("Content-Length: 7\r\n\r\n{\"a\":1}"));
        QCOMPARE(JsonFraming::kMaxFrameBytes, qint64(64) * 1024 * 1024);
    }

    void decodeOneByteAtATime()
    {
        const QByteArray wire
            = JsonFraming::encode("{\"m\":1}") + JsonFraming::encode("{}");
        JsonFraming framing;
        QVector<QByteArray> payloads;
        for (int i = 0; i < wire.size(); ++i) {
            framing.feed(wire.mid(i, 1));
            QByteArray payload;
            while (framing.poll(&payload))
                payloads.append(payload);
            QVERIFY2(!framing.hasError(), qPrintable(framing.error().text));
        }
        QCOMPARE(payloads.size(), 2);
        QCOMPARE(payloads.at(0), QByteArray("{\"m\":1}"));
        QCOMPARE(payloads.at(1), QByteArray("{}"));
    }

    void decodeMultipleMessagesInOneFeed()
    {
        JsonFraming framing;
        framing.feed(JsonFraming::encode("aaa") + JsonFraming::encode("bb")
                     + JsonFraming::encode("c"));
        QByteArray payload;
        QVector<QByteArray> payloads;
        while (framing.poll(&payload))
            payloads.append(payload);
        QCOMPARE(payloads, (QVector<QByteArray>{"aaa", "bb", "c"}));
    }

    void decodeLenientHeaderForms()
    {
        struct Case { const char* wire; const char* payload; };
        const Case cases[] = {
            {"content-length: 4\r\n\r\nabcd", "abcd"},          // case-insensitive
            {"Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n"
             "Content-Length: 4\r\n\r\nabcd", "abcd"},          // content-type ignored
            {"X-Future-Thing: whatever\r\nContent-Length: 2\r\n\r\nhi", "hi"},
            {"content-length:3\n\nfoo", "foo"},                 // LF-only terminator
            {"Content-Length: 3\r\n\nbar", "bar"},              // mixed terminator
        };
        for (const Case& c : cases) {
            JsonFraming framing;
            framing.feed(c.wire);
            QByteArray payload;
            QVERIFY2(framing.poll(&payload),
                     qPrintable(QStringLiteral("case [%1] errored: %2")
                                    .arg(c.wire, framing.error().text)));
            QCOMPARE(payload, QByteArray(c.payload));
            QVERIFY(!framing.hasError());
        }
    }

    void headerBlockFailures()
    {
        struct Case { const char* wire; JsonFraming::ErrorKind kind; };
        const QByteArray flood = QByteArray("X: y\r\n").repeated(12000); // > 64 KiB
        const Case cases[] = {
            {"X-Foo: 1\r\n\r\nbody", JsonFraming::ErrorKind::HeaderMalformed},   // no Content-Length
            {"junkline\r\nContent-Length: 1\r\n\r\nx", JsonFraming::ErrorKind::HeaderMalformed},
            {"Content-Length: 1x\r\n\r\nx", JsonFraming::ErrorKind::HeaderMalformed},
            {"Content-Length: 1\r\nContent-Length: 2\r\n\r\nxy",
             JsonFraming::ErrorKind::HeaderMalformed},                            // duplicate
            {flood.constData(), JsonFraming::ErrorKind::HeaderTooLarge},          // no blank line
        };
        for (const Case& c : cases) {
            JsonFraming framing;
            framing.feed(c.wire);
            QByteArray payload;
            QVERIFY2(!framing.poll(&payload),
                     qPrintable(QStringLiteral("case [%1] must not yield a payload")
                                    .arg(QString::fromUtf8(c.wire).left(40))));
            QCOMPARE(framing.error().kind, c.kind);
        }
    }

    void oversizeContentLengthFailsBeforeAllocation()
    {
        // PLAN-pinned: 64 MiB+1 fails as a transport error, never as an
        // allocation: the declared body is never read into the buffer.
        JsonFraming framing;
        framing.feed("Content-Length: 67108865\r\n\r\n"); // kMaxFrameBytes + 1
        QByteArray payload;
        QVERIFY(!framing.poll(&payload));
        QCOMPARE(framing.error().kind, JsonFraming::ErrorKind::ContentLengthTooLarge);
        framing.feed(QByteArray(4096, 'x')); // junk keeps arriving: dropped, dead
        QVERIFY(!framing.poll(&payload));
        QCOMPARE(framing.error().kind, JsonFraming::ErrorKind::ContentLengthTooLarge);
    }

    void deadDecoderStaysDeadUntilReset()
    {
        JsonFraming framing;
        framing.feed("no colon header\r\n\r\n");
        QByteArray payload;
        // The error lives on the PULL path: feed only buffers, the header
        // block is parsed (and fails) when poll() runs.
        QVERIFY(!framing.poll(&payload));
        QVERIFY(framing.hasError());
        framing.feed(JsonFraming::encode("ok")); // valid data while dead: ignored
        QVERIFY(!framing.poll(&payload));
        framing.reset();
        framing.feed(JsonFraming::encode("ok")); // and only now decodes again
        QVERIFY(framing.poll(&payload));
        QCOMPARE(payload, QByteArray("ok"));
    }
    // ----------------------------------------------------------- RpcClient
    void rpcRoundTripAndIdAllocation()
    {
        QCOMPARE(RpcClient::kInitializeTimeoutMs, 10'000); // PLAN-pinned pair
        QCOMPARE(RpcClient::kFeatureTimeoutMs, 5'000);     // (never a blanket)

        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        RpcClient client(&transport);
        openSession(peer, transport, client);

        QVector<RpcClient::Reply> replies;
        const qint64 id1 = client.request(
            QStringLiteral("alpha"), QJsonObject{{QStringLiteral("x"), 1}},
            [&](const RpcClient::Reply& r) { replies.append(r); });
        QCOMPARE(id1, qint64(1)); // ids are ours, monotonic from 1
        QTRY_COMPARE_WITH_TIMEOUT(peer.messages.size(), 1, 3000);
        const QJsonObject& sent = peer.messages.at(0);
        QCOMPARE(sent.value(QStringLiteral("jsonrpc")).toString(),
                 QStringLiteral("2.0"));
        QCOMPARE(sent.value(QStringLiteral("method")).toString(),
                 QStringLiteral("alpha"));
        QCOMPARE(sent.value(QStringLiteral("id")).toInt(), 1);
        QCOMPARE(sent.value(QStringLiteral("params")).toObject()
                     .value(QStringLiteral("x")).toInt(), 1);

        peer.send(peerResponse(1, QJsonObject{{QStringLiteral("ok"), true}}));
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QVERIFY(!replies.at(0).isError);
        QCOMPARE(replies.at(0).result.toObject().value(QStringLiteral("ok")).toBool(),
                 true);

        const qint64 id2 = client.request(QStringLiteral("beta"), QJsonValue(),
                                          [](const RpcClient::Reply&) {});
        QCOMPARE(id2, qint64(2));
    }

    void rpcErrorReplySurfaces()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        RpcClient client(&transport);
        openSession(peer, transport, client);

        QVector<RpcClient::Reply> replies;
        client.request(QStringLiteral("gamma"), QJsonValue(),
                       [&](const RpcClient::Reply& r) { replies.append(r); });
        QTRY_COMPARE_WITH_TIMEOUT(peer.messages.size(), 1, 3000);

        QJsonObject errorObject;
        errorObject.insert(QStringLiteral("code"), -32001); // a wire server code
        errorObject.insert(QStringLiteral("message"), QStringLiteral("oops"));
        errorObject.insert(QStringLiteral("data"), QStringLiteral("detail"));
        QJsonObject response;
        response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
        response.insert(QStringLiteral("id"), 1);
        response.insert(QStringLiteral("error"), errorObject);
        peer.send(response);

        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QVERIFY(replies.at(0).isError);
        QCOMPARE(replies.at(0).code, -32001);
        QCOMPARE(replies.at(0).message, QStringLiteral("oops"));
        QCOMPARE(replies.at(0).data.toString(), QStringLiteral("detail"));
    }

    void timeoutSendsCancelAndFulfillsOnce()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        RpcClient client(&transport);
        openSession(peer, transport, client);

        QVector<RpcClient::Reply> replies;
        const qint64 id = client.request(QStringLiteral("slow"), QJsonValue(),
                                         [&](const RpcClient::Reply& r) {
                                             replies.append(r);
                                         },
                                         /*per-door budget*/ 120);
        QCOMPARE(id, qint64(1));
        QCOMPARE(client.pendingCount(), 1);

        // PLAN: a timeout CANCELS server-side work, it does not merely walk
        // away (the iide anti-pattern "no $/cancelRequest").
        QTRY_VERIFY_WITH_TIMEOUT(
            peer.indexOf([&](const QJsonObject& m) {
                return m.value(QStringLiteral("method")).toString()
                           == QLatin1String("$/cancelRequest")
                       && m.value(QStringLiteral("params")).toObject()
                              .value(QStringLiteral("id")).toInt() == 1;
            }) >= 0,
            3000);
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QCOMPARE(replies.at(0).code, RpcClient::kErrTimeout);
        QVERIFY(replies.at(0).message.contains(QStringLiteral("slow")));
        QCOMPARE(client.pendingCount(), 0);

        // A late server answer must never re-fire the fulfilled handler.
        peer.send(peerResponse(1, QStringLiteral("late")));
        QTest::qWait(200);
        QCOMPARE(replies.size(), 1);
    }

    void cancelFulfillsOnceAndIgnoresLate()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        RpcClient client(&transport);
        openSession(peer, transport, client);

        QVector<RpcClient::Reply> replies;
        const qint64 id = client.request(QStringLiteral("work"), QJsonValue(),
                                         [&](const RpcClient::Reply& r) {
                                             replies.append(r);
                                         });
        QTRY_COMPARE_WITH_TIMEOUT(peer.methodCount(QStringLiteral("work")), 1, 3000);

        client.cancel(id);
        QTRY_VERIFY_WITH_TIMEOUT(
            peer.indexOf([&](const QJsonObject& m) {
                return m.value(QStringLiteral("method")).toString()
                       == QLatin1String("$/cancelRequest");
            }) >= 0,
            3000);
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        // the REAL LSP code, not an invented one (iide anti-pattern):
        QCOMPARE(replies.at(0).code, RpcClient::kWireRequestCancelled);

        client.cancel(id); // already fulfilled: no second $/cancelRequest
        QTest::qWait(150);
        QCOMPARE(peer.methodCount(QStringLiteral("$/cancelRequest")), 1);

        peer.send(peerResponse(id, QStringLiteral("too late")));
        QTest::qWait(150);
        QCOMPARE(replies.size(), 1);
        QCOMPARE(client.pendingCount(), 0);
    }

    void unregisteredRequestAnswered32601UnknownNotificationIgnored()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        RpcClient client(&transport);
        openSession(peer, transport, client);

        // "Always answer, never lie": a request nobody registered for is
        // answered mechanically. The id space is per-direction: peer-side id
        // 42 coexists with our own allocations without touching pending_.
        peer.send(peerRequest(QStringLiteral("mystery"), 42));
        QTRY_VERIFY_WITH_TIMEOUT(
            peer.indexOf([&](const QJsonObject& m) {
                return m.contains(QStringLiteral("error"))
                       && m.value(QStringLiteral("id")).toInt() == 42;
            }) >= 0,
            3000);
        const int responseIndex = peer.indexOf([&](const QJsonObject& m) {
            return m.contains(QStringLiteral("error"))
                   && m.value(QStringLiteral("id")).toInt() == 42;
        });
        QCOMPARE(peer.messages.at(responseIndex)
                     .value(QStringLiteral("error")).toObject()
                     .value(QStringLiteral("code")).toInt(),
                     RpcClient::kWireMethodNotFound);

        // Unknown NOTIFICATIONS are ignored, per spec, and answered by nothing.
        const int sizeAfterResponse = peer.messages.size();
        peer.send(peerNotification(QStringLiteral("nobody-cares")));
        QTest::qWait(200);
        QCOMPARE(peer.messages.size(), sizeAfterResponse);
    }

    void registeredHandlerAnswersExactlyOnceDoubleAnswerRefused()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        RpcClient client(&transport);
        openSession(peer, transport, client);

        bool first = false;
        bool second = false;
        // EXPLICIT Undefined: Qt 6's default QJsonValue() is a JSON null,
        // and the QTRY below would pass vacuously on the initial value.
        QJsonValue notificationParams(QJsonValue::Undefined);
        client.setRequestHandler(QStringLiteral("ping"),
                                 [&](const QJsonObject& request) {
                                     const QJsonValue id
                                         = request.value(QStringLiteral("id"));
                                     first = client.respond(id, QStringLiteral("pong"));
                                     second = client.respond(id, QStringLiteral("pong2"));
                                 });
        client.setNotificationHandler(QStringLiteral("tick"),
                                      [&](const QJsonValue& params) {
                                          notificationParams = params;
                                      });

        // A STRING id from the server side must round-trip unchanged.
        peer.send(peerRequest(QStringLiteral("ping"),
                              QStringLiteral("str-7"),
                              QJsonObject{{QStringLiteral("n"), 5}}));
        QTRY_VERIFY_WITH_TIMEOUT(
            peer.indexOf([&](const QJsonObject& m) {
                return m.value(QStringLiteral("id")).toString()
                           == QLatin1String("str-7");
            }) >= 0,
            3000);
        QVERIFY(first);
        QVERIFY(!second); // the openIncoming_ guard: never answer twice
        const int responses = peer.indexOf([](const QJsonObject& m) {
            return m.value(QStringLiteral("id")).toString() == QLatin1String("str-7")
                   && m.contains(QStringLiteral("result"));
        });
        QVERIFY(responses >= 0);
        QCOMPARE(peer.messages.at(responses).value(QStringLiteral("result")).toString(),
                 QStringLiteral("pong"));

        peer.send(peerNotification(QStringLiteral("tick"),
                                   QJsonObject{{QStringLiteral("n"), 1}}));
        QTRY_VERIFY_WITH_TIMEOUT(!notificationParams.isUndefined(), 3000);
        QCOMPARE(notificationParams.toObject().value(QStringLiteral("n")).toInt(), 1);
    }

    void pendingEvictedOnPeerClose()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        RpcClient client(&transport);
        openSession(peer, transport, client);

        QVector<RpcClient::Reply> replies;
        auto collect = [&](const RpcClient::Reply& r) { replies.append(r); };
        client.request(QStringLiteral("inflight"), QJsonValue(), collect);
        QTRY_COMPARE_WITH_TIMEOUT(peer.methodCount(QStringLiteral("inflight")), 1, 3000);

        QSignalSpy goneSpy(&client, &RpcClient::transportGone);
        peer.abortClient();
        QTRY_COMPARE_WITH_TIMEOUT(goneSpy.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QCOMPARE(replies.at(0).code, RpcClient::kErrTransportClosed);
        QCOMPARE(client.pendingCount(), 0);
        QVERIFY(client.isGone());

        // After a gone transport a new request is honestly rejected: no
        // bytes on the wire, exactly one kErrTransportClosed reply.
        const qint64 rejected
            = client.request(QStringLiteral("post-mortem"), QJsonValue(), collect);
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 2, 3000);
        QCOMPARE(replies.at(1).code, RpcClient::kErrTransportClosed);
        QCOMPARE(rejected, qint64(-1));
        QCOMPARE(peer.methodCount(QStringLiteral("post-mortem")), 0);
    }

    void twoPhaseStopShutdownThenExit()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        RpcClient client(&transport);
        openSession(peer, transport, client);

        client.stop();
        client.stop(); // idempotent: exactly one shutdown on the wire
        QTRY_COMPARE_WITH_TIMEOUT(peer.methodCount(QStringLiteral("shutdown")), 1, 3000);
        const int shutdownIndex
            = peer.indexOf([&](const QJsonObject& m) {
                  return m.value(QStringLiteral("method")).toString()
                         == QLatin1String("shutdown");
              });
        const QJsonObject& shutdownMsg = peer.messages.at(shutdownIndex);
        QVERIFY(shutdownMsg.contains(QStringLiteral("id")));

        peer.send(peerResponse(shutdownMsg.value(QStringLiteral("id")).toInt(),
                               QJsonValue())); // servers answer shutdown with null
        QTRY_VERIFY_WITH_TIMEOUT(
            peer.indexOf([&](const QJsonObject& m) {
                return m.value(QStringLiteral("method")).toString()
                       == QLatin1String("exit");
            }) >= 0,
            3000);
        // The BARRIER order: shutdown strictly before exit...
        QVERIFY(shutdownIndex
                < peer.indexOf([&](const QJsonObject& m) {
                       return m.value(QStringLiteral("method")).toString()
                              == QLatin1String("exit");
                   }));
        // ...and the transport goes down afterwards.
        QTRY_VERIFY_WITH_TIMEOUT(peer.clientGone(), 3000);
        QCOMPARE(client.stopPhase(), RpcClient::StopPhase::Stopped);
    }

    void stopRefusesNewRequestsImmediately()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        RpcClient client(&transport);
        openSession(peer, transport, client);

        client.stop(); // phase 1 begins; we do not answer shutdown yet
        QVector<RpcClient::Reply> replies;
        const qint64 id = client.request(QStringLiteral("late"), QJsonValue(),
                                         [&](const RpcClient::Reply& r) {
                                             replies.append(r);
                                         });
        QCOMPARE(id, qint64(-1));
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QCOMPARE(replies.at(0).code, RpcClient::kErrTransportClosed);
        QCOMPARE(peer.methodCount(QStringLiteral("late")), 0);

        // Answer the pending shutdown late: the barrier completes normally.
        // (WAIT for the bytes first: nothing above spins the loop long
        // enough for the peer's readyRead to have run.)
        QTRY_COMPARE_WITH_TIMEOUT(peer.methodCount(QStringLiteral("shutdown")), 1,
                                  3000);
        const int shutdownIndex
            = peer.indexOf([&](const QJsonObject& m) {
                  return m.value(QStringLiteral("method")).toString()
                         == QLatin1String("shutdown");
              });
        QVERIFY(shutdownIndex >= 0);
        peer.send(peerResponse(peer.messages.at(shutdownIndex)
                                   .value(QStringLiteral("id")).toInt(),
                               QJsonValue()));
        QTRY_VERIFY_WITH_TIMEOUT(peer.clientGone(), 3000);
        QCOMPARE(client.stopPhase(), RpcClient::StopPhase::Stopped);
    }

    // ---------------------------------------------------- error surfacing
    void malformedJsonSurfacedStreamStaysUsable()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        RpcClient client(&transport);
        openSession(peer, transport, client);

        QSignalSpy malformedSpy(&client, &RpcClient::malformedMessage);
        peer.sendRaw(JsonFraming::encode("this is not json {{"));
        QTRY_COMPARE_WITH_TIMEOUT(malformedSpy.count(), 1, 3000);

        // Valid framing survives one broken JSON payload: the transport and
        // the pending machinery keep working.
        QVector<RpcClient::Reply> replies;
        client.request(QStringLiteral("delta"), QJsonValue(),
                       [&](const RpcClient::Reply& r) { replies.append(r); });
        QTRY_COMPARE_WITH_TIMEOUT(peer.methodCount(QStringLiteral("delta")), 1, 3000);
        peer.send(peerResponse(1, 7));
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QCOMPARE(replies.at(0).result.toInt(), 7);

        // A response to an id we never issued is ignored SILENTLY (late
        // answer to a cancelled request is the spec's normal case).
        peer.send(peerResponse(999, QStringLiteral("stray")));
        QTest::qWait(200);
        QCOMPARE(malformedSpy.count(), 1);
        QVERIFY(transport.isRunning());
    }

    void framingPoisonStopsTransportAndEvictsPending()
    {
        FakePeer peer;
        QVERIFY(peer.listen());
        TcpTransport transport(QStringLiteral("127.0.0.1"), peer.port());
        RpcClient client(&transport);
        openSession(peer, transport, client);

        QVector<RpcClient::Reply> replies;
        client.request(QStringLiteral("doomed"), QJsonValue(),
                       [&](const RpcClient::Reply& r) { replies.append(r); });
        QTRY_COMPARE_WITH_TIMEOUT(peer.methodCount(QStringLiteral("doomed")), 1, 3000);

        QSignalSpy protocolSpy(&client, &RpcClient::protocolError);
        QSignalSpy goneSpy(&client, &RpcClient::transportGone);
        peer.sendRaw("Content-Length: 99999999\r\n\r\n"); // > 64 MiB bound
        QTRY_COMPARE_WITH_TIMEOUT(protocolSpy.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(goneSpy.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QCOMPARE(replies.at(0).code, RpcClient::kErrTransportClosed);
        QCOMPARE(client.stopPhase(), RpcClient::StopPhase::Stopped);
        QTRY_VERIFY_WITH_TIMEOUT(peer.clientGone(), 3000); // transport was asked to die
    }

    // -------------------------------------------------- ProcessTransport
    void processTransportEchoRoundTrip()
    {
        // `cat` gives a byte-exact echo: the framed notification we write
        // comes back on stdout and decodes as an incoming notification.
        ProcessTransport transport(QStringLiteral("cat"), QStringList{});
        RpcClient client(&transport);

        QSignalSpy openedSpy(&transport, &Transport::opened);
        QSignalSpy closedSpy(&transport, &Transport::closed);
        QSignalSpy errorSpy(&transport, &Transport::transportError);
        QVERIFY(transport.start());
        QTRY_COMPARE_WITH_TIMEOUT(openedSpy.count(), 1, 3000);

        int echoCount = 0;
        QJsonValue echoParams;
        client.setNotificationHandler(QStringLiteral("echo"),
                                      [&](const QJsonValue& params) {
                                          ++echoCount;
                                          echoParams = params;
                                      });
        client.notify(QStringLiteral("echo"),
                      QJsonObject{{QStringLiteral("m"), 42}});
        QTRY_COMPARE_WITH_TIMEOUT(echoCount, 1, 3000);
        QCOMPARE(echoParams.toObject().value(QStringLiteral("m")).toInt(), 42);

        transport.stop(); // EOF on stdin: cat exits with code 0
        QTRY_COMPARE_WITH_TIMEOUT(closedSpy.count(), 1, 3000);
        QCOMPARE(errorSpy.count(), 0); // a clean stop never cries ServerDied
    }

    void processServerDiedReportedForUnwantedExit()
    {
        ProcessTransport transport(QStringLiteral("sh"),
                                   QStringList{QStringLiteral("-c"),
                                               QStringLiteral("exit 3")});
        QSignalSpy errorSpy(&transport, &Transport::transportError);
        QSignalSpy closedSpy(&transport, &Transport::closed);
        QVERIFY(transport.start());
        QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 3000);
        QCOMPARE(errorSpy.at(0).at(0).value<Transport::ErrorKind>(),
                 Transport::ErrorKind::ServerDied); // the M2 restart hook fires on this
        QTRY_COMPARE_WITH_TIMEOUT(closedSpy.count(), 1, 3000);
    }

    void refusedTcpConnectSurfacesHonestly()
    {
        // Burn a port, release it, then connect to it: refused.
        quint16 deadPort = 0;
        {
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
            deadPort = probe.serverPort();
            probe.close();
        }
        TcpTransport transport(QStringLiteral("127.0.0.1"), deadPort);
        RpcClient client(&transport);
        QSignalSpy errorSpy(&transport, &Transport::transportError);
        QVERIFY(transport.start());
        QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 3000);
        QCOMPARE(errorSpy.at(0).at(0).value<Transport::ErrorKind>(),
                 Transport::ErrorKind::ConnectFailed);

        // The RpcClient turns the dead pipe into an honest, immediate
        // rejection instead of queueing bytes into a void.
        QVector<RpcClient::Reply> replies;
        const qint64 id = client.request(QStringLiteral("never"), QJsonValue(),
                                         [&](const RpcClient::Reply& r) {
                                             replies.append(r);
                                         });
        QCOMPARE(id, qint64(-1));
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QCOMPARE(replies.at(0).code, RpcClient::kErrTransportClosed);
        QCOMPARE(client.pendingCount(), 0);
    }

    // -------------------------------------------------- M2: LspClient +
    // the QProcess fakeserver (the 5th ctest entry grows machine by
    // machine; fakeserver is a support binary, not a ctest entry).

    void lspInitializeHandshakeCaptabilities()
    {
        TestHarness h(QStringLiteral("basic"));
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);
        QSignalSpy failedSpy(h.client.get(), &qmn::lsp::LspClient::failed);
        h.client->start(TestHarness::initParams());
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);
        QCOMPARE(h.client->state(), qmn::lsp::LspClient::State::Ready);
        QCOMPARE(h.spawnCount, 1);
        QCOMPARE(failedSpy.count(), 0);
        // The initialize RESULT facts behind the two fact doors.
        QVERIFY(h.client->serverCapabilities()
                    .value(QStringLiteral("hoverProvider"))
                    .toBool());
        QCOMPARE(h.client->positionEncoding(), QStringLiteral("utf-16"));

        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(),
                                  qmn::lsp::LspClient::State::Stopped, 5000);
    }

    void lspStderrAndProtocolChannelsStaySeparate()
    {
        TestHarness h(QStringLiteral("stderr"));
        QSignalSpy stderrSpy(h.client.get(), &qmn::lsp::LspClient::serverStderr);
        QSignalSpy logSpy(h.client.get(), &qmn::lsp::LspClient::serverLogMessage);
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);
        h.client->start(TestHarness::initParams());
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(stderrSpy.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(logSpy.count(), 1, 3000);

        // The two channels never merge (PLAN M8 per-server logs): STDERR
        // carries the marker, PROTOCOL the logMessage, and neither signal
        // may ever see the other channel's text.
        QCOMPARE(stderrSpy.at(0).at(0).toString(),
                 QStringLiteral("STDERR-MARKER line one"));
        QCOMPARE(logSpy.at(0).at(0).toInt(), 4); // MessageType Log
        QCOMPARE(logSpy.at(0).at(1).toString(), QStringLiteral("PROTOCOL-MARKER"));
        QVERIFY(!logSpy.at(0).at(1).toString().contains(QStringLiteral("STDERR")));
        QVERIFY(!stderrSpy.at(0).at(0).toString()
                    .contains(QStringLiteral("PROTOCOL")));

        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(),
                                  qmn::lsp::LspClient::State::Stopped, 5000);
    }

    void lspNotificationRelaysAndServerRequestAnsweredNull()
    {
        TestHarness h(QStringLiteral("basic"));
        QSignalSpy messageSpy(h.client.get(),
                              &qmn::lsp::LspClient::serverShowMessage);
        QSignalSpy logSpy(h.client.get(),
                          &qmn::lsp::LspClient::serverLogMessage);
        QSignalSpy progressSpy(h.client.get(),
                               &qmn::lsp::LspClient::serverProgress);
        QSignalSpy requestSpy(h.client.get(),
                              &qmn::lsp::LspClient::serverShowMessageRequest);
        QSignalSpy progressCreateSpy(
            h.client.get(), &qmn::lsp::LspClient::createWorkDoneProgress);
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);
        h.client->start(TestHarness::initParams());
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);

        // The notification relays (window/showMessage, logMessage +
        // $/logTrace sharing the log channel at type 4, $/progress).
        QTRY_COMPARE_WITH_TIMEOUT(messageSpy.count(), 2, 3000);
        QCOMPARE(messageSpy.at(0).at(0).toInt(), 3); // MessageType Message
        QCOMPARE(messageSpy.at(0).at(1).toString(), QStringLiteral("basic hello"));
        QCOMPARE(logSpy.count(), 2); // "basic log" + the $/logTrace relay
        QCOMPARE(logSpy.at(1).at(0).toInt(), 4);
        QCOMPARE(logSpy.at(1).at(1).toString(), QStringLiteral("basic trace"));
        QTRY_COMPARE_WITH_TIMEOUT(progressSpy.count(), 1, 3000);
        QCOMPARE(qvariant_cast<QJsonValue>(progressSpy.at(0).at(0)).toString(),
                 QStringLiteral("p-1"));
        QCOMPARE(qvariant_cast<QJsonValue>(progressSpy.at(0).at(1))
                     .toObject()
                     .value(QStringLiteral("value"))
                     .toInt(),
                 7);

        // showMessageRequest: the client's v1 policy answer is null — the
        // fakeserver validates it on the wire (a wrong shape is exit 2).
        QTRY_COMPARE_WITH_TIMEOUT(requestSpy.count(), 1, 3000);
        QCOMPARE(qvariant_cast<QJsonValue>(requestSpy.at(0).at(0)).toString(),
                 QStringLiteral("s-req-1"));
        QCOMPARE(qvariant_cast<QJsonValue>(requestSpy.at(0).at(1))
                     .toObject()
                     .value(QStringLiteral("message"))
                     .toString(),
                 QStringLiteral("basic request"));
        QTRY_COMPARE_WITH_TIMEOUT(progressCreateSpy.count(), 1, 3000);
        QCOMPARE(qvariant_cast<QJsonValue>(progressCreateSpy.at(0).at(0))
                     .toString(),
                 QStringLiteral("w-1"));

        // The unknown server notification stays silently ignored: RpcClient's
        // machine-1 test (unregisteredRequestAnswered32601UnknownNotification
        // Ignored) pins the mechanism; the fakeserver keeps sending past it,
        // so any answered-notification poison would break the exit-pair
        // oracle below (a stopped-early server is a non-zero exit code).
        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(),
                                  qmn::lsp::LspClient::State::Stopped, 5000);
        QCOMPARE(h.spawnCount, 1);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0, 5000); // shutdown/exit
                                                             // reached the
                                                             // wire
    }

    void lspFeatureGateEchoRoundTripAndCleanStop()
    {
        TestHarness h(QStringLiteral("basic"));
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);

        // Pre-Ready doors: the honest rejection (id -1 + exactly one local
        // kErrNotRunning reply; notify silently drops — LSP forbids
        // pre-init notifications and the M3 syncer owns the queueing).
        QVector<RpcClient::Reply> earlyReplies;
        QCOMPARE(h.client->request(QStringLiteral("test/echo"),
                                   QJsonObject{{QStringLiteral("q"), 1}},
                                   [&](const RpcClient::Reply& r) {
                                       earlyReplies.append(r);
                                   }),
                 qint64(-1));
        h.client->notify(QStringLiteral("test/pre-notify"));
        QCOMPARE(earlyReplies.size(), 0);
        QCOMPARE(h.client->state(), qmn::lsp::LspClient::State::NotStarted);

        h.client->start(TestHarness::initParams());
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(earlyReplies.size(), 1, 3000);
        QVERIFY(earlyReplies.at(0).isError);
        QCOMPARE(earlyReplies.at(0).code, RpcClient::kErrNotRunning);

        // Post-Ready passthrough: the fakeserver echoes params back.
        QVector<RpcClient::Reply> replies;
        const qint64 id = h.client->request(
            QStringLiteral("test/echo"), QJsonObject{{QStringLiteral("q"), 42}},
            [&](const RpcClient::Reply& r) { replies.append(r); });
        QVERIFY(id > 0);
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QVERIFY(!replies.at(0).isError);
        QCOMPARE(replies.at(0).result.toObject().value(QStringLiteral("q")).toInt(),
                 42);

        // Clean stop: Stopped lands, the exit pair reaches the wire (the
        // fakeserver exits 0 on exit — the exitCode is the oracle), a
        // stopped client never auto-restarts, and start() re-uses the door.
        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(),
                                  qmn::lsp::LspClient::State::Stopped, 5000);
        QCOMPARE(h.client->isReady(), false);
        QCOMPARE(h.spawnCount, 1);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0, 5000);
        QVERIFY(!h.client->isReady());
        QCOMPARE(h.client->request(QStringLiteral("test/echo"), QJsonObject{},
                                   [](const RpcClient::Reply&) {}),
                 qint64(-1)); // Stopped is a gated state too

        h.client->start(TestHarness::initParams());
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 2, 5000);
        QCOMPARE(h.spawnCount, 2);
        QCOMPARE(h.client->state(), qmn::lsp::LspClient::State::Ready);
        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(),
                                  qmn::lsp::LspClient::State::Stopped, 5000);
    }

    void lspCrashAutoRestartsOnceThenFailedThenManualRestart()
    {
        // The fakeserver crash mode _exit(7)s right after EVERY handshake,
        // so the whole ladder runs: crash 1 -> the ONE auto-restart ->
        // crash 2 -> FAILED -> restartServer() re-arms -> crash 3 -> the
        // one auto-restart again -> crash 4 -> FAILED.
        TestHarness h(QStringLiteral("crash"));
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);
        QSignalSpy failedSpy(h.client.get(), &qmn::lsp::LspClient::failed);
        h.client->start(TestHarness::initParams());
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 7, 5000); // ServerDied,
                                                             // wanted by no one

        // The backoff re-spawn: the session pair was rebuilt whole. (No
        // QCOMPARE(state, Ready) here: the crash mode server dies right
        // after EVERY handshake, so that Ready is a transient the crash-2
        // failure may already have replaced — ready() firing twice IS the
        // observation, the Failed verdict comes below.)
        QTRY_COMPARE_WITH_TIMEOUT(h.spawnCount, 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 2, 5000);

        // Second crash: budget spent -> FAILED (ready never fires a third
        // time, the state never silently revives).
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
        QCOMPARE(h.client->state(), qmn::lsp::LspClient::State::Failed);
        QCOMPARE(h.spawnCount, 2);
        QCOMPARE(readySpy.count(), 2);
        QVERIFY(!h.client->isReady());
        // The dead sessions are really gone: both spawns exited 7 (the
        // recorded closed() codes — no zombie conversation continues).
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(1), 7, 5000);

        // The manual door re-arms the budget: spawn 3 reaches Ready; crash
        // 3 buys exactly one more auto-restart (spawn 4), crash 4 fails.
        h.client->restartServer();
        QTRY_COMPARE_WITH_TIMEOUT(h.spawnCount, 3, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 3, 5000);
        QCOMPARE(failedSpy.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 2, 5000);
        QCOMPARE(h.client->state(), qmn::lsp::LspClient::State::Failed);
        QTRY_COMPARE_WITH_TIMEOUT(h.spawnCount, 4, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 4, 5000);
        QTest::qWait(600); // the backoff window would have re-spawned; let
                           // the third crash fully land
        QCOMPARE(failedSpy.count(), 2); // exactly two failures: no runaway
        QCOMPARE(h.client->state(), qmn::lsp::LspClient::State::Failed);

        // Feature doors in Failed answer the honest local error.
        QVector<RpcClient::Reply> replies;
        QCOMPARE(h.client->request(QStringLiteral("test/echo"), QJsonObject{},
                                   [&](const RpcClient::Reply& r) {
                                       replies.append(r);
                                   }),
                 qint64(-1));
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 3000);
        QCOMPARE(replies.at(0).code, RpcClient::kErrNotRunning);

        h.client->stop(); // honest teardown from Failed: no new spawns
        QCOMPARE(h.client->state(), qmn::lsp::LspClient::State::Stopped);
        QCOMPARE(h.spawnCount, 4);
    }

    void lspDeafServerHandshakeTimesOutWithoutRespawn()
    {
        // A live-but-unresponsive server must NOT be spawned twice: the
        // handshake fails on the pinned 10 s budget and lands in FAILED
        // with the budget still full (restartServer() is the door).
        TestHarness h(QStringLiteral("deaf"));
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);
        QSignalSpy failedSpy(h.client.get(), &qmn::lsp::LspClient::failed);
        h.client->start(TestHarness::initParams());
        QVERIFY(h.client->state() == qmn::lsp::LspClient::State::Starting
                || h.client->state() == qmn::lsp::LspClient::State::Ready);
        QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 1,
                                 RpcClient::kInitializeTimeoutMs + 4000);
        QCOMPARE(h.client->state(), qmn::lsp::LspClient::State::Failed);
        QCOMPARE(h.spawnCount, 1); // the deaf server is never re-spawned
        QCOMPARE(readySpy.count(), 0);
        // The failed server is shut down honestly afterwards (the deaf
        // mode still answers shutdown/exit so the two-phase barrier can
        // complete): exit 0 on the wire.
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0, 5000);
        QVERIFY(failedSpy.at(0).at(0).toString()
                    .startsWith(QStringLiteral("initialize failed")));

        // restartServer() from Failed is the sanctioned door: a fresh
        // session, budget re-armed.
        h.client->restartServer();
        QCOMPARE(h.spawnCount, 2);
        QVERIFY(h.client->state() == qmn::lsp::LspClient::State::Starting
                || h.client->state() == qmn::lsp::LspClient::State::Ready);
        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(),
                                  qmn::lsp::LspClient::State::Stopped, 5000);
        QCOMPARE(h.spawnCount, 2);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(1), 0, 5000); // the fresh
    }

    void lspBadProgramPathFailsWithNoRestartLoop()
    {
        // The synchronous start() refusal: FAILED at once, and the gone
        // latch that fires INSIDE transport->start() must not burn an
        // auto-restart slot — a typo'd path is not a dead SERVER.
        TestHarness h(QStringLiteral("unused-mode-replaced-below"));
        h.program = QStringLiteral("/nonexistent/qmn-lsp-fakeserver");
        h.modeArgs.clear(); // no argv: the spawn is refused at exec time
        QSignalSpy failedSpy(h.client.get(), &qmn::lsp::LspClient::failed);
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);
        h.client->start(TestHarness::initParams());
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 3000);
        QCOMPARE(h.client->state(), qmn::lsp::LspClient::State::Failed);
        QCOMPARE(readySpy.count(), 0);
        QTest::qWait(qmn::lsp::LspClient::kRestartBackoffMs * 3);
        QCOMPARE(h.spawnCount, 1); // the factory was asked exactly once
        QCOMPARE(failedSpy.count(), 1);
    }

    void lspAlwaysAnswerPolicyBatteryValidatedByServer()
    {
        // The fakeserver fires the whole server->client request battery and
        // validates every wire answer itself; its exit code 0 (reaching the
        // wire through the exit pair + EOF) is the oracle. The BATTERY-PASS
        // stderr marker gates the stop: EOF while the battery still awaits
        // an answer is its honest exit-2 hang, never a silent pass.
        TestHarness h(QStringLiteral("requests"));
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);
        QSignalSpy stderrSpy(h.client.get(), &qmn::lsp::LspClient::serverStderr);
        QSignalSpy messageRequestSpy(
            h.client.get(), &qmn::lsp::LspClient::serverShowMessageRequest);
        QSignalSpy progressCreateSpy(
            h.client.get(), &qmn::lsp::LspClient::createWorkDoneProgress);
        h.client->start(TestHarness::initParams());
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);

        QTRY_COMPARE_WITH_TIMEOUT(messageRequestSpy.count(), 1, 5000);
        QCOMPARE(qvariant_cast<QJsonValue>(messageRequestSpy.at(0).at(0))
                     .toInt(),
                 9001);
        QCOMPARE(qvariant_cast<QJsonValue>(messageRequestSpy.at(0).at(1))
                     .toObject()
                     .value(QStringLiteral("message"))
                     .toString(),
                 QStringLiteral("battery message"));
        QTRY_COMPARE_WITH_TIMEOUT(progressCreateSpy.count(), 1, 5000);
        QCOMPARE(qvariant_cast<QJsonValue>(progressCreateSpy.at(0).at(0))
                     .toString(),
                 QStringLiteral("work-1"));

        QTRY_VERIFY_WITH_TIMEOUT(batteryPassed(stderrSpy), 8000);
        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(),
                                  qmn::lsp::LspClient::State::Stopped, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0,
                                  5000); // BATTERY-PASS: every answer matched
    }

    void lspCapabilityRegistryAndApplyEditDoor()
    {
        TestHarness h(QStringLiteral("requests"));
        h.modeArgs << QStringLiteral("noapply"); // the applyEdit answer is
                                                 // the TEST's door, not the
                                                 // client's own policy
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);
        QSignalSpy stderrSpy(h.client.get(), &qmn::lsp::LspClient::serverStderr);
        QSignalSpy applySpy(h.client.get(), &qmn::lsp::LspClient::serverApplyEdit);
        QObject::connect(h.client.get(), &qmn::lsp::LspClient::serverApplyEdit,
                         h.client.get(), [client = h.client.get()](
                                             const QJsonValue& id,
                                             const QJsonValue& edit) {
                             client->respondApplyEdit(id, edit.toObject()
                                                          .value(QStringLiteral("label"))
                                                          .toString()
                                                      == QLatin1String("battery edit"));
                         });
        h.client->start(TestHarness::initParams());
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);

        // The registry flips as the battery's registrations come and go:
        // 9006 registers hover (flip #1), 9007 unregisters it (flip #2).
        // The 9005 invalid batch answers -32803 and registers NOTHING, so
        // it never fires the signal — the all-or-nothing veto is the
        // fakeserver's wire-side oracle, this gate is the client-side state
        // M5's feature doors will consult.
        // A per-emission recorder, not a QSignalSpy count chase: the
        // battery's two registration rounds can land inside one qWait
        // slice (0 -> 2 with no observable midpoint), but the lambda runs
        // INSIDE each emission, so the recorded hover-gate sequence is the
        // race-free observation.
        QStringList hoverFlips;
        QObject::connect(h.client.get(),
                         &qmn::lsp::LspClient::registrationsChanged,
                         h.client.get(), [&hoverFlips, client = h.client.get()] {
                             hoverFlips << (client->isRegistered(
                                                 QStringLiteral(
                                                     "textDocument/hover"))
                                                 ? QStringLiteral("on")
                                                 : QStringLiteral("off"));
                         });
        QCOMPARE(hoverFlips.size(), 0);
        QCOMPARE(h.client->isRegistered(QStringLiteral("textDocument/hover")),
                 false);
        QTRY_COMPARE_WITH_TIMEOUT(hoverFlips,
                                  (QStringList{QStringLiteral("on"),
                                               QStringLiteral("off")}),
                                  5000);
        QCOMPARE(h.client->isRegistered(QStringLiteral("textDocument/magic")),
                 false); // the veto left NOTHING registered

        QTRY_COMPARE_WITH_TIMEOUT(applySpy.count(), 1, 5000);
        QCOMPARE(qvariant_cast<QJsonValue>(applySpy.at(0).at(0)).toInt(), 9009);
        QCOMPARE(qvariant_cast<QJsonValue>(applySpy.at(0).at(1))
                     .toObject()
                     .value(QStringLiteral("label"))
                     .toString(),
                 QStringLiteral("battery edit"));

        // The door has answered and the battery completed on the wire
        // (BATTERY-PASS): only THEN stop — the battery server exits on the
        // stdin EOF that transport->stop() causes, and an EOF mid-battery
        // would be the fakeserver's exit-2 hang instead of a clean exit 0.
        QTRY_VERIFY_WITH_TIMEOUT(batteryPassed(stderrSpy), 8000);
        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->state(),
                                  qmn::lsp::LspClient::State::Stopped, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0,
                                  5000); // the door's {applied:true} landed
        QCOMPARE(h.client->isRegistered(QStringLiteral("textDocument/hover")),
                 false); // the battery's own unregister round flipped it
                         // off (registrations_ is the client's cross-session
                         // ledger in M2's design)
    }
    // ------------------------------------------------ M3: position converter
    void positionConverterWireEncodingNames()
    {
        using PC = qmn::lsp::PositionConverter;
        using Enc = PC::Encoding;
        QVERIFY(PC::fromWire(QStringLiteral("utf-8")) == Enc::Utf8);
        QVERIFY(PC::fromWire(QStringLiteral("utf-32")) == Enc::Utf32);
        QVERIFY(PC::fromWire(QStringLiteral("utf-16")) == Enc::Utf16);
        // The spec default rule: missing/empty/garbage all read as utf-16.
        QVERIFY(PC::fromWire(QString()) == Enc::Utf16);
        QVERIFY(PC::fromWire(QStringLiteral("utf-64")) == Enc::Utf16);
        QVERIFY(PC::fromWire(QStringLiteral("UTF-8")) == Enc::Utf16); // the
            // wire spelling is exact ("UTF-8" is NOT a spec positionEncoding)
        QCOMPARE(PC::toWire(Enc::Utf8), QStringLiteral("utf-8"));
        QCOMPARE(PC::toWire(Enc::Utf16), QStringLiteral("utf-16"));
        QCOMPARE(PC::toWire(Enc::Utf32), QStringLiteral("utf-32"));
        // One code point, three frames (the table the line walks accumulate).
        QCOMPARE(PC::unitWidth(u'a', Enc::Utf8), 1);
        QCOMPARE(PC::unitWidth(0x00E9, Enc::Utf8), 2);
        QCOMPARE(PC::unitWidth(0x4E2D, Enc::Utf8), 3);
        QCOMPARE(PC::unitWidth(0x1F30D, Enc::Utf8), 4); // non-BMP: 4 bytes
        QCOMPARE(PC::unitWidth(u'a', Enc::Utf16), 1);
        QCOMPARE(PC::unitWidth(0x4E2D, Enc::Utf16), 1);
        QCOMPARE(PC::unitWidth(0x1F30D, Enc::Utf16), 2); // the surrogate pair
        QCOMPARE(PC::unitWidth(u'a', Enc::Utf32), 1);
        QCOMPARE(PC::unitWidth(0x1F30D, Enc::Utf32), 1); // one unit, always
    }

    void positionConverterRoundTripsEveryEncoding()
    {
        using PC = qmn::lsp::PositionConverter;
        using Enc = PC::Encoding;
        // Latin + e-acute + CJK + emoji (a surrogate PAIR) + 'b', newline,
        // Cyrillic: from the emoji on, all three frames disagree on columns.
        const QString text
            = QStringLiteral("a\u00E9\u4E2D\U0001F30Db\n\u043C\u0438\u0440");
        // Indices: a0 e1 CJK2 pair(3,4) b5 NL6 м7 и8 р9 -- size 10.
        QCOMPARE(text.size(), 10);

        // Column pins (the clangd utf-8 reality: the emoji's 4 bytes vs the
        // 2 UTF-16 units shift every later column; utf-32 counts neither).
        QCOMPARE(PC::toPosition(text, 3, Enc::Utf8).character, 6);
        QCOMPARE(PC::toPosition(text, 4, Enc::Utf8).character, 6); // mid-pair:
                                                                   // pair not
                                                                   // counted
        QCOMPARE(PC::toPosition(text, 5, Enc::Utf8).character, 10);
        QCOMPARE(PC::toPosition(text, 6, Enc::Utf8).character, 11);
        QCOMPARE(PC::toPosition(text, 3, Enc::Utf32).character, 3);
        QCOMPARE(PC::toPosition(text, 4, Enc::Utf32).character, 3); // the pair
        QCOMPARE(PC::toPosition(text, 5, Enc::Utf32).character, 4); // is ONE unit
        QCOMPARE(PC::toPosition(text, 6, Enc::Utf32).character, 5);
        QCOMPARE(PC::toPosition(text, 4, Enc::Utf16).character, 4); // utf-16
                                                                    // DOES count
        QCOMPARE(PC::toPosition(text, 5, Enc::Utf16).character, 5); // the pair's
                                                                    // 2 units
        QCOMPARE(PC::toPosition(text, 6, Enc::Utf16).character, 6); // before NL
        QCOMPARE(PC::toPosition(text, 7, Enc::Utf16).line, 1);
        QCOMPARE(PC::toPosition(text, 7, Enc::Utf16).character, 0);
        QCOMPARE(PC::toPosition(text, 8, Enc::Utf8).character, 2); // Cyrillic
        QCOMPARE(PC::toPosition(text, 10, Enc::Utf8).character, 6); // is 2 bytes
        QCOMPARE(PC::toPosition(text, 10, Enc::Utf16).character, 3); // each
        QCOMPARE(PC::toPosition(text, 10, Enc::Utf32).character, 3);

        // The inverse doors (and the surrogate-boundary discipline: the same
        // one the view's backspace keeps — a column landing INSIDE a code
        // point's encoding stops BEFORE it, never splits it).
        QCOMPARE(PC::toOffset(text, 0, 6, Enc::Utf8), 3); // pair START's byte
        QCOMPARE(PC::toOffset(text, 0, 7, Enc::Utf8), 3); // bytes 7..9 fall
        QCOMPARE(PC::toOffset(text, 0, 9, Enc::Utf8), 3); // inside the pair's
        QCOMPARE(PC::toOffset(text, 0, 10, Enc::Utf8), 5); // sequence: all land
                                                           // before it
        QCOMPARE(PC::toOffset(text, 0, 4, Enc::Utf16), 3); // unit 4 would cut
        QCOMPARE(PC::toOffset(text, 0, 5, Enc::Utf16), 5); // the pair: stop at 3
        QCOMPARE(PC::toOffset(text, 0, 4, Enc::Utf32), 5); // pair is unit 3:
                                                           // unit 4 is after it
        QCOMPARE(PC::toOffset(text, 1, 4, Enc::Utf8), 9); // м+и done, р would
        QCOMPARE(PC::toOffset(text, 1, 6, Enc::Utf8), 10); // overshoot: stop

        // Clamps are TOTAL (a tolerant server reads them; they never fault):
        // line past EOF -> document end, character past EOL -> line end.
        QCOMPARE(PC::toOffset(text, 9, 9, Enc::Utf8), text.size());
        QCOMPARE(PC::toOffset(text, 0, 100000, Enc::Utf32), 6);
        QCOMPARE(PC::toOffset(text, -3, -7, Enc::Utf16), 0);
        QCOMPARE(PC::toPosition(text, 100000, Enc::Utf16).line, 1);
        QCOMPARE(PC::toPosition(text, 100000, Enc::Utf16).character, 3);
        QCOMPARE(PC::toPosition(text, -5, Enc::Utf8).character, 0);

        // The round trip: every boundary offset returns to itself in every
        // frame...
        for (const Enc enc : {Enc::Utf8, Enc::Utf16, Enc::Utf32}) {
            for (int offset = 0; offset <= text.size(); ++offset) {
                if (offset == 4)
                    continue; // mid-pair: pinned right below
                const qmn::lsp::LspPosition p
                    = PC::toPosition(text, offset, enc);
                QCOMPARE(PC::toOffset(text, p.line, p.character, enc), offset);
            }
            // ...and the one degenerate offset INSIDE the pair (view edits
            // stay on boundaries, core does not — the mirror could in theory
            // carry one) snaps BACK to the pair's START in every frame: a
            // split never survives the position frame.
            const qmn::lsp::LspPosition split = PC::toPosition(text, 4, enc);
            QCOMPARE(PC::toOffset(text, split.line, split.character, enc), 3);
        }

        // Edges: empty document, blank lines (an empty line between two
        // newlines), trailing-newline offset, combining marks counted as
        // their OWN units (grapheme-accurate columns are NOT ours — the
        // AGENTS.md standing decision).
        QCOMPARE(PC::toPosition(QString(), 0, Enc::Utf8).line, 0);
        QCOMPARE(PC::toPosition(QString(), 0, Enc::Utf32).character, 0);
        QCOMPARE(PC::toOffset(QString(), 4, 8, Enc::Utf8), 0);
        const QString blankLines = QStringLiteral("a\n\nb");
        QCOMPARE(PC::toPosition(blankLines, 2, Enc::Utf16).line, 1);
        QCOMPARE(PC::toPosition(blankLines, 2, Enc::Utf16).character, 0);
        QCOMPARE(PC::toPosition(blankLines, 3, Enc::Utf8).line, 2);
        QCOMPARE(PC::toPosition(blankLines, 3, Enc::Utf8).character, 0);
        QCOMPARE(PC::toOffset(blankLines, 2, 0, Enc::Utf32), 3);
        const QString decomposed = QStringLiteral("e\u0301x");
        QCOMPARE(PC::toPosition(decomposed, 2, Enc::Utf8).character, 3); // e(1B)
                                                                         // + U+0301(2B)
        QCOMPARE(PC::toPosition(decomposed, 2, Enc::Utf16).character, 2);
        QCOMPARE(PC::toPosition(decomposed, 2, Enc::Utf32).character, 2);
    }

    void positionConverterMatchesReferenceUtf8Offsets()
    {
        // The wire-side counterpart of the round-trip slot: for every
        // boundary offset the utf-8 CHARACTER equals what the spec's own
        // definition says — the UTF-8 byte length of the line prefix. Qt's
        // codec is an independent source of truth here (the production
        // converter walks its own width table; agreement is the pin).
        const QString text = QStringLiteral("a\u00E9\u4E2D\U0001F30Db\n"
                                            "\u043C\u0438\u0440 \U0001F680");
        const QByteArray line0 = text.left(6).toUtf8(); // "a" + e-acute + CJK
                                                        // + emoji + "b"
        int utf8Char = 0;
        int p = 0;
        while (p < 6) { // walk code points along the reference utf-8 bytes
            int units = 1;
            if (text.at(p).isHighSurrogate())
                units = 2;
            const QByteArray cp = text.mid(p, units).toUtf8();
            QCOMPARE(line0.indexOf(cp, utf8Char), utf8Char); // contiguous
            utf8Char += cp.size();
            p += units;
            const qmn::lsp::LspPosition pos
                = qmn::lsp::PositionConverter::toPosition(text, p,
                                                          qmn::lsp::PositionConverter::Encoding::Utf8);
            QCOMPARE(pos.line, 0);
            QCOMPARE(pos.character, utf8Char); // the table matches Qt's encoder
            QCOMPARE(qmn::lsp::PositionConverter::toOffset(
                         text, 0, utf8Char,
                         qmn::lsp::PositionConverter::Encoding::Utf8),
                     p);
        }
        QCOMPARE(utf8Char, line0.size()); // 1+2+3+4+1 = 11 bytes
    }

    // -------------------------------------------------- M3: document syncer
    void documentSyncerDeltaStreamMatchesServerUtf16()
    {
        TestHarness h(QStringLiteral("sync"));
        h.modeArgs << QStringLiteral("utf-16");
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);
        h.client->start(TestHarness::initParams());
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 12000);
        QCOMPARE(h.client->positionEncoding(), QStringLiteral("utf-16"));

        qmn::lsp::DocumentSyncer syncer(h.client.get());
        syncer.setUri(QStringLiteral("file:///sync-utf16.txt"));
        syncer.setDebounceMs(50); // the test clocks, not the shipped budgets
        syncer.setTypingCapMs(300);
        QSignalSpy flushedSpy(&syncer,
                              &qmn::lsp::DocumentSyncer::flushed);

        syncer.open(QStringLiteral("one\ntwo\nthree"),
                    QStringLiteral("plaintext"));
        QVERIFY(syncer.isOpen());
        QCOMPARE(syncer.version(), qint64(0)); // didOpen carries version 0
        QJsonObject verdict = assertSyncModel(h.client.get(),
                                              QStringLiteral("one\ntwo\nthree"),
                                              0);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict))); // didOpen handed over the
                                                  // whole text

        // The three-path invariant runs through the WHOLE slot: the naive
        // reference QString (QTest's own path), the syncer mirror and the
        // SERVER model must stay equal across every edit.
        QString reference = QStringLiteral("one\ntwo\nthree");
        auto edit = [&](int start, int end, const QString& inserted) {
            syncer.applyDelta(start, end, inserted);
            reference.replace(start, end - start, inserted);
            QCOMPARE(syncer.mirrorText(), reference); // the mirror replays
                                                      // each delta at once
        };
        edit(3, 3, QStringLiteral("\nzero")); // insert-only split
        edit(9, 12, QStringLiteral("2"));     // range replace ("two" -> "2")
        edit(0, 0, QStringLiteral("W"));      // at document start
        syncer.flush(); // explicit flush: THREE deltas travel as ONE didChange
        QCOMPARE(flushedSpy.count(), 1);
        QCOMPARE(flushedSpy.at(0).at(0).toLongLong(),
                 qint64(1)); // version bumps ONCE per flush, not per delta
        QCOMPARE(flushedSpy.at(0).at(1).toInt(), 3); // sequential replay, no
                                                     // coalescing by design
        verdict = assertSyncModel(h.client.get(), reference, 1);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));

        // The debounce burst: two deltas, NO manual flush, must go out as
        // ONE didChange carrying TWO contentChanges.
        edit(1, 4, QStringLiteral("orld")); // "one" -> "orld" (range)
        edit(reference.size(), reference.size(),
             QStringLiteral("!")); // end-of-document insert
        QTRY_COMPARE_WITH_TIMEOUT(flushedSpy.count(), 2, 5000); // the 50 ms
                                                                // debounce fired
        QCOMPARE(flushedSpy.at(1).at(0).toLongLong(), qint64(2));
        QCOMPARE(flushedSpy.at(1).at(1).toInt(), 2);
        verdict = assertSyncModel(h.client.get(), reference, 2);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));

        // The typing CAP: a burst that never pauses still flushes (the 5 s
        // debounce would starve a fast typist's server forever).
        syncer.setDebounceMs(5000);
        syncer.setTypingCapMs(150);
        edit(0, 0, QStringLiteral("X"));
        QTRY_COMPARE_WITH_TIMEOUT(flushedSpy.count(), 3, 3000); // < the 5 s
                                                                // debounce: the
                                                                // 150 ms cap won
        QCOMPARE(flushedSpy.at(2).at(1).toInt(), 1);
        verdict = assertSyncModel(h.client.get(), reference, 3);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));

        // save(): flush-then-didSave (the server is current BEFORE the save).
        syncer.save();
        verdict = assertSyncModel(h.client.get(), reference, 3);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        QCOMPARE(verdict.value(QStringLiteral("saves")).toInt(),
                 1); // didSave really landed

        // close + reopen on the same key: version restarts at 0 and the
        // server re-seeds from the didOpen text. (A syncer that FORGOT the
        // didClose would drive the fakeserver's structural check to an honest
        // exit 2 — the assert's answer would never arrive.)
        syncer.close();
        QVERIFY(!syncer.isOpen());
        syncer.open(QStringLiteral("fresh"), QStringLiteral("plaintext"));
        QCOMPARE(syncer.version(), qint64(0));
        QCOMPARE(syncer.mirrorText(), QStringLiteral("fresh"));
        verdict = assertSyncModel(h.client.get(), QStringLiteral("fresh"), 0);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        QCOMPARE(verdict.value(QStringLiteral("open")).toBool(), true);
        QCOMPARE(verdict.value(QStringLiteral("saves")).toInt(),
                 0); // the model re-seeded, not carried over

        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(
            h.client->state(), qmn::lsp::LspClient::State::Stopped, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0,
                                  5000); // shutdown/exit stayed honest
    }

    void documentSyncerUtf8ColumnsAndFullFallbackSync()
    {
        TestHarness h(QStringLiteral("sync"));
        h.modeArgs << QStringLiteral("utf-8"); // clangd's REAL frame: the
        // server-side columns disagree with utf-16 ones, so a converter bug
        // can only show up as a model mismatch here.
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);
        h.client->start(TestHarness::initParams());
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 12000);
        QCOMPARE(h.client->positionEncoding(), QStringLiteral("utf-8"));

        qmn::lsp::DocumentSyncer syncer(h.client.get());
        syncer.setUri(QStringLiteral("file:///sync-utf8.txt"));
        syncer.setDebounceMs(50);
        QSignalSpy flushedSpy(&syncer,
                              &qmn::lsp::DocumentSyncer::flushed);
        // привет мир + emoji + " ok": Cyrillic (2 bytes each) + a 4-byte
        // emoji. utf-16 size 16, the utf-8 line carries 27 units.
        const QString base
            = QStringLiteral("\u043F\u0440\u0438\u0432\u0435\u0442 "
                             "\u043C\u0438\u0440 \U0001F30D ok");
        QCOMPARE(base.size(), 16);
        syncer.open(base, QStringLiteral("plaintext"));
        QVERIFY(syncer.encoding()
                == qmn::lsp::PositionConverter::Encoding::Utf8);
        QJsonObject verdict = assertSyncModel(h.client.get(), base, 0);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));

        QString reference = base;
        auto edit = [&](int start, int end, const QString& inserted) {
            syncer.applyDelta(start, end, inserted);
            reference.replace(start, end - start, inserted);
            QCOMPARE(syncer.mirrorText(), reference);
        };
        edit(16, 16, QStringLiteral("!")); // after "ok": the utf-8 character
        edit(10, 10, QStringLiteral("big ")); // for THIS edit is 27+1, not 17;
        syncer.flush(); // the server decodes the columns with its OWN decoder
        QCOMPARE(flushedSpy.count(), 1);
        QCOMPARE(flushedSpy.at(0).at(1).toInt(), 2);
        verdict = assertSyncModel(h.client.get(), reference, 1);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict))); // wrong-frame columns could
                                                  // NOT survive this compare

        // The server-primed edit suppression pair: beginServerEdit flushes
        // the user's queued deltas FIRST, the deltas fed inside ride only the
        // mirror (no echo back), endServerEdit tells the story with ONE
        // range-less (full) contentChange.
        syncer.beginServerEdit();
        edit(0, 6, QStringLiteral("HELLO")); // this must NOT come back as a
                                            // didChange delta...
        QCOMPARE(syncer.hasPendingChanges(), false); // ...nothing was queued
        syncer.endServerEdit(); // ...and here comes the full sync, NOW
        QCOMPARE(flushedSpy.count(), 2);
        QCOMPARE(flushedSpy.at(1).at(1).toInt(), 1); // exactly ONE change
        verdict = assertSyncModel(h.client.get(), reference, 2);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict))); // the range-less text carried
                                                  // the HELLO edit exactly

        // forceFullSync: the desync detector's door — the NEXT flush goes out
        // FULL even though a plain delta is already pending (the full text
        // absorbs the queue; the server sees the mirror, one change).
        syncer.setDebounceMs(5000); // pin: nothing flushes on the debounce
        syncer.setTypingCapMs(6000); // ...or the cap during this phase
        edit(reference.size(), reference.size(), QStringLiteral(" tail"));
        syncer.forceFullSync();
        syncer.flush();
        QCOMPARE(flushedSpy.count(), 3);
        QCOMPARE(flushedSpy.at(2).at(1).toInt(), 1); // the full text, NOT
                                                     // delta+full as two changes
        verdict = assertSyncModel(h.client.get(), reference, 3);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        QVERIFY(!syncer.hasPendingChanges());

        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(
            h.client->state(), qmn::lsp::LspClient::State::Stopped, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0, 5000);
    }

    void documentSyncerReDidOpenSelfHealAcrossRestart()
    {
        // crashonce: the FIRST session _exit(7)s right after its handshake —
        // the client's one auto-restart rebuilds the pair, ready() re-fires,
        // and the syncer's re-didOpen must carry the mirror WHOLE (edits fed
        // while the server was down included: the didOpen IS the full sync).
        TestHarness h(QStringLiteral("sync"));
        h.modeArgs << QStringLiteral("utf-16") << QStringLiteral("crashonce");
        h.crashFirstSessionOnly = true; // harness-side one-shot: the flag
                                        // reaches ONLY spawn #0's argv
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);
        QSignalSpy failedSpy(h.client.get(), &qmn::lsp::LspClient::failed);
        h.client->start(TestHarness::initParams());

        qmn::lsp::DocumentSyncer syncer(h.client.get());
        syncer.setUri(QStringLiteral("file:///sync-heal.txt"));
        syncer.setDebounceMs(10); // flushes WILL fire during the restart
                                  // window and must survive them (they simply
                                  // keep the queue: not-Ready flush is a no-op)
        QSignalSpy flushedSpy(&syncer,
                              &qmn::lsp::DocumentSyncer::flushed);

        syncer.open(QStringLiteral("alpha beta"), QStringLiteral("plaintext"));
        QString reference = QStringLiteral("alpha beta");
        syncer.applyDelta(6, 10, QStringLiteral("BETA")); // "alpha BETA"
        reference.replace(6, 4, QStringLiteral("BETA"));
        QCOMPARE(syncer.mirrorText(), reference);

        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 2, 12000); // TWO handshakes:
        QCOMPARE(failedSpy.count(), 0); // the crash rode the ONE budget
        QCOMPARE(h.spawnCount, 2);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 7, 5000); // the first
                                                             // session crashed
        QVERIFY(syncer.isOpen()); // close did NOT follow the crash: the
                                  // self-heal re-didOpen is the contract

        // Whatever flushed/debounce timers fired while the session was down,
        // the second session's didOpen carried the mirror whole. The version
        // pin proves the didOpen re-began the document (version 0), NOT a
        // resumed didChange stream.
        QJsonObject verdict = assertSyncModel(h.client.get(), reference, 0);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        QCOMPARE(verdict.value(QStringLiteral("open")).toBool(), true);
        QVERIFY(!syncer.hasPendingChanges()); // the didOpen absorbed the queue

        // And the healed session syncs normally afterwards: a delta fed now
        // goes out as a real didChange (version 1 on the re-opened document).
        syncer.applyDelta(0, 0, QStringLiteral("Z"));
        reference.replace(0, 0, QStringLiteral("Z"));
        syncer.flush();
        QCOMPARE(flushedSpy.count(), 1); // the FIRST honest didChange of this
                                         // document lifetime (the down-window
                                         // flushes stayed silent)
        QCOMPARE(flushedSpy.at(0).at(0).toLongLong(), qint64(1));
        verdict = assertSyncModel(h.client.get(), reference, 1);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));

        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(
            h.client->state(), qmn::lsp::LspClient::State::Stopped, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(1), 0, 5000);
    }

    void documentSyncerNotReadyDoorsStayHonest()
    {
        // The non-Ready doors are HONEST: no session means no wire traffic,
        // the syncer keeps the document registered and the mirror current,
        // version never bumps for a flush that did not go out — and the
        // ready() re-didOpen contract owns the resume (the mirror travels
        // inside the didOpen, so even the queued deltas are NOT lost).
        TestHarness h(QStringLiteral("sync"));
        h.modeArgs << QStringLiteral("utf-16");
        qmn::lsp::DocumentSyncer syncer(h.client.get());
        syncer.setUri(QStringLiteral("file:///sync-noconn.txt"));
        syncer.setDebounceMs(10);
        QSignalSpy flushedSpy(&syncer,
                              &qmn::lsp::DocumentSyncer::flushed);

        syncer.open(QStringLiteral("never sent"), QStringLiteral("plaintext"));
        syncer.applyDelta(0, 5, QStringLiteral("ALWAYS"));
        syncer.save(); // flush-then-didSave without a session: silent, safe
        QTest::qWait(600); // past the debounce AND the cap windows: the timers
                           // fired into the not-Ready no-op and stayed quiet
        QCOMPARE(flushedSpy.count(), 0); // NOTHING went out...
        QCOMPARE(syncer.version(), qint64(0)); // ...and no version burned on
                                               // an un-sent flush
        QVERIFY(syncer.hasPendingChanges()); // the document waits, not lost
        QCOMPARE(syncer.mirrorText(), QStringLiteral("ALWAYS sent"));

        // Start for real: ready() arrives, the self-heal didOpen carries the
        // mirror (with the pre-session edit inside), and the assert door
        // confirms the whole story — it rides the SAME stream after the
        // didOpen, so ordering IS the synchronization.
        QSignalSpy readySpy(h.client.get(), &qmn::lsp::LspClient::ready);
        h.client->start(TestHarness::initParams());
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 12000);
        QJsonObject verdict = assertSyncModel(
            h.client.get(), QStringLiteral("ALWAYS sent"), 0);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        QVERIFY(!syncer.hasPendingChanges()); // the didOpen absorbed the queue
        h.client->stop();
        QTRY_COMPARE_WITH_TIMEOUT(
            h.client->state(), qmn::lsp::LspClient::State::Stopped, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 0, 5000);
    }

};

QTEST_GUILESS_MAIN(TstLsp)
#include "tst_lsp.moc"
