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
class QTabWidget;
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
