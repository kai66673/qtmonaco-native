// Ad-hoc model latency bench: 20k random edits inside a ~62 MB document.
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>

#include <random>

#include "core/TextBuffer.hpp"

int main(int argc, char** argv)
{
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { QTextStream(stderr) << "cannot open " << argv[1] << Qt::endl; return 1; }
    QString doc = QString::fromUtf8(f.readAll());
    doc.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));

    qmn::core::TextBuffer buf;
    QElapsedTimer t;
    t.start();
    buf.setText(doc);
    const double setTextMs = t.nsecsElapsed() / 1e6;

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> posDist(0, buf.charCount() - 1);
    const int ops = 20000;
    qint64 totalNs = 0;
    qint64 maxNs = 0;
    t.start();
    for (int i = 0; i < ops; ++i) {
        const int at = posDist(rng);
        QElapsedTimer ot;
        ot.start();
        if ((i % 2) == 0) {
            buf.insertAtOffset(at, QStringLiteral("x = compute_value(42) # new line\n"));
        } else {
            buf.removeRange(at, at + 10);
        }
        const qint64 ns = ot.nsecsElapsed();
        totalNs += ns;
        maxNs = std::max(maxNs, ns);
    }
    const double runMs = t.nsecsElapsed() / 1e6;
    QTextStream out(stderr);
    out << QString("setText %1 ms; %2 ops in %3 ms; mean %4 us/op; max %5 us/op; nodes=%6\n")
               .arg(setTextMs, 0, 'f', 1)
               .arg(ops)
               .arg(runMs, 0, 'f', 1)
               .arg(totalNs / ops / 1000.0, 0, 'f', 1)
               .arg(maxNs / 1000.0, 0, 'f', 1)
               .arg(buf.debugNodeCount());
    out.flush();
    return 0;
}
