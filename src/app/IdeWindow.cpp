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

#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
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

    registerBuiltInCommands();

    statusLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(statusLabel_);
    updateStatusLabel();
}

// Out of line: the unique_ptr members (LspHub, SearchSession) need the
// complete types HERE, and the header deliberately never shows them
// (the AGENTS app-header law: fwd-decls only).
IdeWindow::~IdeWindow() = default;

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


} // namespace qmn::app
