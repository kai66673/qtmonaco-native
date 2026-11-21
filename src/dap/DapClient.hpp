#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>

#include "dap/DapCodec.hpp"
#include "protocol/RpcClient.hpp"
#include "protocol/Transport.hpp"

namespace qmn::dap {

// The DAP adapter client (PLAN car 4, machine D2). Layer rule: this layer
// depends ONLY on src/protocol (+ its own DapCodec); never view, never
// core, never the lsp layer — DapClient and LspClient are SIBLINGS sharing
// the RpcClient mechanism, the iide "Lsp/Dap RPC duplication" anti-pattern
// we deliberately never build.
//
// Same SESSION shape as LspClient: the Transport+RpcClient pair is created
// and destroyed together through the factory seam (M1's pair-rewiring
// hazard, and ProcessTransport is single-session by design). Lazy start is
// the GLUE's decision (D3's DapHub); start() runs the pinned handshake:
// `initialize` REQUEST (RpcClient's 10 s budget) -> on success the
// `initialized` EVENT (DAP's outbound one-way, riding RpcClient::notify —
// the frame shape is DapCodec's), and only then Ready.
//
// THE LIFECYCLE DIFFERENCE from LspClient, PLAN-decided: NO auto-restart
// for debug adapters. A crashed compiler-server is background noise; a
// crashed ADAPTER just killed the user's debug session, and silently
// respawning mid-session resurrects a process whose whole breakpoint/
// thread world is gone (lldb-dap/gdb each have their own restart quirks —
// that rabbit hole is exactly what PLAN keeps out of v1). An unwanted
// death in Starting/Ready lands in Failed + failed(reason); restartServer()
// is the manual door (and D4's demo shows it).
//
// The server->client side is a RELAY, not an enumerator (the signal-shape
// decision from the design pass): DAP events are many and grow with spec
// versions, so every event becomes ONE dapEvent(name, body) signal —
// unknown events reaching nobody is the spec's own ignore rule. Adapter
// reverse REQUESTS (runInTerminal/startDebugging/...) relay as
// dapRequest(request); glue answers through respond()/respondError(), and
// a reverse request nobody claims gets an honest in-envelope failure the
// moment the signal returns (the first-answer-wins ordering LspClient's
// showMessageRequest established). The progress trio answers {} via the
// RpcClient static shelf — real progress UI is out of car 4's scope.
//
// Inbound `cancel` (adapter -> us) is handled per spec: claim it, fulfill
// the pending locally with RpcClient::kErrCancelledByPeer, answer
// success:true — or answer the honest "cannot be cancelled" failure.
//
// The std::function factory stays C++-only (the TransportFactory seam
// precedent from G3: the wheel wraps the glue doors, not transport
// plumbing); every DOOR here speaks plain Qt types so D3's shelf can be
// Shiboken-shaped without surgery.
class DapClient : public QObject {
    Q_OBJECT
public:
    // Transport factory: parent the Transport under `parent` (this client)
    // so the session teardown deletes it. Called ONLY by start() and
    // restartServer().
    using TransportFactory =
        std::function<qmn::protocol::Transport*(QObject* parent)>;

    enum class State {
        NotStarted, // start() never called
        Starting,   // transport up or coming up, initialize pending
        Ready,      // initialized sent; commands may run
        Stopping,   // the disconnect barrier is in flight
        Stopped,    // clean stop completed (terminal; start() usable again)
        Failed,     // handshake failed, or the session died (NO restarts)
    };
    Q_ENUM(State)

    // adapterID: the DAP initialize argument naming the adapter type
    // (required by the spec; the hub's ServerSpec carries it). initialize
    // arguments from start() may override the three client-suggested keys.
    explicit DapClient(TransportFactory factory, const QString& adapterId,
                       QObject* parent = nullptr);
    ~DapClient() override;

    // The initialize ARGUMENTS object (the PLAN D2 bundle filled when
    // absent: adapterID + the clientID/clientName/clientOS/locale identity
    // + the linesStartAt1/columnsStartAt1 pair = true, the spec's own
    // default the glue's ±1 conversions lean on; NO clientCapabilities
    // member — honest absence. Everything else — pathFormat, adapterID
    // overrides... — is the caller's, replayed on restart).
    void start(const QJsonObject& initializeArguments = {});
    void stop();
    // The manual restart door (the ONLY restart route): re-arms a fresh
    // session from Failed/Stopped and replays the stored arguments. From
    // Ready/Starting it is a no-op (no silent mid-session respawn — the
    // no-auto-restart decision's other half).
    void restartServer();

    State state() const { return state_; }
    bool isReady() const { return state_ == State::Ready; }

    // The initialize RESULT object verbatim — on this wire the ADAPTER
    // CAPABILITIES ARE the result (supportsConfigurationDoneRequest,
    // supportsTerminateRequest, ...), no nested "capabilities" member. A
    // `capabilities` EVENT RE-STORES it (adapters publish revisions that
    // way) while the event itself still rides the dapEvent relay.
    QJsonObject adapterCapabilities() const { return adapterCapabilities_; }

    // Feature-door passthrough (D3 rides these): a command sent in a
    // non-Ready state answers exactly once with local kErrNotRunning.
    // timeoutMs: DAP has no per-command budgets in the spec; RpcClient's
    // feature default applies unless the caller names one.
    qint64 request(const QString& command, const QJsonValue& arguments,
                   qmn::protocol::RpcClient::ReplyHandler handler,
                   int timeoutMs = qmn::protocol::RpcClient::kFeatureTimeoutMs);
    // Outbound EVENT frame (the handshake's initialized is the client
    // layer's own; glue uses this door for nothing in v1 — pinned open for
    // progress/notification-shaped adapters).
    void event(const QString& name,
               const QJsonValue& body = QJsonValue(QJsonValue::Undefined));
    // Outbound `cancel` REQUEST for one of our pendings (the wire frame is
    // DapCodec's; the immediate LOCAL answer is RpcClient::cancel's —
    // DAP's own success/false bookkeeping for the cancel's own response
    // lands on the unmatched-ignore path, per the D1 pin).
    void cancel(qint64 requestId);

    // The reverse-request answer doors (glue's first turn, first answer
    // wins). The id to answer with is the REQUEST's seq member, straight
    // out of the dapRequest payload: respond(request.value("seq"), ...).
    bool respond(const QJsonValue& id, const QJsonValue& body);
    bool respondError(const QJsonValue& id, int code, const QString& message,
                      const QJsonValue& data = QJsonValue(QJsonValue::Undefined));

signals:
    // Fires on EVERY (re-)READY — D3's hub re-pushes breakpoints on it.
    void ready();
    // Handshake failed, or the session died (no restart budget exists).
    void failed(const QString& reason);
    // ALL adapter events, generic relay: name = the `event` member
    // ("output", "terminated", "stopped", "breakpoint", ...), body = the
    // `body` member verbatim (QJsonValue:Undefined when absent).
    void dapEvent(const QString& name, const QJsonValue& body);
    // ALL unclaimed adapter->client requests, verbatim (command/arguments/
    // seq inside). Answer via respond()/respondError(id = request's seq)
    // DURING the signal to win against this layer's honest default.
    void dapRequest(const QJsonObject& request);
    // The transport's stderr line — the separate STDERR channel (the LSP
    // layer's PLAN-pinned channel split carries across cars).
    void serverStderr(const QString& line);

private:
    struct Session; // Transport+RpcClient pair, one heap block, session-scoped

    void beginSession();
    void endSession();
    void onOpened();
    void onHandshakeReply(const qmn::protocol::RpcClient::Reply& reply);
    void onGone();
    void enterFailed(const QString& reason);
    void registerPolicyHandlers(qmn::protocol::RpcClient* rpc);

    TransportFactory factory_;
    DapCodec codec_; // BEFORE any RpcClient that points at it (member
                     // order = lifetime order; the codec is not owned)
    Session* session_ = nullptr;

    QString adapterId_;
    QJsonObject initializeArguments_;
    State state_ = State::NotStarted;
    bool sessionOpened_ = false; // this session's transport opened() once
    QJsonObject adapterCapabilities_; // the initialize RESULT (cleared on
                                      // every beginSession: a fresh
                                      // handshake re-reads it)
    QString lastTransportIssue_; // the last transportError() text, for FAILED
};

} // namespace qmn::dap
