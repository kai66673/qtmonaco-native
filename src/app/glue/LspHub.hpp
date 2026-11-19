#pragma once

// M8 machine 4 (PLAN GATE 2): the app-layer LSP glue. This is THE place the
// view and the lsp layers meet — neither knows the other (the layering is
// contractual: `src/lsp/` never touches view/core, the view never touches
// protocol/lsp). The hub owns, per server SPEC (PLAN: one client per server
// spec, multi-document, VS Code shape):
//
//   - ONE LspClient  + ONE transport factory per spec, LAZY-STARTED on the
//     first openDocument of a language the spec claims (lifecycle: crash ->
//     the client's one auto-restart, then FAILED + the restartServer door),
//   - ONE DocumentSyncer per document, wired to the REAL edit stream:
//     CodeDocument::editApplied -> syncer::applyDelta (the PRE-OP contract
//     M3 pinned against synthetic feeds — this is that feed going live),
//   - the diagnostics FEED: textDocument/publishDiagnostics decoded through
//     PositionConverter into CodeEditor::setDiagnostics (M5 decoration).
//
// It also carries the SERVER MATRIX (built-ins + the deep-merged
// `.qmn/lsp.json` per PLAN: objects recurse, arrays replace whole) and the
// workspace root the ${WORKSPACE_ROOT} macro expands against.
//
// Lives in src/app/ (the glue layer). The class is deliberately testable
// headless: setTransportFactory() swaps the ProcessTransport spawn for a
// fakeserver factory (the tsts), and no method needs a visible window.
//
// Layering note (the M10 precedent wording): this header INCLUDES view
// headers and lsp headers, and links qmn_view + qmn_lsp — the dependency
// arrow view -> protocol/lsp stays unbuilt; the hub is a CLIENT of both.

#include <functional>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include "lsp/LspClient.hpp"
#include "lsp/WorkspaceEdit.hpp" // the decoder's records cross the applier doors
#include "protocol/Transport.hpp"

namespace qmn::view {
class CodeEditor;
struct TextPosition; // the caret frame the feature doors take
}

namespace qmn::lsp {
class DocumentSyncer; // src/lsp (the hub owns these; the header stays light)
}

namespace qmn::app {

class LspHub : public QObject {
    Q_OBJECT
public:
    // The destructor's grace for the in-flight stop barriers (LspClient's
    // two-phase stop is ASYNCHRONOUS: shutdown, RpcClient's 2 s reply grace,
    // exit, then the transport's own escalation). ~QProcess would otherwise
    // SIGKILL a server that was mid-handshake-out — a hub that outlives its
    // window must not orphan clangd.
    static constexpr int kStopBarrierMs = 3'000;

    // One matrix entry: how to spawn one language server and what to hand
    // its initialize. `${WORKSPACE_ROOT}` inside program/args/template
    // strings expands against setWorkspaceRoot() at start time.
    struct ServerSpec {
        QString name;
        QString program;
        QStringList args;
        QStringList languages; // the view's language ids this spec claims
                               // ("python", "cpp", ...): the app owns the
                               // suffix->language mapping (MainWindow), the
                               // hub speaks ids only
        QJsonObject initializeTemplate; // merged into the built params
    };

    // Same swap-the-pipe seam as the M2 tests' harness: called per session
    // (start + auto-restart), parent under `parent`. Default: spawn the
    // spec's program via ProcessTransport.
    using TransportFactory =
        std::function<qmn::protocol::Transport*(const ServerSpec& spec,
                                                QObject* parent)>;

    explicit LspHub(QObject* parent = nullptr);
    ~LspHub() override; // stops every client (clean didClose stays queued:
                        // the syncers close BEFORE the clients stop below)

    void setTransportFactory(TransportFactory factory);

    // --- matrix + config (PLAN: loaded BEFORE anything starts) -----------
    void setWorkspaceRoot(const QString& absolutePath);
    QString workspaceRoot() const { return workspaceRoot_; }
    // Reads <workspaceRoot>/.qmn/lsp.json ({"servers": {name: {...}}} —
    // deep-merged over the built-ins: objects recurse, ARRAYS REPLACE whole,
    // "extensions" re-folds the language map). No file: silent no-op.
    bool loadConfig();
    bool loadConfigFromJson(const QJsonObject& root); // the file's body door
    void setServerSpec(const ServerSpec& spec); // programmatic override
    QJsonObject serverSpec(const QString& name) const; // test pin (fields)
    QString specNameForLanguage(const QString& languageId) const;

    // --- documents --------------------------------------------------------
    // Registers `editor`'s document with the hub: connects editApplied ->
    // applyDelta, opens the syncer (didOpen rides the lazy start / the
    // ready() self-heal), and lazy-starts the language's spec client.
    // The hub unregisters automatically when the editor dies (destroyed()).
    // False = nothing registered (no server spec claims this language: the
    // honest no-op, exactly what opening a .txt file must do).
    bool openDocument(qmn::view::CodeEditor* editor, const QString& uri,
                      const QString& languageId,
                      const QString& serverName = QString());
    void closeDocument(qmn::view::CodeEditor* editor); // didClose + release
    // The app's save: flush-then-didSave, then markSaved (v1 modified door).
    // When the serving server's initialize capabilities carry
    // textDocumentSync.willSaveWaitUntil (M8 machine 6), the save takes an
    // ASYNC detour: flush -> textDocument/willSaveWaitUntil -> the reply's
    // edits land through the applier (its endServerEdit emits the full sync,
    // so the didSave sees the EDITED model) -> didSave -> markSaved. The
    // request answers EXACTLY ONCE (timeout/transport-death included: PLAN's
    // RpcClient rule), and a refused or failed edit round NEVER blocks the
    // save — it proceeds exactly like the plain path. No capability, no
    // server, hub closing: byte-identical to the plain synchronous save.
    void saveDocument(qmn::view::CodeEditor* editor);

    // Test/feature doors: the syncer for a registered editor (null when the
    // editor is not registered) and the client serving a uri.
    qmn::lsp::DocumentSyncer* syncerFor(qmn::view::CodeEditor* editor) const;
    qmn::lsp::LspClient* clientForUri(const QString& uri) const;
    // The client of ONE spec — null while the spec was never lazy-started
    // (the lazy-start pin: a spec nobody opened has NO client), its state()
    // door then reads the lifecycle (Starting/Ready/Failed).
    qmn::lsp::LspClient* clientForSpec(const QString& specName) const;
    // Manual lifecycle door for ONE spec (resets the auto-restart budget
    // inside the client; the documents self-heal through ready()).
    void restartServer(const QString& specName);
    void setDebounceMs(int ms); // applied to syncers created from now on...
    int debounceMs() const { return debounceMs_; }

    // --- feature doors v1 (M8 machine 5, PLAN's list) --------------------
    // A door takes the DOCUMENT (the app holds the editor) and a position in
    // the MODEL frame the view speaks — line + utf-16 column, exactly what
    // position()/cursorMoved() report. The handler fires EXACTLY ONCE: a
    // request refused on the spot answers with the code a SERVER would have
    // used (-32002 = no live server serves this document; -32601 = its
    // initialize capabilities do not offer this provider), never with silence
    // (the M2 rule for a non-Ready door). The return is the request id (>0)
    // when the request went out, 0 when it was refused locally.
    //
    // Every door's FIRST act is the syncer's flush(): a feature request asks
    // about the text the user is LOOKING AT, and the wire Position is built
    // against the syncer's mirror — the ONE text the server agrees with. The
    // version pin of a versioned documentChanges entry (see the applier) is
    // the pin that proves the flush really ran.
    //
    // The v1 feature gates are the initialize CAPABILITIES (the registration
    // registry M2 built is the pull-model half, M8b): a provider key counts as
    // offered when it is present and is not `false` — the object form counts
    // ({"workDoneProgress":true}, renameProvider's {"prepareProvider":...}).
    using FeatureHandler = std::function<void(
        const qmn::protocol::RpcClient::Reply&)>;

    qint64 requestHover(qmn::view::CodeEditor* editor,
                        const qmn::view::TextPosition& position,
                        const FeatureHandler& onReply);
    qint64 requestDefinition(qmn::view::CodeEditor* editor,
                             const qmn::view::TextPosition& position,
                             const FeatureHandler& onReply);
    qint64 requestReferences(qmn::view::CodeEditor* editor,
                             const qmn::view::TextPosition& position,
                             bool includeDeclaration,
                             const FeatureHandler& onReply);
    // triggerCharacter empty = CompletionTriggerKind::Invoked (1); a character
    // = TriggerForCharacter (2) and the character travels with it.
    qint64 requestCompletion(qmn::view::CodeEditor* editor,
                             const qmn::view::TextPosition& position,
                             const FeatureHandler& onReply,
                             const QString& triggerCharacter = QString());
    // completionItem/resolve: params ARE the item (spec), so the app hands
    // back the item it got from the completion list.
    qint64 resolveCompletionItem(qmn::view::CodeEditor* editor,
                                 const QJsonObject& item,
                                 const FeatureHandler& onReply);
    // The v1 codeAction request is the zero-width range AT the position plus
    // this document's CURRENT diagnostics in context (clangd's quickfixes
    // depend on them — the hub re-encodes its own decoded set).
    qint64 requestCodeActions(qmn::view::CodeEditor* editor,
                              const qmn::view::TextPosition& position,
                              const FeatureHandler& onReply);
    qint64 requestFormatting(qmn::view::CodeEditor* editor,
                             const FeatureHandler& onReply);
    qint64 requestRename(qmn::view::CodeEditor* editor,
                         const qmn::view::TextPosition& position,
                         const QString& newName, const FeatureHandler& onReply);

    // --- the write path: WorkspaceEdit application ------------------------
    // Both directions of the same machinery: a SERVER-primed edit
    // (workspace/applyEdit, relayed below and answered for real) and the
    // app's own results (a codeAction, a formatting or a rename reply).
    //
    // ALL-OR-NOTHING, and the reasons come back instead of a partial edit:
    // the structure must be well-formed, NO entry may name a file-level
    // operation (v1 has no workspace/file model — that structure is handed to
    // the app, PLAN's rule for a multi-file rename), every uri must be a hub
    // document with a live server (and, when `servingClient` is given, served
    // by THAT client: a cross-wire edit is refused like a cross-wire publish),
    // every versioned documentChange must pin the version our syncer is at,
    // and no two ranges of one document may overlap. A half-applied server
    // edit would make the ONE-undo-group promise a lie.
    //
    // Applying: per document, the syncer's beginServerEdit/endServerEdit pair
    // (the echo suppression PLAN pins) around CodeEditor::applyTextEdits —
    // which is ONE undo group and walks bottom-up — then ONE full-text sync.
    // N documents = N undo steps (each document is its own action); the
    // documents land in uri order (deterministic, pinnable).
    struct ApplyReport {
        bool applied = false;
        int documentsChanged = 0;
        int editsApplied = 0;
        QStringList refused; // WHY nothing was applied (empty when applied)
    };
    ApplyReport applyWorkspaceEdit(const QJsonValue& workspaceEdit,
                                   const qmn::lsp::LspClient* servingClient =
                                       nullptr);
    // The codeAction form: its `edit`, or the WorkspaceEdit hidden in
    // command.arguments[0] (clangd's apply-tweak shape).
    ApplyReport applyCodeAction(const QJsonObject& codeAction,
                                const qmn::lsp::LspClient* servingClient =
                                    nullptr);
    // PLAN's feature-door half: the same door the app uses for a rename or a
    // codeAction result, fed from a reply the app already holds. `result` is
    // the raw request result when that result IS a WorkspaceEdit (rename,
    // willSaveWaitUntil).
    ApplyReport applyReplyEdit(const QJsonValue& result,
                               const qmn::lsp::LspClient* servingClient =
                                   nullptr);
    // The SIBLING shape: formatting / rangeFormatting / onTypeFormatting
    // answer a BARE TextEdit[] (no uri, no version), so the document is the
    // one the app ASKED about — named here by its editor. Same applier, same
    // all-or-nothing gates, same servingClient guard.
    ApplyReport applyTextEditList(qmn::view::CodeEditor* editor,
                                  const QJsonValue& textEditsResult,
                                  const qmn::lsp::LspClient* servingClient =
                                      nullptr);

signals:
    // Relays of the per-server channels (PLAN: separate PROTOCOL/STDERR
    // logs; showMessage is a SIGNAL, visible — the app renders it).
    void serverShowMessage(const QString& spec, int type,
                           const QString& message);
    void serverLogMessage(const QString& spec, int type, const QString& message);
    void serverStderr(const QString& spec, const QString& line);
    // Unwanted death with BOTH budgets spent (auto + never manual): the app
    // surfaces this and offers restartServer.
    void serverFailed(const QString& spec, const QString& reason);
    // window/showMessageRequest RELAY (machine 5): the hub never picks an
    // action itself — the app answers through
    // clientForSpec(spec)->respondShowMessageRequest(id, item). The relay
    // lands BEFORE LspClient's Null default and RpcClient refuses a second
    // answer, so an app that answers inside this relay (a modal dialog
    // answered on the spot) wins the choice; a later, non-modal answer
    // would need the default deferred (an M8b decision).
    void serverShowMessageRequest(const QString& spec, const QJsonValue& id,
                                  const QJsonValue& params);
    // A SERVER-PRIMED workspace/applyEdit that the applier refused: the hub
    // has already answered the server applied:false (all-or-nothing is the
    // rule — a half-applied edit would make the one-undo-group promise a
    // lie), and the reasons come here so the app can show them.
    void applyEditRefused(const QString& spec, const QStringList& reasons);

private:
    struct SpecClient {
        ServerSpec spec;
        qmn::lsp::LspClient* client = nullptr;
    };
    struct Registration {
        qmn::view::CodeEditor* editor = nullptr;
        QString specName;
        qmn::lsp::DocumentSyncer* syncer = nullptr;
    };

    void buildBuiltins();
    SpecClient* specEntry(const QString& specName); // create + lazy start
                                                          // (the private
                                                          // sibling of the
                                                          // door above)
    QJsonObject buildInitializeParams(const ServerSpec& spec) const;
    QJsonValue expandTemplateValue(const QJsonValue& value) const;
    void rebuildLanguageMap();
    QString expand(const QString& text) const; // ${WORKSPACE_ROOT}
    void mergeSpecObject(const QString& name, const QJsonObject& patch);
    void onDiagnostics(const QString& specName, const QJsonObject& params);
    // The server-primed write path (machine 5): decode + gate + apply, then
    // answer the server with the truth and relay any refusal reasons.
    void onApplyEdit(const QString& specName, const QJsonValue& id,
                     const QJsonValue& editValue);
    // The shared body behind applyWorkspaceEdit/applyCodeAction/applyReplyEdit
    // (they differ only in which decoder shape produced the records).
    ApplyReport applyDecoded(const qmn::lsp::DecodedWorkspaceEdit& decoded,
                             const qmn::lsp::LspClient* servingClient);
    // The shared preamble of every feature door: resolve the document, refuse
    // locally (firing onReply with the SERVER's code: -32002 no live server,
    // -32601 capability not offered) or FLUSH the syncer and hand back the
    // pair the params builder needs. False = the refusal already fired.
    struct FeatureTarget {
        qmn::lsp::DocumentSyncer* syncer = nullptr;
        qmn::lsp::LspClient* client = nullptr;
    };
    bool featureGate(qmn::view::CodeEditor* editor,
                     const QString& capabilityKey,
                     const FeatureHandler& onReply, FeatureTarget* target);
    // Model frame -> wire frame against the syncer's MIRROR — the ONE text
    // the server agrees with (which is why featureGate flushes first).
    QJsonObject wirePosition(const qmn::lsp::DocumentSyncer* syncer,
                             const qmn::view::TextPosition& pos) const;
    QJsonObject wireDocumentPosition(
        const qmn::lsp::DocumentSyncer* syncer,
        const qmn::view::TextPosition& pos) const;
    void registerClient(const QString& specName, SpecClient& entry);
    void releaseRegistration(const QString& uri);
    QString resolveUri(qmn::view::CodeEditor* editor, const QString& uri);
    // Pump (bounded by kStopBarrierMs) until every client lands a terminal
    // state: the ONLY way the destructor can promise the servers are gone.
    void waitForStops();

    TransportFactory factory_;
    QString workspaceRoot_;
    QMap<QString, ServerSpec> specs_; // key = lowercased spec name
    QMap<QString, QString> languageToSpec_; // language id -> spec name
    QMap<QString, SpecClient> clients_; // spec name -> live client
    QMap<QString, Registration> registrations_; // uri -> document wiring
    int debounceMs_ = 400; // PLAN default; the hub's test seam
    int nextUntitled_ = 0; // uris the app never set get an "untitled:" key
    bool closingDown_ = false;
};

} // namespace qmn::app
