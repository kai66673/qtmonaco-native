#include "diff/WordDiff.hpp"

#include "diff/MyersCore.hpp"

#include <QHash>

namespace qmn::diff {

namespace {

inline bool isWordChar(const QChar c)
{
    return c == QLatin1Char('_') || c.isLetterOrNumber();
}

// Token content equality without allocating: views into the two line
// QStrings, which live unchanged for the whole compute().
inline bool tokenEq(const QString& a, const WordDiff::Token& ta,
                    const QString& b, const WordDiff::Token& tb)
{
    const int len = ta.end - ta.start;
    return len == tb.end - tb.start
        && QStringView(a).mid(ta.start, len) == QStringView(b).mid(tb.start, len);
}

// Interns two token runs into ONE dense id space with EXACT token-string
// equality (mirrors LineDiff's LineInterner verbatim, QString keys —
// tokens are tiny, and a view-keyed table buys nothing worth its
// lifetime foot-gun here).
struct TokenInterner {
    QHash<QString, int> table;
    int nextId = 0;

    QVector<int> ids(const QString& line, const QVector<WordDiff::Token>& toks,
                     int lo, int hi)
    {
        QVector<int> out;
        out.reserve(hi - lo);
        for (int i = lo; i < hi; ++i) {
            const QString t = line.mid(toks[i].start, toks[i].end - toks[i].start);
            auto it = table.constFind(t);
            if (it == table.constEnd()) {
                table.insert(t, nextId);
                out.push_back(nextId++);
            } else {
                out.push_back(it.value());
            }
        }
        return out;
    }
};

// The absorb pass: merge spans on ONE side that touch or are separated
// only by whitespace (a moved word inside a whitespace run reads as one
// wash, not three — presentation only, the token edit count was taken
// before this pass). Spans arrive increasing and disjoint.
void absorbWhitespaceGaps(const QString& line, QVector<WordDiff::Span>& spans)
{
    QVector<WordDiff::Span> out;
    out.reserve(spans.size());
    for (const WordDiff::Span& s : spans) {
        if (!out.isEmpty()) {
            WordDiff::Span& last = out.back();
            bool merge = s.start <= last.end;
            if (!merge) {
                merge = true;
                for (int i = last.end; i < s.start; ++i) {
                    if (!line.at(i).isSpace()) {
                        merge = false;
                        break;
                    }
                }
            }
            if (merge) {
                last.end = qMax(last.end, s.end);
                continue;
            }
        }
        out.push_back(s);
    }
    spans = std::move(out);
}

} // namespace

QVector<WordDiff::Token> WordDiff::tokenize(const QString& line)
{
    QVector<Token> toks;
    const int n = line.size();
    int i = 0;
    while (i < n) {
        const QChar c = line.at(i);
        int j = i + 1;
        if (c.isSpace()) {
            while (j < n && line.at(j).isSpace())
                ++j;
        } else if (isWordChar(c)) {
            while (j < n && isWordChar(line.at(j)))
                ++j;
        } else {
            // A lone code point is a token; a surrogate pair rides
            // together (isLetterOrNumber answers false on each surrogate,
            // so pairs land here — the atomic step keeps every span
            // boundary pair-safe).
            if (c.isHighSurrogate() && j < n && line.at(j).isLowSurrogate())
                ++j;
        }
        toks.push_back({ i, j });
        i = j;
    }
    return toks;
}

WordDiff::Result WordDiff::compute(const QString& original,
                                   const QString& modified,
                                   int editBudget)
{
    Result result;
    const QVector<Token> ta = tokenize(original);
    const QVector<Token> tb = tokenize(modified);

    // Miller-style heuristics on the token sequences: trim the common
    // prefix and suffix first (the matching-first/last-token lemmas keep
    // this optimality-preserving, same as the line engine's).
    int prefix = 0;
    while (prefix < ta.size() && prefix < tb.size()
           && tokenEq(original, ta[prefix], modified, tb[prefix]))
        ++prefix;
    int suffix = 0;
    while (suffix < ta.size() - prefix && suffix < tb.size() - prefix
           && tokenEq(original, ta[ta.size() - 1 - suffix],
                      modified, tb[tb.size() - 1 - suffix]))
        ++suffix;

    const int aLo = prefix;
    const int aHi = int(ta.size()) - suffix;
    const int bLo = prefix;
    const int bHi = int(tb.size()) - suffix;
    const int innerN = aHi - aLo;
    const int innerM = bHi - bLo;

    const int maxEdit = editBudget < 0 ? kAutoEditBudget : editBudget;
    // Two statements on purpose: the interner is shared mutable state,
    // and argument evaluation order in a call is unspecified (the same
    // indeterminately-sequenced trap the line engine records).
    TokenInterner interner;
    const QVector<int> ia = interner.ids(original, ta, aLo, aHi);
    const QVector<int> ib = interner.ids(modified, tb, bLo, bHi);
    QVector<QVector<int>> trace;
    const int D = detail::myersSearch(ia, ib, maxEdit, trace);
    if (D < 0) {
        // Budget exhausted: the pair degrades to whole-line spans. Valid,
        // just coarse — and the coarser changedTokens (every token of
        // both sides) keeps the count honest about what the spans claim.
        result.budgetExhausted = true;
        result.changedTokens = int(ta.size()) + int(tb.size());
        if (!original.isEmpty())
            result.originalSpans.push_back({ 0, int(original.size()) });
        if (!modified.isEmpty())
            result.modifiedSpans.push_back({ 0, int(modified.size()) });
        return result;
    }

    QVector<detail::Op> ops;
    ops.reserve(int(ta.size()) + int(tb.size()));
    for (int i = 0; i < prefix; ++i)
        ops.append({ detail::Op::Equal, i, i });
    detail::reconstructOps(trace, D, innerN, innerM, aLo, bLo, ops);
    for (int i = 0; i < suffix; ++i)
        ops.append({ detail::Op::Equal, int(ta.size()) - suffix + i,
                     int(tb.size()) - suffix + i });

    // Group the path into change blocks (maximal runs of non-Equal ops —
    // a block has NO Equal token inside by construction, so its deleted
    // original tokens and its inserted modified tokens are each one
    // CONSECUTIVE token range; one span per side per block, riding the
    // first token's start and the last token's end).
    int i = 0;
    while (i < ops.size()) {
        if (ops.at(i).kind == detail::Op::Equal) {
            ++i;
            continue;
        }
        int aFirst = -1, aLast = -1; // original token range of the block
        int bFirst = -1, bLast = -1; // modified token range of the block
        while (i < ops.size() && ops.at(i).kind != detail::Op::Equal) {
            if (ops.at(i).kind == detail::Op::Delete) {
                if (aFirst < 0)
                    aFirst = ops.at(i).aIndex;
                aLast = ops.at(i).aIndex;
            } else {
                if (bFirst < 0)
                    bFirst = ops.at(i).bIndex;
                bLast = ops.at(i).bIndex;
            }
            ++result.changedTokens;
            ++i;
        }
        if (aFirst >= 0)
            result.originalSpans.push_back({ ta[aFirst].start, ta[aLast].end });
        if (bFirst >= 0)
            result.modifiedSpans.push_back({ tb[bFirst].start, tb[bLast].end });
    }

    absorbWhitespaceGaps(original, result.originalSpans);
    absorbWhitespaceGaps(modified, result.modifiedSpans);
    return result;
}

} // namespace qmn::diff
