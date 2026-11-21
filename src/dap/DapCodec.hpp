#pragma once

#include "protocol/EnvelopeCodec.hpp"

namespace qmn::dap {

// The Debug Adapter Protocol envelope (PLAN car 4, machine D2): the SECOND
// EnvelopeCodec policy, living where PLAN's layer rule puts car 4 —
// src/dap depends ONLY on src/protocol (never view, never core, never the
// lsp layer: the two envelope layers are siblings and share only the
// RpcClient mechanism underneath).
//
// DAP vs JSON-RPC, per-message: every DAP message is seqed and typed
// (`seq/type/command/arguments` requests, `request_seq/success/body`
// responses — keyed by the REQUEST's seq, not a mirrored id — and one-way
// `event` frames where JSON-RPC has notifications). The RpcClient side of
// the adaptation (its counter feeding seqs, responsesCarrySeq(), event
// frames riding notify()) is documented at the EnvelopeCodec interface;
// this class is the wire spelling, spec-faithful:
//
//  - success:false answers carry `error:{id,format}` — the adapter's
//    error.id table is POSITIVE and adapter-specific (we answer 0, "no
//    such code", and never invent entries);
//  - cancel is a REQUEST (`cancel` with arguments.requestId), and an
//    inbound one must be ANSWERED (unlike JSON-RPC's $/cancelRequest
//    notification) — the hook + response shape is DapClient's policy, the
//    frames are here;
//  - there is no exit message: the stop barrier is RpcClient's
//    `disconnect` request (arguments restart:false), then the
//    Transport's teardown (hasExitNotification() false).
//
// The test-local TestDapCodec in tst_dap.cpp (machine D1) is this class's
// ancestor minus the spec-shaped argument/body rules; the D1 live slots
// keep using it to pin that the RpcClient MECHANISM is policy-free (it
// would ride any codec), while every slot below D2 pins THIS one.
class DapCodec : public qmn::protocol::EnvelopeCodec {
public:
    // Outbound frames. `seq` is RpcClient's connection counter (every DAP
    // message consumes one, both directions carry their own — the
    // responsesCarrySeq() contract). Undefined arguments/body members are
    // OMITTED, the shape real adapters emit (a bare command with no
    // arguments travels without the member; the spec's defaults cover it).
    QJsonObject buildRequest(qint64 seq, const QString& command,
                             const QJsonValue& arguments) const override;
    // The outbound one-way is the EVENT frame: the handshake's
    // `initialized` event is why this exists (the D2 correction to the
    // D1 note about empty one-ways — DAP HAS one; what it lacks is
    // JSON-RPC-style notifications, so nothing else calls this).
    QJsonObject buildNotification(const QString& event,
                                  const QJsonValue& body,
                                  qint64 seq) const override;
    // Responses echo the inbound `seq` as request_seq and the `command`
    // (the pairing key on this wire).
    QJsonObject buildResponse(const QJsonObject& incomingRequest,
                              const QJsonValue& body,
                              qint64 selfSeq) const override;
    // `data` (when an object) MERGES into the error table — DAP's error
    // object carries the whole record ({id,format,showCrash,...}); there
    // is no LSP-style nested "data" member on this wire.
    QJsonObject buildErrorResponse(const QJsonObject& incomingRequest,
                                   int code, const QString& message,
                                   const QJsonValue& data,
                                   qint64 selfSeq) const override;
    // The mechanical answers: an unregistered command gets an honest
    // success:false ("Unknown request: <command>", error.id 0 — we invent
    // no code); a request frame without a numeric seq is refused by
    // classify (NotEnvelope -> malformedMessage), so THIS builder has no
    // answer to spell and stays empty (the writeAnswer skip rule).
    QJsonObject buildUnregisteredAnswer(const QJsonObject& request,
                                        qint64 selfSeq) const override;
    QJsonObject buildInvalidIdAnswer(const QJsonObject& request) const override;
    QJsonObject buildCancel(qint64 targetId, qint64 selfSeq) const override;
    bool cancelIsRequest() const override { return true; }
    QString cancelCommand() const override { return QStringLiteral("cancel"); }
    qint64 cancelTargetId(const QJsonObject& message) const override;
    bool responsesCarrySeq() const override { return true; }

    QString shutdownCommand() const override { return QStringLiteral("disconnect"); }
    QJsonValue shutdownParams() const override; // arguments {restart:false}
    bool hasExitNotification() const override { return false; } // no exit frame

    Incoming classify(const QJsonObject& message) const override;

    bool responseShapeOk(const QJsonObject& message,
                         QString* why) const override;
    bool responseMatchId(const QJsonObject& message, qint64* id,
                         QString* why) const override;
    // success:true -> body = the result (absent = an empty Undefined
    // result, honest for body-less commands); success:false -> isError,
    // code = error.id (0 when the adapter omits it — the spec's own
    // default, NEVER a local kErr* mask), message = error.format,
    // data = the WHOLE error table.
    bool decodeReply(const QJsonObject& message,
                     qmn::protocol::EnvelopeReply* out,
                     QString* why) const override;
};

} // namespace qmn::dap
