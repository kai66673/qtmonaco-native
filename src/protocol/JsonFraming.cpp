#include "protocol/JsonFraming.hpp"

#include <QByteArray>
#include <QList>

namespace qmn::protocol {

QByteArray JsonFraming::encode(const QByteArray& payload)
{
    QByteArray out;
    out.reserve(payload.size() + 32);
    out += "Content-Length: ";
    out += QByteArray::number(payload.size());
    out += "\r\n\r\n";
    out += payload;
    return out;
}

void JsonFraming::feed(const QByteArray& bytes)
{
    if (error_.kind != ErrorKind::None)
        return; // dead until reset()
    buffer_ += bytes;
}

bool JsonFraming::poll(QByteArray* payload)
{
    if (error_.kind != ErrorKind::None)
        return false;

    if (!inBody_) {
        qint64 length = -1;
        int headerEnd = -1;
        if (scanHeaders(&length, &headerEnd) < 0)
            return false; // error_ set, buffer_ dropped
        if (headerEnd < 0)
            return false; // header block still incomplete
        bodyLength_ = length;
        inBody_ = true;
        buffer_.remove(0, headerEnd);
        scanFrom_ = 0;
    }

    if (buffer_.size() < bodyLength_)
        return false; // body still incomplete (bounded by kMaxFrameBytes)

    *payload = buffer_.left(static_cast<int>(bodyLength_));
    buffer_.remove(0, static_cast<int>(bodyLength_));
    inBody_ = false;
    bodyLength_ = -1;
    return true;
}

void JsonFraming::reset()
{
    error_ = {};
    buffer_.clear();
    scanFrom_ = 0;
    inBody_ = false;
    bodyLength_ = -1;
}

void JsonFraming::setError(ErrorKind kind, const QString& text)
{
    error_ = {kind, text};
    // A framing desync cannot be resynchronized mid-stream: drop everything,
    // the transport is poisoned (the RpcClient closes it).
    buffer_.clear();
    scanFrom_ = 0;
    inBody_ = false;
    bodyLength_ = -1;
}

// returns 1 = header block complete (bodyLength/headerEnd set),
// 0 = still incomplete, -1 = error_ set.
int JsonFraming::scanHeaders(qint64* bodyLength, int* headerEnd)
{
    // The terminator is a blank line. Prefer CRLF CRLF, accept bare LF LF
    // on the inbound side (lenient readers are the norm for LSP servers);
    // our own encode() always writes CRLF.
    const int c1 = buffer_.indexOf("\r\n\r\n", scanFrom_);
    const int c2 = buffer_.indexOf("\n\n", scanFrom_);
    int term = -1;
    int termLen = 0;
    if (c1 >= 0 && (c2 < 0 || c1 <= c2)) {
        term = c1;
        termLen = 4;
    } else if (c2 >= 0) {
        term = c2;
        termLen = 2;
    }
    if (term < 0) {
        // A terminator can start at most 3 bytes before the end of what we
        // have seen, so the next scan may resume at size-3 without missing
        // a straddling match.
        scanFrom_ = qMax(0, buffer_.size() - 3);
        if (buffer_.size() > kMaxHeaderBytes) {
            setError(ErrorKind::HeaderTooLarge,
                     QStringLiteral("header block exceeded %1 bytes with no blank line")
                         .arg(kMaxHeaderBytes));
            return -1;
        }
        return 0; // header block still incomplete
    }

    // Normalize CRLF to LF for line splitting only (block is <= kMaxHeaderBytes).
    const QByteArray block = QByteArray(buffer_.left(term)).replace("\r\n", "\n");
    const QList<QByteArray> lines = block.split('\n');

    qint64 length = -1;
    for (const QByteArray& rawLine : lines) {
        if (rawLine.isEmpty())
            continue; // blank line inside the block: tolerated, no field
        const int colon = rawLine.indexOf(':');
        if (colon <= 0) {
            setError(ErrorKind::HeaderMalformed,
                     QStringLiteral("header line without a field name: \"%1\"")
                         .arg(QString::fromLatin1(rawLine)));
            return -1;
        }
        QByteArray name = rawLine.left(colon).trimmed().toLower();
        const QByteArray value = rawLine.mid(colon + 1).trimmed();
        for (const char c : name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                            || (c >= '0' && c <= '9') || c == '-';
            if (!ok) {
                setError(ErrorKind::HeaderMalformed,
                         QStringLiteral("illegal character in header field name \"%1\"")
                             .arg(QString::fromLatin1(name)));
                return -1;
            }
        }
        if (name == "content-length") {
            if (length >= 0) {
                setError(ErrorKind::HeaderMalformed, "duplicate Content-Length header");
                return -1;
            }
            bool digitsOnly = !value.isEmpty();
            for (const char c : value)
                digitsOnly = digitsOnly && c >= '0' && c <= '9';
            if (!digitsOnly) {
                setError(ErrorKind::HeaderMalformed,
                         QStringLiteral("Content-Length is not a non-negative integer: \"%1\"")
                             .arg(QString::fromLatin1(value)));
                return -1;
            }
            bool ok = false;
            const qlonglong v = value.toLongLong(&ok);
            if (!ok || v > kMaxFrameBytes) {
                // PLAN-pinned: the 64 MiB bound fails as a TRANSPORT ERROR
                // (never an allocation): the declared body is simply never
                // read into the buffer.
                setError(ErrorKind::ContentLengthTooLarge,
                         QStringLiteral("Content-Length %1 exceeds the %2 MiB frame bound")
                             .arg(QString::fromLatin1(value))
                             .arg(kMaxFrameBytes / (1024 * 1024)));
                return -1;
            }
            length = v;
        }
        // content-type and every other known-unknown field: ignored (LSP
        // header blocks are extension points; strictness here buys nothing).
    }

    if (length < 0) {
        setError(ErrorKind::HeaderMalformed, "Content-Length header is required");
        return -1;
    }

    *bodyLength = length;
    *headerEnd = term + termLen;
    return 1;
}

} // namespace qmn::protocol
