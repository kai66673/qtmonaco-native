#include "protocol/JsonRpcCodec.hpp"

namespace qmn::protocol {

namespace {

// Same canonical-id spelling as RpcClient's (the guard for double answers
// and the null-id JSON-RPC rule both key on it).
bool idIsCarriable(const QJsonValue& id)
{
    return id.isDouble() || id.isString();
}

QJsonObject baseMessage()
{
    QJsonObject message;
    message.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    return message;
}

QJsonObject errorObject(int code, const QString& message, const QJsonValue& data)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);
    if (!data.isUndefined())
        error.insert(QStringLiteral("data"), data);
    return error;
}

} // namespace

QJsonObject JsonRpcCodec::buildRequest(qint64 idOrSeq, const QString& method,
                                       const QJsonValue& params) const
{
    QJsonObject message = buildNotification(method, params, 0);
    message.insert(QStringLiteral("id"), idOrSeq);
    return message;
}

QJsonObject JsonRpcCodec::buildNotification(const QString& method,
                                            const QJsonValue& params,
                                            qint64 /*seq*/) const
{
    QJsonObject message = baseMessage();
    message.insert(QStringLiteral("method"), method);
    // Only UNDEFINED params are omitted; a null QJsonValue (what the
    // QJsonValue() default argument builds on Qt 6) is sent as explicit
    // "params": null, which LSP's "params may be null" doors expect.
    if (!params.isUndefined())
        message.insert(QStringLiteral("params"), params);
    return message;
}

QJsonObject JsonRpcCodec::buildResponse(const QJsonObject& incomingRequest,
                                        const QJsonValue& result,
                                        qint64 /*selfSeq*/) const
{
    // JSON-RPC responses carry no id of their own — the request's id is
    // echoed verbatim (responsesCarrySeq() is false, so selfSeq is never
    // allocated).
    QJsonObject message = baseMessage();
    message.insert(QStringLiteral("id"), incomingRequest.value(QStringLiteral("id")));
    message.insert(QStringLiteral("result"), result);
    return message;
}

QJsonObject JsonRpcCodec::buildErrorResponse(const QJsonObject& incomingRequest,
                                             int code, const QString& message,
                                             const QJsonValue& data,
                                             qint64 /*selfSeq*/) const
{
    const QJsonValue id = incomingRequest.value(QStringLiteral("id"));
    QJsonObject response = baseMessage();
    if (!idIsCarriable(id))
        response.insert(QStringLiteral("id"), QJsonValue::Null); // JSON-RPC rule
    else
        response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("error"), errorObject(code, message, data));
    return response;
}

QJsonObject JsonRpcCodec::buildUnregisteredAnswer(const QJsonObject& request,
                                                  qint64 /*selfSeq*/) const
{
    // "Always answer, never lie": mechanically -32601. The policy half —
    // WHICH methods answer what — is the client layer's registration job.
    return buildErrorResponse(
        request, kMethodNotFound,
        QStringLiteral("method not found: %1")
            .arg(request.value(QStringLiteral("method")).toString()),
        QJsonValue(QJsonValue::Undefined), 0);
}

QJsonObject JsonRpcCodec::buildInvalidIdAnswer(const QJsonObject& request) const
{
    // The inbound id cannot be echoed (JSON-RPC: id must be a string or a
    // number) — the answer goes out with id:null.
    return buildErrorResponse(
        request, kInvalidRequest,
        QStringLiteral("invalid request id type"),
        QJsonValue(QJsonValue::Undefined), 0);
}

QJsonObject JsonRpcCodec::buildCancel(qint64 targetId, qint64 /*selfSeq*/) const
{
    QJsonObject cancelParams;
    cancelParams.insert(QStringLiteral("id"), targetId);
    return buildNotification(QStringLiteral("$/cancelRequest"), cancelParams, 0);
}

QString JsonRpcCodec::shutdownCommand() const
{
    return QStringLiteral("shutdown");
}

QJsonValue JsonRpcCodec::shutdownParams() const
{
    // (QJsonValue(nullptr) is AMBIGUOUS on Qt 6 — the enum ctor is the
    // honest spelling of explicit JSON null.)
    return QJsonValue(QJsonValue::Null);
}

QString JsonRpcCodec::exitCommand() const
{
    return QStringLiteral("exit");
}

EnvelopeCodec::Incoming JsonRpcCodec::classify(const QJsonObject& message) const
{
    // The "jsonrpc":"2.0" member is tolerated when absent (lenient
    // inbound; our own outbound always carries it).
    Incoming in;
    if (message.contains(QStringLiteral("method"))) {
        in.method = message.value(QStringLiteral("method")).toString();
        in.params = message.value(QStringLiteral("params"));
        const QJsonValue id = message.value(QStringLiteral("id"));
        if (id.isUndefined() || id.isNull()) {
            in.kind = Incoming::Kind::Notification; // unknown methods are ignored, per spec
            return in;
        }
        in.id = id;
        in.kind = Incoming::Kind::Request;
        return in;
    }
    if (message.contains(QStringLiteral("id"))) {
        in.kind = Incoming::Kind::Response;
        return in;
    }
    in.kind = Incoming::Kind::NotEnvelope;
    in.why = QStringLiteral("no method and no id in message");
    return in;
}

bool JsonRpcCodec::responseShapeOk(const QJsonObject& message, QString* why) const
{
    if (!message.contains(QStringLiteral("result"))
        && !message.contains(QStringLiteral("error"))) {
        *why = QStringLiteral("response with neither result nor error");
        return false;
    }
    return true;
}

bool JsonRpcCodec::responseMatchId(const QJsonObject& message, qint64* id,
                                   QString* why) const
{
    const QJsonValue idValue = message.value(QStringLiteral("id"));
    // We only ever issue numeric ids; a server that echoes a string/other
    // type back has violated the id space of OUR requests.
    if (!idValue.isDouble()) {
        *why = QStringLiteral("response id is not the numeric id we sent");
        return false;
    }
    *id = static_cast<qint64>(idValue.toDouble());
    return true;
}

bool JsonRpcCodec::decodeReply(const QJsonObject& message, EnvelopeReply* out,
                               QString* /*why*/) const
{
    if (message.contains(QStringLiteral("error"))) {
        const QJsonObject error = message.value(QStringLiteral("error")).toObject();
        out->isError = true;
        out->code = error.value(QStringLiteral("code")).toInt();
        out->message = error.value(QStringLiteral("message"))
                           .toString(QStringLiteral("(no message)"));
        out->data = error.value(QStringLiteral("data"));
        return true;
    }
    out->isError = false;
    out->result = message.value(QStringLiteral("result"));
    return true;
}

} // namespace qmn::protocol
