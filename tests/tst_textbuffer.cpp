// Property tests for the piece-table model: random operation streams are
// compared against a naive QString reference, with treap invariants checked
// after every single operation.

#include <QtTest>

#include <random>

#include "core/TextBuffer.hpp"

using qmn::core::TextBuffer;

class TestTextBuffer : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        buffer = new TextBuffer();
    }
    void cleanup()
    {
        delete buffer;
    }

    void emptyDocument();
    void basicOps();
    void setTextTwice();
    void insertClamps();
    void lineAccess();
    void lineColAtMatchesReference();
    void surrogateSplitIsCodeUnitLevel();
    void randomOpsVsReference();
    void undoRedoBasics();
    void undoCoalescingRules();
    void editGroupsAreAtomic();
    void breakUndoCoalescingFreezesChain();
    void undoRedoBranching();
    void historyClearing();
    void capDropsOldestGroups();
    void capByLogBytes();
    void randomUndoRedoVsReference();
    void textRangeVsReferenceAcrossPieces();
    void textRangeWalkStaysPrunedWithManyPieces();

private:
    static void verifyAllLines(const TextBuffer& buf, const QString& ref);
    TextBuffer* buffer = nullptr;
};

void TestTextBuffer::emptyDocument()
{
    QCOMPARE(buffer->charCount(), 0);
    QCOMPARE(buffer->lineCount(), 1);
    QCOMPARE(buffer->lineText(0), QString());
    QCOMPARE(buffer->offsetOfLine(0), 0);
    QVERIFY(buffer->debugCheckInvariants());
}

void TestTextBuffer::basicOps()
{
    buffer->setText(QStringLiteral("hello world"));
    QCOMPARE(buffer->charCount(), 11);
    QCOMPARE(buffer->lineCount(), 1);
    QCOMPARE(buffer->text(), QStringLiteral("hello world"));

    buffer->insertAtOffset(5, QStringLiteral(",\n"));
    QCOMPARE(buffer->text(), QStringLiteral("hello,\n world"));
    QCOMPARE(buffer->lineCount(), 2);
    QCOMPARE(buffer->offsetOfLine(1), 7);

    buffer->removeRange(5, 7); // remove ",\n"
    QCOMPARE(buffer->text(), QStringLiteral("hello world"));
    QCOMPARE(buffer->lineCount(), 1);
    QVERIFY(buffer->debugCheckInvariants());
}

void TestTextBuffer::setTextTwice()
{
    buffer->setText(QStringLiteral("first document"));
    buffer->setText(QStringLiteral("hi"));
    QCOMPARE(buffer->text(), QStringLiteral("hi"));
    QCOMPARE(buffer->charCount(), 2);
    QVERIFY(buffer->debugCheckInvariants());
}

void TestTextBuffer::insertClamps()
{
    buffer->setText(QStringLiteral("abc"));
    buffer->insertAtOffset(-10, QStringLiteral("X"));
    buffer->insertAtOffset(9999, QStringLiteral("Y"));
    QCOMPARE(buffer->text(), QStringLiteral("XabcY"));
    buffer->removeRange(4, 99999); // end clamped to charCount
    QCOMPARE(buffer->text(), QStringLiteral("Xabc"));
    buffer->removeRange(3, 1); // swapped
    QCOMPARE(buffer->text(), QStringLiteral("Xc"));
}

void TestTextBuffer::lineAccess()
{
    buffer->setText(QStringLiteral("one\ntwo\nthree"));
    QCOMPARE(buffer->lineCount(), 3);
    QCOMPARE(buffer->lineText(0), QStringLiteral("one"));
    QCOMPARE(buffer->lineText(1), QStringLiteral("two"));
    QCOMPARE(buffer->lineText(2), QStringLiteral("three"));
    QCOMPARE(buffer->offsetOfLine(0), 0);
    QCOMPARE(buffer->offsetOfLine(1), 4);
    QCOMPARE(buffer->offsetOfLine(2), 8);

    // offsetOf clamps col to the line length (never past the '\n').
    QCOMPARE(buffer->offsetOf(1, 0), 4);
    QCOMPARE(buffer->offsetOf(1, 3), 7);
    QCOMPARE(buffer->offsetOf(1, 999), 7);

    // trailing newline => last line is empty
    buffer->setText(QStringLiteral("a\n"));
    QCOMPARE(buffer->lineCount(), 2);
    QCOMPARE(buffer->lineText(1), QString());
}

void TestTextBuffer::lineColAtMatchesReference()
{
    buffer->setText(QStringLiteral("ab\ncd\nefgh"));
    struct Expected { int line; int col; };
    const Expected expected[] = {
        {0, 0}, {0, 1}, {0, 2}, // offset 2 points at '\n': before it
        {1, 0}, {1, 1}, {1, 2},
        {2, 0}, {2, 1}, {2, 2}, {2, 3}, {2, 4},
    };
    for (int offset = 0; offset <= buffer->charCount(); ++offset) {
        const auto lc = buffer->lineColAt(offset);
        QCOMPARE(lc.line, expected[offset].line);
        QCOMPARE(lc.col, expected[offset].col);
    }
}

void TestTextBuffer::surrogateSplitIsCodeUnitLevel()
{
    // Documents the code-unit semantics: removing/inserting inside a
    // surrogate pair is allowed at this layer; the *view* must never do it.
    buffer->setText(QString::fromUtf8("ab\U0001F600cd"));
    QCOMPARE(buffer->charCount(), 6); // 2 + 2 (surrogate pair) + 2
    buffer->insertAtOffset(3, QStringLiteral("X")); // inside the surrogate pair
    QString expected = QStringLiteral("ab");
    expected += QChar(0xD83D);
    expected += QStringLiteral("X");
    expected += QChar(0xDE00);
    expected += QStringLiteral("cd");
    QCOMPARE(buffer->text(), expected);
    QVERIFY(buffer->debugCheckInvariants());
}

void TestTextBuffer::undoRedoBasics()
{
    QCOMPARE(buffer->canUndo(), false);
    QCOMPARE(buffer->canRedo(), false);
    QCOMPARE(buffer->undoDepth(), 0);

    buffer->setText(QStringLiteral("hello")); // setText never records
    QCOMPARE(buffer->undoDepth(), 0);

    buffer->insertAtOffset(5, QStringLiteral(" world"));
    QCOMPARE(buffer->undoDepth(), 1);
    QVERIFY(buffer->canUndo());
    buffer->removeRange(0, 5);
    QCOMPARE(buffer->undoDepth(), 2); // insert and delete are separate steps
    QCOMPARE(buffer->text(), QStringLiteral(" world"));

    QVERIFY(buffer->undo()); // re-inserts "hello"
    QCOMPARE(buffer->text(), QStringLiteral("hello world"));
    QVERIFY(buffer->undo()); // re-removes " world"
    QCOMPARE(buffer->text(), QStringLiteral("hello"));
    QCOMPARE(buffer->undoDepth(), 0);
    QVERIFY(!buffer->undo());
    QCOMPARE(buffer->redoDepth(), 2);

    QVERIFY(buffer->redo()); // G1 first: re-inserts " world"
    QCOMPARE(buffer->text(), QStringLiteral("hello world"));
    QVERIFY(buffer->redo()); // then G2: deletes "hello" again
    QCOMPARE(buffer->text(), QStringLiteral(" world"));
    QCOMPARE(buffer->redoDepth(), 0);
    QVERIFY(!buffer->redo());
    QCOMPARE(buffer->undoDepth(), 2);

    // No-op edits record nothing.
    buffer->insertAtOffset(3, QString());
    buffer->removeRange(2, 2);
    QCOMPARE(buffer->undoDepth(), 2);
    QVERIFY(buffer->debugCheckInvariants());
}

void TestTextBuffer::undoCoalescingRules()
{
    // Adjacent pure inserts coalesce (typing), but newlines break the chain.
    buffer->insertAtOffset(0, QStringLiteral("a"));
    buffer->insertAtOffset(1, QStringLiteral("b"));
    QCOMPARE(buffer->undoDepth(), 1);
    buffer->insertAtOffset(2, QStringLiteral("\n"));
    QCOMPARE(buffer->undoDepth(), 2);
    buffer->insertAtOffset(3, QStringLiteral("c"));
    QCOMPARE(buffer->undoDepth(), 3);
    // Not adjacent: caret moved to the front (no caret here, so only the
    // op geometry matters; the view freezes chains on motion separately).
    buffer->insertAtOffset(0, QStringLiteral("Z"));
    QCOMPARE(buffer->undoDepth(), 4);

    QVERIFY(buffer->undo());
    QCOMPARE(buffer->text(), QStringLiteral("ab\nc"));
    QVERIFY(buffer->undo());
    QCOMPARE(buffer->text(), QStringLiteral("ab\n"));
    QVERIFY(buffer->undo()); // the whole "ab" burst returns as one step
    QCOMPARE(buffer->text(), QStringLiteral("ab"));
    QVERIFY(buffer->undo());
    QCOMPARE(buffer->text(), QString());

    // Typing leftwards (each insert lands before the previous run) also
    // coalesces into one merged op.
    buffer->insertAtOffset(1, QStringLiteral("b"));
    buffer->insertAtOffset(0, QStringLiteral("a"));
    QCOMPARE(buffer->undoDepth(), 1);
    buffer->breakUndoCoalescing();
    buffer->insertAtOffset(2, QStringLiteral("c"));
    QCOMPARE(buffer->undoDepth(), 2);
    QCOMPARE(buffer->text(), QStringLiteral("abc"));
    QVERIFY(buffer->undo());
    QCOMPARE(buffer->text(), QStringLiteral("ab"));

    // Adjacent pure deletions merge without a size cap; mixed kinds never.
    buffer->setText(QStringLiteral("abcdef"));
    const qint64 s0 = buffer->undoPushSerial();
    buffer->removeRange(4, 6);
    buffer->removeRange(2, 4);
    buffer->removeRange(0, 2);
    QCOMPARE(buffer->undoDepth(), 1);
    QCOMPARE(buffer->text(), QString());
    QVERIFY(buffer->undo()); // all of "abcdef" returns in one step
    QCOMPARE(buffer->text(), QStringLiteral("abcdef"));
    QVERIFY(!buffer->canUndo());
    QCOMPARE(buffer->undoPushSerial(), s0 + 1); // the merged deletes pushed once

    // Mixed adjacent insert+delete stay separate steps.
    buffer->removeRange(3, 6); // delete "def"
    buffer->insertAtOffset(3, QStringLiteral("def")); // adjacent insert
    QCOMPARE(buffer->undoDepth(), 2);

    // The merged-insert cap: beyond a typing burst each edit is its own step.
    buffer->setText(QString());
    for (int i = 0; i < 10; ++i)
        buffer->insertAtOffset(buffer->charCount(), QStringLiteral("0123456789"));
    QCOMPARE(buffer->undoDepth(), 2); // chain breaks once >64 units: 60 + 40
    QVERIFY(buffer->debugCheckInvariants());
}

void TestTextBuffer::editGroupsAreAtomic()
{
    buffer->setText(QStringLiteral("hello"));
    buffer->beginEditGroup();
    buffer->removeRange(0, 2); // "he"
    buffer->insertAtOffset(0, QStringLiteral("XY"));
    buffer->endEditGroup();
    QCOMPARE(buffer->undoDepth(), 1); // the compound action is ONE step
    QCOMPARE(buffer->text(), QStringLiteral("XYllo"));

    QVERIFY(buffer->undo()); // reverted atomically, inverse order
    QCOMPARE(buffer->text(), QStringLiteral("hello"));
    QVERIFY(buffer->redo()); // re-applied atomically, forward order
    QCOMPARE(buffer->text(), QStringLiteral("XYllo"));

    // Nesting: the group commits only at the outer endEditGroup.
    buffer->beginEditGroup();
    buffer->beginEditGroup();
    buffer->insertAtOffset(0, QStringLiteral("Q"));
    buffer->endEditGroup(); // inner end: nothing pushed yet
    QCOMPARE(buffer->undoDepth(), 1);
    buffer->endEditGroup(); // outer end: commits the accumulated group
    QCOMPARE(buffer->undoDepth(), 2);

    // A bracketed no-op records nothing and leaves redo alone.
    QVERIFY(buffer->undo()); // drops the "Q" group again
    QVERIFY(buffer->canRedo());
    buffer->beginEditGroup();
    buffer->endEditGroup();
    QCOMPARE(buffer->undoDepth(), 1);
    QVERIFY(buffer->canRedo());

    // undo/redo while a group is open are refused (stack state ambiguous).
    buffer->beginEditGroup();
    buffer->insertAtOffset(0, QStringLiteral("R"));
    QVERIFY(!buffer->undo());
    QVERIFY(!buffer->redo());
    buffer->endEditGroup();
    QVERIFY(buffer->undo()); // only the "R" group goes back: redo was
    // truncated when R was recorded, so "Q" never comes back
    QCOMPARE(buffer->text(), QStringLiteral("XYllo"));
    QVERIFY(buffer->debugCheckInvariants());
}

void TestTextBuffer::breakUndoCoalescingFreezesChain()
{
    buffer->beginEditGroup();
    buffer->insertAtOffset(0, QStringLiteral("bracketed"));
    buffer->endEditGroup();
    // Adjacent to the bracketed run, but brackets never grow:
    buffer->insertAtOffset(9, QStringLiteral("x"));
    QCOMPARE(buffer->undoDepth(), 2);

    // A frozen unbracketed chain likewise refuses the adjacent merge:
    buffer->insertAtOffset(10, QStringLiteral("chain")); // merges into "x"
    buffer->breakUndoCoalescing();
    buffer->insertAtOffset(15, QStringLiteral("more"));
    QCOMPARE(buffer->undoDepth(), 3); // bracketed | x+chain | more

    buffer->breakUndoCoalescing(); // on an empty-ish stack: harmless
    QVERIFY(buffer->debugCheckInvariants());
}

void TestTextBuffer::undoRedoBranching()
{
    buffer->setText(QStringLiteral("base"));
    buffer->insertAtOffset(4, QStringLiteral(" one"));
    buffer->breakUndoCoalescing(); // " two" touches " one": without the break
    buffer->insertAtOffset(8, QStringLiteral(" two")); // they would coalesce
    QCOMPARE(buffer->text(), QStringLiteral("base one two"));
    QCOMPARE(buffer->undoDepth(), 2);

    QVERIFY(buffer->undo());
    QVERIFY(buffer->undo());
    QCOMPARE(buffer->text(), QStringLiteral("base"));
    QCOMPARE(buffer->redoDepth(), 2);

    // A new edit truncates the whole redo branch on its first op.
    buffer->insertAtOffset(4, QStringLiteral(" three"));
    QCOMPARE(buffer->redoDepth(), 0);
    QVERIFY(!buffer->redo());
    QCOMPARE(buffer->text(), QStringLiteral("base three"));
    QVERIFY(buffer->debugCheckInvariants());
}

void TestTextBuffer::historyClearing()
{
    buffer->insertAtOffset(0, QStringLiteral("gone"));
    buffer->beginEditGroup();
    buffer->insertAtOffset(4, QStringLiteral(" group"));
    const qint64 s0 = buffer->undoPushSerial();
    QVERIFY(s0 > 0);

    buffer->setText(QStringLiteral("fresh")); // setText clears the history...
    QCOMPARE(buffer->undoDepth(), 0);
    QCOMPARE(buffer->redoDepth(), 0);
    QVERIFY(!buffer->canUndo());
    // ...and clearUndoHistory() too, while the push serial stays monotonic
    // so callers can keep their mirror alignment across clears.
    buffer->insertAtOffset(5, QStringLiteral(" data"));
    buffer->clearUndoHistory();
    QCOMPARE(buffer->undoDepth(), 0);
    QVERIFY(buffer->undoPushSerial() > s0);
    QVERIFY(buffer->debugCheckInvariants());
}

void TestTextBuffer::capDropsOldestGroups()
{
    // Each insert contains a '\n' so nothing coalesces: exactly one group
    // per edit, and the chain is never merged.
    const int total = TextBuffer::kMaxUndoGroups + 100;
    for (int i = 0; i < total; ++i)
        buffer->insertAtOffset(buffer->charCount(), QStringLiteral("%1\n").arg(i, 5, 10, QLatin1Char('0')));
    QCOMPARE(buffer->undoDepth(), TextBuffer::kMaxUndoGroups);
    QCOMPARE(buffer->undoPushSerial(), static_cast<qint64>(total));

    // Undo the whole retained history: the document rewinds only to the
    // state the drop-oldest cap left behind, not to empty.
    int undos = 0;
    while (buffer->undo())
        ++undos;
    QCOMPARE(undos, TextBuffer::kMaxUndoGroups); // one undo per retained group
    const int dropped = total - TextBuffer::kMaxUndoGroups;
    QString expected;
    for (int i = 0; i < dropped; ++i)
        expected += QStringLiteral("%1\n").arg(i, 5, 10, QLatin1Char('0'));
    QCOMPARE(buffer->text(), expected);
    QVERIFY(!buffer->canUndo());
    QVERIFY(buffer->debugCheckInvariants());
}

void TestTextBuffer::capByLogBytes()
{
    // Big non-coalescible inserts (each over the 64-unit merge cap): the
    // byte cap drops oldest groups long before the count cap.
    const int big = 2 * 1024 * 1024; // 2M units = 4MB of log per group
    const QString chunk(big, QLatin1Char('X'));
    for (int i = 0; i < 10; ++i)
        buffer->insertAtOffset(0, chunk); // adjacent, but far past the merge cap
    const int expectedDepth = static_cast<int>(TextBuffer::kMaxUndoLogBytes / (2ll * big));
    QCOMPARE(buffer->undoDepth(), expectedDepth); // 32MB / 4MB: exactly 8 kept
    QCOMPARE(buffer->charCount(), 10 * big); // the document itself keeps all text
    QVERIFY(buffer->debugCheckInvariants());
}

void TestTextBuffer::verifyAllLines(const TextBuffer& buf, const QString& ref)
{
    const QStringList refLines = ref.split(QLatin1Char('\n'));
    QCOMPARE(buf.lineCount(), static_cast<int>(refLines.size()));
    int offset = 0;
    for (int i = 0; i < refLines.size(); ++i) {
        QCOMPARE(buf.offsetOfLine(i), offset);
        const QString line = buf.lineText(i);
        if (line != refLines[i])
            qWarning() << "line" << i << "buffer=" << line << "ref=" << refLines[i];
        QCOMPARE(line, refLines[i]);
        offset += refLines[i].size() + 1;
    }
}

void TestTextBuffer::randomOpsVsReference()
{
    std::mt19937 rng(0xC0FFEEu);
    const QString alphabet = QStringLiteral("ab\nc\u00e9\U0001F600z");
    QString ref;
    buffer->setText(ref);

    for (int op = 0; op < 2000; ++op) {
        const int refSize = static_cast<int>(ref.size());
        std::uniform_int_distribution<int> posDist(0, std::max(0, refSize - 1));
        if ((op % 3) != 0 || ref.isEmpty()) {
            // insert a random chunk (possibly empty-ish, minimum length 1)
            std::uniform_int_distribution<int> lenDist(1, 20);
            const int len = lenDist(rng);
            QString chunk;
            chunk.reserve(len);
            std::uniform_int_distribution<int> charDist(0, alphabet.size() - 1);
            for (int i = 0; i < len; ++i) {
                const QChar c = alphabet.at(charDist(rng));
                if (c.isHighSurrogate() && i + 1 < len) {
                    chunk += c;
                    chunk += alphabet.at(charDist(rng)); // may be a low surrogate by chance
                    ++i;
                } else {
                    chunk += c;
                }
            }
            const int at = ref.isEmpty() ? 0 : posDist(rng);
            ref.insert(at, chunk);
            buffer->insertAtOffset(at, chunk);
        } else {
            std::uniform_int_distribution<int> lenDist(0, 15);
            const int a = posDist(rng);
            const int b = std::min<int>(ref.size() - 1, a + lenDist(rng));
            ref.remove(a, b - a + 1);
            buffer->removeRange(a, b + 1);
        }

        QVERIFY2(buffer->debugCheckInvariants(), qPrintable(QStringLiteral("invariant broken at op %1").arg(op)));
        QCOMPARE(buffer->charCount(), ref.size());
        QCOMPARE(buffer->lineCount(), 1 + ref.count(QLatin1Char('\n')));
        const QString full = buffer->text();
        if (full != ref)
            qWarning() << "mismatch at op" << op;
        QCOMPARE(full, ref);
        if ((op % 50) == 0)
            verifyAllLines(*buffer, ref);
    }
    verifyAllLines(*buffer, ref);
    QVERIFY(buffer->debugNodeCount() > 0);
}

void TestTextBuffer::textRangeVsReferenceAcrossPieces()
{
    // The undo inverse-capture door and every small view probe ride on
    // textRange's pruned walk — compare it against a QString reference over
    // a fragmented document. The unpruned M3 walk was correct but slow, so
    // nothing else in the suite pins this door; these spans must stay exact.
    QString ref = QStringLiteral("The quick brown fox.\nsecond line\nthird");
    buffer->setText(ref);
    std::mt19937 rng(0x5EEDu);
    for (int i = 0; i < 40; ++i) {
        std::uniform_int_distribution<int> posDist(0, static_cast<int>(ref.size()));
        const int at = posDist(rng);
        const QString chunk = QStringLiteral("<%1>").arg(i);
        ref.insert(at, chunk);
        buffer->insertAtOffset(at, chunk);
    }
    for (int i = 0; i < 15; ++i) {
        std::uniform_int_distribution<int> posDist(0, std::max(0, static_cast<int>(ref.size()) - 8));
        const int at = posDist(rng);
        ref.remove(at, 5);
        buffer->removeRange(at, at + 5);
    }
    QVERIFY(buffer->debugCheckInvariants());

    // Exhaustive all-pairs over a small window around the front half.
    const int limit = std::min<int>(ref.size(), 60);
    for (int a = 0; a <= limit; ++a)
        for (int b = a; b <= limit; ++b) {
            QVERIFY2(buffer->textRange(a, b) == ref.mid(a, b - a),
                     qPrintable(QStringLiteral("span [%1,%2)").arg(a).arg(b)));
            // Swapped arguments must behave identically (removeRange convention).
            QVERIFY2(buffer->textRange(b, a) == ref.mid(a, b - a),
                     qPrintable(QStringLiteral("swapped span [%1,%2)").arg(b).arg(a)));
            // Single-unit probes (the paint indent-check door).
            if (b + 1 < limit)
                QVERIFY(buffer->textRange(b, b + 1) == ref.mid(b, 1));
        }

    // Random far-flung spans on the whole fragmented document, plus clamps
    // and empty windows at the seams.
    for (int s = 0; s < 500; ++s) {
        std::uniform_int_distribution<int> posDist(0, std::max(0, static_cast<int>(ref.size()) - 1));
        const int a = posDist(rng);
        const int b = std::min<int>(static_cast<int>(ref.size()), a + std::uniform_int_distribution<int>(0, 97)(rng));
        QVERIFY2(buffer->textRange(a, b) == ref.mid(a, b - a),
                 qPrintable(QStringLiteral("random span [%1,%2)").arg(a).arg(b)));
    }
    QCOMPARE(buffer->textRange(-100, 100000), ref); // clamped
    QCOMPARE(buffer->textRange(buffer->charCount(), buffer->charCount()), QString());
    QCOMPARE(buffer->textRange(5, 5), QString());   // empty window
    QCOMPARE(buffer->text(), ref);
}

void TestTextBuffer::textRangeWalkStaysPrunedWithManyPieces()
{
    // Tripwire for the regression the Gate-1 bench exit criterion caught
    // (mean 7 -> 37 us/op on the bench document): a small-span textRange was
    // walking EVERY piece left of the window. Prepend many single-char
    // pieces, then probe the document's last unit: an unpruned walk costs
    // ~pieces-per-probe (~20k steps x 1000 probes is >> this budget on any
    // runner), while the subtree-char-pruned walk costs ~treap depth.
    buffer->setText(QString(20000, QLatin1Char('x')));
    for (int i = 0; i < 20000; ++i)
        buffer->insertAtOffset(0, QStringLiteral("a"));
    buffer->clearUndoHistory(); // probe cost, not history cost
    QVERIFY2(buffer->debugNodeCount() > 20000,
             qPrintable(QStringLiteral("fragmentation sanity: %1 nodes").arg(buffer->debugNodeCount())));

    const int last = buffer->charCount() - 1;
    QElapsedTimer t;
    double bestMs = 1e30;
    for (int round = 0; round < 3; ++round) {
        t.start();
        for (int probe = 0; probe < 1000; ++probe)
            QCOMPARE(buffer->textRange(last, last + 1), QLatin1String("x"));
        bestMs = std::min(bestMs, static_cast<double>(t.nsecsElapsed()) / 1e6);
    }
    // 10 us/probe: ~100x above the pruned walk's ~treap-depth cost, ~5x
    // BELOW the unpruned walk even on a cache-friendly small tree.
    QVERIFY2(bestMs < 10.0,
             qPrintable(QStringLiteral("1000 end-of-document probes took %1 ms best-of-3; "
                                       "the textRange walk is no longer pruned by subtree chars")
                            .arg(bestMs, 0, 'f', 2)));
}

void TestTextBuffer::randomUndoRedoVsReference()
{
    // M3 DoD: undo/redo round-trip against a naive QString reference,
    // same style as randomOpsVsReference. The model mirrors the CORE's
    // public contract only (undoDepth + undoPushSerial + canUndo/canRedo),
    // exactly like the view does - so it also exercises the alignment
    // scheme end to end, coalescing and all.
    std::mt19937 rng(0xBADCAFE0u);
    const QString alphabet = QStringLiteral("ab\nc\u00e9\U0001F600z");
    auto randomChunk = [&](int minLen, int maxLen) {
        std::uniform_int_distribution<int> lenDist(minLen, maxLen);
        const int len = lenDist(rng);
        QString chunk;
        chunk.reserve(len);
        std::uniform_int_distribution<int> charDist(0, alphabet.size() - 1);
        for (int i = 0; i < len; ++i) {
            const QChar c = alphabet.at(charDist(rng));
            if (c.isHighSurrogate() && i + 1 < len) {
                chunk += c;
                chunk += alphabet.at(charDist(rng)); // may be a low surrogate by chance
                ++i;
            } else {
                chunk += c;
            }
        }
        return chunk;
    };
    auto randPos = [&](int limit) {
        std::uniform_int_distribution<int> posDist(0, limit);
        return posDist(rng);
    };

    QString ref;
    buffer->setText(ref);
    QStringList hist; // hist[i] = reference state after core group i
    QStringList future; // undone group states, LIFO (core redoStack mirror)
    hist.append(ref);

    int undoSteps = 0, redoSteps = 0;

    for (int step = 0; step < 800; ++step) {
        const int d0 = buffer->undoDepth();
        const qint64 s0 = buffer->undoPushSerial();
        std::uniform_int_distribution<int> choice(0, 99);
        const int c = choice(rng);

        if (c < 40 || ref.isEmpty()) { // plain unbracketed insert
            const QString chunk = randomChunk(1, 12);
            const int at = randPos(ref.size());
            ref.insert(at, chunk);
            buffer->insertAtOffset(at, chunk);
            future.clear(); // any recorded edit truncates the redo branch
        } else if (c < 62) { // plain unbracketed delete
            const int a = randPos(ref.size() - 1);
            const int len = std::min(1 + randPos(9), static_cast<int>(ref.size()) - a);
            ref.remove(a, len);
            buffer->removeRange(a, a + len);
            future.clear();
        } else if (c < 74) { // bracketed delete+insert (replace-selection)
            const int a = randPos(ref.size() - 1);
            const int len = std::min(1 + randPos(6), static_cast<int>(ref.size()) - a);
            const QString chunk = randomChunk(1, 8);
            ref.remove(a, len);
            ref.insert(a, chunk);
            buffer->beginEditGroup();
            buffer->removeRange(a, a + len);
            buffer->insertAtOffset(a, chunk);
            buffer->endEditGroup();
            future.clear();
        } else if (c < 84) { // bracketed multi-op compound action
            buffer->beginEditGroup();
            const int ops = 2 + randPos(2);
            for (int k = 0; k < ops; ++k) {
                if ((k % 2) == 0 || ref.isEmpty()) {
                    const QString chunk = randomChunk(1, 6);
                    const int at = randPos(ref.size());
                    ref.insert(at, chunk);
                    buffer->insertAtOffset(at, chunk);
                } else {
                    const int a = randPos(ref.size() - 1);
                    const int len = std::min(1 + randPos(4), static_cast<int>(ref.size()) - a);
                    ref.remove(a, len);
                    buffer->removeRange(a, a + len);
                }
            }
            buffer->endEditGroup();
            future.clear();
        } else if (c < 88) { // the view breaks chains on deliberate motion
            buffer->breakUndoCoalescing();
        } else if (c < 95) { // partial undos
            if (buffer->canUndo()) {
                QVERIFY2(hist.size() >= 2, qPrintable(QStringLiteral("mirror lost sync at step %1").arg(step)));
                QVERIFY(buffer->undo());
                future.append(hist.takeLast());
                ref = hist.back();
                ++undoSteps;
                // Follow-up redo burst: real callers hammer Ctrl+Shift+Z
                // right after undoing; the plain redo bucket alone leaves
                // too few redos because edits usually clear the branch first.
                if (randPos(9) < 5) {
                    const int extra = 1 + randPos(2);
                    for (int k = 0; k < extra; ++k) {
                        if (!buffer->canRedo())
                            break;
                        QVERIFY(!future.isEmpty());
                        QVERIFY(buffer->redo());
                        ref = future.takeLast();
                        hist.append(ref);
                        ++redoSteps;
                    }
                }
            } else {
                QVERIFY(!buffer->undo());
                QCOMPARE(hist.size(), 1);
            }
        } else { // redos
            if (buffer->canRedo()) {
                QVERIFY(!future.isEmpty());
                QVERIFY(buffer->redo());
                ref = future.takeLast();
                hist.append(ref);
                ++redoSteps;
            } else {
                QVERIFY(future.isEmpty());
                QVERIFY(!buffer->redo());
            }
        }

        // Mirror reconciliation (the view's contract): a pushed group grew
        // the serial by one; coalescing or no-ops did not touch it.
        const int d1 = buffer->undoDepth();
        const qint64 s1 = buffer->undoPushSerial();
        if (s1 == s0 + 1) {
            QCOMPARE(d1, d0 + 1); // view-side guarantee: one group per action
            hist.append(ref);
        } else {
            QCOMPARE(s1, s0);
        }
        hist[hist.size() - 1] = ref; // coalesced, undone/redone or unchanged

        // Lockstep assertions between the two models.
        QCOMPARE(buffer->undoDepth(), static_cast<int>(hist.size()) - 1);
        QCOMPARE(buffer->redoDepth(), static_cast<int>(future.size()));
        QVERIFY2(buffer->debugCheckInvariants(), qPrintable(QStringLiteral("invariant broken at step %1").arg(step)));
        const QString full = buffer->text();
        if (full != ref)
            qWarning() << "undo-model mismatch at step" << step << "op class" << c;
        QCOMPARE(full, ref);
        if ((step % 40) == 0)
            verifyAllLines(*buffer, ref);
    }

    // The property test must actually have exercised both branches.
    QVERIFY2(undoSteps > 50, "too few undos in the random stream");
    QVERIFY2(redoSteps > 10, "too few redos in the random stream");
    QVERIFY(buffer->debugCheckInvariants());
}

QTEST_GUILESS_MAIN(TestTextBuffer)
#include "tst_textbuffer.moc"
