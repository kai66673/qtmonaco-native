#pragma once

// The fuzzy scorer (GATE 4 car I2, PLAN): the subsequence matcher behind
// the IDE's quick-open (Ctrl+P) and command palette (Ctrl+Shift+P). The
// promotion law paid at I2 opening: the scorer earns TWO users the very
// car it is written for (quick-open + the palette), and it is born
// headless and pure — so it starts as a library, the same exception
// `src/search/` rode (core/diff class: QtCore ONLY, own guiless ctest
// entry, benchmarkable, fuzzable). A THIRD consumer note for the record:
// the plan's "search-boost" (ranking the Find-in-Files RESULTS by path
// relevance) is NOT shipped at I2 — when it lands, this is its third
// user and the promotion already covered it.
//
// THE LAW (frozen; tst_fuzzy pins it against a BRUTE-FORCE reference
// that re-implements these rules independently — the two-mechanisms-
// agree pattern, the wholeWord face's sibling):
//
//   The needle must appear in the haystack as a CASE-INSENSITIVE
//   subsequence (each pair compared through QChar::toLower on both
//   sides — simple case folding, the documented seam: full case folding
//   and multi-char expansions, e.g. 'ß'/'SS', are NOT part of the
//   class — a consumer needing them raises it to the ledger).
//
//   score = Σ per matched character of:
//     +4 if the character sits at the START of the haystack, or
//        directly after a BOUNDARY: one of " \t/\\_-:.~" (path/file
//        separators and punctuation seams) OR a camelCase step
//        (directly after a LOWERCASE char onto an UPPERCASE one);
//     +1 otherwise (the base hit);
//     PLUS +2 when the match is CONSECUTIVE (the previous needle
//     character matched the immediately previous haystack position).
//
//   The reported score is the MAXIMUM TOTAL SCORE over all embeddings,
//   where an embedding's total is Σ per matched character of (the +4/+1
//   context above) + Σ per ADJACENT PAIR of +2 when that pair is
//   consecutive (the previous needle character matched the immediately
//   previous haystack position). A tie between max-score embeddings
//   resolves to the LEXICALLY EARLIEST position vector — the law,
//   pinned from BOTH sides: the DP enforces the maximum by optimal
//   substructure and the reported vector by the greedy left-to-right
//   walk whose per-step test is "the smallest position that can still
//   COMPLETE to max" (a step of the lex-earliest max embedding passes
//   that test by definition, so the walk lands exactly on it); the
//   brute-force reference walks the embeddings in LEX ORDER and keeps
//   only STRICT improvements. Adjacency is a PAIR bonus inside one
//   embedding — it is NOT a per-occurrence property: the +2 shows in
//   the reported total exactly when the reported lex-earliest-max
//   vector carries the consecutive pair (tst_fuzzy::
//   adjacencyIsPairBonusNotOccurrenceProperty sits AT the record). The
//   corpus tripwires the tie class directly.
//
//   An EMPTY needle matches (matched=true, score 0, positions empty —
//   the caller shows its unfiltered list); a needle LONGER than the
//   haystack never matches (the subsequence fact is decided by length).
//   No match ⇒ matched=false, score 0, positions empty.
//
// Offsets are the repo-wide law again: 0-based UTF-16 code-unit
// INDICES into the haystack.

#include <QVector>

namespace qmn::fuzzy {

struct FuzzyMatch {
    bool matched = false;
    int score = 0;
    QVector<int> positions; // ascending haystack indices, size == needle
};

// The one pure face. DP over (needle, haystack) — O(m*n) time and space;
// the reconstruction is the greedy-completion walk of the tie law above.
FuzzyMatch match(const QString& needle, const QString& haystack);

} // namespace qmn::fuzzy
