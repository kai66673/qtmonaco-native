#pragma once

// M8 machine 4 (PLAN GATE 2), promoted by the G1 glue-shelf surgery
// (2026-11-21): the LSP glue hub, now a REAL layer (src/glue, exported as
// qmn::glue) and no longer app code. This is THE place the view and the
// lsp layers meet — neither knows the other (the layering is contractual:
// `src/lsp/` never touches view/core, the view never touches
// protocol/lsp; src/glue is the client where they meet). The hub owns,
// per server SPEC (PLAN: one client per server
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
#include <map>
#include <memory>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include "lsp/LspClient.hpp"
#include "lsp/WorkspaceEdit.hpp" // the decoder's records cross the applier doors
#include "protocol/Transport.hpp"
#include "view/CodeEditor.hpp" // the ACCEPT tap's CompletionItem is a NESTED
                               // type: a forward declaration is not enough

namespace qmn::view {
class CodeEditor;
struct TextPosition; // the caret frame the feature doors take
}

namespace qmn::lsp {
class DocumentSyncer; // src/lsp (the hub owns these; the header stays light)
}

namespace qmn::glue {

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
    // GATE 3 car C (C2): textDocument/signatureHelp. The context rides the
    // SAME triggerKind rule as requestCompletion: empty triggerCharacter =
    // Invoked (1), a character = TriggerForCharacter (2) riding with it. The
    // client declares textDocument.signatureHelp {contextSupport:true} in
    // initialize (and NOTHING about signatureInformation — a plain-text
    // popup cannot paint parameter documentation or an active-parameter
    // underline, so the client never claims it can).
    qint64 requestSignatureHelp(qmn::view::CodeEditor* editor,
                                const qmn::view::TextPosition& position,
                                const FeatureHandler& onReply,
                                const QString& triggerCharacter = QString());
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

    // --- popup TRIGGERING (the M8 close-train sliver, PLAN: "All triggering
    //     is glue-side (cursorMoved/editApplied)") ---------------------------
    // With triggers ON for an OPEN document, the hub watches the view's own
    // signals and feeds the two SHOW doors itself — the view popups stay
    // display-only, only the DECISION moves here:
    //
    //   completion — a qualifying TYPING edit (a pure insertion of exactly
    //   one UTF-16 unit) restarts a kCompletionTriggerMs debounce; at fire
    //   time the unit is an identifier char (TriggerKind Invoked) or one of
    //   completionProvider's triggerCharacters (TriggerForCharacter + the
    //   char), anything else fires NOTHING. A superseded in-flight request
    //   is $/cancelRequest'd (its handler fulfills locally with -32802 and
    //   shows nothing) — one popup, never a late reply overwriting a newer.
    //   hover — a NON-typing caret move that sits idle for kHoverTriggerMs.
    //   The cursorMoved that pairs with an editApplied is the edit's OWN
    //   move (typing never hovers), and hover is skipped while a completion
    //   is in flight or visible.
    //   signatureHelp (GATE 3 car C, C2) — the SAME typing edit also re-arms
    //   a kSignatureTriggerMs debounce; at fire time the unit BEFORE the
    //   caret is classified: one of the signatureHelpProvider's advertised
    //   triggerCharacters (default "(" and "," when the provider names none)
    //   asks TriggerForCharacter (2); anything else SUPERSEDES an in-flight
    //   ask and hides a signature-owned popup (typing ")" or a plain unit
    //   ends the round). The popup is the SHARED hover popup: the hub shows
    //   signatures[activeSignature] (clamped) with its label VERBATIM,
    //   zero-width anchored at the fire-time caret; activeParameter is
    //   DROPPED (a plain-text popup never underlines it — the recorded
    //   plain-label divergence, the m6 no-markdown bullet extended). Hover
    //   and signature never share the moment: each skips while the other
    //   owns the popup or rides in flight, and typing only arms signature,
    //   non-typing moves only arm hover.
    //
    // Decoded feeds (the view doors are plain-Qt payloads, PLAN's popup
    // shape): completion items -> {label, detail}, the list capped at
    // kMaxCompletionRows (the popup is a list, not an indexer); the HOVER
    // popup is SHARED (post-1.8.0 sliver): the caret-line's decoded
    // DIAGNOSTICS lead it — one line each, prefixed by the severity word
    // and the `[source]` label — and the server's hover contents (string /
    // MarkupContent / MarkedString, arrays joined by newlines, VERBATIM,
    // markup NOT rendered) ride at the end when present. The diagnostics
    // half needs no provider: the pushed set lives in the view's
    // decoration state. The whole popup HIDES only when the composition is
    // EMPTY — a null/empty hover result no longer hides while diagnostics
    // carry it (the recorded deviation from m6's empty-hides rule). The
    // hover frame is the server's `range` when it sends one, else the
    // diagnostics frame, else the word at the caret, else a zero-width
    // caret range — the wire parts decoded against the syncer's MIRROR,
    // the one text the server agrees with.
    //
    // OFF by default, per editor: a hub that never opts in changes not one
    // wire byte (every existing test's traffic stays byte-identical). The
    // door needs an OPEN document (triggers ride the registration); turning
    // OFF stops the timers, cancels the in-flight completion AND the in-flight
    // signature ask, and hides both popups. The ACCEPT route rides this door
    // too (post-1.8.0 sliver): a
    // row the user accepts in the popup reaches the hub through the view's
    // completionAccepted signal, and the hub applies the row's textEdit via
    // applyTextEdits — ONE undo group, the syncer riding textChanged like
    // any local edit; no auto re-trigger (the next qualifying burst is a
    // re-type anyway). Accepting with the triggers OFF, or for a released
    // document, is a silent refusal.
    static constexpr int kCompletionTriggerMs = 150;
    static constexpr int kHoverTriggerMs = 500;
    static constexpr int kSignatureTriggerMs = 150;
    static constexpr int kMaxCompletionRows = 100;
    void setPopupTriggers(qmn::view::CodeEditor* editor, bool on);
    bool popupTriggersEnabled(qmn::view::CodeEditor* editor) const;

    // --- fold feed (GATE 3 car C, C3): textDocument/foldingRange ------------
    // The hub-internal half of the fold-provider seam (the VIEW-side seam is
    // the C1 pair `setFoldRanges`/`clearFoldRanges`): when the serving server
    // OFFERS folds — initialize `foldingRangeProvider` or the registry's
    // dynamic `textDocument/foldingRange` registration — the hub asks and
    // feeds, and the C1 seam makes that feed the editor's ONE fold source
    // (grammar `@fold` and the indent fallback go silent: one provider per
    // editor, enforced by construction). No FeatureHandler door crosses here
    // (the hub owns the feed, no std::function in any signature), so the
    // wheel's rejected log gains NOTHING new.
    //
    // The client declares `textDocument.foldingRange {lineFoldingOnly:true}`
    // — honest, because the view folds LINES: the wire's startCharacter/
    // endCharacter are DECODED NOWHERE. The spec's `endLine` is INCLUSIVE and
    // the view's FoldRange pair is too, so the two line pairs meet directly.
    // Decode discipline: clamp each line into the document, DROP `end <=
    // start` (a fold that hides nothing), sort start asc / end desc and keep
    // ONE entry per start (the WIDEST) — the tie rule of the view door
    // `setFoldRanges` verbatim (its documented contract), so the decode and
    // the door can never disagree about what a chatty wire means.
    //
    // Edges: after didOpen (a session already READY when the document opens),
    // after every re-READY, and debounced on EVERY editApplied — a
    // document replace is the core's one whole-document EditEvent, so the
    // same tap covers it (the C1 seam already cleared the stale override at
    // the replace itself). The ask FLUSHES the syncer first (like every
    // feature door: the request asks about the text the server agrees with)
    // and one ask is in flight per document, the re-arm discipline of the
    // completion timer. The answer shapes:
    //   ARRAY  -> feed (even when EMPTY: empty is the authority "nothing is
    //             foldable", the grammar stays mute while the claim stands);
    //   NULL   -> the same empty feed (the spec's own null means "no folds");
    //   ERROR  -> the claim FELL THROUGH: `clearFoldRanges()` hands the fold
    //             source back to the grammar, and THIS document stops asking
    //             until an open/re-READY edge re-arms it (a server that said
    //             no to an advertised provider is not hammered on every
    //             keystroke — the recorded reading);
    // teardown (closeDocument/editor death) cancels the in-flight ask and
    // clears ONLY A SET THE HUB ACTUALLY FED (the single-writer convention D3
    // minted for hints: never wipe an override some other writer installed).
    static constexpr int kFoldRefreshMs = 300;
    // A fold ask answers a plain line table; a HINT ask is heavier (label +
    // position per hint), so its window is wider and CAPPED on both sides:
    // `kInlayHintRangeLines` lines of document per ask, `kMaxLspInlayHints`
    // hints per set (stop adding past the cap, then pin the whole decoded
    // window to the view door — the cap rides the wire, not the paint).
    static constexpr int kHintRefreshMs = 300;
    static constexpr int kMaxLspInlayHints = 500;
    static constexpr int kInlayHintRangeLines = 2000;
    // Test/inspection door: did the fold feed for this document EVER hand the
    // view a set (the teardown-clear's precondition, and the honest answer to
    // "is this editor folding what the server said?").
    bool foldsFedFromServer(qmn::view::CodeEditor* editor) const;

    // car C (C4): the same two questions about the M5 INLAY-HINT door — the
    // hub's hint feed is the other half of D3's "single-writer convention"
    // over that door (qmn reads inlayHints() after a DAP stop-feed clears;
    // here the LSP side writes the same door, and a teardown clears ONLY a
    // set this hub fed).
    bool hintsFedFromServer(qmn::view::CodeEditor* editor) const;

    // car C (C5): the same question about the M5 DIAGNOSTIC door, the PULL
    // route's side — "was the editor's diagnostic set LAST written by this
    // hub's pull feed?" (One fed bit covers the door; the push route's
    // published marks are the SERVER's own trail and were never the hub's
    // to clear — that stance predates C5 and stands. The fallback and the
    // teardown clears ride this bit, single-writer convention again.)
    bool diagnosticsFedFromServer(qmn::view::CodeEditor* editor) const;

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
    // M8 machine 7: pushes one document's view-side stored configuration to
    // its server as workspace/didChangeConfiguration (the view is the BUFFER,
    // the hub the only consumer). Never called by the app: the edges are the
    // view's workspaceConfigurationChanged, this client's ready(), and the
    // document's own open. An empty or unparseable buffer sends nothing
    // (full rules at the definition).
    void pushWorkspaceConfiguration(const QString& uri);
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
    void releaseRegistration(const QString& uri,
                             bool editorAlive
                             = true); // false ONLY from the QObject::destroyed
                                      // relay: the CodeEditor part is gone by
                                      // then, so the fold-feed teardown skips
                                      // the (pointless) view clear — its
                                      // cancel + state death still happen
    QString resolveUri(qmn::view::CodeEditor* editor, const QString& uri);
    // The popup trigger machinery (the doors above): state lives per
    // registration key, born with the first setPopupTriggers(true) and gone
    // with the registration. The editApplied/cursorMoved/completionAccepted
    // taps connect in openDocument (context = the registration's syncer, the
    // same die-with-the-registration trick the edit feed uses), so the slots
    // below only ever see a lookup.
    struct TriggerState; // timers + bookkeeping; defined in the .cpp
    TriggerState* triggerEntry(const QString& uri); // creates + wires timers
    TriggerState* triggerFor(const QString& uri) const;
    void onTriggerEdit(const QString& uri, int start, int end,
                       const QString& inserted);
    void onTriggerCaretMoved(const QString& uri,
                             const qmn::view::TextPosition& pos);
    void onTriggerAccept(const QString& uri,
                         const qmn::view::CodeEditor::CompletionItem& item);
    // The MOUSE-hover route (post-1.8.0 sliver): the view already dwelled
    // (its own kMouseHoverDwellMs chrome debounce, view-side gates: text
    // only, never over the popups/gutter/ruler/original window) and now
    // reports the point under the pointer. The hub still owns the DECISION:
    // the `on` bit + registration liveness + the completion-owns-the-moment
    // rule + a same-point-while-hover-visible no-refire pin, then the ask
    // rides the shared-hover composition AT THAT POINT (the caret is never
    // consulted, and the pointer never moves the caret).
    void onTriggerMouseHover(const QString& uri,
                             const qmn::view::TextPosition& pos);
    void fireCompletionTrigger(const QString& uri);
    void fireSignatureTrigger(const QString& uri); // car C (C2): the
                                                   // signatureHelp twin,
                                                   // classified at fire time
    // The fold feed (car C, C3): the state lives per registration key NEXT
    // TO the trigger state but is NOT gated by its `on` bit — the feed is
    // hub-internal, a hub that never touched setPopupTriggers still feeds.
    struct FoldFeed; // timer + in-flight id + the two authority bits;
                     // defined in the .cpp
    struct HintFeed; // timer + in-flight id + the fed bit; .cpp-defined
    FoldFeed* foldFeedEntry(const QString& uri); // creates it (fold-provider
                                                 // gate ALREADY passed)
    bool foldingRangeLive(const QString& uri) const; // registration + client
                                                     // + offered-or-registered
    void armFoldFeed(const QString& uri,
                     bool authorityEdge = false); // re-arm the kFoldRefreshMs
                                          // debounce (open / re-READY / edit
                                          // edges). `authorityEdge` is the
                                          // open/READY shape: it CLEARS the
                                          // refused bit (a fresh session or a
                                          // fresh document is a fresh claim);
                                          // a plain edit edge stays down while
                                          // the refusal stands
    void requestFoldsNow(const QString& uri); // the fire-time body: flush,
                                              // supersede, ask, decode, feed
    void handBackFolds(const QString& specName); // client-death: every fed
                                                 // document of this spec goes
                                                 // back to the grammar
    // car C (C4): the whole-document inlayHint ask + its decode. Positions
    // come through the PositionConverter against the SYNCER MIRROR (the only
    // frame that keeps wire offsets honest); the view door is a whole-set
    // write, like folds'.
    HintFeed* hintFeedEntry(const QString& uri); // creates it (gate ALREADY
                                                 // passed)
    // The provider gate, folds' shape verbatim: offered at initialize OR
    // dynamic-registered (workspace/didChangeConfiguration). The hub
    // declares `inlayHintProvider: {}` and never asks for semanticTokens, so
    // `textDocument/inlayHint` can never arrive as a dynamic registration —
    // the OR branch stays for symmetry with folds and costs nothing.
    bool inlayHintLive(const QString& uri) const;
    // re-arm the kHintRefreshMs debounce on an edge (open / re-READY /
    // edit). No `refused` bit: an error answer leaves the LAST hint set
    // standing, and the next edge re-arms (the door has no grammar to hand
    // back to — recorded in the close-out).
    void armHintFeed(const QString& uri);
    void requestHintsNow(const QString& uri);
    // clear the hub's OWN hint sets for a dying claim — NEVER a producer this
    // hub never fed (an app's hints, DAP's stop table: the fed bit is the
    // gate, single-writer convention).
    void handBackHints(const QString& specName);
    // car C (C5): the PULL diagnostics feed. The mode is a per-ask GATE, not
    // stored mode state: `pullDiagnosticsLive` says pull is authority NOW
    // (offered at initialize OR dynamic-registered, minus this document's
    // `refused` bit — a server that advertised the provider and then refused
    // the pull FALLS BACK to push, the recorded plan shape, and the bit
    // stops the hammering). The feed asks {textDocument, identifier:"qmn"},
    // NEVER a previousResultId (always a full pull; the answer's resultId is
    // STORED and unused — recorded). kind:"unchanged" ignores the answer,
    // "full" (and the spec-impossible "partial", read tolerantly as full)
    // decodes through the SAME decoder the push route uses.
    static constexpr int kPullDiagnosticMs = 600; // PLAN-pinned debounce
    struct PullDiagFeed; // timer + in-flight id + refused/fed bits +
                         // lastResultId; .cpp-defined
    PullDiagFeed* pullDiagFeedEntry(const QString& uri); // creates it
    bool pullDiagnosticsLive(const QString& uri) const;  // the mode gate
    bool pullModeClient(const qmn::lsp::LspClient* client) const; // the
                                        // client half of the gate: offered
                                        // OR registered (no refused bit —
                                        // the push-drop guard asks this)
    void armPullDiagFeed(const QString& uri,
                         bool authorityEdge = false); // open / re-READY /
                                              // didSave / edit edges; the
                                              // authority edge clears the
                                              // refused bit (fresh claim)
    void requestPullDiagsNow(const QString& uri); // the fire-time body
    void stopPullFeeds(const QString& specName); // client-death: every fed
                                                 // diagnostic set of this
                                                 // spec's documents clears
    // (the push route never cleared at close and C5 does not change that;
                                                 // the fallback refusal
                                                 // clears through the same
                                                 // fed-bit gate)
    void fireHoverTrigger(const QString& uri); // caret wrapper: reads the
                                               // LATEST caret at fire time
    void fireHoverAt(const QString& uri,
                     const qmn::view::TextPosition& at); // the shared-hover
                                                         // composition core
    // Pump (bounded by kStopBarrierMs) until every client lands a terminal
    // state: the ONLY way the destructor can promise the servers are gone.
    void waitForStops();

    TransportFactory factory_;
    QString workspaceRoot_;
    QMap<QString, ServerSpec> specs_; // key = lowercased spec name
    QMap<QString, QString> languageToSpec_; // language id -> spec name
    QMap<QString, SpecClient> clients_; // spec name -> live client
    QMap<QString, Registration> registrations_; // uri -> document wiring
    // uri -> trigger state. std::map (NOT QMap/QHash): the values are
    // move-only, and Qt 6.9+'s detachable containers need copyable ones.
    std::map<QString, std::unique_ptr<TriggerState>> triggers_;
    // uri -> fold-feed state (car C, C3). Same std::map reason, and the SAME
    // life: born with the registration, gone with it.
    std::map<QString, std::unique_ptr<FoldFeed>> folds_;
    // per-DOCUMENT hint state (C4): the debounce timer is one per document,
    // like every feed's; the fed bit protects a producer the hub never
    // installed (an app's hints, DAP's stop table).
    std::map<QString, std::unique_ptr<HintFeed>> hints_;
    // uri -> pull-diagnostic-feed state (C5). Same std::map reason, same
    // life: born with the registration, gone with it.
    std::map<QString, std::unique_ptr<PullDiagFeed>> pulls_;
    int debounceMs_ = 400; // PLAN default; the hub's test seam
    int nextUntitled_ = 0; // uris the app never set get an "untitled:" key
    bool closingDown_ = false;
};

} // namespace qmn::glue
