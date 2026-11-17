#pragma once

#include <QProcess>
#include <QStringList>

#include "protocol/Transport.hpp"

namespace qmn::protocol {

// QProcess stdio transport (the shape every real LSP server ships with:
// spawn, talk over stdin/stdout). stderr is kept a SEPARATE channel
// (PLAN.md M8: per-server PROTOCOL and STDERR logs never merge).
//
// Single session per instance: a restart after ServerDied (the M2
// lifecycle) builds a fresh ProcessTransport.
class ProcessTransport : public Transport {
    Q_OBJECT
public:
    ProcessTransport(QString program, QStringList arguments,
                     QObject* parent = nullptr);

    void setWorkingDirectory(const QString& directory);

    // The finished process's exit code (defined only once the process has
    // finished; the M2 tests use the server's exit code as their wire-side
    // oracle — fakeserver exits 0 clean, 7 crashed, 2/3 on a contract fail).
    int exitCode() const { return process_.exitCode(); }

    bool start() override;
    void write(const QByteArray& framedBytes) override;
    // stopRequested suppresses the ServerDied report for a deliberate
    // teardown (some servers exit non-zero after a normal shutdown/exit
    // pair; a crash mid-stop is still reported). Then: closeWriteChannel
    // (EOF on stdin), terminate after 3 s, kill after 5 s — the protocol
    // half of the two-phase barrier (shutdown/exit) is RpcClient::stop().
    void stop() override;

private:
    void onError(QProcess::ProcessError error);
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void drainStderr(bool flushPartial);

    QProcess process_;
    QString program_;
    QStringList arguments_;
    bool stopRequested_ = false;
    bool ended_ = false; // finished()/Crashed dedupe
    QByteArray stderrLineBuffer_;
};

} // namespace qmn::protocol
