#include "MainWindow.hpp"

#include "glue/DapHub.hpp"
#include "glue/LspHub.hpp"
#include "glue/ProjectSession.hpp"
#include "EditorSettingsDialog.hpp"
#include "view/CodeEditor.hpp"

#include <QAction>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>

#include <memory>

namespace qmn::app {

using qmn::glue::LspHub; // the G1 shelf moved the hub namespace (qmn::app
                         // -> qmn::glue); the unqualified uses below stay

namespace {

QString readTextFile(const QString& path, QString* error)
{
    QFileInfo info(path);
    if (!info.isFile()) {
        *error = QStringLiteral("это не файл");
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return {};
    }
    // Demo loader: UTF-8 only, CRLF/CR normalized to LF (same stance as
    // the spike; encoding detection stays a future milestone).
    QString text = QString::fromUtf8(file.readAll());
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("qtmonaco-native \u2014 \u0434\u0435\u043c\u043e\u043d\u0441\u0442\u0440\u0430\u0446\u0438\u044f"));
    resize(1100, 700);

    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setDocumentMode(true);
    setCentralWidget(tabs_);

    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTabAt);
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        setCurrentTabTitle();
        updateStatusLabel();
    });

    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&Файл"));
    fileMenu->addAction(QStringLiteral("&Открыть\u2026"), QKeySequence::Open,
                        this, &MainWindow::chooseFiles);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Закрыть вкладку"), QKeySequence::Close,
                        this, [this] { closeTabAt(tabs_->currentIndex()); });

    auto* settingsMenu = menuBar()->addMenu(QStringLiteral("&Настройки"));
    settingsMenu->addAction(QStringLiteral("Параметры &редактора\u2026"),
                            QKeySequence(Qt::CTRL | Qt::Key_Comma),
                            this, &MainWindow::editSettings);

    statusLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(statusLabel_);
    updateStatusLabel();

    // G2: the document session exists whether or not --lsp ever turns on
    // (suffix table + registry + config fold are the demo's own needs;
    // PLAN: the tabs become the session's presentation).
    session_ = std::make_unique<qmn::glue::ProjectSession>();
}

// Defined out of line: the header forward-declares LspHub behind a
// unique_ptr, so the destruction point must see the complete type (and it
// also pins WHY the hub must die with the window — its registrations point
// at the tab editors).
MainWindow::~MainWindow() = default;

void MainWindow::setLspEnabled(bool enabled)
{
    if (enabled == (hub_ != nullptr))
        return;
    if (!enabled) {
        // ~LspHub is the honest teardown (didClose for every document, then
        // shutdown/exit per server); the popups are view-side, so sweep
        // them by hand — nobody is left to hide them.
        for (int i = 0; i < tabs_->count(); ++i) {
            if (CodeEditor* editor = editorAt(i)) {
                editor->hideCompletionList();
                editor->hideHover();
            }
        }
        session_->attachHub(nullptr); // detach BEFORE the hub dies (the
                                      // session's raw pointer goes null)
        hub_.reset();
        statusBar()->showMessage(QStringLiteral("LSP: серверы остановлены"));
        return;
    }
    hub_ = std::make_unique<LspHub>(); // built-in specs only; the session
                                       // forwards its workspace root and
                                       // REPLAYS a config it already read
    session_->attachHub(hub_.get());
    statusBar()->showMessage(QStringLiteral("LSP: включён (спеки из коробки)"));
}

void MainWindow::setDapAdapter(const QString& adapterProgram,
                               const QStringList& adapterArgs,
                               const QString& launchProgram)
{
    if (adapterProgram.isEmpty()) {
        if (!dapHub_)
            return;
        // The orderly stop FIRST (barrier + sessionEnded while the status
        // bar is still alive to say so); then ~DapHub is the honest
        // teardown (BOUNDED dtor pump — the M8 machine-4 lesson, paid
        // twice). No hand-sweep of decorations: unlike the LSP popups,
        // these are editor-side feeds the hub CLEARS ITSELF on the way out.
        dapHub_->stopDebugging();
        dapHub_.reset();
        dapStarted_ = false;
        dapProgram_.clear();
        dapAdapter_.clear();
        dapAdapterArgs_.clear();
        statusBar()->showMessage(QStringLiteral("DAP: адаптер остановлен"));
        return;
    }
    if (dapHub_) { // re-arm: the simple honest shape — drop, then build
        dapHub_->stopDebugging();
        dapHub_.reset();
        dapStarted_ = false;
    }
    dapAdapter_ = adapterProgram;
    dapAdapterArgs_ = adapterArgs;
    dapProgram_ = launchProgram;
    dapHub_ = std::make_unique<qmn::glue::DapHub>();
    dapHub_->attachProject(session_.get()); // the G2 uri map resolves every
                                            // DAP source (the hub NEVER reads disk)
    // EVERY answer route on the shelf is a SIGNAL — the demo tells the
    // status bar what the session says:
    connect(dapHub_.get(), &qmn::glue::DapHub::sessionStarted, this, [this] {
        statusBar()->showMessage(QStringLiteral("DAP: сессия запущена"));
        dapHub_->configurationDone(); // the app says: setup ended (the hub
                                      // skips it when the caps say false,
                                      // and defers it behind the setup
                                      // answers — the live-gdb rule)
    });
    connect(dapHub_.get(), &qmn::glue::DapHub::launchRejected, this,
            [this](const QString& message) {
                statusBar()->showMessage(QStringLiteral("DAP: пуск отклонён: %1")
                                             .arg(message));
            });
    connect(dapHub_.get(), &qmn::glue::DapHub::sessionEnded, this,
            [this](const QString& reason) {
                statusBar()->showMessage(QStringLiteral("DAP: сессия завершена (%1)")
                                             .arg(reason));
            });
    connect(dapHub_.get(), &qmn::glue::DapHub::debugStopped, this,
            [this](int threadId, const QString& reason) {
                statusBar()->showMessage(QStringLiteral("DAP: пауза в потоке %1 (%2)")
                                             .arg(threadId).arg(reason));
            });
    connect(dapHub_.get(), &qmn::glue::DapHub::debugContinued, this, [this] {
        statusBar()->showMessage(QStringLiteral("DAP: продолжено"));
    });
    connect(dapHub_.get(), &qmn::glue::DapHub::sourceUnresolved, this,
            [this](const QJsonObject& source, const QJsonObject&) {
                // v1 limitation, recorded: the demo has no door to OPEN a
                // file the ADAPTER names (the user does that through the
                // Файл menu — the NEXT feed resolves it via the session).
                statusBar()->showMessage(QStringLiteral("DAP: кадр вне открытых файлов: %1")
                                             .arg(source.value(QStringLiteral("path")).toString()));
            });
    statusBar()->showMessage(
        QStringLiteral("DAP: адаптер %1 — сессия начнётся с первой вкладкой")
            .arg(adapterProgram));
    if (CodeEditor* first = editorAt(0)) { // armed LATE (doors after the
        maybeStartDapSession(              // tabs exist too: the CLI arms
            first, QFileInfo(tabs_->tabBar()
                                 ->tabData(0)
                                 .toString())
                     .absoluteFilePath()); // before, but the door is a door
    }
}

bool MainWindow::openPath(const QString& path)
{
    QString error;
    const QString text = readTextFile(path, &error);
    if (!error.isEmpty()) {
        // Non-modal on purpose: a status-bar failure keeps the app (and
        // the offscreen test drive) free of blocking dialogs.
        statusBar()->showMessage(QStringLiteral("Не удалось открыть \u00ab%1\u00bb: %2")
                                     .arg(path, error));
        return false;
    }
    lastDir_ = QFileInfo(path).absolutePath();
    addEditorTab(path, text);
    return true;
}

void MainWindow::chooseFiles()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Открыть файл"), lastDir_,
        QStringLiteral("Все файлы (*)"));
    for (const QString& path : paths)
        openPath(path);
}

void MainWindow::editSettings()
{
    // Seed from what the user is looking at; fall back to the stored
    // template when no editor is open.
    EditorSettings initial = settings_;
    if (CodeEditor* current = currentEditor())
        initial = collectSettings(current);

    EditorSettingsDialog dialog(this, initial);
    if (dialog.exec() == QDialog::Accepted)
        applyEditorSettings(dialog.settings());
}

void MainWindow::applyEditorSettings(const EditorSettings& settings)
{
    settings_ = settings; // template for future tabs...
    for (int i = 0; i < tabs_->count(); ++i) {
        if (CodeEditor* editor = editorAt(i))
            applySettings(editor, settings_); // ...and every open editor now
    }
    updateStatusLabel();
}

int MainWindow::editorCount() const
{
    return tabs_->count(); // every tab IS a CodeEditor (addEditorTab only)
}

CodeEditor* MainWindow::editorAt(int index) const
{
    return qobject_cast<CodeEditor*>(tabs_->widget(index)); // nullptr past the end
}

CodeEditor* MainWindow::currentEditor() const
{
    return qobject_cast<CodeEditor*>(tabs_->currentWidget());
}

void MainWindow::addEditorTab(const QString& path, const QString& text)
{
    auto editor = std::make_unique<CodeEditor>();
    editor->setPlainText(text);
    applySettings(editor.get(), settings_); // the tab inherits the current template

    CodeEditor* raw = editor.release(); // the tab widget owns it from here
    const int index = tabs_->addTab(raw, QFileInfo(path).fileName());
    tabs_->tabBar()->setTabData(index, path); // QTabWidget itself has no tabData

    connect(raw, &CodeEditor::cursorMoved, this, &MainWindow::updateStatusLabel);
    connect(raw, &CodeEditor::selectionChanged, this, &MainWindow::updateStatusLabel);

    // G2: ONE session call owns the whole open — the workspace root (the
    // first opened file's directory: PLAN's config rule needs the root
    // BEFORE anything starts), the .qmn/lsp.json fold (the "languages"
    // table session-side, "servers" into an attached hub), the
    // suffix->language resolution + setLanguage, and with a hub attached
    // its openDocument + the M8 popup triggers. Languages no spec claims
    // honestly stay plain-editor tabs (the hub's documented false no-op).
    if (session_->workspaceRoot().isEmpty()) {
        session_->setWorkspaceRoot(QFileInfo(path).absolutePath());
        session_->loadConfig(); // no file: silent, the tables stand
    }
    if (!session_->openDocument(raw, QFileInfo(path).absoluteFilePath())) {
        // The uri is ALREADY a registered document (the session's
        // one-document-per-uri honesty): the duplicate tab still gets its
        // grammar — the coloring is the demo's call, not the registry's.
        const QString language = session_->languageForSuffix(QFileInfo(path).suffix());
        if (!language.isEmpty())
            raw->setLanguage(language);
    }
    maybeStartDapSession(raw, QFileInfo(path).absoluteFilePath());

    tabs_->setCurrentIndex(index); // currentChanged sets title + status
}

// The --dap door's auto-start leg: a debug session needs a REGISTERED
// document (breakpoints and frames resolve through the session's uri map),
// so the session begins at the first tab, not at the door. Storing the
// DEMO breakpoints BEFORE startDebugging is the hub's documented legal
// order — the ready() re-push speaks them the moment the wire exists.
void MainWindow::maybeStartDapSession(CodeEditor* editor, const QString& absolutePath)
{
    if (!dapHub_ || dapStarted_)
        return;
    dapStarted_ = true; // ONE attempt per armed hub: a dead adapter gets
                        // one honest status-bar truth, not a respawn on
                        // every tab open (a re-arm is the door's job)
    // The demo's breakpoint truth (PLAN: the APP owns it — a breakpoint UI
    // is app scope, never the shelf's): lines 3 and 7 (1-based). The PAIR
    // exists so the interesting shapes become VISIBLE in the gutter for
    // any adapter: a pull-back `breakpoint` event hits the first, an
    // unverified answer earns the second row the dap Warning (dapfake's
    // contract pulls the FIRST id; lldb/gdb answer their own honest truth).
    dapHub_->setBreakpoints(editor, {2, 6});
    const QString program
        = dapProgram_.isEmpty() ? absolutePath : dapProgram_;
    dapHub_->startDebugging(dapAdapter_, dapAdapterArgs_,
                            QJsonObject{{QStringLiteral("program"), program}});
}

void MainWindow::closeTabAt(int index)
{
    QWidget* page = tabs_->widget(index);
    if (!page)
        return;
    tabs_->removeTab(index); // does NOT delete: the editor does, now
    // The session drop + (hub attached) the orderly didClose while the
    // server is still alive (the editor's destroyed() would auto-release
    // on BOTH sides too — this is just the orderly version of it).
    session_->closeDocument(qobject_cast<CodeEditor*>(page));
    page->deleteLater();
    setCurrentTabTitle();
    updateStatusLabel();
}

void MainWindow::updateStatusLabel()
{
    const CodeEditor* editor = currentEditor();
    if (!editor) {
        statusLabel_->clear();
        return;
    }
    // 1-based display (user-facing), mirroring qtmonaco's convention;
    // selectedLength() is O(log P) with no text copy, so per-keystroke
    // updates are cheap (same reasoning as the spike's title).
    QString text = QStringLiteral("\u0421\u0442\u0440. %1, \u043a\u043e\u043b. %2")
                       .arg(editor->position().line + 1)
                       .arg(editor->position().col + 1);
    if (editor->hasSelection())
        text += QStringLiteral(" \u00b7 \u0432\u044b\u0434\u0435\u043b\u0435\u043d\u043e %1")
                    .arg(editor->selectedLength());
    statusLabel_->setText(text);
}

void MainWindow::setCurrentTabTitle()
{
    QString title = QStringLiteral("qtmonaco-native \u2014 \u0434\u0435\u043c\u043e\u043d\u0441\u0442\u0440\u0430\u0446\u0438\u044f");
    if (const int index = tabs_->currentIndex(); index >= 0)
        title = tabs_->tabText(index) + QStringLiteral(" \u2014 ") + title;
    setWindowTitle(title);
}

} // namespace qmn::app
