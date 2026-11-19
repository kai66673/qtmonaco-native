// The 5th ctest entry (M8 machine 1): JsonFraming + the three transports +
// RpcClient, headless/guiless — the PLAN.md M8 layering contract made
// literal ("the protocol layers NEVER know CodeEditor/CodeDocument"). The
// in-process QTcpServer fake peer plays the server side for RpcClient;
// the smart text-model fakeserver (machines 2/3) joins this same binary.
// Machine 4 (the app-layer LspHub) is the exception the contract plans for:
// the GLUE is allowed to know the view, so the hub slots drive a real
// CodeEditor and the binary runs offscreen with QTEST_MAIN.
//
// NOTE for anyone adding includes here: view/HighlightEngine.hpp pulls the
// VENDORED tree-sitter api.h, which this target does NOT put on its include
// path (the system /usr/include/tree_sitter trap — AGENTS.md). view/
// CodeEditor.hpp stays clean of it, which is what keeps this file building.

#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <QVector>

#include <functional>
#include <memory>

#include "app/glue/LspHub.hpp"
#include "lsp/DocumentSyncer.hpp"
#include "lsp/LspClient.hpp"
#include "lsp/PositionConverter.hpp"
#include "lsp/WorkspaceEdit.hpp"
#include "protocol/JsonFraming.hpp"
#include "protocol/ProcessTransport.hpp"
#include "protocol/RpcClient.hpp"
#include "protocol/TcpTransport.hpp"
#include "view/CodeDocument.hpp"
#include "view/CodeEditor.hpp"

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
// The door behind every server-side query the M2/M3/M4 slots share: an
// explicit pump, not a QTRY macro — QTRY's failure path RETURNS, which a
// value-returning helper cannot do. No answer means the fakeserver died on a
// structural lie (exit 2): an honest note object, never a silent pass.
QJsonObject askServer(qmn::lsp::LspClient* client, const QString& method,
                      const QJsonObject& params)
{
    bool answered = false;
    QJsonObject result;
    client->request(method, params,
                    [&answered, &result](const RpcClient::Reply& reply) {
                        answered = true;
                        if (!reply.isError)
                            result = reply.result.toObject();
                    });
    for (int waited = 0; waited < 5000 && !answered; waited += 50)
        QTest::qWait(50); // the request rides the SAME stream as the didChange
                          // whose effect it checks: ordering IS the
                          // synchronization, no extra waits
    if (!answered) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("note"),
                            QStringLiteral("%1 never answered (structural "
                                           "fakeserver fail: look for "
                                           "FAKESERVER-FAIL on stderr)")
                                .arg(method)}};
    }
    return result;
}

// The machine-3 verdict door: asks the sync-mode fakeserver to compare its
// INDEPENDENT text model against the expected text (the test's own naive
// QString oracle) and version. A mismatch comes back as an honest ok:false
// (server model included in the reply for the failure dump), never an exit 2
// — a shared decoder would cancel client and server bugs into a green test.
QJsonObject assertSyncModel(qmn::lsp::LspClient* client,
                            const QString& expectedText, qint64 expectedVersion)
{
    return askServer(client, QStringLiteral("test/assertModel"),
                     QJsonObject{{QStringLiteral("expectedText"), expectedText},
                                 {QStringLiteral("expectedVersion"),
                                  double(expectedVersion)}});
}

// M4 multi-doc reading of the same door (the hub carries MANY documents per
// client, so the bare form no longer identifies the subject).
QJsonObject assertSyncModel(qmn::lsp::LspClient* client, const QString& uri,
                            const QString& expectedText, qint64 expectedVersion)
{
    QJsonObject params{{QStringLiteral("expectedText"), expectedText},
                       {QStringLiteral("expectedVersion"),
                        double(expectedVersion)},
                       {QStringLiteral("uri"), uri}};
    return askServer(client, QStringLiteral("test/assertModel"), params);
}

// M4 pins: how many documents the SERVER holds open for this client, and the
// initialize params it was handed (the matrix/template merge proof).
int serverDocumentCount(qmn::lsp::LspClient* client)
{
    const QJsonObject answer = askServer(client,
                                         QStringLiteral("test/documentCount"),
                                         QJsonObject{});
    return answer.contains(QStringLiteral("count"))
               ? answer.value(QStringLiteral("count")).toInt()
               : -1; // never answered (or an error reply): the honest -1
}

QJsonObject serverInitializeParams(qmn::lsp::LspClient* client)
{
    return askServer(client, QStringLiteral("test/initializeParams"),
                     QJsonObject{});
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

// Waits out a client handshake through ANY path (the process spawn, the
// never-Ready document's deferred didOpen, the auto-restart): the state door,
// not a ready() spy that might have missed a signal that already fired.
bool waitReady(qmn::lsp::LspClient* client, int timeoutMs = 15000)
{
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        if (client->isReady())
            return true;
        QTest::qWait(50);
    }
    return client->isReady();
}

// ------------------------------------------------------------- M4 hub seam
// A tiny predicate pump: QSignalSpy's count() and any other int/bool property
// (the QTRY macros end in a bare `return;`, which is fine inside a slot).
template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs = 8000)
{
    for (int waited = 0; waited < timeoutMs; waited += 25) {
        if (predicate())
            return true;
        QTest::qWait(25);
    }
    return predicate();
}

// Machine 5's server-side verdicts (APPLYREQ-OK, APPLYCHECK-OK, ...) ride the
// STDERR channel: a machine-5 pin judged by the fakeserver itself, because
// what the CLIENT answered to a server-primed request is traffic no test on
// this side can observe. An exit-2 structural lie shows up here as
// FAKESERVER-FAIL, and the waits below fail with the whole channel in the
// message.
bool sawMarker(const QSignalSpy& stderrSpy, const QString& marker)
{
    for (const QList<QVariant>& args : stderrSpy)
        if (args.at(0).toString().contains(marker))
            return true;
    return false;
}

bool waitMarker(const QSignalSpy& stderrSpy, const QString& marker,
                int timeoutMs = 8000)
{
    return waitUntil([&] { return sawMarker(stderrSpy, marker); }, timeoutMs);
}

QString stderrDump(const QSignalSpy& stderrSpy)
{
    QStringList lines;
    for (const QList<QVariant>& args : stderrSpy)
        lines << args.at(0).toString();
    return QStringLiteral("server stderr: [%1]").arg(lines.join(QLatin1String(" | ")));
}

// The machine-4 harness: the fakeserver spawn bookkeeping of TestHarness, but
// the LspClients are the HUB's (one per spec, PLAN) and the mode argv comes
// from the spec the test installs. `crashingSpawns` is the harness-side copy
// of the per-process "crashonce" flag: spawn #0..#(n-1) carry it, the rest
// start clean — the lifecycle drill (auto-restart budget, then FAILED, then
// the manual restartServer door) needs exactly two dying sessions.
struct HubHarness {
    struct Spawn {
        ProcessTransport* transport = nullptr; // may die: never dereferenced
        bool opened = false;
        int exitCode = -1; // -1 until the process is reaped
    };

    QString program = QString::fromLatin1(QMN_TEST_LSP_SERVER);
    QStringList modeArgs;
    // The session-death knob (the fakeserver's "crashonce" is PER-PROCESS, so
    // the counting lives here): 0 = no spawn dies, N>0 = the FIRST N spawns
    // die right after their handshake, -1 = every spawn dies.
    static constexpr int kCrashEverySpawn = -1;
    int crashSpawnCount = 0;
    int spawnCount = 0;
    QVector<Spawn> spawned;
    qmn::app::LspHub hub;

    explicit HubHarness(const QStringList& modes)
        : modeArgs(modes)
    {
        // The factory ignores the spec's argv and uses modeArgs (the hub
        // already expands ${WORKSPACE_ROOT} into spec.program, which is what
        // the spawn really needs).
        hub.setTransportFactory(
            [this](const qmn::app::LspHub::ServerSpec& spec, QObject* parent) {
                ++spawnCount;
                QStringList args = modeArgs;
                const bool dies = crashSpawnCount < 0
                                  || (crashSpawnCount > 0
                                      && spawned.size()
                                             < size_t(crashSpawnCount));
                if (dies)
                    args << QStringLiteral("crashonce");
                auto* transport = new ProcessTransport(spec.program, args,
                                                       parent);
                const int index = spawned.size();
                spawned.append(Spawn{transport, false, -1});
                QObject::connect(transport, &Transport::opened, transport,
                                 [this, index] { spawned[index].opened = true; });
                QObject::connect(transport, &Transport::closed, transport,
                                 [this, index] {
                                     if (spawned[index].opened)
                                         spawned[index].exitCode
                                             = spawned[index].transport->exitCode();
                                 });
                return static_cast<Transport*>(transport);
            });
    }

    // The spec under test: program/args are the fakeserver + mode; `name` is
    // what the slots call restartServer with.
    void installSpec(const QString& name, const QStringList& languages,
                     const QJsonObject& initializeTemplate = QJsonObject())
    {
        qmn::app::LspHub::ServerSpec spec;
        spec.name = name;
        spec.program = program;
        spec.args = modeArgs;
        spec.languages = languages;
        spec.initializeTemplate = initializeTemplate;
        hub.setServerSpec(spec);
    }

    int exitCodeOf(int spawnIndex) const
    {
        return spawnIndex < spawned.size() ? spawned.at(spawnIndex).exitCode
                                           : -1;
    }
};

// Machine-5's reply probe for the hub's feature doors: the handler fires
// EXACTLY ONCE (the door's own contract), so ONE capture is the whole
// truth; arrived() distinguishes silence from a refusal.
struct FeatureProbe {
    bool arrived = false;
    qmn::protocol::RpcClient::Reply reply;

    qmn::app::LspHub::FeatureHandler handler()
    {
        return [this](const qmn::protocol::RpcClient::Reply& r) {
            arrived = true;
            reply = r;
        };
    }
};

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

    // ------------------------------------------------------- M4: the glue
    void positionConverterToModelPositionPins()
    {
        // The hub's diagnostics decoder rides this door: the wire frame comes
        // back in the VIEW's space (line + utf-16 column = CodeEditor::
        // Diagnostic's own coordinates), with toOffset's total clamping and
        // its never-split-a-pair rule.
        using qmn::lsp::PositionConverter;
        const QString text = QStringLiteral("ab\ncdé\U0001F600fg\nlast");
        // offsets: a0 b1 \n2 | c3 d4 é5 [emoji 6,7] f8 g9 \n10 | l11..t14
        // line 1 is "cdé😀fg": 7 utf-16 units, 6 utf-32 units, 10 utf-8 bytes.
        // Its utf-16 COLS are c0 d1 é2 emoji3-4 f5 g6 (end = 7).
        const auto m16 = [&](int line, int character) {
            return PositionConverter::toModelPosition(
                text, line, character, PositionConverter::Encoding::Utf16);
        };
        const auto m8 = [&](int line, int character) {
            return PositionConverter::toModelPosition(
                text, line, character, PositionConverter::Encoding::Utf8);
        };
        const auto m32 = [&](int line, int character) {
            return PositionConverter::toModelPosition(
                text, line, character, PositionConverter::Encoding::Utf32);
        };
        QCOMPARE(m16(0, 0).line, 0);
        QCOMPARE(m16(0, 0).col, 0);
        QCOMPARE(m16(1, 0).line, 1); // a line's start stays on ITS line
        QCOMPARE(m16(1, 0).col, 0);
        QCOMPARE(m16(1, 3).col, 3); // before the emoji's high surrogate:
                                    // utf-16 counts code units
        QCOMPARE(m16(1, 6).col, 6); // the last unit of the line ('g')
        QCOMPARE(m16(1, 7).col, 7); // the line's end position
        QCOMPARE(m16(1, 99).col, 7); // character past end-of-line: clamped
        QCOMPARE(m16(1, 99).line, 1);
        QCOMPARE(m16(99, 99).line, 2); // line past EOF: the last line...
        QCOMPARE(m16(99, 99).col, 4); // ...at its end ("last")
        QCOMPARE(m16(-5, -5).line, 0);
        QCOMPARE(m16(-5, -5).col, 0);

        // utf-8 frame of the same line: c0 d1 é2-3 emoji4-7 f8 g9 (end 10) ->
        // character 4 is the boundary AFTER é (col 3); a character INSIDE é's
        // second byte walks back to é's start (col 2).
        QCOMPARE(m8(1, 4).line, 1);
        QCOMPARE(m8(1, 4).col, 3);
        QCOMPARE(m8(1, 3).col, 2); // split é: the boundary BEFORE it
        QCOMPARE(m8(1, 9).col, 6); // character 9 = 'g': é(2)+emoji(4) bytes
        QCOMPARE(m8(1, 10).col, 7); // the end position
        QCOMPARE(m8(1, 99).col, 7);

        // utf-32 frame: c0 d1 é2 emoji3 f4 g5 (end 6) — the pair is ONE unit,
        // so é sits at character 2 and the emoji at 3.
        QCOMPARE(m32(1, 4).col, 5); // 'f'
        QCOMPARE(m32(1, 3).col, 3); // the emoji's start
        QCOMPARE(m32(1, 6).col, 7); // the end position
        QCOMPARE(m32(1, 99).col, 7);
    }

    void hubLazyStartsAndFeedsTheLiveEditStream()
    {
        // THE machine-4 contract: the view's REAL edit stream (CodeDocument::
        // editApplied, PRE-OP offsets, undo included) drives the server model,
        // and no server process exists until a document claims its language.
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16")});
        h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
        h.hub.setDebounceMs(5000); // explicit flush() below owns the versions

        QVERIFY(h.hub.clientForSpec(QStringLiteral("pysrv")) == nullptr);
        QCOMPARE(h.spawnCount, 0); // LAZY: nothing spawned before an open
        QCOMPARE(h.hub.specNameForLanguage(QStringLiteral("python")),
                 QStringLiteral("pysrv"));

        qmn::view::CodeEditor editor;
        editor.setLanguage(QStringLiteral("python"));
        editor.setPlainText(QStringLiteral("one\ntwo\nthree"));
        const QString uri = QStringLiteral("file:///hub-a.py");
        QVERIFY(h.hub.openDocument(&editor, uri, QStringLiteral("python")));
        QCOMPARE(h.spawnCount, 1); // openDocument IS the start signal
        auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
        QVERIFY(client);
        QCOMPARE(h.hub.clientForUri(uri), client);
        QVERIFY(waitReady(client));
        QCOMPARE(client->positionEncoding(), QStringLiteral("utf-16"));

        auto* syncer = h.hub.syncerFor(&editor);
        QVERIFY(syncer);
        QVERIFY(syncer->isOpen());
        QCOMPARE(syncer->uri(), uri);
        QJsonObject verdict
            = assertSyncModel(client, editor.toPlainText(), 0);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict))); // didOpen carried the text

        // The feed goes through the EDITOR's own doors — the surrogate-safe
        // paths a real keystroke takes. The three-path invariant of machine 3
        // with the view's model in place of the hand-written oracle: the
        // syncer mirror tracks the editor's text after EVERY primitive.
        auto mirrorFollowsEditor = [&] {
            QCOMPARE(syncer->mirrorText(), editor.toPlainText());
        };
        editor.insertTextAt(qmn::view::TextPosition(0, 0), QStringLiteral("X"));
        mirrorFollowsEditor();
        editor.insertTextAt(qmn::view::TextPosition(1, 3), QStringLiteral("2"));
        mirrorFollowsEditor();
        editor.insertTextAt(qmn::view::TextPosition(2, 5), QStringLiteral("!"));
        mirrorFollowsEditor();
        editor.undo(); // the edit stream is the truth, not the keystrokes:
        mirrorFollowsEditor(); // undo feeds didChange exactly like a delete
        editor.undo();
        mirrorFollowsEditor();
        editor.redo();
        mirrorFollowsEditor();
        editor.insertTextAt(qmn::view::TextPosition(1, 0),
                            QStringLiteral("\nmid\n")); // the newline-carrying
        mirrorFollowsEditor(); // multi-line delta shape
        syncer->flush(); // the whole burst travels as ONE didChange
        QCOMPARE(syncer->version(), qint64(1));
        verdict = assertSyncModel(client, uri, editor.toPlainText(), 1);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));

        // saveDocument: flush-then-didSave (the server is current BEFORE the
        // save), then the document's modified flag (v1's only save door).
        editor.insertTextAt(qmn::view::TextPosition(0, 1), QStringLiteral("Y"));
        QVERIFY(editor.document()->isModified());
        h.hub.saveDocument(&editor);
        QCOMPARE(syncer->version(), qint64(2)); // the save's own flush
        verdict = assertSyncModel(client, uri, editor.toPlainText(), 2);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        QCOMPARE(verdict.value(QStringLiteral("saves")).toInt(), 1);
        QVERIFY(!editor.document()->isModified());

        // closeDocument: didClose + release (the syncer door goes null, the
        // server's document table goes empty).
        h.hub.closeDocument(&editor);
        QVERIFY(h.hub.syncerFor(&editor) == nullptr);
        QVERIFY(h.hub.clientForUri(uri) == nullptr);
        QCOMPARE(serverDocumentCount(client), 0);

        // An editor that is destroyed takes its wiring with it (the destroyed()
        // hook releases the registration; the server still sees didClose).
        {
            auto* transient = new qmn::view::CodeEditor;
            transient->setPlainText(QStringLiteral("gone"));
            QVERIFY(h.hub.openDocument(
                transient, QStringLiteral("file:///hub-gone.py"),
                QStringLiteral("python")));
            QCOMPARE(serverDocumentCount(client), 1);
            delete transient;
        }
        QCOMPARE(serverDocumentCount(client), 0);
        h.hub.restartServer(QStringLiteral("pysrv")); // Ready already: the
        QVERIFY(waitReady(client));                   // door is a no-op there
    }

    void hubDiagnosticsDecodeTheNegotiatedFrame()
    {
        // The decoder tripwire: the fakeserver publishes its ranges through
        // its OWN encoder in the NEGOTIATED frame (utf-8, clangd's real one),
        // one marker word per wire severity. The hub must land every range on
        // the view's utf-16 columns and every severity on the view's enum.
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-8"),
                                 QStringLiteral("diags")});
        h.installSpec(QStringLiteral("pydiag"), {QStringLiteral("python")});

        qmn::view::CodeEditor editor;
        editor.setLanguage(QStringLiteral("python"));
        // "é" is 2 utf-8 bytes / 1 utf-16 unit, the emoji 4 bytes / 2 units:
        // the marker on line 0 sits at wire character 7 but MODEL column 4 —
        // a decoder that trusts the wire number paints three columns late.
        editor.setPlainText(QStringLiteral("\u00e9\U0001F600 bug\n"
                                          "warn x\nnote\nhint\n"));
        QVERIFY(h.hub.openDocument(&editor, QStringLiteral("file:///diag.py"),
                                   QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("pydiag"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        QCOMPARE(client->positionEncoding(), QStringLiteral("utf-8"));

        auto dumpDiags = [&editor] {
            QString out;
            for (const auto& d : editor.diagnostics())
                out += QStringLiteral("[%1:%2-%3:%4 sev%5 \"%6\"] ")
                           .arg(d.startLine)
                           .arg(d.startCol)
                           .arg(d.endLine)
                           .arg(d.endCol)
                           .arg(int(d.severity))
                           .arg(d.message);
            return out;
        };
        QVERIFY2(waitUntil([&editor] { return editor.diagnostics().size() == 4; }),
                 qPrintable(dumpDiags()));

        using Diag = qmn::view::CodeEditor::Diagnostic;
        using Severity = qmn::view::CodeEditor::Severity;
        const QVector<Diag> expected{
            {0, 4, 0, 7, Severity::Error, QStringLiteral("bug at line 0")},
            {1, 0, 1, 4, Severity::Warning, QStringLiteral("warn at line 1")},
            {2, 0, 2, 4, Severity::Information, QStringLiteral("note at line 2")},
            {3, 0, 3, 4, Severity::Hint, QStringLiteral("hint at line 3")},
        };
        QVERIFY2(editor.diagnostics() == expected, qPrintable(dumpDiags()));

        // The clear signal: an empty array means "no more problems" (a decoder
        // that ignored the empty list would leave stale waves on screen).
        editor.setPlainText(QStringLiteral("clean text, no markers"));
        QVERIFY2(waitUntil([&editor] { return editor.diagnostics().isEmpty(); }),
                 qPrintable(dumpDiags()));
        // And the round trip back: a marker re-typed is published again.
        editor.insertTextAt(qmn::view::TextPosition(0, 5),
                            QStringLiteral(" note"));
        QVERIFY2(waitUntil([&editor] { return editor.diagnostics().size() == 1; }),
                 qPrintable(dumpDiags()));
        const auto only = editor.diagnostics();
        QCOMPARE(only.at(0).startLine, 0);
        QCOMPARE(only.at(0).startCol, 6); // "clean" + " " -> 'n' of the
        QCOMPARE(only.at(0).endCol, 10);  // inserted " note" at col 5
        QCOMPARE(only.at(0).message, QStringLiteral("note at line 0"));
        QCOMPARE(int(only.at(0).severity), int(Severity::Information));
    }

    void hubCarriesManyDocumentsOnOneServer()
    {
        // PLAN's shape: ONE client per server spec carrying MANY documents
        // (the VS Code shape), each with its own syncer and version counter.
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16")});
        h.installSpec(QStringLiteral("multi"), {QStringLiteral("python")});

        qmn::view::CodeEditor alpha;
        qmn::view::CodeEditor beta;
        alpha.setPlainText(QStringLiteral("alpha"));
        beta.setPlainText(QStringLiteral("beta\ncolorado"));
        const QString uriA = QStringLiteral("file:///m1.py");
        const QString uriB = QStringLiteral("file:///m2.py");
        QVERIFY(h.hub.openDocument(&alpha, uriA, QStringLiteral("python")));
        QVERIFY(h.hub.openDocument(&beta, uriB, QStringLiteral("python")));
        QCOMPARE(h.spawnCount, 1); // ONE process for two documents
        auto* client = h.hub.clientForSpec(QStringLiteral("multi"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        QCOMPARE(serverDocumentCount(client), 2);
        QCOMPARE(h.hub.clientForUri(uriB), client);

        auto* syncerA = h.hub.syncerFor(&alpha);
        auto* syncerB = h.hub.syncerFor(&beta);
        QVERIFY(syncerA != syncerB);
        QVERIFY(syncerA && syncerB);
        // One verdict door per subject (the dump only formats on a fail).
        auto modelOk = [&](const QString& whichUri, const QString& text,
                           qint64 version) {
            const QJsonObject verdict
                = assertSyncModel(client, whichUri, text, version);
            QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                     qPrintable(modelDump(verdict)));
        };
        modelOk(uriA, alpha.toPlainText(), 0);
        modelOk(uriB, beta.toPlainText(), 0);

        // Editing A must not disturb B (per-uri accounting on both sides).
        alpha.insertTextAt(qmn::view::TextPosition(0, 0), QStringLiteral("Q"));
        syncerA->flush();
        QCOMPARE(syncerA->version(), qint64(1));
        QCOMPARE(syncerB->version(), qint64(0));
        modelOk(uriA, alpha.toPlainText(), 1);
        modelOk(uriB, QStringLiteral("beta\ncolorado"), 0);

        // Re-opening the same uri is a clean didClose + didOpen pair (the
        // fakeserver exits 2 on the reverse order, so a wrong order never
        // answers and the count pin below fails honestly).
        h.hub.openDocument(&alpha, uriA, QStringLiteral("python"));
        QCOMPARE(serverDocumentCount(client), 2);
        h.hub.closeDocument(&alpha);
        QCOMPARE(serverDocumentCount(client), 1);
        QVERIFY(h.hub.syncerFor(&alpha) == nullptr);
        QVERIFY(h.hub.syncerFor(&beta) != nullptr);
        h.hub.closeDocument(&beta);
        QCOMPARE(serverDocumentCount(client), 0);

        // A language no spec claims (a plain .txt tab): nothing registered, no
        // spawn, no syncer — the honest no-op, not a half-wired document.
        qmn::view::CodeEditor plain;
        plain.setPlainText(QStringLiteral("just text"));
        QVERIFY(!h.hub.openDocument(&plain, QStringLiteral("file:///notes.txt"),
                                    QStringLiteral("plaintext")));
        QVERIFY(h.hub.syncerFor(&plain) == nullptr);
        QCOMPARE(h.spawnCount, 1); // still the one process
        QCOMPARE(h.hub.specNameForLanguage(QStringLiteral("plaintext")),
                 QString());
    }

    void hubConfigFileDeepMergesTheServerMatrix()
    {
        // PLAN's config contract: <workspaceRoot>/.qmn/lsp.json, OBJECTS
        // RECURSE, ARRAYS REPLACE whole, and a file that is not an object is
        // refused WHOLE (never a partial merge).
        QTemporaryDir root;
        QVERIFY(root.isValid());
        qmn::app::LspHub hub;
        hub.setWorkspaceRoot(root.path());
        QCOMPARE(hub.workspaceRoot(), root.path());

        // The built-in matrix: clangd only (a built-in the CI cannot verify is
        // a broken default), and the matrix keys are LANGUAGE IDS, never file
        // suffixes (the app owns that mapping, MainWindow::languageForSuffix).
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("cpp")),
                 QStringLiteral("clangd"));
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("C")),
                 QStringLiteral("clangd")); // ids match case-insensitively
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("py")), QString());
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("python")), QString());
        QVERIFY(!hub.loadConfig()); // no file: silent, the built-ins stand

        QVERIFY(QDir(root.path()).mkpath(QStringLiteral(".qmn")));
        QFile file(root.path() + QStringLiteral("/.qmn/lsp.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"servers\": {"
                   "\"pylsp\": {\"program\": \"pylsp\", \"args\": [\"-v\"],"
                   " \"languages\": [\"PYTHON\"],"
                   " \"initializeTemplate\": {\"initializationOptions\":"
                   " {\"plugins\": {\"jedi\": {\"enabled\": true}}}}},"
                   "\"clangd\": {\"args\": [\"--background-index\"],"
                   " \"languages\": [\"cpp\"],"
                   " \"initializeTemplate\": {\"initializationOptions\":"
                   " {\"index\": {\"background\": true}}}}"
                   "}}");
        file.close();
        QVERIFY(hub.loadConfig());

        // ARRAYS REPLACE whole: the patched clangd no longer claims "c".
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("c")), QString());
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("cpp")),
                 QStringLiteral("clangd"));
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("python")),
                 QStringLiteral("pylsp")); // new spec, ids lowercased
        const QJsonObject clangd
            = hub.serverSpec(QStringLiteral("clangd"));
        QCOMPARE(clangd.value(QStringLiteral("args")).toArray(),
                 QJsonArray{QStringLiteral("--background-index")});
        QCOMPARE(clangd.value(QStringLiteral("languages")).toArray(),
                 QJsonArray{QStringLiteral("cpp")});
        // OBJECTS RECURSE: the built-in's initializationOptions key survived
        // the config's added key, and the config's nested object came through.
        const QJsonObject clangdInit
            = clangd.value(QStringLiteral("initializeTemplate"))
                  .toObject()
                  .value(QStringLiteral("initializationOptions"))
                  .toObject();
        QVERIFY(clangdInit.value(QStringLiteral("clangdFileStatus")).toBool());
        QVERIFY(clangdInit.value(QStringLiteral("index"))
                    .toObject()
                    .value(QStringLiteral("background"))
                    .toBool());
        const QJsonObject pylsp = hub.serverSpec(QStringLiteral("pylsp"));
        QCOMPARE(pylsp.value(QStringLiteral("program")).toString(),
                 QStringLiteral("pylsp"));
        QCOMPARE(pylsp.value(QStringLiteral("args")).toArray(),
                 QJsonArray{QStringLiteral("-v")});
        QCOMPARE(pylsp.value(QStringLiteral("languages")).toArray(),
                 QJsonArray{QStringLiteral("python")});

        // A malformed config is refused WHOLE, never half-merged, and the
        // matrix that worked before keeps standing.
        QFile broken(root.path() + QStringLiteral("/.qmn/lsp.json"));
        QVERIFY(broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
        broken.write("[1,2,3]");
        broken.close();
        QVERIFY(!hub.loadConfig());
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("python")),
                 QStringLiteral("pylsp"));
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("c")), QString());
        // The body door with an empty "servers": false, nothing changed.
        QVERIFY(!hub.loadConfigFromJson(
            QJsonObject{{QStringLiteral("servers"), QJsonObject{}}}));
    }

    void hubBuildsInitializeParamsAndExpandsWorkspaceRoot()
    {
        // NO setTransportFactory in this slot on purpose: the DEFAULT
        // ProcessTransport spawn is what every real user rides, and a
        // ${WORKSPACE_ROOT} that only LOOKED expanded would never start.
        const QString serverPath = QString::fromLatin1(QMN_TEST_LSP_SERVER);
        const QString wsroot = QFileInfo(serverPath).absolutePath(); // holds
                                                                    // fakeserver
        qmn::app::LspHub hub;
        hub.setWorkspaceRoot(wsroot);
        QVERIFY(hub.loadConfigFromJson(QJsonObject{
            {QStringLiteral("servers"),
             QJsonObject{{QStringLiteral("clangd"),
                          QJsonObject{
                              {QStringLiteral("program"),
                               QStringLiteral(
                                   "${WORKSPACE_ROOT}/fakeserver")},
                              {QStringLiteral("args"),
                               QJsonArray{QStringLiteral("sync"),
                                          QStringLiteral("utf-16")}},
                              {QStringLiteral("initializeTemplate"),
                               QJsonObject{
                                   {QStringLiteral("initializationOptions"),
                                    QJsonObject{
                                        {QStringLiteral("probe"),
                                         QStringLiteral(
                                             "${WORKSPACE_ROOT}/probe")}}}}}}}}}}));
        // The config patched program/args ONLY: the built-in's language list
        // and its initializationOptions both survived the deep merge.
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("cpp")),
                 QStringLiteral("clangd"));
        QCOMPARE(hub.specNameForLanguage(QStringLiteral("c")),
                 QStringLiteral("clangd"));

        qmn::view::CodeEditor editor;
        editor.setLanguage(QStringLiteral("cpp"));
        editor.setPlainText(QStringLiteral("int main(){}"));
        const QString uri = QStringLiteral("file:///m.cpp");
        QVERIFY(hub.openDocument(&editor, uri, QStringLiteral("cpp")));
        auto* client = hub.clientForSpec(QStringLiteral("clangd"));
        QVERIFY(client);
        QVERIFY2(waitReady(client),
                 qPrintable(QStringLiteral("client state %1, spawn never came")
                                .arg(int(client->state()))));
        const QJsonObject params = serverInitializeParams(client);
        const QUrl rootUrl = QUrl::fromLocalFile(wsroot);
        QCOMPARE(params.value(QStringLiteral("rootUri")).toString(),
                 rootUrl.toString());
        const QJsonArray folders
            = params.value(QStringLiteral("workspaceFolders")).toArray();
        QCOMPARE(int(folders.size()), 1);
        QCOMPARE(folders.at(0).toObject()
                     .value(QStringLiteral("uri"))
                     .toString(),
                 rootUrl.toString());
        QCOMPARE(params.value(QStringLiteral("processId")).toDouble(),
                 double(QCoreApplication::applicationPid()));
        // The sync capabilities machine 4 really exercises, on the wire.
        const QJsonObject sync
            = params.value(QStringLiteral("capabilities"))
                  .toObject()
                  .value(QStringLiteral("textDocument"))
                  .toObject()
                  .value(QStringLiteral("synchronization"))
                  .toObject();
        QCOMPARE(sync.value(QStringLiteral("didSave")).toBool(), true);
        QCOMPARE(sync.value(QStringLiteral("willSave")).toBool(), false);
        QCOMPARE(sync.value(QStringLiteral("willSaveWaitUntil")).toBool(),
                 false);
        // The template rode ON TOP of the built params: the built-in's key and
        // the config's key side by side, with ${WORKSPACE_ROOT} expanded on
        // the wire (the merge rule applied a second time, template over
        // built-ins).
        const QJsonObject initOptions
            = params.value(QStringLiteral("initializationOptions")).toObject();
        QVERIFY(initOptions.value(QStringLiteral("clangdFileStatus")).toBool());
        QCOMPARE(initOptions.value(QStringLiteral("probe")).toString(),
                 wsroot + QStringLiteral("/probe"));
        QCOMPARE(client->serverCapabilities()
                     .contains(QStringLiteral("hoverProvider")),
                 true);
        QCOMPARE(hub.clientForUri(uri), client);
    }

    void hubCrashFailedThenManualRestartSelfHeals()
    {
        // PLAN's lifecycle end to end: a dying server gets the client's ONE
        // automatic restart, then FAILED — and FAILED STAYS failed (a newly
        // opened document must never silently spawn another process) until the
        // app's restartServer door, whose ready() makes EVERY open document
        // re-didOpen its mirror whole.
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16")});
        h.crashSpawnCount = 2; // sessions #0 and #1 die after the handshake
        h.installSpec(QStringLiteral("crashy"), {QStringLiteral("python")});
        QSignalSpy failedSpy(&h.hub, &qmn::app::LspHub::serverFailed);

        qmn::view::CodeEditor first;
        first.setPlainText(QStringLiteral("first text"));
        qmn::view::CodeEditor second;
        second.setPlainText(QStringLiteral("second text"));
        const QString uriA = QStringLiteral("file:///c1.py");
        const QString uriB = QStringLiteral("file:///c2.py");
        QVERIFY(h.hub.openDocument(&first, uriA, QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("crashy"));
        QVERIFY(client);
        QSignalSpy readySpy(client, &qmn::lsp::LspClient::ready);
        QVERIFY2(waitUntil([&] { return readySpy.count() >= 1; }, 20000),
                 qPrintable(QStringLiteral("no handshake, state %1")
                                .arg(int(client->state()))));
        QVERIFY(h.hub.openDocument(&second, uriB, QStringLiteral("python")));
        QCOMPARE(h.spawnCount, 1); // the second document rode the SAME session

        QVERIFY2(waitUntil([&] { return failedSpy.count() == 1; }, 25000),
                 qPrintable(QStringLiteral("never failed after %1 spawns")
                                .arg(h.spawnCount)));
        QCOMPARE(failedSpy.at(0).at(0).toString(), QStringLiteral("crashy"));
        QVERIFY2(failedSpy.at(0).at(1).toString().size() > 0,
                 "the failed() relay carries a reason");
        QCOMPARE(client->state(), qmn::lsp::LspClient::State::Failed);
        QCOMPARE(h.spawnCount, 2);           // the budget is EXACTLY one restart
        QCOMPARE(readySpy.count(), 2);       // two handshakes, no third
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(0), 7, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.exitCodeOf(1), 7, 5000);

        // FAILED stays FAILED: a THIRD document opens into the dead spec — the
        // registration is real, but no spawn and no wire traffic happen.
        qmn::view::CodeEditor third;
        third.setPlainText(QStringLiteral("third text"));
        const QString uriC = QStringLiteral("file:///c3.py");
        QVERIFY(h.hub.openDocument(&third, uriC, QStringLiteral("python")));
        QCOMPARE(h.spawnCount, 2); // no silent retry: the app decides
        QVERIFY(h.hub.syncerFor(&third) != nullptr);
        QVERIFY(!h.hub.syncerFor(&third)->isOpen()
                || client->state() == qmn::lsp::LspClient::State::Failed);

        // The app's restart door: a FRESH session (the factory hands a clean
        // spawn this time) and the syncers' ready() self-heal re-opens all
        // three documents whole, at version 0.
        h.hub.restartServer(QStringLiteral("crashy"));
        QVERIFY2(waitReady(client),
                 qPrintable(QStringLiteral("restart never came, state %1")
                                .arg(int(client->state()))));
        QCOMPARE(h.spawnCount, 3);
        QCOMPARE(readySpy.count(), 3);
        QCOMPARE(serverDocumentCount(client), 3);
        auto modelOk = [&](const QString& whichUri, const QString& text) {
            const QJsonObject verdict
                = assertSyncModel(client, whichUri, text, 0);
            QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                     qPrintable(modelDump(verdict)));
        };
        modelOk(uriA, first.toPlainText());
        modelOk(uriB, second.toPlainText());
        modelOk(uriC, third.toPlainText());
        // ...and the live feed works again on the rebuilt session.
        auto* syncerC = h.hub.syncerFor(&third);
        third.insertTextAt(qmn::view::TextPosition(0, 0), QStringLiteral(">"));
        QCOMPARE(syncerC->mirrorText(), third.toPlainText());
        syncerC->flush();
        QCOMPARE(syncerC->version(), qint64(1));
        const QJsonObject verdict
            = assertSyncModel(client, uriC, third.toPlainText(), 1);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
    }

    // ------------------------------------------------- M5: the write path
    // Machine 5's contract: the feature doors' FRAMES (what leaves the hub),
    // the WorkspaceEdit decoder (headless, no session), and the applier —
    // ONE undo group per document, all-or-nothing refusals, and the
    // server-primed applyEdit round trip the fakeserver judges from ITS side.

    // The decoder is a pure function over a QJsonValue (QtCore-only layer):
    // no hub, no server, no view. Every shape, every refusal reason the
    // applier will quote, and the pickiness that keeps the applier from ever
    // guessing. (Every literal here is built one insert() at a time: a
    // 6-deep brace-init list is how a brace-count bug is born.)
    void workspaceEditDecoderWalksAllFourShapesAndReportsProblems()
    {
        using namespace qmn::lsp;
        const auto posObj = [](int line, int character) {
            return QJsonObject{{QStringLiteral("line"), line},
                               {QStringLiteral("character"), character}};
        };
        const auto rangeObj = [&](int sl, int sc, int el, int ec) {
            return QJsonObject{{QStringLiteral("start"), posObj(sl, sc)},
                               {QStringLiteral("end"), posObj(el, ec)}};
        };
        const auto editObj = [&](int sl, int sc, int el, int ec,
                                 const QString& text) {
            QJsonObject edit;
            edit.insert(QStringLiteral("range"), rangeObj(sl, sc, el, ec));
            edit.insert(QStringLiteral("newText"), text);
            return edit;
        };
        const auto changesOf = [](const QStringList& uris,
                                  const QVector<QJsonArray>& edits) {
            QJsonObject changes;
            for (int i = 0; i < uris.size(); ++i)
                changes.insert(uris.at(i), edits.at(i));
            return changes;
        };
        const auto editMap = [&](const QStringList& uris,
                                 const QVector<QJsonArray>& edits) {
            return QJsonObject{{QStringLiteral("changes"),
                                changesOf(uris, edits)}};
        };

        // Shape 1: the "changes" map. QMap-sorted key order is the
        // applier's deterministic document sequence, pinned here.
        {
            const DecodedWorkspaceEdit d = WorkspaceEditWire::decode(
                editMap({QStringLiteral("file:///b.py"),
                         QStringLiteral("file:///a.py")},
                        {QJsonArray{editObj(1, 0, 1, 2, QStringLiteral("B"))},
                         QJsonArray{editObj(0, 0, 0, 1, QStringLiteral("A"))}}));
            QVERIFY(d.wellFormed());
            QCOMPARE(int(d.textEdits.size()), 2);
            QCOMPARE(d.textEdits.at(0).uri,
                     QStringLiteral("file:///a.py")); // QMap order
            QCOMPARE(d.textEdits.at(0).endCol, 1);
            QCOMPARE(d.textEdits.at(0).documentVersion, -1); // map: unpinned
            QCOMPARE(d.textEdits.at(1).uri, QStringLiteral("file:///b.py"));
        }
        // Shapes 1+2 together: documentChanges land AFTER "changes" (the
        // spec's own ordering), and the version pin rides every record of a
        // versioned TextDocumentEdit...
        {
            QJsonObject doc;
            doc.insert(QStringLiteral("uri"), QStringLiteral("file:///b.py"));
            doc.insert(QStringLiteral("version"), 7);
            QJsonObject textEditEntry;
            textEditEntry.insert(QStringLiteral("textDocument"), doc);
            textEditEntry.insert(
                QStringLiteral("edits"),
                QJsonArray{editObj(2, 1, 2, 3, QStringLiteral("pinned"))});
            QJsonObject edit = editMap(
                {QStringLiteral("file:///a.py")},
                {QJsonArray{editObj(0, 0, 0, 0, QStringLiteral("first"))}});
            edit.insert(QStringLiteral("documentChanges"),
                        QJsonArray{textEditEntry});
            const DecodedWorkspaceEdit d = WorkspaceEditWire::decode(edit);
            QVERIFY(d.wellFormed());
            QCOMPARE(int(d.textEdits.size()), 2);
            QCOMPARE(d.textEdits.at(0).uri, QStringLiteral("file:///a.py"));
            QCOMPARE(d.textEdits.at(0).documentVersion, -1);
            QCOMPARE(d.textEdits.at(1).uri, QStringLiteral("file:///b.py"));
            QCOMPARE(d.textEdits.at(1).documentVersion, 7);
        }
        // ...file operations decode into the structure records the applier
        // refuses whole, and an "operations" edit (LSP 3.18) is v1-over.
        {
            QJsonArray entries;
            QJsonObject create;
            create.insert(QStringLiteral("kind"), QStringLiteral("create"));
            create.insert(QStringLiteral("uri"), QStringLiteral("n.py"));
            entries.append(create);
            QJsonObject rename;
            rename.insert(QStringLiteral("kind"), QStringLiteral("rename"));
            rename.insert(QStringLiteral("oldUri"), QStringLiteral("o.py"));
            rename.insert(QStringLiteral("newUri"), QStringLiteral("m.py"));
            entries.append(rename);
            QJsonObject del;
            del.insert(QStringLiteral("kind"), QStringLiteral("delete"));
            del.insert(QStringLiteral("uri"), QStringLiteral("d.py"));
            entries.append(del);
            QJsonObject unannotated; // versioned but "operations", not "edits"
            QJsonObject versionedDoc;
            versionedDoc.insert(QStringLiteral("uri"), QStringLiteral("p.py"));
            versionedDoc.insert(QStringLiteral("version"), 3);
            unannotated.insert(QStringLiteral("textDocument"), versionedDoc);
            unannotated.insert(QStringLiteral("operations"), QJsonArray{});
            entries.append(unannotated);
            QJsonObject plainDoc;
            plainDoc.insert(QStringLiteral("uri"), QStringLiteral("q.py"));
            QJsonObject plain; // NO version: the edits still apply, unpinned
            plain.insert(QStringLiteral("textDocument"), plainDoc);
            plain.insert(QStringLiteral("edits"),
                         QJsonArray{editObj(0, 0, 0, 0,
                                            QStringLiteral("q-edit"))});
            entries.append(plain);
            QJsonObject bogus;
            bogus.insert(QStringLiteral("kind"), QStringLiteral("frob"));
            entries.append(bogus);
            QJsonObject edit;
            edit.insert(QStringLiteral("documentChanges"), entries);

            const DecodedWorkspaceEdit d = WorkspaceEditWire::decode(edit);
            QCOMPARE(int(d.fileOperations.size()), 3);
            QCOMPARE(int(d.fileOperations.at(0).kind),
                     int(WireFileOperation::Kind::Create));
            QCOMPARE(d.fileOperations.at(0).newUri, QStringLiteral("n.py"));
            QCOMPARE(int(d.fileOperations.at(1).kind),
                     int(WireFileOperation::Kind::Rename));
            QCOMPARE(d.fileOperations.at(1).oldUri, QStringLiteral("o.py"));
            QCOMPARE(int(d.fileOperations.at(2).kind),
                     int(WireFileOperation::Kind::Delete));
            QCOMPARE(d.fileOperations.at(2).oldUri, QStringLiteral("d.py"));
            QVERIFY(d.fileOperations.at(2).newUri.isEmpty());
            QVERIFY(d.fileOperations.at(0).oldUri.isEmpty());
            QCOMPARE(int(d.textEdits.size()), 1); // "operations" dropped, q.py kept
            QCOMPARE(d.textEdits.at(0).uri, QStringLiteral("q.py"));
            QCOMPARE(d.textEdits.at(0).documentVersion, -1); // unversioned doc
            QCOMPARE(int(d.problems.size()), 2); // "operations" + frob
            const QStringList kinds = d.unsupportedKinds();
            QCOMPARE(int(kinds.size()), 3);
            QVERIFY(kinds.at(0).startsWith(QStringLiteral("create-file")));
            QVERIFY(kinds.at(1).startsWith(QStringLiteral("rename-file")));
            QVERIFY(kinds.at(2).startsWith(QStringLiteral("delete-file")));
        }
        // Picky legs: a truncated Position or a missing newText is a
        // PROBLEM, never a silent line-0 edit. A {} is the legal EMPTY edit;
        // a value that carries neither map is a problem.
        {
            QJsonObject truncatedRange; // "end" is simply gone
            truncatedRange.insert(QStringLiteral("start"), posObj(0, 0));
            QJsonObject truncated;
            truncated.insert(QStringLiteral("range"), truncatedRange);
            truncated.insert(QStringLiteral("newText"), QStringLiteral("x"));
            const DecodedWorkspaceEdit bad = WorkspaceEditWire::decode(
                editMap({QStringLiteral("a.py")}, {QJsonArray{truncated}}));
            QCOMPARE(int(bad.textEdits.size()), 0);
            QCOMPARE(int(bad.problems.size()), 1);
            QVERIFY(bad.problems.at(0).contains(QStringLiteral("Position pair")));

            QJsonObject noText; // a complete Range, no newText at all
            noText.insert(QStringLiteral("range"), rangeObj(0, 0, 0, 0));
            const DecodedWorkspaceEdit missing = WorkspaceEditWire::decode(
                editMap({QStringLiteral("a.py")}, {QJsonArray{noText}}));
            QCOMPARE(int(missing.problems.size()), 1);
            QVERIFY(missing.problems.at(0).contains(QStringLiteral("newText")));

            const DecodedWorkspaceEdit empty
                = WorkspaceEditWire::decode(QJsonObject{});
            QVERIFY(empty.wellFormed()); // {} = nothing to do, nothing refused
            QCOMPARE(int(empty.textEdits.size()), 0);

            QJsonObject unrelated;
            unrelated.insert(QStringLiteral("note"), QStringLiteral("hello"));
            const DecodedWorkspaceEdit neither = WorkspaceEditWire::decode(
                unrelated);
            QCOMPARE(int(neither.problems.size()), 1);
            QVERIFY(neither.problems.at(0).contains(QStringLiteral("neither")));

            const DecodedWorkspaceEdit notObject
                = WorkspaceEditWire::decode(QJsonValue(42));
            QCOMPARE(int(notObject.problems.size()), 1);
        }
        // Shape 3: the codeAction door. `edit` WINS over the command shape;
        // the command NAME is not matched (clangd tweak names change — the
        // payload carries the work); an action with neither is a problem.
        {
            QJsonObject both;
            both.insert(QStringLiteral("edit"),
                        editMap({QStringLiteral("e.py")},
                                {QJsonArray{editObj(0, 0, 0, 0,
                                                    QStringLiteral("from-edit"))}}));
            QJsonObject hiddenEdit = editMap(
                {QStringLiteral("c.py")},
                {QJsonArray{editObj(0, 0, 0, 0,
                                    QStringLiteral("from-command"))}});
            QJsonObject bothCommand;
            bothCommand.insert(QStringLiteral("title"), QStringLiteral("t"));
            bothCommand.insert(QStringLiteral("command"),
                               QStringLiteral("clangd.tweak"));
            bothCommand.insert(QStringLiteral("arguments"),
                               QJsonArray{hiddenEdit});
            both.insert(QStringLiteral("command"), bothCommand);
            const DecodedWorkspaceEdit wins
                = WorkspaceEditWire::decodeCodeAction(both);
            QCOMPARE(int(wins.textEdits.size()), 1);
            QCOMPARE(wins.textEdits.at(0).uri, QStringLiteral("e.py"));
            QCOMPARE(wins.textEdits.at(0).newText,
                     QStringLiteral("from-edit"));

            QJsonObject commandOnly;
            QJsonObject command;
            command.insert(QStringLiteral("arguments"), QJsonArray{hiddenEdit});
            commandOnly.insert(QStringLiteral("command"), command);
            const DecodedWorkspaceEdit commandShape
                = WorkspaceEditWire::decodeCodeAction(commandOnly);
            QVERIFY(commandShape.wellFormed());
            QCOMPARE(int(commandShape.textEdits.size()), 1);
            QCOMPARE(commandShape.textEdits.at(0).uri, QStringLiteral("c.py"));

            const DecodedWorkspaceEdit bare = WorkspaceEditWire::decodeCodeAction(
                QJsonObject{{QStringLiteral("title"),
                             QStringLiteral("nothing")}});
            QCOMPARE(int(bare.problems.size()), 1);
            QVERIFY(bare.problems.at(0).contains(QStringLiteral("neither")));
        }
        // Shape 4: the BARE TextEdit[] a formatting reply carries. null is
        // the spec's own "nothing changed" (legal, empty); a non-array is a
        // problem; entries get the CALLER's uri and no version pin.
        {
            const DecodedWorkspaceEdit nothing
                = WorkspaceEditWire::decodeTextEditList(
                    QJsonValue(QJsonValue::Null), QStringLiteral("f.py"));
            QVERIFY(nothing.wellFormed());
            QCOMPARE(int(nothing.textEdits.size()), 0);

            const DecodedWorkspaceEdit list = WorkspaceEditWire::decodeTextEditList(
                QJsonArray{editObj(0, 0, 0, 0, QStringLiteral("# head\n")),
                           editObj(2, 4, 2, 4, QStringLiteral(" // tail"))},
                QStringLiteral("f.py"));
            QVERIFY(list.wellFormed());
            QCOMPARE(int(list.textEdits.size()), 2);
            QCOMPARE(list.textEdits.at(1).uri, QStringLiteral("f.py"));
            QCOMPARE(list.textEdits.at(1).endLine, 2);
            QCOMPARE(list.textEdits.at(1).documentVersion, -1);

            const DecodedWorkspaceEdit notArray
                = WorkspaceEditWire::decodeTextEditList(
                    QJsonObject{}, QStringLiteral("f.py"));
            QCOMPARE(int(notArray.problems.size()), 1);
            QVERIFY(notArray.problems.at(0).contains(
                QStringLiteral("not an array")));

            const DecodedWorkspaceEdit nameless
                = WorkspaceEditWire::decodeTextEditList(
                    QJsonArray{editObj(0, 0, 0, 0, QStringLiteral("x"))},
                    QString());
            QCOMPARE(int(nameless.textEdits.size()), 0);
            QCOMPARE(int(nameless.problems.size()), 1);
            QVERIFY(nameless.problems.at(0).contains(
                QStringLiteral("names no document")));
        }
    }

    // The gates refuse LOCALLY with the codes a SERVER would have used: the
    // handler fires on the spot, the door returns 0, and NOTHING reaches the
    // wire. The features-mode fakeserver makes this observable from the
    // other side too: its hoverProvider is FALSE, so a hover request that
    // reached it would exit 2 and kill the session.
    void featureDoorsRefuseLocallyWhenNotRegisteredNoServerOrNoProvider()
    {
        using namespace qmn::lsp;

        { // an editor the hub never saw
            HubHarness h(QStringList{QStringLiteral("sync"),
                                     QStringLiteral("utf-16")});
            h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
            qmn::view::CodeEditor editor;
            QVERIFY(h.hub.syncerFor(&editor) == nullptr);
            FeatureProbe probe;
            QCOMPARE(h.hub.requestHover(&editor, qmn::view::TextPosition(0, 0),
                                        probe.handler()),
                     qint64(0));
            QVERIFY(probe.arrived);
            QVERIFY(probe.reply.isError);
            QCOMPARE(probe.reply.code,
                     int(LspClient::kWireServerNotInitialized));
        }
        { // a language no server spec claims: openDocument is the honest no-op
            HubHarness h(QStringList{QStringLiteral("sync"),
                                     QStringLiteral("utf-16")});
            h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
            qmn::view::CodeEditor editor;
            editor.setPlainText(QStringLiteral("no server for this one"));
            QVERIFY(!h.hub.openDocument(&editor,
                                        QStringLiteral("file:///m5-nospec.txt"),
                                        QStringLiteral("plaintext")));
            FeatureProbe probe;
            QCOMPARE(h.hub.requestDefinition(&editor,
                                             qmn::view::TextPosition(0, 0),
                                             probe.handler()),
                     qint64(0));
            QVERIFY(probe.arrived);
            QCOMPARE(probe.reply.code,
                     int(LspClient::kWireServerNotInitialized));
            QCOMPARE(h.spawnCount, 0); // and still nothing was ever spawned
        }
        { // a live session, but the capabilities do not offer this provider
            HubHarness h(QStringList{QStringLiteral("sync"),
                                     QStringLiteral("utf-16")});
            h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
            h.hub.setDebounceMs(5000);
            qmn::view::CodeEditor editor;
            editor.setLanguage(QStringLiteral("python"));
            editor.setPlainText(QStringLiteral("one\ntwo\nthree"));
            QVERIFY(h.hub.openDocument(&editor,
                                       QStringLiteral("file:///m5-gate.py"),
                                       QStringLiteral("python")));
            auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
            QVERIFY(client);
            QVERIFY(waitReady(client));
            // hoverProvider:true is the SYNC-mode set: the door WORKS (a
            // reply arrives, not a refusal)...
            FeatureProbe hover;
            QVERIFY(h.hub.requestHover(&editor, qmn::view::TextPosition(0, 1),
                                       hover.handler())
                    > 0);
            QVERIFY2(waitUntil([&hover] { return hover.arrived; }),
                     "plain-sync hover never got its echo reply");
            // ...while formatting and rename were never advertised: -32601.
            FeatureProbe formatting;
            QCOMPARE(h.hub.requestFormatting(&editor, formatting.handler()),
                     qint64(0));
            QVERIFY(formatting.arrived);
            QVERIFY(formatting.reply.isError);
            QCOMPARE(formatting.reply.code, -32601);
            FeatureProbe rename;
            QCOMPARE(h.hub.requestRename(&editor,
                                         qmn::view::TextPosition(0, 1),
                                         QStringLiteral("x"), rename.handler()),
                     qint64(0));
            QVERIFY(rename.arrived);
            QCOMPARE(rename.reply.code, -32601);
        }
        { // the features-mode set: hoverProvider FALSE (the hub must refuse
          // locally; the fakeserver exits 2 if anything reaches it) and a
          // CLOSED document has no live session at all
            HubHarness h(QStringList{QStringLiteral("sync"),
                                     QStringLiteral("utf-16"),
                                     QStringLiteral("features")});
            h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
            h.hub.setDebounceMs(5000);
            qmn::view::CodeEditor editor;
            editor.setLanguage(QStringLiteral("python"));
            editor.setPlainText(QStringLiteral("one\ntwo\nthree"));
            QVERIFY(h.hub.openDocument(&editor,
                                       QStringLiteral("file:///m5-off.py"),
                                       QStringLiteral("python")));
            auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
            QVERIFY(client);
            QVERIFY(waitReady(client));
            FeatureProbe hover;
            QCOMPARE(h.hub.requestHover(&editor, qmn::view::TextPosition(0, 1),
                                        hover.handler()),
                     qint64(0));
            QVERIFY(hover.arrived);
            QCOMPARE(hover.reply.code, -32601);
            h.hub.closeDocument(&editor);
            FeatureProbe def;
            QCOMPARE(h.hub.requestDefinition(&editor,
                                             qmn::view::TextPosition(0, 1),
                                             def.handler()),
                     qint64(0));
            QVERIFY(def.arrived);
            QCOMPARE(def.reply.code,
                     int(LspClient::kWireServerNotInitialized));
        }
    }

    // THE machine-5 frame pin: the hub builds wire Positions against the
    // syncer's MIRROR, and the fakeserver decodes them with its OWN frame
    // table and reports the offset it landed on. A one-unit encoder bug in
    // any negotiated encoding shows up as an off-target proof, never a
    // matching pair of the same bug.
    void featureFrameMatchesMirrorAcrossEncodings()
    {
        const QString text = QStringLiteral("one\ntwo\nx")
            + QString::fromUtf8("\xc3\xa9\xf0\x9f\x98\x80") // é + one astral
            + QStringLiteral("yz");
        const char* encodings[] = {"utf-16", "utf-8", "utf-32"};
        // model-frame truth, computed the naive way (QString indices):
        // model-frame truth (QString/utf-16 indices): "one" is 0..3, "two"
        // starts at 4, line 2 starts at 8 and is x(8) é(9) U+1F600(10,11)
        // y(12) z(13) -- col 4 lands just PAST the surrogate pair.
        const QVector<int> expectedOff{3, 5, 12};
        const QVector<qmn::view::TextPosition> carets{
            qmn::view::TextPosition(0, 3), qmn::view::TextPosition(1, 1),
            qmn::view::TextPosition(2, 4)};

        for (const char* enc : encodings) {
            HubHarness h(QStringList{QStringLiteral("sync"),
                                     QString::fromLatin1(enc),
                                     QStringLiteral("features")});
            h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
            h.hub.setDebounceMs(5000);
            qmn::view::CodeEditor editor;
            editor.setLanguage(QStringLiteral("python"));
            editor.setPlainText(text);
            const QString uri = QStringLiteral("file:///m5-frame-%1.py")
                                    .arg(QLatin1String(enc));
            QVERIFY(h.hub.openDocument(&editor, uri,
                                       QStringLiteral("python")));
            auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
            QVERIFY(client);
            QVERIFY2(waitReady(client), qPrintable(uri));
            QCOMPARE(client->positionEncoding(),
                     QString::fromLatin1(enc)); // the negotiated frame

            for (int i = 0; i < carets.size(); ++i) {
                FeatureProbe probe;
                QVERIFY(h.hub.requestCompletion(&editor, carets.at(i),
                                                probe.handler())
                        > 0);
                QVERIFY2(waitUntil([&probe] { return probe.arrived; }),
                         qPrintable(QStringLiteral("%1 completion %2 never "
                                                   "answered")
                                        .arg(QLatin1String(enc)).arg(i)));
                QVERIFY2(!probe.reply.isError,
                         qPrintable(QStringLiteral("%1 reply error: %2")
                                        .arg(QLatin1String(enc),
                                             probe.reply.message)));
                const QJsonArray items
                    = probe.reply.result.toObject()
                          .value(QStringLiteral("items")).toArray();
                bool sawOff = false;
                for (const QJsonValue& item : items) {
                    const QString label
                        = item.toObject().value(QStringLiteral("label"))
                              .toString();
                    if (!label.startsWith(QStringLiteral("off=")))
                        continue;
                    sawOff = true;
                    QCOMPARE(label.mid(4).toInt(), expectedOff.at(i));
                }
                QVERIFY2(sawOff, qPrintable(QStringLiteral(
                                      "%1 completion carried no offset proof")
                                                .arg(QLatin1String(enc))));
            }

            // The definition reply rides the SAME frame the other way: the
            // server reports its OWN decode as a Location, and decoding that
            // Location back through the hub's converter table must land on
            // the model word bounds exactly.
            FeatureProbe probe;
            QVERIFY(h.hub.requestDefinition(&editor,
                                            qmn::view::TextPosition(1, 1),
                                            probe.handler())
                    > 0);
            QVERIFY2(waitUntil([&probe] { return probe.arrived; }),
                     "definition never answered");
            QVERIFY(!probe.reply.isError);
            const QJsonArray locations = probe.reply.result.toArray();
            QCOMPARE(int(locations.size()), 1);
            QCOMPARE(locations.at(0).toObject().value(QStringLiteral("uri"))
                         .toString(),
                     uri);
            const QJsonObject range
                = locations.at(0).toObject().value(QStringLiteral("range"))
                      .toObject();
            const QString mirror = h.hub.syncerFor(&editor)->mirrorText();
            const auto encKind
                = qmn::lsp::PositionConverter::fromWire(
                    QString::fromLatin1(enc));
            const qmn::lsp::ModelPosition from
                = qmn::lsp::PositionConverter::toModelPosition(
                    mirror,
                    range.value(QStringLiteral("start"))
                        .toObject()
                        .value(QStringLiteral("line"))
                        .toInt(),
                    range.value(QStringLiteral("start"))
                        .toObject()
                        .value(QStringLiteral("character"))
                        .toInt(),
                    encKind);
            const qmn::lsp::ModelPosition to
                = qmn::lsp::PositionConverter::toModelPosition(
                    mirror,
                    range.value(QStringLiteral("end"))
                        .toObject()
                        .value(QStringLiteral("line"))
                        .toInt(),
                    range.value(QStringLiteral("end"))
                        .toObject()
                        .value(QStringLiteral("character"))
                        .toInt(),
                    encKind);
            QCOMPARE(int(from.line), 1); // the word "two"
            QCOMPARE(int(from.col), 0);
            QCOMPARE(int(to.line), 1);
            QCOMPARE(int(to.col), 3);
            const int fromOff = qmn::lsp::PositionConverter::fromModelPosition(
                mirror, from);
            const int toOff = qmn::lsp::PositionConverter::fromModelPosition(
                mirror, to);
            QCOMPARE(mirror.mid(fromOff, toOff - fromOff),
                     QStringLiteral("two"));
        }
    }

    // Completion is the ONE feature whose reply is a LIST OF DATA the app
    // hands straight back (resolve's params ARE the item), so the round trip
    // is part of the contract: what the list sent, the resolve must survive.
    void completionAndResolveRideTheItemRoundTrip()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("features")});
        h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
        h.hub.setDebounceMs(5000);
        const QString text = QStringLiteral("alpha\nbeta");
        qmn::view::CodeEditor editor;
        editor.setPlainText(text);
        QVERIFY(h.hub.openDocument(&editor,
                                   QStringLiteral("file:///m5-compl.py"),
                                   QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));

        // The Invoked form: triggerKind 1 and NO triggerCharacter (the
        // fakeserver exits 2 if a character rides kind 1). Its labels are the
        // frame pin (offset 2 in the mirror) plus the kind echo.
        FeatureProbe invoked;
        QVERIFY(h.hub.requestCompletion(&editor, qmn::view::TextPosition(0, 2),
                                        invoked.handler())
                > 0);
        QVERIFY2(waitUntil([&] { return invoked.arrived; }),
                 "Invoked completion never answered");
        QVERIFY(!invoked.reply.isError);
        const QJsonObject list = invoked.reply.result.toObject();
        QVERIFY(!list.value(QStringLiteral("isIncomplete")).toBool());
        const QJsonArray items = list.value(QStringLiteral("items")).toArray();
        QCOMPARE(int(items.size()), 3); // alpha, tk=1, off= — no tc= item
        QCOMPARE(items.at(0).toObject().value(QStringLiteral("label")).toString(),
                 QStringLiteral("alpha"));
        QCOMPARE(items.at(0).toObject().value(QStringLiteral("kind")).toInt(),
                 14);
        QVERIFY(items.at(0).toObject().value(QStringLiteral("data")).isObject());
        QCOMPARE(items.at(1).toObject().value(QStringLiteral("label")).toString(),
                 QStringLiteral("tk=1"));
        QCOMPARE(items.at(2).toObject().value(QStringLiteral("label")).toString(),
                 QStringLiteral("off=2"));

        // The trigger-character form: kind 2 carries the character, and the
        // server refuses a character it never advertised.
        FeatureProbe typed;
        QVERIFY(h.hub.requestCompletion(&editor, qmn::view::TextPosition(1, 1),
                                        typed.handler(),
                                        QStringLiteral("."))
                > 0);
        QVERIFY2(waitUntil([&] { return typed.arrived; }),
                 "trigger-character completion never answered");
        QVERIFY(!typed.reply.isError);
        const QJsonArray typedItems
            = typed.reply.result.toObject().value(QStringLiteral("items"))
                  .toArray();
        QCOMPARE(int(typedItems.size()), 4); // alpha, tk=2, tc=., off=
        QCOMPARE(typedItems.at(1).toObject().value(QStringLiteral("label"))
                     .toString(),
                 QStringLiteral("tk=2"));
        QCOMPARE(typedItems.at(2).toObject().value(QStringLiteral("label"))
                     .toString(),
                 QStringLiteral("tc=."));
        QCOMPARE(typedItems.at(3).toObject().value(QStringLiteral("label"))
                     .toString(),
                 QStringLiteral("off=7")); // line 1 starts at 6

        // completionItem/resolve: the app hands back the item the LIST sent,
        // and the server refuses an item that lost the "data" it was given —
        // so this round trip is pinned from both ends.
        const QJsonObject item = items.at(0).toObject();
        FeatureProbe resolved;
        QVERIFY(h.hub.resolveCompletionItem(&editor, item, resolved.handler())
                > 0);
        QVERIFY2(waitUntil([&] { return resolved.arrived; }),
                 "completionItem/resolve never answered");
        QVERIFY2(!resolved.reply.isError,
                 qPrintable(resolved.reply.message));
        const QJsonObject filled = resolved.reply.result.toObject();
        QCOMPARE(filled.value(QStringLiteral("label")).toString(),
                 QStringLiteral("alpha")); // the SAME item, filled in
        QCOMPARE(filled.value(QStringLiteral("kind")).toInt(), 14);
        QCOMPARE(filled.value(QStringLiteral("detail")).toString(),
                 QStringLiteral("resolved:alpha"));
        QCOMPARE(filled.value(QStringLiteral("documentation")).toString(),
                 QStringLiteral("docs for alpha"));

        // A feature REPLY is not an edit: the document did not move, and no
        // didChange burned a version on the way.
        QCOMPARE(editor.toPlainText(), text);
        QCOMPARE(h.hub.syncerFor(&editor)->version(), qint64(0));
    }

    // The NESTED capability gate: completionProvider WITHOUT resolveProvider
    // must still open the completion door, and must refuse the resolve door
    // LOCALLY — the fakeserver exits 2 if a resolve ever reaches it, so the
    // session being alive afterwards is the proof nothing went out.
    void nestedResolveRefusesLocallyWhenTheProviderOmitsIt()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("completiononly")});
        h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
        h.hub.setDebounceMs(5000);
        qmn::view::CodeEditor editor;
        editor.setPlainText(QStringLiteral("alpha\nbeta"));
        QVERIFY(h.hub.openDocument(&editor,
                                   QStringLiteral("file:///m5-resolve.py"),
                                   QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));

        const QJsonObject capabilities = client->serverCapabilities();
        QVERIFY(capabilities.contains(QStringLiteral("completionProvider")));
        QVERIFY(!capabilities.contains(QStringLiteral("definitionProvider")));
        QVERIFY(!capabilities.value(QStringLiteral("completionProvider"))
                     .toObject()
                     .contains(QStringLiteral("resolveProvider")));

        FeatureProbe completion;
        QVERIFY(h.hub.requestCompletion(&editor, qmn::view::TextPosition(0, 0),
                                        completion.handler())
                > 0);
        QVERIFY2(waitUntil([&] { return completion.arrived; }),
                 "the advertised half of completionProvider never answered");
        QVERIFY(!completion.reply.isError);

        // The un-advertised nested half: refused on the spot, code -32601, id
        // 0 (nothing went out to wait for).
        const QJsonObject item
            = completion.reply.result.toObject()
                  .value(QStringLiteral("items"))
                  .toArray()
                  .at(0)
                  .toObject();
        FeatureProbe resolve;
        QCOMPARE(h.hub.resolveCompletionItem(&editor, item, resolve.handler()),
                 qint64(0));
        QVERIFY(resolve.arrived);
        QVERIFY(resolve.reply.isError);
        QCOMPARE(resolve.reply.code, -32601);
        QVERIFY(resolve.reply.message.contains(
            QStringLiteral("completionProvider.resolveProvider")));

        // The tripwire's other half: the server is still standing, still
        // Ready, still holding this document open — a resolve that had
        // reached it would have killed the session (exit 2).
        QVERIFY(client->isReady());
        QCOMPARE(serverDocumentCount(client), 1);
    }

    // Formatting answers a BARE TextEdit[] — no uri, no version — so the
    // document comes from the EDITOR the app asked about. The applier door's
    // whole chain: the options pin (SERVER-side, exit 2), the shape, the
    // bottom-up walk, and the one undo group that reverses both edits at
    // once.
    void formattingReplyAppliesAsOneUndoGroup()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("features")});
        h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
        h.hub.setDebounceMs(5000);
        const QString text = QStringLiteral("alpha\nbeta\ngamma");
        const QString uri = QStringLiteral("file:///m5-fmt.py");
        qmn::view::CodeEditor editor;
        editor.setPlainText(text);

        // A bare TextEdit[] names no document unless the hub knows the
        // editor: refused with a reason, never applied to "the last document".
        qmn::view::CodeEditor stranger;
        stranger.setPlainText(QStringLiteral("never registered"));
        const qmn::app::LspHub::ApplyReport homeless
            = h.hub.applyTextEditList(&stranger, QJsonArray{});
        QVERIFY(!homeless.applied);
        QCOMPARE(int(homeless.refused.size()), 1);
        QVERIFY(homeless.refused.at(0).contains(
            QStringLiteral("not registered with the hub")));

        QVERIFY(h.hub.openDocument(&editor, uri, QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        // The pump discipline: every didChange is the TEST's own flush. The
        // debounce is raised and the typing cap above the drill's length, so
        // no unsolicited sync can jump the version pins below.
        qmn::lsp::DocumentSyncer* syncer = h.hub.syncerFor(&editor);
        QVERIFY(syncer);
        syncer->setDebounceMs(5000);
        syncer->setTypingCapMs(20000);

        FeatureProbe fmt;
        QVERIFY(h.hub.requestFormatting(&editor, fmt.handler()) > 0);
        QVERIFY2(waitUntil([&] { return fmt.arrived; }),
                 "formatting never answered");
        QVERIFY2(!fmt.reply.isError, qPrintable(fmt.reply.message));
        QVERIFY2(fmt.reply.result.isArray(),
                 "the formatting result is not the spec's bare TextEdit[]");
        QCOMPARE(int(fmt.reply.result.toArray().size()), 2);

        // The spec's own "nothing changed": a null reply is the legal EMPTY
        // list — applied, with nothing to do.
        const qmn::app::LspHub::ApplyReport nothing
            = h.hub.applyTextEditList(&editor, QJsonValue(QJsonValue::Null),
                                      client);
        QVERIFY(nothing.applied);
        QCOMPARE(nothing.documentsChanged, 0);
        QCOMPARE(nothing.editsApplied, 0);
        QVERIFY(nothing.refused.isEmpty());
        QCOMPARE(editor.toPlainText(), text);

        const qmn::app::LspHub::ApplyReport report
            = h.hub.applyTextEditList(&editor, fmt.reply.result, client);
        QVERIFY2(report.applied,
                 qPrintable(QStringLiteral("formatting refused: %1")
                                .arg(report.refused.join(QLatin1String("; ")))));
        QCOMPARE(report.documentsChanged, 1);
        QCOMPARE(report.editsApplied, 2);
        QVERIFY(report.refused.isEmpty());
        const QString formatted = QStringLiteral("# fmt\n") + text
            + QStringLiteral(" // fmt");
        QCOMPARE(editor.toPlainText(), formatted);

        // ONE undo group: the whole format is one user action, and the redo
        // replays it as one.
        editor.undo();
        QCOMPARE(editor.toPlainText(), text);
        QVERIFY(!editor.canUndo()); // the format was the ONLY step in the stack
        QVERIFY(editor.canRedo());
        editor.redo();
        QCOMPARE(editor.toPlainText(), formatted);
        editor.undo();

        // The mirror followed: the applier's endServerEdit full sync carried
        // the formatted text (v1) — asked BEFORE the undo's deltas go out, so
        // the stream's own ordering IS the synchronization — and the undo's
        // delta stream carried the original back (v2). The server's own model
        // verdicts, not our mirror.
        QJsonObject verdict = assertSyncModel(client, uri, formatted, 1);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        syncer->flush();
        verdict = assertSyncModel(client, uri, text, 2);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
    }

    // The codeAction door, end to end: the request frame (zero-width range +
    // the hub's OWN decoded diagnostics re-encoded into context), then the
    // applier walking the four shapes the server answers — plain `edit`,
    // clangd's command payload, a structure edit v1 refuses whole, and an
    // action that carries nothing to apply.
    void codeActionResultEditsApplyThroughTheApplier()
    {
        HubHarness h(QStringList{QStringLiteral("sync"), QStringLiteral("utf-16"),
                                 QStringLiteral("features"),
                                 QStringLiteral("diags")});
        h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
        const QString text = QStringLiteral("alpha\nbeta bug\ngamma");
        const QString uri = QStringLiteral("file:///m5-act.py");
        qmn::view::CodeEditor editor;
        editor.setPlainText(text);
        QVERIFY(h.hub.openDocument(&editor, uri, QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        qmn::lsp::DocumentSyncer* syncer = h.hub.syncerFor(&editor);
        QVERIFY(syncer);
        syncer->setDebounceMs(5000);
        syncer->setTypingCapMs(20000);

        // The relay half of M4 first: the server's publishDiagnostics landed
        // on the editor, decoded into the MODEL frame. The codeAction request
        // carries them BACK, and this fakeserver compares context.diagnostics
        // against its own fresh scan — an off-by-one severity or a frame that
        // does not round trip is exit 2 on the server.
        QVERIFY2(waitUntil([&] { return !editor.diagnostics().isEmpty(); }),
                 "the diagnostics never reached the editor to be re-encoded");
        QCOMPARE(int(editor.diagnostics().size()), 1);

        FeatureProbe probe;
        QVERIFY(h.hub.requestCodeActions(&editor, qmn::view::TextPosition(0, 2),
                                         probe.handler())
                > 0);
        QVERIFY2(waitUntil([&] { return probe.arrived; }),
                 "codeAction never answered (a diagnostics re-encode desync "
                 "kills the fakeserver: look for FAKESERVER-FAIL)");
        QVERIFY2(!probe.reply.isError, qPrintable(probe.reply.message));
        const QJsonArray actions = probe.reply.result.toArray();
        QCOMPARE(int(actions.size()), 4);

        // 1. The plain shape: `edit` on the action itself.
        const qmn::app::LspHub::ApplyReport withEdit
            = h.hub.applyCodeAction(actions.at(0).toObject(), client);
        QVERIFY2(withEdit.applied,
                 qPrintable(withEdit.refused.join(QLatin1String("; "))));
        QCOMPARE(withEdit.documentsChanged, 1);
        QCOMPARE(withEdit.editsApplied, 1);
        const QString acted = QStringLiteral("# act\n") + text;
        QCOMPARE(editor.toPlainText(), acted);
        QCOMPARE(syncer->version(), qint64(1)); // endServerEdit's full sync
        QJsonObject verdict = assertSyncModel(client, uri, acted, 1);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        editor.undo(); // the action is ONE user step
        QCOMPARE(editor.toPlainText(), text);
        syncer->flush();
        verdict = assertSyncModel(client, uri, text, 2);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));

        // 2. clangd's shape: the work hides in command.arguments[0], and the
        //    command NAME is never matched.
        const qmn::app::LspHub::ApplyReport viaCommand
            = h.hub.applyCodeAction(actions.at(1).toObject(), client);
        QVERIFY2(viaCommand.applied,
                 qPrintable(viaCommand.refused.join(QLatin1String("; "))));
        QCOMPARE(viaCommand.editsApplied, 1);
        const QString commanded = text + QStringLiteral(" // cmd");
        QCOMPARE(editor.toPlainText(), commanded);
        verdict = assertSyncModel(client, uri, commanded, 3);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        editor.undo();
        syncer->flush();
        verdict = assertSyncModel(client, uri, text, 4);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));

        // 3. A create-file documentChange: v1 has no workspace/file model, so
        //    ALL of it is refused — the text edits it carried included.
        const qmn::app::LspHub::ApplyReport structure
            = h.hub.applyCodeAction(actions.at(2).toObject(), client);
        QVERIFY(!structure.applied);
        QCOMPARE(structure.documentsChanged, 0);
        QCOMPARE(structure.editsApplied, 0);
        QVERIFY(std::any_of(structure.refused.cbegin(), structure.refused.cend(),
                            [](const QString& why) {
                                return why.contains(QStringLiteral("create-file"))
                                    && why.contains(
                                        QStringLiteral("handed to the app"));
                            }));
        QCOMPARE(editor.toPlainText(), text); // nothing moved, half-apply
                                              // impossible

        // 4. Neither shape: the decoder's reason, not a silent success.
        const qmn::app::LspHub::ApplyReport emptyHanded
            = h.hub.applyCodeAction(actions.at(3).toObject(), client);
        QVERIFY(!emptyHanded.applied);
        QCOMPARE(int(emptyHanded.refused.size()), 1);
        QVERIFY(emptyHanded.refused.at(0).contains(QStringLiteral("neither")));
        QCOMPARE(editor.toPlainText(), text);

        // Two refusals burned no version: the server still sits where the
        // undo stream left it.
        QCOMPARE(syncer->version(), qint64(4));
        verdict = assertSyncModel(client, uri, text, 4);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
    }

    // Rename answers a WorkspaceEdit (changes map, or versioned
    // documentChanges). Every occurrence of the word lands in ONE undo group;
    // a document the hub does not hold open, or a version pin the client is
    // not at, refuses the WHOLE edit instead of half a rename.
    void renameReplyAppliesEveryFileEditWithVersionPins()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("features")});
        h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
        const QString uri = QStringLiteral("file:///m5-rename.py");
        const QString text = QStringLiteral("alpha\nbeta alpha\ngamma alpha");
        qmn::view::CodeEditor editor;
        editor.setPlainText(text);
        QVERIFY(h.hub.openDocument(&editor, uri, QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        qmn::lsp::DocumentSyncer* syncer = h.hub.syncerFor(&editor);
        QVERIFY(syncer);
        syncer->setDebounceMs(5000);
        syncer->setTypingCapMs(20000);

        // The door's honest null: a rename that renames nothing answers the
        // spec's null, and the applier door reports it — never a crash, never
        // an applied:true over an empty edit.
        const qmn::app::LspHub::ApplyReport nullReply
            = h.hub.applyReplyEdit(QJsonValue(QJsonValue::Null), client);
        QVERIFY(!nullReply.applied);
        QVERIFY(!nullReply.refused.isEmpty());
        QVERIFY(nullReply.refused.at(0).contains(QStringLiteral("missing")));
        QCOMPARE(editor.toPlainText(), text);

        FeatureProbe probe;
        QVERIFY(h.hub.requestRename(&editor, qmn::view::TextPosition(1, 6),
                                    QStringLiteral("zeta"), probe.handler())
                > 0);
        QVERIFY2(waitUntil([&] { return probe.arrived; }),
                 "rename never answered");
        QVERIFY2(!probe.reply.isError, qPrintable(probe.reply.message));
        const qmn::app::LspHub::ApplyReport renamed
            = h.hub.applyReplyEdit(probe.reply.result, client);
        QVERIFY2(renamed.applied,
                 qPrintable(renamed.refused.join(QLatin1String("; "))));
        QCOMPARE(renamed.documentsChanged, 1);
        QCOMPARE(renamed.editsApplied, 3); // every occurrence, all in one step
        const QString renamedText = QStringLiteral("zeta\nbeta zeta\ngamma zeta");
        QCOMPARE(editor.toPlainText(), renamedText);
        editor.undo(); // ONE undo group reverses the whole rename
        QCOMPARE(editor.toPlainText(), text);
        syncer->flush();
        QJsonObject verdict = assertSyncModel(client, uri, text, 2);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));

        // The multi-file shape: a sibling document this hub does not hold
        // open. v1 hands workspace structure to the app, so this document's
        // own edits are refused too — that is what makes a half-applied
        // rename visible.
        const QString multi = QStringLiteral("alpha RENAMEMULTI\nalpha");
        editor.setPlainText(multi);
        FeatureProbe multiProbe;
        QVERIFY(h.hub.requestRename(&editor, qmn::view::TextPosition(0, 1),
                                    QStringLiteral("x2"), multiProbe.handler())
                > 0);
        QVERIFY2(waitUntil([&] { return multiProbe.arrived; }),
                 "the multi-file rename never answered");
        QVERIFY(!multiProbe.reply.isError);
        const qmn::app::LspHub::ApplyReport multiReport
            = h.hub.applyReplyEdit(multiProbe.reply.result, client);
        QVERIFY(!multiReport.applied);
        QCOMPARE(multiReport.documentsChanged, 0);
        QVERIFY(std::any_of(multiReport.refused.cbegin(),
                            multiReport.refused.cend(),
                            [](const QString& why) {
                                return why.contains(
                                    QStringLiteral("qmn-not-open.cpp"));
                            }));
        QCOMPARE(editor.toPlainText(), multi);
        // The request's own flush carried the new text (v3); the refusal
        // added no echo of its own.
        QCOMPARE(syncer->version(), qint64(3));
        verdict = assertSyncModel(client, uri, multi, 3);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));

        // The version pin: this fakeserver answers documentChanges pinned
        // SEVEN past its own model — which is the client's own version + 7,
        // because both sides bump on the same flush. The refusal must name
        // both numbers.
        const QString pinned = QStringLiteral("alpha RENAMEVERSION\nalpha");
        editor.setPlainText(pinned);
        FeatureProbe pinProbe;
        QVERIFY(h.hub.requestRename(&editor, qmn::view::TextPosition(0, 1),
                                    QStringLiteral("x3"), pinProbe.handler())
                > 0);
        QVERIFY2(waitUntil([&] { return pinProbe.arrived; }),
                 "the version-pinned rename never answered");
        QVERIFY(!pinProbe.reply.isError);
        const qmn::app::LspHub::ApplyReport pinReport
            = h.hub.applyReplyEdit(pinProbe.reply.result, client);
        QVERIFY(!pinReport.applied);
        QCOMPARE(pinReport.documentsChanged, 0);
        QVERIFY(std::any_of(pinReport.refused.cbegin(), pinReport.refused.cend(),
                            [](const QString& why) {
                                return why.contains(
                                    QStringLiteral("pinned at version 11"))
                                    && why.contains(
                                        QStringLiteral("document is at version 4"));
                            }));
        QCOMPARE(editor.toPlainText(), pinned);
        QCOMPARE(syncer->version(), qint64(4));
        verdict = assertSyncModel(client, uri, pinned, 4);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
    }

    // The SERVER-primed half of the write path: workspace/applyEdit arrives
    // on its own (a didSave marker primes it), the hub's applier runs it and
    // answers the truth, and the fakeserver judges BOTH halves from its side:
    // the answer on the wire (APPLYREQ-OK) and its own applier's replay
    // against the client's full-sync echo (APPLYCHECK-OK).
    void serverInitiatedApplyEditRoundTripsAndReappliesInOneUndoStep()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("features")});
        h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
        const QString text = QStringLiteral("APPLYEDIT alpha\nbeta");
        const QString uri = QStringLiteral("file:///m5-applyedit.py");
        qmn::view::CodeEditor editor;
        editor.setPlainText(text);
        QVERIFY(h.hub.openDocument(&editor, uri, QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        qmn::lsp::DocumentSyncer* syncer = h.hub.syncerFor(&editor);
        QVERIFY(syncer);
        syncer->setDebounceMs(5000);
        syncer->setTypingCapMs(20000);

        QSignalSpy stderrSpy(client, &qmn::lsp::LspClient::serverStderr);
        QSignalSpy refusedSpy(&h.hub, &qmn::app::LspHub::applyEditRefused);

        h.hub.saveDocument(&editor); // the marker rides the saved text

        QVERIFY2(waitUntil([&] { return editor.toPlainText().contains(
                                      QStringLiteral("srv\n")); }),
                 qPrintable(QStringLiteral("the server's edit never reached "
                                           "the document: %1")
                                .arg(stderrDump(stderrSpy))));
        const QString served = QStringLiteral("srv\n") + text;
        QCOMPARE(editor.toPlainText(), served);
        QCOMPARE(refusedSpy.count(), 0); // this edit is legal in every gate

        // The server's two verdicts: applied:true on the wire, and the
        // endServerEdit full sync matching THIS file's own applier replay.
        QVERIFY2(waitMarker(stderrSpy, QStringLiteral("APPLYREQ-OK")),
                 qPrintable(stderrDump(stderrSpy)));
        QVERIFY2(waitMarker(stderrSpy, QStringLiteral("APPLYCHECK-OK")),
                 qPrintable(stderrDump(stderrSpy)));

        // The pin the server sent was version 0 — the syncer's own number at
        // that moment, which is only true because the save flushed nothing
        // extra and the applier checked AFTER a flush.
        QCOMPARE(syncer->version(), qint64(1));
        QJsonObject verdict = assertSyncModel(client, uri, served, 1);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));

        // One server edit = one user step: a single undo reverses it whole,
        // and the delta stream carries the original back to the server.
        editor.undo();
        QCOMPARE(editor.toPlainText(), text);
        QVERIFY(!editor.canUndo());
        syncer->flush();
        verdict = assertSyncModel(client, uri, text, 2);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        QVERIFY(!sawMarker(stderrSpy, QStringLiteral("APPLYCHECK-FAIL")));
        QVERIFY(!sawMarker(stderrSpy, QStringLiteral("FAKESERVER-FAIL")));
    }

    // The same request, primed to be REFUSED: a legal edit for this document
    // plus one for a uri the hub does not hold open. All-or-nothing means the
    // honest applied:false the fakeserver checks (APPLYREFUSE-OK), the
    // reasons on the hub's own signal, and a model that never moved.
    void applyEditRefusedKeepsTheModelWhole()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("features")});
        h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
        const QString text = QStringLiteral("APPLYREFUSE alpha\nbeta");
        const QString uri = QStringLiteral("file:///m5-refuse.py");
        qmn::view::CodeEditor editor;
        editor.setPlainText(text);
        QVERIFY(h.hub.openDocument(&editor, uri, QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        qmn::lsp::DocumentSyncer* syncer = h.hub.syncerFor(&editor);
        QVERIFY(syncer);
        syncer->setDebounceMs(5000);
        syncer->setTypingCapMs(20000);

        QSignalSpy stderrSpy(client, &qmn::lsp::LspClient::serverStderr);
        QSignalSpy refusedSpy(&h.hub, &qmn::app::LspHub::applyEditRefused);

        h.hub.saveDocument(&editor);

        QVERIFY2(waitMarker(stderrSpy, QStringLiteral("APPLYREFUSE-OK")),
                 qPrintable(QStringLiteral("the server never saw the honest "
                                           "applied:false: %1")
                                .arg(stderrDump(stderrSpy))));
        QCOMPARE(refusedSpy.count(), 1);
        QCOMPARE(qvariant_cast<QString>(refusedSpy.at(0).at(0)),
                 QStringLiteral("pysrv"));
        const QStringList reasons
            = qvariant_cast<QStringList>(refusedSpy.at(0).at(1));
        QVERIFY(!reasons.isEmpty());
        QVERIFY(std::any_of(reasons.cbegin(), reasons.cend(),
                            [](const QString& why) {
                                return why.contains(
                                    QStringLiteral("qmn-not-open.cpp"))
                                    && why.contains(
                                        QStringLiteral("is not a hub document"));
                            }));

        // Nothing moved, on either side — and NO endServerEdit echo went out
        // for an edit that never happened (the version is the proof).
        QCOMPARE(editor.toPlainText(), text);
        QCOMPARE(syncer->version(), qint64(0));
        syncer->flush(); // nothing pending: an honest no-op, no version burned
        QCOMPARE(syncer->version(), qint64(0));
        const QJsonObject verdict = assertSyncModel(client, uri, text, 0);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
    }

    // window/showMessageRequest: the hub only RELAYS (the app picks the
    // button), and the answer that arrives FIRST is the one the server gets.
    // An app that never answers still gets the client layer's spec-legal Null
    // default — both halves are judged by the fakeserver itself.
    void showMessageRequestFirstAnswerWinsAndNullDefaultFollows()
    {
        { // the app answers inside the relay: the picked item wins
            HubHarness h(QStringList{QStringLiteral("sync"),
                                     QStringLiteral("utf-16"),
                                     QStringLiteral("features")});
            h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
            qmn::view::CodeEditor editor;
            editor.setPlainText(QStringLiteral("MSGREQ-FIRST alpha"));
            QVERIFY(h.hub.openDocument(
                &editor, QStringLiteral("file:///m5-msgreq-first.py"),
                QStringLiteral("python")));
            QVERIFY(waitReady(h.hub.clientForSpec(QStringLiteral("pysrv"))));

            int relayed = 0;
            QObject::connect(
                &h.hub, &qmn::app::LspHub::serverShowMessageRequest,
                [&](const QString& spec, const QJsonValue& id,
                    const QJsonValue& params) {
                    ++relayed;
                    QCOMPARE(spec, QStringLiteral("pysrv"));
                    // The relay carried the params: the app picks its own
                    // button out of the actions the server offered.
                    const QJsonArray actions
                        = params.toObject()
                              .value(QStringLiteral("actions"))
                              .toArray();
                    QCOMPARE(int(actions.size()), 2);
                    QJsonObject picked;
                    for (const QJsonValue& action : actions) {
                        if (action.toObject()
                                    .value(QStringLiteral("title"))
                                    .toString()
                                == QLatin1String("FIRST"))
                            picked = action.toObject();
                    }
                    QVERIFY(!picked.isEmpty());
                    qmn::lsp::LspClient* answered
                        = h.hub.clientForSpec(spec); // the relay's own way back
                    QVERIFY(answered);
                    answered->respondShowMessageRequest(id, picked);
                });

            auto* stderrClient = h.hub.clientForSpec(QStringLiteral("pysrv"));
            QSignalSpy stderrSpy(stderrClient,
                                 &qmn::lsp::LspClient::serverStderr);
            h.hub.saveDocument(&editor);

            QVERIFY2(waitUntil([&] { return relayed > 0; }),
                     qPrintable(stderrDump(stderrSpy)));
            // "FIRST" is the item the app picked, and the server confirms it
            // won the race against this layer's own Null default (the answer
            // that ARRIVES FIRST is the answer the server gets).
            QVERIFY2(waitMarker(stderrSpy, QStringLiteral("MSGREQ-OK")),
                     qPrintable(stderrDump(stderrSpy)));
            QVERIFY(!sawMarker(stderrSpy, QStringLiteral("MSGREQ-NULL-OK")));
        }
        { // the app says nothing: the Null default answers, once
            HubHarness h(QStringList{QStringLiteral("sync"),
                                     QStringLiteral("utf-16"),
                                     QStringLiteral("features")});
            h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
            qmn::view::CodeEditor editor;
            editor.setPlainText(QStringLiteral("MSGREQ-NULL alpha"));
            QVERIFY(h.hub.openDocument(
                &editor, QStringLiteral("file:///m5-msgreq-null.py"),
                QStringLiteral("python")));
            auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
            QVERIFY(client);
            QVERIFY(waitReady(client));

            int relayed = 0;
            QObject::connect(
                &h.hub, &qmn::app::LspHub::serverShowMessageRequest,
                [&](const QString&, const QJsonValue&, const QJsonValue&) {
                    ++relayed; // a modal-less app: it sees the request and
                               // answers no one
                });
            QSignalSpy stderrSpy(client, &qmn::lsp::LspClient::serverStderr);
            h.hub.saveDocument(&editor);

            QVERIFY2(waitUntil([&] { return relayed == 1; }),
                     qPrintable(stderrDump(stderrSpy)));
            QVERIFY2(waitMarker(stderrSpy, QStringLiteral("MSGREQ-NULL-OK")),
                     qPrintable(stderrDump(stderrSpy)));
            // The session is still standing: an unanswered request is not an
            // error, and the default's answer closed the id.
            QVERIFY(client->isReady());
        }
    }

    // --- M8 machine 6: the willSaveWaitUntil save detour (PLAN) -----------
    // The server advertises textDocumentSync.willSaveWaitUntil; saveDocument
    // then routes: flush -> textDocument/willSaveWaitUntil -> the applier (its
    // endServerEdit full sync rides BEFORE the didSave) -> didSave ->
    // markSaved. The ORDERING proof is server-side: the fakeserver arms its
    // own apply oracle when answering and only the client's full sync may
    // clear it, so "the oracle was consumed before didSave landed" is
    // exactly what the WSWU-SAVE-OK marker reports.
    void willSaveWaitUntilEditsLandBeforeDidSave()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("wswu")});
        h.installSpec(QStringLiteral("wsrv"), {QStringLiteral("wslang")});
        const QString uri = QStringLiteral("file:///m6-wswu.wsl");
        const QString text = QStringLiteral("WSWU alpha\nbeta");
        qmn::view::CodeEditor editor;
        editor.setPlainText(text);
        QVERIFY(h.hub.openDocument(&editor, uri, QStringLiteral("wslang")));
        auto* client = h.hub.clientForSpec(QStringLiteral("wsrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        qmn::lsp::DocumentSyncer* syncer = h.hub.syncerFor(&editor);
        QVERIFY(syncer);
        syncer->setDebounceMs(5000);
        syncer->setTypingCapMs(20000);

        QSignalSpy stderrSpy(client, &qmn::lsp::LspClient::serverStderr);

        QVERIFY(editor.document()->isModified()); // an edit happened before
                                                  // the save (open alone never
                                                  // sets the flag)
        h.hub.saveDocument(&editor); // the detour STARTS here; it finishes
                                     // when the reply round completes
        QVERIFY(editor.document()->isModified()); // the async path: markSaved
                                                  // rides the REPLY, not the
                                                  // door
        QVERIFY2(waitUntil([&] {
            return editor.toPlainText().contains(
                QStringLiteral("wswu-applied"));
        }), qPrintable(QStringLiteral("the server's pre-save edit never "
                                      "reached the document: %1")
                         .arg(stderrDump(stderrSpy))));
        QVERIFY2(waitMarker(stderrSpy, QStringLiteral("APPLYCHECK-OK")),
                 qPrintable(stderrDump(stderrSpy))); // the client's full sync
                                                     // echoed the server's own
                                                     // replay
        QVERIFY2(waitMarker(stderrSpy, QStringLiteral("WSWU-SAVE-OK")),
                 qPrintable(stderrDump(stderrSpy))); // didSave found the
                                                     // EDITED model: the
                                                     // ordering PLAN pins
        const QString served = QStringLiteral("wswu-applied\n") + text;
        QCOMPARE(editor.toPlainText(), served);
        QVERIFY(!editor.document()->isModified()); // markSaved rode the reply
        QCOMPARE(syncer->version(), qint64(1)); // the open sat current (the
                                                // pre-request flush burned
                                                // nothing), the applier's
                                                // endServerEdit sync bumped
                                                // once
        QJsonObject verdict = assertSyncModel(client, uri, served, 1);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        QCOMPARE(verdict.value(QStringLiteral("saves")).toInt(), 1);

        // The pre-save edit is ONE user undo step, like every applier action.
        editor.undo();
        QCOMPARE(editor.toPlainText(), text);
        syncer->flush();
        verdict = assertSyncModel(client, uri, text, 2);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
    }

    // The capability gate's OTHER side: no textDocumentSync options object,
    // no request on the wire, and the save stays the synchronous machine-4
    // door. The tripwire runs LOUD: the fakeserver exits 2 if a
    // willSaveWaitUntil request ever reaches a server that never advertised
    // it — a dead spawn would surface in the restart counter pinned below.
    void saveWithoutTheCapabilityNeverAsksAndStaysSynchronous()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16")});
        h.installSpec(QStringLiteral("plainrv"), {QStringLiteral("wslang")});
        const QString uri = QStringLiteral("file:///m6-nowswu.wsl");
        // The SAME drill marker text: a hub that wrongly detoured would have
        // gotten an EDIT (or killed the server on the tripwire) here instead
        // of an ordinary save.
        const QString text = QStringLiteral("WSWU alpha\nbeta");
        qmn::view::CodeEditor editor;
        editor.setPlainText(text);
        QVERIFY(h.hub.openDocument(&editor, uri, QStringLiteral("wslang")));
        auto* client = h.hub.clientForSpec(QStringLiteral("plainrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        QSignalSpy stderrSpy(client, &qmn::lsp::LspClient::serverStderr);

        h.hub.saveDocument(&editor);
        QVERIFY(!editor.document()->isModified()); // SYNCHRONOUS: the flag
                                                   // flipped inside the door
        QCOMPARE(editor.toPlainText(), text); // nothing asked, nothing moved
        QJsonObject verdict = assertSyncModel(client, uri, text, 0);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        QCOMPARE(verdict.value(QStringLiteral("saves")).toInt(), 1);
        QVERIFY(client->isReady()); // the tripwire would have killed it…
        QCOMPARE(h.spawnCount, 1); // …and forced a restart: neither happened
    }

    // A refused wswu round: the fakeserver answers the REFUSE-drill document
    // with a WorkspaceEdit naming a document this hub does not hold open
    // (the honest deviation note lives in the fakeserver's handler). The
    // applier refuses the WHOLE batch, the model stays byte-stable, and the
    // SAVE PROCEEDS: the round trip is a courtesy, never a veto.
    void refusedWillSaveWaitUntilKeepsModelWholeButSaveProceeds()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("wswu")});
        h.installSpec(QStringLiteral("wsrv2"), {QStringLiteral("wslang")});
        const QString uri = QStringLiteral("file:///m6-wswurefuse.wsl");
        const QString text = QStringLiteral("WSWU-REFUSE alpha\nbeta");
        qmn::view::CodeEditor editor;
        editor.setPlainText(text);
        QVERIFY(h.hub.openDocument(&editor, uri, QStringLiteral("wslang")));
        auto* client = h.hub.clientForSpec(QStringLiteral("wsrv2"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        qmn::lsp::DocumentSyncer* syncer = h.hub.syncerFor(&editor);
        QVERIFY(syncer);
        syncer->setDebounceMs(5000);
        syncer->setTypingCapMs(20000);
        QSignalSpy stderrSpy(client, &qmn::lsp::LspClient::serverStderr);

        h.hub.saveDocument(&editor);
        QVERIFY2(waitMarker(stderrSpy,
                            QStringLiteral("WSWU-REFUSE-SAVE-OK")),
                 qPrintable(stderrDump(stderrSpy))); // the didSave found the
                                                     // PRISTINE model: the
                                                     // refusal held on both
                                                     // sides of the wire
        QCOMPARE(editor.toPlainText(), text); // nothing moved on the client
        QVERIFY(!editor.document()->isModified()); // the save still rode the
                                                   // reply: markSaved after
                                                   // the refusal, never
                                                   // instead of it
        QCOMPARE(syncer->version(), qint64(0)); // no flush to do, no edit, no
                                                // full sync: no version
                                                // burned
        QJsonObject verdict = assertSyncModel(client, uri, text, 0);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        QCOMPARE(verdict.value(QStringLiteral("saves")).toInt(), 1);
    }

    // --- M8 machine 7: the stored workspace configuration (qtmonaco parity) --
    // The VIEW is the buffer (its door is a plain QString; the dict->JSON
    // conversion lives in the Python shim), and the HUB is its only consumer.
    // The fakeserver's "config" mode judges the wire from ITS side: a config
    // that arrives before the `initialized` notification, or a params/settings
    // object that is absent or not an object, is a structural lie (exit 2).
    // That oracle is DEFENSIVE — the client layer's own non-Ready notify rule
    // (machine 2) already drops early traffic — and every accepted settings
    // object echoes back as a CONFIG-<n> window/showMessage, so the test sees
    // what actually crossed the wire, in order and byte for byte. What the
    // client side pins is the DELIVERY EDGE: the buffer waits on the view
    // until the session comes up, then follows every real change.
    void storedWorkspaceConfigurationBuffersUntilReadyThenFollowsTheView()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("config")});
        h.installSpec(QStringLiteral("cfgrv"), {QStringLiteral("cfglang")});
        const QString uri = QStringLiteral("file:///m7-config.cfg");
        const QString text = QStringLiteral("cfg alpha");

        QStringList shown; // the server's echoes, in the order they arrived
        QObject::connect(
            &h.hub, &qmn::app::LspHub::serverShowMessage,
            [&](const QString& spec, int type, const QString& message) {
                QCOMPARE(spec, QStringLiteral("cfgrv"));
                QCOMPARE(type, 3);
                shown << message;
            });

        qmn::view::CodeEditor editor;
        editor.setPlainText(text);
        // The buffer is set BEFORE the document is even registered: the
        // "buffered until init" half of the qtmonaco semantics — the view
        // holds it, the hub delivers it when the session comes up.
        editor.setStoredWorkspaceConfiguration(
            QStringLiteral("{\"python\":{\"fmtOnSave\":true}}"));
        QVERIFY(h.hub.openDocument(&editor, uri, QStringLiteral("cfglang")));
        auto* client = h.hub.clientForSpec(QStringLiteral("cfgrv"));
        QVERIFY(client);
        QSignalSpy stderrSpy(client, &qmn::lsp::LspClient::serverStderr);
        QCOMPARE(shown.size(), 0); // the session is still coming up: the
                                   // buffer travels on the wire, not ahead of it
        QVERIFY(waitReady(client));
        QVERIFY2(waitUntil([&] { return shown.size() == 1; }),
                 qPrintable(QStringLiteral("READY brought no configuration: %1")
                                .arg(stderrDump(stderrSpy))));
        QCOMPARE(shown.at(0), QStringLiteral(
                                  "CONFIG-1 {\"python\":{\"fmtOnSave\":true}}"));

        // The live re-push: the view's signal travels to the wire at once
        // (QJsonObject renders keys sorted, so the compact echo is exact).
        editor.setStoredWorkspaceConfiguration(
            QStringLiteral("{\"python\":{\"fmtOnSave\":false},\"x\":1}"));
        QVERIFY2(waitUntil([&] { return shown.size() == 2; }),
                 qPrintable(QStringLiteral("a configuration change never "
                                           "reached the server: %1")
                                .arg(stderrDump(stderrSpy))));
        QCOMPARE(shown.at(1), QStringLiteral(
                                  "CONFIG-2 {\"python\":{\"fmtOnSave\":false},"
                                  "\"x\":1}"));

        // The two honest LOCAL refuses, both proven by real traffic around
        // them (the session is alive and saving, only the config is held):
        //   - an EMPTY buffer = "never configured" = send NOTHING (an empty
        //     settings object would claim "no settings", a different thing);
        //   - a buffer that is not a JSON object has no legal params shape.
        editor.setStoredWorkspaceConfiguration(QString());
        h.hub.saveDocument(&editor); // didSave rides: the wire is talking
        QJsonObject verdict = assertSyncModel(client, uri, text, 0);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        QCOMPARE(verdict.value(QStringLiteral("saves")).toInt(), 1);
        QCOMPARE(shown.size(), 2); // the empty buffer produced NO traffic
        editor.setStoredWorkspaceConfiguration(QStringLiteral("plain, not json"));
        h.hub.saveDocument(&editor);
        verdict = assertSyncModel(client, uri, text, 0);
        QVERIFY2(verdict.value(QStringLiteral("ok")).toBool(),
                 qPrintable(modelDump(verdict)));
        QCOMPARE(verdict.value(QStringLiteral("saves")).toInt(), 2);
        QCOMPARE(shown.size(), 2); // a non-object has no legal params shape
        // A legal object again, and the feed resumes.
        editor.setStoredWorkspaceConfiguration(QStringLiteral("{\"n\":2}"));
        QVERIFY2(waitUntil([&] { return shown.size() == 3; }),
                 qPrintable(QStringLiteral("the refused-then-fixed buffer never "
                                           "arrived: %1")
                                .arg(stderrDump(stderrSpy))));
        QCOMPARE(shown.at(2), QStringLiteral("CONFIG-3 {\"n\":2}"));

        // The OPEN edge: a document registered on a session that is ALREADY
        // ready gets its own buffer pushed at open time (each document owns
        // its buffer — the settings are the editor's, the wire is shared).
        qmn::view::CodeEditor other;
        other.setPlainText(QStringLiteral("cfg beta"));
        other.setStoredWorkspaceConfiguration(QStringLiteral("{\"d\":1}"));
        const QString uri2 = QStringLiteral("file:///m7-config2.cfg");
        QVERIFY(h.hub.openDocument(&other, uri2, QStringLiteral("cfglang")));
        QVERIFY2(waitUntil([&] { return shown.size() == 4; }),
                 qPrintable(QStringLiteral("the open of a document on a live "
                                           "session never pushed its buffer: %1")
                                .arg(stderrDump(stderrSpy))));
        QCOMPARE(shown.at(3), QStringLiteral("CONFIG-4 {\"d\":1}"));
        QCOMPARE(serverDocumentCount(client), 2);

        // The tripwire's other side: this is still the ONE spawn we started.
        // A structural lie in the config mode exits 2, which would have shown
        // up as a dead session plus a restart here.
        QVERIFY(client->isReady());
        QCOMPARE(h.spawnCount, 1);
    }

    // The same buffer's other half: it is NOT lost when the session dies. The
    // drill is machine 4's lifecycle ladder (the first TWO spawns die right
    // after their handshake, the manual restartServer door starts a clean
    // third one), and the buffer is set while the session is FAILED — so the
    // hub must send NOTHING, then deliver it on the next READY. A hub that
    // cached the settings instead of reading the view would deliver nothing
    // here, and a hub that ignored the lifecycle would throw at a dead wire.
    void storedWorkspaceConfigurationSurvivesAFailedSessionAndRidesRestart()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("config")});
        h.installSpec(QStringLiteral("cfgrv2"), {QStringLiteral("cfglang")});
        h.crashSpawnCount = 2; // spawns #0 and #1 die after their handshake
        const QString uri = QStringLiteral("file:///m7-config-restart.cfg");

        QStringList shown;
        QObject::connect(&h.hub, &qmn::app::LspHub::serverShowMessage,
                         [&](const QString&, int, const QString& message) {
                             shown << message;
                         });

        qmn::view::CodeEditor editor;
        editor.setPlainText(QStringLiteral("cfg alpha"));
        QVERIFY(h.hub.openDocument(&editor, uri, QStringLiteral("cfglang")));
        auto* client = h.hub.clientForSpec(QStringLiteral("cfgrv2"));
        QVERIFY(client);
        QVERIFY2(waitUntil([&] { return client->state()
                                           == qmn::lsp::LspClient::State::Failed; }),
                 qPrintable(QStringLiteral(
                     "the two-spawn crash ladder never left the client FAILED")));
        QCOMPARE(h.spawnCount, 2); // the one auto-restart is spent

        // DEAD wire: the buffer is set now, and nothing may travel.
        editor.setStoredWorkspaceConfiguration(QStringLiteral("{\"k\":1}"));
        QTest::qWait(200); // a real settle: the wire had its chance
        QVERIFY(shown.isEmpty());

        // The app's own restart: ready() re-fires and the view's buffer rides
        // it — the settings are re-read from the VIEW, not replayed from a
        // hub-side copy.
        h.hub.restartServer(QStringLiteral("cfgrv2"));
        QVERIFY2(waitUntil([&] { return shown.size() == 1; }),
                 qPrintable(QStringLiteral(
                     "the restart never brought the buffered configuration to "
                     "the wire")));
        QCOMPARE(shown.at(0), QStringLiteral("CONFIG-1 {\"k\":1}"));
        QVERIFY(client->isReady()); // spawn #2 is clean
        QCOMPARE(h.spawnCount, 3);
    }

    // --- M8 close-train sliver: glue-side popup TRIGGERING -----------------
    // The hub decides WHEN the two view popups show; the fakeserver's `reqlog`
    // stderr channel is the traffic oracle (a trigger that fires nothing shows
    // nothing), and the reply's own labels pin the frame the hub built.

    // Completion by TYPING: identifier unit -> Invoked (tk=1), advertised
    // trigger char -> TriggerForCharacter (tk=2 + tc=), caret captured at FIRE
    // time (off=), debounce coalescing every keystroke into one request.
    void hubTriggerCompletionOnTypingAndTriggerChar()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("features"),
                                 QStringLiteral("reqlog")});
        h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
        h.hub.setDebounceMs(5000); // the wire stays spare: only feature REQs
        qmn::view::CodeEditor editor;
        editor.setLanguage(QStringLiteral("python"));
        editor.setPlainText(QStringLiteral("beta"));
        QVERIFY(h.hub.openDocument(&editor,
                                   QStringLiteral("file:///trig-c.py"),
                                   QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        QSignalSpy stderrSpy(client, &qmn::lsp::LspClient::serverStderr);
        editor.show(); // the visibility getters read QWidget::isVisible (M6 trap)
        editor.setPosition({0, 4});
        h.hub.setPopupTriggers(&editor, true);
        QVERIFY(h.hub.popupTriggersEnabled(&editor));

        QTest::keyClick(&editor, Qt::Key_A); // identifier unit at the caret
        QVERIFY2(waitMarker(stderrSpy,
                            QStringLiteral("REQ textDocument/completion")),
                 qPrintable(stderrDump(stderrSpy)));
        QVERIFY2(waitUntil([&] { return editor.completionListVisible(); }),
                 qPrintable(stderrDump(stderrSpy)));
        QListWidget* list = editor.findChild<QListWidget*>(
            QStringLiteral("qmn-completion-list"));
        QVERIFY(list);
        QCOMPARE(list->count(), 3); // alpha, tk=1, off=5 — the caret AT FIRE
                                    // TIME (the typed 'a' sits at offset 4)
        QCOMPARE(list->item(0)->text(), QStringLiteral("alpha"));
        QCOMPARE(list->item(1)->text(), QStringLiteral("tk=1"));
        QCOMPARE(list->item(2)->text(), QStringLiteral("off=5"));
        QCOMPARE(stderrSpy.count(), 1); // ONE request for one qualifying burst

        QTest::keyClick(&editor, Qt::Key_Period); // the ONLY advertised
                                                  // triggerCharacters item
        QVERIFY2(waitUntil(
                     [&] {
                         int reqs = 0;
                         for (const QList<QVariant>& args : stderrSpy)
                             if (args.at(0).toString()
                                     .contains(QStringLiteral(
                                         "REQ textDocument/completion")))
                                 ++reqs;
                         return reqs == 2;
                     }),
                 qPrintable(stderrDump(stderrSpy)));
        QVERIFY2(waitUntil([&] { return editor.completionListVisible(); }),
                 qPrintable(stderrDump(stderrSpy)));
        QCOMPARE(list->count(), 4); // alpha, tk=2, tc=., off=6
        QCOMPARE(list->item(1)->text(), QStringLiteral("tk=2"));
        QCOMPARE(list->item(2)->text(), QStringLiteral("tc=."));
        QCOMPARE(list->item(3)->text(), QStringLiteral("off=6"));
    }

    // SUPERSEDE: with 600 ms of server-side latency the first request is
    // still in flight when the next keystroke fires a new one — the hub
    // $/cancelRequests the stale id (one CANCELREQ) and only the NEWEST
    // reply ever reaches the popup.
    void hubTriggerSupersedesInFlightCompletion()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("features"),
                                 QStringLiteral("reqlog"),
                                 QStringLiteral("cancellog"),
                                 QStringLiteral("completiondelay=600")});
        h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
        h.hub.setDebounceMs(5000);
        qmn::view::CodeEditor editor;
        editor.setLanguage(QStringLiteral("python"));
        editor.setPlainText(QStringLiteral("beta"));
        QVERIFY(h.hub.openDocument(&editor,
                                   QStringLiteral("file:///trig-s.py"),
                                   QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        QSignalSpy stderrSpy(client, &qmn::lsp::LspClient::serverStderr);
        editor.show();
        editor.setPosition({0, 4});
        h.hub.setPopupTriggers(&editor, true);

        QTest::keyClick(&editor, Qt::Key_A);
        QVERIFY2(waitMarker(stderrSpy,
                            QStringLiteral("REQ textDocument/completion")),
                 qPrintable(stderrDump(stderrSpy)));
        QVERIFY(!editor.completionListVisible()); // the reply is still brewing

        QTest::keyClick(&editor, Qt::Key_B); // supersede while IN FLIGHT
        QVERIFY2(waitMarker(stderrSpy, QStringLiteral("CANCELREQ")),
                 qPrintable(stderrDump(stderrSpy)));
        QVERIFY2(waitUntil(
                     [&] {
                         int reqs = 0;
                         for (const QList<QVariant>& args : stderrSpy)
                             if (args.at(0).toString()
                                     .contains(QStringLiteral(
                                         "REQ textDocument/completion")))
                                 ++reqs;
                         return reqs == 2;
                     }),
                 qPrintable(stderrDump(stderrSpy)));
        int cancels = 0;
        for (const QList<QVariant>& args : stderrSpy)
            if (args.at(0).toString().contains(QStringLiteral("CANCELREQ")))
                ++cancels;
        QCOMPARE(cancels, 1); // exactly the one stale request

        // The delayed FIRST reply lands too — and must show NOTHING; only
        // the newest reply paints the popup.
        QVERIFY2(waitUntil([&] { return editor.completionListVisible(); }),
                 qPrintable(stderrDump(stderrSpy)));
        QListWidget* list = editor.findChild<QListWidget*>(
            QStringLiteral("qmn-completion-list"));
        QVERIFY(list);
        QCOMPARE(list->count(), 3);
        QCOMPARE(list->item(2)->text(),
                 QStringLiteral("off=6")); // the SECOND request's caret
        QTest::qWait(700); // the past due: no further traffic, no re-show
        QCOMPARE(cancels, 1);
    }

    // Hover: a NON-typing idle caret move asks after kHoverTriggerMs; the
    // server's answer (contents + range) becomes the popup verbatim; typing
    // NEVER hovers; and while completion owns the popup, hover is skipped.
    void hubTriggerHoverOnIdleCaretAndTypingNeverHovers()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("features"),
                                 QStringLiteral("hoveron"),
                                 QStringLiteral("reqlog")});
        h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
        h.hub.setDebounceMs(5000);
        qmn::view::CodeEditor editor;
        editor.setLanguage(QStringLiteral("python"));
        editor.setPlainText(QStringLiteral("alpha beta\ngamma"));
        QVERIFY(h.hub.openDocument(&editor,
                                   QStringLiteral("file:///trig-h.py"),
                                   QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        QSignalSpy stderrSpy(client, &qmn::lsp::LspClient::serverStderr);
        editor.show();
        editor.setPosition({0, 0});
        h.hub.setPopupTriggers(&editor, true);

        editor.setPosition({0, 7}); // INSIDE the word "beta"
        QVERIFY2(waitMarker(stderrSpy, QStringLiteral("REQ textDocument/hover")),
                 qPrintable(stderrDump(stderrSpy)));
        QVERIFY2(waitUntil([&] { return editor.hoverVisible(); }),
                 qPrintable(stderrDump(stderrSpy)));
        QLabel* label =
            editor.findChild<QLabel*>(QStringLiteral("qmn-hover-label"));
        QVERIFY(label);
        QCOMPARE(label->text(), QStringLiteral("HOVER beta off=7"));
        QTest::qWait(300);
        QCOMPARE(stderrSpy.count(), 1); // ONE idle move, ONE ask

        QTest::keyClick(&editor, Qt::Key_X); // TYPING: the cursorMoved paired
                                             // with the edit is its own move
        QVERIFY2(waitMarker(stderrSpy,
                            QStringLiteral("REQ textDocument/completion")),
                 qPrintable(stderrDump(stderrSpy)));
        QVERIFY2(waitUntil([&] { return editor.completionListVisible(); }),
                 qPrintable(stderrDump(stderrSpy)));
        QVERIFY(!editor.hoverVisible()); // typing dismissed it (view auto-hide)

        editor.setPosition({1, 1}); // INSIDE "gamma": a NON-typing move, but
                                    // completion owns the popup -> hover skipped
        QTest::qWait(900); // past the 500 ms hover debounce + margin
        int hovers = 0;
        for (const QList<QVariant>& args : stderrSpy)
            if (args.at(0).toString()
                    .contains(QStringLiteral("REQ textDocument/hover")))
                ++hovers;
        QCOMPARE(hovers, 1); // still the one ask from the idle move
        QVERIFY(!editor.hoverVisible());
    }

    // OFF is the DEFAULT and the cheap path: an unarmed editor produces ZERO
    // feature traffic; arming works; disarming hides NOW and goes silent.
    void hubTriggersOffByDefaultAndDisarmHidesInstantly()
    {
        HubHarness h(QStringList{QStringLiteral("sync"),
                                 QStringLiteral("utf-16"),
                                 QStringLiteral("features"),
                                 QStringLiteral("reqlog")});
        h.installSpec(QStringLiteral("pysrv"), {QStringLiteral("python")});
        h.hub.setDebounceMs(5000);
        qmn::view::CodeEditor editor;
        editor.setLanguage(QStringLiteral("python"));
        editor.setPlainText(QStringLiteral("beta"));
        QVERIFY(h.hub.openDocument(&editor,
                                   QStringLiteral("file:///trig-off.py"),
                                   QStringLiteral("python")));
        auto* client = h.hub.clientForSpec(QStringLiteral("pysrv"));
        QVERIFY(client);
        QVERIFY(waitReady(client));
        QSignalSpy stderrSpy(client, &qmn::lsp::LspClient::serverStderr);
        editor.show();
        QVERIFY(!h.hub.popupTriggersEnabled(&editor)); // the DEFAULT

        QTest::keyClick(&editor, Qt::Key_A); // typing AND a deliberate move...
        editor.setPosition({0, 0});
        QTest::qWait(900); // ...past BOTH debounces (150 + 500 + margin)
        QVERIFY(stderrSpy.empty()); // never armed: not one feature request
        QVERIFY(!editor.completionListVisible());
        QVERIFY(!editor.hoverVisible());
        h.hub.setPopupTriggers(&editor, false); // OFF for a never-armed editor:
                                                // a dismissal, not an error
        QVERIFY(!h.hub.popupTriggersEnabled(&editor));

        h.hub.setPopupTriggers(&editor, true); // NOW the same gesture asks
        QTest::keyClick(&editor, Qt::Key_A);
        QVERIFY2(waitUntil([&] { return editor.completionListVisible(); }),
                 qPrintable(stderrDump(stderrSpy)));

        h.hub.setPopupTriggers(&editor, false); // hide is INSTANT
        QVERIFY(!editor.completionListVisible());
        QVERIFY(!h.hub.popupTriggersEnabled(&editor));
        const int armedReqs = stderrSpy.count();
        QVERIFY(armedReqs > 0);
        QTest::keyClick(&editor, Qt::Key_Z); // and the taps went quiet too
        QTest::qWait(600);
        QCOMPARE(stderrSpy.count(), armedReqs);
    }

};

QTEST_MAIN(TstLsp) // NOT guiless any more: machine 4's hub slots drive a real
                   // CodeEditor (ctest runs this entry offscreen)
#include "tst_lsp.moc"
