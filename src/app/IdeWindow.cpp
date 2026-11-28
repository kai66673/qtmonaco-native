#include "IdeWindow.hpp"

#include "fuzzy/FuzzyMatcher.hpp"
#include "glue/LspHub.hpp"
#include "glue/ProjectSession.hpp"
#include "search/SearchEngine.hpp"
#include "view/CodeEditor.hpp"

#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
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
    const int index = tabs_->addTab(raw, QFileInfo(path).fileName());
    tabs_->tabBar()->setTabData(index, path);

    connect(raw, &CodeEditor::cursorMoved, this, &IdeWindow::updateStatusLabel);
    connect(raw, &CodeEditor::selectionChanged, this, &IdeWindow::updateStatusLabel);

    // One session call owns the open: registration + the suffix-table
    // language (the G2 shape unchanged). The one-tab guard above means
    // the duplicate-uri leg cannot fire for a TAB duplicate; it CAN
    // still answer false if a closed tab's release has not settled yet
    // (closeTabAt's closeDocument is the orderly drop — and tst_ide's
    // close-then-reopen slot pins the face).
    const QString language = session_->languageForSuffix(QFileInfo(path).suffix());
    if (!language.isEmpty())
        raw->setLanguage(language);
    if (!session_->openDocument(raw, path)) {
        const QString retry = session_->languageForSuffix(QFileInfo(path).suffix());
        if (!retry.isEmpty())
            raw->setLanguage(retry); // the demo's honest duplicate face
                                     // (grammar rides, registry stays)
    }

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

} // namespace qmn::app
