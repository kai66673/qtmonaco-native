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

    // The S1-3 control doors (`--debug-continue` / `--debug-step` /
    // `--debug-stop`): the demo's face of the hub's typed control doors,
    // pure forwarding — the hub's own answers ride the ALREADY-wired
    // status signals (the pause string, "DAP: продолжено", the session end).
    // false = the door's refusal face + an honest status string: no hub,
    // or (continue/step) no Ready session with a known STOPPED thread.
    // debugStop prefers terminateDebugging() where the caps allow (the
    // terminate-before-disconnect demand, live-probe-pinned): the hub's
    // door falls back to the disconnect barrier and says so with false,
    // but the session ENDS either way and the sessionEnded string is the
    // real face — so this door refuses only when there was no session.
    bool debugContinue();
    bool debugStep(); // the everyday Step button = DAP `next`
    bool debugStop();

    // The S1-3 CLI ride (`--debug-step --debug-continue --debug-stop`):
    // a control door CALLED AT STARTUP always refuses (the session has
    // not even started), so the flags QUEUE presses into the session's
    // own event stream instead — the tour's walk (pause → step → pause →
    // continue → continued → stop) driven by the hub's signals, each
    // queued action consuming the next event it accepts ("stop" accepts
    // the continued when a continue rides before it, else a stop). Empty
    // list = clear; an undrained ride dies with its session.
    void queueDebugRide(const QStringList& actions);

    // The S1-1 diff door (`--diff A B`): THREE tabs — plain A, plain B,
    // and a third DIFF tab built through the editor's STRING door (its
    // own attached document IS the modified side, so caret/F7 nav/undo
    // speak modified), FOCUS ON THE DIFF TAB. The pointer door's
    // zero-view refusal makes it illegal here (every tab's document is
    // editor-attached, CodeEditor.cpp:2672), and the two plain tabs stay
    // untouched, editable witnesses of both sides. Language comes from
    // B's suffix through the session's table ("" = plain). False + a
    // status-bar message if either file fails to read (the openPath
    // route; a half-opened pair is repaired by closing nothing — the
    // failure just never builds the diff tab).
    bool openDiffPair(const QString& pathA, const QString& pathB);

    // The S1-2 grammar door (`--grammar`): load a grammar LIBRARY (the
    // view's static loadGrammar seam — the handle stays open forever),
    // validate+REGISTER it under `id` (empty id = the library stem: the
    // loader's own derivation — libfoo.so -> foo), and re-assert
    // setLanguage(id) on EVERY open tab already resolved to `language`
    // (empty = id). A registration does NOT light up old setLanguage
    // calls (the view's documented contract), so the re-assert IS the
    // live-rebind step; a failed load/refused library changes NOTHING in
    // the registry (registerGrammar validates at its own door) and rides
    // false + an exact status-bar refusal. symbolName empty = the
    // tree_sitter_<stem> derivation; queryPath names the .scm highlights
    // source (empty = parse-only, NO sidecar magic: the CLI flag and the
    // tour config name it explicitly). Fold node names stay OUT of the
    // door (fold chords on custom grammars are no consumer today).
    // Future tabs need no wiring here: the suffix->language table is the
    // SESSION's (G2) and setLanguageForSuffix below is its door.
    bool loadGrammarLibrary(const QString& path,
                            const QString& id = QString(),
                            const QString& language = QString(),
                            const QString& symbolName = QString(),
                            const QString& queryPath = QString());

    // The session's suffix->language table, one forwarding door (G2 said
    // the table is the session's — this is the DEMO's route to it): an
    // EMPTY id removes the mapping. The CLI --grammar suffix pairs and
    // the S1-2 tripwire's "future tabs" leg ride it.
    void setLanguageForSuffix(const QString& suffix, const QString& languageId);

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
    void chooseGrammarLibrary(); // the Файл->«Загрузить грамматику…» mouse
                                 // route (S1-2): picks ONE .so and rides the
                                 // door with defaults (stem id, derived
                                 // symbol, parse-only) — the programmatic
                                 // door stays the CLI/test path (the
                                 // no-modal rule: tst_demo never exec()s this)
    void editSettings();      // the Настройки menu slot
    void updateStatusLabel(); // caret/selection readout of the current tab

private:
    void addEditorTab(const QString& path, const QString& text);
    void setCurrentTabTitle();
    void maybeStartDapSession(CodeEditor* editor, const QString& absolutePath);
    void maybeFireDebugRide(const QString& eventName); // S1-3: the queued
                                                       // CLI presses, fired
                                                       // from the hub-signal
                                                       // lambdas

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
    QStringList debugRide_;   // S1-3: the queued CLI control presses
                              // (queueDebugRide), drained by the hub's
                              // stopped/continued stream
};

} // namespace qmn::app
