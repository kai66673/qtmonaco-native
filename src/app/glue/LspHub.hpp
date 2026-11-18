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
#include "protocol/Transport.hpp"

namespace qmn::view {
class CodeEditor;
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
