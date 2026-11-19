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

namespace qmn::app {

class LspHub; // app/glue/LspHub.hpp; kept OUT of this header's include set so
              // tst_demo never learns protocol/lsp types — it only links the
              // static lib that does (AGENTS: the hub is an app-side client)

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
    // (or destroys) the LspHub. OFF by DEFAULT is load-bearing: tst_demo
    // drives this window offscreen and must never spawn a real server. With
    // a hub, every c/cpp tab gets openDocument + setPopupTriggers(true);
    // languages no spec claims simply stay plain-editor tabs.
    void setLspEnabled(bool enabled);
    bool lspEnabled() const { return hub_ != nullptr; }

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

    QTabWidget* tabs_ = nullptr;
    QLabel* statusLabel_ = nullptr; // permanent status widget
    EditorSettings settings_;       // template for new tabs
    QString lastDir_;               // dialog start dir once a file was picked
    std::unique_ptr<LspHub> hub_;   // the --lsp door's hub; null = OFF (the
                                    // DEFAULT: tst_demo never spawns servers)
};

} // namespace qmn::app
