#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>

#include "protocol/RpcClient.hpp"
#include "protocol/Transport.hpp"

namespace qmn::lsp {

// The LSP client lifecycle (PLAN.md GATE 2 / M8, machine 2). Layer rule:
// this layer depends ONLY on src/protocol — never view, never core.
//
// The Transport + RpcClient pair is a SESSION: an unwanted death tears the
// pair down and the factory rebuilds BOTH (M1's "RpcClient never starts the
// Transport" — LspClient is the session owner PLAN pins).
//
// Lifecycle (PLAN-pinned): lazy start is the GLUE's decision; start() runs
// initialize -> initialized with the pinned 10 s door. An UNWANTED death
// while NotStarted/Starting/Ready auto-restarts EXACTLY ONCE with backoff,
// then FAILED + the restartServer() manual door. Every (re-)READY re-emits
// ready() — machine 3's syncer rides that as the re-didOpen self-heal. A
// FAILED HANDSHAKE does not auto-restart (decided in M2: a live-but-
// unresponsive server must not be spawned twice; restartServer() is the door).
//
// The server->client POLICY half (M1 froze the mechanism): every always-
// answer handler is registered — showMessage(+Request)/logMessage(+$logTrace)/
// progress/workDoneProgress-create/configuration/workspaceFolders answer or
// stay spec-silent, and register/unregisterCapability get the REAL
// capability registry (decided 2026-11-15 over the -32601 shortcut).
// applyEdit is EXPOSED to glue: the answer belongs to the applier (machine 5).
//
// The std::function factory stays unbound by design (the wheel wraps the
// VIEW; LspClient's Python shape is machine 7's decision).
class LspClient : public QObject {
    Q_OBJECT
public:
    // Transport factory: parent the Transport under `parent` (the LspClient)
    // so the session teardown deletes it. Called ONLY by start() and the
    // restart path — the M1 pair-rewiring hazard: a raw pointer to an old
    // session's RpcClient must never be kept across sessions.
    using TransportFactory =
        std::function<qmn::protocol::Transport*(QObject* parent)>;

    // Not PLAN-pinned: the auto-restart backoff and the initialize door
    // (the RpcClient-side kInitializeTimeoutMs=10 s still fires inside).
    static constexpr int kRestartBackoffMs = 300;

    // LSP 3.17 wire codes this layer speaks (the -328xx band is LSP's own):
    static constexpr int kWireServerNotInitialized = -32002;
    static constexpr int kWireRequiredClientCapabilityUnsupported = -32803;

    enum class State {
        NotStarted, // start() never called
        Starting,   // transport up or coming up, initialize pending
        Ready,      // initialized sent; features may run
        Stopping,   // the two-phase stop barrier is in flight
        Stopped,    // clean stop completed (terminal; start() usable again)
        Failed,     // handshake failed, OR died with the auto-restart spent
    };
    Q_ENUM(State)

    explicit LspClient(TransportFactory factory, QObject* parent = nullptr);
    ~LspClient() override;

    // Glue passes the FULL initialize params object (machine 4's matrix
    // templates; machine 2's tests hand their own). Params are stored: the
    // auto-restart replays them.
    void start(const QJsonObject& initializeParams);
    void stop();
    // Manual door: resets the auto-restart budget. From Failed/Stopped it
    // builds a FRESH session (new factory call); from Ready it is a no-op.
    void restartServer();

    State state() const { return state_; }
    bool isReady() const { return state_ == State::Ready; }

    // The initialize RESULT facts: the capabilities object + the position
    // encoding negotiation (declare "utf-16", the server may answer utf-8 —
    // machine 3's position converter consumes this door).
    QJsonObject serverCapabilities() const { return serverCapabilities_; }
    QString positionEncoding() const { return positionEncoding_; }

    // Feature-door passthrough (machine 5 rides these; the raw-RpcClient
    // hazard comment above is why NO rpc() door exists): a request sent in a
    // non-Ready state answers exactly once with local kErrNotRunning — the
    // honest rejection, never a silently unfulfilled promise.
    qint64 request(const QString& method, const QJsonValue& params,
                   qmn::protocol::RpcClient::ReplyHandler handler,
                   int timeoutMs = qmn::protocol::RpcClient::kFeatureTimeoutMs);
    void notify(const QString& method,
                const QJsonValue& params = QJsonValue(QJsonValue::Undefined));

    // The $/cancelRequest passthrough (the M8 close-train popup trigger
    // supersedes its own in-flight requests): forwards to the live
    // session's RpcClient::cancel — the wire notification plus the immediate
    // LOCAL -32802 fulfillment, the late server reply then dropped. A no-op
    // when no live session can take it (the pending requests died with it).
    void cancel(qint64 requestId);

    // The capability registry (PLAN-decided, the real thing): true while at
    // least one live registration of the LSP method exists. initialize
    // capabilities are NOT merged here (statics are feature gates, machine 5).
    bool isRegistered(const QString& method) const;

    // Glue answers the applyEdit request (machine 5's applier does).
    void respondApplyEdit(const QJsonValue& id, bool applied);
    // Glue answers a window/showMessageRequest with the chosen MessageActionItem
    // (an object carrying "title"); Null = dismissed. Ordering is the point:
    // serverShowMessageRequest fires BEFORE this layer's own Null default, and
    // RpcClient never sends a SECOND answer to one id — so glue that answers
    // from inside the signal wins, and glue that does not gets the default.
    // The bool (car E machine E3's widening, DapClient::respond's precedent)
    // says whether THIS call was the answer that went out: false = no session,
    // or the id was already answered (the default or an earlier answer won —
    // the first-answer-wins policy made observable).
    bool respondShowMessageRequest(const QJsonValue& id,
                                   const QJsonValue& actionItem);

signals:
    // Fires on EVERY (re-)READY — the syncer's re-didOpen self-heal trigger.
    void ready();
    // Unwanted death with the restart budget spent, or a failed handshake.
    void failed(const QString& reason);
    // window/showMessage (type = MessageType int) — PLAN: a SIGNAL, visible,
    // never a debug print.
    void serverShowMessage(int type, const QString& message);
    // window/logMessage AND $/logTrace share this channel.
    void serverLogMessage(int type, const QString& message);
    // The transport's stderr line — the STDERR log channel, kept SEPARATE
    // from the PROTOCOL channel (PLAN-pinned per-server logs).
    void serverStderr(const QString& line);
    // textDocument/publishDiagnostics (M4, push): relayed VERBATIM (the
    // params object {uri, diagnostics[]}) — the lsp layer stays text-free
    // (DocumentSyncer owns the only mirror; decoding to view Diagnostics is
    // the glue's PositionConverter turn).
    void diagnosticsReceived(const QJsonObject& params);
    // $/progress (token is string-or-number, passed as QJsonValue).
    void serverProgress(const QJsonValue& token, const QJsonValue& value);
    // workDoneProgress/create: answered null, glue may track the token.
    void createWorkDoneProgress(const QJsonValue& token);
    // window/showMessageRequest: delivered BEFORE this layer's Null default,
    // so glue that shows real buttons wins the answer (the first answer to an
    // id is the one that travels — RpcClient never sends a second).
    void serverShowMessageRequest(const QJsonValue& id, const QJsonValue& params);
    void registrationsChanged();
    // workspace/applyEdit: the applier's turn (machine 5), answer with
    // respondApplyEdit(id, applied).
    void serverApplyEdit(const QJsonValue& id, const QJsonValue& edit);

private:
    struct Session; // Transport+RpcClient pair, one heap block, session-scoped

    void beginSession();
    void endSession();
    void onOpened();
    void onReadyOrFailed(const qmn::protocol::RpcClient::Reply& reply,
                         const QString& reasonIfError);
    void onGone();
    void enterFailed(const QString& reason);

    void registerPolicyHandlers(qmn::protocol::RpcClient* rpc);
    void handleRegisterCapability(qmn::protocol::RpcClient* rpc,
                                  const QJsonObject& request);
    void handleUnregisterCapability(qmn::protocol::RpcClient* rpc,
                                    const QJsonObject& request);

    TransportFactory factory_;
    Session* session_ = nullptr;

    QJsonObject initializeParams_;
    State state_ = State::NotStarted;
    int autoRestartsLeft_ = 1;
    bool restartPending_ = false;
    bool sessionOpened_ = false; // this session's transport opened() once

    QJsonObject serverCapabilities_;
    QString positionEncoding_; // the initialize RESULT's value ("" = utf-16)
    QHash<QString, int> registrations_; // method -> live registration count
    QString lastTransportIssue_; // the last transportError() text, for FAILED
};

} // namespace qmn::lsp
