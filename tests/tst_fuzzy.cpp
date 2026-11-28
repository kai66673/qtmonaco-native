// The 10th ctest entry (GATE 4 car I2, PLAN): the fuzzy scorer library —
// the law at src/fuzzy/FuzzyMatcher.hpp, pinned from BOTH mechanisms:
// hand-computed faces + a BRUTE-FORCE reference that re-implements the
// rules independently and ENUMERATES every embedding in LEX ORDER
// (keeping only STRICT improvements — the two-mechanisms-agree pattern,
// the naive-reference pair of tst_search/tst_textbuffer; the agreement
// on the tie class is exactly what makes the lex-earliest law real).
// GUITLESS like textbuffer/diff/search — the layer is QtCore-only.
#include "fuzzy/FuzzyMatcher.hpp"

#include <QTest>
#include <QString>
#include <QVector>

#include <random>

using qmn::fuzzy::FuzzyMatch;
using qmn::fuzzy::match;

namespace {

// ---- the independent reference: rules re-typed from the header law,
// implementation shared with the DP NONE (separate boundary table
// written as a QSet-free switch? No — a plain QString::contains over the
// same documented SET is the law's own words; the SET is the law, the
// MECHANISMS (DP vs full enumeration) are what the pin compares). ----

bool refBoundary(QChar c)
{
    static const QString seps = QStringLiteral(" \t/\\_-:.~");
    return seps.contains(c);
}

int refContext(const QString& hay, int j)
{
    if (j == 0)
        return 4;
    const QChar prev = hay.at(j - 1);
    if (refBoundary(prev))
        return 4;
    if (prev.isLower() && hay.at(j).isUpper())
        return 4;
    return 1;
}

bool refEqFolded(QChar a, QChar b)
{
    return a.toLower() == b.toLower();
}

// The total score of ONE embedding — the pair-bonus reading straight
// from the header.
int refTotal(const QString& hay, const QVector<int>& p)
{
    int s = 0;
    for (int i = 0; i < p.size(); ++i) {
        s += refContext(hay, p[i]);
        if (i > 0 && p[i] == p[i - 1] + 1)
            s += 2;
    }
    return s;
}

struct RefResult {
    bool matched = false;
    int score = 0;
    QVector<int> positions; // lex-earliest among the max
    int maxEmbeddings = 0;  // how many embeddings share the max total
                            // (>=2 = a tie-class leg fired)
};

// FULL enumeration in lex order; keep only on STRICT improvement — the
// enumeration order plus the strict keep IS the tie law's other side.
RefResult refSolve(const QString& needle, const QString& hay)
{
    RefResult r;
    const int m = needle.size();
    const int n = hay.size();
    if (m == 0) {
        r.matched = true;
        return r;
    }
    if (m > n)
        return r;
    QVector<int> pos;
    // recursive lex-order walk
    struct Rec {
        const QString* needle; const QString* hay; int m; int n;
        QVector<int>* pos; RefResult* out;
        void rec(int i, int from)
        {
            if (i == m) {
                const int s = refTotal(*hay, *pos);
                if (!out->matched || s > out->score) {
                    out->matched = true;
                    out->score = s;
                    out->maxEmbeddings = 1;
                    out->positions = *pos;
                } else if (s == out->score) {
                    ++out->maxEmbeddings; // the tie stays visible; the
                                          // kept vector stays the
                                          // lex-earliest (strict keep)
                }
                return;
            }
            for (int j = from; j <= n - (m - 1 - i); ++j) {
                if (!refEqFolded(needle->at(i), hay->at(j)))
                    continue;
                pos->append(j);
                rec(i + 1, j + 1);
                pos->removeLast();
            }
        }
    };
    Rec rec{ &needle, &hay, m, n, &pos, &r };
    rec.rec(0, 0);
    if (!r.matched)
        r.score = 0;
    return r;
}

QString fmt(const QVector<int>& v)
{
    QString s;
    for (int x : v) {
        if (!s.isEmpty())
            s += ',';
        s += QString::number(x);
    }
    return s;
}

} // namespace

class TestFuzzy : public QObject
{
    Q_OBJECT

private slots:

    void emptyNeedleLaws()
    {
        const auto m1 = match(QString(), QStringLiteral("abc"));
        QVERIFY(m1.matched);
        QCOMPARE(m1.score, 0);
        QVERIFY(m1.positions.isEmpty());
        const auto m2 = match(QString(), QString());
        QVERIFY(m2.matched);
        QCOMPARE(m2.score, 0);
        QVERIFY(m2.positions.isEmpty());
        // a NON-empty needle into an empty haystack is decided by length.
        const auto m3 = match(QStringLiteral("a"), QString());
        QVERIFY(!m3.matched);
    }

    void needleLongerThanHaystackNeverMatches()
    {
        // even when every character present would fold-equal: the
        // subsequence fact is decided by LENGTH, not by characters.
        const auto m = match(QStringLiteral("aaa"), QStringLiteral("aa"));
        QVERIFY(!m.matched);
        QCOMPARE(m.score, 0);
        QVERIFY(m.positions.isEmpty());
    }

    void noMatchLeavesTheRecordClean()
    {
        const auto m = match(QStringLiteral("xyz"), QStringLiteral("foo/bar.cpp"));
        QVERIFY(!m.matched);
        QCOMPARE(m.score, 0);
        QVERIFY(m.positions.isEmpty());
    }

    void startAndBoundaryContext()
    {
        // START +4, and each documented BOUNDARY seam +4. Hand-computed
        // from the header law (the pair-bonus reading).
        auto m = match(QStringLiteral("fo"), QStringLiteral("foo/bar"));
        // "fo" at [0,1]: 4 + (1 + 2 adjacency) = 7 (the [0,2] embedding
        // scores 5; [.., o@2 without adjacency is worse).
        QVERIFY(m.matched);
        QCOMPARE(m.score, 7);
        QCOMPARE(m.positions, (QVector<int>{ 0, 1 }));

        m = match(QStringLiteral("fb"), QStringLiteral("foo/bar"));
        // 'f'@0 (4) + 'b'@4 after '/' (4) = 8.
        QVERIFY(m.matched);
        QCOMPARE(m.score, 8);
        QCOMPARE(m.positions, (QVector<int>{ 0, 4 }));

        m = match(QStringLiteral("m"), QStringLiteral("src\\main.cpp"));
        QCOMPARE(m.score, 4); // the '\\' seam
        QCOMPARE(m.positions, (QVector<int>{ 4 }));

        m = match(QStringLiteral("b"), QStringLiteral("a\tb"));
        QCOMPARE(m.score, 4); // the TAB seam
        QCOMPARE(m.positions, (QVector<int>{ 2 }));

        for (const QChar seam : { QChar(u'-'), QChar(u'_'), QChar(u'.'),
                                  QChar(u':'), QChar(u'~') }) {
            const auto mm = match(QStringLiteral("b"),
                                  QStringLiteral("a") + seam + QStringLiteral("b"));
            QVERIFY2(mm.score == 4, qPrintable(QString("seam %1 scored %2")
                                                   .arg(seam.unicode()).arg(mm.score)));
            QCOMPARE(mm.positions, (QVector<int>{ 2 }));
        }
    }

    void camelCaseStepContext()
    {
        auto m = match(QStringLiteral("fb"), QStringLiteral("fooBar"));
        // 'f'@0 (4) + 'B'@3 (camel step: lower onto upper) = 8.
        QVERIFY(m.matched);
        QCOMPARE(m.score, 8);
        QCOMPARE(m.positions, (QVector<int>{ 0, 3 }));

        // The UPPER→lower step is NOT a camel seam: 'c' after 'B' is a
        // base +1 — adjacency +2 rides because the pair is consecutive.
        m = match(QStringLiteral("bc"), QStringLiteral("fooBc"));
        QVERIFY(m.matched);
        QCOMPARE(m.score, 7); // 4 + (1 + 2)
        QCOMPARE(m.positions, (QVector<int>{ 3, 4 }));
    }

    void adjacencyIsPairBonusNotOccurrenceProperty()
    {
        // "aB" in "AaB": the CONSECUTIVE occurrence at 1..2 scores 1+2+4=7,
        // but the max embedding is [0,2] = 4 + 4 (camel seam) = 8 — and
        // its pair is NOT consecutive, so the reported total carries NO
        // adjacency at all. The bonus belongs to the reported embedding,
        // it is not an occurrence property. (The reverse face — adjacency
        // shown when the reported vector IS consecutive — sits in
        // startAndBoundaryContext's "fo" leg.)
        const auto m = match(QStringLiteral("ab"), QStringLiteral("AaB"));
        QVERIFY(m.matched);
        QCOMPARE(m.score, 8);
        QCOMPARE(m.positions, (QVector<int>{ 0, 2 }));
    }

    void lexEarliestTieAmongEqualTotals()
    {
        // "aa" in "a..a.a": embeddings [0,3], [0,5], [3,5] all total 8
        // (4+4, 4+4, 4+4 — the 3/5 pair is NOT consecutive) — a real
        // three-way tie. The reported vector is the LEX-earliest [0,3].
        const auto m = match(QStringLiteral("aa"), QStringLiteral("a..a.a"));
        QVERIFY(m.matched);
        QCOMPARE(m.score, 8);
        QCOMPARE(m.positions, (QVector<int>{ 0, 3 }));

        // length-1 tie class: "a" in "a.a" — positions 0 and 2 both score
        // 4 (start; '.' seam): [0] wins.
        const auto m2 = match(QStringLiteral("a"), QStringLiteral("a.a"));
        QCOMPARE(m2.score, 4);
        QCOMPARE(m2.positions, (QVector<int>{ 0 }));
    }

    void caseFoldSeamSsNotFolded()
    {
        // The documented seam: SIMPLE folding via toLower both sides.
        // 'ß' never folds to "ss"/"SS", in either direction.
        QVERIFY(!match(QStringLiteral("\xC3\x9F"), QStringLiteral("SS")).matched);
        QVERIFY(!match(QStringLiteral("SS"), QStringLiteral("\xC3\x9F")).matched);
        // Plain case-insensitivity rides the same seam the other way:
        // "SS" folds onto "ss" and matches.
        const auto m = match(QStringLiteral("SS"), QStringLiteral("ss"));
        QVERIFY(m.matched);
        QCOMPARE(m.score, 7); // 4 + (1 + 2)
        QCOMPARE(m.positions, (QVector<int>{ 0, 1 }));
    }

    void astralCodeUnitIndices()
    {
        // The UTF-16 code-unit law (the tree-sitter-2x trap's sibling
        // class): the astral char occupies TWO units.
        const QString hay = QStringLiteral("😀ab"); // 😀 = U+1F600
        QCOMPARE(hay.size(), 4);
        auto m = match(QStringLiteral("b"), hay);
        QVERIFY(m.matched);
        QCOMPARE(m.positions, (QVector<int>{ 3 }));
        m = match(QStringLiteral("a"), hay);
        QVERIFY(m.matched);
        QCOMPARE(m.positions, (QVector<int>{ 2 })); // after the surrogate pair
        // the astral char AS the needle (its two units) matches at 0.
        m = match(QStringLiteral("😀"), hay);
        QVERIFY(m.matched);
        QCOMPARE(m.positions, (QVector<int>{ 0, 1 }));
    }

    void fuzzVsBruteForcePair()
    {
        // The property pair (fixed seed — deterministic, reproducible
        // failure, the textbuffer law): random needle/haystack pairs from
        // an alphabet that mixes case, boundary seams and repeats, the DP
        // vs the full-enumeration reference. Agreement on matched, score
        // AND the exact lex-earliest position vector. Plus the report's
        // SELF-consistency, checked a THIRD time: the reported vector is
        // an ascending case-folded embedding of the needle whose total
        // recomputes to the reported score.
        std::mt19937_64 gen(0xC0FFEEull);
        const QString alpha = QStringLiteral("aAbB.\\_-x~:cC ");
        int pairs = 0, ties = 0;
        for (int t = 0; t < 40000; ++t) {
            const int nlen = 1 + int(gen() % 6u);           // 1..6
            const int hlen = int(gen() % 11u);              // 0..10
            QString needle, hay;
            for (int i = 0; i < nlen; ++i)
                needle.append(alpha.at(int(gen() % quint64(alpha.size()))));
            for (int i = 0; i < hlen; ++i)
                hay.append(alpha.at(int(gen() % quint64(alpha.size()))));
            const FuzzyMatch got = match(needle, hay);
            const RefResult ref = refSolve(needle, hay);
            ++pairs;
            if (ref.matched && ref.maxEmbeddings >= 2)
                ++ties; // a tie-class leg fired: the two mechanisms had
                        // to agree on WHICH vector of several max ones
                        // wins — the equality pin below is the promise
            if (got.matched != ref.matched) {
                QFAIL(qPrintable(QString("pair %1: n=%2 h=%3 matched dp=%d ref=%d")
                                     .arg(t).arg(needle, hay)
                                     .arg(int(got.matched)).arg(int(ref.matched))));
            }
            if (ref.matched) {
                if (got.score != ref.score) {
                    QFAIL(qPrintable(QString("pair %1: n=%2 h=%3 score dp=%d ref=%d")
                                         .arg(t).arg(needle, hay)
                                         .arg(got.score).arg(ref.score)));
                }
                if (got.positions != ref.positions) {
                    QFAIL(qPrintable(QString("pair %1: n=%2 h=%3 pos dp=[%4] ref=[%5]")
                                         .arg(t).arg(needle, hay)
                                         .arg(fmt(got.positions), fmt(ref.positions))));
                }
                // self-consistency, third mechanism: recompute the total
                // of the REPORTED vector straight from the header law.
                QVERIFY2(got.positions.size() == needle.size(),
                         qPrintable(QString("pair %1: pos size %2 != needle %3")
                                        .arg(t).arg(got.positions.size()).arg(needle.size())));
                int prev = -1;
                for (int i = 0; i < got.positions.size(); ++i) {
                    const int j = got.positions[i];
                    QVERIFY2(j > prev, qPrintable(QString("pair %1: positions not ascending").arg(t)));
                    QVERIFY2(refEqFolded(needle.at(i), hay.at(j)),
                             qPrintable(QString("pair %1: pos %2 does not embed the needle").arg(t)));
                    prev = j;
                }
                QVERIFY2(got.score == refTotal(hay, got.positions),
                         qPrintable(QString("pair %1: score %2 != total of reported positions %3")
                                        .arg(t).arg(got.score).arg(refTotal(hay, got.positions))));
            } else {
                QCOMPARE(got.score, 0);
                QVERIFY2(got.positions.isEmpty(),
                         qPrintable(QString("pair %1: non-matched carries positions [%2]")
                                        .arg(t).arg(fmt(got.positions))));
            }
        }
        QVERIFY2(ties > 0, "the corpus never fired a tie-class leg — the pair is vacuous");
        qInfo("fuzz pair: %d pairs, %d tie-class legs", pairs, ties);
    }
};

QTEST_GUILESS_MAIN(TestFuzzy)
#include "tst_fuzzy.moc"
