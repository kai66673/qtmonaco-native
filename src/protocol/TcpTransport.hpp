#pragma once

#include <QTcpSocket>

#include "protocol/Transport.hpp"

namespace qmn::protocol {

// TCP transport (PLAN.md M8: TcpTransport) — the connect-and-talk half for
// servers reachable over a socket. Writes before connected() ride
// QAbstractSocket's own outgoing queue; a write that the device refuses
// returns < 0 and our write() door surfaces it as WriteFailed (Qt6 has no
// SocketWriteError error code — the never-swallowed write queue is honest
// at the door itself).
class TcpTransport : public Transport {
    Q_OBJECT
public:
    TcpTransport(QString host, quint16 port, QObject* parent = nullptr);

    bool start() override; // async; opened() follows on connect
    void write(const QByteArray& framedBytes) override;
    // Flush-then-close, abort after 2 s if the socket lingers (the protocol
    // half of the barrier is RpcClient::stop()).
    void stop() override;

private:
    void onError(QAbstractSocket::SocketError error);

    QTcpSocket socket_;
    QString host_;
    quint16 port_;
    bool stopRequested_ = false;
};

} // namespace qmn::protocol
