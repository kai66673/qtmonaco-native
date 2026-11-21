#include "dap/DapCodec.hpp"

#include <QJsonValue>

namespace qmn::dap {

using qmn::protocol::EnvelopeReply;

namespace {

// The one skeleton every response shares (spec: type/seq/request_seq/
// command — success rides per-builder).
QJsonObject baseResponse(const QJsonObject& incomingRequest, qint64 selfSeq)
{
    QJsonObject message;
    message.insert(QStringLiteral("type"), QStringLiteral("response"));
    message.insert(QStringLiteral("seq"), selfSeq);
    message.insert(QStringLiteral("request_seq"),
                   incomingRequest.value(QStringLiteral("seq")));
    message.insert(QStringLiteral("command"),
                   incomingRequest.value(QStringLiteral("command")));
    return message;
}

} // namespace

QJsonObject DapCodec::buildRequest(qint64 seq, const QString& command,
                                   const QJsonValue& arguments) const
{
    QJsonObject message;
    message.insert(QStringLiteral("type"), QStringLiteral("request"));
    message.insert(QStringLiteral("seq"), seq);
    message.insert(QStringLiteral("command"), command);
    if (!arguments.isUndefined())
        message.insert(QStringLiteral("arguments"), arguments);
    return message;
}

QJsonObject DapCodec::buildNotification(const QString& event,
                                        const QJsonValue& body,
                                        qint64 seq) const
{
    if (event.isEmpty())
        return {}; // no name, no event (the RpcClient skip rule: an empty
                   // builder answer writes NOTHING)
    QJsonObject message;
    message.insert(QStringLiteral("type"), QStringLiteral("event"));
    message.insert(QStringLiteral("seq"), seq);
    message.insert(QStringLiteral("event"), event);
    if (!body.isUndefined())
        message.insert(QStringLiteral("body"), body);
    return message;
}

QJsonObject DapCodec::buildResponse(const QJsonObject& incomingRequest,
                                    const QJsonValue& body,
                                    qint64 selfSeq) const
{
    QJsonObject message = baseResponse(incomingRequest, selfSeq);
    message.insert(QStringLiteral("success"), true);
    if (!body.isUndefined())
        message.insert(QStringLiteral("body"), body);
    return message;
}

QJsonObject DapCodec::buildErrorResponse(const QJsonObject& incomingRequest,
                                         int code, const QString& message,
                                         const QJsonValue& data,
                                         qint64 selfSeq) const
{
    QJsonObject out = baseResponse(incomingRequest, selfSeq);
    out.insert(QStringLiteral("success"), false);
    QJsonObject error;
    error.insert(QStringLiteral("id"), code);
    error.insert(QStringLiteral("format"), message);
    // The WHOLE error table is the DAP payload: an object `data` merges
    // its members in (showCrash, osVersion suggestions...); a non-object
    // data has no legal spelling on this wire and is dropped.
    if (data.isObject()) {
        const QJsonObject table = data.toObject();
        for (auto it = table.begin(); it != table.end(); ++it)
            error.insert(it.key(), it.value());
    }
    out.insert(QStringLiteral("error"), error);
    return out;
}

QJsonObject DapCodec::buildUnregisteredAnswer(const QJsonObject& request,
                                              qint64 selfSeq) const
{
    return buildErrorResponse(
        request, 0,
        QStringLiteral("Unknown request: %1")
            .arg(request.value(QStringLiteral("command")).toString()),
        QJsonValue(QJsonValue::Undefined), selfSeq);
}

QJsonObject DapCodec::buildInvalidIdAnswer(const QJsonObject& /*request*/) const
{
    return {}; // no numeric seq to answer INTO; classify already routed such
               // frames to NotEnvelope (malformedMessage), so this site is
               // unreachable-by-design and the skip rule would no-op it
}

QJsonObject DapCodec::buildCancel(qint64 targetId, qint64 selfSeq) const
{
    QJsonObject arguments;
    arguments.insert(QStringLiteral("requestId"), targetId);
    return buildRequest(selfSeq, QStringLiteral("cancel"), arguments);
}

qint64 DapCodec::cancelTargetId(const QJsonObject& message) const
{
    return message.value(QStringLiteral("arguments")).toObject()
        .value(QStringLiteral("requestId")).toVariant().toLongLong();
        // absent/foreign-typed -> 0: "nothing to cancel", the honest
        // refusal route in DapClient
}

QJsonValue DapCodec::shutdownParams() const
{
    QJsonObject arguments;
    arguments.insert(QStringLiteral("restart"), false); // v1 never restarts:
                                                        // restartServer() is
                                                        // the manual door
    return arguments;
}

DapCodec::Incoming DapCodec::classify(const QJsonObject& message) const
{
    Incoming in;
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("event")) {
        // The machine-D1 normalization, kept: an event dispatches EXACTLY
        // like a JSON-RPC notification (name = `event`, payload = `body`,
        // unknown events ignored unless a catch-all claims them).
        in.kind = Incoming::Kind::Notification;
        in.method = message.value(QStringLiteral("event")).toString();
        in.params = message.value(QStringLiteral("body"));
        return in;
    }
    if (type == QStringLiteral("request")) {
        const QJsonValue seq = message.value(QStringLiteral("seq"));
        if (!seq.isDouble()) {
            in.kind = Incoming::Kind::NotEnvelope;
            in.why = QStringLiteral("DAP request has no numeric seq");
            return in; // unanswerable: no key to respond into
        }
        in.kind = Incoming::Kind::Request;
        in.id = seq; // the adapter's seq IS the id the response keys on
        in.method = message.value(QStringLiteral("command")).toString();
        in.params = message.value(QStringLiteral("arguments"));
        return in;
    }
    if (type == QStringLiteral("response")) {
        in.kind = Incoming::Kind::Response; // pairing (request_seq) is the
                                            // response-side codecs below
        return in;
    }
    in.kind = Incoming::Kind::NotEnvelope;
    in.why = QStringLiteral("not a DAP envelope: type member missing or unknown");
    return in;
}

bool DapCodec::responseShapeOk(const QJsonObject& message, QString* why) const
{
    if (!message.value(QStringLiteral("success")).isBool()) {
        *why = QStringLiteral("response without success boolean");
        return false;
    }
    return true;
}

bool DapCodec::responseMatchId(const QJsonObject& message, qint64* id,
                               QString* why) const
{
    const QJsonValue seq = message.value(QStringLiteral("request_seq"));
    if (!seq.isDouble()) {
        *why = QStringLiteral("response has no numeric request_seq");
        return false;
    }
    *id = static_cast<qint64>(seq.toDouble());
    return true;
}

bool DapCodec::decodeReply(const QJsonObject& message, EnvelopeReply* out,
                           QString* /*why*/) const
{
    if (!message.value(QStringLiteral("success")).toBool()) {
        out->isError = true;
        const QJsonValue errorValue = message.value(QStringLiteral("error"));
        const QJsonObject error
            = errorValue.isObject() ? errorValue.toObject() : QJsonObject{};
        out->code = error.value(QStringLiteral("id")).toInt();
        // absent id -> 0, the spec's own default (NEVER a local kErr*
        // mask: a positive-table lie is the iide anti-pattern inverted)
        out->message = error.value(QStringLiteral("format"))
                           .toString(QStringLiteral("(no message)"));
        out->data = errorValue; // the whole table crosses; a
                                // non-object/absent error stays as-is
        return true;
    }
    out->isError = false;
    out->result = message.value(QStringLiteral("body")); // absent = Undefined
    return true;
}

} // namespace qmn::dap
