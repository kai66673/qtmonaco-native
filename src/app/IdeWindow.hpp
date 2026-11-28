#pragma once

// qmn-ide-demo main window (GATE 4 cars I0+I2, PLAN): the IDE skeleton —
// a project folder (QFileSystemModel tree dock), a tab row (ONE tab
// per path: a re-open FOCUSES, never duplicates), the Find-in-Files
// panel over the qmn::search engine (car I1), quick open (Ctrl+P, the
// file pool rides the engine's LIST face — LEDGER catch #2) and the
// command palette (Ctrl+Shift+P) over the window's own registry (car
// I2), and the status-bar truth channel the tours/tests read. The
// window compiles into a static lib (qmn_ide_app) so tst_ide drives the
// EXACT doors the UI slots call, offscreen, without exec()ing anything
// (the qmn_demo_app precedent).
//
// What I0 deliberately does NOT carry (PLAN's I0 shape): no settings
// dialog (the 15-knob template is a promotion candidate the moment a
// second user needs it — never mid-car surgery), no DAP door (the
// debug panels are I3/I4 material; the --lsp door rides the demo's
// shape UNCHANGED from day one), no terminal (owner decision: OUT).
//
// The layering law rides: this header knows the glue shelf and
// qmn::search ONLY through forward-decls behind unique_ptr members
// (AGENTS: an app header carries no glue type beyond the fwd-decls),
// so tst_ide links the static lib without ever including a
// glue/search header itself.

#include <QDockWidget> // car I3: the outline dock (a real widget member)
#include <QJsonValue> // car I3: the raw feature-door items ride app-local
                      // vectors (a plain QtCore type; NO protocol/lsp type
                      // crosses this header — the I0 fwd-decl law rides)
#include <QMainWindow>
#include <QPair>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

#include "EditorSettings.hpp" // the app-namespace CodeEditor alias (the
                               // MainWindow.hpp pattern: EditorSettings
                               // owns the `using qmn::view::CodeEditor`)

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTabWidget;
class QTimer;
class QTreeWidget;
class QTreeView;
class QDockWidget;
class QFileSystemModel;

namespace qmn::glue {
class LspHub;         // the --lsp door (the demo's shape, unchanged)
class ProjectSession; // the ALWAYS-on document session (G2 precedent)
} // namespace qmn::glue

namespace qmn::search {
class SearchSession;
struct FileMatch;
} // namespace qmn::search

namespace qmn::app {

class IdeWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit IdeWindow(QWidget* parent = nullptr);
    ~IdeWindow() override; // out of line: the unique_ptr members need the
                           // complete types at THIS point

    // The project door: the tree root, the session's workspace root and
    // the .qmn/lsp.json fold all land here (the demo set the root at the
    // first openPath; the IDE knows the root EARLIER — it IS a door).
    // A second openProject replaces the tree; open tabs stay (document
    // lifetimes are the session's business, not the tree's).
    void openProject(const QString& dirPath);
    QString projectRoot() const { return projectRoot_; }

    // The tab door. False + a non-modal status-bar message on a failed
    // read (the demo's drivability law — no blocking dialogs here).
    // A path that is ALREADY a tab just FOCUSES it (the one-tab-per-path
    // law — car D's multi-view story is a later face; I0 pins the
    // focus-face, not a second registration).
    bool openFile(const QString& path);

    // The --lsp door: the demo's shape byte-for-byte (create/destroy the
    // hub, attach/detach through the session, sweep the view popups on
    // the way out). OFF by DEFAULT is load-bearing: tst_ide drives this
    // window offscreen and must never spawn a real server.
    void setLspEnabled(bool enabled);
    bool lspEnabled() const { return hub_ != nullptr; }

    // The editor-tab reads (tst_ide's driving surface, the demo's).
    int editorCount() const;
    CodeEditor* editorAt(int index) const;
    CodeEditor* currentEditor() const;
    void closeTabAt(int index); // the session drop + deleteLater (demo law)

    // The Find-in-Files doors (the panel IS the driver; the tst calls
    // these same doors). An empty project root is an honest refusal in
    // the summary, never a silent zero.
    void runSearch(const QString& text, bool caseSensitive = false,
                   bool wholeWord = false, bool isRegex = false);
    void cancelSearch();
    bool searchRunning() const;
    // Flattened result rows (file-major, one row per MATCHED LINE):
    int searchResultRowCount() const;
    // The row model tst_ide pins (index-aligned with the tree leaves and
    // activateSearchResult): the row's file (engine-absolute path) and
    // the 0-based engine line the caret reveal rides. Out-of-range row:
    // empty file / -1 line.
    QString searchResultFileAt(int row) const;
    int searchResultLineAt(int row) const;
    // Opens (or focuses) the row's file and moves the caret+selection to
    // the line's FIRST span (reveal through public view doors:
    // setSelection). False on a failed open (the file vanished mid-walk).
    bool activateSearchResult(int row);
    // The last finished()/refusal summary, Russian UX (the status bar
    // shows the SAME string — tst reads this door, the user reads the
    // bar; one truth, two faces).
    QString searchSummary() const { return searchSummary_; }

    // ---- car I2: quick open (Ctrl+P) + command palette (Ctrl+Shift+P) ----
    // The overlay doors are the driving surface: tst_ide drives the SAME
    // doors the chords and the popups call (the no-exec law rides — the
    // popups are plain child widgets, never modal dialogs).
    void showQuickOpen();
    void showCommandPalette();
    void closeOverlay();
    bool quickOpenVisible() const;
    bool commandPaletteVisible() const;

    // Quick-open ranking (the FROZEN law, PLAN): needle empty → the
    // pool's WALK order; else fuzzy score DESC, ties by the displayed
    // (project-relative) path CODEPOINT ASC — a total order, never a
    // vibe. Capped at kQuickOpenMaxRows rows; truncation SHOWS in the
    // summary (the skip-honesty law). The pool rides the search engine's
    // LIST face (startList — LEDGER catch #2), rebuilt at every
    // openProject; a still-filling pool is a VISIBLE face, never hidden.
    static constexpr int kQuickOpenMaxRows = 50;
    void typeQuickOpenQuery(const QString& text); // re-ranks (the door
                                                  // behind the popup's
                                                  // textChanged)
    int quickOpenRowCount() const;
    QString quickOpenPathAt(int row) const; // the ABSOLUTE path (rows
                                            // DISPLAY project-relative)
    bool activateQuickOpen(int row); // openFile + close the popup ON
                                     // SUCCESS; a false keeps the popup
                                     // open (the openFile truth rides
                                     // the status bar)
    QString quickOpenSummary() const;

    // The window's OWN command registry (the palette's backend — the
    // built-ins land in the ctor). Rows are the FILTERED view: fuzzy
    // over labels, score DESC, ties by REGISTRY ORDER (the tie law).
    void typePaletteQuery(const QString& text); // the popup channel's
                                                // door twin
    int commandRowCount() const;
    QString commandLabelAt(int row) const; // out-of-range: empty
    bool runPaletteCommand(int row);       // close-then-run; false =
                                           // the row never ran

    // The file-pool truth (tst waits on the walk; the popup reads the
    // SAME state the user sees).
    int projectFileCount() const;
    bool projectFilesWalking() const;

    // ---- car I3: LSP depth — jumps, references, rename, code actions,
    //      formatting, and the outline dock ---------------------------------
    // All SIX features ride the ten EXISTING feature doors (raw-Reply
    // FeatureHandler callbacks, C++-only by design) and the EXISTING
    // apply* applier doors — zero new glue surface (PLAN's opening
    // record). The programmatic doors below are the DRIVING SURFACE:
    // chords and palette commands are their twins, and tst_ide pins
    // them END-TO-END through a fakeserver spawn — the owner-granted
    // fakeserver-class law (2026-11-28): a CI-built deterministic far
    // side, $<TARGET_FILE:fakeserver>, the dapfake class tst_demo's
    // --dap slot already rides. The clangd/PATH law STANDS: no slot
    // without an explicit spec pin opens a claimed language.
    //
    // THE TEST ARM DOOR: install a pinned SERVER SPEC and arm the hub
    // over it. setLspEnabled alone cannot drive this window's feature
    // faces offscreen (the built-in spec claims PATH clangd, which
    // tst_ide never spawns) — an explicit program path is the legal
    // dapfake-class shape. Callers who want the built-in matrix still
    // use setLspEnabled.
    // THE SPEC NAME IS LOAD-BEARING: it picks the hub's spec slot. Passing
    // a BUILT-IN's name (clangd) REPLACES that claim whole — the only
    // deterministic way to arm a specific program over c/cpp, since the
    // built-in clangd spec already claims them and PATH clangd is never a
    // guarantee (never spawned here). A NEW name ADDS a spec, and a new
    // name's languages only reach documents no other spec claims.
    void armLspServer(const QString& specName, const QString& program,
                      const QStringList& args, const QStringList& languages);

    // Every request door's bool: was the request SENT (gate passed).
    // Refusals fire the handler SYNCHRONOUSLY (featureGate's local
    // route), so after a false the truth is ALREADY in the matching
    // summary face — the same synchronous-refusal law the search engine
    // froze at its door. Async answers ride the summary/row faces.

    // Definition jump (F12): Location[]/LocationLink[]/single decoded;
    // the FIRST answer is the target; open-or-focus rides the one-tab
    // law; the reveal SELECTS the target token with the CARET at its
    // START (the jump's reveal face — the panel's caret-at-END face
    // belongs to the panel; both pinnable through setSelection, PLAN).
    bool jumpToDefinition();
    QString jumpSummary() const;

    // References (Shift+F12) land in the SAME results panel — its second
    // producer (the row currency is the engine's own FileMatch; the
    // panel's read/activate doors are UNTOUCHED — I1's flattening paid).
    // includeDeclaration is TRUE in v1 always (the panel's law: the
    // declaration is a reference too). Rows ride the SERVER's answer
    // order, grouped per file in FIRST-APPEARANCE order (the walk-order
    // honesty the panel already pins for search).
    bool findReferences();
    // The panel's producer truth: "" (empty/idle), "search", "references".
    QString resultsProducer() const { return resultsProducer_; }

    // Rename (F2): the overlay prefills the selection word, else the
    // word at the caret (the app's own math over CodeDocument::lineText
    // — no new view door). commitRename is the Enter twin: requestRename
    // then applyReplyEdit (ONE undo group per document, the applier's
    // frozen law). A NULL answer is the server's own "cannot rename"
    // face; refusals and applier verdicts ride renameSummary VERBATIM.
    // Success CLOSES the overlay; a false-answer/failure keeps it open
    // (the quick-open failure face's sibling: the next name may yet be
    // the truth).
    bool openRenameOverlay(); // false + truth when no live document/hub
    bool renameOverlayVisible() const;
    QString renamePrefill() const; // the prefill face (the app's own
                                   // word/selection math, pinned not vibes)
    void commitRename(const QString& newName);
    QString renameSummary() const;

    // Code actions (Ctrl+.): the hub sends the zero-width range AT the
    // caret plus this document's CURRENT diagnostics in context (the
    // door's frozen shape). Rows are the answer's order, labels the
    // items' titles; run applies via applyCodeAction (its edit, or the
    // WorkspaceEdit hidden in command.arguments[0]). A row that is
    // PURELY a workspace command (no edit anywhere) is honest VISIBLE
    // and refuses through the applier's reasons — v1 never executes
    // arbitrary server commands (PLAN's v1 stance).
    bool showCodeActions();
    int codeActionRowCount() const;
    QString codeActionLabelAt(int row) const; // out-of-range: empty
    bool runCodeAction(int row);
    QString codeActionsSummary() const;

    // Whole-document formatting (Shift+Alt+F): requestFormatting →
    // applyReplyEdit. The summary carries the ApplyReport truth.
    bool formatDocument();
    QString formatSummary() const;

    // The outline dock (the tenth door's consumer): reads FLATTEN the
    // tree PREORDER; the dock's tree shows the nesting; row text is
    // "kind-label name · detail". Refresh triggers (the policy lives
    // HERE, in the outline — the door has NO hub auto-feed by design):
    // file opened, tab switched, a 600 ms document-change debounce
    // (an app-side timer over the current editor's edits), the dock's
    // own button, this manual door, and a palette command. A sloppy
    // server's FLAT answer is REBUILT from containerName (both wire
    // shapes decode — the flat transport leg is pinned far-side in
    // tst_lsp; the rebuild is pinned here). Refusals/errors CLEAR the
    // rows LOUD (the summary carries the machine reason verbatim) — a
    // stale tree never poses as current.
    void refreshOutline();
    bool outlineBusy() const { return outlineBusy_; }
    int outlineRowCount() const;
    // The nesting face (the flat-rebuild's proof): the row's ADOPTER row,
    // -1 for a root, -2 when the row is out of range.
    int outlineParentAt(int row) const;
    QString outlineTextAt(int row) const; // out-of-range: empty
    bool outlineActivate(int row); // reveal like the definition jump's
                                   // face; false = out-of-range or no
                                   // live document
    QString outlineSummary() const;

private:
    void addEditorTab(const QString& path, const QString& text);
    void updateStatusLabel();
    void onSearchFinished(int scanned, int matchedFiles, int totalMatches,
                          int skipped, const QString& stopReason);
    void appendResultRows(const qmn::search::FileMatch& match);

    // car I2 internals:
    void registerBuiltInCommands();           // the ctor registry
    void closeActiveTabCommand();             // chord + palette share it
    void kickPoolWalk(const QString& root);   // the LIST-face pool refresh
    void onPoolFileListed(const QString& filePath);
    void onPoolFinished(int scanned, int matched, int total, int skipped,
                        const QString& stopReason);
    void rankQuickOpen(const QString& needle);
    void rankPalette(const QString& needle);
    void showOverlay(QWidget* popup, QLineEdit* edit);
    QWidget* makeOverlay(QLineEdit*& editOut, QListWidget*& listOut,
                         const QString& placeholder);

    struct Command { // the registry record (app-local — std::function is
                     // FINE here: nothing of this crosses a public door)
        QString id;
        QString label;
        QString shortcutText; // DISPLAY hint only; the QShortcut itself
                              // is the binding
        std::function<void()> run;
    };

    // car I3 internals (file-local decode helpers live in the .cpp; NO
    // protocol/lsp type ever appears in THIS header — the I0 fwd-decl
    // law rides; PositionConverter is used inside the .cpp only):
    struct JumpTarget { // MODEL frame (line/col, UTF-16 units) — decoded
                        // through PositionConverter against the SYNCER's
                        // mirror (the text the server saw), then applied
                        // to the CURRENT text with the converter's own
                        // total clamping (an edit racing the reply drifts
                        // a reveal, it never corrupts — PLAN)
        QString path;
        int startLine = 0, startCol = 0, endLine = 0, endCol = 0;
    };
    struct OutlineNode { // the outline's flat record (both wire shapes
                         // decode INTO this; the tree widget mirrors it)
        QString name, detail, kindLabel;
        int startLine = 0, startCol = 0, endLine = 0, endCol = 0; // the SELECTION range
        QVector<int> childRows; // indices into outlineRows_
    };
    bool revealJumpTarget(const JumpTarget& target, bool caretAtStart); // open-or-focus +
                                                                       // select
    static QString wordAtColumn(const QString& line, int col); // prefill math
    void decodeOutline(const QString& mirrorFrame, const QString& wireEncoding,
                       const QJsonValue& result); // tree or flat->rebuild
    void rebuildOutlineTree();
    void showActionsOverlay(); // the Ctrl+. list popup (its own show face —
                               // no line edit to focus)
    void clearOutlineRows(const QString& why); // a LOUD clear (the refusal
                                               // face: a stale tree never
                                               // poses as current)
    void buildOutlineDock();
    void showResultsFor(const QVector<qmn::search::FileMatch>& grouped,
                        const QString& producer, const QString& summary);
    QVector<JumpTarget> decodeJumpTargets(const QJsonValue& result,
                                          const QString& mirrorFrame,
                                          const QString& wireEncoding) const;
    // THE SHARED LOCATION DECODE (definition + references + the outline all
    // read the SAME wire Location/DocumentSymbol shapes through these):
    // wire (line, character) -> the MODEL frame through PositionConverter
    // against the text the SERVER holds, then clamped into the CURRENT text.
    QVector<qmn::search::FileMatch> groupReferences(
        const QJsonValue& result, const QString& mirrorFrame,
        const QString& wireEncoding) const;
    // The six answer sinks. PLAIN Qt params by law: no protocol/lsp type
    // ever enters this header (the I0 fwd-decl law) — the FeatureHandler
    // lambdas in the .cpp unpack RpcClient::Reply into these.
    void onDefinitionAnswer(const QJsonValue& result, bool isError, int code,
                            const QString& message,
                            const QString& mirrorFrame,
                            const QString& wireEncoding);
    void onReferencesAnswer(const QJsonValue& result, bool isError, int code,
                            const QString& message,
                            const QString& mirrorFrame,
                            const QString& wireEncoding);
    void onRenameAnswer(const QJsonValue& result, bool isError, int code,
                        const QString& message, const QString& docUri);
    void onCodeActionsAnswer(const QJsonValue& result, bool isError, int code,
                             const QString& message);
    void onFormatAnswer(const QJsonValue& result, bool isError, int code,
                        const QString& message, const QString& docUri);
    void onOutlineAnswer(int generation, const QJsonValue& result,
                         bool isError, int code, const QString& message,
                         const QString& mirrorFrame,
                         const QString& wireEncoding);

    // car I3 state. THE ORDER BELOW IS LOAD-BEARING: these land BEFORE
    // the hub's unique_ptr so member destruction (reverse order) kills
    // the hub — and with it any in-flight FeatureHandler — WHILE these
    // are still alive: a handler invoked inside ~LspHub touches only
    // live state (the car E teardown-safety posture, spelled here).
    QWidget* renamePop_ = nullptr;   // the F2 overlay (edit in, list
    QLineEdit* renameEdit_ = nullptr; //     parked unused)
    QWidget* actionsPop_ = nullptr;  // the Ctrl+. overlay (list in,
    QListWidget* actionsList_ = nullptr; //      edit parked unused)
    QDockWidget* outlineDock_ = nullptr;
    QTreeWidget* outlineTree_ = nullptr;
    QPushButton* outlineRefreshBtn_ = nullptr; // the MANUAL trigger
    QLabel* outlineSummaryLabel_ = nullptr;    // the dock's LOUD truth face
                                               // (the outlineSummary() door
                                               // reads the same string)
    QVector<JumpTarget> jumpTarget_;           // the last decoded list
    QString jumpSummary_;
    QString renameSummary_;
    QString formatSummary_;
    QString actionsSummary_;
    QString resultsProducer_;
    QVector<QJsonValue> codeActions_; // raw answer items (row-aligned)
    QVector<OutlineNode> outlineRows_;
    QVector<int> outlineFlat_;        // preorder row order (the READ order)
    QString outlineSummary_;
    bool outlineBusy_ = false;
    int outlineRetry_ = 0;              // the bounded still-starting wait
    int outlineGeneration_ = 0;       // retires SUPERSEDED outline pulls
    QTimer* outlineDebounce_ = nullptr; // app-side, 600 ms (the refresh
                                        // policy belongs to the outline)

    QTabWidget* tabs_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QTreeView* projectTree_ = nullptr;
    QFileSystemModel* fileModel_ = nullptr; // Qt-parented (the window owns
                                            // it; fwd-decl suffices in the
                                            // header)
    QTreeWidget* resultsTree_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QDockWidget* searchDock_ = nullptr;

    // The car I2 overlays (plain children; hidden until a chord/door
    // raises them — see the ranking law above the doors):
    QWidget* quickOpenPop_ = nullptr;
    QLineEdit* quickOpenEdit_ = nullptr;
    QListWidget* quickOpenList_ = nullptr;
    QWidget* palettePop_ = nullptr;
    QLineEdit* paletteEdit_ = nullptr;
    QListWidget* paletteList_ = nullptr;
    QVector<int> quickOpenRows_;  // pool indices, list-row-aligned
    QVector<int> paletteRows_;    // registry indices, list-row-aligned
    QString quickOpenSummary_;
    QVector<Command> commands_;

    // The quick-open file pool — the engine's LIST face (startList).
    std::unique_ptr<qmn::search::SearchSession> pool_;
    QStringList poolFiles_;   // absolute, walk order
    QStringList poolDisplay_; // project-relative display strings (the
                              // fuzzy haystack rides THIS — what the
                              // user reads is what the scorer scores)
    bool poolWalking_ = false;
    QString poolPendingRoot_; // a restart requested mid-walk (openProject
                              // during a running walk)

    std::unique_ptr<qmn::glue::ProjectSession> session_;
    std::unique_ptr<qmn::glue::LspHub> hub_;
    std::unique_ptr<qmn::search::SearchSession> search_;

    QString projectRoot_;
    QString searchSummary_;
    // The flattened (fileIndex, lineIndex-in-that-file) per result row,
    // index-aligned with resultsTree_ leaves and activateSearchResult:
    QVector<QPair<int, int>> resultRows_;
    QVector<qmn::search::FileMatch> results_; // the FileMatch set the
                                              // rows index
};

} // namespace qmn::app
