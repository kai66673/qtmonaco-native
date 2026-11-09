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

QTEST_GUILESS_MAIN(TestTextBuffer)
#include "tst_textbuffer.moc"
