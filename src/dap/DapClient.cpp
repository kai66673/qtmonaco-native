#include "dap/DapClient.hpp"

#include "protocol/RpcClient.hpp"
#include "protocol/Transport.hpp"

#include <QLocale>
#include <QJsonValue>
#include <QTimer>

namespace qmn::dap {

using qmn::protocol::RpcClient;
using qmn::protocol::Transport;

// The ErrorKind's own name for the FAILED reason string (the same spelling
// audit LspClient keeps, so failure reasons stay greppable across layers).
static QString errorKindName(Transport::ErrorKind kind)
{
    switch (kind) {
    case Transport::ErrorKind::StartFailed:
        return QStringLiteral("StartFailed");
    case Transport::ErrorKind::ConnectFailed:
        return QStringLiteral("ConnectFailed");
    case Transport::ErrorKind::WriteFailed:
        return QStringLiteral("WriteFailed");
    case Transport::ErrorKind::Io:
        return QStringLiteral("Io");
    }
    return QStringLiteral("Unknown");
}

// One Transport+RpcClient pair, created and destroyed together (the M1
// hazard comment in LspClient applies verbatim: RpcClient caches the
// Transport pointer at construction).
struct DapClient::Session {
    Transport* transport = nullptr;
    RpcClient* rpc = nullptr;
};

DapClient::DapClient(TransportFactory factory, const QString& adapterId,
                     QObject* parent)
    : QObject(parent)
    , factory_(std::move(factory))
    , adapterId_(adapterId)
{
}

DapClient::~DapClient()
{
    endSession();
}

void DapClient::start(const QJsonObject& initializeArguments)
{
    initializeArguments_ = initializeArguments;
    if (state_ == State::Stopping)
        return; // a stop barrier in flight: not a "start again" moment
    beginSession();
}

void DapClient::stop()
{
    if (!session_) {
        const bool wasLive = state_ != State::Stopped;
        state_ = State::Stopped;
        if (wasLive) // the stopped() edge is for a session that WAS live
            emit stopped();
        return;
    }
    state_ = State::Stopping; // onGone() maps the eviction to Stopped
    if (session_->rpc->isGone()) {
        // Already-gone session (the same honest-teardown branch LspClient
        // documents: the gone latch fired before anyone asked; make sure
        // the pipe is really closed and land the terminal state here).
        if (session_->transport->isRunning())
            session_->transport->stop();
        endSession();
        state_ = State::Stopped;
        emit stopped();
        return;
    }
    session_->rpc->stop(); // the two-phase barrier: `disconnect` REQUEST
                           // (restart:false), then the transport teardown
                           // (NO exit frame: DapCodec has no exit message)
}

void DapClient::restartServer()
{
    if (state_ == State::Ready || state_ == State::Starting
        || state_ == State::Stopping)
        return; // no silent mid-session respawn (the no-auto-restart
                // decision: restart means FAILED/STOPPED -> a fresh session)
    if (session_)
        session_->transport->stop(); // defensive: a half-dead session's pipe
    endSession();
    beginSession(); // stored initialize arguments replay
}

void DapClient::beginSession()
{
    endSession();
    state_ = State::Starting;
    sessionOpened_ = false;
    adapterCapabilities_ = QJsonObject{}; // a fresh handshake re-reads it

    session_ = new Session;
    session_->transport = factory_(this);
    session_->rpc = new RpcClient(session_->transport, &codec_, this);
    registerPolicyHandlers(session_->rpc);

    connect(session_->transport, &Transport::opened, this,
            &DapClient::onOpened);
    connect(session_->transport, &Transport::stderrLine, this,
            &DapClient::serverStderr); // the separate STDERR channel
    // Keep the last issue for the FAILED reason (connected BEFORE start():
    // a synchronous StartFailed must already be captured).
    connect(session_->transport, &Transport::transportError, this,
            [this](Transport::ErrorKind kind, const QString& text) {
                lastTransportIssue_ =
                    QStringLiteral("%1: %2").arg(errorKindName(kind), text);
            });
    connect(session_->rpc, &RpcClient::transportGone, this,
            &DapClient::onGone);

    if (!session_->transport->start())
        enterFailed(QStringLiteral("transport refused to start: %1")
                        .arg(lastTransportIssue_));
}

void DapClient::endSession()
{
    if (!session_)
        return;
    // The LspClient teardown ritual, carried across on purpose (deleteLater
    // inside callbacks; the transport dies only once closed() has fired, so
    // the ~QProcess can never SIGKILL a live adapter child mid-escalation).
    session_->rpc->disconnect(this);
    session_->transport->disconnect(this);
    session_->rpc->deleteLater();
    if (session_->transport->isRunning()) {
        connect(session_->transport, &Transport::closed, session_->transport,
                &QObject::deleteLater);
    } else {
        session_->transport->deleteLater();
    }
    delete session_;
    session_ = nullptr;
}

void DapClient::onOpened()
{
    if (state_ != State::Starting || !session_)
        return;
    sessionOpened_ = true;
    // The pinned handshake (same 10 s initialize budget as LSP). The PLAN
    // initialize ARGS bundle: the three client-suggested identity keys plus
    // the clientOS/locale pair and the 1-BASED PAIR — declaring
    // linesStartAt1/columnsStartAt1 true IS the spec default (so the
    // glue-side conversions stay −1 in / +1 out unconditionally), and NO
    // clientCapabilities member is sent at all (honest-absence style: we
    // declare no reverse-request capabilities; their relays answer
    // honestly anyway). Everything the caller passed outranks a default.
    QJsonObject args = initializeArguments_;
    if (!args.contains(QStringLiteral("adapterID")))
        args.insert(QStringLiteral("adapterID"), adapterId_);
    if (!args.contains(QStringLiteral("clientID")))
        args.insert(QStringLiteral("clientID"),
                    QStringLiteral("qtmonaco-native"));
    if (!args.contains(QStringLiteral("clientName")))
        args.insert(QStringLiteral("clientName"),
                    QStringLiteral("qtmonaco-native"));
    if (!args.contains(QStringLiteral("clientOS"))) {
#if defined(Q_OS_WIN)
        args.insert(QStringLiteral("clientOS"), QStringLiteral("windows"));
#elif defined(Q_OS_MACOS)
        args.insert(QStringLiteral("clientOS"), QStringLiteral("osx"));
#else
        args.insert(QStringLiteral("clientOS"), QStringLiteral("linux"));
#endif
    }
    if (!args.contains(QStringLiteral("locale")))
        args.insert(QStringLiteral("locale"), QLocale::system().name());
    if (!args.contains(QStringLiteral("linesStartAt1")))
        args.insert(QStringLiteral("linesStartAt1"), true);
    if (!args.contains(QStringLiteral("columnsStartAt1")))
        args.insert(QStringLiteral("columnsStartAt1"), true);
    session_->rpc->request(QStringLiteral("initialize"), args,
                           [this](const RpcClient::Reply& reply) {
                               onHandshakeReply(reply);
                           },
                           RpcClient::kInitializeTimeoutMs);
}

void DapClient::onHandshakeReply(const RpcClient::Reply& reply)
{
    if (state_ != State::Starting || !session_)
        return; // stale callback across a session swap: drop
    if (reply.isError) {
        enterFailed(QStringLiteral("initialize failed: %1").arg(reply.message));
        return;
    }
    adapterCapabilities_ = reply.result.toObject();
    // The SECOND handshake half: the `initialized` EVENT frame (DAP's
    // outbound one-way — buildNotification's event spelling, the seq is
    // RpcClient's own). The spec pins initialize -> initialized -> commands;
    // ready() fires only after this write.
    session_->rpc->notify(QStringLiteral("initialized"), QJsonObject{});
    state_ = State::Ready;
    emit ready();
}

// The gone paths. NO auto-restart (PLAN-decided for adapters): anything
// unwanted lands in Failed, restartServer() is the door.
void DapClient::onGone()
{
    switch (state_) {
    case State::Stopping:
        state_ = State::Stopped;
        endSession();
        emit stopped(); // the clean-barrier terminal edge (D3 hub's feed
                        // teardown rides this; unwanted death → Failed,
                        // never here)
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
        enterFailed(QStringLiteral("transport never opened: %1")
                        .arg(lastTransportIssue_));
        return;
    }
    enterFailed(QStringLiteral("transport closed: %1")
                    .arg(lastTransportIssue_));
    // (the LspClient auto-restart branch deliberately has NO twin here —
    // no restart budget exists for an adapter; the reason string above is
    // what the user's UI shows next to the restart button)
}

void DapClient::enterFailed(const QString& reason)
{
    if (state_ == State::Stopping)
        return; // the stop barrier outranks any failure
    if (state_ == State::Failed)
        return; // failed() fires exactly once per episode
    state_ = State::Failed;
    if (session_) {
        // A live pipe behind a failed handshake still shuts down honestly
        // (disconnect + the Transport's kill escalation).
        session_->rpc->stop();
    }
    emit failed(reason);
}

void DapClient::registerPolicyHandlers(RpcClient* rpc)
{
    // EVENTS: one generic relay for the whole spec (and any future event
    // name) — the dapEvent signal. With no specific handlers installed,
    // the catch-all sees every event; an event reaching a disconnected
    // signal is the spec's own ignore rule. The ONE client-layer policy on
    // the relay (PLAN D2): a `capabilities` EVENT re-stores the capability
    // bag (adapters publish revised capabilities that way) — the event
    // still rides the relay, the hub hears it too.
    rpc->setAnyNotificationHandler([this](const QString& name,
                                          const QJsonValue& body) {
        if (name == QLatin1String("capabilities") && body.isObject())
            adapterCapabilities_ = body.toObject();
        emit dapEvent(name, body);
    });

    // The progress trio (adapter->client REQUESTS in the modern spec):
    // answered {} from the static shelf — real progress UI is out of
    // car 4's scope, but an honest empty result beats relaying something
    // nobody would answer. The shelf outranks the relay below, so these
    // never reach dapRequest.
    const QJsonObject emptyResult;
    rpc->registerMethod(QStringLiteral("progressStart"), emptyResult);
    rpc->registerMethod(QStringLiteral("progressUpdate"), emptyResult);
    rpc->registerMethod(QStringLiteral("progressEnd"), emptyResult);

    // REVERSE REQUESTS (runInTerminal, startDebugging, anything the
    // adapter invents): relay first, honest default second — the first
    // answer to an id wins (RpcClient refuses a second), so glue that
    // answers DURING the signal beats this default, and glue that does
    // not gets an in-envelope failure naming the command (never a silent
    // hang the adapter has to time out on; never a fabricated success).
    rpc->setAnyRequestHandler([rpc, this](const QJsonObject& request) {
        const QJsonValue id = request.value(QStringLiteral("seq"));
        emit dapRequest(request);
        rpc->respondError(
            id, 0,
            QStringLiteral("not handled: %1")
                .arg(request.value(QStringLiteral("command")).toString()));
    });

    // INBOUND CANCEL (the `cancel` COMMAND recognized through the codec's
    // vocabulary, NOT a hardcoded name here): claim the pending locally,
    // answer per spec — success:true when it was cancellable, the honest
    // "request N cannot be cancelled" failure otherwise (the DAP spec's
    // own wording; error.id 0 = we invent no code).
    rpc->setOnCancelRequested([rpc, this](qint64 targetId,
                                          const QJsonObject& message) {
        const QJsonValue id = message.value(QStringLiteral("seq"));
        if (rpc->cancelPending(targetId, RpcClient::kErrCancelledByPeer,
                               QStringLiteral("cancelled by adapter"))) {
            rpc->respond(id, QJsonObject{}); // CancelResult (empty body)
        } else {
            rpc->respondError(
                id, 0,
                QStringLiteral("request %1 cannot be cancelled")
                    .arg(targetId));
        }
    });
}

qint64 DapClient::request(const QString& command, const QJsonValue& arguments,
                          RpcClient::ReplyHandler handler, int timeoutMs)
{
    if (state_ != State::Ready || !session_) {
        if (handler) {
            // Queued (the LspClient pattern): the door's contract is
            // "answers exactly once", and answering INSIDE the call
            // would surprise re-entrant callers.
            QTimer::singleShot(0, this, [handler = std::move(handler)] {
                handler({true, {}, RpcClient::kErrNotRunning,
                         QStringLiteral("DAP client is not Ready"), {}});
            });
        }
        return -1;
    }
    return session_->rpc->request(command, arguments, std::move(handler),
                                  timeoutMs);
}

void DapClient::event(const QString& name, const QJsonValue& body)
{
    if (state_ != State::Ready || !session_)
        return; // same drop rule as LspClient::notify (documented)
    session_->rpc->notify(name, body);
}

void DapClient::cancel(qint64 requestId)
{
    if (!session_ || session_->rpc->isGone())
        return; // the pendings died with the session
    session_->rpc->cancel(requestId);
}

bool DapClient::respond(const QJsonValue& id, const QJsonValue& body)
{
    if (!session_)
        return false;
    return session_->rpc->respond(id, body);
}

bool DapClient::respondError(const QJsonValue& id, int code,
                             const QString& message, const QJsonValue& data)
{
    if (!session_)
        return false;
    return session_->rpc->respondError(id, code, message, data);
}

} // namespace qmn::dap
