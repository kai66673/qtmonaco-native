#include "fuzzy/FuzzyMatcher.hpp"

#include <QChar>
#include <climits>

namespace qmn::fuzzy {

namespace {

// The BOUNDARY set, frozen by the header law: the seams where path and
// identifier words start. (':','~' ride for file-ish spellings — the law
// documents the SET, not a class reference.)
bool isBoundaryChar(QChar c)
{
    switch (c.unicode()) {
    case u' ':
    case u'\t':
    case u'/':
    case u'\\':
    case u'_':
    case u'-':
    case u'.':
    case u':':
    case u'~':
        return true;
    default:
        return false;
    }
}

// The per-character context score (start/boundary/camelCase +4, else +1
// — the adjacency +2 rides SEPARATELY at the transition, because it
// depends on the PAIR of positions, not on the position alone).
int contextScore(const QString& hay, int j)
{
    if (j == 0)
        return 4;
    const QChar prev = hay.at(j - 1);
    if (isBoundaryChar(prev))
        return 4;
    if (prev.isLower() && hay.at(j).isUpper()) // camelCase step
        return 4;
    return 1;
}

bool charEqFolded(QChar a, QChar b)
{
    return a.toLower() == b.toLower();
}

// A floor no real partial embedding can reach: every guard below refuses
// to add to it, so it never propagates into a real cell.
constexpr int kNope = INT_MIN / 4;

} // namespace

FuzzyMatch match(const QString& needle, const QString& haystack)
{
    FuzzyMatch out;
    const int m = needle.size();
    const int n = haystack.size();
    if (m == 0) {
        out.matched = true; // the empty needle matches everything (the
                            // caller shows its unfiltered list)
        return out;
    }
    if (m > n)
        return out; // the subsequence fact is decided by LENGTH

    // tail[i][j]: the best score of an embedding of needle[i..m-1] with
    // needle[i] AT j (its own ctx + the suffix's adjacency only). The
    // recursion splits the NEXT position j' into the consecutive branch
    // (j'==j+1, +2) and the rest (suffix-max of tail[i+1] from j+2).
    // Suffix maxima are precomputed per row, so the whole table is O(m*n).
    QVector<QVector<int>> tail(m, QVector<int>(n, kNope));
    QVector<QVector<int>> sufMax(m, QVector<int>(n + 1, kNope)); // sufMax[i][j] = max of tail[i][j..n-1]
    for (int i = m - 1; i >= 0; --i) {
        // sufMax[i][n] stays kNope — the identity of the max chain.
        for (int j = n - 1; j >= 0; --j) {
            int bestHere = kNope;
            if (i < m - 1) {
                int rest = sufMax[i + 1][j + 2 <= n ? j + 2 : n]; // max of the
                                                                  // non-consecutive tail (kNope = none)
                if (j + 1 < n && tail[i + 1][j + 1] != kNope)
                    rest = qMax(rest, tail[i + 1][j + 1] + 2); // consecutive branch
                if (rest != kNope && charEqFolded(needle.at(i), haystack.at(j)))
                    bestHere = contextScore(haystack, j) + rest;
            } else if (charEqFolded(needle.at(i), haystack.at(j))) {
                bestHere = contextScore(haystack, j);
            }
            tail[i][j] = bestHere;
            // max chain with kNope as identity: a real tail value wins over
            // the identity; only when BOTH are kNope does the cell stay kNope.
            if (sufMax[i][j + 1] == kNope)
                sufMax[i][j] = bestHere;
            else
                sufMax[i][j] = qMax(bestHere, sufMax[i][j + 1]);
        }
    }

    const int best = sufMax[0][0];
    if (best == kNope)
        return out; // no embedding at all

    // The tie law's walk (header): LEXICALLY EARLIEST position vector
    // among the max-score embeddings. p_i is the SMALLEST position above
    // p_{i-1} whose (fact-carried prefix score + transition) + tail[i][p_i]
    // still lands on max. INVARIANT: prefixOf is the score of the chosen
    // prefix chain ITSELF, and a max embedding exists with that exact
    // prefix (holds at step 0; preserved because the completing embedding
    // tail[i][p_i] extends strictly above p_i). Each step's j-set is a
    // superset of the completions of every max embedding carrying the
    // chosen prefix — and the lex-earliest max embedding passes every
    // step by definition — so the walk lands exactly on the lex-earliest.
    // (The tempting shortcut — deciding each step from PREFIX-OPTIMAL
    // prefix scores — is FALSE: a max embedding's prefix need not be
    // prefix-optimal, e.g. "ab" in "aab" where position 0 at the start
    // (4) sits in the lex-earliest max embedding while the prefix-optimal
    // 'a' at position 1 (1) cannot complete to max.)
    out.matched = true;
    out.score = best;
    int prev = -1;
    int prefixOf = 0; // the chosen prefix's OWN score
    for (int i = 0; i < m; ++i) {
        for (int j = prev + 1; j < n; ++j) {
            if (tail[i][j] == kNope)
                continue;
            // STEP 0 has no transition at all (the first matched char has
            // no predecessor — adjacency belongs to needle PAIRS only).
            const int adj = (i > 0 && j == prev + 1) ? 2 : 0;
            if (prefixOf + adj + tail[i][j] == best) {
                prefixOf += contextScore(haystack, j) + adj;
                out.positions.append(j);
                prev = j;
                break;
            }
        }
        if (prev <= (i > 0 ? out.positions.at(i - 1) : -1)) {
            // UNREACHABLE by the invariant (step i-1's completion embeds
            // needle[i] at some passing j). The honest empty face beats a
            // wrong path — defensive, never expected.
            out = FuzzyMatch();
            return out;
        }
    }
    return out;
}

} // namespace qmn::fuzzy
