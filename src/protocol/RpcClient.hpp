#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include <functional>

#include "protocol/EnvelopeCodec.hpp"
#include "protocol/JsonFraming.hpp"
#include "protocol/Transport.hpp"

class QJsonDocument;

namespace qmn::protocol {

// RpcClient — the monotonic-id/pending-map/timeout machinery over a
// Transport + JsonFraming (PLAN.md GATE 2 / M8, machine 1), with the
// JSON-RPC 2.0 envelope as the DEFAULT policy (JsonRpcCodec, D1 seam).
// It carries the mechanism half of the M8 contracts:
//  - monotonic id allocation + a pending map, every pending fulfilled
//    EXACTLY ONCE (success, wire error, timeout, cancel, or eviction);
//  - per-door timeouts, pinned by PLAN: initialize 10 s, feature doors 5 s
//    (never a blanket timeout — the iide anti-pattern);
//  - $/cancelRequest on supersede (cancel()) and on timeout;
//  - write errors SURFACE: the pre-open byte queue is the Qt device's own
//    queue, and every failure of it lands in transportError()/closed()
//    which RpcClient turns into honest reply errors + transportGone();
//  - the two-phase stop barrier: the codec's stop-phase-1 request (LSP
//    `shutdown`, DAP `disconnect`), bounded wait, the codec's final
//    message (LSP's `exit` notification; DAP has none), then transport
//    teardown (the kill escalation lives in the Transport);
//  - server->client "always answer, never lie": a request for an
//    UNREGISTERED method is answered mechanically — the answer SHAPE is
//    codec-owned (JSON-RPC's -32601; DAP's success:false "Unknown
//    request"); the policy half (what to register) is LspClient's M2 job.
//    Unknown NOTIFICATIONS are ignored, per spec (a DAP `event` lands on
//    this same dispatch, machine D1 normalization).
//
// Client-side-only failure codes NEVER masquerade as LSP wire codes (iide
// anti-pattern: "fake -32801"). Wire codes that ARE spec codes stay in
// kWire* below — and are JSON-RPC's ONLY: since machine D1 the reply codes
// are codec-owned (the EnvelopeCodec file header), so the DAP envelope's
// positive error.id table and these negatives never mix.
//
// Layer rule: QtCore + QtNetwork only; this class never knows
// CodeEditor/CodeDocument. The Transport is NOT owned — the caller (the
// M2/M8 glue) starts it, owns its life and may re-arm this client by
// starting a new transport session (opened() re-arms the gone-latch).
//
// Machine D1 (PLAN car 4, the "same RpcClient" clause made literal): the
// wire envelope is a policy object (EnvelopeCodec), NOT this class. Every
// mechanism above is envelope-agnostic; the JSON-RPC builders that used to
// be private methods live in JsonRpcCodec and are the ctor DEFAULT, so
// LSP traffic stays byte-identical (the whole pre-existing tst_lsp binary
// is that claim's tripwire). A codec is NOT owned: it must outlive the
// client (the DapClient will declare its codec member BEFORE the RpcClient
// it feeds).
class RpcClient : public QObject {
    Q_OBJECT
public:
    static constexpr int kInitializeTimeoutMs = 10'000; // PLAN-pinned
    static constexpr int kFeatureTimeoutMs = 5'000;     // PLAN-pinned
    // Bounded wait of stop() phase 1 (the shutdown reply). Not PLAN-pinned;
    // a teardown courtesy window, independent of the request budgets.
    static constexpr int kShutdownWaitMs = 2'000;

    // LSP/JSON-RPC wire codes the DEFAULT codec produces (spec values, never
    // invented). Codec-owned since D1 (JsonRpcCodec's kMethodNotFound/
    // kInvalidRequest are these same numbers); kept here because tst_lsp and
    // the LSP layer name them, and the DAP codec's POSITIVE error.id table
    // never collides with this band (the never-conflated rule).
    static constexpr int kWireMethodNotFound = -32601;
    static constexpr int kWireInvalidRequest = -32600;
    static constexpr int kWireParseError = -32700;
    static constexpr int kWireRequestCancelled = -32802; // the REAL LSP code
    // RpcClient-local only (the negative -33xxx band is no one's wire code):
    static constexpr int kErrTimeout = -33100;
    static constexpr int kErrTransportClosed = -33101;
    static constexpr int kErrWriteFailed = -33102;
    static constexpr int kErrNotRunning = -33103;
    static constexpr int kErrCancelledByPeer = -33104; // the inbound-cancel
                                                       // fulfilment: the
                                                       // peer asked, we
                                                       // agree, locally

    // The shared reply record (EnvelopeReply): D1 made it the codec's type
    // so both envelopes fill one record; the nested name stays for every
    // consumer (RpcClient::Reply is an alias, brace-init unchanged).
    using Reply = EnvelopeReply;
    using ReplyHandler = std::function<void(const Reply&)>;

    // Server->client requests are delivered to registered handlers; the
    // handler must eventually call respond()/respondError() with the
    // request's id value (the "always answer" duty). Requests for
    // unregistered methods never reach a handler: RpcClient answers -32601.
    using RequestHandler = std::function<void(const QJsonObject& request)>;
    using NotificationHandler = std::function<void(const QJsonValue& params)>;
    // Catch-all for notifications/events NO specific handler claimed (the
    // D2 relay rides it: DapClient forwards every adapter event to glue as
    // ONE generic signal instead of enumerating the spec's event list — a
    // forward-compat choice, not an omission). Nobody sets this on a
    // JSON-RPC session, so the unknown-notification-ignored rule stays
    // exactly where the spec tests put it (an unset catch-all is the ignore).
    using AnyNotificationHandler =
        std::function<void(const QString& method, const QJsonValue& params)>;

    enum class StopPhase { Idle, Stopping, Stopped };
    Q_ENUM(StopPhase)

    // The codec is NOT owned (file header): the two-arg form takes a policy
    // object that must outlive this client (DapClient declares its codec
    // member BEFORE the RpcClient it feeds); the single-arg form is the
    // pre-D1 ctor and keeps the process-wide JsonRpcCodec default, which is
    // why the whole existing tst_lsp binary needs zero slot edits.
    explicit RpcClient(Transport* transport, QObject* parent = nullptr);
    explicit RpcClient(Transport* transport, const EnvelopeCodec* codec,
                       QObject* parent = nullptr);

    void setRequestHandler(const QString& method, RequestHandler handler);
    void setNotificationHandler(const QString& method, NotificationHandler handler);
    void setAnyNotificationHandler(AnyNotificationHandler handler);
    // Catch-all for server/adapter->client REQUESTS no specific handler
    // claimed: the request still gets the openIncoming_ registration (the
    // handler MUST answer via respond()/respondError — the same duty as a
    // registered handler), but the mechanical unregistered answer is
    // skipped. Set only by envelope layers whose command set is open-ended
    // (DapClient relays reverse requests); unset on JSON-RPC sessions, so
    // the M1 mechanical -32601 tripwire is untouched.
    void setAnyRequestHandler(RequestHandler handler);
    // Static method answers (machine D2, the relay-answer shelf): an
    // inbound request for a REGISTERED name is answered mechanically with
    // this canned result/failure, BEFORE requestHandlers_ is consulted and
    // instead of the unregistered answer. The DAP relay uses it for the
    // "we always agree" commands (runInTerminal echo); the LSP layer keeps
    // its own dynamic registerCapability registry (different bookkeeping —
    // counts, not answers).
    void registerMethod(const QString& name, const QJsonValue& result);
    void registerMethodFailure(const QString& name, int code,
                               const QString& message);
    void unregisterMethod(const QString& name);
    // Inbound cancellation hook (machine D2): when classify reports the
    // codec's cancelCommand() ($/cancelRequest notification / DAP cancel
    // request), the decoded target + the full message go HERE instead of
    // the notification catch-all or the request relay. The hook's duty is
    // the POLICY (fulfill the pending via cancelPending, then answer):
    // the DAP request must be answered (success:true/false), the JSON-RPC
    // notification must not. cancelPending() fulfills one of OUR pendings
    // with a local code and NO wire write (the peer already initiated —
    // RpcClient's own cancel() would write the outbound frame; here the
    // frame came FROM the peer). False = no such pending (the caller owes
    // the honest refusal answer).
    using CancelHandler =
        std::function<void(qint64 targetId, const QJsonObject& message)>;
    void setOnCancelRequested(CancelHandler handler);
    bool cancelPending(qint64 requestId, int code, const QString& message);

    // Sends a request; handler fires exactly once, timeoutMs is the per-door
    // budget (pass kInitializeTimeoutMs for initialize, default is the
    // feature budget). The returned id can be passed to cancel().
    qint64 request(const QString& method, const QJsonValue& params,
                   ReplyHandler handler, int timeoutMs = kFeatureTimeoutMs);

    void notify(const QString& method, const QJsonValue& params = QJsonValue());

    // Cancel one of OUR pending requests: the wire shape is codec-owned
    // (JSON-RPC: the $/cancelRequest notification; a codec whose
    // cancelIsRequest() is true sends the cancel as a REQUEST, and D2's
    // DAP cancel rides that door with its OWN local contract). The LOCAL
    // fulfillment below is JSON-RPC-shaped (the real -32802 code) — DAP
    // callers do NOT use this method, they cancel through the request door
    // with the adapter answering its cancel request. Never fires twice; a
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
    // Cancel-shape writer shared by cancel() and the timeout path (the
    // $/cancelRequest notification — or a codec's cancel REQUEST — is built
    // by the codec; untracked, its own reply lands on the unmatched-ignore
    // path).
    void sendCancel(qint64 targetId);
    // Sequence numbers for OUTBOUND messages that carry one of their own
    // (DAP responses: every DAP message is seqed; JSON-RPC responses carry
    // only the echoed request id, so JsonRpcCodec's responsesCarrySeq() is
    // false and this helper NEVER moves the counter — LSP id byte-identity
    // is machine D1's tripwire).
    qint64 outboundSeq() { return codec_->responsesCarrySeq() ? nextId_++ : 0; }
    // Write a codec-built answer to an INBOUND request (mechanical answers
    // included — their ids were never entered in openIncoming_, so the
    // respond() guard would refuse them; the M1 tripwire test caught
    // exactly this class of bug: the auto -32601 never reaching the wire).
    // An EMPTY answer object means "this envelope answers nothing here"
    // (buildInvalidIdAnswer's DAP spelling) and writes nothing.
    void writeAnswer(const QJsonObject& answer);
    qint64 sendRequestRaw(const QString& method, const QJsonValue& params,
                          ReplyHandler handler, int timeoutMs);
    void finishStop();

    Transport* transport_;
    const EnvelopeCodec* codec_;
    JsonFraming framing_;

    qint64 nextId_ = 1;
    QHash<qint64, Pending> pending_;
    QHash<QString, RequestHandler> requestHandlers_;
    QHash<QString, NotificationHandler> notificationHandlers_;
    // Server->client requests still unanswered, keyed by canonical id,
    // VALUE = the full incoming request object (DAP responses must echo
    // `command` and the inbound seq, not just the id; JSON-RPC reads its
    // id back out of the same object — byte-identical writes).
    QHash<QString, QJsonObject> openIncoming_;

    // Catch-alls + the static answer shelf (machine D2, all additive):
    // unset on JSON-RPC sessions, so the pre-D2 dispatch behavior — named
    // handlers, the mechanical unregistered answer, the unknown-
    // notification ignore — is exactly what the tst_lsp tripwire pins.
    AnyNotificationHandler anyNotification_;
    RequestHandler anyRequest_;
    CancelHandler cancelHandler_;
    struct MethodAnswer {
        bool isFailure = false;
        QJsonValue result; // when !isFailure
        int code = 0;      // when isFailure (codec-shaped on the wire)
        QString message;   // when isFailure
    };
    QHash<QString, MethodAnswer> methodAnswers_;

    StopPhase stopPhase_ = StopPhase::Idle;
    bool goneFired_ = false;
};

} // namespace qmn::protocol

Q_DECLARE_METATYPE(qmn::protocol::JsonFraming::Error)
