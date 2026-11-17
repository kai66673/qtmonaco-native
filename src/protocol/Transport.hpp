#pragma once

#include <QByteArray>
#include <QObject>

namespace qmn::protocol {

// Raw byte transport under JsonFraming/RpcClient (PLAN.md GATE 2 / M8,
// machine 1). Layer rule: QtCore + QtNetwork only — the protocol layers
// NEVER know CodeEditor/CodeDocument.
//
// A transport moves framed bytes and reports life state:
//  opened()   the byte pipe is live (connect completed / process started);
//             writes issued before this go into the device's own write
//             queue — the RpcClient "write queue whose errors SURFACE":
//             any failure of that queue must end in transportError() or
//             closed(), never silence (the iide anti-pattern).
//  closed()   the pipe ended cleanly (peer close / stop()); exactly once.
//  transportError() then closed() for any abnormal end.
//  stderrLine() the SEPARATE log channel PLAN pins per server (PROTOCOL and
//             STDERR stay different streams); silent on TCP.
class Transport : public QObject {
    Q_OBJECT
public:
    enum class ErrorKind {
        StartFailed, // program could not start
        ConnectFailed, // TCP connect refused / host unreachable
        WriteFailed, // a write could not reach the peer
        ServerDied,  // process crashed or exited non-zero (restart hook, M2)
        Io,          // any other socket/IO failure
    };
    Q_ENUM(ErrorKind)

    explicit Transport(QObject* parent = nullptr);
    ~Transport() override;

    // Kick off the connection asynchronously; opened()/transportError()
    // follow through the signals. Returns false only on a synchronous
    // "cannot even begin" (bad program path).
    virtual bool start() = 0;

    // Framed wire bytes (JsonFraming::encode already applied). A no-op with
    // a WriteFailed error if the pipe is not alive — writes are never lost
    // silently.
    virtual void write(const QByteArray& framedBytes) = 0;

    // Begin closing the pipe. The LSP two-phase stop (shutdown request ->
    // exit notification) is RpcClient's protocol dance; this is the
    // process/socket teardown underneath, with its own kill escalation.
    virtual void stop() = 0;

    bool isRunning() const { return running_; }

signals:
    void bytesReceived(const QByteArray& bytes);
    void opened();
    void closed();
    void transportError(Transport::ErrorKind kind, const QString& text);
    void stderrLine(const QString& line);

protected:
    // Shared end-of-life bookkeeping: fires closed() exactly once and flips
    // isRunning(); subclasses call it from their EOF/finished paths.
    void markClosed();
    void markOpened();
    bool running_ = false;
    bool closedFired_ = false;
};

} // namespace qmn::protocol
