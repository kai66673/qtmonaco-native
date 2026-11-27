#include "IdeWindow.hpp"

#include "glue/LspHub.hpp"
#include "glue/ProjectSession.hpp"
#include "search/SearchEngine.hpp"
#include "view/CodeEditor.hpp"

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
#include <QPushButton>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeView>
#include <QVBoxLayout>

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
    auto* searchDock = new QDockWidget(QStringLiteral("Поиск по проекту"), this);
    auto* searchPanel = new QWidget(searchDock);
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
    searchDock->setWidget(searchPanel);
    addDockWidget(Qt::BottomDockWidgetArea, searchDock);

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
