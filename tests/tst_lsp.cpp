// The 5th ctest entry (M8 machine 1): JsonFraming + the three transports +
// RpcClient, headless/guiless — the PLAN.md M8 layering contract made
// literal ("the protocol layers NEVER know CodeEditor/CodeDocument"). The
// in-process QTcpServer fake peer plays the server side for RpcClient;
// the smart text-model fakeserver (machines 2/3) joins this same binary.

#include <QtTest>

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVector>

#include <functional>

#include "protocol/JsonFraming.hpp"
#include "protocol/ProcessTransport.hpp"
#include "protocol/RpcClient.hpp"
#include "protocol/TcpTransport.hpp"

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
};

QTEST_GUILESS_MAIN(TstLsp)
#include "tst_lsp.moc"
