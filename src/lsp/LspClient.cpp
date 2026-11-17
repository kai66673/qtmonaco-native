#include "lsp/LspClient.hpp"

#include "protocol/RpcClient.hpp"

#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QTimer>

namespace qmn::lsp {

using qmn::protocol::RpcClient;
using qmn::protocol::Transport;

// One Transport+RpcClient pair. Created together, destroyed together: M1's
// RpcClient caches the Transport pointer at construction, so mixing an old
// RpcClient with a new Transport is a dangling-pointer hazard, and Process-
// Transport is single-session by design.
struct LspClient::Session {
    Transport* transport = nullptr;
    RpcClient* rpc = nullptr;
};

// The ErrorKind's own name for the FAILED reason string (the enum is
// Q_ENUM-declared; spelling it here keeps the reason greppable).
static QString errorKindName(Transport::ErrorKind kind)
{
    switch (kind) {
    case Transport::ErrorKind::StartFailed:
        return QStringLiteral("StartFailed");
    case Transport::ErrorKind::ConnectFailed:
        return QStringLiteral("ConnectFailed");
    case Transport::ErrorKind::WriteFailed:
        return QStringLiteral("WriteFailed");
    case Transport::ErrorKind::ServerDied:
        return QStringLiteral("ServerDied");
    case Transport::ErrorKind::Io:
        return QStringLiteral("Io");
    }
    return QStringLiteral("Unknown");
}

LspClient::LspClient(TransportFactory factory, QObject* parent)
    : QObject(parent)
    , factory_(std::move(factory))
{
}

LspClient::~LspClient()
{
    endSession();
}

void LspClient::start(const QJsonObject& initializeParams)
{
    initializeParams_ = initializeParams;
    if (state_ == State::Stopping)
        return; // a stop barrier in flight: not a "start again" moment
    autoRestartsLeft_ = 1; // a deliberate start re-arms the budget
    beginSession();
}

void LspClient::stop()
{
    if (!session_) {
        state_ = State::Stopped;
        return;
    }
    state_ = State::Stopping; // onGone() maps the eviction to Stopped
    if (session_->rpc->isGone()) {
        // The session is already gone (the crash ladder's markGone fired
        // before anyone asked to stop): RpcClient::stop() would be an
        // idempotent no-op and the Stopping state would never flip. Honest
        // teardown here: make sure the pipe is really closed (a gone-but-
        // still-running process needs the kill escalation) and land the
        // terminal state ourselves.
        if (session_->transport->isRunning())
            session_->transport->stop();
        endSession();
        state_ = State::Stopped;
        return;
    }
    session_->rpc->stop(); // the two-phase barrier (RpcClient's dance)
}

void LspClient::restartServer()
{
    if (state_ == State::Ready || state_ == State::Starting || restartPending_)
        return;
    if (state_ == State::Stopping)
        return;
    if (session_)
        session_->transport->stop(); // defensive: a half-dead session's pipe
    endSession();
    autoRestartsLeft_ = 1; // the manual door RE-ARMS the budget
    beginSession();        // a live session re-runs the handshake too
}

void LspClient::beginSession()
{
    endSession();
    state_ = State::Starting;
    sessionOpened_ = false;

    session_ = new Session;
    session_->transport = factory_(this);
    session_->rpc = new RpcClient(session_->transport, this);
    registerPolicyHandlers(session_->rpc);

    connect(session_->transport, &Transport::opened, this,
            &LspClient::onOpened);
    connect(session_->transport, &Transport::stderrLine, this,
            &LspClient::serverStderr); // the separate STDERR channel
    // Keep the last issue for the FAILED reason string. Connected BEFORE
    // transport->start(): the StartFailed error fires synchronously inside
    // start() and must already be captured.
    connect(session_->transport, &Transport::transportError, this,
            [this](Transport::ErrorKind kind, const QString& text) {
                lastTransportIssue_ =
                    QStringLiteral("%1: %2")
                        .arg(errorKindName(kind), text);
            });
    connect(session_->rpc, &RpcClient::transportGone, this,
            &LspClient::onGone);

    if (!session_->transport->start()) {
        // Synchronous "cannot even begin". The restart semantics cover a
        // DEAD SERVER, not a missing program: no retry loop on a typo'd path
        // (the never-opened rule in onGone() already burned no budget).
        enterFailed(QStringLiteral("transport refused to start: %1")
                        .arg(lastTransportIssue_));
        return;
    }
    // started/opened -> initialize; if the pipe dies BEFORE the handshake,
    // onGone() reports with the pinning reason (FailedToStart etc.).
}

void LspClient::endSession()
{
    if (!session_)
        return;
    // deleteLater, not delete: we may be running INSIDE a callback of this
    // very session (onGone / a reply handler), and self-deletion mid-callback
    // is UB. The pointers are nulled immediately: no dangling, the objects
    // die when the stack unwinds.
    session_->rpc->disconnect(this);
    session_->transport->disconnect(this);
    session_->rpc->deleteLater();
    // Ownership door for the transport: the ONLY safe moment to delete a
    // ProcessTransport (its ~QProcess would otherwise SIGKILL a live child
    // and warn "Destroyed while process is still running") is after the
    // child was really reaped — i.e. after closed() fired. isRunning() is
    // NOT that oracle: finishStop()->markGone runs while the EOF/exit pair
    // is still in flight, so a stop-initiated teardown finds running_ still
    // true with the child milliseconds from an honest exit. closed() is
    // guaranteed (the transport's own terminate/kill escalation arms it),
    // so this connect can never strand the object.
    if (session_->transport->isRunning()) {
        connect(session_->transport, &Transport::closed, session_->transport,
                &QObject::deleteLater);
    } else {
        // closed() already fired (or the transport was never running): the
        // child is reaped, QProcess deletion is safe now.
        session_->transport->deleteLater();
    }
    delete session_;
    session_ = nullptr;
}

void LspClient::onOpened()
{
    if (state_ != State::Starting || !session_)
        return;
    sessionOpened_ = true; // the auto-restart hook arms only once the pipe
                           // was really live at least once
    // The pinned 10 s initialize door. A timeout answers exactly once with
    // kErrTimeout — and the handshake failure does NOT auto-restart (the
    // spawn-twice-a-deaf-server rule, decided in M2).
    session_->rpc->request(QStringLiteral("initialize"), initializeParams_,
                           [this](const RpcClient::Reply& reply) {
                               onReadyOrFailed(reply,
                                               QStringLiteral("initialize"));
                           },
                           RpcClient::kInitializeTimeoutMs);
}

void LspClient::onReadyOrFailed(const RpcClient::Reply& reply,
                                const QString& reasonIfError)
{
    if (state_ != State::Starting || !session_)
        return; // stale callback across a session swap: drop (never crash)
    if (reply.isError) {
        enterFailed(QStringLiteral("%1 failed: %2")
                        .arg(reasonIfError, reply.message));
        return;
    }
    const QJsonObject result = reply.result.toObject();
    serverCapabilities_ = result.value(QStringLiteral("capabilities"))
                              .toObject();
    positionEncoding_ = result.value(QStringLiteral("positionEncoding"))
                            .toString(); // absent -> "" = utf-16 default
    session_->rpc->notify(QStringLiteral("initialized"),
                          QJsonObject{}); // empty params object
    state_ = State::Ready;
    emit ready(); // EVERY (re-)READY: machine 3's self-heal rides this
}

// The gone paths. An eviction the client did not ask for (the M1 latch is
// what makes "asked-for" decidable) is the crash/restart hook; clean stops
// land in Stopped.
void LspClient::onGone()
{
    switch (state_) {
    case State::Stopping:
        state_ = State::Stopped;
        endSession();
        return;
    case State::NotStarted:
    case State::Stopped:
    case State::Failed:
        return; // terminal/stale: a dead session's last gasp is not an event
    case State::Starting:
    case State::Ready:
        break;
    }
    if (!sessionOpened_) {
        // The pipe died before it ever opened: that is a server that could
        // not be spawned, not a server that died mid-session. No restart
        // budget is burned (the typo'd-path rule: no retry loop on a
        // program the OS refuses to start).
        enterFailed(QStringLiteral("transport never opened: %1")
                        .arg(lastTransportIssue_));
        return;
    }
    if (autoRestartsLeft_ > 0) {
        --autoRestartsLeft_;
        restartPending_ = true;
        state_ = State::Starting; // this IS another startup: the feature
                                  // doors keep answering kErrNotRunning
                                  // meanwhile (honest gate), and the
                                  // backoff lambda's guard must pass
        endSession();
        QTimer::singleShot(kRestartBackoffMs, this, [this] {
            restartPending_ = false;
            if (state_ == State::Starting)
                beginSession(); // factory re-spawn, handshake replayed
        });
        return;
    }
    enterFailed(QStringLiteral("transport closed (auto-restart spent): %1")
                    .arg(lastTransportIssue_));
}

void LspClient::enterFailed(const QString& reason)
{
    if (state_ == State::Stopping)
        return; // the stop barrier outranks any failure
    if (state_ == State::Failed)
        return; // failed() fires exactly once per episode: the synchronous
                // StartFailed chain can reach here twice (the gone latch
                // inside transport->start(), then beginSession's verdict)
    state_ = State::Failed;
    if (session_) {
        // A live pipe behind a failed handshake must still be shut down
        // honestly: the two-phase barrier + the transport's kill escalation.
        // If the transport already died (gone path), RpcClient::stop() is a
        // latch-checked no-op.
        session_->rpc->stop();
    }
    emit failed(reason);
}

void LspClient::registerPolicyHandlers(RpcClient* rpc)
{
    // ---- notifications (spec-silent; relayed to glue as signals) ----
    // Connect-free handlers via std::function: the captured rpc/this belong
    // to THIS session; after a session swap the old rpc never fires again.
    rpc->setNotificationHandler(QStringLiteral("window/showMessage"),
                                [this](const QJsonValue& params) {
                                    const QJsonObject o = params.toObject();
                                    emit serverShowMessage(
                                        o.value(QStringLiteral("type")).toInt(),
                                        o.value(QStringLiteral("message"))
                                            .toString());
                                });
    auto logRelay = [this](const QJsonObject& o) {
        emit serverLogMessage(o.value(QStringLiteral("type")).toInt(),
                              o.value(QStringLiteral("message")).toString());
    };
    rpc->setNotificationHandler(QStringLiteral("window/logMessage"),
                                [logRelay](const QJsonValue& params) {
                                    logRelay(params.toObject());
                                });
    // $/logTrace rides the SAME channel (PLAN pairs them); its params carry
    // no type, so it logs at MessageType Log(4).
    rpc->setNotificationHandler(QStringLiteral("$/logTrace"),
                                [this](const QJsonValue& params) {
                                    emit serverLogMessage(
                                        4, params.toObject()
                                               .value(QStringLiteral("message"))
                                               .toString());
                                });
    rpc->setNotificationHandler(QStringLiteral("$/progress"),
                                [this](const QJsonValue& params) {
                                    const QJsonObject o = params.toObject();
                                    emit serverProgress(
                                        o.value(QStringLiteral("token")),
                                        o.value(QStringLiteral("value")));
                                });

    // ---- requests: always answer ----
    rpc->setRequestHandler(QStringLiteral("window/showMessageRequest"),
                           [rpc, this](const QJsonObject& request) {
                               // v1 policy: answered null = no action picked
                               // (spec-legal); the button UI lands with the
                               // popups (machines 5/6) via respondApplyEdit-
                               // style glue.
                               rpc->respond(request.value(QStringLiteral("id")),
                                            QJsonValue(QJsonValue::Null));
                               emit serverShowMessageRequest(
                                   request.value(QStringLiteral("id")),
                                   request.value(QStringLiteral("params")));
                           });
    // The M4 config layer answers these for real; the honest v1 answer is a
    // per-item null array / null folders (spec: null = no settings).
    rpc->setRequestHandler(QStringLiteral("workspace/configuration"),
                           [rpc](const QJsonObject& request) {
                               const QJsonArray items =
                                   request.value(QStringLiteral("params"))
                                       .toObject()
                                       .value(QStringLiteral("items"))
                                       .toArray();
                               QJsonArray answers;
                               for (int i = 0; i < items.size(); ++i)
                                   answers.append(
                                       QJsonValue(QJsonValue::Null));
                               rpc->respond(request.value(QStringLiteral("id")),
                                            answers);
                           });
    rpc->setRequestHandler(QStringLiteral("workspace/workspaceFolders"),
                           [rpc](const QJsonObject& request) {
                               rpc->respond(request.value(QStringLiteral("id")),
                                            QJsonValue(QJsonValue::Null));
                           });
    rpc->setRequestHandler(QStringLiteral("workDoneProgress/create"),
                           [rpc, this](const QJsonObject& request) {
                               rpc->respond(request.value(QStringLiteral("id")),
                                            QJsonValue(QJsonValue::Null));
                               emit createWorkDoneProgress(
                                   request.value(QStringLiteral("params"))
                                       .toObject()
                                       .value(QStringLiteral("token")));
                           });
    // applyEdit: the POLICY is the applier's (machine 5) — LspClient exposes
    // the request and keeps the openIncoming_ id alive for respondApplyEdit.
    rpc->setRequestHandler(QStringLiteral("workspace/applyEdit"),
                           [this](const QJsonObject& request) {
                               emit serverApplyEdit(
                                   request.value(QStringLiteral("id")),
                                   request.value(QStringLiteral("params"))
                                       .toObject()
                                       .value(QStringLiteral("edit")));
                           });

    // ---- the capability registry (2026-11-15 decision, the real thing) ----
    rpc->setRequestHandler(QStringLiteral("client/registerCapability"),
                           [rpc, this](const QJsonObject& request) {
                               handleRegisterCapability(rpc, request);
                           });
    rpc->setRequestHandler(QStringLiteral("client/unregisterCapability"),
                           [rpc, this](const QJsonObject& request) {
                               handleUnregisterCapability(rpc, request);
                           });
}

// The v1 registrable set = PLAN's registration sources + semanticTokens
// (its wire+decoder half ships in M8, so its dynamic registration is
// ACCEPTED-and-kept-as-data — painting stays tree-sitter's).
static bool isRegistrableMethod(const QString& method)
{
    static const QSet<QString> kRegistrable{
        QStringLiteral("textDocument/completion"),
        QStringLiteral("textDocument/hover"),
        QStringLiteral("textDocument/definition"),
        QStringLiteral("textDocument/references"),
        QStringLiteral("textDocument/codeAction"),
        QStringLiteral("textDocument/formatting"),
        QStringLiteral("textDocument/rename"),
        QStringLiteral("textDocument/didChange"), // the sync-change source
        QStringLiteral("textDocument/semanticTokens/full"),
    };
    return kRegistrable.contains(method);
}

void LspClient::handleRegisterCapability(RpcClient* rpc,
                                         const QJsonObject& request)
{
    const QJsonValue id = request.value(QStringLiteral("id"));
    const QJsonArray registrations =
        request.value(QStringLiteral("params"))
            .toObject()
            .value(QStringLiteral("registrations"))
            .toArray();

    // VALIDATE FIRST, register NONE on any unknown method (all-or-nothing,
    // decided in M2: a half-applied dynamic registration is a state lie).
    for (const QJsonValue& entry : registrations) {
        const QString method =
            entry.toObject().value(QStringLiteral("method")).toString();
        if (!isRegistrableMethod(method)) {
            rpc->respondError(
                id, kWireRequiredClientCapabilityUnsupported,
                QStringLiteral(
                    "dynamic registration not supported for \"%1\"")
                    .arg(method));
            return;
        }
    }
    for (const QJsonValue& entry : registrations)
        ++registrations_[entry.toObject()
                             .value(QStringLiteral("method"))
                             .toString()];
    rpc->respond(id, QJsonValue(QJsonValue::Null)); // LSP: result is null
    emit registrationsChanged();
}

void LspClient::handleUnregisterCapability(RpcClient* rpc,
                                           const QJsonObject& request)
{
    const QJsonValue id = request.value(QStringLiteral("id"));
    const QJsonArray registrations =
        request.value(QStringLiteral("params"))
            .toObject()
            .value(QStringLiteral("registrations"))
            .toArray();
    // Count-per-method bookkeeping; an unknown method idempotently answers
    // null (removing what is not there is not a lie, it is a no-op).
    for (const QJsonValue& entry : registrations) {
        const QString method =
            entry.toObject().value(QStringLiteral("method")).toString();
        auto it = registrations_.find(method);
        if (it != registrations_.end() && it.value() > 0 && --it.value() == 0)
            registrations_.erase(it);
    }
    rpc->respond(id, QJsonValue(QJsonValue::Null));
    emit registrationsChanged();
}

bool LspClient::isRegistered(const QString& method) const
{
    return registrations_.value(method, 0) > 0;
}

qint64 LspClient::request(const QString& method, const QJsonValue& params,
                          RpcClient::ReplyHandler handler, int timeoutMs)
{
    if (state_ != State::Ready || !session_) {
        // The honest rejection: exactly one local kErrNotRunning reply,
        // never a silently unfulfilled promise (M1's gate precedent).
        if (handler) {
            QTimer::singleShot(0, this, [handler = std::move(handler)] {
                handler({true, {}, RpcClient::kErrNotRunning,
                         QStringLiteral("LSP client is not Ready"), {}});
            });
        }
        return -1;
    }
    return session_->rpc->request(method, params, std::move(handler),
                                  timeoutMs);
}

void LspClient::notify(const QString& method, const QJsonValue& params)
{
    if (state_ != State::Ready || !session_)
        return; // documented drop: LSP forbids pre-Ready notifications;
                // the syncer (M3) queues didOpen/didChange itself
    session_->rpc->notify(method, params);
}

void LspClient::respondApplyEdit(const QJsonValue& id, bool applied)
{
    if (!session_)
        return;
    session_->rpc->respond(
        id, QJsonObject{{QStringLiteral("applied"), applied}});
}

} // namespace qmn::lsp