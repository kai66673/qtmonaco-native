#include "MainWindow.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qmn-demo"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("qtmonaco-native demo: tabbed editors + a settings dialog "
                       "(Файл->Открыть…, Настройки->Параметры редактора…)"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("files"),
                                 QStringLiteral("Text files to open as tabs"),
                                 QStringLiteral("[files...]"));
    parser.addOption({QStringLiteral("exit-after-ms"),
                      QStringLiteral("Quit automatically after the given delay "
                                     "(headless smoke runs)"),
                      QStringLiteral("ms")});
    parser.addOption({QStringLiteral("lsp"),
                      QStringLiteral("Enable the LSP hub: built-in specs "
                                     "(clangd for c/cpp) + .qmn/lsp.json, "
                                     "completion/hover popups on typing and "
                                     "idle caret moves")});
    parser.process(app);

    qmn::app::MainWindow window;
    if (parser.isSet(QStringLiteral("lsp")))
        window.setLspEnabled(true); // BEFORE the tabs: openPath wires them
    for (const QString& path : parser.positionalArguments())
        window.openPath(path); // CLI failures land in the status bar, like menu opens
    window.show();

    const QString exitAfterMs = parser.value(QStringLiteral("exit-after-ms"));
    if (!exitAfterMs.isEmpty())
        QTimer::singleShot(exitAfterMs.toInt(), &app, &QApplication::quit);
    return app.exec();
}
