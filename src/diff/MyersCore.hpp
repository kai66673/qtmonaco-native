#pragma once

// INTERNAL seam shared by LineDiff and WordDiff (car B machine B1,
// PLAN.md GATE 3 / M10b): the Myers greedy O(ND) D-loop, the stored
// furthest-reaching trace and the backward reconstruction, lifted verbatim
// out of LineDiff.cpp's anonymous namespace (the tst_diff suite pinned the
// behavior before and after the lift). Never a public door, never
// installed — the same seam policy as the layer's own headers.
//
// Search variant: the greedy D-loop keeps the full path trace, i.e.
// O(D^2) memory (D = edit distance), NOT the linear-space middle snake.
// Diff-view / diff-pair budgets are small; the per-caller auto caps guard
// the trace (LineDiff::kAutoEditBudget, WordDiff::kAutoEditBudget).
//
// This file must depend on QtCore ONLY. No QtGui/QtWidgets.

#include <QVector>

#include <algorithm>

namespace qmn::diff {
namespace detail {

// One step of a reconstructed edit path (absolute sequence indices).
struct Op {
    enum Kind { Equal, Delete, Insert };
    Kind kind;
    int aIndex; // set for Equal/Delete, -1 for Insert
    int bIndex; // set for Equal/Insert, -1 for Delete
};

// Myers' greedy O(ND) search over interned id sequences. trace[d] holds
// the furthest-reaching x of every visited diagonal k of parity d
// (index k + d). Returns the edit distance, or -1 when maxEdit was
// exhausted without reaching (n, m).
inline int myersSearch(const QVector<int>& ia, const QVector<int>& ib,
                       int maxEdit, QVector<QVector<int>>& trace)
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
// (Equal/Delete/Insert ops with absolute indices = inner index +
// aBase/bBase) to `ops`. Assumes myersSearch returned D >= 0.
inline void reconstructOps(const QVector<QVector<int>>& trace, int D, int n, int m,
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

} // namespace detail
} // namespace qmn::diff
