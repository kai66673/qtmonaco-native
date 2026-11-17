#include "protocol/RpcClient.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace qmn::protocol {

RpcClient::RpcClient(Transport* transport, QObject* parent)
    : QObject(parent)
    , transport_(transport)
{
    connect(transport_, &Transport::bytesReceived, this, &RpcClient::onBytes);
    connect(transport_, &Transport::opened, this, &RpcClient::onOpened);
    connect(transport_, &Transport::closed, this, &RpcClient::onClosed);
    connect(transport_, &Transport::transportError, this,
            &RpcClient::onTransportError);
}

void RpcClient::setRequestHandler(const QString& method, RequestHandler handler)
{
    requestHandlers_.insert(method, std::move(handler));
}

void RpcClient::setNotificationHandler(const QString& method,
                                       NotificationHandler handler)
{
    notificationHandlers_.insert(method, std::move(handler));
}

QJsonObject RpcClient::envelope(const QString& method, const QJsonValue& params)
{
    QJsonObject message;
    message.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    message.insert(QStringLiteral("method"), method);
    // Only UNDEFINED params are omitted; a null QJsonValue (what the
    // QJsonValue() default argument builds on Qt 6) is sent as explicit
    // "params": null, which LSP's "params may be null" doors expect.
    if (!params.isUndefined())
        message.insert(QStringLiteral("params"), params);
    return message;
}

QByteArray RpcClient::encode(const QJsonObject& message) const
{
    return JsonFraming::encode(
        QJsonDocument(message).toJson(QJsonDocument::Compact));
}

QString RpcClient::canonicalId(const QJsonValue& id)
{
    if (id.isDouble())
        return QStringLiteral("n:%1").arg(id.toInt()); // LSP ids are small ints
    if (id.isString())
        return QStringLiteral("s:%1").arg(id.toString());
    return QStringLiteral("bad");
}

qint64 RpcClient::request(const QString& method, const QJsonValue& params,
                          ReplyHandler handler, int timeoutMs)
{
    if (stopPhase_ != StopPhase::Idle) {
        // The stop barrier has begun: no new business (the shutdown request
        // of the barrier itself goes through sendRequestRaw instead).
        if (handler) {
            QTimer::singleShot(0, this, [handler = std::move(handler)] {
                handler({true, {}, kErrTransportClosed,
                         QStringLiteral("client is stopping/stopped"), {}});
            });
        }
        return -1;
    }
    return sendRequestRaw(method, params, std::move(handler), timeoutMs);
}

qint64 RpcClient::sendRequestRaw(const QString& method, const QJsonValue& params,
                                 ReplyHandler handler, int timeoutMs)
{
    // Honest rejection BEFORE any side effect: writing to a dead pipe would
    // surface (WriteFailed) but could land between the write and the
    // pending-insert and leave the request unfulfilled forever. -1 = never
    // sent; the handler still fires exactly once, asynchronously.
    if (goneFired_ || !transport_->isRunning()) {
        if (handler) {
            QTimer::singleShot(0, this, [handler = std::move(handler)] {
                handler({true, {}, kErrTransportClosed,
                         QStringLiteral("transport is not running"), {}});
            });
        }
        return -1;
    }

    const qint64 id = nextId_++;

    QJsonObject message = envelope(method, params);
    message.insert(QStringLiteral("id"), id);
    // The device-level write queue absorbs bytes until the pipe is live;
    // its failures are NOT swallowed: they surface as transportError(),
    // which marks every pending (this one included) via markGone().
    transport_->write(encode(message));

    if (handler || stopPhase_ == StopPhase::Stopping) {
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        Pending pending{std::move(handler), timer, method};
        pending_.insert(id, std::move(pending));
        connect(timer, &QTimer::timeout, this, [this, id, timeoutMs] {
            if (!pending_.contains(id))
                return;
            // PLAN: timeout also CANCELS server-side work before eviction.
            QJsonObject cancelParams;
            cancelParams.insert(QStringLiteral("id"), id);
            notify(QStringLiteral("$/cancelRequest"), cancelParams);
            fulfill(id, {true, {}, kErrTimeout,
                         QStringLiteral("request \"%1\" timed out after %2 ms")
                             .arg(pending_.value(id).method)
                             .arg(timeoutMs),
                         {}});
        });
        timer->start(timeoutMs);
    }
    return id;
}

void RpcClient::notify(const QString& method, const QJsonValue& params)
{
    transport_->write(encode(envelope(method, params)));
}

void RpcClient::cancel(qint64 requestId)
{
    if (!pending_.contains(requestId))
        return; // already fulfilled/evicted: nothing to cancel
    QJsonObject cancelParams;
    cancelParams.insert(QStringLiteral("id"), requestId);
    notify(QStringLiteral("$/cancelRequest"), cancelParams);
    // Local fulfillment with the REAL LSP cancellation code (the iide
    // anti-pattern was faking a withdrawn -32801-style code; -32802 is the
    // spec's own). A late server response to this id then finds no pending
    // entry and is ignored silently, per JSON-RPC.
    fulfill(requestId, {true, {}, kWireRequestCancelled,
                        QStringLiteral("request cancelled by client"), {}});
}

bool RpcClient::respond(const QJsonValue& id, const QJsonValue& result)
{
    const QString key = canonicalId(id);
    if (!openIncoming_.remove(key))
        return false;
    QJsonObject message;
    message.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    message.insert(QStringLiteral("id"), id);
    message.insert(QStringLiteral("result"), result);
    transport_->write(encode(message));
    return true;
}

bool RpcClient::respondError(const QJsonValue& id, int code,
                             const QString& message, const QJsonValue& data)
{
    const QString key = canonicalId(id);
    if (!openIncoming_.remove(key))
        return false;
    writeErrorResponse(id, code, message, data);
    return true;
}

void RpcClient::writeErrorResponse(const QJsonValue& id, int code,
                                   const QString& message,
                                   const QJsonValue& data)
{
    QJsonObject errorObject;
    errorObject.insert(QStringLiteral("code"), code);
    errorObject.insert(QStringLiteral("message"), message);
    if (!data.isUndefined())
        errorObject.insert(QStringLiteral("data"), data);
    QJsonObject response;
    response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    if (canonicalId(id) == QLatin1String("bad"))
        response.insert(QStringLiteral("id"), QJsonValue::Null); // JSON-RPC rule
    else
        response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("error"), errorObject);
    transport_->write(encode(response));
}

void RpcClient::stop()
{
    if (stopPhase_ != StopPhase::Idle)
        return; // idempotent
    stopPhase_ = StopPhase::Stopping;

    if (!goneFired_ && transport_->isRunning()) {
        // Phase 1: the LSP shutdown request (params null). Its own bounded
        // wait doubles as the fallback if the peer vanishes mid-shutdown.
        // (QJsonValue(nullptr) is AMBIGUOUS on Qt 6 — the enum ctor is the
        // honest spelling of explicit JSON null.)
        sendRequestRaw(QStringLiteral("shutdown"), QJsonValue(QJsonValue::Null),
                       [this](const Reply&) { finishStop(); }, kShutdownWaitMs);
        QTimer::singleShot(kShutdownWaitMs + 500, this,
                           [this] { finishStop(); });
    } else {
        finishStop();
    }
}

void RpcClient::finishStop()
{
    if (stopPhase_ == StopPhase::Stopped)
        return; // exactly-once barrier
    stopPhase_ = StopPhase::Stopped;
    if (!goneFired_ && transport_->isRunning())
        notify(QStringLiteral("exit")); // honest best effort; may surface as WriteFailed
    transport_->stop();
    markGone(kErrTransportClosed, QStringLiteral("client stopped"));
}

void RpcClient::onOpened()
{
    goneFired_ = false; // a new transport session re-arms the latch
}

void RpcClient::onBytes(const QByteArray& bytes)
{
    framing_.feed(bytes);
    while (!framing_.hasError()) {
        QByteArray payload;
        if (!framing_.poll(&payload))
            break;
        handlePayload(payload);
    }
    if (framing_.hasError()) {
        const JsonFraming::Error error = framing_.error();
        emit protocolError(error);
        transport_->stop(); // poisons the stream; the closed() path evicts
    }
}

void RpcClient::onClosed()
{
    markGone(kErrTransportClosed, QStringLiteral("transport closed"));
}

void RpcClient::onTransportError(Transport::ErrorKind kind,
                                 const QString& text)
{
    // PLAN: "a write queue whose errors SURFACE". A write failure with the
    // pipe otherwise alive still fails the pending requests — the bytes
    // they carry are exactly the ones the pipe refused.
    markGone(kind == Transport::ErrorKind::WriteFailed ? kErrWriteFailed
                                                       : kErrTransportClosed,
             text.isEmpty() ? QStringLiteral("transport error") : text);
}

void RpcClient::markGone(int code, const QString& message)
{
    if (goneFired_)
        return; // error-then-closed must fulfill pendings exactly once
    goneFired_ = true;
    stopPhase_ = StopPhase::Stopped;
    failAllPending(code, message);
    emit transportGone();
}

void RpcClient::failAllPending(int code, const QString& message)
{
    const QList<qint64> ids = pending_.keys();
    for (qint64 id : ids)
        fulfill(id, {true, {}, code, message, {}});
}

void RpcClient::fulfill(qint64 id, const Reply& reply)
{
    auto it = pending_.find(id);
    if (it == pending_.end())
        return;
    Pending pending = std::move(it.value()); // erase BEFORE the callback so a
    pending_.erase(it);                      // re-entrant call cannot see it twice
    if (pending.timer) {
        pending.timer->stop();
        pending.timer->deleteLater();
    }
    if (pending.handler)
        pending.handler(reply);
}

void RpcClient::handlePayload(const QByteArray& payload)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // Valid frame, broken JSON: surfaced (never swallowed), transport
        // stays open — framing is still synchronized.
        emit malformedMessage(
            parseError.error == QJsonParseError::NoError
                ? QStringLiteral("payload is not a JSON object")
                : parseError.errorString(),
            payload);
        return;
    }
    handleIncoming(doc.object());
}

void RpcClient::handleIncoming(const QJsonObject& message)
{
    // The "jsonrpc":"2.0" member is tolerated when absent (lenient inbound;
    // our own outbound always carries it).
    if (message.contains(QStringLiteral("method"))) {
        const QString method = message.value(QStringLiteral("method")).toString();
        const QJsonValue id = message.value(QStringLiteral("id"));
        if (id.isUndefined() || id.isNull()) {
            // Notification: unknown methods are ignored, per spec.
            auto it = notificationHandlers_.find(method);
            if (it != notificationHandlers_.end())
                it.value()(message.value(QStringLiteral("params")));
            return;
        }

        const QString key = canonicalId(id);
        if (key == QLatin1String("bad")) {
            // Mechanical answer, no openIncoming_ registration (see
            // writeErrorResponse): the guard is for HANDLER double-answers.
            writeErrorResponse(id, kWireInvalidRequest,
                               QStringLiteral("invalid request id type"),
                               QJsonValue(QJsonValue::Undefined));
            return;
        }
        auto it = requestHandlers_.find(method);
        if (it == requestHandlers_.end()) {
            // "Always answer, never lie": mechanically -32601. The policy
            // half — WHICH methods answer what — is LspClient's M2 job
            // (the real client/registerCapability registry lives there).
            writeErrorResponse(id, kWireMethodNotFound,
                               QStringLiteral("method not found: %1").arg(method),
                               QJsonValue(QJsonValue::Undefined));
            return;
        }
        openIncoming_.insert(key);
        // Handlers must answer; a handler that forgets leaves openIncoming_
        // populated — LspClient (M2) owns that duty, tests pin every path.
        it.value()(message);
        return;
    }

    if (message.contains(QStringLiteral("id"))) {
        handleResponse(message);
        return;
    }
    emit malformedMessage(QStringLiteral("no method and no id in message"),
                          QJsonDocument(message).toJson(QJsonDocument::Compact));
}

void RpcClient::handleResponse(const QJsonObject& message)
{
    const bool hasResult = message.contains(QStringLiteral("result"));
    const bool hasError = message.contains(QStringLiteral("error"));
    if (!hasResult && !hasError) {
        emit malformedMessage(
            QStringLiteral("response with neither result nor error"),
            QJsonDocument(message).toJson(QJsonDocument::Compact));
        return;
    }

    const QJsonValue idValue = message.value(QStringLiteral("id"));
    // We only ever issue numeric ids; a server that echoes a string/other
    // type back has violated the id space of OUR requests.
    if (!idValue.isDouble()) {
        emit malformedMessage(
            QStringLiteral("response id is not the numeric id we sent"),
            QJsonDocument(message).toJson(QJsonDocument::Compact));
        return;
    }
    const qint64 id = static_cast<qint64>(idValue.toDouble());
    if (!pending_.contains(id))
        return; // late response to a cancelled/timed-out/evicted request

    if (hasError) {
        const QJsonObject errorObject =
            message.value(QStringLiteral("error")).toObject();
        fulfill(id, {true, {},
                     errorObject.value(QStringLiteral("code")).toInt(),
                     errorObject.value(QStringLiteral("message"))
                         .toString(QStringLiteral("(no message)")),
                     errorObject.value(QStringLiteral("data"))});
        return;
    }
    fulfill(id, {false, message.value(QStringLiteral("result")), 0, {}, {}});
}

} // namespace qmn::protocol
