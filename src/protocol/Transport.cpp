#include "protocol/Transport.hpp"

namespace qmn::protocol {

Transport::Transport(QObject* parent)
    : QObject(parent)
{
}

Transport::~Transport() = default;

void Transport::markOpened()
{
    running_ = true;
    emit opened();
}

void Transport::markClosed()
{
    if (closedFired_)
        return;
    closedFired_ = true;
    running_ = false;
    emit closed();
}

} // namespace qmn::protocol
