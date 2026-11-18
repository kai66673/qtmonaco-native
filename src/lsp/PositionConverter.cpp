#include "lsp/PositionConverter.hpp"

namespace qmn::lsp {

namespace {

inline bool isHigh(unsigned u) { return u >= 0xD800u && u <= 0xDBFFu; }
inline bool isLow(unsigned u) { return u >= 0xDC00u && u <= 0xDFFFu; }

// The code point starting at utf-16 index `p` (a real surrogate pair is a
// property of the string, not of any offset): returns the code point and
// sets `units` to 1 or 2. `lineEnd` bounds the pair to stay on the line.
inline unsigned codePointAt(const QString& text, int p, int lineEnd, int& units)
{
    const unsigned hi = text.at(p).unicode();
    if (isHigh(hi) && p + 1 < lineEnd && isLow(text.at(p + 1).unicode())) {
        const unsigned lo = text.at(p + 1).unicode();
        units = 2;
        return 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
    }
    units = 1;
    return hi; // a BMP unit, or a lone surrogate at a split boundary
}

} // namespace

PositionConverter::Encoding PositionConverter::fromWire(const QString& wire)
{
    if (wire == QLatin1String("utf-8"))
        return Encoding::Utf8;
    if (wire == QLatin1String("utf-32"))
        return Encoding::Utf32;
    return Encoding::Utf16; // "utf-16", "", missing, malformed -> the default
}

QString PositionConverter::toWire(Encoding enc)
{
    switch (enc) {
    case Encoding::Utf8:
        return QStringLiteral("utf-8");
    case Encoding::Utf32:
        return QStringLiteral("utf-32");
    case Encoding::Utf16:
        break;
    }
    return QStringLiteral("utf-16");
}

int PositionConverter::unitWidth(unsigned codePoint, Encoding enc)
{
    switch (enc) {
    case Encoding::Utf32:
        return 1; // every code point is exactly one utf-32 unit
    case Encoding::Utf16:
        return codePoint > 0xFFFFu ? 2 : 1; // a surrogate pair = 2 units
    case Encoding::Utf8:
        break;
    }
    if (codePoint < 0x80u)
        return 1;
    if (codePoint < 0x800u)
        return 2;
    if (codePoint < 0x10000u)
        return 3; // the whole BMP (Cyrillic, CJK, ...) is 3 utf-8 bytes
    return 4;     // non-BMP (emoji, astral scripts) is 4
}

LspPosition PositionConverter::toPosition(const QString& text, int utf16Offset,
                                          Encoding enc)
{
    const int size = text.size();
    if (utf16Offset < 0)
        utf16Offset = 0;
    if (utf16Offset > size)
        utf16Offset = size;

    // Line index + the utf-16 offset where that line BEGINS.
    int line = 0;
    int lineStart = 0;
    for (int i = 0; i < utf16Offset; ++i) {
        if (text.at(i) == u'\n') {
            ++line;
            lineStart = i + 1;
        }
    }

    // The character column, counted in `enc` units from the line start to the
    // offset. A code point fully inside the [lineStart, offset) window adds
    // its full width; if the window splits a code point (only utf-16 can
    // split a surrogate pair), utf-16 counts the partial units and utf-8/32
    // stop — that split only exists if the caller hands a mid-pair offset.
    // Pair detection uses the STRING (size), never the offset as boundary: an
    // offset landing INSIDE a pair must still SEE the pair (else the lone
    // high surrogate gets counted as a 3-byte BMP char — the v1.7.0 trap
    // this very file's pins caught).
    int character = 0;
    int p = lineStart;
    while (p < utf16Offset) {
        int units = 1;
        const unsigned cp = codePointAt(text, p, size, units);
        const int q = p + units; // the code point's end (may exceed offset)
        if (q <= utf16Offset) {
            character += unitWidth(cp, enc);
            p = q;
        } else {
            // Straddles the offset: only a split surrogate pair can do this.
            if (enc == Encoding::Utf16)
                character += (utf16Offset - p); // 1 unit (the lone high)
            break;
        }
    }
    return LspPosition{line, character};
}

int PositionConverter::toOffset(const QString& text, int line, int character,
                                Encoding enc)
{
    const int size = text.size();
    if (line < 0)
        line = 0;
    if (character < 0)
        character = 0;

    // Walk to the start of the `line`-th line (clamped to EOF when the
    // request names a line that does not exist).
    int lineStart = 0;
    for (int l = 0; l < line; ++l) {
        if (lineStart >= size)
            break;
        while (lineStart < size && text.at(lineStart) != u'\n')
            ++lineStart;
        if (lineStart < size)
            ++lineStart; // step over the newline -> the next line's start
    }
    int lineEnd = lineStart;
    while (lineEnd < size && text.at(lineEnd) != u'\n')
        ++lineEnd;

    // Consume code points while the accumulated `enc`-unit width stays within
    // `character`; a code point that would overshoot is NOT consumed, so a
    // mid-pair `character` for utf-8/32 lands on the boundary before it.
    int acc = 0;
    int p = lineStart;
    int target = lineStart;
    while (p < lineEnd && acc < character) {
        int units = 1;
        const unsigned cp = codePointAt(text, p, lineEnd, units);
        const int w = unitWidth(cp, enc);
        if (acc + w > character)
            break;
        acc += w;
        p += units;
        target = p;
    }
    return target;
}

ModelPosition PositionConverter::toModelPosition(
    const QString& text, int line, int character, Encoding enc)
{
    const int offset = toOffset(text, line, character, enc);
    int lineStart = 0;
    int outLine = 0;
    for (int i = 0; i < offset; ++i) {
        if (text.at(i) == u'\n') {
            lineStart = i + 1;
            ++outLine;
        }
    }
    return ModelPosition{outLine, offset - lineStart};
}

} // namespace qmn::lsp
