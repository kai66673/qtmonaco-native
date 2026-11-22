#pragma once

#include <QString>
#include <QVector>

namespace qmn::diff {

// Word-level inline diff for ONE pair of aligned lines — the M10b follow-
// on to the line engine (PLAN.md GATE 3 / car B machine B1). The line
// engine decides WHICH lines pair up; this one decides which WORDS inside
// a replace-hunk pair changed, so the view can wash only the changed
// tokens (Monaco's inline diff look).
//
// Same Myers family as LineDiff (the kernel is the shared internal seam,
// diff/MyersCore.hpp), run over the pair's TOKEN sequences instead of
// lines: common token prefix + suffix trimmed, greedy D-loop under an
// edit budget, budget exhaustion degrades the pair to whole-line spans
// (VALID, just coarse — the line engine's fallback discipline verbatim).
//
// Token grammar (tripwired in tst_diff):
//  - a maximal run of letter/digit/'_' (Unicode-aware) is one token;
//  - a maximal run of whitespace (QChar::isSpace) is one token;
//  - every other CODE POINT is its own token (surrogate pairs ride
//    together: this layer paints, so it keeps pair boundaries the core
//    deliberately does not care about);
//  - every code unit of the line belongs to exactly one token, so token
//    edges tile the whole line with no gaps.
//
// Result invariants (tripwired in tst_diff):
//  - both span lists are strictly increasing and non-overlapping, and
//    every span has start < end inside [0, line size);
//  - a span boundary sits on a token edge, EXCEPT where the absorb pass
//    merged two spans whose gap is whitespace-only (the gap chars are
//    inside the merged span — presentation only, `changedTokens` still
//    counts the real token edits);
//  - spans never split a surrogate pair (boundaries are token edges);
//  - changedTokens == (token count of a) + (of b) - 2 * LCS when
//    budgetExhausted == false (checked against a naive LCS DP over the
//    token sequences at small scale);
//  - budget exhaustion yields exactly one whole-line span per non-empty
//    side (and changedTokens degenerates to ta.size() + tb.size()).
//
// This file must depend on QtCore ONLY. No QtGui/QtWidgets.
class WordDiff {
public:
    struct Token {
        int start = 0; // [start, end) UTF-16 code units, line-local
        int end = 0;
    };

    struct Span {
        int start = 0; // [start, end) UTF-16 code units, line-local
        int end = 0;
    };

    struct Result {
        QVector<Span> originalSpans; // deleted-token spans (ORIGINAL line coords)
        QVector<Span> modifiedSpans; // inserted-token spans (MODIFIED line coords)
        int changedTokens = 0;       // token edit distance (deletions + insertions)
        bool budgetExhausted = false;
    };

    // editBudget: max token edits the search may use; 0 means "the
    // trimmed token regions must already match", -1 means the auto cap
    // kAutoEditBudget (the O(D^2) trace guard, ~1 MiB transient per pair).
    static Result compute(const QString& original,
                          const QString& modified,
                          int editBudget = -1);

    // Trace memory guard: the stored D-paths cost (D+1)^2 ints; a single
    // PAIR rarely needs more than a few dozen edits, so 256 bounds the
    // worst pair at ~256 KiB of trace.
    static constexpr int kAutoEditBudget = 256;

    // The tokenization seam: exposed (not just internal) because the
    // tripwires re-derive token edges straight from the line text.
    static QVector<Token> tokenize(const QString& line);
};

} // namespace qmn::diff
