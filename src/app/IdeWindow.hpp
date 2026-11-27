#pragma once

// qmn-ide-demo main window (GATE 4 car I0, PLAN): the IDE skeleton —
// a project folder (QFileSystemModel tree dock), a tab row (ONE tab
// per path: a re-open FOCUSES, never duplicates), the Find-in-Files
// panel over the qmn::search engine (car I1), and the status-bar
// truth channel the tours/tests read. The window compiles into a
// static lib (qmn_ide_app) so tst_ide drives the EXACT doors the UI
// slots call, offscreen, without exec()ing anything (the qmn_demo_app
// precedent).
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
#include <QVector>

#include <memory>

#include "EditorSettings.hpp" // the app-namespace CodeEditor alias (the
                               // MainWindow.hpp pattern: EditorSettings
                               // owns the `using qmn::view::CodeEditor`)

class QLabel;
class QLineEdit;
class QTabWidget;
class QTreeWidget;
class QTreeView;
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

private:
    void addEditorTab(const QString& path, const QString& text);
    void updateStatusLabel();
    void onSearchFinished(int scanned, int matchedFiles, int totalMatches,
                          int skipped, const QString& stopReason);
    void appendResultRows(const qmn::search::FileMatch& match);

    QTabWidget* tabs_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QTreeView* projectTree_ = nullptr;
    QFileSystemModel* fileModel_ = nullptr; // Qt-parented (the window owns
                                            // it; fwd-decl suffices in the
                                            // header)
    QTreeWidget* resultsTree_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;

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
