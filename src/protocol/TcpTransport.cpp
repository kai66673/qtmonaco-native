#include "protocol/TcpTransport.hpp"

#include <QTimer>

namespace qmn::protocol {

TcpTransport::TcpTransport(QString host, quint16 port, QObject* parent)
    : Transport(parent)
    , host_(std::move(host))
    , port_(port)
{
    connect(&socket_, &QTcpSocket::connected, this, &TcpTransport::markOpened);
    connect(&socket_, &QTcpSocket::disconnected, this, &TcpTransport::markClosed);
    connect(&socket_, &QTcpSocket::readyRead, this,
            [this] { emit bytesReceived(socket_.readAll()); });
    connect(&socket_, &QTcpSocket::errorOccurred, this,
            &TcpTransport::onError);
}

bool TcpTransport::start()
{
    socket_.connectToHost(host_, port_);
    running_ = true; // bytes written now queue inside QAbstractSocket
    return true;
}

void TcpTransport::write(const QByteArray& framedBytes)
{
    if (!running_) {
        emit transportError(Transport::ErrorKind::WriteFailed,
                            QStringLiteral("write on a not-running transport"));
        return;
    }
    if (socket_.write(framedBytes) < 0)
        emit transportError(Transport::ErrorKind::WriteFailed,
                            socket_.errorString());
}

void TcpTransport::stop()
{
    stopRequested_ = true;
    if (socket_.state() == QAbstractSocket::UnconnectedState)
        return; // already closed; the disconnected() path fired
    socket_.disconnectFromHost(); // flushes the queue, then closes
    QTimer::singleShot(2000, this, [this] {
        if (socket_.state() != QAbstractSocket::UnconnectedState)
            socket_.abort(); // abort() emits no disconnected(): close by hand
        markClosed();
    });
}

void TcpTransport::onError(QAbstractSocket::SocketError error)
{
    if (closedFired_) {
        // Post-close companions (the peer-close follow-ups Qt emits around
        // teardown) are NOT new failures: closed() has already been the
        // verdict, and crying transportError afterwards would fake a second
        // death and poison M2's restart counting.
        return;
    }
    switch (error) {
    case QAbstractSocket::RemoteHostClosedError:
        // The peer closed cleanly; disconnected() may or may not follow —
        // markClosed() is deduped in the base.
        markClosed();
        break;
    case QAbstractSocket::ConnectionRefusedError:
    case QAbstractSocket::HostNotFoundError:
        emit transportError(Transport::ErrorKind::ConnectFailed,
                            socket_.errorString());
        markClosed();
        break;
    default:
        // Write failures are NOT a Qt socket error: write() returns < 0 and
        // our write() door reports WriteFailed itself. Everything else in
        // Qt6's SocketError enum (OperationError, SocketTimeout, Proxy*/Ssl*
        // family, Temporary...) is an undifferentiated IO failure — naming
        // every value here would pin build portability across Qt minors for
        // zero value (Qt6 dropped SocketWriteError/AbortError outright).
        emit transportError(Transport::ErrorKind::Io, socket_.errorString());
        break;
    }
}

} // namespace qmn::protocol
