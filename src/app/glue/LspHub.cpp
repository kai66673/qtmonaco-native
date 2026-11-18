#include "app/glue/LspHub.hpp"

// M8 machine 4: the app-layer glue in one object. Everything that connects
// the view's reality (CodeDocument::editApplied, CodeEditor::setDiagnostics)
// to the lsp layers (LspClient + DocumentSyncer) lives here — never the
// other way around.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUrl>

#include "lsp/DocumentSyncer.hpp"
#include "lsp/PositionConverter.hpp"
#include "protocol/ProcessTransport.hpp"
#include "view/CodeDocument.hpp"
#include "view/CodeEditor.hpp"

namespace qmn::app {

namespace {
constexpr int kLspSeverityError = 1; // LSP DiagnosticSeverity (wire)

// PLAN's config merge rule: OBJECTS RECURSE, arrays (and scalars) REPLACE
// whole. Used both for the `.qmn/lsp.json` initializeTemplate patch and for
// the spec template riding on top of the built initialize params.
QJsonObject deepMerge(const QJsonObject& base, const QJsonObject& patch)
{
    QJsonObject out = base;
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
        const QJsonValue incoming = it.value();
        if (incoming.isObject()
            && out.value(it.key()).isObject()) { // recurse only object+object
            out.insert(it.key(),
                       deepMerge(out.value(it.key()).toObject(),
                                 incoming.toObject()));
        } else {
            out.insert(it.key(), incoming);
        }
    }
    return out;
}
} // namespace

LspHub::LspHub(QObject* parent)
    : QObject(parent)
{
    factory_ = [](const ServerSpec& spec, QObject* owner) {
        return static_cast<qmn::protocol::Transport*>(
            new qmn::protocol::ProcessTransport(spec.program, spec.args, owner));
    };
    buildBuiltins();
}

LspHub::~LspHub()
{
    // Teardown order is the honest LSP order: every document says didClose
    // (notifications queued BEFORE the shutdown request, and RpcClient's
    // write queue is ordered), then the two-phase stop of each client.
    closingDown_ = true;
    const QStringList uris = registrations_.keys();
    for (const QString& uri : uris)
        releaseRegistration(uri);
    const QStringList specs = clients_.keys();
    for (const QString& spec : specs)
        if (clients_[spec].client)
            clients_[spec].client->stop();
    waitForStops(); // the stop is a BARRIER, not a request: see below
}

// The stop barrier is asynchronous, and the clients die as this hub's
// children the moment the body returns: ~QProcess would then SIGKILL a
// server that never got to answer its exit request (measured: "QProcess:
// Destroyed while process is still running"). A bounded local pump hands
// the barrier the events it needs — the shutdown/exit replies, the QProcess
// finished() signal — instead of pretending the wait is free.
void LspHub::waitForStops()
{
    QEventLoop pump;
    QTimer wake; // heartbeat: 25 ms slices, no busy spin
    wake.setInterval(25);
    connect(&wake, &QTimer::timeout, &pump, &QEventLoop::quit);
    wake.start();

    const QStringList specs = clients_.keys(); // SNAPSHOT: pump.exec() runs
                                              // arbitrary queued code, which
                                              // may edit clients_
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kStopBarrierMs) {
        bool pending = false;
        for (const QString& name : specs) {
            const auto it = clients_.constFind(name);
            if (it == clients_.constEnd() || !it->client)
                continue; // the entry is gone: nothing left to wait for
            const qmn::lsp::LspClient::State state = it->client->state();
            if (state != qmn::lsp::LspClient::State::Stopped
                && state != qmn::lsp::LspClient::State::Failed)
                pending = true;
        }
        if (!pending)
            return;
        pump.exec();
    }
    // Out of grace: LspClient's own teardown (endSession) closes the pipes
    // and the transport's terminate/kill ladder finishes the job.
}

void LspHub::setTransportFactory(TransportFactory factory)
{
    factory_ = std::move(factory);
}

// ---------------------------------------------------------------- built-ins
// The matrix deliberately ships ONE built-in: clangd, the plan's reference
// server (the only one with a clean single-binary stdio story, and the one
// the close-car probe drills). Other servers arrive through `.qmn/lsp.json`
// — a built-in we cannot verify is a broken default, not a feature.
void LspHub::buildBuiltins()
{
    ServerSpec clangd;
    clangd.name = QStringLiteral("clangd");
    clangd.program = QStringLiteral("clangd");
    clangd.languages = {QStringLiteral("c"), QStringLiteral("cpp")};
    clangd.initializeTemplate.insert(
        QStringLiteral("initializationOptions"),
        QJsonObject{{QStringLiteral("clangdFileStatus"), true}});
    setServerSpec(clangd);
}

// ------------------------------------------------------------------ config
void LspHub::setWorkspaceRoot(const QString& absolutePath)
{
    workspaceRoot_ = absolutePath;
}

bool LspHub::loadConfig()
{
    if (workspaceRoot_.isEmpty())
        return false;
    QFile file(QDir::cleanPath( // the PLAN path, verbatim
        workspaceRoot_ + QStringLiteral("/.qmn/lsp.json")));
    if (!file.open(QIODevice::ReadOnly))
        return false; // no config: silent, the built-ins stand
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false; // a malformed config is LOUD downstream, not a partial
                      // merge: we refuse the whole file
    return loadConfigFromJson(doc.object());
}

bool LspHub::loadConfigFromJson(const QJsonObject& root)
{
    const QJsonObject servers = root.value(QStringLiteral("servers")).toObject();
    if (servers.isEmpty())
        return false;
    for (auto it = servers.begin(); it != servers.end(); ++it)
        mergeSpecObject(it.key(), it.value().toObject());
    return true;
}

void LspHub::mergeSpecObject(const QString& name, const QJsonObject& patch)
{
    const QString key = name.toLower();
    ServerSpec& spec = specs_[key]; // default-constructs a new entry
    if (spec.name.isEmpty())
        spec.name = name;
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const QString field = it.key();
        const QJsonValue value = it.value();
        if (field == QLatin1String("program") && value.isString()) {
            spec.program = value.toString();
        } else if (field == QLatin1String("args") && value.isArray()) {
            QStringList args; // ARRAYS REPLACE whole (PLAN)
            for (const QJsonValue& a : value.toArray())
                args << a.toString();
            spec.args = args;
        } else if (field == QLatin1String("languages") && value.isArray()) {
            QStringList languages; // ditto: a replaced list, not an append
            for (const QJsonValue& l : value.toArray())
                languages << l.toString().toLower();
            spec.languages = languages;
        } else if (field == QLatin1String("initializeTemplate")
                   && value.isObject()) {
            // OBJECTS RECURSE (PLAN): a config file can add one key to a
            // built-in's initializationOptions without restating the rest.
            spec.initializeTemplate
                = deepMerge(spec.initializeTemplate, value.toObject());
        }
    }
    rebuildLanguageMap();
}

void LspHub::setServerSpec(const ServerSpec& spec)
{
    ServerSpec copy = spec;
    if (copy.name.isEmpty())
        copy.name = copy.program;
    specs_[copy.name.toLower()] = copy;
    rebuildLanguageMap();
}

QJsonObject LspHub::serverSpec(const QString& name) const
{
    auto it = clients_.constFind(name.toLower());
    const ServerSpec& spec
        = it != clients_.constEnd() ? it->spec : specs_.value(name.toLower());
    QJsonObject out;
    out.insert(QStringLiteral("name"), spec.name);
    out.insert(QStringLiteral("program"), spec.program);
    out.insert(QStringLiteral("args"), QJsonArray::fromStringList(spec.args));
    out.insert(QStringLiteral("languages"),
               QJsonArray::fromStringList(spec.languages));
    out.insert(QStringLiteral("initializeTemplate"), spec.initializeTemplate);
    return out;
}

QString LspHub::specNameForLanguage(const QString& languageId) const
{
    return languageToSpec_.value(languageId.toLower());
}

void LspHub::rebuildLanguageMap()
{
    languageToSpec_.clear();
    for (auto it = specs_.constBegin(); it != specs_.constEnd(); ++it)
        for (const QString& language : it->languages)
            languageToSpec_.insert(language.toLower(), it.key());
}

QString LspHub::expand(const QString& text) const
{
    if (workspaceRoot_.isEmpty())
        return text;
    QString out = text; // replace() mutates: work on the copy
    out.replace(QStringLiteral("${WORKSPACE_ROOT}"), workspaceRoot_);
    return out;
}

QJsonObject LspHub::buildInitializeParams(const ServerSpec& spec) const
{
    // The shape every real client must carry (the fakeserver's exit-2 pin
    // says "capabilities" is mandatory; the harness's initParams is the
    // same skeleton the matrix template rides on top of).
    QJsonObject params;
    params.insert(QStringLiteral("processId"),
                  double(QCoreApplication::applicationPid()));
    params.insert(QStringLiteral("clientInfo"),
                  QJsonObject{{QStringLiteral("name"),
                               QStringLiteral("qtmonaco-native")},
                              {QStringLiteral("version"),
                               QStringLiteral("1.7.0")}});
    params.insert(QStringLiteral("locale"), QStringLiteral("en"));
    const QUrl rootUrl
        = workspaceRoot_.isEmpty() ? QUrl() : QUrl::fromLocalFile(workspaceRoot_);
    params.insert(QStringLiteral("rootUri"),
                  rootUrl.isEmpty() ? QJsonValue(QJsonValue::Null)
                                    : QJsonValue(rootUrl.toString()));
    if (rootUrl.isEmpty()) {
        params.insert(QStringLiteral("workspaceFolders"),
                      QJsonValue(QJsonValue::Null)); // spec: no folders
    } else {
        params.insert(
            QStringLiteral("workspaceFolders"),
            QJsonArray{QJsonObject{
                {QStringLiteral("uri"), rootUrl.toString()},
                {QStringLiteral("name"),
                 QFileInfo(workspaceRoot_).fileName()}}});
    }
    // Static v1 client capabilities (PLAN: statics are machine 5's feature
    // gates; the registry is separate). The SYNC piece is what machine 4
    // actually exercises: incremental changes + didSave, no willSave.
    params.insert(
        QStringLiteral("capabilities"),
        QJsonObject{
            {QStringLiteral("workspace"),
             QJsonObject{{QStringLiteral("applyEdit"), true},
                         {QStringLiteral("workspaceFolders"), true}}},
            {QStringLiteral("textDocument"),
             QJsonObject{{QStringLiteral("synchronization"),
                          QJsonObject{{QStringLiteral("dynamicRegistration"),
                                       false},
                                      {QStringLiteral("willSave"), false},
                                      {QStringLiteral("willSaveWaitUntil"),
                                       false},
                                      {QStringLiteral("didSave"), true}}}}},
            {QStringLiteral("window"),
             QJsonObject{{QStringLiteral("workDoneProgress"), false}}}});
    // The template deep-merges ON TOP (its ${WORKSPACE_ROOT} expands first,
    // then objects recurse / arrays replace — the one merge rule, applied
    // twice: config-over-built-in and template-over-built-params).
    const QJsonObject expanded
        = expandTemplateValue(spec.initializeTemplate).toObject();
    return deepMerge(params, expanded);
}

QJsonValue LspHub::expandTemplateValue(const QJsonValue& value) const
{
    if (value.isString())
        return expand(value.toString());
    if (value.isArray()) {
        QJsonArray out;
        for (const QJsonValue& item : value.toArray())
            out.append(expandTemplateValue(item));
        return out;
    }
    if (value.isObject()) {
        QJsonObject out;
        const QJsonObject in = value.toObject();
        for (auto it = in.begin(); it != in.end(); ++it)
            out.insert(it.key(), expandTemplateValue(it.value()));
        return out;
    }
    return value;
}

// -------------------------------------------------------------- lifecycle
void LspHub::registerClient(const QString& specName, SpecClient& entry)
{
    qmn::lsp::LspClient* client = entry.client;
    connect(client, &qmn::lsp::LspClient::serverShowMessage, this,
            [this, specName](int type, const QString& message) {
                emit serverShowMessage(specName, type, message);
            });
    connect(client, &qmn::lsp::LspClient::serverLogMessage, this,
            [this, specName](int type, const QString& message) {
                emit serverLogMessage(specName, type, message);
            });
    connect(client, &qmn::lsp::LspClient::serverStderr, this,
            [this, specName](const QString& line) {
                emit serverStderr(specName, line);
            });
    connect(client, &qmn::lsp::LspClient::failed, this,
            [this, specName](const QString& reason) {
                emit serverFailed(specName, reason);
            });
    connect(client,
            &qmn::lsp::LspClient::diagnosticsReceived,
            this, [this, specName](const QJsonObject& params) {
                onDiagnostics(specName, params);
            });
}

LspHub::SpecClient* LspHub::specEntry(const QString& specName)
{
    if (closingDown_)
        return nullptr; // teardown in flight: no new sessions, no new spawns
                        // (the pump in waitForStops runs queued code that may
                        // still knock on these doors)
    const QString key = specName.toLower();
    auto it = clients_.find(key);
    if (it != clients_.end())
        return &it.value(); // live or failed: FAILED stays FAILED until the
                            // app calls restartServer (PLAN's lifecycle; a
                            // newly opened document does NOT silently retry)
    auto specIt = specs_.constFind(key);
    if (specIt == specs_.constEnd())
        return nullptr; // no such spec: the honest null, no spawn
    SpecClient& entry = clients_[key];
    entry.spec = specIt.value();
    entry.spec.program = expand(entry.spec.program);
    QStringList args;
    for (const QString& arg : entry.spec.args)
        args << expand(arg);
    entry.spec.args = args;
    entry.client = new qmn::lsp::LspClient(
        [this, spec = entry.spec](QObject* owner) {
            return factory_(spec, owner);
        },
        this);
    registerClient(key, entry);
    entry.client->start(buildInitializeParams(entry.spec)); // LAZY: the first
                                                           // openDocument of
                                                           // this language IS
                                                           // the start signal
    return &entry;
}

qmn::lsp::LspClient* LspHub::clientForSpec(const QString& specName) const
{
    auto it = clients_.constFind(specName.toLower());
    return it == clients_.constEnd() ? nullptr : it->client;
}

void LspHub::restartServer(const QString& specName)
{
    auto it = clients_.find(specName.toLower());
    if (it == clients_.end() || !it->client)
        return;
    it->client->restartServer(); // resets the budget; ready() re-fires and
                                 // every open document re-didOpens whole
}

// --------------------------------------------------------------- documents
QString LspHub::resolveUri(qmn::view::CodeEditor* editor, const QString& uri)
{
    if (!uri.isEmpty())
        return uri;
    qmn::view::CodeDocument* doc = editor->document();
    if (!doc->documentUri().isEmpty())
        return doc->documentUri();
    const QString minted = QStringLiteral("untitled:qmn-%1")
                               .arg(++nextUntitled_); // a key is needed: the
                                                      // uri is the document's
                                                      // LSP identity
    doc->setDocumentUri(minted);
    return minted;
}

bool LspHub::openDocument(qmn::view::CodeEditor* editor, const QString& uri,
                          const QString& languageId,
                          const QString& serverName)
{
    if (!editor || !editor->document())
        return false;
    const QString specName
        = !serverName.isEmpty() ? serverName : specNameForLanguage(languageId);
    if (specName.isEmpty())
        return false; // no server claims this language: nothing to sync to
    SpecClient* entry = specEntry(specName);
    if (!entry)
        return false;

    const QString key = resolveUri(editor, uri);
    if (registrations_.contains(key))
        releaseRegistration(key); // re-open = didClose then didOpen (the
                                  // server pins that order)

    Registration reg;
    reg.editor = editor;
    reg.specName = entry->spec.name;
    reg.syncer = new qmn::lsp::DocumentSyncer(entry->client, this);
    reg.syncer->setDebounceMs(debounceMs_);
    reg.syncer->setUri(key);
    registrations_.insert(key, reg);

    // THE live feed (machine 3 tested this wiring with synthetic deltas;
    // this is the same signal from the real document): every primitive
    // mutation, PRE-OP offsets, undo/redo included. The context is the
    // syncer itself: the captured raw pointer can never fire after it dies
    // (a captured Registration would dangle across releaseRegistration).
    qmn::view::CodeDocument* doc = editor->document();
    connect(doc, &qmn::view::CodeDocument::editApplied, reg.syncer,
            [syncer = reg.syncer](int start, int end, const QString& inserted) {
                syncer->applyDelta(start, end, inserted);
            });
    // The editor dying releases the wiring (the document dies with it).
    connect(editor, &QObject::destroyed, this, [this, key] {
        releaseRegistration(key);
    });

    reg.syncer->open(doc->toPlainText(), languageId); // didOpen now, or on
                                                      // ready() if the server
                                                      // is still coming up
    return true;
}

void LspHub::closeDocument(qmn::view::CodeEditor* editor)
{
    for (auto it = registrations_.begin(); it != registrations_.end(); ++it) {
        if (it->editor == editor) {
            releaseRegistration(it.key());
            return;
        }
    }
}

void LspHub::releaseRegistration(const QString& uri)
{
    auto it = registrations_.find(uri);
    if (it == registrations_.end())
        return;
    Registration reg = it.value();
    registrations_.erase(it);
    if (reg.syncer) {
        reg.syncer->close(); // didClose (a stopped server drops it silently,
                             // the M2 notify rule)
        reg.syncer->deleteLater();
    }
}

void LspHub::saveDocument(qmn::view::CodeEditor* editor)
{
    qmn::lsp::DocumentSyncer* syncer = syncerFor(editor);
    if (syncer)
        syncer->save(); // flush-then-didSave (machine 3's door)
    if (editor && editor->document())
        editor->document()->markSaved(); // the modified flag is the document's
}

qmn::lsp::DocumentSyncer* LspHub::syncerFor(
    qmn::view::CodeEditor* editor) const
{
    for (auto it = registrations_.constBegin(); it != registrations_.constEnd();
         ++it)
        if (it->editor == editor)
            return it->syncer;
    return nullptr;
}

qmn::lsp::LspClient* LspHub::clientForUri(const QString& uri) const
{
    auto reg = registrations_.constFind(uri);
    if (reg == registrations_.constEnd())
        return nullptr;
    auto client = clients_.constFind(reg->specName.toLower());
    return client == clients_.constEnd() ? nullptr : client->client;
}

void LspHub::setDebounceMs(int ms)
{
    debounceMs_ = ms;
    for (auto it = registrations_.begin(); it != registrations_.end(); ++it)
        if (it->syncer)
            it->syncer->setDebounceMs(ms); // a live knob, like every M19
                                           // options door
}

// ------------------------------------------------------------- diagnostics
void LspHub::onDiagnostics(const QString& specName, const QJsonObject& params)
{
    if (closingDown_)
        return; // the teardown pump still drains the socket: a late publish
                // has nothing to paint into (registrations_ are already gone)
    const QString uri = params.value(QStringLiteral("uri")).toString();
    auto reg = registrations_.find(uri);
    if (reg == registrations_.end())
        return; // not our document (a window/showMessageRequest-style
                // broadcast): nothing to paint
    // The cross-wire guard: a publish from a server that is NOT the one this
    // document's spec resolved to is a wire lie — drop it, do not paint it.
    auto entry = clients_.constFind(specName.toLower());
    if (entry == clients_.constEnd() || clientForUri(uri) != entry->client)
        return;
    const qmn::lsp::LspClient* client = clientForUri(uri);
    const qmn::lsp::PositionConverter::Encoding encoding
        = qmn::lsp::PositionConverter::fromWire(
            client ? client->positionEncoding() : QString());
    const QString text = reg->syncer->mirrorText(); // the clamping frame
    QVector<qmn::view::CodeEditor::Diagnostic> decoded;
    const QJsonArray incoming
        = params.value(QStringLiteral("diagnostics")).toArray();
    decoded.reserve(incoming.size());
    for (const QJsonValue& value : incoming) {
        const QJsonObject diag = value.toObject();
        const QJsonObject range
            = diag.value(QStringLiteral("range")).toObject();
        const QJsonObject start
            = range.value(QStringLiteral("start")).toObject();
        const QJsonObject end = range.value(QStringLiteral("end")).toObject();
        const qmn::lsp::ModelPosition from
            = qmn::lsp::PositionConverter::toModelPosition(
                text, start.value(QStringLiteral("line")).toInt(),
                start.value(QStringLiteral("character")).toInt(), encoding);
        const qmn::lsp::ModelPosition to
            = qmn::lsp::PositionConverter::toModelPosition(
                text, end.value(QStringLiteral("line")).toInt(),
                end.value(QStringLiteral("character")).toInt(), encoding);
        qmn::view::CodeEditor::Diagnostic out;
        out.startLine = from.line;
        out.startCol = from.col;
        out.endLine = to.line;
        out.endCol = to.col;
        if (out.endLine < out.startLine
            || (out.endLine == out.startLine && out.endCol < out.startCol))
            std::swap(out.startLine, out.endLine); // a liar's inverted range:
                                                   // read it the tolerant way
        if (out.endLine == out.startLine && out.endCol < out.startCol)
            std::swap(out.startCol, out.endCol);
        const int wireSeverity
            = diag.value(QStringLiteral("severity")).toInt(kLspSeverityError);
        switch (wireSeverity) {
        case 2:
            out.severity = qmn::view::CodeEditor::Severity::Warning;
            break;
        case 3:
            out.severity = qmn::view::CodeEditor::Severity::Information;
            break;
        case 4:
            out.severity = qmn::view::CodeEditor::Severity::Hint;
            break;
        default: // 1 and anything out of range: Error (the safe read)
            out.severity = qmn::view::CodeEditor::Severity::Error;
            break;
        }
        out.message = diag.value(QStringLiteral("message")).toString();
        decoded.append(out);
    }
    reg->editor->setDiagnostics(decoded); // M5's decoration door, no new
                                          // geometry
}

} // namespace qmn::app
