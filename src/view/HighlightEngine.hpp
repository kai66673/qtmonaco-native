#pragma once

// M4 syntax highlighting (PLAN.md Phase D2): private seam of CodeEditor.
// This header owns the vendored tree-sitter parser/query state and is
// included ONLY by view implementation files - CodeEditor.hpp keeps the
// public API tree-sitter-free so it stays Shiboken-wrappable and the
// qmn_ts link stays PRIVATE (layering rule).
//
// Design (facts measured against tree-sitter v0.27.2, see
// third_party/tree-sitter/VENDORED.md):
//  - Input is fed zero-copy via the core's pieceAt(): the v0.27 read
//    callback returns a borrowed pointer + byte count, so piece data goes
//    straight to the lexer with no memcpy and no transcoding. Encoding is
//    TSInputEncodingUTF16LE, hence every tree-sitter byte offset is
//    exactly 2x our UTF-16-code-unit offset.
//  - Edits arrive as core EditEvents (one per primitive piece-table
//    mutation, offsets in the PRE-op frame). Each event becomes one
//    ts_tree_edit immediately - every input change must hit the tree
//    before that tree is reused for parsing; the reparse itself is
//    deferred to refreshIfNeeded(), so a burst of edits within one key
//    press (multi-caret fan-out, paste) reparses once.
//  - TSInputEdit wants line/col points. The core's lineColAt() is
//    O(document), and offsetOfLine() would scan inside one giant piece
//    after setText of a big file, so the engine keeps its own sorted
//    line-start index, updated incrementally per event.
//  - Query captures merge into ONE sorted, non-overlapping span list per
//    tree generation ("paint-over": captures applied in ascending start
//    give correct nesting - an inner escape repainted over the enclosing
//    string wins inside; for identical spans the EARLIER pattern in the
//    query file wins, matching tree-sitter's own conflict convention, so
//    those are applied LAST with pattern_index descending).

#include "core/TextBuffer.hpp"
#include "view/CodeEditor.hpp"

#include <tree_sitter/api.h>

#include <QColor>
#include <QString>
#include <QStringList>

#include <vector>

namespace qmn::view {

class HighlightEngine {
public:
    // One merged token span: document offsets in UTF-16 code units,
    // [start, end). Valid only after refreshIfNeeded(); sorted by start.
    struct Span {
        int start = 0;
        int end = 0;
        CodeEditor::TokenKind kind = CodeEditor::TokenKind::Text;
    };

    HighlightEngine();
    ~HighlightEngine();
    HighlightEngine(const HighlightEngine&) = delete;
    HighlightEngine& operator=(const HighlightEngine&) = delete;

    // Borrows the view's buffer. Single-thread discipline: every callback
    // runs while nobody mutates the buffer (parsing happens in paint).
    void attach(const qmn::core::TextBuffer* buffer);

    // "" or an unknown id disables highlighting (tree + query dropped,
    // the widget draws everything in the default text color).
    void setLanguage(const QString& id);
    QString language() const { return languageId_; }
    bool active() const { return lang_ != nullptr; }

    // Language ids with a parser + query (registry order).
    static QStringList languageIds();

    // Core edit-observer entry: applies ts_tree_edit and keeps the line
    // index current; defers the reparse until refreshIfNeeded().
    void onEdit(const qmn::core::TextBuffer::EditEvent& ev);

    // The widget's visible document window [startUnit, endUnit) in model
    // offsets; paintEvent feeds it every frame. On big documents it lets
    // the adaptive policy scope reparse+query to what is on screen
    // (measured, tools/bench_ts_split.cpp: full incremental reparse is
    // ~1-3ms/keystroke for json/js but ~23ms for python at 200KB, while
    // viewport-range parse + viewport-scoped query stay under ~0.7ms for
    // every pinned grammar; capture sets verified IDENTICAL vs the full
    // tree inside the window). Repeating the same window is a no-op.
    void setViewport(int startUnit, int endUnit);

    // Brings the engine current (like refreshIfNeeded) and guarantees the
    // returned spans truly cover [startUnit, endUnit): a viewport-scoped
    // refresh leaves spans_ viewport-only, so this may pay one synchronous
    // re-query on the existing tree - or, when the window tree does not
    // reach the request, one full-document parse+query. The paint path
    // never needs it (visible lines are inside the parsed window); it is
    // the exactness contract behind CodeEditor::highlightSpans.
    const std::vector<Span>& spansFor(int startUnit, int endUnit);

    // Bring the tree current if dirty, then rebuild the merged spans if
    // the tree changed. Cheap no-op otherwise. Call before reading
    // spans()/lineStarts().
    void refreshIfNeeded();

    const std::vector<Span>& spans() const { return spans_; }
    // Consistent with spans() after refreshIfNeeded(): lineStarts_[L] is
    // the document offset of line L. Only populated while active().
    const std::vector<int>& lineStarts() const { return lineStarts_; }

    // v1 fixed theme (a set_theme/Monaco-palette bridge is a later
    // milestone). An invalid return means "use the default text pen".
    static QColor colorFor(CodeEditor::TokenKind kind);

private:
    // Raw capture before merging (query emission order kept in seq).
    struct Capture {
        int start;
        int end;
        CodeEditor::TokenKind kind;
        uint16_t pattern; // query pattern index, for the conflict rule
        uint32_t seq;     // emission order, final tie-break
    };

    void clearState();
    void rebuildLineIndex(); // full scan through pieceAt() chunks
    void updateLineIndex(const qmn::core::TextBuffer::EditEvent& ev);
    TSPoint pointOfUnit(int offset) const; // pre-op frame, via lineStarts_
    void reparseFull();
    bool reparseViewport(); // false => caller falls back to reparseFull()
    int lineForUnit(int offset) const; // index of the line containing offset
    int anchorLineFor(int topLine) const; // nearest col-0 non-blank line at/above
    // Merged-span rebuild. qStart < 0 queries the WHOLE tree; otherwise
    // only captures intersecting [qStart, qEnd) (ts_query_cursor_set_-
    // byte_range, settable after exec - v0.27.2 measured).
    void rebuildSpans(int qStart, int qEnd);

    // Adaptive parse mode (D2 threading sub-decision, bench-decided):
    // no threads for editor v1; instead measure the full incremental
    // parse and, once it busts the latency budget on a big document,
    // switch to viewport-range parsing anchored at the nearest col-0
    // line (restores top-level context for column-based grammars like
    // python). Threading is deferred to M13 (folding needs full trees;
    // snapshot-copy cost is trivial per bench).
    enum class Mode { Full, Viewport };
    Mode mode_ = Mode::Full; // parse strategy of the NEXT reparse
    Mode spansMode_ = Mode::Full; // query scope that produced spans_
    bool viewportSet_ = false;
    bool windowDirty_ = false; // viewport moved since the last refresh
    int vpStart_ = 0, vpEnd_ = 0; // current viewport window (units)
    int covStart_ = 0, covEnd_ = 0; // tree_ covers [covStart_, covEnd_)
    int spanQStart_ = 0, spanQEnd_ = 0; // query window behind spans_
    double lastParseMs_ = 0.0;

    static constexpr double kParseBudgetMs = 8.0; // json 1.1 / js 3.3 vs py 23 @200KB
    static constexpr int kViewportMinUnits = 16 * 1024; // policy only pays off big
    static constexpr int kAnchorScanCap = 2000; // lines searched upward, then anchor=0

    // Zero-copy pull reader (PLAN.md D2 core decision). Kept a static
    // member so it matches the TSInput::read function-pointer signature
    // while still reaching the private buffer_ through the payload
    // (payload is always the HighlightEngine*).
    //
    // The lexer calls with byte-aligned offsets only under
    // TSInputEncodingUTF16LE (it advances in 2-byte units), so byte/2 is
    // a code-unit offset; the returned pointer borrows the piece data,
    // valid until the next mutation - parsing never interleaves with
    // edits (single-threaded GUI discipline).
    static const char* readPiece(void* payload, uint32_t byteIndex,
                                 TSPoint anchor, uint32_t* bytesRead);
    TSInput makeInput() const;

    const qmn::core::TextBuffer* buffer_ = nullptr;
    QString languageId_;
    const TSLanguage* lang_ = nullptr;
    TSParser* parser_ = nullptr;
    TSQueryCursor* cursor_ = nullptr;
    TSTree* tree_ = nullptr;
    TSQuery* query_ = nullptr;
    std::vector<int> captureKind_; // capture id -> TokenKind int, -1 = skip
    std::vector<int> lineStarts_;  // ascending; [0] == 0; empty = inactive
    bool dirty_ = true;            // tree_ lags behind buffer_
    bool spansDirty_ = true;
    std::vector<Span> spans_;
};

} // namespace qmn::view
