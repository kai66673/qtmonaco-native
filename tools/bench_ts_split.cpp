// Split of the M4 keystroke cost into tree-sitter parts (PLAN.md D2
// threading sub-decision input). Raw tree-sitter API against the core's
// piece table, mirroring HighlightEngine's input path - no engine, so
// parse vs query exec can be timed separately, plus the viewport-scoped
// query option (ts_query_cursor_set_byte_range).
//
//   g++ -O3 -std=c++20 tools/bench_ts_split.cpp -Isrc \
//       build/src/core/libqmn_core.a build/third_party/tree-sitter/libqmn_ts.a \
//       -isystem /usr/include/qt6/QtCore -isystem /usr/include/qt6 \
//       -lQt6Core -o bench_ts_split <file-with-python-source-or-none>

#include "core/TextBuffer.hpp"

#include <tree_sitter/api.h>

#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <tuple>
#include <vector>

extern "C" const TSLanguage* tree_sitter_python(void);
extern "C" const TSLanguage* tree_sitter_json(void);
extern "C" const TSLanguage* tree_sitter_javascript(void);

using qmn::core::TextBuffer;

static const char* readPiece(void* payload, uint32_t byteIndex, TSPoint, uint32_t* bytesRead)
{
    auto* buf = static_cast<TextBuffer*>(payload);
    const auto pv = buf->pieceAt(int(byteIndex / 2));
    if (!pv.data || pv.len <= 0) {
        *bytesRead = 0;
        return nullptr;
    }
    *bytesRead = uint32_t(pv.len) * 2;
    return reinterpret_cast<const char*>(pv.data);
}

static TSInput makeInput(TextBuffer* buf)
{
    TSInput in;
    in.payload = buf;
    in.read = &readPiece;
    in.encoding = TSInputEncodingUTF16LE;
    in.decode = nullptr;
    return in;
}

static QString syntheticPython(int lines)
{
    QString out;
    int block = 0;
    while (out.count(QLatin1Char('\n')) < lines) {
        out += QStringLiteral("def func%1(arg_a, arg_b):\n").arg(block);
        out += QStringLiteral("    total = arg_a + arg_b  # accumulate func%1\n").arg(block);
        out += QStringLiteral("    if total > 100:\n        return \"value-%1\" %% total\n").arg(block);
        out += QStringLiteral("    return [x for x in range(arg_a, arg_b)]\n\n");
        ++block;
    }
    return out;
}

int main(int argc, char** argv)
{
    const QByteArray lang = argc > 1 ? QByteArray(argv[1]) : QByteArray("python");
    const bool isJson = lang == "json";
    const bool isJs = lang == "javascript";
    const TSLanguage* (*langFn)() =
        isJson ? &tree_sitter_json : (isJs ? &tree_sitter_javascript : &tree_sitter_python);

    QString text;
    if (argc > 2) {
        QFile f(argv[2]);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::fprintf(stderr, "cannot open %s\n", argv[2]);
            return 1;
        }
        text = QString::fromUtf8(f.readAll());
    } else if (isJson) {
        QString out = QStringLiteral("[\n");
        for (int i = 0; i < 4000; ++i)
            out += QStringLiteral("  {\"name\": \"item-%1\", \"qty\": %2, \"flag\": %3},\n")
                       .arg(i).arg(i % 97).arg(i % 2 ? "true" : "false");
        out += QStringLiteral("  {\"name\": \"last\", \"qty\": 0, \"flag\": false}\n]\n");
        text = out;
    } else if (isJs) {
        QString out;
        for (int i = 0; i < 4000; ++i)
            out += QStringLiteral("function func%1(a, b) { const t = a + b; return t; } // c%1\n").arg(i);
        text = out;
    } else {
        text = syntheticPython(4000);
    }
    QString qscm;
    {
        QFile q(QStringLiteral("third_party/tree-sitter/grammars/%1/queries/highlights.scm")
                    .arg(QString::fromLatin1(lang)));
        if (!q.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "cannot open highlights.scm (run from repo root)\n");
            return 1;
        }
        qscm = QString::fromUtf8(q.readAll());
    }

    TextBuffer buffer;
    buffer.setText(text);
    const int units = buffer.charCount();

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, langFn());
    TSQueryError errType = TSQueryErrorNone;
    uint32_t errOff = 0;
    const QByteArray scm = qscm.toUtf8();
    TSQuery* query = ts_query_new(langFn(), scm.constData(), uint32_t(scm.size()), &errOff, &errType);
    if (!query) {
        std::fprintf(stderr, "query error %d @%u\n", int(errType), errOff);
        return 1;
    }
    TSQueryCursor* cursor = ts_query_cursor_new();
    TSTree* tree = nullptr;

    QElapsedTimer t;
    std::printf("doc: %d UTF-16 units, %d lines\n", units, text.count('\n') + 1);

    // 1) cold parse
    t.start();
    tree = ts_parser_parse(parser, nullptr, makeInput(&buffer));
    std::printf("cold parse:                 %8.2f ms\n", double(t.nsecsElapsed()) / 1e6);

    // 2) full-document query exec (tree clean, repeated)
    auto execQuery = [&](bool viewport, uint32_t b0, uint32_t b1) {
        t.start();
        ts_query_cursor_exec(cursor, query, ts_tree_root_node(tree));
        if (viewport)
            ts_query_cursor_set_byte_range(cursor, b0, b1);
        TSQueryMatch m;
        uint32_t ci = 0;
        unsigned n = 0;
        while (ts_query_cursor_next_capture(cursor, &m, &ci))
            ++n;
        const double ms = double(t.nsecsElapsed()) / 1e6;
        return std::make_pair(ms, n);
    };
    {
        auto [ms, n] = execQuery(false, 0, 0);
        std::printf("query full doc (clean tree):%8.2f ms  (%u captures)\n", ms, n);
        // viewport = first ~50 lines
        const int lineStart0 = buffer.offsetOfLine(0);
        const int lineEnd50 = buffer.offsetOfLine(std::min(49, int(text.count('\n'))));
        auto [msv, nv] = execQuery(true, uint32_t(lineStart0) * 2, uint32_t(lineEnd50 + 1) * 2);
        std::printf("query 50-line viewport:     %8.2f ms  (%u captures)\n", msv, nv);
    }

    // 3) incremental: random single-char edits, each = ts_tree_edit + parse
    quint64 seed = 0xC0FFEE; // uint64 LCG: SIGNED overflow would be UB and
    std::vector<double> parseMs, editMs; // -O2 folds it into garbage offsets
    int applied = 0, skipped = 0;
    for (int i = 0; i < 300; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        int off = int(seed % quint64(std::max(1, buffer.charCount() - 1)));
        if (isJson) { // keep the document valid: insert digits INSIDE numbers
            while (off > 0 && !buffer.textRange(off, off + 1).at(0).isDigit())
                --off;
        } else { // keep it valid AND typical: letters inside LONG identifiers
            // (>=8 letters: never inside a keyword, and matches real typing
            // into names like `accumulate`/`funcNN`)
            int start = off, end = off;
            while (start > 0 && buffer.textRange(start - 1, start).at(0).isLetterOrNumber())
                --start;
            while (end < buffer.charCount() && buffer.textRange(end, end + 1).at(0).isLetterOrNumber())
                ++end;
            if (end - start < 8) { ++skipped; continue; }
            ++applied;
            off = (start + end) / 2;
        }
        // Edit the document FIRST (same order as the widget: buffer op
        // fires the event, we translate it to ts_tree_edit here).
        // The points MUST be consistent with the byte offsets - inconsistent
        // edits poison the incremental-reuse path (measured: segfaults).
        const auto pre = buffer.lineColAt(off);
        buffer.insertAtOffset(off, QStringLiteral("x"));
        const auto post = buffer.lineColAt(off + 1);
        t.start();
        const TSInputEdit te{
            .start_byte = uint32_t(off) * 2,
            .old_end_byte = uint32_t(off) * 2,
            .new_end_byte = uint32_t(off + 1) * 2,
            .start_point = {uint32_t(pre.line), uint32_t(pre.col) * 2},
            .old_end_point = {uint32_t(pre.line), uint32_t(pre.col) * 2},
            .new_end_point = {uint32_t(post.line), uint32_t(post.col) * 2},
        };
        ts_tree_edit(tree, &te);
        editMs.push_back(double(t.nsecsElapsed()) / 1e6);
        t.start();
        TSTree* fresh = ts_parser_parse(parser, tree, makeInput(&buffer));
        ts_tree_delete(tree);
        tree = fresh;
        parseMs.push_back(double(t.nsecsElapsed()) / 1e6);
    }
    std::sort(parseMs.begin(), parseMs.end());
    std::printf("edit placement: %d applied, %d skipped (short run / invalid)\n", applied, skipped);
    if (parseMs.empty()) { std::printf("NO EDITS APPLIED - stats skipped\n"); return 1; }
    std::printf("incremental parse x%d:       mean %7.3f ms  p50 %7.3f  p95 %7.3f  max %7.3f ms\n",
                std::accumulate(parseMs.begin(), parseMs.end(), 0.0) / parseMs.size(),
                parseMs[parseMs.size() / 2], parseMs[parseMs.size() * 95 / 100], parseMs.back());
    std::printf("ts_tree_edit alone:         mean %7.3f ms\n",
                std::accumulate(editMs.begin(), editMs.end(), 0.0) / editMs.size());

    // 4) incremental parse + viewport query combined = realistic keystroke
    //    cost with a viewport-scoped query.
    {
        auto [msv, nv] = execQuery(true, 0, uint32_t(buffer.offsetOfLine(49) + 1) * 2);
        std::printf("keystroke model: parse+viewport query ~= %.3f + %.3f = %.3f ms\n",
                    parseMs[parseMs.size() / 2], msv, parseMs[parseMs.size() / 2] + msv);
    }

    // 5) Viewport-limited parsing (ts_parser_set_included_ranges): only
    //    the visible window is included content; the question is whether
    //    the lexer skips excluded regions cheaply (GUI-thread bound
    //    without threads), and what a SCROLL (range change) costs.
    //    5b) CORRECTNESS: captures from the viewport-tree query must
    //    match captures from the FULL tree restricted to the same range
    //    (with the top anchor = first non-indented line above, which
    //    restores top-level context for column-based grammars).
    {
        const int lineA = 2000, lineB = 2050;
        if (lineA + 100 >= buffer.lineCount()) {
            std::printf("viewport-range parse: skipped (doc too short)\n");
        } else {
            // Top anchor: first line at/above lineA starting at column 0
            // (non-blank) = top-level context for column-based grammars.
            int anchor = lineA;
            for (int l = lineA; l >= 0; --l) {
                const QString lt = buffer.lineText(l);
                if (!lt.isEmpty() && !lt.at(0).isSpace()) {
                    anchor = l;
                    break;
                }
            }
            const int lastLine = std::min(lineB, buffer.lineCount() - 1);
            // Point/byte pairs in TSRange MUST be consistent (the same
            // lesson as TSInputEdit): end_byte = start of line lastLine+1
            // pairs with end_point = {lastLine + 1, 0}.
            TSRange r{ TSPoint{uint32_t(anchor), 0}, TSPoint{uint32_t(lastLine + 1), 0},
                       uint32_t(buffer.offsetOfLine(anchor)) * 2,
                       uint32_t(buffer.offsetOfLine(lastLine + 1)) * 2 };
            std::printf("viewport parse anchor line: %d (viewport %d..%d)\n", anchor, lineA, lastLine);
            if (ts_parser_set_included_ranges(parser, &r, 1)) {
                t.start();
                TSTree* vp = ts_parser_parse(parser, tree, makeInput(&buffer));
                std::printf("viewport-range parse (fresh): %8.2f ms\n", double(t.nsecsElapsed()) / 1e6);
                t.start();
                TSTree* vp2 = ts_parser_parse(parser, vp, makeInput(&buffer));
                std::printf("viewport-range parse (same range, vp tree): %8.2f ms\n",
                            double(t.nsecsElapsed()) / 1e6);
                // Scroll: shift the window and reparse against the partial tree.
                const int newStart = lineA + 50;
                const int newEnd = std::min(lastLine + 50, buffer.lineCount() - 2);
                r.start_byte = uint32_t(buffer.offsetOfLine(newStart)) * 2;
                r.end_byte = uint32_t(buffer.offsetOfLine(newEnd + 1)) * 2;
                r.start_point = TSPoint{uint32_t(newStart), 0};
                r.end_point = TSPoint{uint32_t(newEnd + 1), 0};
                ts_parser_set_included_ranges(parser, &r, 1);
                t.start();
                TSTree* vp3 = ts_parser_parse(parser, vp2, makeInput(&buffer));
                std::printf("viewport-range parse (scroll): %8.2f ms\n", double(t.nsecsElapsed()) / 1e6);

                // 5b) correctness diff: full tree vs viewport tree, same window.
                const uint32_t w0 = uint32_t(buffer.offsetOfLine(lineA)) * 2;
                const uint32_t w1 = uint32_t(buffer.offsetOfLine(lastLine) + 1) * 2;
                auto collect = [&](TSTree* tr) {
                    std::vector<std::tuple<uint32_t, uint32_t, uint16_t>> out;
                    ts_query_cursor_exec(cursor, query, ts_tree_root_node(tr));
                    ts_query_cursor_set_byte_range(cursor, w0, w1);
                    TSQueryMatch m;
                    uint32_t ci = 0;
                    while (ts_query_cursor_next_capture(cursor, &m, &ci)) {
                        const TSNode n = m.captures[ci].node;
                        const uint32_t s = ts_node_start_byte(n), e = ts_node_end_byte(n);
                        if (s < w1 && e > w0)
                            out.emplace_back(s, e, m.captures[ci].index);
                    }
                    std::sort(out.begin(), out.end());
                    return out;
                };
                const auto fullC = collect(tree);
                const auto vpC = collect(vp2);
                std::printf("viewport capture match: full=%zu vp=%zu %s\n", fullC.size(), vpC.size(),
                            fullC == vpC ? "IDENTICAL" : "MISMATCH");
                if (fullC != vpC) {
                    for (size_t i = 0; i < std::min(fullC.size(), vpC.size()); ++i)
                        if (fullC[i] != vpC[i]) {
                            uint32_t l1 = 0, l2 = 0;
                            const char* n1 = ts_query_capture_name_for_id(query, std::get<2>(fullC[i]), &l1);
                            const char* n2 = ts_query_capture_name_for_id(query, std::get<2>(vpC[i]), &l2);
                            const uint32_t b = std::get<0>(fullC[i]);
                            const TSNode fn = ts_node_descendant_for_byte_range(ts_tree_root_node(tree), b, b + 1);
                            const TSNode vn = ts_node_descendant_for_byte_range(ts_tree_root_node(vp2), b, b + 1);
                            std::printf("  first mismatch @%u: full(%u,%u,c%u=%s '%s') vs vp(%u,%u,c%u=%s '%s')\n",
                                        b, std::get<0>(fullC[i]), std::get<1>(fullC[i]),
                                        uint32_t{std::get<2>(fullC[i])}, n1, ts_node_type(fn),
                                        std::get<0>(vpC[i]), std::get<1>(vpC[i]),
                                        uint32_t{std::get<2>(vpC[i])}, n2, ts_node_type(vn));
                            break;
                        }
                }
                ts_tree_delete(vp);
                ts_tree_delete(vp2);
                ts_tree_delete(vp3);
            } else {
                std::printf("set_included_ranges failed\n");
            }
            TSRange full{ TSPoint{0, 0}, TSPoint{0, 0}, 0, uint32_t(buffer.charCount()) * 2 };
            ts_parser_set_included_ranges(parser, &full, 1);
        }
    }

    ts_tree_delete(tree);
    ts_query_cursor_delete(cursor);
    ts_query_delete(query);
    ts_parser_delete(parser);
    return 0;
}
