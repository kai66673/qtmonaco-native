#pragma once

// qmn-demo main window: QTabWidget central area; "Файл -> Открыть…" adds
// a tab per picked file (content via setPlainText, grammar guessed from
// the suffix like the spike); "Настройки" opens the settings dialog, and
// on accept its values go to EVERY open editor and become the template
// for future tabs. The programmatic doors (openPath, applyEditorSettings)
// are the CLI path AND the test surface — they never spawn modal dialogs.

#include <QMainWindow>

#include <memory>

#include "EditorSettings.hpp"

class QLabel;
class QTabWidget;

namespace qmn::glue {
// Both glue types are kept OUT of this header's include set so tst_demo
// never learns protocol/lsp/glue types — it only links the static lib
// that does (AGENTS: app links the glue TARGET, no glue type crosses
// this header beyond these forward-decls).
class LspHub;         // the G1 shelf's hub (the --lsp door)
class ProjectSession; // G2: the demo's document registry — the tabs become
                      // its presentation
class DapHub;         // the D3 shelf hub (the --dap door, car 4)
} // namespace qmn::glue

namespace qmn::app {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Reads the file (UTF-8, CRLF/CR normalized to LF) and adds a tab.
    // False + a status-bar message on failure — deliberately NO modal
    // warning, so the harness stays drivable offscreen.
    bool openPath(const QString& path);

    // The --lsp door (the M8 close-train popup-trigger demo path): creates
    // (or destroys) the LspHub and attaches (detaches) it to the ALWAYS-on
    // document session. OFF by DEFAULT is load-bearing: tst_demo drives
    // this window offscreen and must never spawn a real server. With a
    // hub attached, every tab whose language a spec claims goes through
    // the session's open (hub openDocument + popup triggers); languages
    // no spec claims simply stay plain-editor tabs.
    void setLspEnabled(bool enabled);
    bool lspEnabled() const { return hub_ != nullptr; }

    // The --dap door (car 4, D4): GENERIC by design — the adapter PROGRAM
    // and its argv come from the CLI (no built-in matrix: an adapter is
    // not a language server). Arming creates the DapHub, attaches it to
    // the ALWAYS-on document session and wires the status-bar truths
    // (session start/end, debugStopped/continued, launchRejected,
    // sourceUnresolved); the session auto-starts at the FIRST openPath —
    // a debug session needs a registered document, and store-BEFORE-ready
    // breakpoints are the hub's legal order. launchProgram empty = the
    // first opened tab's file becomes the launch `program`. Arming with an
    // EMPTY adapterProgram = OFF: ~DapHub pumps the disconnect barrier.
    // OFF by DEFAULT stays load-bearing for every tst_demo slot except
    // the ONE tripwire (which spawns dapfake — CI-built: the recorded,
    // reasoned difference from the --lsp door's no-spawn rule).
    void setDapAdapter(const QString& adapterProgram,
                       const QStringList& adapterArgs = {},
                       const QString& launchProgram = QString());
    bool dapEnabled() const { return dapHub_ != nullptr; }

    // Stores the template AND pushes it to every open editor (the
    // dialog-accept door). Getter = the current template.
    void applyEditorSettings(const EditorSettings& settings);
    const EditorSettings& editorSettings() const { return settings_; }

    QTabWidget* tabWidget() const { return tabs_; }
    int editorCount() const;
    CodeEditor* editorAt(int index) const; // nullptr when out of range
    CodeEditor* currentEditor() const;     // nullptr when no tab is active
    // The close-button door (tabCloseRequested rides it); public so the
    // offscreen tests drive the same path the tab bar's buttons take.
    void closeTabAt(int index);

private slots:
    void chooseFiles();       // the Файл->Открыть… menu slot
    void editSettings();      // the Настройки menu slot
    void updateStatusLabel(); // caret/selection readout of the current tab

private:
    void addEditorTab(const QString& path, const QString& text);
    void setCurrentTabTitle();
    void maybeStartDapSession(CodeEditor* editor, const QString& absolutePath);

    QTabWidget* tabs_ = nullptr;
    QLabel* statusLabel_ = nullptr; // permanent status widget
    EditorSettings settings_;       // template for new tabs
    QString lastDir_;               // dialog start dir once a file was picked
    std::unique_ptr<qmn::glue::ProjectSession> session_; // ALWAYS on: the
        // document registry + language table + config fold (G2); the tabs
        // mirror it, the --lsp door only ATTACHES a hub to it
    std::unique_ptr<qmn::glue::LspHub> hub_; // the --lsp door's hub; null =
                                    // OFF (the DEFAULT: tst_demo never
                                    // spawns servers)
    std::unique_ptr<qmn::glue::DapHub> dapHub_; // the --dap door's hub;
                                    // null = OFF (the DEFAULT; its dtor
                                    // pumps the disconnect barrier)
    QString dapAdapter_;   // the --dap door's adapter program (the CLI's)
    QStringList dapAdapterArgs_; // its argv (repeatable --dap-arg)
    QString dapProgram_;   // the launch-program override (empty = the
                           // first opened tab's file)
    bool dapStarted_ = false; // one auto-start attempt per armed hub (the
                              // hub itself pins ONE session anyway)
};

} // namespace qmn::app
