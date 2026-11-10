#include "view/HighlightEngine.hpp"

#include "view/QueryData.hpp"

#include <QElapsedTimer>

#include <algorithm>
#include <cstring>

// Generated parsers export their entry points with C linkage; the
// vendored grammars ship no public header declaring these (their
// src/tree_sitter/ is the parser's private half), so declare them here.
extern "C" {
const TSLanguage* tree_sitter_python(void);
const TSLanguage* tree_sitter_json(void);
const TSLanguage* tree_sitter_javascript(void);
}

namespace qmn::view {

namespace {

using TokenKind = CodeEditor::TokenKind;

struct LangEntry {
    const char* id;
    const TSLanguage* (*langFn)();
    const char* (*queryFn)();
};

// Registry order is surfaced by CodeEditor::availableLanguages().
const LangEntry kRegistry[] = {
    {"python", &tree_sitter_python, &queries::pythonHighlights},
    {"json", &tree_sitter_json, &queries::jsonHighlights},
    {"javascript", &tree_sitter_javascript, &queries::javascriptHighlights},
};

// Capture name -> TokenKind: the union of the capture names used by the
// three vendored highlights.scm files (VENDORED.md keeps them unmodified
// upstream copies; filtering unknown names is this table's job). Names
// outside the table map to -1 and their captures are dropped.
int kindForCapture(const char* name, uint32_t len)
{
    struct Entry {
        const char* name;
        int kind;
    };
    static const Entry table[] = {
        {"comment", int(TokenKind::Comment)},
        {"string", int(TokenKind::String)},
        {"string.special", int(TokenKind::StringSpecial)},
        {"string.special.key", int(TokenKind::StringSpecialKey)},
        {"number", int(TokenKind::Number)},
        {"keyword", int(TokenKind::Keyword)},
        {"operator", int(TokenKind::Operator)},
        {"punctuation", int(TokenKind::Punctuation)},
        {"punctuation.bracket", int(TokenKind::PunctuationBracket)},
        {"punctuation.delimiter", int(TokenKind::PunctuationDelimiter)},
        {"punctuation.special", int(TokenKind::PunctuationSpecial)},
        {"function", int(TokenKind::Function)},
        {"function.builtin", int(TokenKind::FunctionBuiltin)},
        {"function.method", int(TokenKind::FunctionMethod)},
        {"constructor", int(TokenKind::Constructor)},
        {"type", int(TokenKind::Type)},
        {"constant", int(TokenKind::Constant)},
        {"constant.builtin", int(TokenKind::ConstantBuiltin)},
        {"variable", int(TokenKind::Variable)},
        {"variable.builtin", int(TokenKind::VariableBuiltin)},
        {"property", int(TokenKind::Property)},
        {"escape", int(TokenKind::Escape)},
        {"embedded", int(TokenKind::Embedded)},
    };
    for (const Entry& e : table)
        if (std::strlen(e.name) == len && std::memcmp(e.name, name, len) == 0)
            return e.kind;
    return -1;
}

} // namespace

// Zero-copy pull reader (PLAN.md D2 core decision). The lexer calls with
// byte-aligned offsets only under TSInputEncodingUTF16LE (it advances in
// 2-byte units), so byte/2 is a code-unit offset; the returned pointer
// borrows the piece data, valid until the next mutation - parsing never
// interleaves with edits (single-threaded GUI discipline).
const char* HighlightEngine::readPiece(void* payload, uint32_t byteIndex,
                                       TSPoint, uint32_t* bytesRead)
{
    auto* self = static_cast<HighlightEngine*>(payload);
    const auto pv = self->buffer_->pieceAt(int(byteIndex / 2));
    if (!pv.data || pv.len <= 0) {
        *bytesRead = 0;
        return nullptr; // EOF for the lexer
    }
    *bytesRead = uint32_t(pv.len) * 2;
    return reinterpret_cast<const char*>(pv.data);
}

TSInput HighlightEngine::makeInput() const
{
    TSInput in;
    in.payload = const_cast<HighlightEngine*>(this);
    in.read = &HighlightEngine::readPiece;
    in.encoding = TSInputEncodingUTF16LE;
    in.decode = nullptr; // UTF16LE decoding is built into the runtime
    return in;
}

HighlightEngine::HighlightEngine()
{
    parser_ = ts_parser_new();
    cursor_ = ts_query_cursor_new();
}

HighlightEngine::~HighlightEngine()
{
    ts_tree_delete(tree_);
    ts_query_delete(query_);
    ts_query_cursor_delete(cursor_);
    ts_parser_delete(parser_);
}

void HighlightEngine::attach(const qmn::core::TextBuffer* buffer)
{
    buffer_ = buffer;
}

QStringList HighlightEngine::languageIds()
{
    QStringList ids;
    for (const LangEntry& e : kRegistry)
        ids << QString::fromLatin1(e.id);
    return ids;
}

void HighlightEngine::clearState()
{
    ts_tree_delete(tree_);
    tree_ = nullptr;
    ts_query_delete(query_);
    query_ = nullptr;
    captureKind_.clear();
    lineStarts_.clear();
    spans_.clear();
    lang_ = nullptr;
    dirty_ = true;
    spansDirty_ = true;
    mode_ = Mode::Full;
    spansMode_ = Mode::Full;
    viewportSet_ = false;
    windowDirty_ = false;
    covStart_ = covEnd_ = 0;
    spanQStart_ = spanQEnd_ = 0;
    lastParseMs_ = 0.0;
}

void HighlightEngine::setLanguage(const QString& id)
{
    const QString norm = id.trimmed().toLower();
    if (norm == languageId_)
        return;
    clearState();
    languageId_.clear();
    const LangEntry* hit = nullptr;
    for (const LangEntry& e : kRegistry) {
        if (QString::fromLatin1(e.id) == norm) {
            hit = &e;
            break;
        }
    }
    if (!hit)
        return; // "" or unknown: plain text, and language() stays empty
    if (!ts_parser_set_language(parser_, hit->langFn()))
        return;
    lang_ = hit->langFn();
    languageId_ = norm;

    const char* src = hit->queryFn();
    uint32_t errOffset = 0;
    TSQueryError err = TSQueryErrorNone;
    query_ = ts_query_new(lang_, src, uint32_t(std::strlen(src)), &errOffset, &err);
    // A broken query (it must not happen with the committed .scm pins, but
    // never trust embedded data) only costs colors: parsing and line
    // tracking keep working, so folding/etc. can reuse the tree later.
    if (query_) {
        const uint32_t n = ts_query_capture_count(query_);
        captureKind_.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t len = 0;
            const char* name = ts_query_capture_name_for_id(query_, i, &len);
            captureKind_[i] = kindForCapture(name, len);
        }
    }
    rebuildLineIndex();
    dirty_ = true; // first parse is a full parse (old tree is null)
    spansDirty_ = true;
}

void HighlightEngine::rebuildLineIndex()
{
    lineStarts_.clear();
    lineStarts_.push_back(0); // lineCount() >= 1: offset 0 always exists
    int off = 0;
    const int n = buffer_->charCount();
    while (off < n) {
        const auto pv = buffer_->pieceAt(off);
        if (!pv.data || pv.len <= 0)
            break; // defensive: never spin on a broken view
        for (int i = 0; i < pv.len; ++i)
            if (pv.data[i] == u'\n')
                lineStarts_.push_back(off + i + 1);
        off += pv.len;
    }
}

TSPoint HighlightEngine::pointOfUnit(int offset) const
{
    // lineStarts_ is sorted; line = index of the last entry <= offset.
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    int line = int(it - lineStarts_.begin()) - 1;
    if (line < 0)
        line = 0;
    const int col = offset - lineStarts_[size_t(line)];
    return TSPoint{uint32_t(line), uint32_t(col) * 2}; // byte = 2x code units
}

void HighlightEngine::updateLineIndex(const qmn::core::TextBuffer::EditEvent& ev)
{
    // Pre-op frame: drop line starts that fell inside the replaced range
    // (start, end] - a deleted '\n' merges its next line, and its start
    // offset is exactly the offset after that '\n', i.e. in (start, end].
    auto b1 = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), ev.start);
    auto b2 = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), ev.end);
    lineStarts_.erase(b1, b2);
    const size_t tail = size_t(b1 - lineStarts_.begin()); // erase() already
                                                          // invalidated b1/b2
    const int delta = int(ev.inserted.size()) - (ev.end - ev.start);
    for (size_t i = tail; i < lineStarts_.size(); ++i)
        lineStarts_[i] += delta;
    // Newlines inside the inserted text create line starts at absolute
    // post-op offsets; they are all below the shifted tail (which starts
    // at >= old end + 1 + delta > inserted end), so they go at `tail`.
    size_t at = tail;
    for (int i = 0; i < int(ev.inserted.size()); ++i)
        if (ev.inserted.at(i) == u'\n')
            lineStarts_.insert(lineStarts_.begin() + long(at++), ev.start + i + 1);
}

void HighlightEngine::onEdit(const qmn::core::TextBuffer::EditEvent& ev)
{
    if (!lang_)
        return; // no tree to maintain, no index in use
    if (tree_) {
        // All three points in the PRE-op frame (update the index only
        // after computing them). new_end_point = start_point advanced by
        // the inserted text's extent (row/col arithmetic on the insert).
        const TSPoint sp = pointOfUnit(ev.start);
        const TSPoint ep = pointOfUnit(ev.end);
        int row = int(sp.row);
        int colUnits = ev.start - lineStarts_[size_t(row)];
        for (const QChar& c : ev.inserted) {
            if (c == u'\n') {
                ++row;
                colUnits = 0;
            } else {
                ++colUnits;
            }
        }
        const TSInputEdit te{
            .start_byte = uint32_t(ev.start) * 2,
            .old_end_byte = uint32_t(ev.end) * 2,
            .new_end_byte = uint32_t(ev.start + int(ev.inserted.size())) * 2,
            .start_point = sp,
            .old_end_point = ep,
            .new_end_point = TSPoint{uint32_t(row), uint32_t(colUnits) * 2},
        };
        ts_tree_edit(tree_, &te);
    }
    updateLineIndex(ev);
    dirty_ = true;
    spansDirty_ = true;
}

void HighlightEngine::reparseFull()
{
    ts_parser_set_included_ranges(parser_, nullptr, 0); // count 0 = whole doc
    QElapsedTimer t;
    t.start();
    TSTree* fresh = ts_parser_parse(parser_, tree_, makeInput());
    lastParseMs_ = double(t.nsecsElapsed()) / 1e6;
    ts_tree_delete(tree_); // caller owns the old tree until replaced
    tree_ = fresh;
    covStart_ = 0;
    covEnd_ = buffer_->charCount();
    dirty_ = false;
    spansDirty_ = true;
}

int HighlightEngine::lineForUnit(int offset) const
{
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    const int line = int(it - lineStarts_.begin()) - 1;
    return line < 0 ? 0 : line;
}

int HighlightEngine::anchorLineFor(int topLine) const
{
    // Top-level boundaries of python (and of the other pinned grammars'
    // typical code) sit at column 0: def/class/import, statement starts.
    // Indented lines are inside some block whose opener lies at/above the
    // first col-0 line, so starting the range there restores enough
    // context for the column-based external indent scanner to align.
    // Bench-verified (tools/bench_ts_split.cpp): captures inside the
    // window are IDENTICAL to the full tree's. A json-style document with
    // no col-0 line inside the cap ends up anchored at 0 - still cheap,
    // and json never enters this mode anyway (incremental stays ~1ms).
    const int n = buffer_->charCount();
    for (int l = topLine; l >= 0 && topLine - l <= kAnchorScanCap; --l) {
        const int s = lineStarts_[size_t(l)];
        if (s >= n)
            continue; // empty tail line
        if (!buffer_->textRange(s, s + 1).at(0).isSpace())
            return l;
    }
    return 0;
}

bool HighlightEngine::reparseViewport()
{
    if (lineStarts_.empty())
        return false;
    const int n = buffer_->charCount();
    const int topLine = lineForUnit(vpStart_);
    const int botLine = lineForUnit(vpEnd_ > 0 ? vpEnd_ - 1 : 0);
    const int anchor = anchorLineFor(topLine);
    // TSRange point/byte pairs MUST agree (same trap as TSInputEdit:
    // inconsistent coordinates poison the parser; measured crash).
    TSRange r{};
    r.start_byte = uint32_t(lineStarts_[size_t(anchor)]) * 2;
    r.start_point = TSPoint{uint32_t(anchor), 0};
    if (size_t(botLine) + 1 < lineStarts_.size()) {
        r.end_byte = uint32_t(lineStarts_[size_t(botLine) + 1]) * 2; // past the '\n'
        r.end_point = TSPoint{uint32_t(botLine + 1), 0};
    } else {
        r.end_byte = uint32_t(n) * 2;
        r.end_point = pointOfUnit(n); // end of the document
    }
    if (!ts_parser_set_included_ranges(parser_, &r, 1)) {
        ts_parser_set_included_ranges(parser_, nullptr, 0);
        return false;
    }
    QElapsedTimer t;
    t.start();
    TSTree* fresh = ts_parser_parse(parser_, tree_, makeInput());
    lastParseMs_ = double(t.nsecsElapsed()) / 1e6;
    ts_tree_delete(tree_);
    tree_ = fresh;
    if (!tree_) {
        ts_parser_set_included_ranges(parser_, nullptr, 0);
        covStart_ = covEnd_ = 0;
        return false;
    }
    covStart_ = lineStarts_[size_t(anchor)];
    covEnd_ = int(r.end_byte) / 2; // the parser's end is exclusive in bytes too
    dirty_ = false;
    spansDirty_ = true;
    return true;
}

void HighlightEngine::setViewport(int startUnit, int endUnit)
{
    if (!lang_) {
        viewportSet_ = false;
        return;
    }
    const int n = buffer_->charCount();
    startUnit = std::clamp(startUnit, 0, n);
    endUnit = std::clamp(endUnit, startUnit, n);
    if (!viewportSet_ || startUnit != vpStart_ || endUnit != vpEnd_) {
        vpStart_ = startUnit;
        vpEnd_ = endUnit;
        windowDirty_ = true;
        viewportSet_ = true;
    }
}

void HighlightEngine::refreshIfNeeded()
{
    if (!lang_)
        return;

    // A moved window needs a fresh tree only in Viewport mode (the old
    // window-tree does not cover the new lines); scroll cost measured
    // 0.04-0.4ms. Full-mode trees always cover the whole document.
    const bool needsParse =
        dirty_ || (mode_ == Mode::Viewport && viewportSet_ && windowDirty_);
    if (needsParse) {
        bool vpDone = false;
        if (mode_ == Mode::Viewport)
            vpDone = reparseViewport();
        else
            reparseFull();
        if (mode_ == Mode::Viewport && !vpDone) {
            mode_ = Mode::Full;
            reparseFull();
        }
        // Adaptive switch: a busted budget means this grammar's
        // incremental reuse is dead weight (python's indent scanner);
        // viewport parsing takes over from the NEXT reparse, while this
        // paint still gets its full-document spans.
        if (mode_ == Mode::Full && lastParseMs_ > kParseBudgetMs && viewportSet_
            && buffer_->charCount() >= kViewportMinUnits)
            mode_ = Mode::Viewport;
    }

    // Query scoping: once the document is big, never walk whole-document
    // captures again (js measured 24.3ms/196k captures per keystroke);
    // the visible window is enough for paint, and spansFor() re-queries
    // synchronously for programmatic requests outside it.
    const bool scoped = viewportSet_ && buffer_->charCount() >= kViewportMinUnits;
    const int qStart = scoped ? vpStart_ : -1;
    const int qEnd = scoped ? vpEnd_ : -1;
    const bool queryStale =
        (spansMode_ == Mode::Viewport) != scoped
        || (scoped && (spanQStart_ != vpStart_ || spanQEnd_ != vpEnd_));
    if (spansDirty_ || queryStale)
        rebuildSpans(qStart, qEnd);
    windowDirty_ = false;
}

const std::vector<HighlightEngine::Span>& HighlightEngine::spansFor(int startUnit, int endUnit)
{
    refreshIfNeeded();
    if (!tree_ || startUnit >= endUnit)
        return spans_;
    if (spansMode_ == Mode::Full)
        return spans_; // spans_ already cover the whole document
    if (startUnit >= spanQStart_ && endUnit <= spanQEnd_)
        return spans_; // current scoped query already covers the window
    if (startUnit >= covStart_ && endUnit <= covEnd_) {
        // Tree reaches the window; just re-query it at the requested
        // range (bench: whole-window query exec is 0.08-0.29ms).
        rebuildSpans(startUnit, endUnit);
        return spans_;
    }
    // Window parse never saw this region: pay one synchronous full pass
    // for exactness. The adaptive rule flips Viewport back on the next
    // paint's reparse, so a stray whole-document dump costs a single hit.
    mode_ = Mode::Full;
    reparseFull();
    rebuildSpans(-1, -1);
    return spans_;
}

void HighlightEngine::rebuildSpans(int qStart, int qEnd)
{
    spans_.clear();
    spansDirty_ = false;
    if (!query_ || !tree_) {
        spansMode_ = Mode::Full;
        spanQStart_ = 0;
        spanQEnd_ = buffer_->charCount();
        return;
    }

    ts_query_cursor_exec(cursor_, query_, ts_tree_root_node(tree_));
    if (qStart >= 0 && qStart < qEnd) {
        // v0.27.2 allows setting the range after exec; captures fully
        // outside [qStart, qEnd) are then never emitted.
        ts_query_cursor_set_byte_range(cursor_, uint32_t(qStart) * 2, uint32_t(qEnd) * 2);
        spansMode_ = Mode::Viewport;
        spanQStart_ = qStart;
        spanQEnd_ = qEnd;
    } else {
        ts_query_cursor_set_byte_range(cursor_, 0, UINT32_MAX); // reset a previous scope
        spansMode_ = Mode::Full;
        spanQStart_ = 0;
        spanQEnd_ = buffer_->charCount();
    }
    std::vector<Capture> caps;
    TSQueryMatch match;
    uint32_t capIdx = 0;
    uint32_t seq = 0;
    while (ts_query_cursor_next_capture(cursor_, &match, &capIdx)) {
        const int kind = captureKind_[match.captures[capIdx].index];
        if (kind < 0)
            continue; // capture name outside our table: drop it
        const TSNode n = match.captures[capIdx].node;
        const int s = int(ts_node_start_byte(n)) / 2; // byte = 2x units
        const int e = int(ts_node_end_byte(n)) / 2;
        if (s >= e)
            continue; // zero-width captures carry nothing to paint
        caps.push_back(Capture{s, e, TokenKind(kind), match.pattern_index, seq++});
    }

    // Deterministic paint-over order, independent of cursor emission
    // order: ascending start (inner/later captures land on top of their
    // enclosing ones), then ascending end (longer beats shorter at equal
    // start), then DESCENDING pattern index so that among identical spans
    // the earlier query pattern is applied last and wins - tree-sitter's
    // own conflict convention (json's pair-key capture beating the plain
    // (string) capture over the same node depends on this).
    std::stable_sort(caps.begin(), caps.end(), [](const Capture& a, const Capture& b) {
        if (a.start != b.start)
            return a.start < b.start;
        if (a.end != b.end)
            return a.end < b.end;
        if (a.pattern != b.pattern)
            return a.pattern > b.pattern;
        return a.seq < b.seq;
    });

    for (const Capture& c : caps) {
        // Paint [c.start, c.end) over the merged list: split the left
        // neighbor that straddles the range start, then clip/erase every
        // following span that intersects, then insert.
        auto first = std::upper_bound(
            spans_.begin(), spans_.end(), c.start,
            [](int v, const Span& s) { return v < s.end; });
        size_t i = size_t(first - spans_.begin());
        if (i < spans_.size() && spans_[i].start < c.start) {
            spans_[i].end = c.start; // keep the left remnant visible
            ++i;
        }
        while (i < spans_.size() && spans_[i].start < c.end) {
            if (spans_[i].end <= c.end)
                spans_.erase(spans_.begin() + long(i));
            else {
                spans_[i].start = c.end; // right remnant survives the repaint
                ++i;
            }
        }
        spans_.insert(spans_.begin() + long(i), Span{c.start, c.end, c.kind});
    }
}

QColor HighlightEngine::colorFor(CodeEditor::TokenKind kind)
{
    // v1 fixed colors in the VS Code Dark+ family; they stay legible on
    // light backgrounds too (monospace spike defaults to the platform
    // Base). A real theme bridge (Monaco palette / set_theme parity) is a
    // later milestone - until then this table is the ONLY color source.
    switch (kind) {
    case CodeEditor::TokenKind::Comment: return QColor(0x6A, 0x99, 0x55);
    case CodeEditor::TokenKind::Keyword: return QColor(0x56, 0x9C, 0xD6);
    case CodeEditor::TokenKind::Number: return QColor(0xB5, 0xCE, 0xA8);
    case CodeEditor::TokenKind::String: return QColor(0xCE, 0x91, 0x78);
    case CodeEditor::TokenKind::StringSpecial: return QColor(0xD7, 0xBA, 0x7D);
    case CodeEditor::TokenKind::StringSpecialKey: return QColor(0x9C, 0xDC, 0xFE);
    case CodeEditor::TokenKind::Escape: return QColor(0xD7, 0xBA, 0x7D);
    case CodeEditor::TokenKind::Embedded: return QColor(0x9C, 0xDC, 0xFE);
    case CodeEditor::TokenKind::Constant: return QColor(0x4F, 0xC1, 0xFF);
    case CodeEditor::TokenKind::ConstantBuiltin: return QColor(0x56, 0x9C, 0xD6);
    case CodeEditor::TokenKind::Constructor: return QColor(0x4E, 0xC9, 0xB0);
    case CodeEditor::TokenKind::Function: return QColor(0xDC, 0xDC, 0xAA);
    case CodeEditor::TokenKind::FunctionBuiltin: return QColor(0xDC, 0xDC, 0xAA);
    case CodeEditor::TokenKind::FunctionMethod: return QColor(0xDC, 0xDC, 0xAA);
    case CodeEditor::TokenKind::Type: return QColor(0x4E, 0xC9, 0xB0);
    case CodeEditor::TokenKind::Variable: return QColor(0x9C, 0xDC, 0xFE);
    case CodeEditor::TokenKind::VariableBuiltin: return QColor(0x9C, 0xDC, 0xFE);
    case CodeEditor::TokenKind::Property: return QColor(0x9C, 0xDC, 0xFE);
    case CodeEditor::TokenKind::Operator: return QColor(0xD4, 0xD4, 0xD4);
    case CodeEditor::TokenKind::Punctuation: return QColor(0xD4, 0xD4, 0xD4);
    case CodeEditor::TokenKind::PunctuationBracket: return QColor(0xD4, 0xD4, 0xD4);
    case CodeEditor::TokenKind::PunctuationDelimiter: return QColor(0xD4, 0xD4, 0xD4);
    case CodeEditor::TokenKind::PunctuationSpecial: return QColor(0xD4, 0xD4, 0xD4);
    case CodeEditor::TokenKind::Text: break;
    }
    return QColor(); // invalid: default text pen
}

} // namespace qmn::view
