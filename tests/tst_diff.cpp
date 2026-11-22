// Property + golden tests for the M10 line-diff engine (PLAN.md GATE 2).
// Modelled on tst_textbuffer: fixed seeds, deterministic, checked against
// naive references (row projections, LCS DP). QtCore only, guiless.

#include "diff/LineDiff.hpp"
#include "diff/WordDiff.hpp"

#include <QtTest>

using qmn::diff::LineDiff;
using qmn::diff::WordDiff;

namespace {

quint64 rngNext(quint64& s) // xorshift64, fixed seeds -> reproducible
{
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

QStringList makeRandomDoc(int lines, int alphabetSize, quint64& seed)
{
    QStringList doc;
    doc.reserve(lines);
    for (int i = 0; i < lines; ++i)
        doc << QStringLiteral("w%1").arg(rngNext(seed) % alphabetSize);
    return doc;
}

QStringList mutateDoc(const QStringList& doc, int edits, quint64& seed)
{
    QStringList out = doc;
    for (int e = 0; e < edits; ++e) {
        const quint64 roll = rngNext(seed);
        if (out.isEmpty() || roll % 3 == 0) { // insert
            const int pos = static_cast<int>(roll % (out.size() + 1));
            out.insert(pos, QStringLiteral("i%1").arg(e));
        } else if (roll % 3 == 1) { // delete
            const int pos = static_cast<int>(roll % out.size());
            out.removeAt(pos);
        } else { // replace
            const int pos = static_cast<int>(roll % out.size());
            out[pos] = QStringLiteral("r%1").arg(e);
        }
    }
    return out;
}

// Naive LCS length: the reference for edit minimality (small docs only).
int naiveLcsLength(const QStringList& a, const QStringList& b)
{
    QVector<int> prev(b.size() + 1, 0);
    QVector<int> cur(b.size() + 1, 0);
    for (int i = 1; i <= a.size(); ++i) {
        for (int j = 1; j <= b.size(); ++j) {
            if (a.at(i - 1) == b.at(j - 1))
                cur[j] = prev[j - 1] + 1;
            else
                cur[j] = qMax(prev[j], cur[j - 1]);
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// --- car B machine B1 (PLAN §GATE 3 / car B): the word-engine helpers -----
// The token-level oracles: re-tokenize a line into its token strings,
// build/mutate token PIECE lists (joined to make the lines), naive token
// LCS, and the geometry that must hold for EVERY word result: strictly
// increasing disjoint non-empty spans inside the line, every span edge
// sitting on a token edge (the absorb pass only ever widens a span
// across whitespace, which lands on token edges too), no two spans
// separated by a whitespace-only gap (that IS the absorb pass: maximal
// modulo whitespace), and the token tiling covering the line exactly.

QStringList tokenStringsOf(const QString& line)
{
    QStringList out;
    for (const WordDiff::Token& t : WordDiff::tokenize(line))
        out << line.mid(t.start, t.end - t.start);
    return out;
}

QString joinPieces(const QStringList& pieces)
{
    QString s;
    for (const QString& p : pieces)
        s += p;
    return s;
}

// Word/separator alternating piece lists; the join tokenizes exactly to
// the pieces while classes alternate (mutations may merge neighbors into
// one token — harmless: the oracles re-tokenize the JOINED line, never
// the piece list).
QStringList wordPieces(int words, int alphabet, quint64& seed)
{
    QStringList out;
    for (int i = 0; i < words; ++i) {
        if (i > 0) {
            switch (rngNext(seed) % 4) {
            case 0: out << QLatin1String(" "); break;
            case 1: out << QLatin1String("  "); break;
            case 2: out << QLatin1String(","); break;
            default: out << QLatin1String("\t"); break;
            }
        }
        out << QStringLiteral("w%1").arg(int(rngNext(seed) % alphabet));
    }
    return out;
}

QStringList mutatePieces(const QStringList& pieces, int edits, quint64& seed)
{
    QStringList out = pieces;
    for (int e = 0; e < edits; ++e) {
        const quint64 roll = rngNext(seed);
        switch (roll % 3) {
        case 0:
            out.insert(int(roll % quint64(out.size() + 1)),
                       QStringLiteral("z%1").arg(int(rngNext(seed) % 9)));
            break;
        case 1:
            if (!out.isEmpty())
                out.removeAt(int(roll % quint64(out.size())));
            break;
        default:
            if (!out.isEmpty())
                out[int(roll % quint64(out.size()))] =
                    QStringLiteral("z%1").arg(int(rngNext(seed) % 9));
            break;
        }
    }
    return out;
}

// The token grammar's char classes, re-derived independently of the
// engine (class 0 = any other single code point):
int charClassOf(const QChar c)
{
    if (c.isSpace())
        return 1;
    if (c == QLatin1Char('_') || c.isLetterOrNumber())
        return 2;
    return 0;
}

// Token-level invariants: tiling covers the line exactly, every token is
// homogeneous (one class, or one code point / one surrogate pair), and
// adjacent tokens never share a word/space class (the grammar never
// misses a merge opportunity).
void verifyTokenGrammar(const QString& line)
{
    const QByteArray ctx = "line=" + line.toUtf8() + ' ';
    const auto toks = WordDiff::tokenize(line);
    int pos = 0;
    const QChar nul;
    QChar prevLast = nul;
    for (const WordDiff::Token& t : toks) {
        QVERIFY2(t.start == pos, ctx.constData());
        QVERIFY2(t.end > t.start, ctx.constData());
        const int len = t.end - t.start;
        const int cls = charClassOf(line.at(t.start));
        if (cls == 0) {
            QVERIFY2(len == 1 || (len == 2 && line.at(t.start).isHighSurrogate()
                                      && line.at(t.start + 1).isLowSurrogate()),
                     ctx.constData());
        } else {
            for (int i = t.start; i < t.end; ++i)
                QVERIFY2(charClassOf(line.at(i)) == cls, ctx.constData());
        }
        QVERIFY2(!(cls != 0 && charClassOf(prevLast) == cls),
                 (ctx + "adjacent same-class tokens").constData());
        prevLast = line.at(t.end - 1);
        pos = t.end;
    }
    QVERIFY2(pos == int(line.size()), ctx.constData());
}

// The naive token LCS distance, the changedTokens oracle at small
// scale (bounded piece lists keep the LCS cells under 65k, so the
// row type can be a narrow quint16 — the memo-overflow anti-pattern).
int naiveWordLcsDistance(const QStringList& a, const QStringList& b)
{
    const int n = a.size();
    const int m = b.size();
    if (n == 0 || m == 0)
        return n + m;
    Q_ASSERT(n <= 255 && m <= 255); // keeps quint16 provably safe here
    QVector<quint16> prev(m + 1, 0);
    QVector<quint16> cur(m + 1, 0);
    for (int i = 1; i <= n; ++i) {
        cur[0] = 0;
        for (int j = 1; j <= m; ++j) {
            if (a.at(i - 1) == b.at(j - 1))
                cur[j] = quint16(prev[j - 1] + 1);
            else
                cur[j] = prev[j] > cur[j - 1] ? prev[j] : cur[j - 1];
        }
        std::swap(prev, cur);
    }
    return n + m - 2 * int(prev[m]);
}

void verifyWordSpans(const QString& line, const QVector<WordDiff::Span>& spans,
                     const char* where)
{
    const QByteArray ctx = QByteArray(where) + " line=" + line.toUtf8() + ' ';

    QSet<int> edges;
    int pos = 0;
    for (const WordDiff::Token& t : WordDiff::tokenize(line)) {
        QVERIFY2(t.start == pos, ctx.constData());
        edges.insert(t.end);
        pos = t.end;
    }
    edges.insert(0);
    QVERIFY2(pos == int(line.size()), ctx.constData());

    int prevEnd = -1;
    for (const WordDiff::Span& s : spans) {
        QVERIFY2(s.start >= 0 && s.end <= int(line.size()), ctx.constData());
        QVERIFY2(s.start < s.end, ctx.constData());
        QVERIFY2(s.start > prevEnd, ctx.constData());
        QVERIFY2(edges.contains(s.start) && edges.contains(s.end),
                 (ctx + "span edge off a token edge").constData());
        if (prevEnd >= 0) { // maximality modulo whitespace: the gap must
            bool gapAllSpace = true; // carry a non-space token, else absorb
            for (int i = prevEnd; i < s.start; ++i) { // was lazy and it
                if (!line.at(i).isSpace()) {          // failed hard
                    gapAllSpace = false;
                    break;
                }
            }
            QVERIFY2(!gapAllSpace, (ctx + "whitespace-only span gap").constData());
        }
        prevEnd = s.end;
    }
}

QByteArray describe(const QStringList& a, const QStringList& b, const LineDiff::Result& r)
{
    QByteArray out;
    out += "a=" + a.join('|').toUtf8() + " b=" + b.join('|').toUtf8() + "\n";
    for (int i = 0; i < r.rows.size(); ++i)
        out += QStringLiteral("row %1: %2/%3\n")
                   .arg(i)
                   .arg(r.rows.at(i).originalLine)
                   .arg(r.rows.at(i).modifiedLine)
                   .toUtf8();
    for (const auto& h : r.hunks)
        out += QStringLiteral("hunk %1+%2 / %3+%4\n")
                   .arg(h.originalLine)
                   .arg(h.originalCount)
                   .arg(h.modifiedLine)
                   .arg(h.modifiedCount)
                   .toUtf8();
    return out;
}

// The invariants that must hold for EVERY result, budgeted or not:
// rows project onto both full line sequences, hunks are ordered,
// in-bounds and non-overlapping, and every row outside the hunks
// pairs lines with equal content.
void verifyCommonInvariants(const QStringList& a, const QStringList& b,
                            const LineDiff::Result& r, const char* where)
{
    const QByteArray ctx = describe(a, b, r);

    int expectA = 0;
    int expectB = 0;
    for (const auto& row : r.rows) {
        QVERIFY2(row.originalLine != -1 || row.modifiedLine != -1, ctx.constData());
        if (row.originalLine != -1) {
            QVERIFY2(row.originalLine == expectA, ctx.constData());
            ++expectA;
        }
        if (row.modifiedLine != -1) {
            QVERIFY2(row.modifiedLine == expectB, ctx.constData());
            ++expectB;
        }
    }
    QVERIFY2(expectA == a.size(), ctx.constData());
    QVERIFY2(expectB == b.size(), ctx.constData());

    int prevAEnd = 0;
    int prevBEnd = 0;
    for (const auto& h : r.hunks) {
        QVERIFY2(h.originalCount >= 0 && h.modifiedCount >= 0, ctx.constData());
        QVERIFY2(h.originalCount > 0 || h.modifiedCount > 0, ctx.constData());
        QVERIFY2(h.originalLine >= prevAEnd, ctx.constData());
        QVERIFY2(h.originalLine + h.originalCount <= a.size(), ctx.constData());
        QVERIFY2(h.modifiedLine >= prevBEnd, ctx.constData());
        QVERIFY2(h.modifiedLine + h.modifiedCount <= b.size(), ctx.constData());
        prevAEnd = h.originalLine + h.originalCount;
        prevBEnd = h.modifiedLine + h.modifiedCount;
    }

    // Interleave check: rows are maximal runs of content-equal aligned
    // pairs alternating with the hunk blocks; a hunk's rows must match
    // its record cell-for-cell (side-by-side pairing, blanks past the
    // shorter side). This is the exact hunks<->rows consistency guard.
    int rc = 0;
    for (const auto& h : r.hunks) {
        const int aEnd = h.originalLine + h.originalCount;
        const int bEnd = h.modifiedLine + h.modifiedCount;
        auto belongs = [h, aEnd, bEnd](const LineDiff::Row& row) {
            if (row.originalLine == -1 || row.modifiedLine == -1)
                return true;
            return (row.originalLine >= h.originalLine && row.originalLine < aEnd)
                || (row.modifiedLine >= h.modifiedLine && row.modifiedLine < bEnd);
        };
        while (rc < r.rows.size() && !belongs(r.rows.at(rc))) {
            const auto& row = r.rows.at(rc);
            QVERIFY2(a.at(row.originalLine) == b.at(row.modifiedLine), ctx.constData());
            ++rc;
        }
        const int span = qMax(h.originalCount, h.modifiedCount);
        QVERIFY2(rc + span <= r.rows.size(), ctx.constData());
        for (int t = 0; t < span; ++t, ++rc) {
            const LineDiff::Row expected {
                h.originalCount > t ? h.originalLine + t : -1,
                h.modifiedCount > t ? h.modifiedLine + t : -1
            };
            QVERIFY2(r.rows.at(rc).originalLine == expected.originalLine
                     && r.rows.at(rc).modifiedLine == expected.modifiedLine,
                     ctx.constData());
        }
    }
    while (rc < r.rows.size()) {
        const auto& row = r.rows.at(rc);
        QVERIFY2(row.originalLine != -1 && row.modifiedLine != -1, ctx.constData());
        QVERIFY2(a.at(row.originalLine) == b.at(row.modifiedLine), ctx.constData());
        ++rc;
    }
    Q_UNUSED(where);
}

int totalEdits(const LineDiff::Result& r)
{
    int edits = 0;
    for (const auto& h : r.hunks)
        edits += h.originalCount + h.modifiedCount;
    return edits;
}

} // namespace

class TestLineDiff : public QObject {
    Q_OBJECT

private slots:
    void identicalDocuments()
    {
        const QStringList doc { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") };
        const auto r = LineDiff::compute(doc, doc);
        QVERIFY(!r.budgetExhausted);
        QVERIFY(r.hunks.isEmpty());
        QCOMPARE(r.rows.size(), 3);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(r.rows.at(i).originalLine, i);
            QCOMPARE(r.rows.at(i).modifiedLine, i);
        }
        verifyCommonInvariants(doc, doc, r, Q_FUNC_INFO);
    }

    void emptyAndSingleSided()
    {
        const QStringList empty;
        const QStringList one { QStringLiteral("a"), QStringLiteral("b") };

        auto r = LineDiff::compute(empty, empty);
        QVERIFY(r.hunks.isEmpty());
        QVERIFY(r.rows.isEmpty());

        r = LineDiff::compute(empty, one);
        QCOMPARE(r.hunks.size(), 1);
        QCOMPARE(r.hunks.at(0).originalLine, 0);
        QCOMPARE(r.hunks.at(0).originalCount, 0);
        QCOMPARE(r.hunks.at(0).modifiedLine, 0);
        QCOMPARE(r.hunks.at(0).modifiedCount, 2);
        verifyCommonInvariants(empty, one, r, Q_FUNC_INFO);

        r = LineDiff::compute(one, empty);
        QCOMPARE(r.hunks.size(), 1);
        QCOMPARE(r.hunks.at(0).originalCount, 2);
        QCOMPARE(r.hunks.at(0).modifiedCount, 0);
        verifyCommonInvariants(one, empty, r, Q_FUNC_INFO);
    }

    void pureInsertion()
    {
        const QStringList a { QStringLiteral("A"), QStringLiteral("B") };
        const QStringList b { QStringLiteral("A"), QStringLiteral("X"), QStringLiteral("B") };
        const auto r = LineDiff::compute(a, b);
        QCOMPARE(r.hunks.size(), 1);
        QCOMPARE(r.hunks.at(0).originalLine, 1);
        QCOMPARE(r.hunks.at(0).originalCount, 0); // insertion point
        QCOMPARE(r.hunks.at(0).modifiedLine, 1);
        QCOMPARE(r.hunks.at(0).modifiedCount, 1);
        QCOMPARE(r.rows.size(), 3);
        QCOMPARE(r.rows.at(1).originalLine, -1); // blank row on the original side
        QCOMPARE(r.rows.at(1).modifiedLine, 1);
        verifyCommonInvariants(a, b, r, Q_FUNC_INFO);
    }

    void pureDeletion()
    {
        const QStringList a { QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C") };
        const QStringList b { QStringLiteral("A"), QStringLiteral("C") };
        const auto r = LineDiff::compute(a, b);
        QCOMPARE(r.hunks.size(), 1);
        QCOMPARE(r.hunks.at(0).originalLine, 1);
        QCOMPARE(r.hunks.at(0).originalCount, 1);
        QCOMPARE(r.hunks.at(0).modifiedLine, 1); // deletion point
        QCOMPARE(r.hunks.at(0).modifiedCount, 0);
        QCOMPARE(r.rows.at(1).originalLine, 1);
        QCOMPARE(r.rows.at(1).modifiedLine, -1);
        verifyCommonInvariants(a, b, r, Q_FUNC_INFO);
    }

    void replaceBlock()
    {
        const QStringList a { QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C") };
        const QStringList b { QStringLiteral("A"), QStringLiteral("X"), QStringLiteral("C") };
        const auto r = LineDiff::compute(a, b);
        QCOMPARE(r.hunks.size(), 1);
        QCOMPARE(r.hunks.at(0).originalLine, 1);
        QCOMPARE(r.hunks.at(0).originalCount, 1);
        QCOMPARE(r.hunks.at(0).modifiedLine, 1);
        QCOMPARE(r.hunks.at(0).modifiedCount, 1);
        // The replacement pairs side-by-side, no blanks at equal counts.
        QCOMPARE(r.rows.size(), 3);
        QCOMPARE(r.rows.at(1).originalLine, 1);
        QCOMPARE(r.rows.at(1).modifiedLine, 1);
        verifyCommonInvariants(a, b, r, Q_FUNC_INFO);
    }

    void moveBlock()
    {
        // "a b c" -> "c a b": unique optimal is insert-c-top + delete-c-bottom.
        const QStringList a { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") };
        const QStringList b { QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("b") };
        const auto r = LineDiff::compute(a, b);
        QCOMPARE(totalEdits(r), 2);
        QCOMPARE(r.hunks.size(), 2);
        QCOMPARE(r.hunks.at(0).originalCount, 0);
        QCOMPARE(r.hunks.at(0).modifiedCount, 1);
        QCOMPARE(r.hunks.at(1).originalLine, 2);
        QCOMPARE(r.hunks.at(1).originalCount, 1);
        QCOMPARE(r.hunks.at(1).modifiedCount, 0);
        QCOMPARE(r.rows.size(), 4);
        verifyCommonInvariants(a, b, r, Q_FUNC_INFO);
    }

    void trimKeepsMiddleHunk()
    {
        QStringList a;
        QStringList b;
        for (int i = 0; i < 50; ++i) {
            a << QStringLiteral("L%1").arg(i);
            b << QStringLiteral("L%1").arg(i);
        }
        a[25] = QStringLiteral("changed");
        const auto r = LineDiff::compute(a, b);
        QCOMPARE(r.hunks.size(), 1);
        QCOMPARE(r.hunks.at(0).originalLine, 25);
        QCOMPARE(r.hunks.at(0).modifiedLine, 25);
        QCOMPARE(r.rows.size(), 50);
        verifyCommonInvariants(a, b, r, Q_FUNC_INFO);
    }

    void repeatedLines()
    {
        const QStringList a { QStringLiteral("a"), QStringLiteral("b"),
                             QStringLiteral("b"), QStringLiteral("c") };
        const QStringList b { QStringLiteral("a"), QStringLiteral("c") };
        const auto r = LineDiff::compute(a, b);
        QCOMPARE(totalEdits(r), 2);
        verifyCommonInvariants(a, b, r, Q_FUNC_INFO);
    }

    void swapEdges()
    {
        // Two equally optimal alignments exist; assert invariants and
        // minimality, not the exact path (the engine is deterministic,
        // the choice between optima is not part of the contract).
        const QStringList a { QStringLiteral("x"), QStringLiteral("y") };
        const QStringList b { QStringLiteral("y"), QStringLiteral("x") };
        const auto r = LineDiff::compute(a, b);
        QCOMPARE(totalEdits(r), 2);
        verifyCommonInvariants(a, b, r, Q_FUNC_INFO);
    }

    void insertionAtEnd()
    {
        const QStringList a { QStringLiteral("a"), QStringLiteral("b") };
        const QStringList b { QStringLiteral("a"), QStringLiteral("b"),
                             QStringLiteral("b"), QStringLiteral("b"), QStringLiteral("b") };
        const auto r = LineDiff::compute(a, b);
        QCOMPARE(r.hunks.size(), 1);
        QCOMPARE(r.hunks.at(0).originalLine, 2);
        QCOMPARE(r.hunks.at(0).originalCount, 0);
        QCOMPARE(r.hunks.at(0).modifiedCount, 3);
        verifyCommonInvariants(a, b, r, Q_FUNC_INFO);
    }

    void randomProjections()
    {
        // 300 deterministic pairs (tiny alphabets on purpose -> lots of
        // duplicate lines, the classic diff torture case).
        quint64 seed = 0xBEEFC0DE;
        for (int caseNo = 0; caseNo < 300; ++caseNo) {
            const int alphabet = 2 + int(rngNext(seed) % 4);
            const int lines = int(rngNext(seed) % 41);
            const QStringList a = makeRandomDoc(lines, alphabet, seed);
            QStringList b;
            if (rngNext(seed) % 4 == 0)
                b = makeRandomDoc(int(rngNext(seed) % 41), alphabet, seed);
            else
                b = mutateDoc(a, int(rngNext(seed) % 9), seed);
            const auto r = LineDiff::compute(a, b);
            QVERIFY2(!r.budgetExhausted, describe(a, b, r).constData());
            verifyCommonInvariants(a, b, r, "randomProjections");
        }
    }

    void minimalityVsNaiveLcs()
    {
        quint64 seed = 0xC0FFEE;
        for (int caseNo = 0; caseNo < 120; ++caseNo) {
            const int alphabet = 2 + int(rngNext(seed) % 5);
            const int lines = 1 + int(rngNext(seed) % 24);
            const QStringList a = makeRandomDoc(lines, alphabet, seed);
            const QStringList b = mutateDoc(a, int(rngNext(seed) % 7), seed);
            const auto r = LineDiff::compute(a, b);
            QVERIFY2(!r.budgetExhausted, describe(a, b, r).constData());
            const int optimal = a.size() + b.size() - 2 * naiveLcsLength(a, b);
            QVERIFY2(totalEdits(r) == optimal,
                     (describe(a, b, r) + QByteArray("expected edits ")
                      + QByteArray::number(optimal)).constData());
        }
    }

    void budgetFallback()
    {
        quint64 seed = 0xD1FF30;
        // ~40 real edits but a budget of 5: the search must give up and
        // fall back to replace blocks - result stays fully valid.
        const QStringList a = makeRandomDoc(200, 50, seed);
        const QStringList b = mutateDoc(a, 40, seed);
        const auto r = LineDiff::compute(a, b, 5);
        QVERIFY(r.budgetExhausted);
        verifyCommonInvariants(a, b, r, Q_FUNC_INFO);
    }

    void budgetZeroRequiresExactMatch()
    {
        const QStringList same { QStringLiteral("a"), QStringLiteral("b") };
        QVERIFY(!LineDiff::compute(same, same, 0).budgetExhausted);

        QStringList different = same;
        different[1] = QStringLiteral("z");
        const auto r = LineDiff::compute(same, different, 0);
        QVERIFY(r.budgetExhausted);
        verifyCommonInvariants(same, different, r, Q_FUNC_INFO);
    }

    void autoBudgetGuardOnHugeChurn()
    {
        // Unbudgeted, but two essentially unrelated 3000-line documents
        // exceed kAutoEditBudget's trace guard: fallback must kick in
        // instead of blowing up memory, with the result still valid.
        quint64 seed = 0x515151;
        const QStringList a = makeRandomDoc(3000, 5000, seed);
        const QStringList b = makeRandomDoc(3000, 5000, seed);
        const auto r = LineDiff::compute(a, b);
        QVERIFY(r.budgetExhausted);
        verifyCommonInvariants(a, b, r, Q_FUNC_INFO);
    }

    void perfTripwire()
    {
        // 20k-line doc, 45 scattered edits: the expected Myers run is a
        // handful of milliseconds; the 250 ms budget keeps ~10x margin.
        QStringList a;
        a.reserve(20'000);
        for (int i = 0; i < 20'000; ++i)
            a << QStringLiteral("line %1 content %2").arg(i).arg(i % 997);
        QStringList b = a;
        for (int i = 100; i < 20'000; i += 1000)
            b[i] = QStringLiteral("EDITED %1").arg(i);
        b.removeAt(19'500);
        b.insert(500, QStringLiteral("brand new line"));

        QElapsedTimer timer;
        timer.start();
        const auto r = LineDiff::compute(a, b);
        const qint64 elapsed = timer.elapsed();

        QVERIFY2(!r.budgetExhausted, "20k-line doc with 45 edits must not need the fallback");
        QCOMPARE(totalEdits(r), 42); // 20 edits + 1 delete + 1 insert
        verifyCommonInvariants(a, b, r, Q_FUNC_INFO);
        QVERIFY2(elapsed < 250, qPrintable(QStringLiteral("diff took %1 ms").arg(elapsed)));
    }
// --- car B machine B1: the word-diff engine (PLAN §GATE 3 / car B) -------

    void wordTokenizationTilesTheLine()
    {
        const QStringList samples = {
            QString(), QStringLiteral("hello world"), QStringLiteral("a,b  c\td"),
            QStringLiteral("   "), QStringLiteral("x_1 42 + ="),
            QStringLiteral("\U0001F600x"), QStringLiteral("a\U0001F600b"),
        };
        for (const QString& line : samples)
            verifyTokenGrammar(line);

        // Surrogate pairs are ONE token (the view-painting discipline this
        // layer keeps; a split here would paint half a grapheme).
        const auto emo = WordDiff::tokenize(QStringLiteral("\U0001F600x"));
        QCOMPARE(emo.size(), 2);
        QCOMPARE(emo.at(0).start, 0);
        QCOMPARE(emo.at(0).end, 2);
        QCOMPARE(emo.at(1).start, 2);
        QCOMPARE(emo.at(1).end, 3);
        const auto mixed = WordDiff::tokenize(QStringLiteral("a\U0001F600b"));
        QCOMPARE(mixed.size(), 3);
        QCOMPARE(mixed.at(0).end, 1);
        QCOMPARE(mixed.at(1).end, 3);
        QCOMPARE(mixed.at(2).end, 4);
    }

    void wordSpansSitOnTokenEdgesAndStayOrdered()
    {
        quint64 seed = 0xC0FFEE;
        for (int iter = 0; iter < 300; ++iter) {
            const QStringList pieces = wordPieces(1 + int(rngNext(seed) % 13), 7, seed);
            const QStringList mod = mutatePieces(pieces, 1 + int(rngNext(seed) % 3), seed);
            const QString a = joinPieces(pieces);
            const QString b = joinPieces(mod);
            const auto r = WordDiff::compute(a, b);
            verifyWordSpans(a, r.originalSpans, Q_FUNC_INFO);
            verifyWordSpans(b, r.modifiedSpans, Q_FUNC_INFO);
        }
    }

    void wordChangedTokenCountMatchesNaiveLcs()
    {
        quint64 seed = 0xBEEF;
        for (int iter = 0; iter < 150; ++iter) {
            const QStringList pieces = wordPieces(1 + int(rngNext(seed) % 12), 5, seed);
            const QStringList mod = mutatePieces(pieces, 1 + int(rngNext(seed) % 4), seed);
            const QString a = joinPieces(pieces);
            const QString b = joinPieces(mod);
            const auto r = WordDiff::compute(a, b);
            QVERIFY2(!r.budgetExhausted, Q_FUNC_INFO); // small pairs, far from 256
            QCOMPARE(r.changedTokens,
                     naiveWordLcsDistance(tokenStringsOf(a), tokenStringsOf(b)));
        }
    }

    void wordSpansAbsorbWhitespaceOnlyGaps()
    {
        // Two words changed across a plain space: ONE whole-line wash per
        // side (the absorb pass; the edit COUNT stays 4 tokens).
        auto r = WordDiff::compute(QStringLiteral("a b"), QStringLiteral("X Y"));
        QCOMPARE(r.changedTokens, 4);
        QCOMPARE(r.originalSpans.size(), 1);
        QCOMPARE(r.originalSpans.at(0).start, 0);
        QCOMPARE(r.originalSpans.at(0).end, 3);
        QCOMPARE(r.modifiedSpans.size(), 1);
        QCOMPARE(r.modifiedSpans.at(0).start, 0);
        QCOMPARE(r.modifiedSpans.at(0).end, 3);

        // An unchanged NON-space token between the changes forbids the
        // merge (only whitespace gaps absorb).
        r = WordDiff::compute(QStringLiteral("a q b"), QStringLiteral("X q Y"));
        QCOMPARE(r.changedTokens, 4);
        QCOMPARE(r.originalSpans.size(), 2);
        QCOMPARE(r.originalSpans.at(0).start, 0);
        QCOMPARE(r.originalSpans.at(0).end, 1);
        QCOMPARE(r.originalSpans.at(1).start, 4);
        QCOMPARE(r.originalSpans.at(1).end, 5);
        QCOMPARE(r.modifiedSpans.size(), 2);
        QCOMPARE(r.modifiedSpans.at(1).start, 4);
        QCOMPARE(r.modifiedSpans.at(1).end, 5);

        // One word changed: the span sits exactly on the token.
        r = WordDiff::compute(QStringLiteral("foo  bar"), QStringLiteral("foo  baz"));
        QCOMPARE(r.changedTokens, 2);
        QCOMPARE(r.originalSpans.size(), 1);
        QCOMPARE(r.originalSpans.at(0).start, 5);
        QCOMPARE(r.originalSpans.at(0).end, 8);
        QCOMPARE(r.modifiedSpans.size(), 1);
        QCOMPARE(r.modifiedSpans.at(0).start, 5);
        QCOMPARE(r.modifiedSpans.at(0).end, 8);

        // A wider (double-space) absorb gap: still one whole-line wash.
        r = WordDiff::compute(QStringLiteral("a  b"), QStringLiteral("X  Y"));
        QCOMPARE(r.changedTokens, 4);
        QCOMPARE(r.originalSpans.size(), 1);
        QCOMPARE(r.originalSpans.at(0).end, 4);
        QCOMPARE(r.modifiedSpans.size(), 1);
        QCOMPARE(r.modifiedSpans.at(0).end, 4);
    }

    void wordDiffBudgetFallbackCoversWholeLines()
    {
        // The pair the auto cap trips on (602 token edits > 256): whole-
        // line spans on both non-empty sides + the coarse changedTokens
        // (every token of both sides counted) — the line engine's valid-
        // but-coarser fallback discipline, verbatim.
        const QString small = QStringLiteral("l16 EDIT");
        QStringList ys;
        for (int i = 0; i <= 300; ++i)
            ys << QStringLiteral("Y%1").arg(i);
        const QString heavy = ys.join(QLatin1Char(' ')) + QLatin1Char(' ') + small;
        const auto r = WordDiff::compute(heavy, small);
        QVERIFY2(r.budgetExhausted, Q_FUNC_INFO);
        QCOMPARE(r.originalSpans.size(), 1);
        QCOMPARE(r.originalSpans.at(0).start, 0);
        QCOMPARE(r.originalSpans.at(0).end, heavy.size());
        QCOMPARE(r.modifiedSpans.size(), 1);
        QCOMPARE(r.modifiedSpans.at(0).start, 0);
        QCOMPARE(r.modifiedSpans.at(0).end, small.size());
        QCOMPARE(r.changedTokens,
                 WordDiff::tokenize(heavy).size() + WordDiff::tokenize(small).size());

        // An explicit budget above the cap lets the SAME pair through:
        // one delete-block span over exactly the Y run (the small line is
        // a token SUFFIX of the heavy one), no modified-side span, and
        // the minimal token edit count.
        const auto r2 = WordDiff::compute(heavy, small, 1000);
        QVERIFY2(!r2.budgetExhausted, Q_FUNC_INFO);
        QVERIFY2(r2.modifiedSpans.isEmpty(), Q_FUNC_INFO);
        QCOMPARE(r2.originalSpans.size(), 1);
        QCOMPARE(r2.originalSpans.at(0).start, 0);
        QCOMPARE(r2.originalSpans.at(0).end, heavy.indexOf(small));
        QCOMPARE(r2.changedTokens,
                 WordDiff::tokenize(heavy).size() - WordDiff::tokenize(small).size());

        // Budget 0: any difference in the trimmed-in region collapses the
        // pair to whole lines...
        const auto r3 = WordDiff::compute(heavy, small, 0);
        QVERIFY2(r3.budgetExhausted, Q_FUNC_INFO);
        QCOMPARE(r3.originalSpans.size(), 1);
        QCOMPARE(r3.modifiedSpans.size(), 1);

        // ...while a fully-trimmed equal pair stays silent at ANY budget.
        const auto r4 = WordDiff::compute(heavy, heavy, 0);
        QVERIFY2(!r4.budgetExhausted, Q_FUNC_INFO);
        QVERIFY2(r4.changedTokens == 0, Q_FUNC_INFO);
        QVERIFY2(r4.originalSpans.isEmpty() && r4.modifiedSpans.isEmpty(), Q_FUNC_INFO);

        // Equal-and-empty pairs are silent at the auto budget too:
        // identical lines produce no spans at all...
        const auto r5 = WordDiff::compute(QStringLiteral("x y z"),
                                          QStringLiteral("x y z"));
        QVERIFY2(r5.changedTokens == 0 && r5.originalSpans.isEmpty()
                     && r5.modifiedSpans.isEmpty() && !r5.budgetExhausted, Q_FUNC_INFO);
        // ...and a whole-line vs empty pair is one block span exactly as
        // large as the non-empty side's token extent (the whole line).
        const auto r6 = WordDiff::compute(QStringLiteral(""),
                                          QStringLiteral("a b"));
        QVERIFY2(r6.originalSpans.isEmpty(), Q_FUNC_INFO);
        QCOMPARE(r6.modifiedSpans.size(), 1);
        QCOMPARE(r6.modifiedSpans.at(0).start, 0);
        QCOMPARE(r6.modifiedSpans.at(0).end, 3);
        QCOMPARE(r6.changedTokens, 3);
        const auto r7 = WordDiff::compute(QStringLiteral("a b"),
                                          QStringLiteral(""));
        QVERIFY2(r7.modifiedSpans.isEmpty(), Q_FUNC_INFO);
        QCOMPARE(r7.originalSpans.size(), 1);
        QCOMPARE(r7.originalSpans.at(0).end, 3);
        QCOMPARE(r7.changedTokens, 3);
    }

    void wordDiffInlinePassSkipsExhaustedPairs()
    {
        // The pair-level story the view's inline pass (B2) rides, run
        // engine-side inside ONE line-alignment frame: replace-hunk pairs
        // whose token D exceeds kAutoEditBudget come back
        // budgetExhausted while the others stay exact — the view can
        // therefore skip exactly the coarse rows and wash the rest. (The
        // LINE engine's far larger auto budget does NOT trip on this
        // document: only the word engine's per-pair cap does.)
        QStringList a;
        for (int i = 0; i < 40; ++i)
            a << QStringLiteral("l%1 EDIT").arg(i);
        QStringList b = a;
        for (int i = 13; i <= 15; ++i)
            b[i] = QStringLiteral("l%1 EDIT2").arg(i); // light: 2 token edits
        QStringList ys;
        for (int i = 0; i <= 300; ++i)
            ys << QStringLiteral("Y%1").arg(i);
        const QString yRun = ys.join(QLatin1Char(' '));
        for (int i = 16; i <= 24; ++i)
            b[i] = yRun + QLatin1Char(' ') + a[i]; // heavy: 602 token edits

        const auto frame = LineDiff::compute(a, b);
        QVERIFY2(!frame.budgetExhausted, Q_FUNC_INFO); // the line engine is fine
        QCOMPARE(frame.rows.size(), 40);

        // The inline walk over the frame's rows, exactly as B2 will run it.
        QVector<WordDiff::Span> modifiedSpans;
        QVector<WordDiff::Span> originalSpans;
        QElapsedTimer timer;
        timer.start();
        for (const auto& row : frame.rows) {
            if (row.originalLine < 0 || row.modifiedLine < 0)
                continue; // a blank cell never pairs
            if (a.at(row.originalLine) == b.at(row.modifiedLine))
                continue; // an aligned equal row needs no inline detail
            const auto r = WordDiff::compute(a.at(row.originalLine),
                                             b.at(row.modifiedLine));
            if (r.budgetExhausted)
                continue; // the row stays bare-band: coarse but valid
            modifiedSpans << r.modifiedSpans;
            originalSpans << r.originalSpans;
        }
        const qint64 elapsed = timer.elapsed();
        QVERIFY2(elapsed < 800, qPrintable(QStringLiteral("inline storm took %1 ms").arg(elapsed)));
        QCOMPARE(modifiedSpans.size(), 3); // the three light pairs survived
        QCOMPARE(originalSpans.size(), 3);
        QCOMPARE(modifiedSpans.at(0).start, 4); // "l13 EDIT2": the EDIT token
        QCOMPARE(modifiedSpans.at(0).end, b.at(13).size());
        QCOMPARE(originalSpans.at(0).start, 4);
        QCOMPARE(originalSpans.at(0).end, a.at(13).size());
    }

    void wordDiffCapSurvivesBudgetExhaustedPairs()
    {
        // A storm of cap-tripping pairs: the D-loop work stays bounded by
        // the cap (256^2/2 diagonals per pair, NOT the pair size), every
        // result stays a VALID whole-line geometry, and the storm costs
        // real but bounded time. (The line engine's own budget slot above
        // pins the line-shape version of this story.)
        QStringList base;
        for (int i = 0; i < 160; ++i)
            base << QStringLiteral("t%1").arg(i);
        const QString a = base.join(QLatin1Char(' '));
        QStringList other;
        for (int i = 0; i < 160; ++i)
            other << QStringLiteral("u%1").arg(i);
        const QString b = other.join(QLatin1Char(' '));
        QElapsedTimer timer;
        timer.start();
        for (int iter = 0; iter < 300; ++iter) {
            const auto r = WordDiff::compute(a, b);
            QVERIFY2(r.budgetExhausted, Q_FUNC_INFO);
            QCOMPARE(r.originalSpans.size(), 1);
            QCOMPARE(r.originalSpans.at(0).start, 0);
            QCOMPARE(r.originalSpans.at(0).end, a.size());
            QCOMPARE(r.modifiedSpans.size(), 1);
            QCOMPARE(r.modifiedSpans.at(0).end, b.size());
            QVERIFY2(r.changedTokens == WordDiff::tokenize(a).size()
                                   + WordDiff::tokenize(b).size(), Q_FUNC_INFO);
        }
        const qint64 elapsed = timer.elapsed();
        QVERIFY2(elapsed < 800, qPrintable(QStringLiteral("cap storm took %1 ms").arg(elapsed)));
    }
};

QTEST_GUILESS_MAIN(TestLineDiff)
#include "tst_diff.moc"
