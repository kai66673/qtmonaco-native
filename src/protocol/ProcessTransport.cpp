#include "protocol/ProcessTransport.hpp"

#include <QTimer>

namespace qmn::protocol {

ProcessTransport::ProcessTransport(QString program, QStringList arguments,
                                   QObject* parent)
    : Transport(parent)
    , program_(std::move(program))
    , arguments_(std::move(arguments))
{
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&process_, &QProcess::started, this, [this] { markOpened(); });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        emit bytesReceived(process_.readAllStandardOutput());
    });
    connect(&process_, &QProcess::readyReadStandardError, this,
            [this] { drainStderr(false); });
    connect(&process_, &QProcess::errorOccurred, this,
            &ProcessTransport::onError);
    connect(&process_,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            &ProcessTransport::onFinished);
}

void ProcessTransport::setWorkingDirectory(const QString& directory)
{
    process_.setWorkingDirectory(directory);
}

bool ProcessTransport::start()
{
    process_.start(program_, arguments_);
    if (process_.state() == QProcess::NotRunning) {
        // Synchronous "cannot even begin" (empty path); errorOccurred has
        // already surfaced it (StartFailed -> closed()).
        running_ = false;
        return false;
    }
    // Writes before started() ride QProcess's own buffer — the write queue
    // whose errors surface through onError/onFinished, never silence.
    running_ = true;
    return true;
}

void ProcessTransport::write(const QByteArray& framedBytes)
{
    if (!running_) {
        emit transportError(Transport::ErrorKind::WriteFailed,
                            QStringLiteral("write on a not-running transport"));
        return;
    }
    // Qt 6 removed writeToStdin: QProcess is a QIODevice, write() is stdin.
    if (process_.write(framedBytes) < 0) {
        emit transportError(Transport::ErrorKind::WriteFailed,
                            process_.errorString());
    }
}

void ProcessTransport::stop()
{
    stopRequested_ = true;
    if (process_.state() == QProcess::NotRunning)
        return; // already dead; finished() has fired the closed() path
    process_.closeWriteChannel(); // EOF: well-behaved servers exit on their own
    QTimer::singleShot(3000, this, [this] {
        if (process_.state() != QProcess::NotRunning)
            process_.terminate();
    });
    QTimer::singleShot(5000, this, [this] {
        if (process_.state() != QProcess::NotRunning)
            process_.kill();
    });
}

void ProcessTransport::onError(QProcess::ProcessError error)
{
    if (ended_)
        return;
    if (error == QProcess::FailedToStart) {
        ended_ = true;
        emit transportError(Transport::ErrorKind::StartFailed,
                            process_.errorString());
        markClosed();
    } else if (error == QProcess::Crashed) {
        // Crashed also arrives with finished(CrashExit); ended_ dedupes,
        // whichever comes first wins.
        ended_ = true;
        emit transportError(Transport::ErrorKind::ServerDied,
                            QStringLiteral("process crashed: %1")
                                .arg(process_.errorString()));
        markClosed();
    }
    // QProcess::Timedout: we never use QProcess blocking waits, so N/A.
}

void ProcessTransport::onFinished(int exitCode, QProcess::ExitStatus status)
{
    if (ended_)
        return;
    ended_ = true;
    drainStderr(true);
    if (status == QProcess::CrashExit) {
        emit transportError(Transport::ErrorKind::ServerDied,
                            QStringLiteral("process crashed (exit code %1)")
                                .arg(exitCode));
    } else if (exitCode != 0 && !stopRequested_) {
        // The M2 restart hook only for UNWANTED deaths; a deliberate stop()
        // of a server that exits non-zero after shutdown/exit is clean.
        emit transportError(Transport::ErrorKind::ServerDied,
                            QStringLiteral("process exited with code %1")
                                .arg(exitCode));
    }
    markClosed();
}

void ProcessTransport::drainStderr(bool flushPartial)
{
    stderrLineBuffer_ += process_.readAllStandardError();
    const QList<QByteArray> lines = stderrLineBuffer_.split('\n');
    stderrLineBuffer_ = lines.last(); // partial tail stays buffered
    for (int i = 0; i + 1 < lines.size(); ++i) {
        QByteArray line = lines.at(i);
        if (line.endsWith('\r'))
            line.chop(1);
        emit stderrLine(QString::fromUtf8(line));
    }
    if (flushPartial && !stderrLineBuffer_.isEmpty()) {
        QByteArray line = stderrLineBuffer_;
        if (line.endsWith('\r'))
            line.chop(1);
        emit stderrLine(QString::fromUtf8(line));
        stderrLineBuffer_.clear();
    }
}

} // namespace qmn::protocol
