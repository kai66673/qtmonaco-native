#include "glue/DapHub.hpp"

#include "dap/DapClient.hpp"
#include "glue/ProjectSession.hpp"
#include "protocol/ProcessTransport.hpp"
#include "protocol/RpcClient.hpp"
#include "view/CodeDocument.hpp"
#include "view/CodeEditor.hpp"

#include <QEventLoop>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <tuple>
#include <utility>

namespace qmn::glue {

using qmn::dap::DapClient;
using qmn::protocol::ProcessTransport;
using Reply = qmn::protocol::RpcClient::Reply; // the D1 seam's reply record
using qmn::protocol::RpcClient;
using qmn::protocol::Transport;
using qmn::view::CodeDocument;
using qmn::view::CodeEditor;

namespace {

// The SAME normalization ProjectSession applies to every uri it registers
// (its static is private; the semantics are four lines, and matching them
// is what "resolve against the session's uri map" means for keys the app
// hands us from DAP payloads — a bare path normalizes, a scheme passes
// through). If the two spellings ever drift, the uri map IS the proof.
QString normalizeUriLocal(const QString& uriOrPath)
{
    if (uriOrPath.contains(QLatin1String("://")))
        return uriOrPath;
    return QUrl::fromLocalFile(QFileInfo(uriOrPath).absoluteFilePath()).toString();
}

bool isWordChar(const QChar& ch)
{
    return ch.isLetterOrNumber() || ch == QLatin1Char('_');
}

} // namespace

DapHub::DapHub(QObject* parent)
    : QObject(parent)
    , factory_([](const QString& program, const QStringList& args,
                  QObject* tp) -> Transport* {
          return new ProcessTransport(program, args, tp);
      })
{
}

DapHub::~DapHub()
{
    closingDown_ = true;
    ++stopGen_;
    ++sessionGen_; // in-flight async replies all drop
    if (client_)
        client_->stop(); // the disconnect barrier, codec-shaped
    waitForStops(); // the BOUNDED pump — the M8 machine-4 lesson, paid twice
}

void DapHub::waitForStops()
{
    QEventLoop pump;
    QTimer wake; // heartbeat: 25 ms slices, no busy spin
    wake.setInterval(25);
    connect(&wake, &QTimer::timeout, &pump, &QEventLoop::quit);
    wake.start();

    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kStopBarrierMs) {
        if (!client_
            || (client_->state() != DapClient::State::Starting
                && client_->state() != DapClient::State::Ready
                && client_->state() != DapClient::State::Stopping)) {
            return; // terminal (Stopped/Failed) or never started
        }
        pump.exec();
    }
    // Out of grace: DapClient's own teardown closes the pipes and the
    // transport's terminate/kill ladder finishes the job.
}

void DapHub::setTransportFactory(TransportFactory factory)
{
    factory_ = std::move(factory);
}

void DapHub::attachProject(ProjectSession* session)
{
    session_ = session; // QPointer: a dying session simply unattaches
}

// ------------------------------------------------------------------ session

bool DapHub::startDebugging(const QString& adapterProgram,
                            const QStringList& adapterArgs,
                            const QJsonObject& launchArgs,
                            const QString& adapterId)
{
    if (adapterProgram.isEmpty() || !factory_)
        return false;
    if (client_
        && (client_->state() == DapClient::State::Starting
            || client_->state() == DapClient::State::Ready
            || client_->state() == DapClient::State::Stopping)) {
        return false; // ONE session at a time (PLAN): the previous one must
                      // end before a start (stopDebugging is the door)
    }
    // A Failed/Stopped client is REPLACED (a fresh session from a fresh
    // client; the DapClient restartServer door belongs to that layer's
    // tests, the hub's restart story is this door — recorded shape).
    client_.reset();
    ++sessionGen_;
    ++stopGen_;
    setupPending_ = 0;     // fresh session, fresh setup accounting
    cfgDoneWaiting_ = false;

    adapterProgram_ = adapterProgram;
    adapterArgs_ = adapterArgs;
    adapterId_ = adapterId.isEmpty() ? QFileInfo(adapterProgram).fileName()
                                     : adapterId;
    launchArgs_ = launchArgs;
    if (!launchArgs_.contains(QStringLiteral("request")))
        launchArgs_.insert(QStringLiteral("request"),
                           QStringLiteral("launch")); // the spec-required
    if (!launchArgs_.contains(QStringLiteral("noDebug")))
        launchArgs_.insert(QStringLiteral("noDebug"), false); // member pair

    const quint64 gen = sessionGen_;
    client_.reset(new DapClient(
        [this](QObject* parent) -> Transport* {
            return factory_(adapterProgram_, adapterArgs_, parent);
        },
        adapterId_, this));
    connect(client_.get(), &DapClient::ready, this, [this, gen] {
        if (gen != sessionGen_)
            return;
        // The launch REQUEST first (the VS Code order: launch, then
        // configuration, then configurationDone). The answer is not inert:
        // an error (a real adapter refusing the program is the everyday
        // shape) rides launchRejected — the session itself stays, the app
        // decides (the recorded split, same as terminated's). The launch
        // is DELIBERATELY NOT in the configurationDone setup band (the
        // live-gdb daplog, 2026-11-21): gdb's python adapter answers
        // `launch` only AFTER configurationDone — gating the word on the
        // launch answer deadlocks the handshake against the adapter's own
        // flow. Only the breakpoint pushes are the race gdb actually has
        // (an early configurationDone starts the inferior before the
        // pending breakpoints resolve).
        client_->request(QStringLiteral("launch"), launchArgs_,
                         [this, gen](const qmn::protocol::RpcClient::Reply& reply) {
                             if (gen != sessionGen_ || !reply.isError)
                                 return;
                             emit launchRejected(reply.message.isEmpty()
                                                     ? QStringLiteral("launch rejected")
                                                     : reply.message);
                         });
        // Then every stored breakpoint re-pushes (the ready() re-push is
        // the same code path a session restart rides):
        const QStringList uris = bound_.keys(); // SNAPSHOT: handlers may edit
        for (const QString& uri : uris)
            sendSetBreakpoints(uri);
        emit sessionStarted();
    });
    connect(client_.get(), &DapClient::failed, this, [this, gen](const QString& reason) {
        if (gen != sessionGen_)
            return;
        onAdapterGone(reason);
    });
    connect(client_.get(), &DapClient::stopped, this, [this, gen] {
        if (gen != sessionGen_)
            return;
        onAdapterGone(QStringLiteral("stopped"));
    });
    connect(client_.get(), &DapClient::dapEvent, this,
            [this, gen](const QString& name, const QJsonValue& body) {
                if (gen != sessionGen_)
                    return;
                emit dapEvent(name, body);
                if (name == QLatin1String("capabilities")) {
                    // The client RE-STORED its bag (the D2 rule); nothing
                    // to mirror — every capability read goes live through
                    // client().
                } else if (name == QLatin1String("stopped")) {
                    handleStopped(body.toObject());
                } else if (name == QLatin1String("continued")) {
                    ++stopGen_;
                    // S1-3: the thread is RUNNING again — the typed control
                    // doors' "known stopped thread" memory dies here (no
                    // reader of lastThreadId_ survives a continue today, so
                    // no D3 pin moves).
                    lastThreadId_ = 0;
                    clearStopFeed();
                    emit debugContinued();
                } else if (name == QLatin1String("breakpoint")) {
                    handleBreakpointEvent(body);
                } else if (name == QLatin1String("loadedSource")) {
                    const QJsonObject src = body.toObject().value(QStringLiteral("source")).toObject();
                    const int ref = src.value(QStringLiteral("sourceReference")).toInt();
                    const QString path = src.value(QStringLiteral("path")).toString();
                    if (ref > 0 && !path.isEmpty())
                        sourceRefPaths_.insert(ref, path);
                } else if (name == QLatin1String("terminated")) {
                    // The DEBUG session ended, the PROCESS may live on for
                    // a fresh launch: clear the feeds, relay honestly, and
                    // leave the pipe to the app's judgement (stopDebugging
                    // or another startDebugging — v1 records this split).
                    ++stopGen_;
                    clearAllFeeds();
                    lastThreadId_ = 0; // S1-3: the debug session is over —
                                       // the typed doors must not step a
                                       // thread the adapter already retired
                                       // (the pipe may still be Ready).
                    emit sessionEnded(QStringLiteral("terminated"));
                }
            });
    connect(client_.get(), &DapClient::dapRequest, this, [this, gen](const QJsonObject& request) {
        if (gen != sessionGen_)
            return;
        emit dapRequest(request); // answer through client()->respond during
                                  // the signal; the client's honest default
                                  // fires the moment the signal returns
    });
    connect(client_.get(), &DapClient::serverStderr, this, [this, gen](const QString& line) {
        if (gen != sessionGen_)
            return;
        emit adapterStderr(line); // the channel split carries (never a dapEvent)
    });

    client_->start(); // the pinned handshake (initialize request -> Ready
                      // on its response; the adapter's own `initialized`
                      // event rides the relay only); commands before Ready
                      // answer kErrNotRunning
    return true;
}

void DapHub::stopDebugging()
{
    if (client_)
        client_->stop(); // the two-phase barrier; stopped() lands us in
                         // onAdapterGone("stopped") (feeds + sessionEnded)
}

bool DapHub::terminateDebugging()
{
    if (!client_ || !client_->isReady())
        return false;
    const bool supported = client_->adapterCapabilities()
                               .value(QStringLiteral("supportsTerminateRequest"))
                               .toBool();
    if (!supported) {
        stopDebugging(); // the honest fallback: no terminate here
        return false;
    }
    client_->request(QStringLiteral("terminate"), QJsonValue(QJsonValue::Undefined),
                     [this](const Reply&) { stopDebugging(); });
    return true;
}

bool DapHub::isDebugging() const
{
    return client_
        && (client_->state() == DapClient::State::Starting
            || client_->state() == DapClient::State::Ready);
}

bool DapHub::isReady() const
{
    return client_ && client_->isReady();
}

QJsonObject DapHub::adapterCapabilities() const
{
    return client_ ? client_->adapterCapabilities() : QJsonObject{};
}

void DapHub::configurationDone()
{
    if (!client_ || !client_->isReady())
        return;
    const QJsonObject caps = client_->adapterCapabilities();
    // The DAP spec's own note: some adapters DEFAULT this to true and
    // never publish the key — absence means SEND, an explicit false skips.
    if (caps.contains(QStringLiteral("supportsConfigurationDoneRequest"))
        && !caps.value(QStringLiteral("supportsConfigurationDoneRequest")).toBool())
        return;
    if (setupPending_ > 0) {
        cfgDoneWaiting_ = true; // one-shot: the word travels when the last
        return;                 // setup answer lands (settleConfigurationDone)
    }
    client_->request(QStringLiteral("configurationDone"),
                     QJsonValue(QJsonValue::Undefined), nullptr);
}

void DapHub::settleConfigurationDone()
{
    if (setupPending_ > 0 || !cfgDoneWaiting_ || closingDown_)
        return;
    cfgDoneWaiting_ = false;
    configurationDone(); // re-enters the door: the caps gate re-runs, and
                         // the request is ordered strictly AFTER the setup
                         // answers — the live-gdb serialization rule
}

qint64 DapHub::request(const QString& command, const QJsonValue& arguments)
{
    if (!client_ || !client_->isReady())
        return -1; // the state gate stays honest (the client would answer
                   // kErrNotRunning anyway, but the door says so directly)
    return client_->request(command, arguments, nullptr);
}

// ------------------------------------------------- the typed control face (S1-3)

bool DapHub::threadControl(const QString& command)
{
    // The two-part gate, in this order on purpose: Ready first (the wire
    // must be standing), then the stopped-thread memory — a control
    // request without a threadId names no thread to control, and DAP's
    // control commands all REQUIRE arguments.threadId. Riding request()
    // keeps the wire byte-identical to the raw passthrough.
    if (!isReady() || lastThreadId_ == 0)
        return false;
    return request(command, QJsonObject{{QStringLiteral("threadId"),
                                         lastThreadId_}})
        >= 0; // request() already gated Ready; the id only fails as -1
}

bool DapHub::continueDebugging() { return threadControl(QStringLiteral("continue")); }
bool DapHub::stepOver()          { return threadControl(QStringLiteral("next")); }
bool DapHub::stepIn()            { return threadControl(QStringLiteral("stepIn")); }
bool DapHub::stepOut()           { return threadControl(QStringLiteral("stepOut")); }

// ---------------------------------------------------------- answer doors (E3)

bool DapHub::answerRequest(const QJsonValue& id, const QJsonValue& body)
{
    // NO state gate here, deliberately (the request door above has one):
    // an ANSWER is judged by RpcClient's openIncoming_ table, not by the
    // lifecycle — a session that died between the relay and the answer
    // must read false, and inventing a kErrNotRunning-shaped frame for a
    // reverse answer would be a wire lie. The client doors are the whole
    // mechanism; the hub forwards.
    return client_ && client_->respond(id, body);
}

bool DapHub::answerRequestError(const QJsonValue& id, int code,
                                const QString& message,
                                const QJsonValue& data)
{
    return client_ && client_->respondError(id, code, message, data);
}

// ------------------------------------------------------------- breakpoints

QString DapHub::localPathForUri(const QString& uri)
{
    const QUrl url(uri);
    return url.isLocalFile() ? url.toLocalFile() : uri;
}

QString DapHub::resolveEditorUri(CodeEditor* editor, QString* normalizedOut) const
{
    if (!editor || !editor->document())
        return QString();
    QString uri = editor->document()->documentUri();
    if (uri.isEmpty() && session_)
        uri = session_->uriForEditor(editor); // the registry's key for this editor
    if (uri.isEmpty())
        return QString();
    if (normalizedOut)
        *normalizedOut = normalizeUriLocal(uri);
    return uri;
}

DapHub::BoundSource* DapHub::boundForUri(const QString& uri)
{
    BoundSource& bound = bound_[uri];
    if (!bound.resend) {
        bound.resend = new QTimer(this); // hub-parented; single-shot debounce
        bound.resend->setSingleShot(true);
        const QString keyed = uri;
        connect(bound.resend, &QTimer::timeout, this, [this, keyed] {
            sendSetBreakpoints(keyed);
        });
    }
    return &bound;
}

DapHub::BoundSource* DapHub::findBound(const QString& uri) const
{
    const auto it = bound_.constFind(uri);
    return it == bound_.constEnd() ? nullptr : const_cast<BoundSource*>(&it.value());
}

bool DapHub::setBreakpoints(CodeEditor* editor, const QVector<int>& modelLines)
{
    QString normalized;
    if (resolveEditorUri(editor, &normalized).isEmpty())
        return false; // a breakpoint needs an identity (no uri = no source)

    BoundSource* bound = boundForUri(normalized);
    // E1: the ASKING view joins the store (a joined view is a legitimate
    // asker — the uri resolves through document identity either way). The
    // session's attach order stays the authority for fan-out while a
    // session is attached (liveViews answers from it); this append is the
    // no-session half of the record.
    if (!bound->views.contains(editor))
        bound->views.append(editor);
    // Re-anchor the debounce onto THIS editor's document EXACTLY ONCE PER
    // URI (the connection dies with an old document on its own; rewiring
    // on an editor swap is the same honesty as the LspHub's
    // re-registration). E1 note: with views of ONE shared document the
    // door asks can come from any of them — a second connect would DOUBLE
    // the setBreakpoints resends, so the connect stays single and
    // uri-keyed (the tst_dap debounce-burst pin is the oracle).
    if (bound->editConn)
        disconnect(bound->editConn);
    CodeDocument* doc = editor->document();
    bound->editConn = connect(doc, &CodeDocument::textChanged, this,
                              [this, normalized] {
                                  BoundSource* b = findBound(normalized);
                                  if (b && b->resend && !closingDown_)
                                      b->resend->start(kBreakpointResendMs);
                              });

    QVector<BrEntry> entries;
    entries.reserve(modelLines.size());
    for (int line : modelLines) {
        BrEntry entry;
        entry.desiredLine = line;
        entry.actualLine = line;
        entries.append(entry);
    }
    bound->entries = entries; // a new request re-assigns the DAP ids (the
                              // old ones die with this setBreakpoints)

    sendSetBreakpoints(normalized); // silent no-op while not Ready — the
                                    // ready() re-push covers that
    return true;
}

QVector<int> DapHub::requestedBreakpoints(const CodeEditor* editor) const
{
    QString normalized;
    if (resolveEditorUri(const_cast<CodeEditor*>(editor), &normalized).isEmpty())
        return {};
    const BoundSource* bound = findBound(normalized);
    if (!bound)
        return {};
    QVector<int> lines;
    lines.reserve(bound->entries.size());
    for (const BrEntry& entry : bound->entries)
        lines.append(entry.desiredLine);
    return lines;
}

QMap<int, int> DapHub::breakpointMap(const CodeEditor* editor) const
{
    QString normalized;
    if (resolveEditorUri(const_cast<CodeEditor*>(editor), &normalized).isEmpty())
        return {};
    const BoundSource* bound = findBound(normalized);
    if (!bound)
        return {};
    QMap<int, int> map;
    for (const BrEntry& entry : bound->entries) {
        if (entry.verified) // "only while a reply carried that line": the
                            // D2/PLAN shape — unverified entries stay OUT
            map.insert(entry.desiredLine, entry.actualLine);
    }
    return map;
}

QList<CodeEditor*> DapHub::editorsForSourcePath(const QString& path) const
{
    if (!session_ || path.isEmpty())
        return {};
    // ONE lookup covers "exact, then bare-path normalization both sides":
    // editorsForUri normalizes its argument exactly like openDocument
    // registered the key, and a scheme-bearing uri passes through
    // unchanged (ProjectSession::normalizeUri's own rule). E1: the answer
    // is EVERY live view, owner first — the car-D alias-paint divergence
    // (dots on the OWNER only) closed here; every feed below fans out over
    // this list and the caret door is the single documented exception.
    return session_->editorsForUri(path);
}

QList<CodeEditor*> DapHub::liveViews(BoundSource& bound, const QString& uri) const
{
    QList<CodeEditor*> fromSession = editorsForSourcePath(localPathForUri(uri));
    if (fromSession.isEmpty()) {
        for (const QPointer<CodeEditor>& view : bound.views) {
            if (view && view->document())
                fromSession.append(view.data());
        }
    }
    bound.views.clear();
    for (CodeEditor* view : fromSession)
        bound.views.append(view);
    return fromSession;
}

void DapHub::sendSetBreakpoints(const QString& uri)
{
    if (!client_ || !client_->isReady())
        return; // the ready() re-push is the other half of this door
    BoundSource* bound = findBound(uri);
    if (!bound)
        return;

    liveViews(*bound, uri); // refresh the view store (bookkeeping only —
                            // the PAYLOAD speaks from the uri: source.path
                            // never needs a view)

    const QString path = localPathForUri(uri);
    QJsonObject source;
    source.insert(QStringLiteral("name"), QFileInfo(path).fileName());
    source.insert(QStringLiteral("path"), path);
    QJsonArray breakpoints;
    for (const BrEntry& entry : bound->entries) {
        // The 1-based pair the handshake DECLARED (linesStartAt1=true is
        // the spec default and the client's own fill — the +/-1 stays
        // total, and the D3 tripwires pin it against dapfake):
        breakpoints.append(QJsonObject{{QStringLiteral("line"),
                                        entry.desiredLine + 1}});
    }
    QJsonObject args;
    args.insert(QStringLiteral("source"), source);
    args.insert(QStringLiteral("breakpoints"), breakpoints);

    const quint64 gen = sessionGen_;
    ++setupPending_; // every push belongs to the SETUP band until answered
    client_->request(QStringLiteral("setBreakpoints"), args,
                     [this, uri, gen](const Reply& reply) {
                         if (gen != sessionGen_ || closingDown_)
                             return;
                         --setupPending_;
                         settleConfigurationDone();
                         if (reply.isError) {
                             // The command FAILED: honest unverified with
                             // the adapter's/local's reason (the iide
                             // "keep the dot lying" anti-pattern dies here).
                             markAllUnverified(uri, reply.message);
                             feedDecorations(uri);
                             return;
                         }
                         applyBreakpointBody(uri, reply.result.toObject());
                     });
}

void DapHub::applyBreakpointBody(const QString& uri, const QJsonObject& body)
{
    BoundSource* bound = findBound(uri);
    if (!bound)
        return;
    const QJsonArray returned
        = body.value(QStringLiteral("breakpoints")).toArray();
    for (int i = 0; i < bound->entries.size(); ++i) {
        BrEntry& entry = bound->entries[i];
        entry.verified = false;
        entry.actualLine = entry.desiredLine;
        if (i >= returned.size()) {
            entry.message = QStringLiteral("adapter sent no entry for this breakpoint");
            entry.id = 0;
            continue;
        }
        const QJsonObject item = returned.at(i).toObject();
        entry.verified = item.value(QStringLiteral("verified")).toBool();
        if (item.value(QStringLiteral("line")).isDouble())
            entry.actualLine = item.value(QStringLiteral("line")).toInt() - 1;
        entry.message = item.value(QStringLiteral("message"))
                            .toString(item.value(QStringLiteral("instruction"))
                                          .toString());
        entry.id = item.value(QStringLiteral("id")).toInt();
        if (!entry.verified && entry.message.isEmpty())
            entry.message = QStringLiteral("breakpoint not verified");
    }
    updateSourceRefs(uri, returned);
    feedDecorations(uri);
}

void DapHub::updateSourceRefs(const QString& uri, const QJsonArray& bodyBreakpoints)
{
    Q_UNUSED(uri); // the map is global by DAP's own bookkeeping (ids are
                   // adapter-wide), not per source — the uri stays in the
                   // signature for the call-site readability
    for (const QJsonValue& value : bodyBreakpoints) {
        const QJsonObject src = value.toObject().value(QStringLiteral("source")).toObject();
        const int ref = src.value(QStringLiteral("sourceReference")).toInt();
        const QString path = src.value(QStringLiteral("path")).toString();
        if (ref > 0 && !path.isEmpty())
            sourceRefPaths_.insert(ref, path);
    }
}

void DapHub::markAllUnverified(const QString& uri, const QString& message)
{
    BoundSource* bound = findBound(uri);
    if (!bound)
        return;
    for (BrEntry& entry : bound->entries) {
        entry.verified = false;
        entry.actualLine = entry.desiredLine;
        entry.message = message.isEmpty() ? QStringLiteral("setBreakpoints failed")
                                          : message;
    }
}

void DapHub::feedDecorations(const QString& uri)
{
    BoundSource* bound = findBound(uri);
    if (!bound)
        return;
    const QList<CodeEditor*> views = liveViews(*bound, uri);
    if (views.isEmpty())
        return; // the store stays current; the feed follows the editors

    QVector<int> verifiedLines;
    QVector<CodeEditor::Diagnostic> dapDiags;
    for (const BrEntry& entry : bound->entries) {
        if (entry.verified) {
            verifiedLines.append(entry.actualLine); // the ACTUAL line: a
                                                    // moved breakpoint's
                                                    // dot lands where the
                                                    // adapter says it lives
        } else {
            CodeEditor::Diagnostic diag;
            diag.startLine = entry.desiredLine;
            diag.endLine = entry.desiredLine;
            diag.endCol = 1; // the view clamps to the line when painting
            diag.severity = CodeEditor::Severity::Warning;
            diag.message = entry.message.isEmpty()
                ? QStringLiteral("breakpoint not verified")
                : entry.message;
            diag.source = QStringLiteral("dap"); // the producer label —
                                                 // what the merge below keys by
            dapDiags.append(diag);
        }
    }
    std::sort(verifiedLines.begin(), verifiedLines.end());

    // E1 fan-out: ONE computation, painted on EVERY live view (the car-D
    // alias divergence — dots on the OWNER only — dies here). The
    // diagnostic merge reads EACH view's own table: another producer may
    // have painted one view and not the other, and per-view honesty is
    // the point of a per-view decoration table.
    for (CodeEditor* editor : views) {
        editor->setBreakpointLines(verifiedLines); // M5 door (1)

        // The diagnostic merge: dap REPLACES ONLY its own rows (source
        // "dap") and never stomps another producer's (clangd's list on
        // the same tab is LspHub's full-replace feed — the two producers
        // coexist here).
        QVector<CodeEditor::Diagnostic> merged;
        const QVector<CodeEditor::Diagnostic> current = editor->diagnostics();
        merged.reserve(current.size() + dapDiags.size());
        for (const CodeEditor::Diagnostic& diag : current)
            if (diag.source != QLatin1String("dap"))
                merged.append(diag);
        merged.append(dapDiags);
        editor->setDiagnostics(merged); // M5 door (3)
    }
}

void DapHub::handleBreakpointEvent(const QJsonValue& body)
{
    const QJsonObject obj = body.toObject();
    const QJsonObject bp = obj.value(QStringLiteral("breakpoint")).toObject();
    const int id = bp.value(QStringLiteral("id")).toInt();
    if (id <= 0)
        return; // no id to key by: the raw event already rode dapEvent
    for (auto it = bound_.begin(); it != bound_.end(); ++it) {
        for (BrEntry& entry : it->entries) {
            if (entry.id != id)
                continue;
            if (bp.contains(QStringLiteral("verified")))
                entry.verified = bp.value(QStringLiteral("verified")).toBool();
            if (bp.value(QStringLiteral("line")).isDouble())
                entry.actualLine = bp.value(QStringLiteral("line")).toInt() - 1;
            const QString message
                = bp.value(QStringLiteral("message")).toString();
            if (!message.isEmpty())
                entry.message = message;
            // The spec's event reason is TOP LEVEL (changed/new/removed):
            // a removed breakpoint never verifies again under this id.
            if (obj.value(QStringLiteral("reason")).toString() == QLatin1String("removed"))
                entry.verified = false;
            if (!entry.verified && entry.message.isEmpty())
                entry.message = QStringLiteral("breakpoint not verified");
            feedDecorations(it.key());
            return; // DAP ids are adapter-unique: first match is THE match
        }
    }
}

// ----------------------------------------------------------- stopped feed

void DapHub::handleStopped(const QJsonObject& body)
{
    ++stopGen_;
    const quint64 gen = stopGen_;
    lastThreadId_ = body.value(QStringLiteral("threadId")).isDouble()
        ? body.value(QStringLiteral("threadId")).toInt()
        : 1; // the spec requires threadId; absence reads as thread 1 (the
             // single-thread v1 shape, recorded)
    emit debugStopped(lastThreadId_,
                      body.value(QStringLiteral("reason")).toString());

    if (!client_ || !client_->isReady())
        return;
    client_->request(QStringLiteral("stackTrace"),
                     QJsonObject{{QStringLiteral("threadId"), lastThreadId_},
                                 {QStringLiteral("startFrame"), 0},
                                 {QStringLiteral("levels"), 1}},
                     [this, gen](const Reply& reply) {
                         handleStackTrace(gen, reply);
                     });
}

void DapHub::handleStackTrace(quint64 gen, const Reply& reply)
{
    if (gen != stopGen_ || closingDown_)
        return; // superseded by a newer stopped/continued/death
    if (reply.isError)
        return; // the event already surfaced; a failed stack feed paints
                // nothing (honest, recorded)
    const QJsonArray frames
        = reply.result.toObject().value(QStringLiteral("stackFrames")).toArray();
    if (frames.isEmpty())
        return;
    const QJsonObject top = frames.at(0).toObject();

    // source.path, or sourceReference -> path through the hub's own store
    // (the store fills from setBreakpoints replies + loadedSource events);
    // NEVER disk (the G2 rule).
    QJsonObject source = top.value(QStringLiteral("source")).toObject();
    QString path = source.value(QStringLiteral("path")).toString();
    if (path.isEmpty() && source.value(QStringLiteral("sourceReference")).toInt() > 0) {
        path = sourceRefPaths_.value(
            source.value(QStringLiteral("sourceReference")).toInt());
        if (!path.isEmpty() && source.isEmpty())
            source = QJsonObject{{QStringLiteral("path"), path}};
    }

    const QList<CodeEditor*> views = editorsForSourcePath(path);
    // Shared-document identity (car D): every view in the list prints the
    // SAME document, so the first live one stands for the text scan and
    // the clamps below are computed once for all.
    CodeDocument* doc = views.isEmpty() ? nullptr : views.first()->document();
    if (!doc) {
        emit sourceUnresolved(source, top); // the app's call to open an
                                            // editor + openDocument() it
        return;
    }

    // The ±1 conversion (the declared pair makes it total) + the clamps —
    // RECORDED limitation: DAP columns are adapter-unit-dependent, we
    // declared UTF-16 units and trust the declaration; the tripwire pins
    // the boundary on an ASCII line.
    int line = top.value(QStringLiteral("line")).toInt() - 1;
    line = std::clamp(line, 0, doc->lineCount() - 1);
    int col = top.value(QStringLiteral("column")).isDouble()
        ? top.value(QStringLiteral("column")).toInt() - 1
        : 0;
    col = std::clamp(col, 0, static_cast<int>(doc->lineText(line).size()));

    // E1 fan-out: the band lands on EVERY live view; the CARET rides the
    // OWNER (views.first(), the session's attach order) ONLY — caret and
    // selection are per-view state, and a stop must not yank the caret of
    // a view the user is typing in (the recorded owner-only rule, pinned
    // by its own two-view leg). After an owner-death handover the
    // session's next answer puts a survivor first, so the NEXT stop owns
    // the survivor's caret.
    views.first()->setPosition({line, col}); // the public set_cursor caret
                                             // door (selection-clearing by
                                             // design), no new view API
    for (CodeEditor* view : views)
        view->setHighlightedLines(line, line); // the exec-pointer band
    stopViews_.clear();
    for (CodeEditor* view : views)
        stopViews_.append(view);

    // The StackFrame's identity field is `id` (the REQUEST argument is the
    // one spelled frameId — reading "frameId" off the frame would silently
    // kill the value-hint leg):
    const double frameId = top.value(QStringLiteral("id")).toDouble();
    if (frameId == 0)
        return; // no frameId = no scopes leg (spec-required; an adapter that
                // omits it honestly gets no value hints, never a wrong one)
    // The chain carries the SNAPSHOT of QPointers (copyable through the
    // lambda): the async legs may land after a view death, and the
    // survivors of this snapshot keep the feed (the handover rule).
    const QList<QPointer<CodeEditor>> stopViews = stopViews_;
    client_->request(QStringLiteral("scopes"),
                     QJsonObject{{QStringLiteral("frameId"), frameId}},
                     [this, stopViews, line, gen](const Reply& reply) {
                         handleScopes(stopViews, line, gen, reply);
                     });
}

void DapHub::handleScopes(const QList<QPointer<CodeEditor>>& views, int line,
                          quint64 gen, const Reply& reply)
{
    if (gen != stopGen_ || closingDown_ || reply.isError)
        return;
    const QJsonArray scopes
        = reply.result.toObject().value(QStringLiteral("scopes")).toArray();
    if (scopes.isEmpty())
        return;
    // THE first top-frame scope (PLAN: top level only — nested scopes and
    // children are a recorded deferred sliver).
    const int varsRef = scopes.at(0).toObject()
                            .value(QStringLiteral("variablesReference"))
                            .toInt();
    if (varsRef <= 0)
        return; // spec: 0 means "no variables here"
    client_->request(QStringLiteral("variables"),
                     QJsonObject{{QStringLiteral("variablesReference"), varsRef}},
                     [this, views, line, gen](const Reply& reply) {
                         handleVariables(views, line, gen, reply);
                     });
}

void DapHub::handleVariables(const QList<QPointer<CodeEditor>>& views, int line,
                             quint64 gen, const Reply& reply)
{
    if (gen != stopGen_ || closingDown_ || reply.isError)
        return;
    if (stopViews_.isEmpty())
        return; // the band this chain answers for is gone (continued/death
                // bumped the gen, but the stop-feed guard makes it airtight)
    // Prune views that died during the async chain — the SURVIVORS still
    // get the hints (a joined view's or owner's death is not the end of
    // the stop feed).
    QList<CodeEditor*> live;
    CodeDocument* doc = nullptr;
    for (const QPointer<CodeEditor>& view : views) {
        if (!view || !view->document())
            continue;
        live.append(view.data());
        if (!doc)
            doc = view->document();
    }
    if (!doc)
        return;

    // name -> value, TOP LEVEL only, first kMaxValueHints entries, first
    // NAME wins against a duplicate:
    QHash<QString, QString> wanted;
    const QJsonArray variables
        = reply.result.toObject().value(QStringLiteral("variables")).toArray();
    for (int i = 0; i < variables.size() && wanted.size() < kMaxValueHints; ++i) {
        const QJsonObject var = variables.at(i).toObject();
        const QString name = var.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || wanted.contains(name))
            continue;
        wanted.insert(name, var.value(QStringLiteral("value")).toString());
    }
    if (wanted.isEmpty())
        return;

    // WORD matching on the stopped line (code-unit scan — the model-wide
    // convention, grapheme honesty unchanged): hint at the FIRST
    // occurrence, anchored just past the word (the M5 door's
    // "into existing whitespace after the text" anchor rule).
    const QString text = doc->lineText(line);
    QVector<CodeEditor::InlayHint> hints;
    int hintsUsed = 0;
    int pos = 0;
    while (pos < text.size() && hintsUsed < kMaxValueHints) {
        if (!isWordChar(text.at(pos))) {
            ++pos;
            continue;
        }
        const int start = pos;
        while (pos < text.size() && isWordChar(text.at(pos)))
            ++pos;
        const QString word = text.mid(start, pos - start);
        if (!wanted.contains(word))
            continue;
        CodeEditor::InlayHint hint;
        hint.line = line;
        hint.col = pos; // one past the word's last unit = the word-END col
        hint.text = QStringLiteral(" = ") + wanted.value(word);
        hints.append(hint);
        ++hintsUsed;
        wanted.remove(word); // FIRST occurrence per NAME only
    }
    std::sort(hints.begin(), hints.end(),
              [](const CodeEditor::InlayHint& a, const CodeEditor::InlayHint& b) {
                  return std::make_tuple(a.line, a.col)
                       < std::make_tuple(b.line, b.col);
              });
    // E1: the scan ran ONCE on the shared document; the SAME computed
    // hint list feeds every live view (the fan-out rule — per-view text
    // scans over one document would be redundant work, not honesty).
    for (CodeEditor* editor : live)
        editor->setInlayHints(hints); // the M5 door finally carries the
                                      // DAP meaning its name was minted
                                      // with
}

void DapHub::clearStopFeed()
{
    // E1: the stop feed is a LIST — every live view of it loses the band
    // and the hints (the same list that got them, minus the dead).
    for (const QPointer<CodeEditor>& view : stopViews_) {
        if (view && view->document()) {
            view->clearHighlightedLines();
            view->setInlayHints({}); // single-writer convention (the
                                     // LspHub hint-feed precedent)
        }
    }
    stopViews_.clear();
}

void DapHub::clearAllFeeds()
{
    clearStopFeed();
    const QStringList uris = bound_.keys(); // SNAPSHOT: handlers may edit
    for (const QString& uri : uris) {
        BoundSource* bound = findBound(uri);
        if (!bound)
            continue;
        for (BrEntry& entry : bound->entries) {
            entry.verified = false; // the session's VERIFIED world died
            entry.actualLine = entry.desiredLine;
            entry.id = 0;           // with the adapter; the DESIRED truth
        }
        // The feed empties, it does NOT repaint as warnings: mid-session a
        // failing setBreakpoints earns a dap Diagnostic (the adapter said
        // "cannot bind"), but a dead session gets to say nothing — the
        // app's gutter truth stays, the next session's re-push re-verifies.
        // E1 fan-out: every live view empties its own feed (dots + dap
        // rows); a view the session lost keeps whatever it has — nothing
        // left to clear.
        for (CodeEditor* editor : liveViews(*bound, uri)) {
            editor->setBreakpointLines({});
            QVector<CodeEditor::Diagnostic> kept;
            const QVector<CodeEditor::Diagnostic> current
                = editor->diagnostics();
            for (const CodeEditor::Diagnostic& diag : current)
                if (diag.source != QLatin1String("dap"))
                    kept.append(diag);
            editor->setDiagnostics(kept);
        }
    }
    sourceRefPaths_.clear();
}

void DapHub::onAdapterGone(const QString& reason)
{
    ++stopGen_;
    ++sessionGen_; // every in-flight reply chain dies here
    clearAllFeeds();
    lastThreadId_ = 0;
    emit sessionEnded(reason);
}

} // namespace qmn::glue
