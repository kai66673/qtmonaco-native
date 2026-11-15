#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace qmn::diff {

// Pure line diff for the M10 diff view (PLAN.md GATE 2 / M10).
//
// Miller-Myers family: Myers' O(ND) edit-graph search over interned
// (exact-string-equality) line ids, wrapped in the Miller-style
// heuristics: common prefix + suffix are trimmed up front, and the
// search runs under an edit budget. When the budget (or the auto cap)
// is exhausted, the remaining region falls back to one replace block,
// which keeps the result VALID, just coarser.
//
// Search variant: the greedy D-loop keeps the full furthest-reaching
// path trace, i.e. O(D^2) memory (D = edit distance), NOT the
// linear-space middle snake. Diff-view budgets are small; M10b may
// revisit.
//
// Result invariants (tripwired in tst_diff):
//  - the rows project onto the FULL original line sequence when -1
//    cells are skipped, and onto the full modified sequence;
//  - hunks are maximal change blocks, non-overlapping, increasing;
//    a pure insert/delete hunk has the unchanged side's count 0 and
//    its line number marks the insertion point;
//  - every aligned equal row outside a hunk pairs lines with equal
//    content;
//  - when budgetExhausted == false the edit count is minimal (checked
//    against a naive LCS DP at small scale in the tests).
//
// This file must depend on QtCore ONLY. No QtGui/QtWidgets.
class LineDiff {
public:
    struct Hunk {
        int originalLine = 0;  // first original line; insertion point for a pure insert
        int originalCount = 0; // deletions, 0 for a pure insert
        int modifiedLine = 0;
        int modifiedCount = 0; // insertions, 0 for a pure delete
    };

    // One row of the alignment table: one visual slot of the diff view.
    // -1 means a blank cell on that side.
    struct Row {
        int originalLine = -1;
        int modifiedLine = -1;
    };

    struct Result {
        QVector<Hunk> hunks;           // maximal change blocks, increasing
        QVector<Row> rows;             // alignment table, one entry per visual slot
        bool budgetExhausted = false;  // a budget/cap replace fallback happened
    };

    // editBudget: max edits (insertions + deletions) the search may use;
    // 0 means "the trimmed regions must already match", -1 means the
    // auto budget kAutoEditBudget (the O(D^2) trace guard).
    static Result compute(const QStringList& original,
                          const QStringList& modified,
                          int editBudget = -1);

    // Trace memory guard for editBudget < 0: the stored D-paths cost
    // (D+1)^2 ints, so 2048 edits is ~16 MiB transient per compute.
    static constexpr int kAutoEditBudget = 2048;
};

} // namespace qmn::diff
