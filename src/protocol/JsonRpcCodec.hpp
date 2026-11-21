#pragma once

#include "protocol/EnvelopeCodec.hpp"

namespace qmn::protocol {

// The JSON-RPC 2.0 / LSP envelope — RpcClient's ORIGINAL wire logic,
// extracted verbatim in machine D1 (every string, code and shape below
// is the one RpcClient.cpp emitted before the seam; the ENTIRE existing
// tst_lsp binary, the fakeserver's transcript oracles included, is this
// codec's regression tripwire and lands green with zero slot edits).
// Stateless: one process-wide const instance is the RpcClient default.
class JsonRpcCodec : public EnvelopeCodec {
public:
    // Wire codes RpcClient's kWire* constants name; kept here because the
    // builders, not the client, now spell the answers:
    static constexpr int kMethodNotFound = -32601;
    static constexpr int kInvalidRequest = -32600;

    QJsonObject buildRequest(qint64 idOrSeq, const QString& method,
                             const QJsonValue& params) const override;
    QJsonObject buildNotification(const QString& method,
                                  const QJsonValue& params,
                                  qint64 seq) const override; // seq unused:
                                  // JSON-RPC one-way messages carry no id
    QJsonObject buildResponse(const QJsonObject& incomingRequest,
                              const QJsonValue& result,
                              qint64 selfSeq) const override;
    QJsonObject buildErrorResponse(const QJsonObject& incomingRequest,
                                   int code, const QString& message,
                                   const QJsonValue& data,
                                   qint64 selfSeq) const override;
    QJsonObject buildUnregisteredAnswer(const QJsonObject& request,
                                        qint64 selfSeq) const override;
    QJsonObject buildInvalidIdAnswer(const QJsonObject& request) const override;
    QJsonObject buildCancel(qint64 targetId, qint64 selfSeq) const override;

    QString shutdownCommand() const override;   // "shutdown"
    QJsonValue shutdownParams() const override; // explicit JSON null (LSP)
    bool hasExitNotification() const override { return true; }
    QString exitCommand() const override;       // "exit"

    Incoming classify(const QJsonObject& message) const override;
    bool responseShapeOk(const QJsonObject& message, QString* why) const override;
    bool responseMatchId(const QJsonObject& message, qint64* id,
                         QString* why) const override;
    bool decodeReply(const QJsonObject& message, EnvelopeReply* out,
                     QString* why) const override;
};

// The RpcClient default ctor hands this out; inline (C++17) so one
// process-wide object serves every client. NOT `constexpr`: a constexpr
// variable of a polymorphic type needs a constexpr DESTRUCTOR, and GCC
// < 12 (the wheel container's Rocky 9 gcc 11.5 — caught at the 1.9.0
// wheel rebuild; the Arch host's newer GCC waves it through, the same
// container-catches-host class as the system-tree-sitter trap) miscounts
// any virtual dtor here as "used before its definition" (its own bug;
// clang and DR-correct GCC compile it). `inline const` value-init of a
// trivial type is constant-initialized all the same — zero static-init
// cost — and nothing here needs the constexpr.
inline const JsonRpcCodec kJsonRpcCodec{};

} // namespace qmn::protocol
