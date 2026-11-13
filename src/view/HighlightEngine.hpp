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
//    string wins inside; for identical spans the LATER pattern in the
//    query file wins, matching tree-sitter master's conflict convention
//    (and the written assumption of the current upstream py/js queries),
//    so those are applied LAST with pattern_index ascending).
//  - The v0.27.2 runtime does NOT evaluate query predicates: the cursor
//    emits EVERY syntactically matching capture (measured, #match?/@eq?
//    included). Under later-wins that makes the most general catch-all
//    patterns ((identifier) @variable first in py/js) stomp every
//    refined role, so the engine evaluates #eq?/#match? itself in
//    rebuildSpans and drops the capture when a predicate fails. Policy:
//    #eq? is exact text equality, #match? is an unanchored search (both
//    match tree-sitter's Oniguruma semantics); directives (#is-not?),
//    set-props (#set!) and #is? are environment facts we cannot compute
//    per capture yet, so they pass as satisfied - the pinned queries use
//    #is-not? only as a shadowing refinement with no visible effect on
//    our demo corpus.

#include "core/TextBuffer.hpp"
#include "view/CodeEditor.hpp"

#include <tree_sitter/api.h>

#include <QColor>
#include <QRegularExpression>
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
    // spans()/lineStarts()/folds().
    void refreshIfNeeded();

    const std::vector<Span>& spans() const { return spans_; }

    // M13 fold ranges (Phase E2): the @fold captures of a per-language fold
    // query, computed at the SAME refresh cadence and query window as spans
    // (so a big document stays viewport-scoped here too). A range is an
    // inclusive pair of MODEL lines with startLine < endLine: the fold hides
    // (startLine, endLine] — the header line always stays visible, and the
    // end row is the node's own last row (a node whose end_point sits at
    // column 0 of the next row loses that row: the exclusive end landed on
    // the newline, not on content). Sorted by (start asc, end desc),
    // duplicates merged; single-line captures dropped (nothing to hide).
    // Raw captures, so nested/overlapping ranges stay — the view normalizes
    // them into a proper fold tree. An EMPTY list either means "no fold
    // query for this language" (the view falls back to its indent scan) or
    // "not computed yet"; empty is never a wrong answer, only a missing one.
    struct FoldRange {
        int startLine;
        int endLine;
    };

    // The list the last refresh produced (window-scoped on big documents —
    // same caveat as spans()). The paint path reads this; it never pays a
    // whole-document fold query.
    const std::vector<FoldRange>& folds() const { return folds_; }
    // True when this language has a usable composed fold query: the view
    // then treats the grammar as the ONLY fold source (its indent scan
    // goes silent — js/json/python nesting follows syntax, not leading
    // whitespace). False for plain text or a fold query that failed to
    // compose, where the view's indent scan is the only fold source.
    bool hasFoldQuery() const { return foldQuery_ != nullptr; }
    // Exactness-forcing whole-document list (the spansFor escalation
    // pattern): CodeEditor::foldLine()/collapseAll()/foldableRanges()/M22
    // hover may address a fold outside the painted window, and
    // collapseAll() needs every fold. Guarantees EXACT ends: a
    // viewport-range-PARSED tree truncates folds at the parse seam, so a
    // coverage shortfall pays one synchronous full reparse (single-hit
    // cost, the spansFor rule); otherwise one whole-tree fold query on
    // the CURRENT tree, cached until the next scoped rebuild. Never
    // called from paint.
    const std::vector<FoldRange>& foldsFull();
    // Consistent with spans() after refreshIfNeeded(): lineStarts_[L] is
    // the document offset of line L. Only populated while active().
    const std::vector<int>& lineStarts() const { return lineStarts_; }

    // Fixed token colors in the VS Code Dark+ family. Since M24 this is
    // the FALLBACK under the theme's token map (CodeEditorTheme::
    // setTokenColor overrides per kind; the Dark preset is value-identical
    // to this table — a test tripwires that). An invalid return means
    // "use the default text pen"; that rule is unchanged.
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

    // One evaluated predicate step of a pattern, flattened at setLanguage
    // time from ts_query_predicates_for_pattern() (the runtime never
    // applies them itself). captureId is the GLOBAL capture id the @name
    // refers to - v0.27.2 keys captures by NAME (symbol table): a name
    // used by several patterns has one id, and the cursor emits exactly
    // that id in match.captures[].index (runtime query.c pushes
    // step->capture_ids verbatim; its own analysis cross-checks predicate
    // ids against step ids in the same namespace). So predicate targets
    // match captureKind_ / cursor indices directly, no remapping.
    // value holds the raw regex text for match (re compiled once per
    // language); value is unused for pass-through kinds.
    enum class PredicateOp { Eq, Match, Pass };
    struct Predicate {
        uint32_t captureId; // global capture id the @name refers to
        PredicateOp op;
        QString value;            // #eq? literal or #match? regex source
        QRegularExpression regex; // compiled for Match, null on compile failure
    };
    // Per-pattern predicate lists, indexed by pattern id; patterns
    // without predicates hold an empty vector.
    using PatternPredicates = std::vector<Predicate>;

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
    // byte_range, settable after exec - v0.27.2 measured). Each emitted
    // capture must also pass its pattern's predicates (see Predicate).
    void rebuildSpans(int qStart, int qEnd);
    // M13 fold-capture rebuild: same window semantics as rebuildSpans
    // (qStart < 0 = whole tree), no predicates (fold patterns are bare node
    // types). Runs exactly when rebuildSpans runs, so fold cost rides the
    // same at-most-once-per-keypress cadence and the same viewport scoping.
    void rebuildFolds(int qStart, int qEnd);
    // Node text as UTF-16 units via the buffer's piece view (the tree and
    // buffer are always current inside rebuildSpans).
    QString nodeText(const TSNode& node) const;
    // Flatten ts_query_predicates_for_pattern() steps into
    // queryPredicates_; runs once per setLanguage with a live query_.
    void buildQueryPredicates();

    // Adaptive parse mode (D2 threading sub-decision, bench-decided):
    // no threads for editor v1; instead measure the full incremental
    // parse and, once it busts the latency budget on a big document,
    // switch to viewport-range parsing anchored at the nearest col-0
    // line (restores top-level context for column-based grammars like
    // python). Threading stays deferred: M13 folding turned out NOT to
    // need whole trees on the paint path (fold captures ride the same
    // viewport window as highlight captures; only the explicit
    // collapseAll()/foldLine/M22-hover path pays one whole-tree fold
    // pass — the fold query, plus one full reparse when the PARSE itself
    // was window-clamped, see foldsFull()).
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
    std::vector<PatternPredicates> queryPredicates_; // indexed by pattern id
    // M13 fold query (Phase E2): composed `(node) @fold` patterns, one per
    // node type of the language's fold-node list (see kFold* in the .cpp).
    // Null = no fold query for this language -> the view's indent fallback
    // is the only fold source. foldsWhole_ records whether the last fold
    // pass covered the whole tree (foldsFull) or the spans window.
    TSQuery* foldQuery_ = nullptr;
    TSQueryCursor* foldCursor_ = nullptr;
    std::vector<FoldRange> folds_;
    bool foldsWhole_ = false;
    std::vector<int> lineStarts_;  // ascending; [0] == 0; empty = inactive
    bool dirty_ = true;            // tree_ lags behind buffer_
    bool spansDirty_ = true;
    std::vector<Span> spans_;
};

} // namespace qmn::view
