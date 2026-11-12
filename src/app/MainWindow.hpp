#pragma once

// qmn-demo main window: QTabWidget central area; "Файл -> Открыть…" adds
// a tab per picked file (content via setPlainText, grammar guessed from
// the suffix like the spike); "Настройки" opens the settings dialog, and
// on accept its values go to EVERY open editor and become the template
// for future tabs. The programmatic doors (openPath, applyEditorSettings)
// are the CLI path AND the test surface — they never spawn modal dialogs.

#include <QMainWindow>

#include "EditorSettings.hpp"

class QLabel;
class QTabWidget;

namespace qmn::app {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Reads the file (UTF-8, CRLF/CR normalized to LF) and adds a tab.
    // False + a status-bar message on failure — deliberately NO modal
    // warning, so the harness stays drivable offscreen.
    bool openPath(const QString& path);

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
};

} // namespace qmn::app
