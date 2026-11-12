#include "MainWindow.hpp"

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

namespace {

// Same suffix->grammar map as the spike's main.cpp (keep both in sync
// with CodeEditor::availableLanguages(); an unmapped suffix simply stays
// plain text — setLanguage's documented fallback).
QString languageForSuffix(const QString& suffix)
{
    if (suffix == u"py")
        return QStringLiteral("python");
    if (suffix == u"json")
        return QStringLiteral("json");
    if (suffix == u"js" || suffix == u"mjs" || suffix == u"cjs")
        return QStringLiteral("javascript");
    return {};
}

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
    const QString language = languageForSuffix(QFileInfo(path).suffix().toLower());
    if (!language.isEmpty())
        editor->setLanguage(language);
    applySettings(editor.get(), settings_); // the tab inherits the current template

    CodeEditor* raw = editor.release(); // the tab widget owns it from here
    const int index = tabs_->addTab(raw, QFileInfo(path).fileName());
    tabs_->tabBar()->setTabData(index, path); // QTabWidget itself has no tabData

    connect(raw, &CodeEditor::cursorMoved, this, &MainWindow::updateStatusLabel);
    connect(raw, &CodeEditor::selectionChanged, this, &MainWindow::updateStatusLabel);
    tabs_->setCurrentIndex(index); // currentChanged sets title + status
}

void MainWindow::closeTabAt(int index)
{
    QWidget* page = tabs_->widget(index);
    if (!page)
        return;
    tabs_->removeTab(index); // does NOT delete: the editor does, now
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
