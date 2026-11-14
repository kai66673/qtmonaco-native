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

// M4 demo convenience (examples/): guess the grammar from the suffix.
// Keep the ids in sync with CodeEditor::availableLanguages().
QString languageForSuffix(const QString& suffix)
{
    if (suffix == u"py")
        return QStringLiteral("python");
    if (suffix == u"json")
        return QStringLiteral("json");
    if (suffix == u"js" || suffix == u"mjs" || suffix == u"cjs")
        return QStringLiteral("javascript");
    // .h is a cpp header first (this widget's home is C++/Qt IDEs; the cpp
    // grammar also parses plain C headers near identically). .c gets c.
    if (suffix == u"c")
        return QStringLiteral("c");
    if (suffix == u"h" || suffix == u"cpp" || suffix == u"cc" || suffix == u"cxx"
        || suffix == u"hpp" || suffix == u"hh" || suffix == u"hxx")
        return QStringLiteral("cpp");
    return {};
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
    parser.addOption({QStringLiteral("language"),
                      QStringLiteral("Highlight grammar (python|json|javascript|c|cpp); "
                                     "default: guessed from the file suffix"),
                      QStringLiteral("id")});
    parser.process(app);

    qmn::view::CodeEditor editor;
    editor.resize(1000, 700);

    const QStringList args = parser.positionalArguments();
    QString baseTitle = QStringLiteral("qmn-spike");
    if (!args.isEmpty()) {
        QTextStream err(stderr);
        QElapsedTimer loadTimer;
        loadTimer.start();
        const QString text = loadTextFile(args.first(), err);
        const qint64 readMs = loadTimer.elapsed();
        if (!text.isNull()) {
            loadTimer.restart();
            editor.setPlainText(text);
            baseTitle = QFileInfo(args.first()).fileName();
            fprintf(stderr, "loaded %s: read %lld ms, setPlainText %lld ms, %lld lines\n",
                    qPrintable(args.first()), readMs, loadTimer.elapsed(),
                    static_cast<long long>(editor.lineCount()));
            fflush(stderr);
        }
    }

    // M4 demo convenience: --language wins over the suffix guess (examples/
    // README). An unknown/empty id leaves the editor plain text - that is
    // setLanguage's documented fallback, so this block can never break the
    // harness.
    QString language = parser.value(QStringLiteral("language")).trimmed().toLower();
    if (language.isEmpty() && !args.isEmpty())
        language = languageForSuffix(QFileInfo(args.first()).suffix().toLower());
    if (!language.isEmpty()) {
        editor.setLanguage(language);
        fprintf(stderr, "highlight: %s\n",
                editor.language().isEmpty()
                    ? qPrintable(QStringLiteral("none ('%1' not in %2)")
                                     .arg(language, qmn::view::CodeEditor::availableLanguages().join(QLatin1Char(','))))
                    : qPrintable(editor.language()));
        fflush(stderr);
    }

    // Live status in the title bar: cursor position and selection size.
    // selectedLength() is O(log P) with no text copy, so this is cheap.
    auto updateTitle = [&editor, baseTitle] {
        QString title = baseTitle
                        + QStringLiteral("  \u2014  %1:%2")
                              .arg(editor.position().line + 1)
                              .arg(editor.position().col + 1);
        if (editor.hasSelection())
            title += QStringLiteral("  (%1 code units selected)").arg(editor.selectedLength());
        editor.setWindowTitle(title);
    };
    QObject::connect(&editor, &qmn::view::CodeEditor::cursorMoved, &editor, updateTitle);
    QObject::connect(&editor, &qmn::view::CodeEditor::selectionChanged, &editor, updateTitle);
    updateTitle();

    editor.show();

    const QString exitAfterMs = parser.value(QStringLiteral("exit-after-ms"));
    if (!exitAfterMs.isEmpty()) {
        QTimer::singleShot(exitAfterMs.toInt(), &app, &QCoreApplication::quit);
    }
    return app.exec();
}
