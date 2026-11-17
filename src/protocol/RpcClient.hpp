#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>

#include "protocol/JsonFraming.hpp"
#include "protocol/Transport.hpp"

class QJsonDocument;

namespace qmn::protocol {

// JSON-RPC 2.0 client over a Transport + JsonFraming (PLAN.md GATE 2 / M8,
// machine 1). It carries the mechanism half of the M8 contracts:
//  - monotonic id allocation + a pending map, every pending fulfilled
//    EXACTLY ONCE (success, wire error, timeout, cancel, or eviction);
//  - per-door timeouts, pinned by PLAN: initialize 10 s, feature doors 5 s
//    (never a blanket timeout — the iide anti-pattern);
//  - $/cancelRequest on supersede (cancel()) and on timeout;
//  - write errors SURFACE: the pre-open byte queue is the Qt device's own
//    queue, and every failure of it lands in transportError()/closed()
//    which RpcClient turns into honest reply errors + transportGone();
//  - the two-phase stop barrier: shutdown request, bounded wait, exit
//    notification, then transport teardown (the kill escalation lives in
//    the Transport);
//  - server->client "always answer, never lie": a request for an
//    UNREGISTERED method is answered -32601 mechanically; the policy half
//    (what to register) is LspClient's M2 job. Unknown NOTIFICATIONS are
//    ignored, per spec.
//
// Client-side-only failure codes NEVER masquerade as LSP wire codes (iide
// anti-pattern: "fake -32801"). Wire codes that ARE spec codes stay in
// kWire* below.
//
// Layer rule: QtCore + QtNetwork only; this class never knows
// CodeEditor/CodeDocument. The Transport is NOT owned — the caller (the
// M2/M8 glue) starts it, owns its life and may re-arm this client by
// starting a new transport session (opened() re-arms the gone-latch).
class RpcClient : public QObject {
    Q_OBJECT
public:
    static constexpr int kInitializeTimeoutMs = 10'000; // PLAN-pinned
    static constexpr int kFeatureTimeoutMs = 5'000;     // PLAN-pinned
    // Bounded wait of stop() phase 1 (the shutdown reply). Not PLAN-pinned;
    // a teardown courtesy window, independent of the request budgets.
    static constexpr int kShutdownWaitMs = 2'000;

    // LSP/JSON-RPC wire codes we produce (spec values, never invented):
    static constexpr int kWireMethodNotFound = -32601;
    static constexpr int kWireInvalidRequest = -32600;
    static constexpr int kWireParseError = -32700;
    static constexpr int kWireRequestCancelled = -32802; // the REAL LSP code
    // RpcClient-local only (the negative -33xxx band is no one's wire code):
    static constexpr int kErrTimeout = -33100;
    static constexpr int kErrTransportClosed = -33101;
    static constexpr int kErrWriteFailed = -33102;
    static constexpr int kErrNotRunning = -33103;

    struct Reply {
        bool isError = false;
        QJsonValue result; // meaningful when !isError
        int code = 0;      // wire kWire* or local kErr*
        QString message;
        QJsonValue data;   // wire error "data", when present
    };
    using ReplyHandler = std::function<void(const Reply&)>;

    // Server->client requests are delivered to registered handlers; the
    // handler must eventually call respond()/respondError() with the
    // request's id value (the "always answer" duty). Requests for
    // unregistered methods never reach a handler: RpcClient answers -32601.
    using RequestHandler = std::function<void(const QJsonObject& request)>;
    using NotificationHandler = std::function<void(const QJsonValue& params)>;

    enum class StopPhase { Idle, Stopping, Stopped };
    Q_ENUM(StopPhase)

    explicit RpcClient(Transport* transport, QObject* parent = nullptr);

    void setRequestHandler(const QString& method, RequestHandler handler);
    void setNotificationHandler(const QString& method, NotificationHandler handler);

    // Sends a request; handler fires exactly once, timeoutMs is the per-door
    // budget (pass kInitializeTimeoutMs for initialize, default is the
    // feature budget). The returned id can be passed to cancel().
    qint64 request(const QString& method, const QJsonValue& params,
                   ReplyHandler handler, int timeoutMs = kFeatureTimeoutMs);

    void notify(const QString& method, const QJsonValue& params = QJsonValue());

    // $/cancelRequest to the server + immediate LOCAL fulfillment of that
    // pending request with kWireRequestCancelled (never fires twice); a
    // late server response to the same id is then ignored silently.
    void cancel(qint64 requestId);

    // Answer one server->client request (the id value is taken from the
    // incoming request object). Returns false for an unknown/already-
    // answered id (double answers are never sent).
    bool respond(const QJsonValue& id, const QJsonValue& result);
    // data defaults to UNDEFINED (= the wire "data" member is omitted);
    // note Qt 6's QJsonValue() default-constructs to NULL, not undefined.
    bool respondError(const QJsonValue& id, int code, const QString& message,
                      const QJsonValue& data = QJsonValue(QJsonValue::Undefined));

    // The two-phase barrier, see file header. Idempotent; after the phase-1
    // wait (or an earlier transport death) it sends exit and tears the
    // transport down. New request() calls answer kErrTransportClosed once
    // stop() has begun.
    void stop();

    StopPhase stopPhase() const { return stopPhase_; }
    int pendingCount() const { return pending_.size(); }
    // (named isGone, not transportGone: the SIGNAL of that name is already
    // this object's member — a same-name getter would shadow-overload it)
    bool isGone() const { return goneFired_; }

signals:
    // One of closed()/transportError() was seen: every pending was fulfilled
    // with kErrTransportClosed (or kErrWriteFailed for a write failure).
    void transportGone();
    // A frame arrived whose bytes are not valid JSON or not a JSON-RPC
    // envelope: counted + surfaced, the transport STAYS open (framing is
    // still synchronized).
    void malformedMessage(const QString& text, const QByteArray& rawPayload);
    // The framing stream desynchronized (bad header block / oversize bound):
    // the decoder is dead and the transport was asked to stop; pendings die
    // with the transport.
    void protocolError(const JsonFraming::Error& error);

private:
    struct Pending {
        ReplyHandler handler;
        QTimer* timer = nullptr;
        QString method;
    };

    static QJsonObject envelope(const QString& method, const QJsonValue& params);
    static QString canonicalId(const QJsonValue& id); // "n:<int>"/"s:<str>"/"bad"
    QByteArray encode(const QJsonObject& message) const;

    void onBytes(const QByteArray& bytes);
    void onOpened();
    void onClosed();
    void onTransportError(Transport::ErrorKind kind, const QString& text);
    void handlePayload(const QByteArray& payload);
    void handleIncoming(const QJsonObject& message);
    void handleResponse(const QJsonObject& message);

    void fulfill(qint64 id, const Reply& reply);      // drops the pending first
    void failAllPending(int code, const QString& message);
    void markGone(int code, const QString& message);  // gone-latch bookkeeping
    // The wire response writer, shared by respondError (guard-checked) and
    // the MECHANICAL answers (-32600/-32601) whose ids were never entered in
    // openIncoming_ — the guard would refuse those writes (the M1 tripwire
    // test caught exactly this: the auto -32601 never reached the wire).
    void writeErrorResponse(const QJsonValue& id, int code,
                            const QString& message, const QJsonValue& data);
    qint64 sendRequestRaw(const QString& method, const QJsonValue& params,
                          ReplyHandler handler, int timeoutMs);
    void finishStop();

    Transport* transport_;
    JsonFraming framing_;

    qint64 nextId_ = 1;
    QHash<qint64, Pending> pending_;
    QHash<QString, RequestHandler> requestHandlers_;
    QHash<QString, NotificationHandler> notificationHandlers_;
    QSet<QString> openIncoming_; // server->client requests still unanswered

    StopPhase stopPhase_ = StopPhase::Idle;
    bool goneFired_ = false;
};

} // namespace qmn::protocol

Q_DECLARE_METATYPE(qmn::protocol::JsonFraming::Error)
