#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>

#include "view/CodeEditor.hpp"

namespace {

QString loadTextFile(const QString& path, QTextStream& err)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        err << "cannot open '" << path << "': " << file.errorString() << Qt::endl;
        return {};
    }
    const QByteArray data = file.readAll();
    // Spike loader: UTF-8 only, CRLF/CR normalized to LF. Encoding detection
    // and large-file streaming are later milestones.
    QString text = QString::fromUtf8(data);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qmn-spike"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("qtmonaco-native spike: monospace canvas + piece-table model"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("file"),
                                 QStringLiteral("Text file to open (optional)"));
    parser.addOption({QStringLiteral("exit-after-ms"),
                      QStringLiteral("Quit automatically after the given delay (smoke/bench)"),
                      QStringLiteral("ms")});
    parser.process(app);

    qmn::view::CodeEditor editor;
    editor.resize(1000, 700);

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        QTextStream err(stderr);
        QElapsedTimer loadTimer;
        loadTimer.start();
        const QString text = loadTextFile(args.first(), err);
        const qint64 readMs = loadTimer.elapsed();
        if (!text.isNull()) {
            loadTimer.restart();
            editor.setPlainText(text);
            editor.setWindowTitle(QFileInfo(args.first()).fileName());
            fprintf(stderr, "loaded %s: read %lld ms, setPlainText %lld ms, %lld lines\n",
                    qPrintable(args.first()), readMs, loadTimer.elapsed(),
                    static_cast<long long>(editor.lineCount()));
            fflush(stderr);
        }
    }

    editor.show();

    const QString exitAfterMs = parser.value(QStringLiteral("exit-after-ms"));
    if (!exitAfterMs.isEmpty()) {
        QTimer::singleShot(exitAfterMs.toInt(), &app, &QCoreApplication::quit);
    }
    return app.exec();
}
