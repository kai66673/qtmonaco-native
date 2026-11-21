#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace qmn::protocol {

// The envelope half of RpcClient (PLAN car 4, machine D1): everything
// RpcClient MECHANICALLY does (id allocation, the pending map, exactly-
// once fulfilment, per-door budgets, cancel-then-ignore-late, the two-
// phase stop, the gone-latch) is envelope-agnostic; what is NOT is the
// WIRE SHAPE — JSON-RPC 2.0's `jsonrpc/method/params/id/result/error`
// side versus DAP's `seq/type/command/arguments/request_seq/success/body`
// side. One mechanism, two codecs: the iide docs flag their own Lsp/Dap
// RPC duplication as a RECOMMENDATION to fix — this interface is why we
// never build that duplicate.
//
// A codec is a STATELESS policy object: RpcClient assigns the ids/seqs
// (its own counter is per-connection and monotonic — DAP seqs are per-
// direction and monotonic, the same counter fits) and passes the value
// into the builders. The codec is NOT owned: the caller keeps it alive
// for at least as long as the RpcClient (the default JsonRpcCodec is a
// process-wide const object; DapClient will hold its codec as a member
// declared BEFORE the RpcClient it feeds).
//
// Layer rule: QtCore only, never view/core (this file lives with its
// user).

// The shared reply record RpcClient carries (RpcClient::Reply is this
// type; the nested name predates the seam and stays for consumers).
struct EnvelopeReply {
    bool isError = false;
    QJsonValue result; // meaningful when !isError
    int code = 0;      // codec-owned wire code, or RpcClient's local kErr*
    QString message;
    QJsonValue data;   // wire error payload, when present (LSP "data",
                       // DAP's whole "error" table object)
};

class EnvelopeCodec {
public:
    virtual ~EnvelopeCodec() = default;

    // --- outbound builders ------------------------------------------------
    // `idOrSeq` is the value RpcClient just allocated (request() and the
    // stop barrier's shutdown/disconnect ride the same counter). Params
    // spelling (omit-vs-null) belongs to the envelope, so the params
    // value arrives untouched.
    virtual QJsonObject buildRequest(qint64 idOrSeq, const QString& method,
                                     const QJsonValue& params) const = 0;
    // The outbound ONE-WAY message: JSON-RPC's notification carries no id
    // of its own (seq stays 0, the counter must not move — LSP byte-
    // identity); DAP's counterpart is the `initialized` EVENT, and every
    // DAP message is seqed, so RpcClient allocates and passes the seq
    // exactly when responsesCarrySeq() is true (machine D2's correction to
    // the D1 note "an empty buildNotification means no outbound one-way
    // exists": DAP DOES have one — the handshake's initialized event —
    // while the empty-answer skip rule stays for the other builders).
    virtual QJsonObject buildNotification(const QString& method,
                                          const QJsonValue& params,
                                          qint64 seq) const = 0;
    // Answers to an INBOUND request: the full incoming object is handed
    // over because DAP must echo `command` and the inbound seq, and an
    // envelope may derive its id member from the request (JSON-RPC's id).
    virtual QJsonObject buildResponse(const QJsonObject& incomingRequest,
                                      const QJsonValue& result,
                                      qint64 selfSeq) const = 0;
    virtual QJsonObject buildErrorResponse(const QJsonObject& incomingRequest,
                                           int code, const QString& message,
                                           const QJsonValue& data,
                                           qint64 selfSeq) const = 0;
    // The MECHANICAL answers (the M1 tripwires: these ids were never
    // entered in openIncoming_, so no respond() guard protects them):
    // unregistered method/command, and an inbound id/seq the envelope
    // cannot carry. An EMPTY object from buildInvalidIdAnswer means "no
    // mechanical answer exists" (the classify pass already routed such
    // inbound elsewhere).
    virtual QJsonObject buildUnregisteredAnswer(const QJsonObject& request,
                                                qint64 selfSeq) const = 0;
    virtual QJsonObject buildInvalidIdAnswer(const QJsonObject& request) const = 0;
    // Cancel one of OUR pending requests: LSP answers with the
    // $/cancelRequest NOTIFICATION (selfSeq unused); DAP has the `cancel`
    // REQUEST — a request carries its own seq, which RpcClient allocates
    // exactly when cancelIsRequest() is true (the cancel's own response
    // then lands on the unmatched-pending ignore path — never tracked).
    virtual QJsonObject buildCancel(qint64 targetId, qint64 selfSeq) const = 0;
    virtual bool cancelIsRequest() const { return false; }
    // The INBOUND cancel vocabulary (machine D2): the command/method name
    // an inbound cancellation arrives under (JSON-RPC's $/cancelRequest
    // NOTIFICATION vs DAP's `cancel` REQUEST — classify routes both onto
    // the kind they are; RpcClient recognizes the NAME via this door and
    // hands the decoded target to its client layer), and the extractor for
    // the request-to-cancel (JSON-RPC: params.id; DAP: arguments.requestId).
    // A non-numeric/absent target decodes to 0 ("nothing to cancel" — the
    // client layer's answer must then be the honest refusal).
    virtual QString cancelCommand() const { return QStringLiteral("$/cancelRequest"); }
    virtual qint64 cancelTargetId(const QJsonObject& message) const
    {
        return message.value(QStringLiteral("params")).toObject()
            .value(QStringLiteral("id")).toVariant().toLongLong();
    }
    // Outbound RESPONSES that consume one of our own seqs (DAP: every
    // message is seqed) versus those that carry no id of their own
    // (JSON-RPC: the response only mirrors the request id — the counter
    // must NOT move, or every later LSP request id would drift: byte-
    // identity of the LSP wire is a machine-D1 tripwire).
    virtual bool responsesCarrySeq() const { return false; }

    // --- the two-phase stop barrier's vocabulary ---------------------------
    // Phase 1 is a request on both wires (LSP `shutdown` params-null; DAP
    // `disconnect` arguments-restart-false) with RpcClient's own
    // kShutdownWaitMs budget; phase 2's final message is envelope-owned
    // (LSP's `exit` notification; DAP has no exit message — the kill
    // escalation is the Transport's and stays there).
    virtual QString shutdownCommand() const = 0;
    virtual QJsonValue shutdownParams() const = 0;
    virtual bool hasExitNotification() const { return false; }
    virtual QString exitCommand() const { return {}; }

    // --- inbound classification --------------------------------------------
    // The pipeline feeds a PARSED JSON object (handlePayload already
    // surfaced broken JSON); the codec routes it:
    //   Response     -> the pending map side (handleResponse)
    //   Request      -> requestHandlers_ by `method` (LSP method / DAP
    //                   command), id-carrying
    //   Notification -> notificationHandlers_ by `method` — a DAP
    //                   `type:"event"` normalizes onto this SAME shape
    //                   (method := the `event` name, params := the
    //                   `body`), so the unknown-notification-ignored rule
    //                   carries across envelopes untouched
    //   NotEnvelope  -> malformedMessage(why, raw) — the envelope decides
    //                   what an unrecognizable message IS
    struct Incoming {
        enum class Kind { Response, Request, Notification, NotEnvelope };
        Kind kind = Kind::NotEnvelope;
        QString method;   // the dispatch key (empty for Response)
        QJsonValue id;    // inbound request id (Undefined for the rest)
        QJsonValue params; // payload for handlers (params/arguments/body)
        QString why;      // NotEnvelope: the reason, surfaced verbatim
    };
    virtual Incoming classify(const QJsonObject& message) const = 0;

    // --- the response side of the pending map ------------------------------
    // Split shape-then-key to pin the JSON-RPC ORDER the machine-1 tests
    // grew up with (a frame with neither result nor error reports that
    // first, whatever its id looks like); decodeReply only ever sees
    // messages whose key already matched a live pending.
    virtual bool responseShapeOk(const QJsonObject& message,
                                 QString* why) const = 0;
    virtual bool responseMatchId(const QJsonObject& message, qint64* id,
                                 QString* why) const = 0;
    virtual bool decodeReply(const QJsonObject& message, EnvelopeReply* out,
                             QString* why) const = 0;
};

} // namespace qmn::protocol
