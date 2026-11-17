#pragma once

#include <QByteArray>
#include <QString>

namespace qmn::protocol {

// The LSP "base protocol": Content-Length header block + payload. One
// JsonFraming instance decodes ONE inbound byte stream incrementally;
// encode() is static and stateless (PLAN.md GATE 2 / M8, machine 1).
//
// The 64 MiB upper bound is PLAN-pinned: an oversize declared body fails
// as a TRANSPORT ERROR, it is never an allocation request. A Content-Length
// is REQUIRED — LSP 3.17 dropped the old no-length Content-Type-only form.
//
// This file must depend on QtCore ONLY. No QtGui/QtWidgets, never view.
class JsonFraming {
public:
    // PLAN M8: 64 MiB hard bound on one frame's body.
    static constexpr qint64 kMaxFrameBytes = 64LL * 1024 * 1024;
    // Not PLAN-pinned but load-bearing: without it, a flood of header lines
    // with no blank line would buffer up to kMaxFrameBytes BEFORE failing.
    // 64 KiB is ~1000x any real LSP header block.
    static constexpr int kMaxHeaderBytes = 64 * 1024;

    enum class ErrorKind {
        None,
        HeaderMalformed,       // garbage line / missing Content-Length inside the block
        HeaderTooLarge,        // unterminated header block exceeded kMaxHeaderBytes
        ContentLengthTooLarge, // declared body exceeded kMaxFrameBytes
    };

    struct Error {
        ErrorKind kind = ErrorKind::None;
        QString text;
    };

    // Exact wire form: "Content-Length: N\r\n\r\n" + payload bytes.
    static QByteArray encode(const QByteArray& payload);

    // Incremental decoder. feed() appends wire bytes; poll() extracts one
    // complete payload per call (true) until the buffer is drained. After
    // an error the decoder is DEAD: feed/poll do nothing and the buffered
    // bytes are dropped — a framing desync cannot be resynchronized from
    // mid-stream, so the caller (RpcClient) must treat the transport as
    // poisoned. reset() clears error + buffer for a fresh stream.
    void feed(const QByteArray& bytes);
    bool poll(QByteArray* payload);

    bool hasError() const { return error_.kind != ErrorKind::None; }
    Error error() const { return error_; }
    void reset();

private:
    // Tries to find the header-block terminator in buffer_ and parse it.
    // Returns 1 = complete (bodyLength/headerEnd set), 0 = still incomplete,
    // -1 = error_ set (buffer_ dropped).
    int scanHeaders(qint64* bodyLength, int* headerEnd);
    void setError(ErrorKind kind, const QString& text);

    QByteArray buffer_;
    int scanFrom_ = 0;      // incremental terminator search start
    qint64 bodyLength_ = -1; // valid between header-parse and message-consume
    bool inBody_ = false;
    Error error_;
};

} // namespace qmn::protocol
