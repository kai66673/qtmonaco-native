#include "diff/LineDiff.hpp"

#include <QHash>

#include <algorithm>

namespace qmn::diff {

namespace {

// One step of the reconstructed edit path (absolute line numbers).
struct Op {
    enum Kind { Equal, Delete, Insert };
    Kind kind;
    int originalLine; // set for Equal/Delete, -1 for Insert
    int modifiedLine; // set for Equal/Insert, -1 for Delete
};

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

// Myers' greedy O(ND) search over the interned regions. trace[d] holds
// the furthest-reaching x of every visited diagonal k of parity d
// (index k + d). Returns the edit distance, or -1 when maxEdit was
// exhausted without reaching (n, m).
int myersSearch(const QVector<int>& ia, const QVector<int>& ib, int maxEdit,
                QVector<QVector<int>>& trace)
{
    const int n = ia.size();
    const int m = ib.size();
    const int maxD = qMin(n + m, maxEdit);
    trace.resize(maxD + 1);
    for (int d = 0; d <= maxD; ++d) {
        QVector<int> v(2 * d + 1, -1);
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (d == 0) {
                x = 0;
            } else if (k == -d
                       || (k != d
                           && trace[d - 1][k - 1 + (d - 1)] < trace[d - 1][k + 1 + (d - 1)])) {
                x = trace[d - 1][k + 1 + (d - 1)];     // came from above: an insertion
            } else {
                x = trace[d - 1][k - 1 + (d - 1)] + 1; // came from the left: a deletion
            }
            int y = x - k;
            while (x < n && y < m && ia[x] == ib[y]) {
                ++x;
                ++y;
            }
            v[k + d] = x;
            if (x >= n && y >= m) {
                trace[d] = std::move(v);
                return d;
            }
        }
        trace[d] = std::move(v);
    }
    return -1;
}

// Walks the stored trace back from (n, m) and appends the edit path
// (Equal/Delete/Insert ops with absolute line numbers = inner index +
// aBase/bBase) to `ops`. Assumes myersSearch returned D >= 0.
void reconstructOps(const QVector<QVector<int>>& trace, int D, int n, int m,
                    int aBase, int bBase, QVector<Op>& ops)
{
    QVector<Op> rev;
    rev.reserve(n + m);
    int x = n;
    int y = m;
    for (int d = D; d >= 1; --d) {
        const QVector<int>& prev = trace[d - 1];
        const int k = x - y;
        int prevK;
        if (k == -d || (k != d && prev[k - 1 + (d - 1)] < prev[k + 1 + (d - 1)]))
            prevK = k + 1;
        else
            prevK = k - 1;
        const int prevX = prev[prevK + (d - 1)];
        const int prevY = prevX - prevK;
        while (x > prevX && y > prevY) {
            --x;
            --y;
            rev.append({Op::Equal, x + aBase, y + bBase});
        }
        if (x == prevX) {
            rev.append({Op::Insert, -1, prevY + bBase});
            y = prevY;
        } else {
            rev.append({Op::Delete, prevX + aBase, -1});
            x = prevX;
        }
    }
    // The d == 0 path is one diagonal snake from the origin.
    while (x > 0 && y > 0) {
        --x;
        --y;
        rev.append({Op::Equal, x + aBase, y + bBase});
    }
    std::reverse(rev.begin(), rev.end());
    // APPEND, not assign: `ops` already carries the trimmed prefix's
    // Equal ops and gets the suffix appended by the caller.
    ops.append(std::move(rev));
}

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

    QVector<Op> ops;
    ops.reserve(n + m);
    for (int i = 0; i < prefix; ++i)
        ops.append({Op::Equal, i, i});

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
    const int D = myersSearch(ia, ib, maxEdit, trace);
    if (D >= 0) {
        reconstructOps(trace, D, innerN, innerM, aLo, bLo, ops);
    } else {
        // Budget exhausted: the region degrades into one replace block.
        // Still a valid (just coarser) alignment of both documents.
        result.budgetExhausted = true;
        for (int i = aLo; i < aHi; ++i)
            ops.append({Op::Delete, i, -1});
        for (int j = bLo; j < bHi; ++j)
            ops.append({Op::Insert, -1, j});
    }

    for (int i = 0; i < suffix; ++i)
        ops.append({Op::Equal, n - suffix + i, m - suffix + i});

    // Group the path into maximal change blocks: hunks + aligned rows.
    result.rows.reserve(n + m);
    int curA = 0; // next unconsumed original line
    int curB = 0; // next unconsumed modified line
    int i = 0;
    while (i < ops.size()) {
        if (ops.at(i).kind == Op::Equal) {
            result.rows.append({ops.at(i).originalLine, ops.at(i).modifiedLine});
            curA = ops.at(i).originalLine + 1;
            curB = ops.at(i).modifiedLine + 1;
            ++i;
            continue;
        }
        Hunk hunk;
        hunk.originalLine = curA;
        hunk.modifiedLine = curB;
        int delCount = 0;
        int insCount = 0;
        while (i < ops.size() && ops.at(i).kind != Op::Equal) {
            if (ops.at(i).kind == Op::Delete)
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
