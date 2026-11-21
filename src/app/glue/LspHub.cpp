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

#include <algorithm>

#include "lsp/DocumentSyncer.hpp"
#include "lsp/PositionConverter.hpp"
#include "lsp/WorkspaceEdit.hpp"
#include "protocol/ProcessTransport.hpp"
#include "view/CodeDocument.hpp"
#include "view/CodeEditor.hpp"
#include "view/CodeEditorOptions.hpp" // the readOnly door (M19): options-only

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

// The popup-trigger state (the M8 close-train sliver): defined early because
// releaseRegistration already tears it down long before the trigger member
// functions further down.
struct LspHub::TriggerState {
    QTimer completionTimer; // single-shot -> fireCompletionTrigger
    QTimer hoverTimer;      // single-shot -> fireHoverTrigger
    bool on = false;
    QChar typedChar;                // the one unit to classify at fire time
    bool pendingTypingMove = false; // an editApplied just fired: the next
                                    // cursorMoved is THAT edit's own move
                                    // (unmatched: one skipped hover cycle —
                                    // an edit that never moves the caret,
                                    // harmless and documented)
    qint64 pendingCompletion = 0;   // 0 = nothing in flight (local refusals
                                    // return 0 from the door, -1 from the
                                    // client — neither is a cancelable id)
    qmn::view::TextPosition lastMouseHoverPos{-1, -1}; // post-1.8.0 sliver:
                                    // the last mouse-dwell point that got a
                                    // hover ask (a re-settle INSIDE the same
                                    // visible hover = no second wire ask);
                                    // the caret path and (re)arming clear it
};

namespace {

// The ONE identifier predicate the trigger shares: the completion
// classifier (an identifier unit completes Invoked) and the fallback hover
// word bounds walk with it.
bool isTriggerUnit(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

// LSP's hover contents shapes — string | MarkupContent | MarkedString |
// (string | MarkedString)[] — collapse into the popup's ONE plain string
// (PLAN: no markdown rendering; markup VALUES pass VERBATIM, the view paints
// plain text). Arrays join with newlines, exactly like an editor shows them.
QString hoverPlainText(const QJsonValue& contents)
{
    if (contents.isString())
        return contents.toString();
    if (contents.isArray()) {
        QStringList parts;
        for (const QJsonValue& value : contents.toArray()) {
            if (value.isString())
                parts << value.toString();
            else if (value.isObject())
                parts << value.toObject().value(QStringLiteral("value")).toString();
        }
        return parts.join(QLatin1Char('\n'));
    }
    if (contents.isObject())
        return contents.toObject().value(QStringLiteral("value")).toString();
    return QString();
}

// The ACCEPT payload of one completion row (post-1.8.0 sliver): the wire's
// `textEdit` in either legal shape — the plain {range, newText} or the
// insert/replace alternative range {insert, replace, newText} that clangd
// prefers. The INSERT range wins: accept is a type-and-accept flow, and the
// insert range is the one anchored at the request position. With no usable
// textEdit the server's `insertText` (else the `label`) becomes a ZERO-WIDTH
// insertion at the caret captured at trigger FIRE time — the client
// advertises insertTextFormat PLAIN TEXT in initialize, so no snippet
// markers are expected from a conforming server. All-zero output = "no
// payload" (onTriggerAccept refuses such accepts silently).
qmn::view::CodeEditor::TextEdit completionTextEdit(const QJsonObject& item,
                                                   qmn::view::TextPosition caretAtFire)
{
    qmn::view::CodeEditor::TextEdit ed;
    const QJsonValue teVal = item.value(QStringLiteral("textEdit"));
    if (teVal.isObject()) {
        const QJsonObject te = teVal.toObject();
        QJsonObject range = te.value(QStringLiteral("range")).toObject();
        if (!te.contains(QStringLiteral("range"))) {
            if (te.contains(QStringLiteral("insert")))
                range = te.value(QStringLiteral("insert")).toObject();
            else if (te.contains(QStringLiteral("replace")))
                range = te.value(QStringLiteral("replace")).toObject();
        }
        const QJsonObject s = range.value(QStringLiteral("start")).toObject();
        const QJsonObject e = range.value(QStringLiteral("end")).toObject();
        if (s.value(QStringLiteral("line")).isDouble()
            && e.value(QStringLiteral("line")).isDouble()) {
            ed.startLine = s.value(QStringLiteral("line")).toInt();
            ed.startCol = s.value(QStringLiteral("character")).toInt();
            ed.endLine = e.value(QStringLiteral("line")).toInt();
            ed.endCol = e.value(QStringLiteral("character")).toInt();
            ed.newText = te.value(QStringLiteral("newText")).toString();
            return ed;
        }
    }
    QString text = item.value(QStringLiteral("insertText")).toString();
    if (text.isEmpty())
        text = item.value(QStringLiteral("label")).toString();
    if (!text.isEmpty()) {
        ed.startLine = ed.endLine = caretAtFire.line;
        ed.startCol = ed.endCol = caretAtFire.col;
        ed.newText = text;
    }
    return ed;
}

// The fallback hover frame: the WORD at the caret (no word = a zero-width
// caret range). Clamped into the mirror text — the frame the server answered
// against.
qmn::view::CodeEditor::HoverRange wordRangeAt(const QString& mirrorText,
                                              qmn::view::TextPosition caret)
{
    const QStringList lines = mirrorText.split(QLatin1Char('\n'));
    if (caret.line >= lines.size())
        caret.line = lines.isEmpty() ? 0 : lines.size() - 1;
    const QString line = lines.value(caret.line);
    if (caret.col > line.size())
        caret.col = line.size();
    int from = caret.col;
    while (from > 0 && isTriggerUnit(line.at(from - 1)))
        --from;
    int to = caret.col;
    while (to < line.size() && isTriggerUnit(line.at(to)))
        ++to;
    return {caret.line, from, caret.line, to};
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
                               // Configured by CMake (QMN_PACKAGE_VERSION =
                               // the project() VERSION), NOT typed here: the
                               // 1.8.0 close caught a hand-kept literal riding
                               // 1.7.0 while the project line still said 1.6.1.
                               QStringLiteral(QMN_PACKAGE_VERSION)}});
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
                                      {QStringLiteral("didSave"), true}}},
                         // PLAIN-TEXT completions only (post-1.8.0 accept
                         // sliver): the popups have no snippet engine, so
                         // the client advertises insertTextFormat 1 and NO
                         // snippet support — a conforming server (clangd
                         // included) then sends textEdit/insertText as plain
                         // text, and the accept path applies them verbatim.
                         {QStringLiteral("completion"),
                          QJsonObject{{QStringLiteral("completionItem"),
                                       QJsonObject{
                                           {QStringLiteral("snippetSupport"),
                                            false},
                                           {QStringLiteral("insertTextFormat"),
                                            QJsonObject{
                                                {QStringLiteral("valueSupport"),
                                                 true},
                                                {QStringLiteral("value"),
                                                 1}}}}}}}}},
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
    // The server-primed write path (machine 5): the hub applies what it can
    // (all-or-nothing), answers the server with the truth, and relays the
    // refusal reasons.
    connect(client, &qmn::lsp::LspClient::serverApplyEdit, this,
            [this, specName](const QJsonValue& id, const QJsonValue& edit) {
                onApplyEdit(specName, id, edit);
            });
    // window/showMessageRequest: the hub only RELAYS — it never picks an
    // action. The relay lands before LspClient's Null default, so an app
    // that answers clientForSpec(spec)->respondShowMessageRequest from
    // inside this signal wins the choice (the first answer to an id is the
    // one that travels).
    connect(client, &qmn::lsp::LspClient::serverShowMessageRequest, this,
            [this, specName](const QJsonValue& id, const QJsonValue& params) {
                emit serverShowMessageRequest(specName, id, params);
            });
    // M8 machine 7 (the stored-configuration feed): a document opened before
    // this session was READY kept its buffer on the VIEW — this is the edge
    // that finally delivers it (qtmonaco's "buffered until init", living in
    // the hub, not the widget). ready() re-fires on every (re-)READY, so a
    // restarted server is re-told the settings too.
    connect(client, &qmn::lsp::LspClient::ready, this, [this, specName] {
        for (auto it = registrations_.constBegin();
             it != registrations_.constEnd(); ++it) {
            if (it->specName == specName)
                pushWorkspaceConfiguration(it.key());
        }
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
    // M8 machine 7: the live re-push of the view's stored workspace
    // configuration (qtmonaco's `update_workspace_configuration`). Context is
    // the SYNCER: it dies with this registration, so a re-open (release +
    // re-register the same editor) can never double-send on one change.
    connect(editor, &qmn::view::CodeEditor::workspaceConfigurationChanged,
            reg.syncer, [this, key] { pushWorkspaceConfiguration(key); });
    // The popup-trigger taps (M8 close-train): connected for EVERY
    // registration, cheap, and inert until setPopupTriggers says otherwise —
    // the slots re-look-up the per-key state and honor its `on` bit. The
    // context is again the syncer: a re-open (release + re-register the same
    // editor) can never double-fire on one signal.
    connect(doc, &qmn::view::CodeDocument::editApplied, reg.syncer,
            [this, key](int start, int end, const QString& inserted) {
                onTriggerEdit(key, start, end, inserted);
            });
    connect(editor, &qmn::view::CodeEditor::cursorMoved, reg.syncer,
            [this, key](qmn::view::TextPosition pos) {
                onTriggerCaretMoved(key, pos);
            });
    connect(editor, &qmn::view::CodeEditor::completionAccepted, reg.syncer,
            [this, key](int /*index*/,
                        const qmn::view::CodeEditor::CompletionItem& item) {
                onTriggerAccept(key, item); // the ACCEPT route (post-1.8.0
                                            // sliver): the hub does the insert
            });
    connect(editor, &qmn::view::CodeEditor::mouseHovered, reg.syncer,
            [this, key](qmn::view::TextPosition pos) {
                onTriggerMouseHover(key, pos); // the MOUSE-hover route
                                               // (post-1.8.0 sliver): the
                                               // view dwelled, the hub asks
            });

    reg.syncer->open(doc->toPlainText(), languageId); // didOpen now, or on
                                                      // ready() if the server
                                                      // is still coming up
    // A buffer set BEFORE this document was registered: the session may
    // already be READY (another document of this language opened first), so
    // the open is a push edge too. An empty buffer makes it a no-op.
    pushWorkspaceConfiguration(key);
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
    qmn::lsp::LspClient* client = clientForUri(uri); // BEFORE the erase
    Registration reg = it.value();
    registrations_.erase(it);
    // The trigger state dies with the registration: timers off, and an
    // in-flight completion gets canceled so its handler (which re-looks-up
    // this registration and finds it gone) can never show into a closed
    // document. MOVE the state out before erasing, and erase BEFORE the
    // cancel: cancel() fires the stale handler synchronously, and that
    // handler must already find no state/registration to touch.
    std::unique_ptr<TriggerState> trig;
    auto tit = triggers_.find(uri);
    if (tit != triggers_.end()) {
        trig = std::move(tit->second);
        triggers_.erase(tit);
    }
    if (trig && trig->pendingCompletion != 0 && client)
        client->cancel(trig->pendingCompletion);
    if (reg.syncer) {
        reg.syncer->close(); // didClose (a stopped server drops it silently,
                             // the M2 notify rule)
        reg.syncer->deleteLater();
    }
}

// M8 machine 7: the stored workspace configuration — the VIEW's buffer
// (setStoredWorkspaceConfiguration) pushed to the wire. The rules are the
// honest ones:
//   - no registration / no live client / not READY: NOTHING goes out. The
//     buffer waits on the view and this door is re-knocked by ready() (which
//     re-fires on every (re-)READY) and by the view's own change signal;
//   - an EMPTY buffer means "never configured" = the spec default, so the
//     hub sends NOTHING: an empty settings object would tell the server "no
//     settings", which is not the same claim as "I have no opinion";
//   - a buffer that is not a JSON OBJECT is refused locally: there is no
//     legal params shape for it (the Python shim's json.dumps always
//     produces one; a hand-fed non-object must not reach the server).
void LspHub::pushWorkspaceConfiguration(const QString& uri)
{
    if (closingDown_)
        return; // teardown in flight: no new wire traffic
    const auto it = registrations_.constFind(uri);
    if (it == registrations_.constEnd() || !it->editor)
        return; // released between the signal and the delivery
    qmn::lsp::LspClient* client = clientForUri(uri);
    if (!client || !client->isReady())
        return;
    const QString json = it->editor->storedWorkspaceConfiguration();
    if (json.trimmed().isEmpty())
        return;
    QJsonParseError err{};
    const QJsonDocument parsed = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !parsed.isObject())
        return;
    QJsonObject params;
    params.insert(QStringLiteral("settings"), parsed.object());
    client->notify(QStringLiteral("workspace/didChangeConfiguration"), params);
}

namespace {
// The willSaveWaitUntil half of the textDocumentSync capability (M8 machine
// 6): an integer change form or an absent key never advertises it; only the
// TextDocumentSyncOptions OBJECT with a true `willSaveWaitUntil` does.
bool willSaveWaitUntilOffered(const qmn::lsp::LspClient* client)
{
    const QJsonValue sync = client->serverCapabilities().value(
        QStringLiteral("textDocumentSync"));
    if (!sync.isObject())
        return false;
    return sync.toObject().value(QStringLiteral("willSaveWaitUntil")).toBool();
}
} // namespace

void LspHub::saveDocument(qmn::view::CodeEditor* editor)
{
    qmn::lsp::DocumentSyncer* syncer = syncerFor(editor);
    qmn::lsp::LspClient* client =
        (syncer && !closingDown_) ? clientForUri(syncer->uri()) : nullptr;
    if (!syncer || !client || !client->isReady()
        || !willSaveWaitUntilOffered(client)) {
        if (syncer)
            syncer->save(); // flush-then-didSave (machine 3's door)
        if (editor && editor->document())
            editor->document()->markSaved(); // the modified flag is the
                                             // document's
        return;
    }
    // The advertised round trip (semantics: the door comment in the header).
    // The flush FIRST is the same act that makes feature frames honest: the
    // request's TextDocumentIdentifier rides a mirror that already holds
    // every keystroke. reason 1 = TextDocumentSaveReason.Manual.
    const QString uri = syncer->uri();
    client->request(
        QStringLiteral("textDocument/willSaveWaitUntil"),
        QJsonObject{{QStringLiteral("textDocument"),
                     QJsonObject{{QStringLiteral("uri"), uri}}},
                    {QStringLiteral("reason"), 1}},
        [this, uri](const qmn::protocol::RpcClient::Reply& reply) {
            // RE-RESOLVE by uri: the editor may have died, the document may
            // have closed, the client may have restarted since the request
            // (the hub dtor releases every registration BEFORE stopping the
            // clients, so a late reply finds an empty table and stops).
            auto it = registrations_.constFind(uri);
            qmn::view::CodeEditor* ed =
                it == registrations_.constEnd() ? nullptr : it->editor;
            qmn::lsp::DocumentSyncer* nowSyncer = syncerFor(ed);
            if (!ed || !nowSyncer)
                return; // the document is gone: nothing left to save
            if (!reply.isError && !reply.result.isNull()) {
                // The reply is a bare TextEdit[] in the machine-3 sync mode
                // (formatting shape) — but a server may also answer a full
                // WorkspaceEdit. The applier stays ALL-OR-NOTHING: a refused
                // batch changes nothing, and the save proceeds regardless
                // (PLAN: the round trip is a courtesy, not a veto).
                const ApplyReport report =
                    reply.result.isArray()
                        ? applyTextEditList(ed, reply.result,
                                            clientForUri(uri))
                        : applyReplyEdit(reply.result, clientForUri(uri));
                Q_UNUSED(report); // v1 does not surface wswu refusals: the
                                  // didSave marker is the observable trail
            }
            nowSyncer->save(); // any applied edit's endServerEdit full sync
                               // is already queued, so the didSave rides the
                               // EDITED model — the ordering PLAN pins
            if (ed->document())
                ed->document()->markSaved();
        });
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
        out.source = diag.value(QStringLiteral("source")).toString();
        decoded.append(out);
    }
    reg->editor->setDiagnostics(decoded); // M5's decoration door, no new
                                          // geometry
}

// ------------------------------------------------- feature doors (machine 5)
namespace {
// The LOCAL refusals speak the codes a SERVER would have used, so a feature
// handler cannot tell a hub gate from a server answer: -32002 = no live
// server serves this document, -32601 = the provider is not offered (the
// M2 rule for a non-Ready door: a refusal answers EXACTLY ONCE, never
// silence).
constexpr int kRefusalNoServer = qmn::lsp::LspClient::kWireServerNotInitialized;
constexpr int kRefusalNoProvider = -32601;

qmn::protocol::RpcClient::Reply localRefusal(int code, const QString& message)
{
    qmn::protocol::RpcClient::Reply reply;
    reply.isError = true;
    reply.code = code;
    reply.message = message;
    return reply;
}

// The v1 feature gate is the initialize CAPABILITIES (the registration
// registry M2 built is the pull-model half, M8b): the key counts as offered
// when it is PRESENT and not `false` — the object/options form counts (a
// server may answer {"workDoneProgress":true}; renameProvider is an object
// when prepareProvider rides along).
bool capabilityOffered(const qmn::lsp::LspClient* client, const QString& key)
{
    const QJsonValue value = client->serverCapabilities().value(key);
    if (value.isUndefined() || value.isNull())
        return false;
    if (value.isBool())
        return value.toBool();
    return true;
}
} // namespace

bool LspHub::featureGate(qmn::view::CodeEditor* editor,
                         const QString& capabilityKey,
                         const FeatureHandler& onReply, FeatureTarget* target)
{
    *target = FeatureTarget{};
    if (closingDown_ || !editor) {
        if (onReply)
            onReply(localRefusal(kRefusalNoServer,
                                 QStringLiteral("the hub is closing down")));
        return false;
    }
    qmn::lsp::DocumentSyncer* syncer = syncerFor(editor);
    qmn::lsp::LspClient* client =
        syncer ? clientForUri(syncer->uri()) : nullptr;
    if (!syncer || !syncer->isOpen() || !client || !client->isReady()) {
        if (onReply) {
            const QString what =
                syncer ? syncer->uri() : QStringLiteral("document not registered");
            onReply(localRefusal(kRefusalNoServer,
                                 QStringLiteral("no live server serves %1").arg(what)));
        }
        return false;
    }
    if (!capabilityKey.isEmpty() && !capabilityOffered(client, capabilityKey)) {
        if (onReply)
            onReply(localRefusal(kRefusalNoProvider,
                                 QStringLiteral("capability \"%1\" is not offered")
                                     .arg(capabilityKey)));
        return false;
    }
    syncer->flush(); // THE act that makes the frame honest: the wire Position
                     // is built against the mirror, the text the server holds
    target->syncer = syncer;
    target->client = client;
    return true;
}

// The one model-frame -> wire-frame hop the feature doors need (the caret's
// line/col through PositionConverter against the mirror). Roundabout on
// purpose: toPosition honors the NEGOTIATED encoding, and fromModelPosition
// is the model-frame entry into the document's offset frame.
QJsonObject LspHub::wirePosition(const qmn::lsp::DocumentSyncer* syncer,
                                 const qmn::view::TextPosition& pos) const
{
    const QString text = syncer->mirrorText();
    const int offset = qmn::lsp::PositionConverter::fromModelPosition(
        text, qmn::lsp::ModelPosition{pos.line, pos.col});
    const qmn::lsp::LspPosition wire = qmn::lsp::PositionConverter::toPosition(
        text, offset, syncer->encoding());
    return QJsonObject{{QStringLiteral("line"), wire.line},
                       {QStringLiteral("character"), wire.character}};
}

QJsonObject LspHub::wireDocumentPosition(
    const qmn::lsp::DocumentSyncer* syncer,
    const qmn::view::TextPosition& pos) const
{
    return QJsonObject{{QStringLiteral("textDocument"),
                        QJsonObject{{QStringLiteral("uri"), syncer->uri()}}},
                       {QStringLiteral("position"), wirePosition(syncer, pos)}};
}

qint64 LspHub::requestHover(qmn::view::CodeEditor* editor,
                            const qmn::view::TextPosition& position,
                            const FeatureHandler& onReply)
{
    FeatureTarget t;
    if (!featureGate(editor, QStringLiteral("hoverProvider"), onReply, &t))
        return 0;
    return t.client->request(QStringLiteral("textDocument/hover"),
                             wireDocumentPosition(t.syncer, position), onReply);
}

qint64 LspHub::requestDefinition(qmn::view::CodeEditor* editor,
                                 const qmn::view::TextPosition& position,
                                 const FeatureHandler& onReply)
{
    FeatureTarget t;
    if (!featureGate(editor, QStringLiteral("definitionProvider"), onReply, &t))
        return 0;
    return t.client->request(QStringLiteral("textDocument/definition"),
                             wireDocumentPosition(t.syncer, position), onReply);
}

qint64 LspHub::requestReferences(qmn::view::CodeEditor* editor,
                                 const qmn::view::TextPosition& position,
                                 bool includeDeclaration,
                                 const FeatureHandler& onReply)
{
    FeatureTarget t;
    if (!featureGate(editor, QStringLiteral("referencesProvider"), onReply, &t))
        return 0;
    QJsonObject params = wireDocumentPosition(t.syncer, position);
    params.insert(QStringLiteral("context"),
                  QJsonObject{{QStringLiteral("includeDeclaration"),
                               includeDeclaration}});
    return t.client->request(QStringLiteral("textDocument/references"), params,
                             onReply);
}

qint64 LspHub::requestCompletion(qmn::view::CodeEditor* editor,
                                 const qmn::view::TextPosition& position,
                                 const FeatureHandler& onReply,
                                 const QString& triggerCharacter)
{
    FeatureTarget t;
    if (!featureGate(editor, QStringLiteral("completionProvider"), onReply, &t))
        return 0;
    QJsonObject params = wireDocumentPosition(t.syncer, position);
    QJsonObject context;
    if (triggerCharacter.isEmpty()) {
        context.insert(QStringLiteral("triggerKind"), 1); // Invoked
    } else {
        context.insert(QStringLiteral("triggerKind"), 2); // TriggerForCharacter
        context.insert(QStringLiteral("triggerCharacter"), triggerCharacter);
    }
    params.insert(QStringLiteral("context"), context);
    return t.client->request(QStringLiteral("textDocument/completion"), params,
                             onReply);
}

qint64 LspHub::resolveCompletionItem(qmn::view::CodeEditor* editor,
                                     const QJsonObject& item,
                                     const FeatureHandler& onReply)
{
    FeatureTarget t;
    if (!featureGate(editor, QStringLiteral("completionProvider"), onReply, &t))
        return 0;
    // resolveProvider is NESTED (completionProvider's object form; the spec
    // defaults it to false), so the flat key gate above is not enough.
    const QJsonValue completion =
        t.client->serverCapabilities().value(QStringLiteral("completionProvider"));
    if (!completion.isObject()
        || !completion.toObject().value(QStringLiteral("resolveProvider")).toBool()) {
        if (onReply)
            onReply(localRefusal(kRefusalNoProvider,
                                 QStringLiteral("capability "
                                                "\"completionProvider.resolveProvider\" "
                                                "is not offered")));
        return 0;
    }
    // Params ARE the item (spec): the app hands back what the list sent.
    return t.client->request(QStringLiteral("completionItem/resolve"),
                             QJsonValue(item), onReply);
}

qint64 LspHub::requestCodeActions(qmn::view::CodeEditor* editor,
                                  const qmn::view::TextPosition& position,
                                  const FeatureHandler& onReply)
{
    FeatureTarget t;
    if (!featureGate(editor, QStringLiteral("codeActionProvider"), onReply, &t))
        return 0;
    QJsonObject params = wireDocumentPosition(t.syncer, position);
    const QJsonObject positionObject =
        params.value(QStringLiteral("position")).toObject();
    params.remove(QStringLiteral("position")); // the range shape wins here
    params.insert(QStringLiteral("range"),
                  QJsonObject{{QStringLiteral("start"), positionObject},
                              {QStringLiteral("end"), positionObject}});
    // context.diagnostics: the document's CURRENT decoded diagnostics,
    // re-encoded through the mirror's wire frame — clangd's quickfixes key
    // off them, and the hub is the only party holding the decoded set.
    const QString text = t.syncer->mirrorText();
    const qmn::lsp::PositionConverter::Encoding enc = t.syncer->encoding();
    QJsonArray diagnostics;
    const QVector<qmn::view::CodeEditor::Diagnostic> current =
        editor->diagnostics();
    for (const qmn::view::CodeEditor::Diagnostic& diag : current) {
        const qmn::lsp::LspPosition from = qmn::lsp::PositionConverter::toPosition(
            text,
            qmn::lsp::PositionConverter::fromModelPosition(
                text, qmn::lsp::ModelPosition{diag.startLine, diag.startCol}),
            enc);
        const qmn::lsp::LspPosition to = qmn::lsp::PositionConverter::toPosition(
            text,
            qmn::lsp::PositionConverter::fromModelPosition(
                text, qmn::lsp::ModelPosition{diag.endLine, diag.endCol}),
            enc);
        QJsonObject record{
            {QStringLiteral("range"),
             QJsonObject{{QStringLiteral("start"),
                          QJsonObject{{QStringLiteral("line"), from.line},
                                      {QStringLiteral("character"), from.character}}},
                         {QStringLiteral("end"),
                          QJsonObject{{QStringLiteral("line"), to.line},
                                      {QStringLiteral("character"), to.character}}}}},
            {QStringLiteral("severity"), static_cast<int>(diag.severity) + 1},
            {QStringLiteral("message"), diag.message}};
        if (!diag.source.isEmpty()) // the post-1.8.0 label rides ONLY when
                                    // the producer sent one: the context
                                    // stays byte-identical for unlabeled
                                    // servers (the fakeserver's machine-5
                                    // scan-compare sees the same symmetry)
            record.insert(QStringLiteral("source"), diag.source);
        diagnostics.append(record);
    }
    params.insert(QStringLiteral("context"),
                  QJsonObject{{QStringLiteral("diagnostics"), diagnostics}});
    return t.client->request(QStringLiteral("textDocument/codeAction"), params,
                             onReply);
}

qint64 LspHub::requestFormatting(qmn::view::CodeEditor* editor,
                                 const FeatureHandler& onReply)
{
    FeatureTarget t;
    if (!featureGate(editor, QStringLiteral("documentFormattingProvider"),
                     onReply, &t))
        return 0;
    // v1 sends fixed options (the view indents with literal tabs; there is
    // no formatter-settings surface yet — that joins with the popup train).
    return t.client->request(
        QStringLiteral("textDocument/formatting"),
        QJsonObject{{QStringLiteral("textDocument"),
                     QJsonObject{{QStringLiteral("uri"), t.syncer->uri()}}},
                    {QStringLiteral("options"),
                     QJsonObject{{QStringLiteral("tabSize"), 4},
                                 {QStringLiteral("insertSpaces"), true}}}},
        onReply);
}

qint64 LspHub::requestRename(qmn::view::CodeEditor* editor,
                             const qmn::view::TextPosition& position,
                             const QString& newName,
                             const FeatureHandler& onReply)
{
    FeatureTarget t;
    if (!featureGate(editor, QStringLiteral("renameProvider"), onReply, &t))
        return 0;
    QJsonObject params = wireDocumentPosition(t.syncer, position);
    params.insert(QStringLiteral("newName"), newName);
    return t.client->request(QStringLiteral("textDocument/rename"), params,
                             onReply);
}

// ------------------------------------------------- popup triggering (M8 sliver)
// PLAN always assigned this half to the app side ("All triggering is
// glue-side (cursorMoved/editApplied)"); the M8 close-train wired it into
// the hub, where the feature doors, the syncer mirror and the registration
// bookkeeping already live. The view stays display-only: only the DECISION
// of when to ask moved here. Everything is opt-in per editor — a hub that
// never calls setPopupTriggers changes not one wire byte. (The state struct
// and the decode helpers sit up top, where releaseRegistration needs them.)

LspHub::TriggerState* LspHub::triggerEntry(const QString& uri)
{
    auto it = triggers_.find(uri);
    if (it != triggers_.end())
        return it->second.get();
    auto state = std::make_unique<TriggerState>();
    TriggerState* raw = state.get();
    raw->completionTimer.setSingleShot(true);
    raw->hoverTimer.setSingleShot(true);
    connect(&raw->completionTimer, &QTimer::timeout, this,
            [this, uri] { fireCompletionTrigger(uri); });
    connect(&raw->hoverTimer, &QTimer::timeout, this,
            [this, uri] { fireHoverTrigger(uri); });
    triggers_.emplace(uri, std::move(state));
    return raw;
}

LspHub::TriggerState* LspHub::triggerFor(const QString& uri) const
{
    auto it = triggers_.find(uri);
    return it == triggers_.end() ? nullptr : it->second.get();
}

void LspHub::setPopupTriggers(qmn::view::CodeEditor* editor, bool on)
{
    if (!editor || closingDown_)
        return;
    qmn::lsp::DocumentSyncer* syncer = syncerFor(editor);
    if (!syncer)
        return; // no OPEN hub document: there is no registration to arm
    TriggerState* t = on ? triggerEntry(syncer->uri())
                         : triggerFor(syncer->uri());
    if (!t) { // OFF for a never-armed editor is still a dismissal, nothing more
        editor->hideCompletionList();
        editor->hideHover();
        return;
    }
    if (!on) {
        t->on = false;
        t->completionTimer.stop();
        t->hoverTimer.stop();
        t->pendingTypingMove = false;
        t->lastMouseHoverPos = {-1, -1};
        if (t->pendingCompletion != 0) {
            if (qmn::lsp::LspClient* client = clientForUri(syncer->uri()))
                client->cancel(t->pendingCompletion);
            t->pendingCompletion = 0;
        }
        editor->hideCompletionList();
        editor->hideHover();
        return;
    }
    t->on = true;
    t->lastMouseHoverPos = {-1, -1}; // a fresh arm asks fresh
}

bool LspHub::popupTriggersEnabled(qmn::view::CodeEditor* editor) const
{
    if (!editor)
        return false;
    const qmn::lsp::DocumentSyncer* syncer = syncerFor(editor);
    const TriggerState* t = syncer ? triggerFor(syncer->uri()) : nullptr;
    return t && t->on;
}

void LspHub::onTriggerEdit(const QString& uri, int start, int end,
                           const QString& inserted)
{
    TriggerState* t = triggerFor(uri);
    if (!t || !t->on)
        return;
    // The edit's OWN cursorMoved follows (the buffer mutation emitted this
    // editApplied, and the view re-emits cursorMoved once the caret is
    // remapped): typing never hovers.
    t->pendingTypingMove = true;
    if (end != start || inserted.size() != 1)
        return; // only TYPING counts: a pure one-unit insertion
    t->typedChar = inserted.at(0);
    t->completionTimer.start(kCompletionTriggerMs); // every keystroke
                                                    // RE-ARMS the debounce:
                                                    // a burst coalesces into
                                                    // one request
}

void LspHub::onTriggerCaretMoved(const QString& uri,
                                 const qmn::view::TextPosition& pos)
{
    Q_UNUSED(pos); // the caret itself is read at fire time — the LATEST one
    TriggerState* t = triggerFor(uri);
    if (!t || !t->on)
        return;
    if (t->pendingTypingMove) {
        t->pendingTypingMove = false; // the edit's own move: no hover
        return;
    }
    t->hoverTimer.start(kHoverTriggerMs); // every non-typing move RE-ARMS:
                                          // the hover fires when the caret
                                          // sits still
}

// The MOUSE-hover route (post-1.8.0 sliver): the view already DWELLED
// (kMouseHoverDwellMs, view-side gates: text only, never popups/gutter/
// ruler/original window) and reports the point under the pointer. The hub
// still decides: the `on` bit + registration liveness + completion owns
// the moment (the caret-path rule, mirrored) + a no-refire pin — if the
// pointer re-settles on the SAME point while that hover is still visible,
// the wire stays silent. Then the shared-hover composition asks AT THE
// POINTER (the caret is never consulted here).
void LspHub::onTriggerMouseHover(const QString& uri,
                                 const qmn::view::TextPosition& pos)
{
    TriggerState* t = triggerFor(uri);
    if (!t || !t->on)
        return; // triggers OFF: the tap rides every registration, the bit
                // is what makes an unarmed editor a silent no-op
    auto reg = registrations_.constFind(uri);
    if (reg == registrations_.constEnd() || !reg->editor)
        return;
    qmn::view::CodeEditor* editor = reg->editor;
    if (editor->completionListVisible() || t->pendingCompletion != 0)
        return;
    if (pos == t->lastMouseHoverPos && editor->hoverVisible())
        return; // same point, same popup: no second ask
    t->lastMouseHoverPos = pos;
    fireHoverAt(uri, pos);
}

// The ACCEPT route (post-1.8.0 sliver): the user accepted a shown row and
// the VIEW emitted it — the view never inserts (display-only contract), so
// the insertion lives HERE. The guards ride the frozen semantics: the
// trigger `on` bit is the opt-in gate (the tap is connected for EVERY
// registration, so the gate is what makes an OFF editor a silent refusal),
// the registration must still be alive, and a row with NO payload (the
// all-zero default from a server that sent neither textEdit nor insertText)
// is refused silently. ONE applyTextEdits call = ONE undo group (one Ctrl+Z
// reverses the accept), and the syncer rides the view's textChanged like
// for any local edit, so the server sees the didChange right away. No auto
// re-completion after accept (VS Code's rule): the next qualifying typing
// burst re-triggers on its own.
void LspHub::onTriggerAccept(const QString& uri,
                             const qmn::view::CodeEditor::CompletionItem& item)
{
    const TriggerState* t = triggerFor(uri);
    if (!t || !t->on)
        return;
    const auto reg = registrations_.constFind(uri);
    if (reg == registrations_.constEnd() || !reg->editor)
        return; // released between the emit and the delivery
    const qmn::view::CodeEditor::TextEdit& ed = item.textEdit;
    if (ed.newText.isEmpty() && ed.startLine == 0 && ed.startCol == 0
        && ed.endLine == 0 && ed.endCol == 0)
        return; // no payload at all: nothing to insert
    reg->editor->applyTextEdits({ed});
}

void LspHub::fireCompletionTrigger(const QString& uri)
{
    TriggerState* t = triggerFor(uri);
    if (!t || !t->on)
        return;
    auto reg = registrations_.constFind(uri);
    if (reg == registrations_.constEnd() || !reg->editor)
        return; // the document is gone: nothing to ask, nothing to show
    qmn::lsp::LspClient* client = clientForUri(uri);
    // SUPERSEDE: the cancel fulfills the stale request LOCALLY with -32802
    // right here (its handler below shows nothing) and tells the server not
    // to bother — one popup, never a late reply clobbering a newer one.
    if (t->pendingCompletion != 0) {
        if (client)
            client->cancel(t->pendingCompletion);
        t->pendingCompletion = 0;
    }
    // Is the typed unit a REASON to complete? An identifier unit is always
    // Invoked (kind 1); anything else only when it is one of the
    // completionProvider's triggerCharacters (kind 2, the char rides);
    // anything else fires NOTHING (typing a space hides and stays hidden).
    const QChar unit = t->typedChar;
    QString triggerCharacter;
    if (!isTriggerUnit(unit)) {
        QStringList advertised;
        if (client) {
            const QJsonArray chars
                = client->serverCapabilities()
                      .value(QStringLiteral("completionProvider"))
                      .toObject()
                      .value(QStringLiteral("triggerCharacters"))
                      .toArray();
            for (const QJsonValue& value : chars)
                advertised << value.toString();
        }
        if (!advertised.contains(QString(unit)))
            return;
        triggerCharacter = QString(unit);
    }
    const qmn::view::TextPosition caretAtFire
        = reg->editor->position(); // captured for the reply: the accept
                                   // payload's fallback anchor (the caret may
                                   // have moved on when the reply lands)
    const qint64 id = requestCompletion(
        reg->editor, caretAtFire,
        [this, uri, caretAtFire](const qmn::protocol::RpcClient::Reply& reply) {
            if (TriggerState* s = triggerFor(uri))
                s->pendingCompletion = 0; // THIS request is over, by whatever
                                          // route (reply, -32802, timeout, refusal)
            auto reg2 = registrations_.constFind(uri);
            if (reg2 == registrations_.constEnd() || !reg2->editor)
                return; // nowhere to show
            if (reply.isError)
                return; // refusals/cancels/timeouts show NOTHING (the
                        // superseded request never touches the popup)
            // The result is CompletionItem[] OR a CompletionList object —
            // both legal per spec; label+detail plus the decoded/synthesized
            // ACCEPT payload (completionTextEdit above) cross into the view,
            // capped at kMaxCompletionRows (no resolve path, PLAN).
            QJsonArray items;
            if (reply.result.isArray())
                items = reply.result.toArray();
            else if (reply.result.isObject())
                items = reply.result.toObject()
                            .value(QStringLiteral("items"))
                            .toArray();
            QVector<qmn::view::CodeEditor::CompletionItem> rows;
            for (const QJsonValue& value : items) {
                if (!value.isObject())
                    continue;
                const QJsonObject item = value.toObject();
                const QString label
                    = item.value(QStringLiteral("label")).toString();
                if (label.isEmpty())
                    continue; // a row without a label is no row
                rows.append({label,
                             item.value(QStringLiteral("detail")).toString(),
                             completionTextEdit(item, caretAtFire)});
                if (rows.size() >= kMaxCompletionRows)
                    break;
            }
            reg2->editor->showCompletionList(rows); // EMPTY rows = hide
        },
        triggerCharacter);
    t->pendingCompletion = id > 0 ? id : 0; // 0 = locally refused: nothing
                                            // in flight to supersede
}

// The diagnostics half of the SHARED hover popup (post-1.8.0 sliver, the
// user-decided VS Code shape): the records are the VIEW's decoration state
// (onDiagnostics fed them; the view holds them sorted). Match = the range
// OVERLAPPING the caret's LINE — a column mismatch never hides a current-
// line entry. The severity word + the `[source]` label tell the multi-
// producer story (the LSP `source`, decoded as it came); a multi-line
// message prints ONLY its first line — the popup is a strip, not an
// output panel.
QStringList hoverDiagnosticLines(
    const QVector<qmn::view::CodeEditor::Diagnostic>& all, int caretLine)
{
    QStringList out;
    for (const qmn::view::CodeEditor::Diagnostic& d : all) {
        if (d.startLine > caretLine || d.endLine < caretLine)
            continue;
        QString line;
        switch (d.severity) {
        case qmn::view::CodeEditor::Severity::Warning:
            line = QStringLiteral("warning:");
            break;
        case qmn::view::CodeEditor::Severity::Information:
            line = QStringLiteral("info:");
            break;
        case qmn::view::CodeEditor::Severity::Hint:
            line = QStringLiteral("hint:");
            break;
        default:
            line = QStringLiteral("error:");
            break;
        }
        line += QLatin1Char(' ');
        if (!d.source.isEmpty())
            line += QStringLiteral("[") + d.source + QStringLiteral("] ");
        const int nl = d.message.indexOf(QLatin1Char('\n'));
        out << line + (nl < 0 ? d.message : d.message.left(nl));
    }
    return out;
}

// The frame of a diagnostics-only hover: the record CONTAINING the caret
// first, else the first one overlapping its line (the view's set is
// sorted, so "first" reads top-left). All -1 = none: the caller falls
// back to its usual word/caret frame.
qmn::view::CodeEditor::HoverRange hoverDiagnosticFrame(
    const QVector<qmn::view::CodeEditor::Diagnostic>& all,
    const qmn::view::TextPosition& caret)
{
    qmn::view::CodeEditor::HoverRange onLine{-1, -1, -1, -1};
    for (const qmn::view::CodeEditor::Diagnostic& d : all) {
        if (d.startLine > caret.line || d.endLine < caret.line)
            continue;
        const bool afterStart = d.startLine < caret.line
            || (d.startLine == caret.line && d.startCol <= caret.col);
        const bool beforeEnd = caret.line < d.endLine
            || (caret.line == d.endLine && caret.col <= d.endCol);
        if (afterStart && beforeEnd)
            return {d.startLine, d.startCol, d.endLine, d.endCol};
        if (onLine.startLine < 0)
            onLine = {d.startLine, d.startCol, d.endLine, d.endCol};
    }
    return onLine;
}

// The CARET wrapper (the hoverTimer's target): the LATEST caret is read at
// fire time, and the mouse-point cache is invalidated — a caret popup is a
// different hover region, so the next pointer dwell must ask again.
void LspHub::fireHoverTrigger(const QString& uri)
{
    auto reg = registrations_.constFind(uri);
    if (reg == registrations_.constEnd() || !reg->editor)
        return;
    if (TriggerState* t = triggerFor(uri))
        t->lastMouseHoverPos = {-1, -1};
    fireHoverAt(uri, reg->editor->position());
}

// The shared-hover composition core: BOTH hover routes (the caret trigger
// and the post-1.8.0 mouse-dwell route) ride it, anchored at `at` — which
// for the mouse route is the pointer point, NOT the caret.
void LspHub::fireHoverAt(const QString& uri, const qmn::view::TextPosition& at)
{
    TriggerState* t = triggerFor(uri);
    if (!t || !t->on)
        return;
    auto reg = registrations_.constFind(uri);
    if (reg == registrations_.constEnd() || !reg->editor)
        return;
    qmn::view::CodeEditor* editor = reg->editor;
    // Completion and hover NEVER share the moment: the hover is skipped
    // while completion owns the popup — in flight or visible.
    if (editor->completionListVisible() || t->pendingCompletion != 0)
        return;
    // The SHARED hover popup (post-1.8.0 sliver): the hovered line's
    // decoded diagnostics LEAD the popup and the server's hover contents
    // ride at the end. The diagnostics half needs no provider at all — the
    // pushed set already lives in the view's decoration state.
    const QStringList diagLines
        = hoverDiagnosticLines(editor->diagnostics(), at.line);
    const qmn::view::CodeEditor::HoverRange diagFrame
        = hoverDiagnosticFrame(editor->diagnostics(), at);
    qmn::lsp::LspClient* client = clientForUri(uri);
    if (!client || !client->isReady()
        || !capabilityOffered(client, QStringLiteral("hoverProvider"))) {
        // No provider (or no live server): the hover QUESTION never goes
        // to the wire — the diagnostics half fills the popup right away,
        // and an empty composition keeps m6's hide.
        if (diagLines.isEmpty()) {
            editor->hideHover();
            return;
        }
        const QString text
            = reg->syncer ? reg->syncer->mirrorText() : QString();
        editor->showHover(
            diagFrame.startLine < 0 ? wordRangeAt(text, at) : diagFrame,
            diagLines.join(QLatin1Char('\n')));
        return;
    }
    requestHover(
        editor, at,
        [this, uri, at, diagLines,
         diagFrame](const qmn::protocol::RpcClient::Reply& reply) {
            auto reg2 = registrations_.constFind(uri);
            if (reg2 == registrations_.constEnd() || !reg2->editor)
                return;
            qmn::view::CodeEditor* ed = reg2->editor;
            if (reply.isError)
                return; // refusal/cancel/timeout: an OLD popup stays as-is
            QString plain;
            QJsonObject rangeWire;
            if (!reply.result.isNull() && reply.result.isObject()) {
                const QJsonObject hover = reply.result.toObject();
                plain = hoverPlainText(
                    hover.value(QStringLiteral("contents")));
                rangeWire = hover.value(QStringLiteral("range")).toObject();
            }
            // The SHARED popup: diagnostics rows first, the contents after
            // them. A null/empty hover result NO LONGER HIDES when the
            // diagnostics half carries the popup (the recorded deviation
            // from m6's empty-hides rule); the hide survives for the
            // EMPTY composition.
            QStringList out = diagLines;
            if (!plain.isEmpty())
                out << plain;
            if (out.isEmpty()) {
                ed->hideHover(); // nothing to show: m6's empty hides
                return;
            }
            // The FRAME decodes against the MIRROR — the one text the
            // server answered against (featureGate flushed it before the
            // request). The server's `range` wins; else the diagnostics
            // frame; else the word at the hover point; else the
            // zero-width range there.
            const QString text = reg2->syncer ? reg2->syncer->mirrorText()
                                              : QString();
            const qmn::lsp::PositionConverter::Encoding encoding
                = reg2->syncer ? reg2->syncer->encoding()
                               : qmn::lsp::PositionConverter::Encoding::Utf16;
            qmn::view::CodeEditor::HoverRange range;
            if (!rangeWire.isEmpty()) {
                const QJsonObject from
                    = rangeWire.value(QStringLiteral("start")).toObject();
                const QJsonObject to
                    = rangeWire.value(QStringLiteral("end")).toObject();
                const qmn::lsp::ModelPosition fromPos
                    = qmn::lsp::PositionConverter::toModelPosition(
                        text, from.value(QStringLiteral("line")).toInt(),
                        from.value(QStringLiteral("character")).toInt(),
                        encoding);
                const qmn::lsp::ModelPosition toPos
                    = qmn::lsp::PositionConverter::toModelPosition(
                        text, to.value(QStringLiteral("line")).toInt(),
                        to.value(QStringLiteral("character")).toInt(),
                        encoding);
                range = {fromPos.line, fromPos.col, toPos.line, toPos.col};
            } else if (diagFrame.startLine >= 0) {
                range = diagFrame;
            } else {
                range = wordRangeAt(text, at);
            }
            ed->showHover(range, out.join(QLatin1Char('\n')));
        });
}

// --------------------------------------------------- the write path (M5)
LspHub::ApplyReport LspHub::applyDecoded(
    const qmn::lsp::DecodedWorkspaceEdit& decoded,
    const qmn::lsp::LspClient* servingClient)
{
    ApplyReport report;
    report.refused = decoded.problems;

    // v1 has no workspace/file model: a create/rename/delete documentChange
    // is STRUCTURE the app owns (PLAN's rule for a multi-file rename). The
    // text edits of such an edit are NOT applied half — all-or-nothing.
    for (const QString& kind : decoded.unsupportedKinds())
        report.refused << QStringLiteral(
            "workspace structure change \"%1\" is handed to the app "
            "(v1 applies text edits only)")
                           .arg(kind);

    // Group per document in the model frame, gating EVERY uri first: the
    // reason list comes back whole, never one refusal at a time.
    struct Gate {
        qmn::view::CodeEditor* editor = nullptr;
        qmn::lsp::DocumentSyncer* syncer = nullptr;
    };
    QMap<QString, QVector<qmn::view::CodeEditor::TextEdit>> byDoc;
    QMap<QString, Gate> gates;
    for (const qmn::lsp::WireTextEdit& wire : decoded.textEdits) {
        auto reg = registrations_.constFind(wire.uri);
        if (reg == registrations_.constEnd()) {
            report.refused << QStringLiteral("\"%1\" is not a hub document")
                               .arg(wire.uri);
            continue;
        }
        qmn::lsp::LspClient* client = clientForUri(wire.uri);
        if (!client || !client->isReady()) {
            report.refused << QStringLiteral("no live server serves \"%1\"")
                               .arg(wire.uri);
            continue;
        }
        if (servingClient && client != servingClient) {
            // The write-path cross-wire guard (M4's publish rule, told the
            // other way): an edit naming a document that THIS client does
            // not serve is a wire lie.
            report.refused << QStringLiteral("\"%1\" is served by a "
                                             "different server")
                               .arg(wire.uri);
            continue;
        }
        reg->syncer->flush(); // the server-side frame BEFORE the pin check:
                              // version() tracks what the server holds, and
                              // the mirror (== the editor's text) is the
                              // decode frame only once everything went out
        if (wire.documentVersion >= 0
            && qint64(wire.documentVersion) != reg->syncer->version()) {
            report.refused << QStringLiteral(
                "\"%1\" edit pinned at version %2, the document is at version %3")
                               .arg(wire.uri)
                               .arg(wire.documentVersion)
                               .arg(reg->syncer->version());
            continue;
        }
        if (reg->editor->options()->isReadOnly()) {
            // M19 rule: readOnly lives on the options object, not a flat
            // editor door. A locked document refuses the WHOLE edit.
            report.refused << QStringLiteral("\"%1\" is read-only").arg(wire.uri);
            continue;
        }
        // Wire -> model, against the mirror (the clamping frame M4 taught:
        // PositionConverter clamps total, an inverted pair is swapped).
        const QString text = reg->syncer->mirrorText();
        const qmn::lsp::PositionConverter::Encoding enc = reg->syncer->encoding();
        qmn::lsp::ModelPosition from = qmn::lsp::PositionConverter::toModelPosition(
            text, wire.startLine, wire.startCol, enc);
        qmn::lsp::ModelPosition to = qmn::lsp::PositionConverter::toModelPosition(
            text, wire.endLine, wire.endCol, enc);
        if (to.line < from.line || (to.line == from.line && to.col < from.col))
            std::swap(from, to);
        qmn::view::CodeEditor::TextEdit edit;
        edit.startLine = from.line;
        edit.startCol = from.col;
        edit.endLine = to.line;
        edit.endCol = to.col;
        edit.newText = wire.newText;
        byDoc[wire.uri].append(edit);
        gates.insert(wire.uri, Gate{reg->editor, reg->syncer});
    }

    // Overlap pre-check on the mirror frame — mirror == editor text after
    // the flush, so these offsets are EXACTLY what the view door computes;
    // catching it here is what keeps the refusal ALL-OR-NOTHING across
    // documents (the view would only refuse ONE document's batch).
    for (auto it = byDoc.constBegin(); it != byDoc.constEnd(); ++it) {
        const QString text = gates.value(it.key()).syncer->mirrorText();
        QVector<QPair<int, int>> ranges;
        ranges.reserve(it->size());
        for (const qmn::view::CodeEditor::TextEdit& edit : it.value()) {
            const int a = qmn::lsp::PositionConverter::fromModelPosition(
                text, qmn::lsp::ModelPosition{edit.startLine, edit.startCol});
            const int b = qmn::lsp::PositionConverter::fromModelPosition(
                text, qmn::lsp::ModelPosition{edit.endLine, edit.endCol});
            ranges.append(a <= b ? qMakePair(a, b) : qMakePair(b, a));
        }
        std::sort(ranges.begin(), ranges.end());
        for (int i = 1; i < ranges.size(); ++i) {
            if (ranges[i - 1].second > ranges[i].first) {
                report.refused << QStringLiteral(
                    "\"%1\" carries overlapping TextEdit ranges").arg(it.key());
                break; // one reason per document is enough
            }
        }
    }

    if (!report.refused.isEmpty())
        return report; // nothing was touched: the reasons are the report

    // Apply: uri order (QMap's own) -> deterministic, pinnable. The
    // begin/endServerEdit pair suppresses the echo and closes with ONE full
    // sync; applyTextEdits is ONE undo group per document, walking bottom-up
    // (N documents = N undo steps: each document is its own action).
    for (auto it = byDoc.constBegin(); it != byDoc.constEnd(); ++it) {
        const Gate gate = gates.value(it.key());
        gate.syncer->beginServerEdit();
        const int applied = gate.editor->applyTextEdits(it.value());
        gate.syncer->endServerEdit();
        if (applied > 0) {
            report.documentsChanged += 1;
            report.editsApplied += applied;
        }
    }
    report.applied = true;
    return report;
}

LspHub::ApplyReport LspHub::applyWorkspaceEdit(
    const QJsonValue& workspaceEdit, const qmn::lsp::LspClient* servingClient)
{
    return applyDecoded(qmn::lsp::WorkspaceEditWire::decode(workspaceEdit),
                        servingClient);
}

LspHub::ApplyReport LspHub::applyCodeAction(const QJsonObject& codeAction,
                                            const qmn::lsp::LspClient* servingClient)
{
    return applyDecoded(qmn::lsp::WorkspaceEditWire::decodeCodeAction(codeAction),
                        servingClient);
}

LspHub::ApplyReport LspHub::applyReplyEdit(const QJsonValue& result,
                                           const qmn::lsp::LspClient* servingClient)
{
    return applyDecoded(qmn::lsp::WorkspaceEditWire::decode(result),
                        servingClient);
}

LspHub::ApplyReport LspHub::applyTextEditList(
    qmn::view::CodeEditor* editor, const QJsonValue& textEditsResult,
    const qmn::lsp::LspClient* servingClient)
{
    qmn::lsp::DocumentSyncer* syncer = syncerFor(editor);
    if (!syncer) {
        ApplyReport report;
        report.refused << QStringLiteral(
            "this document is not registered with the hub, so a bare "
            "TextEdit[] reply names no document to apply to");
        return report;
    }
    return applyDecoded(qmn::lsp::WorkspaceEditWire::decodeTextEditList(
                            textEditsResult, syncer->uri()),
                        servingClient);
}

void LspHub::onApplyEdit(const QString& specName, const QJsonValue& id,
                         const QJsonValue& editValue)
{
    qmn::lsp::LspClient* client = clientForSpec(specName);
    if (!client)
        return; // the session died mid-request: RpcClient's id is gone anyway
    // servingClient = the SENDER: every uri this edit names must be served
    // by this very client (the cross-wire guard for the write path).
    const ApplyReport report =
        applyDecoded(qmn::lsp::WorkspaceEditWire::decode(editValue), client);
    client->respondApplyEdit(id, report.applied); // the truth, either way
    if (!report.applied && !report.refused.isEmpty())
        emit applyEditRefused(specName, report.refused);
}

} // namespace qmn::app
