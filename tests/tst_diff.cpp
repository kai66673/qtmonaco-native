// Property + golden tests for the M10 line-diff engine (PLAN.md GATE 2).
// Modelled on tst_textbuffer: fixed seeds, deterministic, checked against
// naive references (row projections, LCS DP). QtCore only, guiless.

#include "diff/LineDiff.hpp"

#include <QtTest>

using qmn::diff::LineDiff;

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
};

QTEST_GUILESS_MAIN(TestLineDiff)
#include "tst_diff.moc"
