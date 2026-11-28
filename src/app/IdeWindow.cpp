#include "IdeWindow.hpp"

#include "fuzzy/FuzzyMatcher.hpp"
#include "glue/LspHub.hpp"
#include "glue/ProjectSession.hpp"
#include "lsp/DocumentSyncer.hpp" // car I3: the ENCODED decode — wire
#include "lsp/LspClient.hpp"      //   positions reconcile through the
#include "lsp/PositionConverter.hpp" // shelf's PUBLIC qmn_lsp link, and no
                                  // protocol/lsp value is ever STORED (the
                                  // header shows none: the I0 law stands)
#include "protocol/RpcClient.hpp" // the FeatureHandler's Reply unpacking,
                                  // inside this TU's lambdas only
#include "search/SearchEngine.hpp"
#include "view/CodeDocument.hpp" // car I3: the clamp math reads the model
#include "view/CodeEditor.hpp"
#include "view/CodeEditorOptions.hpp" // car I4: the output pane's knobs (readOnly
                                      // and soft wrap are OPTIONS doors — the
                                      // view keeps those setters private)

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent> // car I4: the runner's window-close death
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument> // car I4: .qmn/tasks.json (the app reads its OWN
                         // config file — the shelf owns no task door)
#include <QJsonObject>
#include <QProcess> // car I4: the runner (QProcess is QtCore and the app
                    // layer is where a process belongs; the type NEVER
                    // crosses a public door — the I0 law rides)
#include <QRegularExpression> // car I4: the problem matcher
#include <QSet>
#include <QUrl>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QShortcut>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace qmn::app {

using qmn::view::TextPosition; // the header's EditorSettings.hpp already
                               // carries the CodeEditor `using` (the
                               // MainWindow pattern)

namespace {

// The demo's loader law, replicated (UTF-8, CRLF/CR normalized to LF):
// the two app copies are deliberate at I0 size — this helper earning a
// THIRD home (or a bug-fix divergence) is the second-user tripwire that
// promotes it to an app-level util. Promotion only ever happens at a
// car boundary with its own record (the GATE 4 promotion law).
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
    QString text = QString::fromUtf8(file.readAll());
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

constexpr int kRowRole = Qt::UserRole + 1; // the leaf item's row index
constexpr int kOutlineMaxRetries = 10;     // car I3: the outline's bounded
                                           // wait for a server still finishing
                                           // its handshake

// car I3: the outline's OWN refresh policy (the shelf's tenth door has no
// auto-feed by design — the app owns the WHEN; PLAN's opening record).
constexpr int kOutlineDebounceMs = 600;

// ---- car I4: the runner's + the matcher's frozen numbers (PLAN's opening
// record; tst_ide pins the CAPS and the faces, never a wall-clock) ----
// terminate → kill escalation grace. Short enough to be a UI truth the user
// waits through, long enough for a child that handles SIGTERM to answer.
constexpr int kStopGraceMs = 1500;
// The pane's paced whole-text replace (a build log arrives in bursts; one
// replace per burst, and a FINAL one at the run's end).
constexpr int kOutputFlushMs = 60;
// THE TWO PROBLEM CAPS are CLASS constants next to the pane's kOutputMaxLines
// in the header — the same posture that lets a test NAME the cap it pins. Their
// faces (the panel summary's "ещё N не показаны" and the
// run summary's "(матчер пропустил N)") are pinned
// in tst_ide::i4ProblemRowAndFeedCapsCountAndShowTheSkipped.
// THE PRODUCER TAG on every task-fed diagnostic (the post-1.8.0 opaque
// `source` field is exactly a producer label — the DapHub merge mechanism
// keys by it, and the app rides the same idiom with its own tag).
constexpr char kTaskSourceTag[] = "qmn-tasks";
// The config file's home (the lsp.json fold's layout precedent, ONE door
// deeper: .qmn/<name>.json under the project root).
constexpr char kTasksConfigName[] = ".qmn/tasks.json";
constexpr char kWorkspaceRootToken[] = "${WORKSPACE_ROOT}";

} // namespace

IdeWindow::IdeWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("qtmonaco-native \u2014 \u0418\u0414\u0415 (\u0434\u0435\u043c\u043e)"));
    resize(1200, 760);

    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &IdeWindow::closeTabAt);
    setCentralWidget(tabs_);

    // G2 law inherited whole: the document session is ALWAYS on, hub or
    // no hub (registry + suffix table + config fold are the app's own
    // needs; the tabs are its presentation).
    session_ = std::make_unique<qmn::glue::ProjectSession>();

    // The project dock: Qt's OWN tree plumbing (QFileSystemModel +
    // QTreeView) — the only qmn code here is the open-on-double-click.
    // Nothing to promote: the tree is a view of the filesystem, the
    // engine's the engine's.
    fileModel_ = new QFileSystemModel(this);
    fileModel_->setReadOnly(true);
    projectTree_ = new QTreeView(this);
    projectTree_->setModel(fileModel_);
    projectTree_->setHeaderHidden(true);
    connect(projectTree_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        const QString path = fileModel_->filePath(index);
        if (!path.isEmpty() && QFileInfo(path).isFile())
            openFile(path);
    });
    auto* projectDock = new QDockWidget(QStringLiteral("Проект"), this);
    projectDock->setWidget(projectTree_);
    addDockWidget(Qt::LeftDockWidgetArea, projectDock);

    // The Find-in-Files dock (I1's panel): query row + options + the
    // result tree (file groups, matched-LINE leaves — a leaf row is one
    // activateSearchResult target).
    searchDock_ = new QDockWidget(QStringLiteral("Поиск по проекту"), this);
    auto* searchPanel = new QWidget(searchDock_);
    auto* searchLayout = new QVBoxLayout(searchPanel);
    auto* queryRow = new QHBoxLayout();
    searchEdit_ = new QLineEdit(searchPanel);
    searchEdit_->setPlaceholderText(QStringLiteral("\u0447\u0442\u043e \u0438\u0441\u043a\u0430\u0442\u044c"));
    auto* findButton = new QPushButton(QStringLiteral("\u041d\u0430\u0439\u0442\u0438"), searchPanel);
    queryRow->addWidget(searchEdit_, 1);
    queryRow->addWidget(findButton);
    searchLayout->addLayout(queryRow);
    auto* optionRow = new QHBoxLayout();
    auto* caseCheck = new QCheckBox(QStringLiteral("\u0420\u0435\u0433\u0438\u0441\u0442\u0440"), searchPanel);
    auto* wordCheck = new QCheckBox(QStringLiteral("\u0421\u043b\u043e\u0432\u043e \u0446\u0435\u043b\u0438\u043a\u043e\u043c"), searchPanel);
    auto* regexCheck = new QCheckBox(QStringLiteral("\u0420\u0435\u0433. \u0432\u044b\u0440\u0430\u0436\u0435\u043d\u0438\u0435"), searchPanel);
    optionRow->addWidget(caseCheck);
    optionRow->addWidget(wordCheck);
    optionRow->addWidget(regexCheck);
    optionRow->addStretch(1);
    searchLayout->addLayout(optionRow);
    resultsTree_ = new QTreeWidget(searchPanel);
    resultsTree_->setHeaderLabels({QStringLiteral("\u0424\u0430\u0439\u043b"),
                                   QStringLiteral("\u0421\u0442\u0440\u043e\u043a\u0430"),
                                   QStringLiteral("\u0421\u043e\u0434\u0435\u0440\u0436\u0438\u043c\u043e\u0435")});
    resultsTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    resultsTree_->setRootIsDecorated(true);
    connect(resultsTree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (item->parent() && item->data(0, kRowRole).isValid())
                    activateSearchResult(item->data(0, kRowRole).toInt());
            });
    searchLayout->addWidget(resultsTree_, 1);
    searchDock_->setWidget(searchPanel);
    addDockWidget(Qt::BottomDockWidgetArea, searchDock_);

    // CONNECT-BEFORE-ANY-START (car E's law, paid at this door once):
    // the engine's refusal faces land synchronously inside start(), so
    // these connections exist from the ctor, before any runSearch.
    search_ = std::make_unique<qmn::search::SearchSession>(this);
    connect(search_.get(), &qmn::search::SearchSession::fileMatched, this,
            [this](const qmn::search::FileMatch& match) {
                appendResultRows(match);
            });
    connect(search_.get(), &qmn::search::SearchSession::finished, this,
            &IdeWindow::onSearchFinished);
    connect(findButton, &QPushButton::clicked, this, [this, caseCheck, wordCheck, regexCheck] {
        runSearch(searchEdit_->text(), caseCheck->isChecked(),
                  wordCheck->isChecked(), regexCheck->isChecked());
    });
    connect(searchEdit_, &QLineEdit::returnPressed, this,
            [findButton] { findButton->click(); }); // click() RE-EMITS
                                                    // clicked() — one slot
                                                    // serves both faces

    // ---- car I2: the file pool, the overlays, the chords, the registry.
    // POOL: the engine's LIST face (startList — LEDGER catch #2): names
    // only, contents never read. CONNECT-BEFORE-START rides THIS door
    // too — the pool's root refusals land synchronously inside
    // startList, and openProject can run before the window is shown.
    pool_ = std::make_unique<qmn::search::SearchSession>(this);
    connect(pool_.get(), &qmn::search::SearchSession::fileListed, this,
            &IdeWindow::onPoolFileListed);
    connect(pool_.get(), &qmn::search::SearchSession::finished, this,
            &IdeWindow::onPoolFinished);

    // The overlays are plain child widgets (never modal, never exec()ed —
    // the no-exec law): a QLineEdit + a QListWidget, hidden until a
    // chord/door raises them. itemActivated covers Enter AND double
    // click (one face, one slot); Esc closes through a child-scoped
    // shortcut so it never collides with the view's frozen Escape
    // (Escape = the view's own dismiss chord — it only reaches the
    // popup while the POPUP subtree has focus).
    quickOpenPop_ = makeOverlay(quickOpenEdit_, quickOpenList_,
                                QStringLiteral("\u0438\u043c\u044f \u0444\u0430\u0439\u043b\u0430"));
    palettePop_ = makeOverlay(paletteEdit_, paletteList_,
                              QStringLiteral("\u043a\u043e\u043c\u0430\u043d\u0434\u0430"));
    connect(quickOpenEdit_, &QLineEdit::textChanged, this,
            [this](const QString& text) { rankQuickOpen(text); });
    connect(paletteEdit_, &QLineEdit::textChanged, this,
            [this](const QString& text) { rankPalette(text); });
    connect(quickOpenList_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) { activateQuickOpen(quickOpenList_->row(item)); });
    connect(paletteList_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) { runPaletteCommand(paletteList_->row(item)); });

    // THE THREE CHORDS — car I2's chord-table audit against the VIEW's
    // frozen keymap (the I2-stub law: never re-bind a frozen view
    // chord): the view binds Ctrl+A/C/V/X/Z/Y, Ctrl+D, Ctrl+Return,
    // Tab/BackTab, Escape, the arrows/Home/End/PageUp-PageDown (plain
    // and with Shift/Ctrl) and Delete/Backspace — NO Key_P and NO Key_W
    // ever appear in its table (tst_ide pins the mask face: the chords
    // reach the WINDOW while the EDITOR keeps focus and the document
    // text stays byte-identical). F7/Shift+F7 stay S1's diff-nav chords
    // — the IDE has no diff mode yet, so they ride UNBOUND here.
    auto* scQuickOpen = new QShortcut(QKeySequence(QStringLiteral("Ctrl+P")), this);
    connect(scQuickOpen, &QShortcut::activated, this, &IdeWindow::showQuickOpen);
    auto* scPalette = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")), this);
    connect(scPalette, &QShortcut::activated, this, &IdeWindow::showCommandPalette);
    auto* scCloseTab = new QShortcut(QKeySequence(QStringLiteral("Ctrl+W")), this);
    connect(scCloseTab, &QShortcut::activated, this, &IdeWindow::closeActiveTabCommand);

    // ---- car I3: the outline dock, its refresh policy, the LSP chords and
    // the two feature overlays. The overlays are plain children again (the
    // I2 no-exec law), and their Esc is CHILD-scoped (WidgetWithChildren-
    // Shortcut) so the view's frozen Escape chord stays the editor's own.
    buildOutlineDock();
    outlineDebounce_ = new QTimer(this);
    outlineDebounce_->setSingleShot(true);
    outlineDebounce_->setInterval(kOutlineDebounceMs);
    connect(outlineDebounce_, &QTimer::timeout, this,
            [this] { refreshOutline(); });
    // THE OUTLINE'S REFRESH LAW: a tab switch or a file-open-then-focus
    // rides currentChanged (addTab sets the index), an edit re-arms the
    // debounce per tab (addEditorTab), the dock button + the palette
    // command + the manual door are the explicit asks. NOTHING auto-pulls
    // per keystroke (the tenth door carries the ask, the outline owns the
    // WHEN — PLAN's opening decision, spelled in code here).
    connect(tabs_, &QTabWidget::currentChanged, this,
            [this] { refreshOutline(); });
    const auto addEsc = [this](QWidget* pop) {
        auto* esc = new QAction(pop);
        esc->setShortcut(QKeySequence(Qt::Key_Escape));
        esc->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(esc, &QAction::triggered, this, &IdeWindow::closeOverlay);
        pop->addAction(esc);
    };
    renamePop_ = new QWidget(this);
    renamePop_->setVisible(false);
    {
        auto* lay = new QVBoxLayout(renamePop_);
        lay->setContentsMargins(6, 6, 6, 6);
        renameEdit_ = new QLineEdit(renamePop_);
        renameEdit_->setPlaceholderText(
            QStringLiteral("\u043d\u043e\u0432\u043e\u0435 \u0438\u043c\u044f \u00b7 Enter \u2014 \u043f\u0440\u0438\u043c\u0435\u043d\u0438\u0442\u044c"));
        lay->addWidget(renameEdit_);
        connect(renameEdit_, &QLineEdit::returnPressed, this,
                [this] { commitRename(renameEdit_->text()); });
        addEsc(renamePop_);
    }
    actionsPop_ = new QWidget(this);
    actionsPop_->setVisible(false);
    {
        auto* lay = new QVBoxLayout(actionsPop_);
        lay->setContentsMargins(6, 6, 6, 6);
        actionsList_ = new QListWidget(actionsPop_);
        lay->addWidget(actionsList_, 1);
        connect(actionsList_, &QListWidget::itemActivated, this,
                [this](QListWidgetItem* item) {
                    runCodeAction(actionsList_->row(item));
                });
        addEsc(actionsPop_);
    }

    // THE FIVE NEW CHORDS (the I2 chord-table audit rides: the view's frozen
    // keymap binds F7/Shift+F7 among function keys — Key_F12 and Key_F2
    // NEVER appear in it — and Ctrl+letters, so plain Ctrl+period is free
    // too; the I2 trio and S1's F7 pair are untouched, and tst_ide's
    // chord-mask tripwire grows these five legs).
    auto* scDefinition = new QShortcut(QKeySequence(Qt::Key_F12), this);
    connect(scDefinition, &QShortcut::activated, this,
            [this] { jumpToDefinition(); });
    auto* scReferences = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F12),
                                       this);
    connect(scReferences, &QShortcut::activated, this,
            [this] { findReferences(); });
    auto* scRename = new QShortcut(QKeySequence(Qt::Key_F2), this);
    connect(scRename, &QShortcut::activated, this,
            [this] { openRenameOverlay(); });
    auto* scFormat = new QShortcut(
        QKeySequence(QStringLiteral("Shift+Alt+F")), this);
    connect(scFormat, &QShortcut::activated, this,
            [this] { formatDocument(); });
    auto* scActions = new QShortcut(QKeySequence(QStringLiteral("Ctrl+.")), this);
    connect(scActions, &QShortcut::activated, this,
            [this] { showCodeActions(); });

    // ---- car I4: the output pane (the stub's dogfood bet, ON), the task
    // picker, the runner's two timers, and the two task chords.
    //
    // THE OUTPUT PANE IS AN EDITOR — CodeEditor under the M19 readOnly lock.
    // The lock refuses every USER edit gesture at the front door (typing/IME,
    // paste/cut/drop, undo/redo, the Ctrl word-delete, find/replace ACTIONS,
    // Tab indent) while the PROGRAMMATIC setPlainText door stays unlocked —
    // that is how the log lands — and copy() is unlocked too: a locked pane
    // is still copyable, which is the whole point of a build log. The pane is
    // NOT a document tab (no session registration: it names no file), and the
    // tail follow is app-side arithmetic over the I1 public setSelection door
    // + the view's OWN public reveal channel, revealAt(…, Minimal): ZERO new
    // view API and no ledger entry (the opening audit's answer, PLAN). Soft
    // wrap ON because a build line is long; line numbers OFF because a log's
    // line numbers mean nothing.
    outputDock_ = new QDockWidget(QStringLiteral("Вывод задач"), this);
    {
        auto* panel = new QWidget(outputDock_);
        auto* lay = new QVBoxLayout(panel);
        lay->setContentsMargins(4, 4, 4, 4);
        auto* head = new QHBoxLayout();
        auto* clearBtn = new QPushButton(QStringLiteral("Очистить"), panel);
        outputSummaryLabel_ = new QLabel(
            QStringLiteral("задачи: ни одна ещё не запускалась"), panel);
        outputSummaryLabel_->setWordWrap(true);
        head->addWidget(clearBtn);
        head->addWidget(outputSummaryLabel_, 1);
        lay->addLayout(head);
        outputEditor_ = new CodeEditor(panel);
        // THE LOCK rides its M19 door — readOnly is an OPTIONS knob (the view
        // keeps setReadOnly private), which is also how the 15-knob settings
        // template spells it: the app owns this editor, so the app sets the
        // knob on its own options object.
        outputEditor_->options()->setReadOnly(true);
        outputEditor_->options()->setSoftWrapEnabled(true); // a build line is
                                                            // long
        outputEditor_->setLineNumbersMode(CodeEditor::LineNumbersMode::Off);
        outputEditor_->setOverviewRulerEnabled(false);
        lay->addWidget(outputEditor_, 1);
        outputDock_->setWidget(panel);
        connect(clearBtn, &QPushButton::clicked, this,
                [this] { clearTaskOutput(); });
        addDockWidget(Qt::BottomDockWidgetArea, outputDock_);
    }

    // THE PICKER: the third popup on the I2 overlay law whole (a plain child,
    // never exec()ed; Esc CHILD-scoped so the view's frozen Escape stays the
    // editor's own; itemActivated covers Enter AND double click).
    taskPickerPop_ = makeOverlay(taskPickerEdit_, taskPickerList_,
                                 QStringLiteral("имя задачи"));
    connect(taskPickerEdit_, &QLineEdit::textChanged, this,
            [this](const QString& text) { rankTaskPicker(text); });
    connect(taskPickerList_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) {
                activateTask(taskPickerList_->row(item));
            });
    // NO addEsc HERE — and that is a bug the drill caught, not a style choice:
    // makeOverlay ALREADY wires the child-scoped Esc QAction (the quick-open and
    // palette popups are built by it), and adding a SECOND QAction with the same
    // shortcut in the same context makes the shortcut map AMBIGUOUS — Qt then
    // matches NEITHER, the Escape falls through to the QLineEdit (which consumes
    // it as its own undo/clear), and the popup never closes. The picker's Esc leg
    // in tst_ide is the tripwire for this. (renamePop_/actionsPop_ call addEsc
    // because they are hand-built, not makeOverlay popups — each ends up with
    // exactly ONE Esc action, which is the invariant.)

    // THE RUNNER'S TIMERS (created in the CTOR, before any run — the
    // connect-before-any-start law car E paid at this window's door): the
    // paced pane replace, and the terminate→kill escalation grace.
    taskFlushTimer_ = new QTimer(this);
    taskFlushTimer_->setSingleShot(true);
    taskFlushTimer_->setInterval(kOutputFlushMs);
    connect(taskFlushTimer_, &QTimer::timeout, this,
            [this] { flushTaskOutput(); });
    taskStopTimer_ = new QTimer(this);
    taskStopTimer_->setSingleShot(true);
    taskStopTimer_->setInterval(kStopGraceMs);
    connect(taskStopTimer_, &QTimer::timeout, this, [this] {
        if (taskProc_ && taskProc_->state() != QProcess::NotRunning)
            taskProc_->kill(); // the escalation's witness is the app's OWN
    });                        // stopped face — a stubborn child has no voice

    // THE TWO NEW CHORDS, mask-free and audited AT OPENING (the I2/I3
    // precedent; the tripwire grows two legs): the view's frozen keymap binds
    // NO Key_B and NO Key_R EVER (its Ctrl switch's cases are Home/End/
    // Left/Right/Delete/Down/Up/A/D/Return/Enter and its `default:` IGNORES
    // the event — the exact mechanism that let I2's Ctrl+P/Ctrl+W and I3's
    // Ctrl+. through) and its QKeySequence:: matches carry an EXACT modifier
    // set. Ctrl+Shift+B runs the default task; Ctrl+Shift+R raises the picker.
    auto* scRunDefault = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+Shift+B")), this);
    connect(scRunDefault, &QShortcut::activated, this,
            [this] { runDefaultTask(); });
    auto* scTaskPicker = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+Shift+R")), this);
    connect(scTaskPicker, &QShortcut::activated, this,
            [this] { showTaskPicker(); });

    registerBuiltInCommands();

    statusLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(statusLabel_);
    updateStatusLabel();
}

// Out of line: the unique_ptr members (LspHub, SearchSession) need the
// complete types HERE, and the header deliberately never shows them
// (the AGENTS app-header law: fwd-decls only). car I4 adds a BODY: a
// running task must die HERE, before ~QObject takes the child away and
// before the widgets a finished-run handler would touch go away (the same
// teardown-safety posture car E spelled around the hub).
IdeWindow::~IdeWindow()
{
    killRunningTask();
}

// car I4: the window-close half of the runner's death law (the dtor above
// covers a delete; a real close goes through HERE).
void IdeWindow::closeEvent(QCloseEvent* event)
{
    killRunningTask();
    event->accept();
}

void IdeWindow::openProject(const QString& dirPath)
{
    projectRoot_ = QDir(dirPath).absolutePath();
    fileModel_->setRootPath(dirPath);
    projectTree_->setRootIndex(fileModel_->index(dirPath));
    // The session's root + config fold ride the SAME door (the demo set
    // the root at the first openPath because it had no project concept;
    // the IDE knows the root earlier — openProject IS the concept).
    session_->setWorkspaceRoot(projectRoot_);
    session_->loadConfig(); // no .qmn/lsp.json: silent, the tables stand
    // car I4: the task half of the SAME door (the config FILE is the door —
    // app code reads its own file, the shelf owns nothing here). THE LAW OF
    // THIS SEAM: a running task DIES at a project switch (the pool-replacement
    // seam's process sibling), and its pane + problems + diagnostics retire
    // with it — a task from the old root never poses as this project's truth.
    // THE ORDER IS THE FACE: retire the finished run's faces FIRST, then kill —
    // a LIVE run's death overwrites them with its own testimony (kTaskExitCrashed
    // + "прервана", killRunningTask's comment spells why). A switch that killed
    // and then reset to "pending" would erase the very face that proves the kill.
    clearProblemFeed(QStringLiteral("смена проекта"));
    taskLog_.clear();
    taskHiddenLines_ = 0;
    taskExitCode_ = kTaskExitPending;
    taskElapsedMs_ = -1;
    taskPendingBytes_.clear();
    if (outputEditor_)
        outputEditor_->setPlainText(QString());
    // (the label is NOT reset here — killRunningTask names the run in its own
    // testimony; a finished run's label retires in the branch below)
    const bool killedLive = killRunningTask(); // writes the killed run's face
    if (!killedLive)
        taskRunLabel_.clear(); // a FINISHED run's label retires with the run;
                               // a KILLED one stays named in the summary —
                               // "задача «build» прервана" is the switch's own
                               // testimony that a live process was ended here
    readTasksConfig();
    // The quick-open pool re-arms at the SAME door the tree re-arms
    // (openProject IS the project concept): cancel-and-restart lands in
    // onPoolFinished — the engine retires walk_ BEFORE the emit, so a
    // restart inside that signal is the engine's own sanctioned face.
    kickPoolWalk(projectRoot_);
    statusBar()->showMessage(QStringLiteral("\u041f\u0440\u043e\u0435\u043a\u0442: %1").arg(projectRoot_));
    // car I3: a project switch may have folded a NEW server in — the
    // outline re-asks at the same door the tree and the pool re-arm at (and
    // with no hub yet it clears LOUD, which is the honest pre-lsp face).
    refreshOutline();
}

bool IdeWindow::openFile(const QString& path)
{
    const QString absolute = QFileInfo(path).absoluteFilePath();

    // The one-tab-per-path law: a re-open FOCUSES (never a second
    // registration, never a second didOpen — car D's JOIN story needs
    // TWO views first, and the IDE does not offer that face yet; when
    // it does, that is a recorded feature, not a silent duplicate).
    for (int i = 0; i < tabs_->count(); ++i) {
        if (tabs_->tabBar()->tabData(i).toString() == absolute) {
            tabs_->setCurrentIndex(i);
            statusBar()->showMessage(QStringLiteral("\u0412\u043a\u043b\u0430\u0434\u043a\u0430 \u0443\u0436\u0435 \u043e\u0442\u043a\u0440\u044b\u0442\u0430 \u2014 \u0444\u043e\u043a\u0443\u0441: %1")
                                         .arg(QFileInfo(absolute).fileName()));
            return true;
        }
    }

    QString error;
    const QString text = readTextFile(path, &error);
    if (!error.isEmpty()) {
        // Non-modal, the demo's drivability law whole.
        statusBar()->showMessage(QStringLiteral("\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u043e\u0442\u043a\u0440\u044b\u0442\u044c \u00ab%1\u00bb: %2")
                                     .arg(path, error));
        return false;
    }
    addEditorTab(absolute, text);
    return true;
}

void IdeWindow::setLspEnabled(bool enabled)
{
    if (enabled == (hub_ != nullptr))
        return;
    if (!enabled) {
        // The demo's order byte-for-byte: sweep the view-side popups by
        // hand (nobody is left to hide them), detach the session's raw
        // pointer BEFORE the hub dies, ~LspHub does the honest wire
        // teardown.
        for (int i = 0; i < tabs_->count(); ++i) {
            if (CodeEditor* editor = editorAt(i)) {
                editor->hideCompletionList();
                editor->hideHover();
            }
        }
        session_->attachHub(nullptr);
        hub_.reset();
        // car I3: with the key down, NOTHING can answer — the outline goes
        // empty LOUDLY (a stale tree never poses as current) and the six
        // feature summaries retire with the key. The generation bump
        // retires an answer still on the wire (its handler now drops it).
        outlineDebounce_->stop();
        ++outlineGeneration_;
        clearOutlineRows(QStringLiteral("\u0441\u0438\u043c\u0432\u043e\u043b\u044b: LSP-\u0441\u0435\u0440\u0432\u0435\u0440 \u043d\u0435 \u043f\u043e\u0434\u043a\u043b\u044e\u0447\u0451\u043d"));
        jumpSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0445\u043e\u0434: LSP-\u043a\u043b\u044e\u0447 \u043e\u043f\u0443\u0448\u0435\u043d");
        renameSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u043d\u0438\u0435: LSP-\u043a\u043b\u044e\u0447 \u043e\u043f\u0443\u0448\u0435\u043d");
        formatSummary_ = QStringLiteral("\u0444\u043e\u0440\u043c\u0430\u0442\u0438\u0440\u043e\u0432\u0430\u043d\u0438\u0435: LSP-\u043a\u043b\u044e\u0447 \u043e\u043f\u0443\u0448\u0435\u043d");
        actionsSummary_ = QStringLiteral("\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u044f: LSP-\u043a\u043b\u044e\u0447 \u043e\u043f\u0443\u0448\u0435\u043d");
        statusBar()->showMessage(QStringLiteral("LSP: \u0441\u0435\u0440\u0432\u0435\u0440\u044b \u043e\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u044b"));
        return;
    }
    hub_ = std::make_unique<qmn::glue::LspHub>(); // built-in specs; the
                                                  // session replays its
                                                  // already-read config
    session_->attachHub(hub_.get());
    statusBar()->showMessage(QStringLiteral("LSP: \u0432\u043a\u043b\u044e\u0447\u0451\u043d (\u0441\u043f\u0435\u043a\u0438 \u0438\u0437 \u043a\u043e\u0440\u043e\u0431\u043a\u0438)"));
}

int IdeWindow::editorCount() const
{
    return tabs_->count(); // addEditorTab only ever makes CodeEditor tabs
}

CodeEditor* IdeWindow::editorAt(int index) const
{
    return qobject_cast<CodeEditor*>(tabs_->widget(index));
}

CodeEditor* IdeWindow::currentEditor() const
{
    return qobject_cast<CodeEditor*>(tabs_->currentWidget());
}

void IdeWindow::addEditorTab(const QString& path, const QString& text)
{
    auto editor = std::make_unique<CodeEditor>();
    editor->setPlainText(text);

    CodeEditor* raw = editor.release(); // the tab owns it from here

    // CAR I3 ORDER LAW (the outline's first ask depends on it): the SESSION
    // open lands BEFORE the tab becomes current. tabs_->addTab() emits
    // currentChanged, and the outline refresh riding that signal asks the hub
    // about the CURRENT document — an ask that reaches the hub before the
    // document is registered is a "not registered" refusal the outline's
    // retry law cannot tell apart from a real one (no syncer to wait on).
    // Register first, then raise the tab: the first ask then rides either a
    // Ready server or the bounded still-starting wait.
    const QString language = session_->languageForSuffix(QFileInfo(path).suffix());
    if (!language.isEmpty())
        raw->setLanguage(language);
    if (!session_->openDocument(raw, path)) {
        const QString retry = session_->languageForSuffix(QFileInfo(path).suffix());
        if (!retry.isEmpty())
            raw->setLanguage(retry); // the demo's honest duplicate face
                                     // (grammar rides, registry stays)
    }
    // car I4: a standing task feed CATCHES UP on a newly opened tab (the
    // DapHub join analogue, app-side): the problems are the RUN's truth, not
    // the tab's, so opening the file a compiler complained about shows the
    // squiggle immediately — no re-run asked. The merge reads THIS view's own
    // decoration table (a foreign producer may already own rows in it).
    if (!problems_.isEmpty())
        mergeTaskDiagnostics(raw, path);

    const int index = tabs_->addTab(raw, QFileInfo(path).fileName());
    tabs_->tabBar()->setTabData(index, path);

    connect(raw, &CodeEditor::cursorMoved, this, &IdeWindow::updateStatusLabel);
    connect(raw, &CodeEditor::selectionChanged, this, &IdeWindow::updateStatusLabel);
    // car I3's refresh law: an edit re-arms the outline's debounce (the ask
    // lands a beat after the last keystroke — never per keystroke).
    connect(raw, &CodeEditor::textChanged, this, [this] {
        if (hub_)
            outlineDebounce_->start();
    });

    tabs_->setCurrentIndex(index);
}

void IdeWindow::closeTabAt(int index)
{
    QWidget* page = tabs_->widget(index);
    if (!page)
        return;
    tabs_->removeTab(index); // does NOT delete: the editor does, now
    session_->closeDocument(qobject_cast<CodeEditor*>(page));
    page->deleteLater();
    updateStatusLabel();
}

void IdeWindow::runSearch(const QString& text, bool caseSensitive,
                          bool wholeWord, bool isRegex)
{
    if (search_->running())
        search_->cancel(); // the engine's reentry law: one walk at a
                           // time; the cancelled walk's partial rows
                           // stay (the panel's truth up to that stop)

    results_.clear();
    resultRows_.clear();
    resultsTree_->clear();
    problemRowMap_.clear(); // car I4: a new producer takes the panel whole
    resultsProducer_ = QStringLiteral("search"); // the panel's producer
                                                 // truth (I3: the same panel
                                                 // is the references face)

    if (projectRoot_.isEmpty()) {
        searchSummary_ = QStringLiteral(
            "\u041f\u043e\u0438\u0441\u043a: \u0441\u043d\u0430\u0447\u0430\u043b\u0430 \u043e\u0442\u043a\u0440\u043e\u0439\u0442\u0435 \u043f\u0440\u043e\u0435\u043a\u0442");
        statusBar()->showMessage(searchSummary_);
        return;
    }

    qmn::search::SearchQuery query;
    query.text = text;
    query.caseSensitive = caseSensitive;
    query.wholeWord = wholeWord;
    query.isRegex = isRegex;
    search_->start(projectRoot_, query);
    // Refusal faces land SYNCHRONOUSLY right here (onSearchFinished has
    // been connected since the ctor — car E's connect-before-start law).
}

void IdeWindow::cancelSearch()
{
    search_->cancel(); // the truth lands through onSearchFinished
}

bool IdeWindow::searchRunning() const
{
    return search_->running();
}

void IdeWindow::appendResultRows(const qmn::search::FileMatch& match)
{
    const int fileIndex = results_.size();
    results_.append(match);

    const QString shown = projectRoot_.isEmpty()
        ? match.filePath
        : QDir(projectRoot_).relativeFilePath(match.filePath);
    auto* fileItem = new QTreeWidgetItem(resultsTree_);
    fileItem->setText(0, shown);
    fileItem->setText(2, QStringLiteral("\u0441\u0442\u0440\u043e\u043a: %1")
                            .arg(match.lines.size()));
    for (int li = 0; li < match.lines.size(); ++li) {
        const qmn::search::LineMatch& line = match.lines.at(li);
        auto* lineItem = new QTreeWidgetItem(fileItem);
        lineItem->setText(1, QString::number(line.line + 1)); // 1-based
                                                              // DISPLAY;
                                                              // the row's
                                                              // engine
                                                              // line is
                                                              // 0-based
        lineItem->setText(2, line.lineText);
        lineItem->setData(0, kRowRole, resultRows_.size());
        resultRows_.append({fileIndex, li});
    }
    fileItem->setExpanded(true);
}

void IdeWindow::onSearchFinished(int scanned, int matchedFiles, int totalMatches,
                                 int skipped, const QString& stopReason)
{
    // The Russian UX, one truth two faces (status bar + the searchSummary()
    // door tst_ide reads). The engine's MACHINE reason rides verbatim
    // inside the refusal/limit faces — the human text never erases the
    // truth the engine told.
    if (stopReason.isEmpty()) {
        searchSummary_ = QStringLiteral(
            "\u041d\u0430\u0439\u0434\u0435\u043d\u043e %1 \u0441\u043e\u0432\u043f\u0430\u0434\u0435\u043d\u0438\u0439 \u0432 %2 \u0444\u0430\u0439\u043b\u0430\u0445 (\u043f\u0440\u043e\u0441\u043c\u043e\u0442\u0440\u0435\u043d\u043e %3, \u043f\u0440\u043e\u043f\u0443\u0449\u0435\u043d\u043e %4)")
                           .arg(totalMatches).arg(matchedFiles).arg(scanned).arg(skipped);
    } else if (stopReason.startsWith(QStringLiteral("query:"))
               || stopReason.startsWith(QStringLiteral("root:"))) {
        searchSummary_ = QStringLiteral("\u041f\u043e\u0438\u0441\u043a \u043e\u0442\u043a\u043b\u043e\u043d\u0451\u043d: %1").arg(stopReason);
    } else if (stopReason == QLatin1String("cancelled")) {
        searchSummary_ = QStringLiteral(
            "\u041f\u043e\u0438\u0441\u043a \u043e\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d \u00b7 \u0447\u0430\u0441\u0442\u0438\u0447\u043d\u044b\u0439 \u0440\u0435\u0437\u0443\u043b\u044c\u0442\u0430\u0442: %1 \u0441\u043e\u0432\u043f\u0430\u0434\u0435\u043d\u0438\u0439 \u0432 %2 \u0444\u0430\u0439\u043b\u0430\u0445")
                           .arg(totalMatches).arg(matchedFiles);
    } else {
        searchSummary_ = QStringLiteral("\u041f\u043e\u0438\u0441\u043a \u043e\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d: %1").arg(stopReason);
    }
    statusBar()->showMessage(searchSummary_);
}

int IdeWindow::searchResultRowCount() const
{
    return resultRows_.size();
}

QString IdeWindow::searchResultFileAt(int row) const
{
    if (row < 0 || row >= resultRows_.size())
        return {};
    return results_.at(resultRows_.at(row).first).filePath;
}

int IdeWindow::searchResultLineAt(int row) const
{
    if (row < 0 || row >= resultRows_.size())
        return -1;
    const auto [fileIndex, lineIndex] = resultRows_.at(row);
    return results_.at(fileIndex).lines.at(lineIndex).line;
}

bool IdeWindow::activateSearchResult(int row)
{
    if (row < 0 || row >= resultRows_.size())
        return false;
    const auto [fileIndex, lineIndex] = resultRows_.at(row);
    const qmn::search::FileMatch& match = results_.at(fileIndex);
    if (lineIndex < 0 || lineIndex >= match.lines.size())
        return false;

    if (!openFile(match.filePath))
        return false; // the file vanished mid-walk (deleted after the
                      // scan): the honest false + the openFile status
    CodeEditor* editor = currentEditor(); // openFile focuses/opens; the
                                          // current tab IS this file
    if (!editor)
        return false;

    const qmn::search::LineMatch& line = match.lines.at(lineIndex);
    if (line.spans.isEmpty())
        return false;
    // The reveal through PUBLIC view doors only (the tour law): caret +
    // selection ride setSelection; the view scrolls itself to the caret.
    const TextPosition start{line.line, line.spans.first().startCol};
    const TextPosition end{line.line, line.spans.first().endCol};    editor->setPosition(start);
    editor->setSelection(start, end);
    editor->setFocus();
    return true;
}

// ================= car I2: quick open + command palette =================

QWidget* IdeWindow::makeOverlay(QLineEdit*& editOut, QListWidget*& listOut,
                                const QString& placeholder)
{
    auto* pop = new QWidget(this);
    pop->setVisible(false); // shown only by the doors/chords below
    auto* lay = new QVBoxLayout(pop);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(4);
    editOut = new QLineEdit(pop);
    editOut->setPlaceholderText(placeholder);
    listOut = new QListWidget(pop);
    lay->addWidget(editOut);
    lay->addWidget(listOut, 1);
    // Esc closes the POPUP — and only while the popup subtree has focus
    // (WidgetWithChildrenShortcut): the view's frozen Escape chord stays
    // the editor's own while the editor holds focus (the chord-mask law).
    auto* esc = new QAction(pop);
    esc->setShortcut(QKeySequence(Qt::Key_Escape));
    esc->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(esc, &QAction::triggered, this, &IdeWindow::closeOverlay);
    pop->addAction(esc);
    return pop;
}

void IdeWindow::registerBuiltInCommands()
{
    commands_.clear(); // the registry is the palette's backend: built-ins
                       // land in the ctor, ONCE, in a frozen order (the
                       // tie law rides registry order)
    const auto reg = [this](const QString& id, const QString& label,
                            const QString& shortcutText, std::function<void()> fn) {
        commands_.append(Command{ id, label, shortcutText, std::move(fn) });
    };
    reg(QStringLiteral("quick-open"),
        QStringLiteral("\u0411\u044b\u0441\u0442\u0440\u043e\u0435 \u043e\u0442\u043a\u0440\u044b\u0442\u0438\u0435 \u0444\u0430\u0439\u043b\u043e\u0432"),
        QStringLiteral("Ctrl+P"), [this] { showQuickOpen(); });
    reg(QStringLiteral("command-palette"),
        QStringLiteral("\u041f\u0430\u043b\u0438\u0442\u0440\u0430 \u043a\u043e\u043c\u0430\u043d\u0434"),
        QStringLiteral("Ctrl+Shift+P"), [this] { showCommandPalette(); });
    reg(QStringLiteral("find-in-files"),
        QStringLiteral("\u041f\u043e\u0438\u0441\u043a \u043f\u043e \u043f\u0440\u043e\u0435\u043a\u0442\u0443 \u2014 \u0444\u043e\u043a\u0443\u0441 \u043f\u0430\u043d\u0435\u043b\u0438"),
        QString(), [this] {
            searchDock_->show();
            searchEdit_->setFocus();
        });
    reg(QStringLiteral("close-tab"),
        QStringLiteral("\u0417\u0430\u043a\u0440\u044b\u0442\u044c \u0430\u043a\u0442\u0438\u0432\u043d\u0443\u044e \u0432\u043a\u043b\u0430\u0434\u043a\u0443"),
        QStringLiteral("Ctrl+W"), [this] { closeActiveTabCommand(); });
    // car I3: the six LSP commands APPEND to the frozen registry order (the
    // palette's tie law rides registry order — an append never disturbs the
    // I2 four).
    reg(QStringLiteral("goto-definition"),
        QStringLiteral("\u041f\u0435\u0440\u0435\u0445\u043e\u0434 \u043a \u043e\u043f\u0440\u0435\u0434\u0435\u043b\u0435\u043d\u0438\u044e"),
        QStringLiteral("F12"), [this] { jumpToDefinition(); });
    reg(QStringLiteral("find-references"),
        QStringLiteral("\u041d\u0430\u0439\u0442\u0438 \u0441\u0441\u044b\u043b\u043a\u0438 \u2014 \u043f\u0430\u043d\u0435\u043b\u044c \u0440\u0435\u0437\u0443\u043b\u044c\u0442\u0430\u0442\u043e\u0432"),
        QStringLiteral("Shift+F12"), [this] { findReferences(); });
    reg(QStringLiteral("rename-symbol"),
        QStringLiteral("\u041f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u0442\u044c \u0441\u0438\u043c\u0432\u043e\u043b"),
        QStringLiteral("F2"), [this] { openRenameOverlay(); });
    reg(QStringLiteral("format-document"),
        QStringLiteral("\u0424\u043e\u0440\u043c\u0430\u0442\u0438\u0440\u043e\u0432\u0430\u0442\u044c \u0434\u043e\u043a\u0443\u043c\u0435\u043d\u0442"),
        QStringLiteral("Shift+Alt+F"), [this] { formatDocument(); });
    reg(QStringLiteral("code-actions"),
        QStringLiteral("\u0411\u044b\u0441\u0442\u0440\u044b\u0435 \u043f\u0440\u0430\u0432\u043a\u0438 \u043e\u0442 \u0441\u0435\u0440\u0432\u0435\u0440\u0430"),
        QStringLiteral("Ctrl+."), [this] { showCodeActions(); });
    reg(QStringLiteral("outline-refresh"),
        QStringLiteral("\u041e\u0431\u043d\u043e\u0432\u0438\u0442\u044c \u0441\u043f\u0438\u0441\u043e\u043a \u0441\u0438\u043c\u0432\u043e\u043b\u043e\u0432"),
        QString(), [this] { refreshOutline(); });
    registerTaskCommands(); // car I4: the task commands APPEND (the palette's
                            // tie law rides registry order — an append never
                            // disturbs the I2 four or the I3 six)
}

void IdeWindow::closeActiveTabCommand()
{
    if (tabs_->count() == 0) {
        // The honest no-op face (the F7-outside-diff precedent): a loud
        // status line, never a silent shrug.
        statusBar()->showMessage(QStringLiteral("\u041d\u0435\u0442 \u043e\u0442\u043a\u0440\u044b\u0442\u043e\u0439 \u0432\u043a\u043b\u0430\u0434\u043a\u0438"));
        return;
    }
    closeTabAt(tabs_->currentIndex());
}

void IdeWindow::kickPoolWalk(const QString& root)
{
    if (pool_->running()) {
        poolPendingRoot_ = root; // the restart lands in onPoolFinished
        pool_->cancel();         // (never a silent second session)
        return;
    }
    poolFiles_.clear();
    poolDisplay_.clear();
    poolWalking_ = true;
    pool_->startList(root); // root refusals land SYNCHRONOUSLY here
                            // (the ctor's connects were FIRST)
}

void IdeWindow::onPoolFileListed(const QString& filePath)
{
    if (!poolWalking_)
        return; // the cancelled-then-replaced walk's late legs belong
                // to the OLD project; the fresh start clears the pool
    poolFiles_.append(filePath);
    poolDisplay_.append(QDir(projectRoot_).relativeFilePath(filePath));
}

void IdeWindow::onPoolFinished(int, int, int, int, const QString& stopReason)
{
    poolWalking_ = false;
    if (!poolPendingRoot_.isEmpty()) {
        const QString root = std::move(poolPendingRoot_);
        poolPendingRoot_.clear();
        kickPoolWalk(root); // running() is false HERE (the engine
                            // retired walk_ before THIS emit)
        return;
    }
    // The pool's own faces are LOUD too (the never-erase-the-engine's-
    // truth law): a refused/budget-capped pool says so in the bar.
    if (stopReason.startsWith(QLatin1String("root:"))
        || stopReason.startsWith(QLatin1String("budget:"))) {
        statusBar()->showMessage(QStringLiteral("\u0424\u0430\u0439\u043b\u044b \u043f\u0440\u043e\u0435\u043a\u0442\u0430: %1").arg(stopReason));
    }
}

void IdeWindow::showQuickOpen()
{
    // Idempotence face: the chord (or a palette command) reaching an
    // ALREADY-open quick-open is a NO-OP — a retriger must never wipe
    // the query the user is typing. (The OTHER popup still switches:
    // showOverlay's closeOverlay handles palette→quick-open in order.)
    if (quickOpenVisible())
        return;
    quickOpenEdit_->setText(QString()); // a FRESH query each open (the
                                        // deterministic face; a stale
                                        // textChanged may or may not
                                        // fire, so rank EXPLICITLY):
    rankQuickOpen(quickOpenEdit_->text());
    showOverlay(quickOpenPop_, quickOpenEdit_);
}

void IdeWindow::showCommandPalette()
{
    if (commandPaletteVisible())
        return; // the same idempotence law
    paletteEdit_->setText(QString());
    rankPalette(paletteEdit_->text());
    showOverlay(palettePop_, paletteEdit_);
}

void IdeWindow::showOverlay(QWidget* popup, QLineEdit* edit)
{
    closeOverlay(); // one overlay at a time
    const int w = qMax(320, qMin(640, static_cast<int>(width() * 0.6)));
    const int h = qMax(200, qMin(420, static_cast<int>(height() * 0.5)));
    popup->setGeometry((width() - w) / 2, static_cast<int>(height() * 0.15), w, h);
    popup->show();
    popup->raise();
    edit->setFocus();
}

void IdeWindow::closeOverlay()
{
    quickOpenPop_->hide();
    palettePop_->hide();
    if (renamePop_) // car I3's overlays share the ONE close face (the Esc
        renamePop_->hide(); //   twins and the doors both route here)
    if (actionsPop_)
        actionsPop_->hide();
    if (taskPickerPop_) // car I4's picker joins the ONE close face (the third
        taskPickerPop_->hide(); // popup on the I2 law — one overlay at a time)
}

bool IdeWindow::quickOpenVisible() const
{
    return quickOpenPop_ && quickOpenPop_->isVisible();
}

bool IdeWindow::commandPaletteVisible() const
{
    return palettePop_ && palettePop_->isVisible();
}

void IdeWindow::typeQuickOpenQuery(const QString& text)
{
    quickOpenEdit_->setText(text); // textChanged → rankQuickOpen (the
                                   // popup's OWN channel — the door and
                                   // the keystroke share the slot)
}

void IdeWindow::typePaletteQuery(const QString& text)
{
    paletteEdit_->setText(text);
}

void IdeWindow::typeTaskPickerQuery(const QString& text)
{
    taskPickerEdit_->setText(text); // textChanged → rankTaskPicker (the popup's
}                                   // OWN channel — the door and the keystroke
                                    // share the slot, the I2 shape)

void IdeWindow::rankQuickOpen(const QString& needle)
{
    quickOpenRows_.clear();
    quickOpenList_->clear();

    if (projectRoot_.isEmpty()) {
        // The loud refusal (the search panel's law, sibling face): an
        // empty pool is never a silent zero-hit list.
        quickOpenSummary_ = QStringLiteral(
            "\u0411\u044b\u0441\u0442\u0440\u043e\u0435 \u043e\u0442\u043a\u0440\u044b\u0442\u0438\u0435: \u0441\u043d\u0430\u0447\u0430\u043b\u0430 \u043e\u0442\u043a\u0440\u043e\u0439\u0442\u0435 \u043f\u0440\u043e\u0435\u043a\u0442");
        return;
    }

    const int total = poolDisplay_.size();
    struct Row { int poolIndex; int score; };
    QVector<Row> rows;
    if (needle.isEmpty()) {
        for (int i = 0; i < total; ++i) // the POOL WALK ORDER rides (the
            rows.append({ i, 0 });      // frozen empty-needle face — no
                                        // scorer call at all
    } else {
        rows.reserve(total);
        for (int i = 0; i < total; ++i) {
            // The scorer scores the DISPLAY string — what the user
            // reads is what gets matched (the absolute path's shared
            // root would dilute every boundary seam).
            const auto m = qmn::fuzzy::match(needle, poolDisplay_.at(i));
            if (m.matched)
                rows.append({ i, m.score });
        }
        std::stable_sort(rows.begin(), rows.end(), [this](const Row& a, const Row& b) {
            if (a.score != b.score)
                return a.score > b.score;
            return poolDisplay_.at(a.poolIndex) < poolDisplay_.at(b.poolIndex);
        });
    }

    const int shown = qMin(rows.size(), kQuickOpenMaxRows);
    for (int r = 0; r < shown; ++r) {
        quickOpenRows_.append(rows[r].poolIndex);
        quickOpenList_->addItem(poolDisplay_.at(rows[r].poolIndex));
    }

    // Russian UX, the SAME truth laws: truncation VISIBLE (the skip-
    // honesty law), a still-filling pool VISIBLE, a refusal LOUD.
    const QString partialSuffix = poolWalking_
        ? QStringLiteral(" \u00b7 \u0441\u043f\u0438\u0441\u043e\u043a \u0444\u0430\u0439\u043b\u043e\u0432 \u0435\u0449\u0451 \u0441\u0442\u0440\u043e\u0438\u0442\u0441\u044f")
        : QString();
    if (needle.isEmpty()) {
        quickOpenSummary_ = total > kQuickOpenMaxRows
            ? QStringLiteral("\u041f\u043e\u043a\u0430\u0437\u0430\u043d\u044b \u043f\u0435\u0440\u0432\u044b\u0435 %1 \u0438\u0437 %2 \u0444\u0430\u0439\u043b\u043e\u0432").arg(kQuickOpenMaxRows).arg(total)
            : QStringLiteral("\u0412\u0441\u0435\u0433\u043e \u0444\u0430\u0439\u043b\u043e\u0432: %1").arg(total);
    } else if (rows.isEmpty()) {
        quickOpenSummary_ = poolWalking_
            ? QStringLiteral("\u041d\u0435\u0442 \u0441\u043e\u0432\u043f\u0430\u0434\u0435\u043d\u0438\u0439 \u2014 \u0441\u043f\u0438\u0441\u043e\u043a \u0444\u0430\u0439\u043b\u043e\u0432 \u0435\u0449\u0451 \u0441\u0442\u0440\u043e\u0438\u0442\u0441\u044f")
            : QStringLiteral("\u041d\u0435\u0442 \u0441\u043e\u0432\u043f\u0430\u0434\u0435\u043d\u0438\u0439 (%1 \u0444\u0430\u0439\u043b\u043e\u0432 \u0432 \u043f\u0443\u043b\u0435)").arg(total);
    } else if (rows.size() > kQuickOpenMaxRows) {
        quickOpenSummary_ = QStringLiteral("\u041f\u043e\u043a\u0430\u0437\u0430\u043d\u044b \u043f\u0435\u0440\u0432\u044b\u0435 %1, \u0435\u0449\u0451 %2 \u0441\u043a\u0440\u044b\u0442\u043e")
                                .arg(kQuickOpenMaxRows).arg(rows.size() - kQuickOpenMaxRows)
                            + partialSuffix;
    } else {
        quickOpenSummary_ = QStringLiteral("\u0421\u043e\u0432\u043f\u0430\u0434\u0435\u043d\u0438\u0439: %1").arg(rows.size()) + partialSuffix;
    }
}

int IdeWindow::quickOpenRowCount() const
{
    return quickOpenRows_.size();
}

QString IdeWindow::quickOpenPathAt(int row) const
{
    if (row < 0 || row >= quickOpenRows_.size())
        return {}; // the out-of-range honesty face (the search rows' law)
    return poolFiles_.at(quickOpenRows_.at(row));
}

bool IdeWindow::activateQuickOpen(int row)
{
    if (row < 0 || row >= quickOpenRows_.size())
        return false;
    const QString path = poolFiles_.at(quickOpenRows_.at(row));
    if (!openFile(path))
        return false; // the popup STAYS open (another row may still be
                      // the truth); openFile's status message carries
                      // the failure (the file vanished after the list)
    closeOverlay();
    return true;
}

QString IdeWindow::quickOpenSummary() const
{
    return quickOpenSummary_;
}

void IdeWindow::rankPalette(const QString& needle)
{
    paletteRows_.clear();
    paletteList_->clear();
    if (needle.isEmpty()) {
        for (int i = 0; i < commands_.size(); ++i) // REGISTRY ORDER is
            paletteRows_.append(i);                // the frozen empty-
    } else {                                       // needle face
        struct Row { int index; int score; };
        QVector<Row> rows;
        for (int i = 0; i < commands_.size(); ++i) {
            const auto m = qmn::fuzzy::match(needle, commands_.at(i).label);
            if (m.matched)
                rows.append({ i, m.score });
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.score != b.score)
                return a.score > b.score;
            return a.index < b.index; // ties ride the REGISTRY order
        });
        for (const Row& r : rows)
            paletteRows_.append(r.index);
    }
    for (int index : paletteRows_) {
        const Command& c = commands_.at(index);
        paletteList_->addItem(c.shortcutText.isEmpty()
                                  ? c.label
                                  : QStringLiteral("%1  \u00b7  %2").arg(c.label, c.shortcutText));
    }
}

int IdeWindow::commandRowCount() const
{
    return paletteRows_.size();
}

QString IdeWindow::commandLabelAt(int row) const
{
    if (row < 0 || row >= paletteRows_.size())
        return {};
    return commands_.at(paletteRows_.at(row)).label;
}

bool IdeWindow::runPaletteCommand(int row)
{
    if (row < 0 || row >= paletteRows_.size())
        return false; // out of range: NOTHING ran (pinned face)
    const Command c = commands_.at(paletteRows_.at(row)); // COPY: the
                                                         // popup-closing
                                                         // handler must
                                                         // not dangle
    closeOverlay(); // close BEFORE running: a command that opens the
                    // other overlay lands clean, not under a hide
    c.run();
    return true;
}

int IdeWindow::projectFileCount() const
{
    return poolFiles_.size();
}

bool IdeWindow::projectFilesWalking() const
{
    return poolWalking_; // true from the kick until the finished leg
                         // (a pending restart keeps it true across the
                         // cancel seam)
}

void IdeWindow::updateStatusLabel()
{
    const CodeEditor* editor = currentEditor();
    if (!editor) {
        statusLabel_->clear();
        return;
    }
    QString text = QStringLiteral("\u0421\u0442\u0440. %1, \u043a\u043e\u043b. %2")
                       .arg(editor->position().line + 1)
                       .arg(editor->position().col + 1);
    if (editor->hasSelection())
        text += QStringLiteral(" \u00b7 \u0432\u044b\u0434\u0435\u043b\u0435\u043d\u043e %1")
                    .arg(editor->selectedLength());
    statusLabel_->setText(text);
}


// ================= GATE 4 car I3: LSP depth =============================
// Six features (PLAN's I3 shape) ON TOP of what the shelf already ships:
// definition, references, rename, code actions, formatting, and the OUTLINE
// dock. New glue surface: exactly ONE door (requestDocumentSymbol, the tenth
// feature sibling). Everything else rides the nine existing request doors,
// the four apply* appliers, and the I1 public setSelection reveal channel —
// zero workarounds, zero new shelf surface beyond the ledger's own entry.
//
// THE POSITION LAW all three location-bearing features share: a wire
// Location/DocumentSymbol position is decoded through
// qmn::lsp::PositionConverter against THE TEXT THE SERVER HOLDS — the
// syncer's mirror for an OPEN document, the file ON DISK for a closed one
// (that IS the text a server holds for a closed file) — and the decode-frame
// coordinates are then CLAMPED at reveal time into whatever the document
// became meanwhile. An edit racing an answer drifts a reveal; it never
// corrupts (the converter's total clamping is the posture M4's diagnostics
// feeding froze). PositionConverter is reached through qmn_glue's PUBLIC
// qmn_lsp link — the app stores no protocol/lsp type anywhere (the header
// never shows one), it only decodes through free statics in this TU.
//
// THE SYNC-REFUSAL LAW the six doors share (the hub's featureGate route, the
// search engine's frozen face): a refusal fires the FeatureHandler
// SYNCHRONOUSLY inside the call, so after a `false` return the summary face
// ALREADY carries the machine reason VERBATIM — a door never overwrites what
// its own handler just said, and a refusal never needs an answer poll.
namespace {

constexpr int kOutlineMaxDepth = 64;    // a hostile nesting cannot blow the
                                        // decode recursion, and the cut says
                                        // so in the summary (skip honesty)
constexpr int kOutlineRowRole = Qt::UserRole + 3; // tree item -> outline row

QString kindLabelFor(int kind)
{
    switch (kind) {
    case 1: return QStringLiteral("file");
    case 2: return QStringLiteral("module");
    case 3: return QStringLiteral("namespace");
    case 4: return QStringLiteral("package");
    case 5: return QStringLiteral("class");
    case 6: return QStringLiteral("method");
    case 7: return QStringLiteral("property");
    case 8: return QStringLiteral("field");
    case 9: return QStringLiteral("constructor");
    case 10: return QStringLiteral("enum");
    case 11: return QStringLiteral("interface");
    case 12: return QStringLiteral("function");
    case 13: return QStringLiteral("variable");
    case 14: return QStringLiteral("constant");
    case 15: return QStringLiteral("string");
    case 16: return QStringLiteral("number");
    case 17: return QStringLiteral("boolean");
    case 18: return QStringLiteral("array");
    case 19: return QStringLiteral("object");
    case 20: return QStringLiteral("key");
    case 21: return QStringLiteral("null");
    case 22: return QStringLiteral("enum member");
    case 23: return QStringLiteral("struct");
    case 24: return QStringLiteral("event field");
    case 25: return QStringLiteral("operator");
    case 26: return QStringLiteral("type parameter");
    default: return QStringLiteral("symbol(%1)").arg(kind); // an out-of-range
                                                            // claim rides
                                                            // VISIBLY — it
                                                            // is never
                                                            // renamed away
    }
}

qint64 wireInt(const QJsonObject& obj, const QString& key, qint64 fallback = 0)
{
    const QJsonValue v = obj.value(key);
    if (v.isDouble())
        return qint64(v.toDouble());
    if (v.isBool())
        return v.toBool() ? 1 : 0;
    if (v.isString()) {
        bool ok = false;
        const qint64 parsed = v.toString().toLongLong(&ok);
        if (ok)
            return parsed;
    }
    return fallback;
}

QString wireStr(const QJsonObject& obj, const QString& key)
{
    const QJsonValue v = obj.value(key);
    return v.isString() ? v.toString() : QString();
}

QJsonObject wireObj(const QJsonObject& obj, const QString& key)
{
    const QJsonValue v = obj.value(key);
    return v.isObject() ? v.toObject() : QJsonObject();
}

// One wire Range -> its Positions as QPoint(x = character, y = line), still
// the WIRE frame (the encoding is reconciled by the caller, per frame).
QPair<QPoint, QPoint> wireRange(const QJsonObject& holder, const QString& key)
{
    const QJsonObject range = wireObj(holder, key);
    auto side = [&range](const QString& name) {
        const QJsonObject p = wireObj(range, name);
        return QPoint(int(wireInt(p, QStringLiteral("character"))),
                      int(wireInt(p, QStringLiteral("line"))));
    };
    return {side(QStringLiteral("start")), side(QStringLiteral("end"))};
}

// THE ONE Location decode — definition, references and the outline's flat
// rows all read it (the reply-shape law frozen at the hub's definition door:
// Location | Location[] | LocationLink[] | null, plus SymbolInformation's
// {location} holder). Selection-range preference:
// targetSelectionRange > selectionRange > location > targetRange > range.
// An entry with no uri is INVALID — and invalid NEVER degrades into a zero
// position posing as (0,0).
struct WireLocation {
    QString uri;     // the raw wire uri ("" = invalid)
    QPoint selStart; // (character, line) — the wire frame
    QPoint selEnd;   //   "   "
    bool valid = false;
};

WireLocation decodeWireLocation(const QJsonValue& value)
{
    WireLocation out;
    if (!value.isObject())
        return out;
    const QJsonObject obj = value.toObject();
    out.uri = wireStr(obj, QStringLiteral("uri"));
    if (out.uri.isEmpty())
        out.uri = wireStr(obj, QStringLiteral("targetUri"));
    if (out.uri.isEmpty())
        return out; // no frame to decode in at all
    const QJsonObject location = wireObj(obj, QStringLiteral("location"));
    const QJsonObject selection
        = wireObj(obj, QStringLiteral("targetSelectionRange"));
    const QJsonObject plainSel = wireObj(obj, QStringLiteral("selectionRange"));
    const QJsonObject target = wireObj(obj, QStringLiteral("targetRange"));
    const QJsonObject plain = wireObj(obj, QStringLiteral("range"));
    QPair<QPoint, QPoint> range;
    if (!selection.isEmpty())
        range = wireRange(obj, QStringLiteral("targetSelectionRange"));
    else if (!plainSel.isEmpty())
        range = wireRange(obj, QStringLiteral("selectionRange"));
    else if (!location.isEmpty())
        range = wireRange(location, QStringLiteral("range"));
    else if (!target.isEmpty())
        range = wireRange(obj, QStringLiteral("targetRange"));
    else
        range = wireRange(obj, QStringLiteral("range"));
    out.selStart = range.first;
    out.selEnd = range.second;
    out.valid = true;
    return out;
}

// A file:// uri -> a local path (`localOut` set); any other scheme keeps the
// uri as the DISPLAY string and leaves localOut empty (a foreign location
// still SHOWS its row; it just cannot be opened — the reveal says so).
QString pathFromUri(const QString& uri, QString* localOut)
{
    const QUrl url(uri);
    if (url.isLocalFile()) {
        *localOut = url.toLocalFile();
        return *localOut;
    }
    localOut->clear();
    return uri;
}

// A preview of one line of a decode frame, truncated at the SEARCH engine's
// own cap with the same trailing ellipsis: the results panel's rows are ONE
// currency (FileMatch/LineMatch), so a reference row never dresses better
// than a search row.
QString previewOfLine(const QString& frame, int line)
{
    const QString text = frame.section(QLatin1Char('\n'), line, line);
    if (text.size() <= qmn::search::kPreviewCap)
        return text;
    return text.left(qmn::search::kPreviewCap - 1) + QChar(0x2026);
}

} // namespace

// ------------------------------------------------------------ the arm door
void IdeWindow::armLspServer(const QString& specName, const QString& program,
                             const QStringList& args,
                             const QStringList& languages)
{
    // The TEST-ARM door — the owner-granted fakeserver-class shape (2026
    // 11 28): an explicit program path, the far side's feature flags as
    // args, and the hub raised over it. setLspEnabled alone cannot drive
    // these faces offscreen: the built-in spec claims PATH clangd, which
    // tst_ide never spawns (that law STANDS for every other slot). Built-in
    // matrix users still call setLspEnabled; this door is additive app-side
    // only — the shelf saw nothing new.
    if (!hub_)
        setLspEnabled(true);
    qmn::glue::LspHub::ServerSpec spec;
    spec.name = specName;
    spec.program = program;
    spec.args = args;
    spec.languages = languages;
    hub_->setServerSpec(spec); // the hub's language map routes the claim
    statusBar()->showMessage(QStringLiteral("LSP: \u0442\u0435\u0441\u0442\u043e\u0432\u044b\u0439 \u0441\u043f\u0435\u043a ")
                                 + program);
}

// ------------------------------------------------------ I3-3: definition
bool IdeWindow::jumpToDefinition()
{
    CodeEditor* editor = currentEditor();
    jumpTarget_.clear();
    if (!hub_ || !editor) {
        jumpSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0445\u043e\u0434: \u043d\u0435\u0442 \u0430\u043a\u0442\u0438\u0432\u043d\u043e\u0433\u043e \u0440\u0435\u0434\u0430\u043a\u0442\u043e\u0440\u0430 \u0438\u043b\u0438 LSP-\u043a\u043b\u044e\u0447\u0430");
        statusBar()->showMessage(jumpSummary_);
        return false;
    }
    qmn::lsp::DocumentSyncer* syncer = hub_->syncerFor(editor);
    const QString frame = syncer ? syncer->mirrorText() : QString();
    qmn::lsp::LspClient* client = hub_->clientForUri(session_->uriForEditor(editor));
    const QString wireEncoding = client ? client->positionEncoding() : QString();
    const qint64 tag = hub_->requestDefinition(
        editor, editor->position(),
        [this, frame, wireEncoding](const qmn::protocol::RpcClient::Reply& r) {
            onDefinitionAnswer(r.result, r.isError, r.code, r.message,
                               frame, wireEncoding);
        });
    if (tag == 0) {
        // The refusal fired the handler SYNCHRONOUSLY above: jumpSummary_
        // already holds the machine reason. Re-showing it is fine; writing
        // a fresh text over it is not.
        statusBar()->showMessage(jumpSummary_);
        return false;
    }
    jumpSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0445\u043e\u0434: \u0437\u0430\u043f\u0440\u043e\u0448\u0435\u043d\u2026");
    statusBar()->showMessage(jumpSummary_);
    return true;
}

void IdeWindow::onDefinitionAnswer(const QJsonValue& result, bool isError,
                                   int code, const QString& message,
                                   const QString& mirrorFrame,
                                   const QString& wireEncoding)
{
    if (isError) {
        jumpSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0445\u043e\u0434 \u043e\u0442\u043a\u043b\u043e\u043d\u0451\u043d: \u043a\u043e\u0434 %1 \u00b7 %2").arg(code).arg(message);
        statusBar()->showMessage(jumpSummary_);
        return;
    }
    if (result.isNull()) {
        jumpSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0445\u043e\u0434: \u0441\u0435\u0440\u0432\u0435\u0440 \u043e\u0442\u0432\u0435\u0442\u0438\u043b \u043f\u0443\u0441\u0442\u043e");
        statusBar()->showMessage(jumpSummary_);
        return;
    }
    jumpTarget_ = decodeJumpTargets(result, mirrorFrame, wireEncoding);
    if (jumpTarget_.isEmpty()) {
        jumpSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0445\u043e\u0434: \u0432 \u043e\u0442\u0432\u0435\u0442\u0435 \u043d\u0435\u0442 \u043c\u0435\u0441\u0442\u043e\u043f\u043e\u043b\u043e\u0436\u0435\u043d\u0438\u0439");
        statusBar()->showMessage(jumpSummary_);
        return;
    }
    const bool revealed = revealJumpTarget(jumpTarget_.first(), true);
    jumpSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0445\u043e\u0434: %1:%2")
                       .arg(QFileInfo(jumpTarget_.first().path).fileName())
                       .arg(jumpTarget_.first().startLine + 1);
    if (jumpTarget_.size() > 1)
        jumpSummary_ += QStringLiteral(" \u00b7 \u0432\u0430\u0440\u0438\u0430\u043d\u0442\u043e\u0432: %1").arg(jumpTarget_.size());
    if (!revealed)
        jumpSummary_ += QStringLiteral(" \u00b7 \u043e\u0442\u043a\u0440\u044b\u0442\u044c \u043d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c");
    statusBar()->showMessage(jumpSummary_);
}

QVector<IdeWindow::JumpTarget>
IdeWindow::decodeJumpTargets(const QJsonValue& result,
                             const QString& mirrorFrame,
                             const QString& wireEncoding) const
{
    const auto enc = qmn::lsp::PositionConverter::fromWire(wireEncoding);
    const QString currentPath
        = tabs_->currentIndex() >= 0
              ? tabs_->tabBar()->tabData(tabs_->currentIndex()).toString()
              : QString();
    QHash<QString, QString> frames;
    frames.insert(currentPath, mirrorFrame);
    auto frameFor = [&frames, &mirrorFrame](const QString& path) {
        if (frames.contains(path))
            return frames.value(path);
        if (path.isEmpty())
            return mirrorFrame;
        QString error;
        // A CLOSED file: the disk IS the text the server holds — decoding
        // its positions against anything else would drift every column
        // after a non-ASCII character on the line (clangd answers utf-8).
        const QString disk = readTextFile(path, &error);
        frames.insert(path, disk); // an unreadable file decodes against ""
        return disk;               // (a total clamp, never a crash)
    };
    QVector<JumpTarget> out;
    const auto addOne = [&](const QJsonValue& v) {
        const WireLocation w = decodeWireLocation(v);
        if (!w.valid)
            return;
        QString localPath;
        pathFromUri(w.uri, &localPath); // "" for an external scheme
        const QString frame = frameFor(localPath);
        JumpTarget t;
        t.path = localPath;
        const qmn::lsp::ModelPosition s = qmn::lsp::PositionConverter::
            toModelPosition(frame, w.selStart.y(), w.selStart.x(), enc);
        const qmn::lsp::ModelPosition e = qmn::lsp::PositionConverter::
            toModelPosition(frame, w.selEnd.y(), w.selEnd.x(), enc);
        t.startLine = s.line;
        t.startCol = s.col;
        t.endLine = e.line;
        t.endCol = e.col;
        out.append(t);
    };
    if (result.isArray()) {
        for (const QJsonValue& v : result.toArray())
            addOne(v);
    } else {
        addOne(result);
    }
    return out;
}

bool IdeWindow::revealJumpTarget(const JumpTarget& target, bool caretAtStart)
{
    if (!target.path.isEmpty()) {
        if (!QFileInfo(target.path).isFile())
            return false; // the answer named a file that is gone (or a
                          // foreign path) — loud via the jump summary
        if (!openFile(target.path))
            return false; // openFile says its own truth in the bar
    }
    CodeEditor* editor = currentEditor();
    if (!editor)
        return false;
    qmn::view::CodeDocument* doc = editor->document();
    const int lines = doc->lineCount();
    auto clampTo = [doc, lines](int line, int col) {
        line = qBound(0, line, lines - 1);
        TextPosition pos;
        pos.line = line;
        pos.col = qBound(0, col, int(doc->lineText(line).size()));
        return pos;
    };
    const TextPosition start = clampTo(target.startLine, target.startCol);
    const TextPosition end = clampTo(target.endLine, target.endCol);
    // THE TWO CARET FACES, one public door (the I1 widening): a JUMP puts
    // the caret at the token START — the next keystroke belongs to the
    // symbol's front; a PANEL/OUTLINE reveal puts it at the END (I1's
    // frozen shape). Reversed ends are legal — setSelection sorts them.
    if (caretAtStart) {
        editor->setPosition(start);
        editor->setSelection(end, start);
    } else {
        editor->setPosition(start);
        editor->setSelection(start, end);
    }
    editor->setFocus();
    return true;
}

QString IdeWindow::wordAtColumn(const QString& line, int col)
{
    auto isWord = [](QChar c) {
        return c.isLetterOrNumber() || c == QLatin1Char('_')
               || c == QLatin1Char('$');
    };
    if (col < 0 || col >= line.size() || !isWord(line.at(col)))
        return {};
    int start = col;
    while (start > 0 && isWord(line.at(start - 1)))
        --start;
    int end = col;
    while (end < line.size() && isWord(line.at(end)))
        ++end;
    return line.mid(start, end - start);
}

// ------------------------------------------------------ I3-3: references
bool IdeWindow::findReferences()
{
    CodeEditor* editor = currentEditor();
    if (!hub_ || !editor) {
        resultsProducer_.clear();
        searchSummary_ = QStringLiteral("\u0441\u0441\u044b\u043b\u043a\u0438: \u043d\u0435\u0442 \u0430\u043a\u0442\u0438\u0432\u043d\u043e\u0433\u043e \u0440\u0435\u0434\u0430\u043a\u0442\u043e\u0440\u0430 \u0438\u043b\u0438 LSP-\u043a\u043b\u044e\u0447\u0430");
        statusBar()->showMessage(searchSummary_);
        return false;
    }
    qmn::lsp::DocumentSyncer* syncer = hub_->syncerFor(editor);
    const QString frame = syncer ? syncer->mirrorText() : QString();
    qmn::lsp::LspClient* client = hub_->clientForUri(session_->uriForEditor(editor));
    const QString wireEncoding = client ? client->positionEncoding() : QString();
    const qint64 tag = hub_->requestReferences(
        editor, editor->position(), true,
        [this, frame, wireEncoding](const qmn::protocol::RpcClient::Reply& r) {
            onReferencesAnswer(r.result, r.isError, r.code, r.message,
                               frame, wireEncoding);
        });
    if (tag == 0) {
        resultsProducer_ = QStringLiteral("references");
        statusBar()->showMessage(searchSummary_); // the refusal, verbatim
        return false;
    }
    resultsProducer_ = QStringLiteral("references");
    searchSummary_ = QStringLiteral("\u0441\u0441\u044b\u043b\u043a\u0438: \u0437\u0430\u043f\u0440\u043e\u0448\u0435\u043d\u044b\u2026");
    statusBar()->showMessage(searchSummary_);
    return true;
}

void IdeWindow::onReferencesAnswer(const QJsonValue& result, bool isError,
                                   int code, const QString& message,
                                   const QString& mirrorFrame,
                                   const QString& wireEncoding)
{
    resultsProducer_ = QStringLiteral("references");
    if (isError) {
        showResultsFor({}, QStringLiteral("references"),
                       QStringLiteral("\u0441\u0441\u044b\u043b\u043a\u0438 \u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u044b: \u043a\u043e\u0434 %1 \u00b7 %2").arg(code).arg(message));
        return;
    }
    if (result.isNull()) {
        showResultsFor({}, QStringLiteral("references"),
                       QStringLiteral("\u0441\u0441\u044b\u043b\u043e\u043a \u043d\u0435\u0442: \u0441\u0435\u0440\u0432\u0435\u0440 \u043e\u0442\u0432\u0435\u0442\u0438\u043b \u043f\u0443\u0441\u0442\u043e"));
        return;
    }
    const QVector<qmn::search::FileMatch> grouped
        = groupReferences(result, mirrorFrame, wireEncoding);
    int hits = 0;
    for (const qmn::search::FileMatch& fm : grouped)
        hits += fm.lines.size();
    if (grouped.isEmpty()) {
        showResultsFor({}, QStringLiteral("references"),
                       QStringLiteral("\u0441\u0441\u044b\u043b\u043e\u043a \u043d\u0435\u0442 \u00b7 \u043d\u0435\u0433\u043e\u0434\u043d\u044b\u0445 \u0437\u0430\u043f\u0438\u0441\u0435\u0439 \u043e\u0442\u0432\u0435\u0442\u0430: %1")
                           .arg(result.isArray() ? result.toArray().size()
                                                 : 0));
        return;
    }
    showResultsFor(grouped, QStringLiteral("references"),
                   QStringLiteral("\u0421\u0441\u044b\u043b\u043e\u043a: %1 \u0432 %2 \u0444\u0430\u0439\u043b\u0430\u0445")
                       .arg(hits)
                       .arg(grouped.size()));
}

QVector<qmn::search::FileMatch>
IdeWindow::groupReferences(const QJsonValue& result,
                           const QString& mirrorFrame,
                           const QString& wireEncoding) const
{
    const auto enc = qmn::lsp::PositionConverter::fromWire(wireEncoding);
    const QString currentPath
        = tabs_->currentIndex() >= 0
              ? tabs_->tabBar()->tabData(tabs_->currentIndex()).toString()
              : QString();
    QHash<QString, QString> frames;
    frames.insert(currentPath, mirrorFrame);
    auto frameFor = [&frames, &mirrorFrame](const QString& path) {
        if (frames.contains(path))
            return frames.value(path);
        if (path.isEmpty())
            return mirrorFrame;
        QString error;
        const QString disk = readTextFile(path, &error);
        frames.insert(path, disk);
        return disk;
    };
    // Files in FIRST-APPEARANCE order of the server's own list; rows inside
    // a file keep the SERVER's order (the panel's walk-order honesty, the
    // I1 shape: the producer's order is the row order, never a re-sort).
    QVector<qmn::search::FileMatch> out;
    QHash<QString, int> fileRow;
    const auto addOne = [&](const QJsonValue& v) {
        const WireLocation w = decodeWireLocation(v);
        if (!w.valid)
            return; // a location that names no frame: it never becomes a
                    // (0,0) phantom row
        QString localPath;
        pathFromUri(w.uri, &localPath);
        const QString frame = frameFor(localPath);
        const qmn::lsp::ModelPosition s = qmn::lsp::PositionConverter::
            toModelPosition(frame, w.selStart.y(), w.selStart.x(), enc);
        const qmn::lsp::ModelPosition e = qmn::lsp::PositionConverter::
            toModelPosition(frame, w.selEnd.y(), w.selEnd.x(), enc);
        const QString key = localPath.isEmpty() ? w.uri : localPath;
        int index = -1;
        auto it = fileRow.constFind(key);
        if (it == fileRow.constEnd()) {
            index = out.size();
            fileRow.insert(key, index);
            qmn::search::FileMatch fm;
            fm.filePath = localPath.isEmpty() ? w.uri : localPath;
            out.append(fm);
        } else {
            index = it.value();
        }
        qmn::search::LineMatch lm;
        lm.line = s.line;
        qmn::search::MatchSpan span;
        span.startCol = s.col;
        span.endCol = e.col;
        lm.spans.append(span);
        lm.lineText = previewOfLine(frame, s.line);
        out[index].lines.append(lm);
    };
    if (result.isArray()) {
        for (const QJsonValue& v : result.toArray())
            addOne(v);
    } else {
        addOne(result);
    }
    return out;
}

void IdeWindow::showResultsFor(const QVector<qmn::search::FileMatch>& grouped,
                               const QString& producer, const QString& summary)
{
    resultsProducer_ = producer;
    results_.clear();
    resultRows_.clear();
    resultsTree_->clear();
    problemRowMap_.clear(); // car I4: the row→problem map is the PRODUCER's
                            // business (only the problems feed fills it)
    for (const qmn::search::FileMatch& fm : grouped)
        appendResultRows(fm); // the I1 door: file groups + per-line leaves,
                              // row-aligned with resultRows_ — a reference
                              // row activates through the SAME
                              // activateSearchResult as a search row
    searchSummary_ = summary;
    statusBar()->showMessage(summary);
}

// ---------------------------------------------------------- I3-4: rename
bool IdeWindow::openRenameOverlay()
{
    CodeEditor* editor = currentEditor();
    if (!hub_ || !editor) {
        renameSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u043d\u0438\u0435: \u043d\u0435\u0442 \u0430\u043a\u0442\u0438\u0432\u043d\u043e\u0433\u043e \u0440\u0435\u0434\u0430\u043a\u0442\u043e\u0440\u0430 \u0438\u043b\u0438 LSP-\u043a\u043b\u044e\u0447\u0430");
        statusBar()->showMessage(renameSummary_);
        return false;
    }
    // The prefill is the app's own math over public model doors: the
    // selection if there is one, else the word AT the caret (no new view
    // door — the LEDGER stays clean).
    QString prefill;
    if (editor->hasSelection())
        prefill = editor->selectedText();
    if (prefill.isEmpty()) {
        const TextPosition caret = editor->position();
        prefill = wordAtColumn(editor->document()->lineText(caret.line),
                               caret.col);
    }
    renameEdit_->setText(prefill);
    renameEdit_->selectAll();
    showOverlay(renamePop_, renameEdit_);
    renameSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u043d\u0438\u0435: \u0432\u0432\u0435\u0434\u0438\u0442\u0435 \u0438\u043c\u044f \u0438 Enter");
    statusBar()->showMessage(renameSummary_);
    return true;
}

bool IdeWindow::renameOverlayVisible() const
{
    return renamePop_ && renamePop_->isVisible();
}

QString IdeWindow::renamePrefill() const
{
    return renameEdit_ ? renameEdit_->text() : QString();
}

void IdeWindow::commitRename(const QString& newName)
{
    CodeEditor* editor = currentEditor();
    if (!hub_ || !editor) {
        renameSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u043d\u0438\u0435: \u043d\u0435\u0442 \u0430\u043a\u0442\u0438\u0432\u043d\u043e\u0433\u043e \u0440\u0435\u0434\u0430\u043a\u0442\u043e\u0440\u0430 \u0438\u043b\u0438 LSP-\u043a\u043b\u044e\u0447\u0430");
        statusBar()->showMessage(renameSummary_);
        return;
    }
    if (newName.trimmed().isEmpty()) {
        // LOUD, and the overlay STAYS open: the name is the user's, not
        // ours to invent (the quick-open empty-query face's sibling).
        renameSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u043d\u0438\u0435: \u043f\u0443\u0441\u0442\u043e\u0435 \u0438\u043c\u044f \u2014 \u0437\u0430\u043f\u0440\u043e\u0441 \u043d\u0435 \u043e\u0442\u043f\u0440\u0430\u0432\u043b\u0435\u043d");
        statusBar()->showMessage(renameSummary_);
        return;
    }
    const QString uri = session_->uriForEditor(editor);
    const qint64 tag = hub_->requestRename(
        editor, editor->position(), newName,
        [this, uri](const qmn::protocol::RpcClient::Reply& r) {
            onRenameAnswer(r.result, r.isError, r.code, r.message, uri);
        });
    if (tag == 0) {
        statusBar()->showMessage(renameSummary_); // the refusal, verbatim
        return;                                   // the overlay stays open
    }
    renameSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u043d\u0438\u0435: \u0437\u0430\u043f\u0440\u043e\u0448\u0435\u043d\u043e\u2026");
    statusBar()->showMessage(renameSummary_);
}

void IdeWindow::onRenameAnswer(const QJsonValue& result, bool isError, int code,
                               const QString& message, const QString& docUri)
{
    if (isError) {
        renameSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u043d\u0438\u0435 \u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u043e: \u043a\u043e\u0434 %1 \u00b7 %2")
                             .arg(code).arg(message);
        statusBar()->showMessage(renameSummary_); // the overlay stays OPEN —
                                                  // the next name may yet be
                                                  // the truth
        return;
    }
    if (result.isNull()) {
        // The server's OWN cannot-rename face (prepareRename's null): no
        // applier runs, nothing changes.
        renameSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u043d\u0438\u0435: \u0441\u0435\u0440\u0432\u0435\u0440 \u043e\u0442\u0432\u0435\u0442\u0438\u043b \u043e\u0442\u043a\u0430\u0437\u043e\u043c null");
        statusBar()->showMessage(renameSummary_);
        return;
    }
    qmn::lsp::LspClient* client = hub_->clientForUri(docUri);
    const qmn::glue::LspHub::ApplyReport report
        = hub_->applyReplyEdit(result, client); // ONE undo group per document
                                                // (the applier's frozen law —
                                                // the app never hand-rolls
                                                // the edit walk)
    if (report.applied) {
        renameSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u043d\u0438\u0435: \u043f\u0440\u0438\u043c\u0435\u043d\u0435\u043d\u043e \u00b7 \u043f\u0440\u0430\u0432\u043e\u043a: %1 \u00b7 \u0434\u043e\u043a\u0443\u043c\u0435\u043d\u0442\u043e\u0432: %2")
                             .arg(report.editsApplied)
                             .arg(report.documentsChanged);
        closeOverlay();
    } else {
        renameSummary_ = QStringLiteral("\u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u043d\u0438\u0435 \u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u043e: %1")
                             .arg(report.refused.join(QStringLiteral("; ")));
    }
    statusBar()->showMessage(renameSummary_);
}

// --------------------------------------------------- I3-4: code actions
bool IdeWindow::showCodeActions()
{
    CodeEditor* editor = currentEditor();
    if (!hub_ || !editor) {
        actionsSummary_ = QStringLiteral("\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u044f: \u043d\u0435\u0442 \u0430\u043a\u0442\u0438\u0432\u043d\u043e\u0433\u043e \u0440\u0435\u0434\u0430\u043a\u0442\u043e\u0440\u0430 \u0438\u043b\u0438 LSP-\u043a\u043b\u044e\u0447\u0430");
        statusBar()->showMessage(actionsSummary_);
        return false;
    }
    codeActions_.clear();
    actionsList_->clear();
    const qint64 tag = hub_->requestCodeActions(
        editor, editor->position(),
        [this](const qmn::protocol::RpcClient::Reply& r) {
            onCodeActionsAnswer(r.result, r.isError, r.code, r.message);
        });
    if (tag == 0) {
        statusBar()->showMessage(actionsSummary_); // the refusal, verbatim
        return false;
    }
    actionsSummary_ = QStringLiteral("\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u044f: \u0437\u0430\u043f\u0440\u043e\u0448\u0435\u043d\u044b\u2026");
    statusBar()->showMessage(actionsSummary_);
    return true;
}

void IdeWindow::onCodeActionsAnswer(const QJsonValue& result, bool isError,
                                    int code, const QString& message)
{
    if (isError) {
        actionsSummary_ = QStringLiteral("\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u044f \u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u044b: \u043a\u043e\u0434 %1 \u00b7 %2")
                              .arg(code).arg(message);
        statusBar()->showMessage(actionsSummary_);
        return;
    }
    if (!result.isArray()) {
        actionsSummary_ = result.isNull()
                              ? QStringLiteral("\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0439 \u043d\u0435\u0442: \u0441\u0435\u0440\u0432\u0435\u0440 \u043e\u0442\u0432\u0435\u0442\u0438\u043b \u043f\u0443\u0441\u0442\u043e")
                              : QStringLiteral("\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u044f: \u043d\u0435\u043e\u0436\u0438\u0434\u0430\u043d\u043d\u044b\u0439 \u043e\u0442\u0432\u0435\u0442 \u2014 \u043d\u0435 \u0441\u043f\u0438\u0441\u043e\u043a");
        statusBar()->showMessage(actionsSummary_);
        return;
    }
    for (const QJsonValue& v : result.toArray()) {
        codeActions_.append(v);
        const QString title
            = v.isObject() ? wireStr(v.toObject(), QStringLiteral("title"))
                           : QString();
        actionsList_->addItem(title.isEmpty()
                                  ? QStringLiteral("\u0437\u0430\u043f\u0438\u0441\u044c \u0431\u0435\u0437 \u043d\u0430\u0437\u0432\u0430\u043d\u0438\u044f")
                                  : title);
    }
    actionsSummary_ = QStringLiteral("\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0439: %1").arg(codeActions_.size());
    statusBar()->showMessage(actionsSummary_);
    if (!codeActions_.isEmpty())
        showActionsOverlay(); // the rows EXIST even with no popup — the
                              // count/label/run doors are the driving face
}

void IdeWindow::showActionsOverlay()
{
    closeOverlay(); // one overlay at a time
    const int w = qMax(360, qMin(680, static_cast<int>(width() * 0.6)));
    const int h = qMax(220, qMin(460, static_cast<int>(height() * 0.55)));
    actionsPop_->setGeometry((width() - w) / 2,
                             static_cast<int>(height() * 0.2), w, h);
    actionsPop_->show();
    actionsPop_->raise();
    actionsList_->setFocus();
}

int IdeWindow::codeActionRowCount() const
{
    return codeActions_.size();
}

QString IdeWindow::codeActionLabelAt(int row) const
{
    if (row < 0 || row >= codeActions_.size()
        || !codeActions_.at(row).isObject())
        return {};
    const QString title
        = wireStr(codeActions_.at(row).toObject(), QStringLiteral("title"));
    return title; // "" for an untitled entry — the row still RANKS, it is
                  // just honest about having no label
}

bool IdeWindow::runCodeAction(int row)
{
    if (row < 0 || row >= codeActions_.size())
        return false;
    if (!hub_) {
        actionsSummary_ = QStringLiteral("\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0435: LSP-\u043a\u043b\u044e\u0447 \u0443\u0436\u0435 \u043e\u043f\u0443\u0449\u0435\u043d");
        statusBar()->showMessage(actionsSummary_);
        return false;
    }
    const QJsonValue v = codeActions_.at(row);
    if (!v.isObject()) {
        actionsSummary_ = QStringLiteral("\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0435 \u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u043e: \u0437\u0430\u043f\u0438\u0441\u044c \u043e\u0442\u0432\u0435\u0442\u0430 \u043d\u0435 \u043e\u0431\u044a\u0435\u043a\u0442");
        statusBar()->showMessage(actionsSummary_);
        return false;
    }
    const QJsonObject action = v.toObject();
    const QString docUri = wireStr(action, QStringLiteral("documentUri"));
    const QString uri = docUri.isEmpty()
        ? (currentEditor() ? session_->uriForEditor(currentEditor()) : QString())
        : docUri;
    qmn::lsp::LspClient* client = hub_->clientForUri(uri);
    // applyCodeAction is the applier's own door: the item's `edit`, or the
    // WorkspaceEdit hidden in command.arguments[0] (clangd's tweak shape).
    // An item that is PURELY a workspace command refuses THROUGH THE
    // APPLIER with its reason — v1 never executes a server's arbitrary
    // command (PLAN's v1 stance, and the refusal text is the record).
    const qmn::glue::LspHub::ApplyReport report
        = hub_->applyCodeAction(action, client);
    if (report.applied) {
        actionsSummary_ = QStringLiteral("\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0435 \u043f\u0440\u0438\u043c\u0435\u043d\u0435\u043d\u043e: \u043f\u0440\u0430\u0432\u043e\u043a %1 \u00b7 \u0434\u043e\u043a\u0443\u043c\u0435\u043d\u0442\u043e\u0432 %2")
                              .arg(report.editsApplied)
                              .arg(report.documentsChanged);
        closeOverlay();
    } else {
        actionsSummary_ = QStringLiteral("\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0435 \u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u043e: %1")
                              .arg(report.refused.join(QStringLiteral("; ")));
    }
    statusBar()->showMessage(actionsSummary_);
    return report.applied;
}

// ------------------------------------------------------- I3-4: formatting
bool IdeWindow::formatDocument()
{
    CodeEditor* editor = currentEditor();
    if (!hub_ || !editor) {
        formatSummary_ = QStringLiteral("\u0444\u043e\u0440\u043c\u0430\u0442\u0438\u0440\u043e\u0432\u0430\u043d\u0438\u0435: \u043d\u0435\u0442 \u0430\u043a\u0442\u0438\u0432\u043d\u043e\u0433\u043e \u0440\u0435\u0434\u0430\u043a\u0442\u043e\u0440\u0430 \u0438\u043b\u0438 LSP-\u043a\u043b\u044e\u0447\u0430");
        statusBar()->showMessage(formatSummary_);
        return false;
    }
    const QString uri = session_->uriForEditor(editor);
    const qint64 tag = hub_->requestFormatting(
        editor, [this, uri](const qmn::protocol::RpcClient::Reply& r) {
            onFormatAnswer(r.result, r.isError, r.code, r.message, uri);
        });
    if (tag == 0) {
        statusBar()->showMessage(formatSummary_); // the refusal, verbatim
        return false;
    }
    formatSummary_ = QStringLiteral("\u0444\u043e\u0440\u043c\u0430\u0442\u0438\u0440\u043e\u0432\u0430\u043d\u0438\u0435: \u0437\u0430\u043f\u0440\u043e\u0448\u0435\u043d\u043e\u2026");
    statusBar()->showMessage(formatSummary_);
    return true;
}

void IdeWindow::onFormatAnswer(const QJsonValue& result, bool isError, int code,
                               const QString& message, const QString& docUri)
{
    if (isError) {
        formatSummary_ = QStringLiteral("\u0444\u043e\u0440\u043c\u0430\u0442\u0438\u0440\u043e\u0432\u0430\u043d\u0438\u0435 \u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u043e: \u043a\u043e\u0434 %1 \u00b7 %2")
                             .arg(code).arg(message);
        statusBar()->showMessage(formatSummary_);
        return;
    }
    if (result.isNull()) {
        formatSummary_ = QStringLiteral("\u0444\u043e\u0440\u043c\u0430\u0442\u0438\u0440\u043e\u0432\u0430\u043d\u0438\u0435: \u0441\u0435\u0440\u0432\u0435\u0440 \u043e\u0442\u0432\u0435\u0442\u0438\u043b \u043f\u0443\u0441\u0442\u043e");
        statusBar()->showMessage(formatSummary_);
        return;
    }
    if (!result.isArray()) {
        formatSummary_ = QStringLiteral("\u0444\u043e\u0440\u043c\u0430\u0442\u0438\u0440\u043e\u0432\u0430\u043d\u0438\u0435: \u043d\u0435\u043e\u0436\u0438\u0434\u0430\u043d\u043d\u044b\u0439 \u043e\u0442\u0432\u0435\u0442 \u2014 \u043d\u0435 \u0441\u043f\u0438\u0441\u043e\u043a");
        statusBar()->showMessage(formatSummary_);
        return;
    }
    // The document is resolved BY URI at answer time (never a captured view
    // pointer): a tab closed mid-flight leaves a null here, not a dangling
    // CodeEditor the applier would write into.
    CodeEditor* target = session_->editorForUri(docUri);
    if (!target) {
        formatSummary_ = QStringLiteral("\u0444\u043e\u0440\u043c\u0430\u0442\u0438\u0440\u043e\u0432\u0430\u043d\u0438\u0435: \u0432\u043a\u043b\u0430\u0434\u043a\u0430 \u0434\u043e\u043a\u0443\u043c\u0435\u043d\u0442\u0430 \u0437\u0430\u043a\u0440\u044b\u0442\u0430, \u043f\u043e\u043a\u0430 \u0448\u0451\u043b \u043e\u0442\u0432\u0435\u0442");
        statusBar()->showMessage(formatSummary_);
        return;
    }
    qmn::lsp::LspClient* client = hub_->clientForUri(docUri);
    const qmn::glue::LspHub::ApplyReport report
        = hub_->applyTextEditList(target, result, client); // a BARE TextEdit[]
                                                           // (the formatting
                                                           // shape) — the
                                                           // applier's door,
                                                           // not a hand walk
    if (report.applied) {
        formatSummary_ = QStringLiteral("\u0444\u043e\u0440\u043c\u0430\u0442\u0438\u0440\u043e\u0432\u0430\u043d\u0438\u0435: \u043f\u0440\u0438\u043c\u0435\u043d\u0435\u043d\u043e \u00b7 \u043f\u0440\u0430\u0432\u043e\u043a: %1")
                             .arg(report.editsApplied);
    } else {
        formatSummary_ = QStringLiteral("\u0444\u043e\u0440\u043c\u0430\u0442\u0438\u0440\u043e\u0432\u0430\u043d\u0438\u0435 \u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u043e: %1")
                             .arg(report.refused.join(QStringLiteral("; ")));
    }
    statusBar()->showMessage(formatSummary_);
}

// --------------------------------------------------------- I3-5: outline
void IdeWindow::buildOutlineDock()
{
    outlineDock_ = new QDockWidget(QStringLiteral("\u0421\u0438\u043c\u0432\u043e\u043b\u044b"), this);
    auto* panel = new QWidget(outlineDock_);
    auto* lay = new QVBoxLayout(panel);
    auto* head = new QHBoxLayout();
    outlineRefreshBtn_ = new QPushButton(QStringLiteral("\u041e\u0431\u043d\u043e\u0432\u0438\u0442\u044c"), panel);
    outlineSummaryLabel_ = new QLabel(QStringLiteral("\u0441\u0438\u043c\u0432\u043e\u043b\u044b: LSP-\u0441\u0435\u0440\u0432\u0435\u0440 \u043d\u0435 \u043f\u043e\u0434\u043a\u043b\u044e\u0447\u0451\u043d"), panel);
    outlineSummaryLabel_->setWordWrap(true);
    head->addWidget(outlineRefreshBtn_);
    head->addWidget(outlineSummaryLabel_, 1);
    lay->addLayout(head);
    outlineTree_ = new QTreeWidget(panel);
    outlineTree_->setHeaderLabels({QStringLiteral("\u0421\u0438\u043c\u0432\u043e\u043b"), QStringLiteral("\u0421\u0442\u0440\u043e\u043a\u0430")});
    outlineTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    outlineTree_->setRootIsDecorated(true);
    connect(outlineTree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                const QVariant row = item->data(0, kOutlineRowRole);
                if (row.isValid())
                    outlineActivate(row.toInt());
            });
    lay->addWidget(outlineTree_, 1);
    outlineDock_->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, outlineDock_);
    connect(outlineRefreshBtn_, &QPushButton::clicked, this,
            [this] {
                outlineDebounce_->stop(); // a manual pull outranks a pending
                                          // debounce — one ask, no queue
                refreshOutline();
            });
}

void IdeWindow::refreshOutline()
{
    ++outlineGeneration_; // EVERY refresh retires the pulls already on the
                          // wire (a late answer for another tab or an older
                          // text never paints over the newest ask)
    outlineBusy_ = false;
    CodeEditor* editor = currentEditor();
    if (!hub_ || !editor) {
        clearOutlineRows(QStringLiteral("\u0441\u0438\u043c\u0432\u043e\u043b\u044b: LSP-\u0441\u0435\u0440\u0432\u0435\u0440 \u043d\u0435 \u043f\u043e\u0434\u043a\u043b\u044e\u0447\u0451\u043d"));
        return;
    }
    qmn::lsp::DocumentSyncer* syncer = hub_->syncerFor(editor);
    const QString frame = syncer ? syncer->mirrorText() : QString();
    qmn::lsp::LspClient* client =
        hub_->clientForUri(session_->uriForEditor(editor));
    const QString wireEncoding = client ? client->positionEncoding() : QString();
    const int generation = outlineGeneration_;
    const qint64 tag = hub_->requestDocumentSymbol(
        editor, [this, generation, frame, wireEncoding](
                    const qmn::protocol::RpcClient::Reply& r) {
            onOutlineAnswer(generation, r.result, r.isError, r.code,
                            r.message, frame, wireEncoding);
        });
    if (tag == 0) {
        // The refusal fired the handler SYNCHRONOUSLY above (either it
        // re-armed the debounce — the still-starting face — or it cleared
        // the rows LOUDLY; a stale tree never poses as current).
        return;
    }
    outlineRetry_ = 0; // an ask that MADE IT onto the wire resets the wait
    outlineBusy_ = true;
    outlineSummary_ = QStringLiteral("\u0441\u0438\u043c\u0432\u043e\u043b\u044b: \u0437\u0430\u043f\u0440\u043e\u0448\u0435\u043d\u044b\u2026");
    outlineSummaryLabel_->setText(outlineSummary_);
}

void IdeWindow::onOutlineAnswer(int generation, const QJsonValue& result,
                                bool isError, int code, const QString& message,
                                const QString& mirrorFrame,
                                const QString& wireEncoding)
{
    if (generation != outlineGeneration_)
        return; // SUPERSEDED: the tab moved or the document moved on while
                // the wire was in flight — painting this would pose an old
                // text as the current one
    outlineBusy_ = false;
    // THE STILL-STARTING FACE (the outline's OWN policy — the tenth door
    // only answers): a CLAIMED document whose server has not finished its
    // handshake is not a truth the user should eat, so the outline re-arms
    // its debounce, bounded by kOutlineMaxRetries, and the face SAYS so. A
    // document NO spec claims never reaches this branch (no syncer), and the
    // cap bounds even a server that never becomes ready — past the cap the
    // refusal face is the honest one.
    if (code == int(qmn::lsp::LspClient::kWireServerNotInitialized)
        && hub_ && currentEditor() && hub_->syncerFor(currentEditor())
        && outlineRetry_ < kOutlineMaxRetries) {
        ++outlineRetry_;
        outlineSummary_ = QStringLiteral("\u0441\u0438\u043c\u0432\u043e\u043b\u044b: \u0441\u0435\u0440\u0432\u0435\u0440 \u0435\u0449\u0451 \u043f\u043e\u0434\u043d\u0438\u043c\u0430\u0435\u0442\u0441\u044f \u00b7 \u043f\u043e\u043f\u044b\u0442\u043a\u0430 %1")
                              .arg(outlineRetry_);
        outlineSummaryLabel_->setText(outlineSummary_);
        outlineDebounce_->start();
        return;
    }
    if (isError) {
        clearOutlineRows(QStringLiteral("\u0441\u0438\u043c\u0432\u043e\u043b\u044b \u043e\u0442\u043a\u043b\u043e\u043d\u0435\u043d\u044b: \u043a\u043e\u0434 %1 \u00b7 %2")
                             .arg(code).arg(message));
        return;
    }
    decodeOutline(mirrorFrame, wireEncoding, result);
}

void IdeWindow::clearOutlineRows(const QString& why)
{
    outlineRows_.clear();
    outlineFlat_.clear();
    if (outlineTree_)
        outlineTree_->clear();
    if (why.isEmpty())
        return;
    outlineSummary_ = why;
    if (outlineSummaryLabel_)
        outlineSummaryLabel_->setText(outlineSummary_);
    // The outline NEVER posts to the status bar: that channel is the SHARED
    // one the project/tab/search faces already own (tst_ide's project-
    // message pin is the tripwire that caught this at the car), while the
    // dock's OWN label is where an outline truth lives — one face, no steal.

}

void IdeWindow::decodeOutline(const QString& mirrorFrame,
                              const QString& wireEncoding,
                              const QJsonValue& result)
{
    outlineRows_.clear();
    outlineFlat_.clear();
    if (!result.isArray()) {
        clearOutlineRows(result.isNull()
                             ? QStringLiteral("\u0441\u0438\u043c\u0432\u043e\u043b\u043e\u0432 \u043d\u0435\u0442: \u0441\u0435\u0440\u0432\u0435\u0440 \u043e\u0442\u0432\u0435\u0442\u0438\u043b \u043f\u0443\u0441\u0442\u043e")
                             : QStringLiteral("\u0441\u0438\u043c\u0432\u043e\u043b\u044b: \u043d\u0435\u043e\u0436\u0438\u0434\u0430\u043d\u043d\u044b\u0439 \u043e\u0442\u0432\u0435\u0442 \u2014 \u043d\u0435 \u0441\u043f\u0438\u0441\u043e\u043a"));
        return;
    }
    const QJsonArray items = result.toArray();
    if (items.isEmpty()) {
        clearOutlineRows(QStringLiteral("\u0441\u0438\u043c\u0432\u043e\u043b\u043e\u0432 \u043d\u0435\u0442: \u0441\u0435\u0440\u0432\u0435\u0440 \u043e\u0442\u0432\u0435\u0442\u0438\u043b \u043f\u0443\u0441\u0442\u043e"));
        return;
    }
    const auto enc = qmn::lsp::PositionConverter::fromWire(wireEncoding);
    // THE TWO WIRE SHAPES, decoded per ENTRY (a conforming server answers
    // DocumentSymbol[] with selectionRange/children; a sloppy one answers
    // SymbolInformation[] with location + containerName DESPITE the client's
    // hierarchicalDocumentSymbolSupport claim — tst_lsp's flat leg proves the
    // door carries that whole shape, this is where it becomes a tree):
    //   * tree entry  -> recursion, children nest UNDER the parent;
    //   * flat entry  -> a parent-less row, adopted by the LAST earlier row
    //     whose name equals containerName (a flat SymbolInformation[] is
    //     preorder, so the parent always precedes its children — the LAST
    //     such row is the innermost one; no containerName match = a root).
    auto decodePos = [this, enc, &mirrorFrame](const QJsonObject& obj,
                                               const QString& key,
                                               int* startLine, int* startCol,
                                               int* endLine, int* endCol) {
        const QPair<QPoint, QPoint> r = wireRange(obj, key);
        const qmn::lsp::ModelPosition s = qmn::lsp::PositionConverter::
            toModelPosition(mirrorFrame, r.first.y(), r.first.x(), enc);
        const qmn::lsp::ModelPosition e = qmn::lsp::PositionConverter::
            toModelPosition(mirrorFrame, r.second.y(), r.second.x(), enc);
        *startLine = s.line;
        *startCol = s.col;
        *endLine = e.line;
        *endCol = e.col;
    };
    bool sawTree = false, sawFlat = false, depthCut = false;
    std::function<int(const QJsonValue&, int)> addTree =
        [&](const QJsonValue& value, int depth) -> int {
        if (!value.isObject())
            return -1;
        const QJsonObject obj = value.toObject();
        OutlineNode node;
        node.name = wireStr(obj, QStringLiteral("name"));
        node.detail = wireStr(obj, QStringLiteral("detail"));
        const int kind = int(wireInt(obj, QStringLiteral("kind"), -1));
        node.kindLabel = kindLabelFor(kind);
        decodePos(obj, QStringLiteral("selectionRange"), &node.startLine,
                  &node.startCol, &node.endLine, &node.endCol);
        const int row = outlineRows_.size();
        outlineRows_.append(node);
        outlineFlat_.append(row); // PREORDER is the read order (row i of the
                                  // read faces IS outlineFlat_[i])
        if (depth < kOutlineMaxDepth) {
            for (const QJsonValue& child :
                 obj.value(QStringLiteral("children")).toArray())
                if (const int childRow = addTree(child, depth + 1);
                    childRow >= 0)
                    outlineRows_[row].childRows.append(childRow);
        } else {
            depthCut = true;
        }
        return row;
    };
    for (const QJsonValue& v : items) {
        if (!v.isObject())
            continue;
        const QJsonObject obj = v.toObject();
        if (obj.contains(QStringLiteral("selectionRange"))
            || obj.contains(QStringLiteral("children"))) {
            sawTree = true;
            addTree(v, 0);
            continue;
        }
        sawFlat = true;
        OutlineNode node;
        node.name = wireStr(obj, QStringLiteral("name"));
        const QJsonObject loc = wireObj(obj, QStringLiteral("location"));
        const int kind = int(wireInt(obj, QStringLiteral("kind"), -1));
        node.kindLabel = kindLabelFor(kind);
        decodePos(loc, QStringLiteral("range"), &node.startLine, &node.startCol,
                  &node.endLine, &node.endCol);
        // The FLAT answer carries no detail; a non-spec extra member (the
        // fake's "hover" witness) is simply NOT read — the door carried it,
        // the outline has no use for it, and nothing about it is invented.
        const int row = outlineRows_.size();
        outlineRows_.append(node);
        outlineFlat_.append(row);
        const QString parentName = wireStr(obj, QStringLiteral("containerName"));
        if (!parentName.isEmpty()) {
            for (int i = row - 1; i >= 0; --i) {
                if (outlineRows_.at(i).name == parentName) {
                    outlineRows_[i].childRows.append(row); // LAST match wins:
                                                           // the innermost
                                                           // earlier namesake
                    break;
                }
            }
        }
    }
    rebuildOutlineTree();
    if (outlineRows_.isEmpty()) {
        clearOutlineRows(QStringLiteral("\u0441\u0438\u043c\u0432\u043e\u043b\u043e\u0432 \u043d\u0435\u0442: \u043d\u0438 \u043e\u0434\u043d\u0430 \u0437\u0430\u043f\u0438\u0441\u044c \u043e\u0442\u0432\u0435\u0442\u0430 \u043d\u0435 \u0440\u0430\u0437\u043e\u0431\u0440\u0430\u043d\u0430"));
        return;
    }
    QString mode;
    if (sawTree && sawFlat)
        mode = QStringLiteral(" \u00b7 \u0432 \u043e\u0442\u0432\u0435\u0442\u0435 \u0441\u043c\u0435\u0448\u0430\u043d\u044b \u043e\u0431\u0435 \u0444\u043e\u0440\u043c\u044b");
    else if (sawFlat)
        mode = QStringLiteral(" \u00b7 \u043e\u0442\u0432\u0435\u0442 \u0441\u0435\u0440\u0432\u0435\u0440\u0430 \u043f\u043b\u043e\u0441\u043a\u0438\u0439, \u0438\u0435\u0440\u0430\u0440\u0445\u0438\u044f \u0441\u043e\u0431\u0440\u0430\u043d\u0430 \u043f\u043e containerName");
    else
        mode = QStringLiteral(" \u00b7 \u0438\u0435\u0440\u0430\u0440\u0445\u0438\u044f");
    if (depthCut)
        mode += QStringLiteral(" \u00b7 \u0441\u0440\u0435\u0437\u0430\u043d\u043e \u043f\u043e \u0433\u043b\u0443\u0431\u0438\u043d\u0435 %1").arg(kOutlineMaxDepth);
    outlineSummary_ = QStringLiteral("\u0441\u0438\u043c\u0432\u043e\u043b\u043e\u0432: %1%2")
                          .arg(outlineRows_.size())
                          .arg(mode);
    outlineSummaryLabel_->setText(outlineSummary_); // the dock's face, never
                                                   // the shared bar
}

void IdeWindow::rebuildOutlineTree()
{
    outlineTree_->clear();
    // The QTreeWidget MIRRORS outlineRows_ with the SAME row order the read
    // doors expose (outlineFlat_ is preorder, so a parent row is always
    // built before its children — one pass, no recursion).
    QVector<int> parentOf(outlineRows_.size(), -1);
    for (int row = 0; row < outlineRows_.size(); ++row)
        for (int child : outlineRows_.at(row).childRows)
            parentOf[child] = row;
    QVector<QTreeWidgetItem*> items(outlineRows_.size(), nullptr);
    for (int row : outlineFlat_) {
        const OutlineNode& node = outlineRows_.at(row);
        const int parentRow = parentOf.value(row, -1);
        QTreeWidgetItem* parentItem
            = (parentRow >= 0 && items.value(parentRow)) ? items.at(parentRow)
                                                         : nullptr;
        // A stranded row (its adopter is gone — adoption only ever points
        // BACK to an earlier row, so a cycle is impossible and the miss is a
        // belt, never a routine) still GETS A ROW at the top level: dropping
        // a symbol would erase the server's truth.
        auto* item = parentItem ? new QTreeWidgetItem(parentItem)
                                : new QTreeWidgetItem(outlineTree_);
        QString text = node.kindLabel + QLatin1Char(' ') + node.name;
        if (!node.detail.isEmpty())
            text += QStringLiteral(" \u00b7 ") + node.detail;
        item->setText(0, text);
        item->setText(1, QString::number(node.startLine + 1)); // 1-based
                                                               // DISPLAY (the
                                                               // panel's law)
        item->setData(0, kOutlineRowRole, row);
        items[row] = item;
    }
    outlineTree_->expandAll();
}

int IdeWindow::outlineParentAt(int row) const
{
    // THE NESTING FACE (the flat rebuild's proof: an adoption that never
    // happened shows up here as nothing but -1 rows): the row's ADOPTER,
    // -1 for a root, -2 when the row itself is out of range.
    if (row < 0 || row >= outlineRows_.size())
        return -2; // OUT of range — a value no real row can carry
    for (int candidate = 0; candidate < outlineRows_.size(); ++candidate)
        if (outlineRows_.at(candidate).childRows.contains(row))
            return candidate;
    return -1;
}

int IdeWindow::outlineRowCount() const
{
    return outlineFlat_.size();
}

QString IdeWindow::outlineTextAt(int row) const
{
    if (row < 0 || row >= outlineFlat_.size())
        return {};
    const OutlineNode& node = outlineRows_.at(outlineFlat_.at(row));
    QString text = node.kindLabel + QLatin1Char(' ') + node.name;
    if (!node.detail.isEmpty())
        text += QStringLiteral(" \u00b7 ") + node.detail;
    return text;
}

bool IdeWindow::outlineActivate(int row)
{
    if (row < 0 || row >= outlineFlat_.size())
        return false;
    if (!currentEditor())
        return false; // the outline always describes the CURRENT document —
                      // its rows never claim another file to open
    const OutlineNode& node = outlineRows_.at(outlineFlat_.at(row));
    JumpTarget target;
    target.path = tabs_->tabBar()->tabData(tabs_->currentIndex()).toString();
    target.startLine = node.startLine;
    target.startCol = node.startCol;
    target.endLine = node.endLine;
    target.endCol = node.endCol;
    return revealJumpTarget(target, false); // caret at the END (the panel's
                                            // face; a jump is the START face)
}

QString IdeWindow::outlineSummary() const
{
    return outlineSummary_;
}

QString IdeWindow::jumpSummary() const { return jumpSummary_; }
QString IdeWindow::renameSummary() const { return renameSummary_; }
QString IdeWindow::formatSummary() const { return formatSummary_; }
QString IdeWindow::codeActionsSummary() const { return actionsSummary_; }


// ================= car I4: tasks + the problem matcher =================
//
// The layering note this section has to state (PLAN's opening record): the
// matcher stays APP CODE — one user, and its test surface already rides
// tst_ide's link of qmn_ide_app through the window's own doors, exactly like
// the I1 panel's rows did. No new library, and no search-record widening: the
// rows ride qmn::search's OWN FileMatch/LineMatch/MatchSpan currency and the
// I1 flattening door (appendResultRows) plus the I3 owner door (showResultsFor)
// unchanged.

namespace {

// THE TWO MATCHER SHAPES (v1: the gcc/clang family and MSVC — the declared
// pair; "none" names nothing). tst_ide pins them against tests/faketask's
// TEXT: each mode prints its own family's rows, the noise a real build prints,
// and the OTHER family's line, which must be refused.
//
// gcc:  ^\S.*? : LINE : [COL :] SEVERITY : MESSAGE $
//   * ^\S is load-bearing: an indented caret/continuation line
//     ("      |          ^") never becomes a row;
//   * requiring ":DIGITS:" refuses BOTH "collect2: error: ld returned 1 exit
//     status" (no digits after the colon) and the MSVC shape (whose colon is
//     followed by a space) — the never-invent-a-row law decided by regex
//     arithmetic, not by a name table;
//   * the column group is optional (the no-column gcc shape is real).
constexpr char kGccProblemPattern[] =
    R"(^(\S.*?):(\d+):(?:(\d+):)?\s*(fatal error|error|warning|note):\s*(.*)$)";
// msvc: path(LINE[,COL]): SEVERITY CODE : MESSAGE — the parens and the code
// token are what make a gcc line refuse under THIS pattern (a symmetric
// refusal: the pair never reads each other's shape).
constexpr char kMsvcProblemPattern[] =
    R"(^(\S*?)\((\d+)(?:,(\d+))?\):\s*(fatal error|error|warning|note)\s+([A-Za-z]+\d*)\s*:\s*(.*))";

const QRegularExpression& gccProblemRe()
{
    static const QRegularExpression re(QString::fromLatin1(kGccProblemPattern));
    return re;
}

const QRegularExpression& msvcProblemRe()
{
    static const QRegularExpression re(QString::fromLatin1(kMsvcProblemPattern));
    return re;
}

// The severity table (recorded at the door, PLAN): error AND fatal error are
// the view's Error, warning is Warning, note is Information. The door's face
// is a plain QString; the enum is built only where the M5 feed is written.
QString severityFromWord(const QString& word)
{
    if (word == QLatin1String("warning"))
        return QStringLiteral("warning");
    if (word == QLatin1String("note"))
        return QStringLiteral("information");
    return QStringLiteral("error"); // "error" | "fatal error"
}

CodeEditor::Severity severityToView(const QString& name)
{
    if (name == QLatin1String("warning"))
        return CodeEditor::Severity::Warning;
    if (name == QLatin1String("information"))
        return CodeEditor::Severity::Information;
    return CodeEditor::Severity::Error;
}

} // namespace

bool IdeWindow::parseGccLine(const QString& line, ProblemRow* out)
{
    const QRegularExpressionMatch m = gccProblemRe().match(line);
    if (!m.hasMatch())
        return false;
    bool ok = false;
    const int wireLine = m.captured(2).toInt(&ok);
    if (!ok || wireLine < 1)
        return false; // line 0 is a structural lie, never a row
    out->filePath = m.captured(1);
    out->line = wireLine - 1; // wire 1-based -> model 0-based (the whole
                              // project's convention)
    bool colOk = false;
    const int wireCol = m.captured(3).toInt(&colOk); // absent -> 0, colOk false
    out->startCol = colOk && wireCol >= 1 ? wireCol - 1 : 0;
    // ONE CELL, never an invented extent: the compiler pointed at a COLUMN,
    // and the caret line that WOULD show the extent is noise we refuse to
    // parse. A missing column marks the line's start. The panel's span and the
    // Diagnostic's range both ride their doors' total clamping (a column past
    // the line's length is a drifting reveal, never a corruption — the
    // clamping law the panel already pins).
    out->endCol = out->startCol + 1;
    out->severity = severityFromWord(m.captured(4));
    out->message = m.captured(5); // verbatim, the gcc '[-Wflag]' tail and all
    return true;
}

bool IdeWindow::parseMsvcLine(const QString& line, ProblemRow* out)
{
    const QRegularExpressionMatch m = msvcProblemRe().match(line);
    if (!m.hasMatch())
        return false;
    bool ok = false;
    const int wireLine = m.captured(2).toInt(&ok);
    if (!ok || wireLine < 1)
        return false;
    out->filePath = m.captured(1);
    out->line = wireLine - 1;
    bool colOk = false;
    const int wireCol = m.captured(3).toInt(&colOk);
    out->startCol = colOk && wireCol >= 1 ? wireCol - 1 : 0;
    out->endCol = out->startCol + 1;
    out->severity = severityFromWord(m.captured(4));
    // The MSVC code token (C2065 & friends) is STRUCTURAL — it is folded back
    // into the message rather than dropped (no separate code face in v1).
    out->message = m.captured(5) + QStringLiteral(": ") + m.captured(6);
    return true;
}

void IdeWindow::resolveProblemPath(ProblemRow* row) const
{
    // An ABSOLUTE path is honored; a RELATIVE one resolves against the TASK's
    // cwd (the compiler's own frame of reference — usually the process's, and
    // the task's cwd is exactly what we set and what the far side prints back).
    QString path = row->filePath;
    path = QDir::isAbsolutePath(path)
        ? QDir::cleanPath(path)
        : QDir::cleanPath(QDir(taskCwd_).filePath(path));
    row->filePath = path;
    // A compiler may name a file that is GONE. The row is the answer; the
    // reveal is a separate truth (openFile's own law), so a phantom row stays
    // VISIBLE with this face carrying the difference.
    row->fileExists = QFileInfo(path).isFile();
}

void IdeWindow::ingestTaskLine(const QString& line)
{
    if (taskMatcher_ == QLatin1String("none"))
        return; // "none" names NOTHING: the run still prints, it just claims
                // no problems
    ProblemRow row;
    const bool hit = taskMatcher_ == QLatin1String("msvc")
        ? parseMsvcLine(line, &row)
        : parseGccLine(line, &row);
    if (!hit)
        return; // the pane's text keeps the line (it is output); it is simply
                // not a problem row
    row.matcher = taskMatcher_;
    resolveProblemPath(&row);
    if (problems_.size() >= kProblemFeedMax) {
        ++problemFeedSkipped_; // the FEED cap's honest face: a pathological
        return;                // 200 000-line log never eats the decoration
    }                          // table
    problems_.append(row);
}

// ---------------- the config face (the file IS the door) ----------------

QString IdeWindow::expandRoots(const QString& text) const
{
    QString out = text;
    out.replace(QLatin1String(kWorkspaceRootToken), projectRoot_);
    return out;
}

bool IdeWindow::readTasksConfig()
{
    tasks_.clear();
    taskConfigSummary_.clear();
    if (projectRoot_.isEmpty())
        return false; // no root: nothing to read, nothing to say

    const QString path = QDir(projectRoot_).filePath(QLatin1String(kTasksConfigName));
    if (!QFileInfo(path).isFile())
        return false; // NO FILE is not a refusal — nothing is configured, and
                      // an empty summary is the honest face (the lsp.json
                      // fold's own precedent at this same door)

    QString error;
    const QString text = readTextFile(path, &error);
    if (!error.isEmpty()) {
        taskConfigSummary_ = QStringLiteral("задачи: не прочитать %1 — %2")
                                 .arg(path, error);
        return false;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parse);
    if (doc.isNull() || !doc.isObject()) {
        taskConfigSummary_ = QStringLiteral("задачи: %1 — %2")
                                 .arg(path,
                                      parse.error != QJsonParseError::NoError
                                          ? parse.errorString()
                                          : QStringLiteral("ожидался объект-корень"));
        return false;
    }
    const QJsonObject root = doc.object();
    const auto refuse = [this, &path](const QString& reason) {
        // ONE refusal shape, naming the FILE, and the config reads as NOTHING:
        // a half-registry of tasks is the lie a silent partial read tells (the
        // loud-refusal law this window already froze for the search engine's
        // query and the I3 feature gates).
        taskConfigSummary_ = QStringLiteral("задачи: %1 — %2").arg(path, reason);
        tasks_.clear();
        return false;
    };

    const QJsonValue tasksValue = root.value(QStringLiteral("tasks"));
    if (!tasksValue.isArray())
        return refuse(QStringLiteral("ключ \"tasks\" должен быть списком"));

    QVector<TaskDef> read;
    const QJsonArray arr = tasksValue.toArray();
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonValue value = arr.at(i);
        if (!value.isObject())
            return refuse(QStringLiteral("элемент %1 — не объект").arg(i));
        const QJsonObject obj = value.toObject();
        TaskDef def;
        const QJsonValue label = obj.value(QStringLiteral("label"));
        const QJsonValue command = obj.value(QStringLiteral("command"));
        if (!label.isString() || label.toString().trimmed().isEmpty())
            return refuse(QStringLiteral("элемент %1: нет \"label\"").arg(i));
        if (!command.isString() || command.toString().trimmed().isEmpty())
            return refuse(QStringLiteral("элемент %1 (\"%2\"): нет \"command\"")
                              .arg(i)
                              .arg(label.toString()));
        def.label = label.toString();
        def.command = expandRoots(command.toString());

        const QJsonValue args = obj.value(QStringLiteral("args"));
        if (!args.isUndefined()) {
            if (!args.isArray())
                return refuse(QStringLiteral("элемент %1: \"args\" — не список").arg(i));
            for (const QJsonValue& arg : args.toArray()) {
                if (!arg.isString())
                    return refuse(QStringLiteral("элемент %1: аргумент — не строка").arg(i));
                def.args << expandRoots(arg.toString());
            }
        }
        const QJsonValue cwd = obj.value(QStringLiteral("cwd"));
        if (!cwd.isUndefined()) {
            if (!cwd.isString())
                return refuse(QStringLiteral("элемент %1: \"cwd\" — не строка").arg(i));
            def.cwd = expandRoots(cwd.toString());
        }
        if (def.cwd.isEmpty())
            def.cwd = projectRoot_; // the declared default

        // THE MATCHER IS NAMED, NEVER CONFIGURED INLINE. An unknown name is
        // REFUSED (a silent gcc fallback would invent problems from a language
        // the task does not speak); unknown KEYS elsewhere are IGNORED (the
        // declared forwards-compatibility face).
        const QJsonValue matcherValue = obj.value(QStringLiteral("problemMatcher"));
        def.matcher = QStringLiteral("gcc"); // the declared default
        if (!matcherValue.isUndefined()) {
            if (!matcherValue.isString())
                return refuse(QStringLiteral("элемент %1: \"problemMatcher\" — v1 "
                                             "принимает имя строкой (не список, не "
                                             "объект)")
                                  .arg(i));
            def.matcher = matcherValue.toString();
        }
        if (def.matcher != QLatin1String("gcc")
            && def.matcher != QLatin1String("msvc")
            && def.matcher != QLatin1String("none"))
            return refuse(QStringLiteral("элемент %1 (\"%2\"): неизвестный матчер "
                                         "\"%3\" (есть: gcc, msvc, none)")
                              .arg(i)
                              .arg(def.label, def.matcher));

        const QJsonValue defaultValue = obj.value(QStringLiteral("default"));
        if (!defaultValue.isUndefined()) {
            if (!defaultValue.isBool())
                return refuse(QStringLiteral("элемент %1: \"default\" — не bool").arg(i));
            def.isDefault = defaultValue.toBool();
        }
        for (const TaskDef& done : read) {
            if (done.label == def.label)
                return refuse(QStringLiteral("повторяющаяся метка \"%1\"").arg(def.label));
        }
        read.append(def);
    }

    int defaults = 0;
    for (const TaskDef& def : read)
        defaults += def.isDefault ? 1 : 0;
    if (defaults > 1)
        return refuse(QStringLiteral("более одной задачи по умолчанию (\"default\" "
                                     "стоит на %1 задачах)")
                          .arg(defaults)); // never a silent first-wins

    tasks_ = read;
    return true;
}

QString IdeWindow::taskLabelAt(int index) const
{
    if (index < 0 || index >= tasks_.size())
        return {}; // the out-of-range honesty face (every row door's law)
    return tasks_.at(index).label;
}

QString IdeWindow::taskMatcherAt(int index) const
{
    if (index < 0 || index >= tasks_.size())
        return {};
    return tasks_.at(index).matcher;
}

bool IdeWindow::taskIsDefaultAt(int index) const
{
    return index >= 0 && index < tasks_.size() && tasks_.at(index).isDefault;
}

QString IdeWindow::taskWorkingDirAt(int index) const
{
    if (index < 0 || index >= tasks_.size())
        return {};
    return tasks_.at(index).cwd;
}

bool IdeWindow::openTaskConfig()
{
    if (projectRoot_.isEmpty()) {
        taskRunSummary_ = QStringLiteral("задачи: сначала откройте проект");
        statusBar()->showMessage(taskRunSummary_);
        return false;
    }
    // The app never CREATES a config file in v1 — opening one that is not
    // there is openFile's honest refusal, and the bar carries its message.
    return openFile(QDir(projectRoot_).filePath(QLatin1String(kTasksConfigName)));
}

// ---------------- the runner (never a terminal) ----------------

bool IdeWindow::runTask(const QString& label)
{
    if (projectRoot_.isEmpty()) {
        taskRunSummary_ = QStringLiteral("задачи: сначала откройте проект");
        statusBar()->showMessage(taskRunSummary_);
        return false;
    }
    if (taskRunning_) {
        // ONE run at a time — never a queue (the search/pool reentry law's
        // process sibling).
        taskRunSummary_ = QStringLiteral("задача «%1» ещё выполняется — "
                                         "одна за раз (остановите раньше)")
                              .arg(taskRunLabel_);
        statusBar()->showMessage(taskRunSummary_);
        return false;
    }
    for (const TaskDef& def : tasks_) {
        if (def.label == label)
            return startTask(def); // the ONE start path
    }
    QString known;
    for (int i = 0; i < tasks_.size(); ++i) {
        if (i)
            known += QStringLiteral(", ");
        known += tasks_.at(i).label;
    }
    if (!taskConfigSummary_.isEmpty())
        taskRunSummary_ = taskConfigSummary_; // a refused config speaks with
                                              // ITS own face, verbatim
    else if (known.isEmpty())
        taskRunSummary_ = QStringLiteral("нет настроенных задач — %1")
                              .arg(QLatin1String(kTasksConfigName));
    else
        taskRunSummary_ = QStringLiteral("нет задачи «%1» (настроено: %2)")
                              .arg(label, known);
    statusBar()->showMessage(taskRunSummary_);
    return false;
}

bool IdeWindow::runDefaultTask()
{
    if (projectRoot_.isEmpty()) {
        taskRunSummary_ = QStringLiteral("задачи: сначала откройте проект");
        statusBar()->showMessage(taskRunSummary_);
        return false;
    }
    for (const TaskDef& def : tasks_) {
        if (def.isDefault)
            return runTask(def.label); // the SAME door, one start path
    }
    if (tasks_.isEmpty()) {
        taskRunSummary_ = !taskConfigSummary_.isEmpty()
            ? taskConfigSummary_
            : QStringLiteral("нет настроенных задач — %1")
                  .arg(QLatin1String(kTasksConfigName));
        statusBar()->showMessage(taskRunSummary_);
        return false;
    }
    taskRunSummary_ = QStringLiteral("нет задачи по умолчанию (ключ "
                                     "\"default\": true ни у одной из %1)")
                          .arg(tasks_.size());
    statusBar()->showMessage(taskRunSummary_);
    return false;
}

bool IdeWindow::startTask(const TaskDef& def)
{
    // A NEW run replaces the LAST run's problems — panel rows AND the tagged
    // decoration rows (both directions of the shared channel).
    clearProblemFeed(QStringLiteral("новая задача"));
    // ONE run, one log: the pane resets HERE, so the tail cap and the hidden
    // count always describe a single run (mixing two runs into a capped tail
    // is the ambiguity that makes those faces untestable).
    taskLog_.clear();
    taskHiddenLines_ = 0;
    taskPendingBytes_.clear();
    if (outputEditor_)
        outputEditor_->setPlainText(QString());

    taskMatcher_ = def.matcher;
    taskCwd_ = def.cwd;
    problemTaskLabel_ = def.label;
    taskRunLabel_ = def.label;
    taskStopRequested_ = false;
    taskExitCode_ = kTaskExitPending;
    taskElapsedMs_ = -1;
    ++taskRunGeneration_; // every straggler of a PREVIOUS run dies at this bump

    taskProc_ = new QProcess(this); // Qt-parented; finishTask retires the corpse
    // ARRIVAL ORDER, the documented way: separate channels carry NO cross-
    // stream ordering guarantee, so a stderr line could otherwise land out of
    // order in the pane. MergedChannels is what a real terminal does, and it
    // costs the per-stream COUNT faces — which is why the pane has none (the
    // opening record's mark idea dies here, unused).
    taskProc_->setProcessChannelMode(QProcess::MergedChannels);
    taskProc_->setProgram(def.command);
    taskProc_->setArguments(def.args);
    taskProc_->setWorkingDirectory(def.cwd); // NO shell, NO PTY, NO stdin
                                             // writes: one-shot semantics
    connect(taskProc_, &QProcess::readyReadStandardOutput, this,
            [this, gen = taskRunGeneration_] {
                if (gen != taskRunGeneration_ || !taskProc_)
                    return; // a dead run's straggler never speaks
                consumeTaskBytes();
            });
    connect(taskProc_, &QProcess::finished, this,
            [this, gen = taskRunGeneration_](int exitCode,
                                             QProcess::ExitStatus status) {
                if (gen != taskRunGeneration_)
                    return;
                finishTask(exitCode, status == QProcess::CrashExit, QString());
            });
    connect(taskProc_, &QProcess::errorOccurred, this,
            [this, gen = taskRunGeneration_](QProcess::ProcessError error) {
                if (gen != taskRunGeneration_)
                    return;
                // FailedToStart emits NO finished() — this IS that face (the
                // only start error v1 names: nothing ever writes to the
                // child's stdin, so a write error has no home here).
                if (error == QProcess::FailedToStart) {
                    finishTask(kTaskExitFailedToStart, false,
                               taskProc_ ? taskProc_->errorString()
                                         : QStringLiteral("не удалось запустить"));
                }
            });

    taskRunning_ = true;
    taskClock_.start();
    taskProc_->start();
    taskRunSummary_ = QStringLiteral("задача «%1» запущена · cwd: %2 · матчер: %3")
                          .arg(def.label, def.cwd, def.matcher);
    statusBar()->showMessage(taskRunSummary_);
    if (outputSummaryLabel_)
        outputSummaryLabel_->setText(taskRunSummary_);
    return true;
}

bool IdeWindow::stopTask()
{
    if (!taskRunning_ || !taskProc_) {
        taskRunSummary_ = QStringLiteral("нет выполняющейся задачи");
        statusBar()->showMessage(taskRunSummary_);
        return false;
    }
    // terminate FIRST (the child gets a voice: a faketask that catches SIGTERM
    // prints its witness into the pane), kill only PAST the grace (a stubborn
    // child's witness is then our own stopped face instead).
    taskStopRequested_ = true;
    taskProc_->terminate();
    if (taskStopTimer_)
        taskStopTimer_->start();
    taskRunSummary_ = QStringLiteral("задача «%1» останавливается…").arg(taskRunLabel_);
    statusBar()->showMessage(taskRunSummary_);
    return true;
}

bool IdeWindow::killRunningTask()
{
    ++taskRunGeneration_; // the retirement bump FIRST: no signal of this run
                          // touches a dying window or a replaced project
    const bool wasRunning = taskRunning_;
    taskRunning_ = false;
    if (taskStopTimer_)
        taskStopTimer_->stop();
    if (taskFlushTimer_)
        taskFlushTimer_->stop();
    if (!taskProc_)
        return false;
    QProcess* proc = taskProc_;
    taskProc_ = nullptr;
    proc->disconnect(this); // NO finish face while the window/project dies (the
                            // widgets a finish handler would reach are leaving)
    if (proc->state() != QProcess::NotRunning) {
        proc->kill();
        proc->waitForFinished(2000);
    }
    proc->deleteLater();
    if (!wasRunning)
        return false;
    taskExitCode_ = kTaskExitCrashed; // WE ended it: the honest crashed face
    taskRunSummary_ = QStringLiteral("задача «%1» прервана").arg(taskRunLabel_);
    // THE FACE SURVIVES THE SWITCH (the caller's law, spelled at openProject):
    // the pane, the rows and the log retire, but the run's death keeps its
    // testimony — "we ended it" is a real face, and a switch that silently
    // rewrote it to "never ran" would hide the kill from the very drill that
    // pins the kill. A SIGKILL'd child has no voice, so this is the witness.
    return true;
}

void IdeWindow::consumeTaskBytes()
{
    if (!taskProc_)
        return;
    taskPendingBytes_ += taskProc_->readAllStandardOutput();
    int cut = taskPendingBytes_.indexOf('\n');
    while (cut >= 0) {
        QByteArray bytes = taskPendingBytes_.left(cut);
        taskPendingBytes_.remove(0, cut + 1);
        if (bytes.endsWith('\r'))
            bytes.chop(1); // a CRLF-shaped builder on any pipe stays clean text
        appendTaskLine(QString::fromUtf8(bytes));
        cut = taskPendingBytes_.indexOf('\n');
    }
}

void IdeWindow::appendTaskLine(const QString& line)
{
    taskLog_ << line;
    if (taskLog_.size() > kOutputMaxLines) {
        // THE TAIL CAP: newest kept, and the hidden count is a DOOR that also
        // rides the summary (the truncation-shows law I2 froze for quick-open).
        const int drop = taskLog_.size() - kOutputMaxLines;
        taskLog_.remove(0, drop);
        taskHiddenLines_ += drop;
    }
    ingestTaskLine(line); // the matcher rides ARRIVAL — a tail-capped pane
                          // never hides a problem from the FEED
    if (taskFlushTimer_ && !taskFlushTimer_->isActive())
        taskFlushTimer_->start(); // ONE whole-text replace per burst
}

void IdeWindow::flushTaskOutput()
{
    if (!outputEditor_)
        return;
    // THE PANE IS A WHOLE-TEXT REPLACE (the append door is NOT taken — PLAN's
    // opening decision; the close ritual's MEASUREMENT is what decides whether
    // it gets asked). v1's known cost, said out loud rather than discovered:
    // the replace resets the pane's selection, so a user mid-copy loses it on
    // the next burst.
    outputEditor_->setPlainText(taskLog_.join(QLatin1Char('\n')));
    // THE TAIL FOLLOW, app-side arithmetic over PUBLIC doors only:
    // setPlainText parks the caret at (0,0), so the app writes the last
    // visible line's start through the I1 public setSelection door (its
    // clamping is total) and reveals through the view's OWN reveal channel —
    // revealAt(…, Minimal) is the conditional edge-flush (the
    // ensureCaretVisible leg's public sibling: it no-ops when the tail is
    // already on screen and edge-flushes when it is not). ZERO new view API,
    // ZERO ledger entry.
    const int lastLine = taskLog_.isEmpty() ? 0 : taskLog_.size() - 1;
    const TextPosition tail{ qMax(0, lastLine), 0 };
    outputEditor_->setSelection(tail, tail);
    outputEditor_->revealAt(tail, CodeEditor::RevealMode::Minimal);
}

void IdeWindow::finishTask(int exitCode, bool crashed, const QString& startError)
{
    if (!taskRunning_)
        return; // ONE finish face per run: errorOccurred(Crashed) and
    taskRunning_ = false; // finished() BOTH fire for a signal death
    if (taskStopTimer_)
        taskStopTimer_->stop();
    consumeTaskBytes(); // whatever the pipe still holds
    if (!taskPendingBytes_.isEmpty()) {
        // a last line with NO trailing newline is still a line
        const QByteArray rest = taskPendingBytes_;
        taskPendingBytes_.clear();
        appendTaskLine(QString::fromUtf8(rest));
    }
    if (taskFlushTimer_)
        taskFlushTimer_->stop();
    taskElapsedMs_ = taskClock_.isValid() ? taskClock_.elapsed() : -1;
    taskExitCode_ = crashed ? kTaskExitCrashed : exitCode;
    if (taskProc_) {
        taskProc_->deleteLater();
        taskProc_ = nullptr;
    }
    flushTaskOutput(); // the FINAL replace: the paced flush never skips a tail
    buildProblemFeed();

    if (!startError.isEmpty()) {
        taskRunSummary_ = QStringLiteral("задача «%1»: не удалось запустить — %2")
                              .arg(taskRunLabel_, startError);
    } else {
        QString state;
        if (taskStopRequested_ && crashed)
            state = QStringLiteral("остановлена (пришлось убить)");
        else if (taskStopRequested_)
            state = QStringLiteral("остановлена");
        else if (crashed)
            state = QStringLiteral("завершилась сигналом");
        else if (taskExitCode_ == 0)
            state = QStringLiteral("успешно");
        else
            state = QStringLiteral("код %1").arg(taskExitCode_);
        taskRunSummary_ = QStringLiteral("задача «%1»: %2 · строк вывода: %3")
                              .arg(taskRunLabel_, state)
                              .arg(taskLog_.size());
        if (taskHiddenLines_ > 0)
            taskRunSummary_ += QStringLiteral(" (скрыто %1)").arg(taskHiddenLines_);
        taskRunSummary_ += QStringLiteral(" · проблем: %1").arg(problems_.size());
        if (problemFeedSkipped_ > 0)
            taskRunSummary_ +=
                QStringLiteral(" (матчер пропустил %1)").arg(problemFeedSkipped_);
        taskRunSummary_ += QStringLiteral(" · время: %1 мс").arg(taskElapsedMs_);
    }
    statusBar()->showMessage(taskRunSummary_);
    if (outputSummaryLabel_)
        outputSummaryLabel_->setText(taskRunSummary_);
}

// ---------------- the pane's faces ----------------

QString IdeWindow::taskOutput() const
{
    return taskLog_.join(QLatin1Char('\n'));
}

int IdeWindow::taskOutputLineCount() const
{
    return taskLog_.size();
}

bool IdeWindow::taskOutputVisible() const
{
    return outputDock_ && outputDock_->isVisible();
}

void IdeWindow::showTaskOutput(bool visible)
{
    if (!outputDock_)
        return;
    if (visible)
        outputDock_->show();
    else
        outputDock_->hide();
}

void IdeWindow::clearTaskOutput()
{
    taskLog_.clear();
    taskHiddenLines_ = 0;
    taskPendingBytes_.clear();
    if (outputEditor_)
        outputEditor_->setPlainText(QString());
    if (outputSummaryLabel_)
        outputSummaryLabel_->setText(QStringLiteral("вывод очищен"));
}

// ---------------- the problems feed (the panel's third producer) ---------

void IdeWindow::buildProblemFeed()
{
    problemSkipped_ = 0;
    if (problems_.isEmpty())
        return; // an EMPTY problem set never steals the panel from a standing
                // search/references result — resultsProducer() says who owns it

    // THE SAME CURRENCY, THE SAME FLATTENING (the I1 door appendResultRows and
    // the I3 owner door showResultsFor — a reference row and a problem row
    // activate through the SAME door): files in FIRST-APPEARANCE order, rows
    // inside a file in EMISSION order (the producer's order is the row order,
    // the walk-order honesty the panel already pins).
    QVector<qmn::search::FileMatch> grouped;
    QHash<QString, int> fileRow;
    QVector<int> rowMap;
    int rows = 0;
    for (int i = 0; i < problems_.size(); ++i) {
        const ProblemRow& p = problems_.at(i);
        if (rows >= kProblemMaxRows) {
            ++problemSkipped_; // the ROW cap's face (the FEED keeps them all)
            continue;
        }
        int index = -1;
        auto it = fileRow.constFind(p.filePath);
        if (it == fileRow.constEnd()) {
            index = grouped.size();
            fileRow.insert(p.filePath, index);
            qmn::search::FileMatch fm;
            fm.filePath = p.filePath;
            grouped.append(fm);
        } else {
            index = it.value();
        }
        qmn::search::LineMatch lm;
        lm.line = p.line;
        qmn::search::MatchSpan span; // the panel's span IS the diagnostic's
        span.startCol = p.startCol;  //   range — one record, never two truths
        span.endCol = p.endCol;
        lm.spans.append(span);
        lm.lineText = p.message; // the row's OWN text (producer-governed: the
                                 // other two producers carry a preview here —
                                 // no new field anywhere in the search records)
        grouped[index].lines.append(lm);
        rowMap.append(i);
        ++rows;
    }

    QString summary = QStringLiteral("проблемы задачи «%1»: %2 в %3 файлах")
                          .arg(problemTaskLabel_)
                          .arg(rows)
                          .arg(grouped.size());
    if (problemSkipped_ > 0)
        summary += QStringLiteral(" · ещё %1 не показаны").arg(problemSkipped_);
    showResultsFor(grouped, QStringLiteral("problems"), summary);
    problemRowMap_ = rowMap; // index-aligned with the panel's leaves (assigned
                             // AFTER showResultsFor, which clears it)
    searchDock_->show();

    // THE DECORATION FEED (the M5 door): problems whose file is an OPEN tab
    // reach the gutter NOW; a file that is not open keeps its panel rows and
    // catches up when it opens (addEditorTab's merge).
    QSet<QString> paths;
    for (const ProblemRow& p : problems_)
        paths.insert(p.filePath);
    for (int i = 0; i < tabs_->count(); ++i) {
        CodeEditor* editor = editorAt(i);
        const QString path = tabs_->tabBar()->tabData(i).toString();
        if (!editor || path.isEmpty() || !paths.contains(path))
            continue;
        mergeTaskDiagnostics(editor, path);
    }
}

void IdeWindow::mergeTaskDiagnostics(CodeEditor* editor, const QString& path)
{
    // THE DapHub MERGE IDIOM, app-side (the shelf's own coexistence answer):
    // the app replaces ONLY rows its tag owns and never a foreign producer's.
    // THE RECORDED COEXISTENCE LIMIT (PLAN's I4 close record, honest and
    // pinned-by-construction rather than pinned-by-test): `LspHub`'s published
    // paint is a WHOLE-LIST replace, not a merge — so on a hub-served document,
    // a later `textDocument/publishDiagnostics` evicts the task rows (the tag
    // survives only in the app's feed, and the next task re-run or re-open puts
    // them back). The app cannot fix that from its side without stealing the
    // hub's paint; the view has no per-producer replace door (LEDGER-adjacent,
    // NOT asked: the hub owns its own list, and the app's coexistence pair —
    // merge over foreign, restore foreign on clear — is pinned against a
    // hand-set foreign producer, which is the mechanism, not the coincidence).
    QVector<CodeEditor::Diagnostic> mine;
    for (const ProblemRow& p : problems_) {
        if (p.filePath != path)
            continue;
        CodeEditor::Diagnostic diag;
        diag.startLine = p.line;
        diag.startCol = p.startCol;
        diag.endLine = p.line;
        diag.endCol = p.endCol;
        diag.severity = severityToView(p.severity);
        diag.message = QStringLiteral("[%1] %2").arg(p.matcher, p.message);
        diag.source = QString::fromLatin1(kTaskSourceTag); // the tag IS the
        mine.append(diag);                                 // producer identity
    }
    QVector<CodeEditor::Diagnostic> merged;
    const QVector<CodeEditor::Diagnostic> current = editor->diagnostics();
    merged.reserve(current.size() + mine.size());
    for (const CodeEditor::Diagnostic& diag : current)
        if (diag.source != QLatin1String(kTaskSourceTag))
            merged.append(diag);
    merged.append(mine);
    editor->setDiagnostics(merged); // M5 door (3) — the view sorts, the app
}                                  // never re-sorts behind its back

void IdeWindow::clearProblemFeed(const QString& why)
{
    for (int i = 0; i < tabs_->count(); ++i) {
        CodeEditor* editor = editorAt(i);
        if (!editor)
            continue;
        const QVector<CodeEditor::Diagnostic> current = editor->diagnostics();
        bool tagged = false;
        for (const CodeEditor::Diagnostic& diag : current) {
            if (diag.source == QLatin1String(kTaskSourceTag)) {
                tagged = true;
                break;
            }
        }
        if (!tagged)
            continue;
        // The RESTORE half (the shelf's own posture): taking OUR feed away
        // puts back exactly what the tag did not own — a stopped or re-run
        // task never erases another producer's marks.
        QVector<CodeEditor::Diagnostic> kept;
        for (const CodeEditor::Diagnostic& diag : current)
            if (diag.source != QLatin1String(kTaskSourceTag))
                kept.append(diag);
        editor->setDiagnostics(kept);
    }
    problems_.clear();
    problemSkipped_ = 0;
    problemFeedSkipped_ = 0;
    problemTaskLabel_.clear();
    if (resultsProducer_ == QLatin1String("problems")) {
        // The panel claim retires WITH the feed (a stale problem row never
        // poses as current). The bar is NOT posted here: the I3 lesson is that
        // a per-dock truth rides its own summary face, not the shared bar.
        resultsTree_->clear();
        results_.clear();
        resultRows_.clear();
        problemRowMap_.clear();
        resultsProducer_.clear();
        searchSummary_ = QStringLiteral("проблемы сняты (%1)").arg(why);
    }
}

void IdeWindow::clearTaskProblems()
{
    clearProblemFeed(QStringLiteral("вручную"));
    taskRunSummary_ = searchSummary_;
    statusBar()->showMessage(searchSummary_);
}

int IdeWindow::problemCount() const
{
    if (resultsProducer_ != QLatin1String("problems"))
        return -1; // the panel is NOT the problems producer's: an honest "no
                   // such face", never a borrowed row count
    return resultRows_.size();
}

int IdeWindow::problemFeedCount() const
{
    return problems_.size();
}

QString IdeWindow::problemSeverityAt(int row) const
{
    if (resultsProducer_ != QLatin1String("problems") || row < 0
        || row >= problemRowMap_.size())
        return {}; // another producer's row has no severity — the search
                   // records carry none, which is the whole point of the
                   // app-side face
    return problems_.at(problemRowMap_.at(row)).severity;
}

bool IdeWindow::problemFileExistsAt(int row) const
{
    if (resultsProducer_ != QLatin1String("problems") || row < 0
        || row >= problemRowMap_.size())
        return false;
    return problems_.at(problemRowMap_.at(row)).fileExists;
}

QString IdeWindow::searchResultTextAt(int row) const
{
    if (row < 0 || row >= resultRows_.size())
        return {};
    const auto [fileIndex, lineIndex] = resultRows_.at(row);
    if (fileIndex < 0 || fileIndex >= results_.size())
        return {};
    const qmn::search::FileMatch& match = results_.at(fileIndex);
    if (lineIndex < 0 || lineIndex >= match.lines.size())
        return {};
    return match.lines.at(lineIndex).lineText;
}

// ---------------- the task picker (qmn::fuzzy's third user) --------------

void IdeWindow::showTaskPicker()
{
    if (taskPickerVisible())
        return; // the I2 idempotence law: a retrigger never wipes a query
    taskPickerEdit_->setText(QString()); // a FRESH query each open
    rankTaskPicker(taskPickerEdit_->text());
    showOverlay(taskPickerPop_, taskPickerEdit_);
}

bool IdeWindow::taskPickerVisible() const
{
    return taskPickerPop_ && taskPickerPop_->isVisible();
}

void IdeWindow::rankTaskPicker(const QString& needle)
{
    taskPickerRows_.clear();
    taskPickerList_->clear();

    if (tasks_.isEmpty()) {
        // The loud refusal (the quick-open empty-pool law's sibling): zero
        // rows is never a silent list.
        taskPickerSummary_ = !taskConfigSummary_.isEmpty()
            ? taskConfigSummary_
            : QStringLiteral("нет настроенных задач — %1")
                  .arg(QLatin1String(kTasksConfigName));
        return;
    }

    struct Row { int taskIndex; int score; };
    QVector<Row> rows;
    if (needle.isEmpty()) {
        for (int i = 0; i < tasks_.size(); ++i) // CONFIG ORDER (the frozen
            rows.append({ i, 0 });              // empty-needle face, and the
                                                // registry-order tie law's twin)
    } else {
        for (int i = 0; i < tasks_.size(); ++i) {
            const auto m = qmn::fuzzy::match(needle, tasks_.at(i).label);
            if (m.matched)
                rows.append({ i, m.score });
        }
        // score DESC, ties CONFIG order — a stable sort over a config-order
        // scan gives exactly that (the palette's registry-order tie law).
        std::stable_sort(rows.begin(), rows.end(),
                         [](const Row& a, const Row& b) { return a.score > b.score; });
    }

    for (const Row& row : rows) {
        taskPickerRows_.append(row.taskIndex);
        taskPickerList_->addItem(tasks_.at(row.taskIndex).label);
    }
    taskPickerSummary_ = needle.isEmpty()
        ? QStringLiteral("задач: %1").arg(tasks_.size())
        : (rows.isEmpty() ? QStringLiteral("Нет совпадений (%1 задач настроено)")
                              .arg(tasks_.size())
                          : QStringLiteral("Совпадений: %1").arg(rows.size()));
}

int IdeWindow::taskPickerRowCount() const
{
    return taskPickerRows_.size();
}

QString IdeWindow::taskPickerLabelAt(int row) const
{
    if (row < 0 || row >= taskPickerRows_.size())
        return {};
    return tasks_.at(taskPickerRows_.at(row)).label;
}

QString IdeWindow::taskPickerSummary() const
{
    return taskPickerSummary_;
}

bool IdeWindow::activateTask(int row)
{
    if (row < 0 || row >= taskPickerRows_.size())
        return false;
    const QString label = tasks_.at(taskPickerRows_.at(row)).label;
    if (!runTask(label))
        return false; // the popup STAYS open on a refusal (the quick-open
                      // failure face's sibling); the truth is in taskRunSummary
    closeOverlay();
    return true;
}

// ---------------- the palette registry (an append) ----------------

void IdeWindow::registerTaskCommands()
{
    const auto reg = [this](const QString& id, const QString& label,
                            const QString& shortcutText, std::function<void()> fn) {
        commands_.append(Command{ id, label, shortcutText, std::move(fn) });
    };
    // Six more APPENDS after I2's four and I3's six (the palette's tie law
    // rides registry order — an append never disturbs the frozen rows). A
    // command that refuses at the ask refuses LOUDLY the way outline-refresh
    // does with no hub up: the registry is never conditional on the config.
    reg(QStringLiteral("task-run-default"),
        QStringLiteral("Запустить задачу по умолчанию"),
        QStringLiteral("Ctrl+Shift+B"), [this] { runDefaultTask(); });
    reg(QStringLiteral("task-run-picker"),
        QStringLiteral("Выбрать задачу для запуска"),
        QStringLiteral("Ctrl+Shift+R"), [this] { showTaskPicker(); });
    reg(QStringLiteral("task-stop"),
        QStringLiteral("Остановить задачу"),
        QString(), [this] { stopTask(); });
    reg(QStringLiteral("task-show-output"),
        QStringLiteral("Показать вывод задач"),
        QString(), [this] { showTaskOutput(true); });
    reg(QStringLiteral("task-clear-problems"),
        QStringLiteral("Снять проблемы задач"),
        QString(), [this] { clearTaskProblems(); });
    reg(QStringLiteral("task-open-config"),
        QStringLiteral("Открыть файл настроек задач"),
        QString(), [this] { openTaskConfig(); });
}

} // namespace qmn::app
