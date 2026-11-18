#pragma once

#include <QString>

namespace qmn::lsp {

// Offset <-> LSP Position conversion honoring the NEGOTIATED positionEncoding
// (PLAN.md GATE 2 / M8, machine 3). The whole model speaks 0-based UTF-16
// CODE-UNIT offsets; the LSP wire speaks {line, character}, where `character`
// counts units of the encoding the server chose in its initialize RESULT
// (utf-8 bytes / utf-16 units / utf-32 code points; the spec default when the
// server says nothing is utf-16). This class is the one place that reconciles
// the two frames — clangd answers utf-8, so an emoji on the line shifts every
// later column by 3 relative to our utf-16 count, and only here does that
// matter.
//
// QtCore ONLY (this header does not even pull src/protocol): pure functions
// over a caller-supplied QString, so they are testable with no session, no
// transport, no view. Offsets are the document frame (UTF-16 code units of the
// passed text); line/character are 0-based LSP fields.
//
// Clamping is deliberate and total: a generous position (line past EOF,
// character past end-of-line) is clamped the way a tolerant server reads it,
// never made to fault. toOffset NEVER lands inside a surrogate pair — for
// utf-8/utf-32 a `character` that would split a code point stops BEFORE it
// (the same surrogate-boundary discipline the view's own backspace keeps).
struct LspPosition {
    int line = 0;
    int character = 0;
};

// The VIEW frame: line + column counted in UTF-16 code units — exactly the
// coordinate space CodeEditor::Diagnostic (and the caret) speak.
struct ModelPosition {
    int line = 0;
    int col = 0;
};

class PositionConverter {
public:
    enum class Encoding { Utf8, Utf16, Utf32 };

    // "utf-8" / "utf-16" / "utf-32" -> Encoding; anything else (missing,
    // empty, malformed) is the spec default Utf16.
    static Encoding fromWire(const QString& wire);
    static QString toWire(Encoding enc);

    // UTF-16 offset -> LSP position in `enc`. `utf16Offset` clamped into
    // [0, text.size()].
    static LspPosition toPosition(const QString& text, int utf16Offset,
                                  Encoding enc);

    // Inverse. `line`/`character` clamped into the document/line; the result
    // is a UTF-16 offset that never splits a surrogate pair.
    static int toOffset(const QString& text, int line, int character,
                        Encoding enc);

    // Wire position -> the VIEW frame (M4's diagnostics feeding decodes
    // publishDiagnostics through here): toOffset re-expressed as line +
    // utf-16-column. Same total clamping; the offset never splits a pair,
    // so the column never does either.
    static ModelPosition toModelPosition(const QString& text, int line,
                                         int character, Encoding enc);

    // Width, in `enc` units, of one code point (0..0x10FFFF) — the per-unit
    // count the line walks accumulate. Public for the round-trip tests.
    static int unitWidth(unsigned codePoint, Encoding enc);
};

} // namespace qmn::lsp
