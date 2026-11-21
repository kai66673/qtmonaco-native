#include "protocol/RpcClient.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include "protocol/JsonRpcCodec.hpp"

namespace qmn::protocol {

RpcClient::RpcClient(Transport* transport, QObject* parent)
    : RpcClient(transport, &kJsonRpcCodec, parent)
{
}

RpcClient::RpcClient(Transport* transport, const EnvelopeCodec* codec,
                     QObject* parent)
    : QObject(parent)
    , transport_(transport)
    , codec_(codec)
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

void RpcClient::setAnyNotificationHandler(AnyNotificationHandler handler)
{
    anyNotification_ = std::move(handler);
}

void RpcClient::setAnyRequestHandler(RequestHandler handler)
{
    anyRequest_ = std::move(handler);
}

void RpcClient::registerMethod(const QString& name, const QJsonValue& result)
{
    MethodAnswer answer;
    answer.result = result;
    methodAnswers_.insert(name, answer);
}

void RpcClient::registerMethodFailure(const QString& name, int code,
                                      const QString& message)
{
    MethodAnswer answer;
    answer.isFailure = true;
    answer.code = code;
    answer.message = message;
    methodAnswers_.insert(name, answer);
}

void RpcClient::unregisterMethod(const QString& name)
{
    methodAnswers_.remove(name);
}

void RpcClient::setOnCancelRequested(CancelHandler handler)
{
    cancelHandler_ = std::move(handler);
}

bool RpcClient::cancelPending(qint64 requestId, int code,
                              const QString& message)
{
    if (!pending_.contains(requestId))
        return false; // already fulfilled/evicted: the caller owes the
                      // honest refusal answer (DAP wants success:false)
    fulfill(requestId, {true, {}, code, message, {}});
    return true;
}

QByteArray RpcClient::encode(const QJsonObject& message) const
{
    return JsonFraming::encode(
        QJsonDocument(message).toJson(QJsonDocument::Compact));
}

QString RpcClient::canonicalId(const QJsonValue& id)
{
    if (id.isDouble())
        return QStringLiteral("n:%1").arg(id.toInt()); // ids on both wires are small ints
    if (id.isString())
        return QStringLiteral("s:%1").arg(id.toString());
    return QStringLiteral("bad");
}

qint64 RpcClient::request(const QString& method, const QJsonValue& params,
                          ReplyHandler handler, int timeoutMs)
{
    if (stopPhase_ != StopPhase::Idle) {
        // The stop barrier has begun: no new business (the stop barrier's own
        // phase-1 request goes through sendRequestRaw instead).
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

    QJsonObject message = codec_->buildRequest(id, method, params);
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
            sendCancel(id);
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
    // The outbound one-way: JSON-RPC's notification takes no seq (the
    // counter must not move — byte-identity); DAP's initialized EVENT does
    // (every DAP message is seqed). An EMPTY message object still means
    // "this envelope sends no one-way messages here" and writes NOTHING.
    const qint64 seq = codec_->responsesCarrySeq() ? nextId_++ : 0;
    const QJsonObject message = codec_->buildNotification(method, params, seq);
    if (message.isEmpty())
        return;
    transport_->write(encode(message));
}

void RpcClient::sendCancel(qint64 targetId)
{
    // Untracked on purpose: a cancel REQUEST codec (DAP) allocates its own
    // seq here, and that request's response finds no pending entry and
    // lands on the unmatched-response ignore path.
    const qint64 seq = codec_->cancelIsRequest() ? nextId_++ : 0;
    transport_->write(encode(codec_->buildCancel(targetId, seq)));
}

void RpcClient::cancel(qint64 requestId)
{
    if (!pending_.contains(requestId))
        return; // already fulfilled/evicted: nothing to cancel
    sendCancel(requestId);
    // Local fulfillment with the REAL LSP cancellation code (the iide
    // anti-pattern was faking a withdrawn -32801-style code; -32802 is the
    // spec's own). A late server response to this id then finds no pending
    // entry and is ignored silently, per JSON-RPC. (JSON-RPC-shaped local
    // code — see the cancel() header contract: DAP cancels via the request
    // door, machine D2.)
    fulfill(requestId, {true, {}, kWireRequestCancelled,
                        QStringLiteral("request cancelled by client"), {}});
}

bool RpcClient::respond(const QJsonValue& id, const QJsonValue& result)
{
    const QString key = canonicalId(id);
    auto it = openIncoming_.find(key);
    if (it == openIncoming_.end())
        return false; // unknown/already-answered: double answers are never sent
    const QJsonObject request = it.value();
    openIncoming_.erase(it);
    writeAnswer(codec_->buildResponse(request, result, outboundSeq()));
    return true;
}

bool RpcClient::respondError(const QJsonValue& id, int code,
                             const QString& message, const QJsonValue& data)
{
    const QString key = canonicalId(id);
    auto it = openIncoming_.find(key);
    if (it == openIncoming_.end())
        return false;
    const QJsonObject request = it.value();
    openIncoming_.erase(it);
    writeAnswer(codec_->buildErrorResponse(request, code, message, data,
                                           outboundSeq()));
    return true;
}

void RpcClient::writeAnswer(const QJsonObject& answer)
{
    if (answer.isEmpty())
        return; // the codec declines to answer THIS site (buildInvalidIdAnswer's
                // lenient DAP spelling); JSON-RPC always builds an object
    transport_->write(encode(answer));
}

void RpcClient::stop()
{
    if (stopPhase_ != StopPhase::Idle)
        return; // idempotent
    stopPhase_ = StopPhase::Stopping;

    if (!goneFired_ && transport_->isRunning()) {
        // Phase 1: the codec's stop request (LSP `shutdown` with params
        // null; DAP `disconnect` with restart:false). Its own bounded wait
        // doubles as the fallback if the peer vanishes mid-shutdown. (The
        // params value is the codec's to spell — QJsonValue(nullptr) would
        // be AMBIGUOUS on Qt 6, so JsonRpcCodec uses the enum ctor for the
        // honest explicit-JSON-null shape.)
        sendRequestRaw(codec_->shutdownCommand(), codec_->shutdownParams(),
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
    // The codec's final message: LSP's `exit` notification; DAP has no
    // exit message (its barrier ends at the disconnect reply — the kill
    // escalation is the Transport's and stays there).
    if (codec_->hasExitNotification() && !goneFired_ && transport_->isRunning())
        notify(codec_->exitCommand()); // honest best effort; may surface as WriteFailed
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
        // stays open — framing is still synchronized. (The envelope's own
        // "not an envelope" verdict is classify()'s, one layer down; JSON
        // damage is envelope-independent and lives here.)
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
    // The envelope routing (method-vs-id vs command-vs-type, and the DAP
    // event normalization) is the codec's classify(); the DISPATCH side of
    // each kind — handler tables, the openIncoming_ guard, the mechanical
    // answers, the unknown-notification ignore — is envelope-agnostic
    // mechanism. Lenient inbound (a missing "jsonrpc":"2.0" member) is
    // JsonRpcCodec::classify's call, unchanged from the pre-seam code.
    const EnvelopeCodec::Incoming in = codec_->classify(message);
    switch (in.kind) {
    case EnvelopeCodec::Incoming::Kind::Notification: {
        // The inbound-cancel hook outranks everything (JSON-RPC's
        // $/cancelRequest lands here; the DAP cancel REQUEST lands on the
        // Request case below with the same hook).
        if (in.method == codec_->cancelCommand() && cancelHandler_) {
            cancelHandler_(codec_->cancelTargetId(message), message);
            return;
        }
        // Unknown methods/events are ignored, per spec, on both wires —
        // unless a catch-all is installed (the DAP event relay; unset on
        // JSON-RPC sessions, so the ignore-rule tests pin exactly before).
        auto it = notificationHandlers_.find(in.method);
        if (it != notificationHandlers_.end()) {
            it.value()(in.params);
        } else if (anyNotification_) {
            anyNotification_(in.method, in.params);
        }
        return;
    }
    case EnvelopeCodec::Incoming::Kind::Request: {
        const QString key = canonicalId(in.id);
        if (key == QLatin1String("bad")) {
            // Mechanical answer, no openIncoming_ registration (the guard
            // is for HANDLER double-answers): the shape is codec-owned.
            writeAnswer(codec_->buildInvalidIdAnswer(message));
            return;
        }
        if (in.method == codec_->cancelCommand() && cancelHandler_) {
            // DAP's cancel is a REQUEST (a DAP message must be answered):
            // registered like a normal request so the hook can respond()/
            // respondError() its way out (success:true / the honest
            // "cannot cancel" refusal).
            openIncoming_.insert(key, message);
            cancelHandler_(codec_->cancelTargetId(message), message);
            return;
        }
        auto it = requestHandlers_.find(in.method);
        if (it == requestHandlers_.end()) {
            // Static answer shelf first (machine D2 relay answers), then
            // the open-ended catch-all (DAP reverse requests, handler
            // MUST answer), then the mechanical one ("always answer,
            // never lie": JSON-RPC's -32601 / DAP's success:false
            // "Unknown request"). The M1 tripwire rides exactly this last
            // branch — unset shelves keep it first-in-line for LSP.
            auto answer = methodAnswers_.find(in.method);
            if (answer != methodAnswers_.end()) {
                writeAnswer(answer->isFailure
                                ? codec_->buildErrorResponse(
                                      message, answer->code, answer->message,
                                      QJsonValue(QJsonValue::Undefined),
                                      outboundSeq())
                                : codec_->buildResponse(message, answer->result,
                                                        outboundSeq()));
                return;
            }
            if (anyRequest_) {
                openIncoming_.insert(key, message);
                anyRequest_(message);
                return;
            }
            writeAnswer(codec_->buildUnregisteredAnswer(message, outboundSeq()));
            return;
        }
        openIncoming_.insert(key, message);
        // Handlers must answer; a handler that forgets leaves openIncoming_
        // populated — LspClient (M2) owns that duty, tests pin every path.
        it.value()(message);
        return;
    }
    case EnvelopeCodec::Incoming::Kind::Response:
        handleResponse(message);
        return;
    case EnvelopeCodec::Incoming::Kind::NotEnvelope:
        emit malformedMessage(in.why,
                              QJsonDocument(message).toJson(QJsonDocument::Compact));
        return;
    }
}

void RpcClient::handleResponse(const QJsonObject& message)
{
    // Shape-then-key order is PINNED by the machine-1 tests (a frame whose
    // payload is broken reports THAT first, whatever its id looks like);
    // the two codec calls keep the order, the raw-payload surfacing and
    // the silent late-response rule around it.
    QString why;
    if (!codec_->responseShapeOk(message, &why)) {
        emit malformedMessage(why,
                              QJsonDocument(message).toJson(QJsonDocument::Compact));
        return;
    }
    qint64 id = 0;
    if (!codec_->responseMatchId(message, &id, &why)) {
        emit malformedMessage(why,
                              QJsonDocument(message).toJson(QJsonDocument::Compact));
        return;
    }
    if (!pending_.contains(id))
        return; // late response to a cancelled/timed-out/evicted request

    Reply reply;
    if (!codec_->decodeReply(message, &reply, &why)) {
        // A codec that cannot read its own matched response is a wire bug
        // on the peer's side: surface, keep the transport (same rule as the
        // other malformed paths).
        emit malformedMessage(why,
                              QJsonDocument(message).toJson(QJsonDocument::Compact));
        return;
    }
    fulfill(id, reply);
}

} // namespace qmn::protocol
