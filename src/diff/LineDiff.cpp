#include "diff/LineDiff.hpp"

#include "diff/MyersCore.hpp"

#include <QHash>

#include <algorithm>

namespace qmn::diff {

namespace {

// Interns two line ranges into ONE dense id space with EXACT string
// equality (QHash only accelerates the lookup; equality decides). The
// shared table is the whole point: the Myers snake test ia[x] == ib[y]
// is meaningless if the two regions carry independent id namespaces.
struct LineInterner {
    QHash<QString, int> table;
    int nextId = 0;

    QVector<int> ids(const QStringList& lines, int lo, int hi)
    {
        QVector<int> out;
        out.reserve(hi - lo);
        for (int i = lo; i < hi; ++i) {
            auto it = table.constFind(lines.at(i));
            if (it == table.constEnd()) {
                table.insert(lines.at(i), nextId);
                out.push_back(nextId++);
            } else {
                out.push_back(it.value());
            }
        }
        return out;
    }
};

} // namespace

LineDiff::Result LineDiff::compute(const QStringList& original,
                                   const QStringList& modified,
                                   int editBudget)
{
    Result result;
    const int n = original.size();
    const int m = modified.size();

    // Miller-style heuristics: trim the common prefix and suffix first;
    // the matching-first/last-line lemmas keep this optimality-preserving.
    int prefix = 0;
    while (prefix < n && prefix < m && original.at(prefix) == modified.at(prefix))
        ++prefix;
    int suffix = 0;
    while (suffix < n - prefix && suffix < m - prefix
           && original.at(n - 1 - suffix) == modified.at(m - 1 - suffix))
        ++suffix;

    QVector<detail::Op> ops;
    ops.reserve(n + m);
    for (int i = 0; i < prefix; ++i)
        ops.append({detail::Op::Equal, i, i});

    const int aLo = prefix;
    const int aHi = n - suffix;
    const int bLo = prefix;
    const int bHi = m - suffix;
    const int innerN = aHi - aLo;
    const int innerM = bHi - bLo;

    const int maxEdit = editBudget < 0 ? kAutoEditBudget : editBudget;
    // Two statements on purpose: the interner is shared mutable state,
    // and argument evaluation order in a call is unspecified (the same
    // indeterminately-sequenced trap as the span-insert note in the view).
    LineInterner interner;
    const QVector<int> ia = interner.ids(original, aLo, aHi);
    const QVector<int> ib = interner.ids(modified, bLo, bHi);
    QVector<QVector<int>> trace;
    const int D = detail::myersSearch(ia, ib, maxEdit, trace);
    if (D >= 0) {
        detail::reconstructOps(trace, D, innerN, innerM, aLo, bLo, ops);
    } else {
        // Budget exhausted: the region degrades into one replace block.
        // Still a valid (just coarser) alignment of both documents.
        result.budgetExhausted = true;
        for (int i = aLo; i < aHi; ++i)
            ops.append({detail::Op::Delete, i, -1});
        for (int j = bLo; j < bHi; ++j)
            ops.append({detail::Op::Insert, -1, j});
    }

    for (int i = 0; i < suffix; ++i)
        ops.append({detail::Op::Equal, n - suffix + i, m - suffix + i});

    // Group the path into maximal change blocks: hunks + aligned rows.
    result.rows.reserve(n + m);
    int curA = 0; // next unconsumed original line
    int curB = 0; // next unconsumed modified line
    int i = 0;
    while (i < ops.size()) {
        if (ops.at(i).kind == detail::Op::Equal) {
            result.rows.append({ops.at(i).aIndex, ops.at(i).bIndex});
            curA = ops.at(i).aIndex + 1;
            curB = ops.at(i).bIndex + 1;
            ++i;
            continue;
        }
        Hunk hunk;
        hunk.originalLine = curA;
        hunk.modifiedLine = curB;
        int delCount = 0;
        int insCount = 0;
        while (i < ops.size() && ops.at(i).kind != detail::Op::Equal) {
            if (ops.at(i).kind == detail::Op::Delete)
                ++delCount;
            else
                ++insCount;
            ++i;
        }
        hunk.originalCount = delCount;
        hunk.modifiedCount = insCount;
        // Deletions/insertions inside a block pair side-by-side (the
        // extra rows of the longer side get a blank cell).
        const int pairCount = qMax(delCount, insCount);
        for (int t = 0; t < pairCount; ++t) {
            result.rows.append({delCount > t ? hunk.originalLine + t : -1,
                                insCount > t ? hunk.modifiedLine + t : -1});
        }
        result.hunks.append(hunk);
        curA += delCount;
        curB += insCount;
    }

    return result;
}

} // namespace qmn::diff
