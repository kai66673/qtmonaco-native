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
#include <QElapsedTimer> // car I4: the run's own clock (a value member — the
                         // elapsed face is a door, and wall-clock NEVER rides
                         // a test pin)
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
class QProcess; // car I4: the task runner (a raw Qt-PARENTED pointer — the
                // window owns the child, and the dtor/closeEvent kill it by
                // hand BEFORE the widgets go away; no QProcess type ever
                // crosses a public door)

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
    // The row's OWN text, producer-governed (car I4: the panel's third
    // producer rides the same currency and the same flattening): the
    // engine's PREVIEW under "search", the line's preview under
    // "references", the compiler's MESSAGE (code + text) under "problems".
    // Out-of-range row: empty. The search records stay untouched — this
    // reads the LineMatch's own text field, no new field anywhere.
    QString searchResultTextAt(int row) const;
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
    // car I4's picker twin (the popup's own textChanged channel — the door and
    // the keystroke share the slot):
    void typeTaskPickerQuery(const QString& text);
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

    // ---- car I4: tasks (.qmn/tasks.json) + the problem matcher ----------
    // THE CONFIG DOOR IS THE FILE: openProject() reads <root>/.qmn/tasks.json
    // (the lsp.json fold's FILE-LAYOUT precedent — the shelf owns no task
    // door; app code reads its own file, PLAN's opening record). The v1
    // schema is declared here and is NOT VS Code's:
    //   {"tasks":[{"label","command","args"?,"cwd"?,"problemMatcher"?,
    //              "default"?}]}
    // ${WORKSPACE_ROOT} expands in command/args/cwd (the lsp.json spelling);
    // cwd defaults to the project root; problemMatcher defaults to "gcc"
    // ("none" names nothing); "default": true marks the Ctrl+Shift+B target
    // and MORE THAN ONE default is a loud config refusal, never a silent
    // first-wins. Declared deviations: no group/type/dependsOn/presentation/
    // env/runOptions, and a matcher is NAMED, never configured inline.
    // THE REFUSAL LAWS (frozen at the door): unknown KEYS are IGNORED
    // (forwards-compatible); an unknown MATCHER NAME is refused (a silent
    // gcc fallback would invent problems from a language it does not speak);
    // malformed JSON, a missing label/command, a non-list args, a duplicate
    // label = loud refusals naming the file and the index. A refused config
    // reads as NOTHING (never a half-registry).
    bool tasksConfigured() const { return !tasks_.isEmpty(); }
    QString taskConfigSummary() const { return taskConfigSummary_; } // "" = clean
    int taskCount() const { return tasks_.size(); }                 // config order
    QString taskLabelAt(int index) const;    // out-of-range: empty
    QString taskMatcherAt(int index) const;  // the matcher's NAME
    bool taskIsDefaultAt(int index) const;
    QString taskWorkingDirAt(int index) const; // the EXPANDED cwd
    // The config FILE as a tab (the dogfood face: the IDE opens the file whose
    // truth it just read). False + the openFile truth on the bar (no project,
    // or the file is not there — the app never CREATES a config file in v1).
    bool openTaskConfig();

    // THE RUNNER — QProcess in the app (the app layer is where a process
    // belongs) and NEVER a terminal (the owner's no-terminal decision made
    // concrete): one-shot, run-to-completion, NO stdin (a task that waits
    // for input hangs, and STOP is its answer — the law, not a discovery),
    // no shell, no PTY (`command` is EXECUTED, not run through `sh -c`).
    // stdout+stderr arrive MERGED IN ARRIVAL ORDER (QProcess's own
    // MergedChannels — the documented answer for cross-stream order).
    // NEVER auto-run: a run starts only on an explicit act (chord, picker
    // row, palette command, the CLI door). ONE run at a time — a second ask
    // is a loud refusal, never a queue. A run NEVER saves buffers. A task
    // DIES at a project switch and at window close (the honest face).
    // The bool doors: was the run STARTED; false + the truth ALREADY in
    // taskRunSummary() (the synchronous-refusal law the search engine and
    // I3's feature doors both froze).
    bool runTask(const QString& label); // EXACT label match
    bool runDefaultTask();              // the "default": true task
    bool stopTask();                    // terminate, then kill past the grace
    bool taskRunning() const { return taskRunning_; }
    // The exit face. Sentinels are NEGATIVE and distinct from any child code:
    static constexpr int kTaskExitPending = -1000;     // never ran, or running
    static constexpr int kTaskExitCrashed = -1001;     // signal death
    static constexpr int kTaskExitFailedToStart = -1002;
    int taskExitCode() const { return taskExitCode_; }
    QString taskRunLabel() const { return taskRunLabel_; }   // "" = never ran
    QString taskRunSummary() const { return taskRunSummary_; } // Russian truth,
                                                               // status-bar twin
    qint64 taskElapsedMs() const { return taskElapsedMs_; }  // -1 while running

    // THE OUTPUT PANE — the stub's dogfood bet, ON (PLAN's opening audit):
    // the read-only surface EXISTS and is the right shape. CodeEditor's
    // M19 readOnly lock refuses EVERY USER edit gesture (typing/IME,
    // paste/cut/drop, undo/redo, the Ctrl word-delete, find/replace ACTIONS,
    // Tab indent) while the programmatic setPlainText door is NOT locked —
    // so the log still lands — and copy() is NOT locked either: a locked
    // pane stays copyable, which is the whole point of a build log.
    // THE APPEND DOOR IS NOT TAKEN (the ledger stays empty at opening): the
    // pane is a whole-text REPLACE of the log TAIL on a paced flush, newest
    // kOutputMaxLines lines kept, and the hidden count is a VISIBLE face (the
    // truncation-shows law I2 froze). Whether the replace costs too much is
    // ANSWERED BY THE CLOSE RITUAL'S MEASUREMENT, not by taste — if it hurts,
    // the ledger entry goes in before any workaround.
    static constexpr int kOutputMaxLines = 2000; // newest kept; the rest is HIDDEN
    // THE TWO PROBLEM CAPS, pinned as FACES (the skip-honesty law I2 froze for
    // quick-open, riding the panel and the feed now): more rows than a human
    // reads is a panel that LIES about its own size, so the excess is COUNTED and
    // SHOWN (the panel summary's "\u0435\u0449\u0451 N \u043d\u0435 \u043f\u043e\u043a\u0430\u0437\u0430\u043d\u044b").
    // The DIAGNOSTICS feed is NOT capped by the row cap — the gutter truth stays
    // whole — and kProblemFeedMax only keeps a pathological 200 000-line log from
    // eating the decoration table; that cap's face is the run summary's
    // "(\u043c\u0430\u0442\u0447\u0435\u0440 \u043f\u0440\u043e\u043f\u0443\u0441\u0442\u0438\u043b N)".
    static constexpr int kProblemMaxRows = 500;
    static constexpr int kProblemFeedMax = 2000;
    QString taskOutput() const;                  // the pane's VISIBLE text
    int taskOutputHiddenLines() const { return taskHiddenLines_; }
    int taskOutputLineCount() const; // the pane's visible line count
    bool taskOutputVisible() const;
    // The pane's WIDGET (the driving-surface precedent this window already
    // sets with editorAt(): tst_ide pins the M19 lock half of the dogfood bet
    // — a user edit gesture refused, the pane still copyable — and the tail-
    // follow arithmetic, all through the real editor, no separate shadow face).
    CodeEditor* outputPane() const { return outputEditor_; }
    void showTaskOutput(bool visible = true);
    void clearTaskOutput(); // also DROPS the pane's problem rows? NO — the
                            // output and the problems are two truths: this
                            // clears the pane only (clearTaskProblems() is
                            // the other one)

    // The task picker (Ctrl+Shift+R) — the THIRD popup riding the I2 overlay
    // law whole (child-scoped Esc, close-then-run, a retrigger never wiping
    // the query, empty needle = registry order, fuzzy over LABELS with ties
    // by CONFIG order). qmn::fuzzy's third user (the I2 record's "two users"
    // note gets its addendum at this close).
    void showTaskPicker();
    bool taskPickerVisible() const;
    int taskPickerRowCount() const;
    QString taskPickerLabelAt(int row) const; // the config label
    QString taskPickerSummary() const;       // the popup's own truth face (the
                                             // quick-open summary's sibling: a
                                             // refusal LOUD, never a silent
                                             // zero-row list)
    bool activateTask(int row);               // close-then-run

    // THE PROBLEM MATCHER — the panel's THIRD producer (search I1,
    // references I3, problems I4) over the SAME qmn::search currency:
    // FileMatch/LineMatch/MatchSpan, records NOT widened (no severity and no
    // message field — the row's TEXT rides the line's own text field: a
    // preview for search and references, the compiler's MESSAGE for
    // problems; the meaning is producer-governed, see searchResultTextAt).
    // Two matchers + none: "gcc" (the gcc/clang family: `path:line:col:
    // error:|warning:|note:|fatal error: message`, the no-column shape, an
    // optional `[-Wflag]` code tail; the `In file included from` header line,
    // the caret line and `collect2: error: ld returned 1 exit status` are
    // NOISE and never become rows) and "msvc" (`path(line,col): error C#### :
    // message`). NEVER a shell glob, NEVER an invented file from a non-path
    // token. An ABSOLUTE path is honored; a RELATIVE one resolves against the
    // TASK's cwd. A path that does not exist is an honest row (a compiler may
    // name a deleted file) whose REVEAL refuses loudly — the row is the
    // answer, the reveal is a separate truth. Columns are 1-based on the
    // wire, 0-based here; a missing column makes the span one cell at 0; the
    // span is CLAMPED like every other panel feed. Severity maps to the
    // view's enum (error/fatal error → Error, warning → Warning, note →
    // Information) and reaches the gutter through the M5 door with source
    // "qmn-tasks" (the DapHub merge mechanism reused whole: the app merges
    // OVER what its tag does not own, and clearing restores exactly the
    // foreign rows).
    void clearTaskProblems();
    int problemCount() const;                  // the panel's row count ONLY
                                               // while the problems producer
                                               // owns the panel; -1 otherwise
    QString problemSeverityAt(int row) const;  // "error"/"warning"/
                                               // "information"/"hint"; "" for
                                               // another producer or a bad row
    bool problemFileExistsAt(int row) const;   // the phantom-row face
    int problemFeedCount() const;              // the run's whole problem count
                                               // (panel rows may be capped)

protected:
    // car I4: a running task DIES at window close (the honest face of the
    // runner's law — no orphan child outliving the pane that printed it).
    void closeEvent(QCloseEvent* event) override;

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

    // car I4 internals (the matcher stays APP CODE — one user, and its test
    // surface already rides tst_ide's link of qmn_ide_app through the
    // window's own doors: the promotion question is answered at the door,
    // not deferred, PLAN's opening record).
    struct TaskDef { // the config record (app-local; nothing crosses a door)
        QString label, command, cwd; // cwd is EXPANDED at read time
        QStringList args;
        QString matcher; // "gcc" | "msvc" | "none"
        bool isDefault = false;
    };
    struct ProblemRow { // the matcher's record (app-local — the SEARCH
                        // records are deliberately NOT widened)
        QString filePath; // absolute, resolved against the task's cwd
        int line = 0, startCol = 0, endCol = 0; // 0-based model frame
        QString severity; // "error" | "warning" | "information" (the door's
                          // plain-Qt face; the view's enum is built from it)
        QString message;  // the compiler's text, the flag/code face folded in
        QString matcher;  // which matcher produced it
        bool fileExists = false; // the phantom face
    };
    bool readTasksConfig(); // openProject's task half (the file IS the door)
    bool startTask(const TaskDef& def); // the ONE start path (every ask face
                                        // funnels here: chord, picker, palette,
                                        // CLI — the never-auto-run law)
    bool killRunningTask(); // the project-switch / window-close death: returns
                            // TRUE when a LIVE run was ended HERE (the caller's
                            // face question at openProject — see the kill's body)
    QString expandRoots(const QString& text) const; // ${WORKSPACE_ROOT} (the
                                                    // lsp.json spelling; the
                                                    // hub's expand() is
                                                    // PRIVATE, so the runner
                                                    // has its own — TWO homes
                                                    // today, recorded as a
                                                    // third-home promotion
                                                    // candidate, NOT refactored
                                                    // mid-car)
    static bool parseGccLine(const QString& line, ProblemRow* out);
    static bool parseMsvcLine(const QString& line, ProblemRow* out);
    void ingestTaskLine(const QString& line); // the matcher rides ARRIVAL — a
                                              // tail-capped pane never hides a
                                              // problem from the feed
    void resolveProblemPath(ProblemRow* row) const; // relative → the TASK's cwd
    void consumeTaskBytes(); // the pending-bytes line split (UTF-8 safe)
    void appendTaskLine(const QString& line); // log tail + the paced flush
    void flushTaskOutput(); // the whole-text replace + the tail follow
    void finishTask(int exitCode, bool crashed, const QString& startError);
    void buildProblemFeed(); // panel rows + the diagnostics merge, one pass
    void mergeTaskDiagnostics(CodeEditor* editor, const QString& path); // the
                                                                        // DapHub
                                                                        // merge
                                                                        // idiom
    void clearProblemFeed(const QString& why); // rows + every tagged feed
    void registerTaskCommands(); // the registry APPEND (the palette tie law)
    void rankTaskPicker(const QString& needle);

    // car I4 state. The runner's process is Qt-parented (the window owns it)
    // and killed by hand in the dtor/closeEvent BEFORE the widgets go away.
    QProcess* taskProc_ = nullptr;
    bool taskRunning_ = false;
    bool taskStopRequested_ = false; // the terminate→kill escalation rides it
    int taskExitCode_ = kTaskExitPending;
    QString taskRunLabel_, taskRunSummary_;
    qint64 taskElapsedMs_ = -1;
    QElapsedTimer taskClock_;
    QTimer* taskStopTimer_ = nullptr; // the terminate→kill grace
    QByteArray taskPendingBytes_;     // an undecoded partial line
    QStringList taskLog_;             // the log TAIL (newest kOutputMaxLines)
    int taskHiddenLines_ = 0;
    QTimer* taskFlushTimer_ = nullptr; // the paced pane replace
    int taskRunGeneration_ = 0;        // retires a dead run's stragglers

    // The problems feed (the panel's third producer). `problems_` is the
    // WHOLE set (a panel row cap never hides a problem from the diagnostics
    // feed); `resultSeverity_` is the panel-row-parallel severity face.
    QVector<TaskDef> tasks_;       // the config registry (CONFIG order = the
                                   // picker's order; a refused config reads as
                                   // NOTHING — never a half-registry)
    QString taskConfigSummary_;    // "" clean/no file; the loud refusal VERBATIM
    QString taskMatcher_, taskCwd_;// the RUNNING run's matcher + cwd (the
                                   // relative-path frame of every row)
    QVector<ProblemRow> problems_;
    QString problemTaskLabel_;     // who produced the current feed
    int problemSkipped_ = 0;       // the row cap's honest face
    int problemFeedSkipped_ = 0;   // the FEED cap's honest face
    QVector<int> problemRowMap_; // PANEL row → problems_ index, only filled
                                 // under the "problems" producer (the severity
                                 // and the phantom face read through it)

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
    // car I4's output dock (the dogfood pane: a CodeEditor under the M19
    // readOnly lock + its own LOUD label face, the outline precedent — the
    // pane never poses as a document tab):
    QDockWidget* outputDock_ = nullptr;
    CodeEditor* outputEditor_ = nullptr;
    QLabel* outputSummaryLabel_ = nullptr;
    // car I4's task picker overlay (the third popup on the I2 law):
    QWidget* taskPickerPop_ = nullptr;
    QLineEdit* taskPickerEdit_ = nullptr;
    QListWidget* taskPickerList_ = nullptr;
    QVector<int> taskPickerRows_; // task indices, list-row-aligned
    QString taskPickerSummary_;   // the popup's truth face (the I2
                                  // quickOpenSummary_ precedent)

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
