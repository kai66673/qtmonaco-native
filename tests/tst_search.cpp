// tst_search — the GATE 4 car I1 property tests for qmn::search.
// GUITLESS by layer law (the textbuffer/diff class): the engine is
// QtCore-only, so no QT_QPA_PLATFORM story, no display, pure walk.
//
// The heart is the NAIVE-REFERENCE pair on a FIXED-SEED corpus (the
// tst_textbuffer style): the reference re-derives every span by a
// dead-simple per-position scan (never the engine's indexOf loop), and
// the wholeWord face gets its OWN boundary logic re-implemented in the
// reference (the engine's pattern-wrap for regex wholeWord and the
// reference's raw-iterator-plus-boundary-check are two DIFFERENT
// mechanisms for the same semantic — their agreement is the pin).
//
// A real divergence this file caught at writing time (kept as a named
// tripwire): a REJECTED wholeWord occurrence must SLIDE one position,
// never skip past its window — needle "a.a" on line "xa.a.a" has an
// occurrence at 1 with a dirty left boundary and a nested occurrence at
// 3 with CLEAN boundaries; a jump-past-window implementation reports
// zero hits where slide reports exactly [3,6). Both faces (fixed-string
// indexOf and regex pattern-wrap — PCRE slides lookaround-failed
// positions by itself) answer 3-6; the tripwire is wholeWordNestedSlide.
//
// Documented seam (kept OUT of the corpus deliberately): regex \w with
// unicode properties also covers combining MARKS, which the shared
// letter/number/'_' boundary class does not. If a consumer ever needs
// mark neighbors, the face-agreement question goes to the ledger.

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QRandomGenerator>

#include <functional>

#include "search/SearchEngine.hpp"

using namespace qmn::search;

namespace {

// ---------------------------------------------------------------- naive

bool refIsWordChar(QChar c)
{
    // Re-implemented boundary class (letter/number/underscore) — the
    // engine keeps its own copy; the pin is that they AGREE on the
    // letter/number/'_' space the corpus lives in.
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

QString refPreview(const QString& line)
{
    // The preview law re-derived independently (CR courtesy + cap +
    // ellipsis) — the fmt helpers below never print it, but keeping the
    // law next to the reference makes the pin pair honest.
    QString preview = line;
    if (preview.endsWith(QLatin1Char('\r')))
        preview.chop(1);
    if (preview.size() > kPreviewCap)
        preview = preview.left(kPreviewCap) + QChar(0x2026);
    return preview;
}

QList<LineMatch> refScanText(const QString& text, const SearchQuery& q)
{
    QList<LineMatch> out;
    const QStringList lines = text.split(QLatin1Char('\n')); // whole-file
                                                             // split: the
                                                             // naive face
    int lineNo = 0;
    for (const QString& line : lines) {
        QList<MatchSpan> spans;
        if (q.isRegex) {
            // Reference regex face: the RAW pattern + a manual boundary
            // check + a MANUAL SLIDE on a rejected match (the
            // from-cursor loop). The engine WRAPS the pattern in
            // lookarounds instead; the two mechanisms agreeing is the
            // wholeWord-regex pin (see wholeWordNestedSlide).
            QRegularExpression re(
                q.text,
                (q.caseSensitive ? QRegularExpression::PatternOptions{}
                                 : QRegularExpression::CaseInsensitiveOption)
                | QRegularExpression::UseUnicodePropertiesOption);
            int from = 0;
            for (;;) {
                const QRegularExpressionMatch m = re.match(line, from);
                if (!m.hasMatch())
                    break;
                const int start = m.capturedStart(0);
                const int len = m.capturedLength(0);
                if (len <= 0) { // zero-length hits never count (shared
                                // documented law; advance to not loop)
                    from = start + 1;
                    continue;
                }
                const int end = start + len;
                const bool okL = start == 0 || !refIsWordChar(line.at(start - 1));
                const bool okR = end >= line.size() || !refIsWordChar(line.at(end));
                if (!q.wholeWord || (okL && okR)) {
                    spans.append({start, end});
                    from = end;
                } else {
                    from = start + 1;
                }
            }
        } else {
            // Fixed-string reference: per-position compare (quadratic on
            // purpose — this is the reference, not the implementation).
            int i = 0;
            while (i + q.text.size() <= line.size()) {
                const QString piece = line.mid(i, q.text.size());
                bool hit = q.caseSensitive
                    ? (piece == q.text)
                    : (piece.compare(q.text, Qt::CaseInsensitive) == 0);
                if (hit && q.wholeWord) {
                    if (i > 0 && refIsWordChar(line.at(i - 1)))
                        hit = false;
                    else if (i + q.text.size() < line.size()
                             && refIsWordChar(line.at(i + q.text.size())))
                        hit = false;
                }
                if (hit) {
                    spans.append({i, int(i + q.text.size())});
                    i += q.text.size();
                } else {
                    ++i;
                }
            }
        }
        if (!spans.isEmpty())
            out.append({lineNo, spans, refPreview(line)});
        ++lineNo;
    }
    return out;
}

// A flat comparable spelling: "line:start-end,start-end|line:…" (the
// preview deliberately OUT of the spelling — its own pins live in
// previewCapFace/crlfPreviewLaw).
QString fmtMatches(const QList<LineMatch>& lines)
{
    QStringList perLine;
    for (const LineMatch& lm : lines) {
        QStringList spans;
        for (const MatchSpan& s : lm.spans)
            spans.append(QStringLiteral("%1-%2").arg(s.startCol).arg(s.endCol));
        perLine.append(QStringLiteral("%1:%2").arg(lm.line).arg(spans.join(QLatin1Char(','))));
    }
    return perLine.join(QLatin1Char('|'));
}

int spanTotal(const QList<LineMatch>& lines)
{
    int total = 0;
    for (const LineMatch& lm : lines)
        total += lm.spans.size();
    return total;
}

bool waitFinished(QSignalSpy& doneSpy, int timeoutMs = 20000)
{
    for (int waited = 0; doneSpy.isEmpty(); waited += 10) {
        if (waited > timeoutMs)
            return false;
        QTest::qWait(10);
    }
    return true;
}

void writeRaw(const QString& absPath, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(absPath).absolutePath());
    QFile f(absPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(bytes);
    f.close();
}

} // namespace

class TestSearch : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());

        // The corpus: seeded generation (the repo's fixed-seed law — a
        // failure is reproducible), aimed at WHOLE-WORD neighbors, case
        // folds, astral columns and CRLF files all at once.
        QRandomGenerator rng(0xC0FFEEu);
        const QStringList pool {
            QStringLiteral("sum"), QStringLiteral("Sum"), QStringLiteral("SUM"),
            QStringLiteral("sums"), QStringLiteral("subtotal"), QStringLiteral("x"),
            QStringLiteral("sum1"), QStringLiteral("\U0001F600"), QStringLiteral("data"),
            QStringLiteral("a.a"), QStringLiteral("(sum)")};
        for (int f = 0; f < 24; ++f) {
            QString rel = (f % 4 == 3)
                ? QStringLiteral("sub/deep/f%1.txt").arg(f, 2, 10, QLatin1Char('0'))
                : (f % 4 == 2)
                ? QStringLiteral("sub/f%1.txt").arg(f, 2, 10, QLatin1Char('0'))
                : QStringLiteral("f%1.txt").arg(f, 2, 10, QLatin1Char('0'));
            if (f % 4 == 0)
                rel = QStringLiteral("sub/gitdir/") + QFileInfo(rel).fileName();
            if (f == 7)
                rel = QStringLiteral(".f7.txt"); // the hidden face
            const bool crlf = (f % 3 == 0);
            QString text;
            const int lineCount = 40 + int(rng.bounded(81));
            for (int l = 0; l < lineCount; ++l) {
                const int tokens = 1 + int(rng.bounded(8));
                QStringList parts;
                for (int t = 0; t < tokens; ++t)
                    parts.append(pool.at(int(rng.bounded(pool.size()))));
                const QString sep = (l % 5 == 0) ? QStringLiteral(",")
                                  : (l % 7 == 0) ? QStringLiteral("\t")
                                                 : QStringLiteral(" ");
                text += parts.join(sep);
                text += crlf ? QStringLiteral("\r\n") : QStringLiteral("\n");
            }
            writeRaw(m_dir.filePath(rel), text.toUtf8());
        }
        writeRaw(m_dir.filePath(QStringLiteral(".git/config.txt")),
                 QStringLiteral("sum sum\n").toUtf8());
        writeRaw(m_dir.filePath(QStringLiteral(".hidden_dir/h.txt")),
                 QStringLiteral("sum deep-hidden\n").toUtf8());

        // The reference's view of the SAME tree under the session's
        // DEFAULT contract (skip hidden, skip .git) — the contract
        // mirroring lives HERE, in the test, never borrowed from the
        // engine's own walk.
        std::function<void(const QString&)> refWalk = [&](const QString& abs) {
            QDir dir(abs);
            for (const QFileInfo& e : dir.entryInfoList(
                     QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot)) {
                if (e.fileName() == QLatin1String(".git"))
                    continue;
                if (e.isDir()) {
                    refWalk(e.absoluteFilePath());
                } else {
                    QFile f(e.absoluteFilePath());
                    QVERIFY(f.open(QIODevice::ReadOnly));
                    m_refTexts.insert(e.absoluteFilePath(),
                                      QString::fromUtf8(f.readAll()));
                }
            }
        };
        refWalk(m_dir.path());
        // 24 corpus files, one hidden (.f7.txt) — the reference's
        // DEFAULT-contract view must see 23 (a generator slip once hid
        // 18 of them behind one path; the >=20 floor is that trap's
        // tripwire, not a fudge):
        QCOMPARE(m_refTexts.size(), 23);
    }

    // ------------------------------------------------- the property core

    void fixedSeedWalkMatchesNaiveReference()
    {
        for (const QString& text : {QStringLiteral("sum"), QStringLiteral("data")}) {
            SearchQuery q;
            q.text = text; // default: case-INsensitive (the naive
                           // compare carries the fold face)
            QHash<QString, QString> refMap;
            int refTotalMatches = 0, refMatchedFiles = 0;
            for (auto it = m_refTexts.constBegin(); it != m_refTexts.constEnd(); ++it) {
                const QList<LineMatch> hits = refScanText(it.value(), q);
                if (hits.isEmpty())
                    continue;
                refMap.insert(it.key(), fmtMatches(hits));
                refTotalMatches += spanTotal(hits);
                ++refMatchedFiles;
            }
            QVERIFY(!refMap.isEmpty());

            SearchSession session; // fresh per query (the reentry law
                                   // also guarantees a clean walk)
            QSignalSpy matchedSpy(&session, &SearchSession::fileMatched);
            QSignalSpy doneSpy(&session, &SearchSession::finished);
            session.start(m_dir.path(), q);
            QVERIFY2(waitFinished(doneSpy), "session never finished");
            QCOMPARE(doneSpy.at(0).at(4).toString(), QString()); // completed
            QHash<QString, QString> gotMap;
            for (const QList<QVariant>& args : matchedSpy)
                gotMap.insert(args.at(0).value<FileMatch>().filePath,
                              fmtMatches(args.at(0).value<FileMatch>().lines));
            QCOMPARE(gotMap.size(), refMap.size());
            for (auto it = gotMap.constBegin(); it != gotMap.constEnd(); ++it) {
                QVERIFY2(refMap.contains(it.key()),
                         qPrintable(QStringLiteral("engine matched unseen file %1").arg(it.key())));
                QVERIFY2(it.value() == refMap.value(it.key()),
                         qPrintable(QStringLiteral("file %1 query %2\n got [%3]\nwant [%4]")
                                        .arg(it.key(), text, it.value(),
                                             refMap.value(it.key()))));
            }
            // The finished() totals agree with the reference's sums, and
            // the scanned count agrees with the reference CONTRACT walk
            // (the same tree, both walks under the same skip law):
            const int scanned = doneSpy.at(0).at(0).toInt();
            QCOMPARE(scanned, m_refTexts.size());
            QCOMPARE(doneSpy.at(0).at(1).toInt(), refMatchedFiles);
            QCOMPARE(doneSpy.at(0).at(2).toInt(), refTotalMatches);
            QCOMPARE(doneSpy.at(0).at(3).toInt(), 0); // nothing skipped
        }
    }

    void scanTextFaceAgreesWithWalkFace()
    {
        // The panel's in-buffer face and the walk face are the SAME
        // engine — the FileMatch lines must equal scanText(file text).
        SearchQuery q;
        q.text = QStringLiteral("sum");
        q.caseSensitive = true;
        int checked = 0;
        for (auto it = m_refTexts.constBegin(); it != m_refTexts.constEnd(); ++it) {
            const FileScan fs = Engine::scanFile(it.key(), q);
            QCOMPARE(int(fs.skip), int(SkipReason::None));
            const QString walkFmt = fmtMatches(fs.match.lines);
            const QString bufferFmt = fmtMatches(Engine::scanText(it.value(), q));
            QVERIFY2(walkFmt == bufferFmt,
                     qPrintable(QStringLiteral("%1\nwalk   [%2]\nbuffer [%3]")
                                    .arg(it.key(), walkFmt, bufferFmt)));
            ++checked;
        }
        QVERIFY(checked >= 20);
    }

    // ------------------------------------------------------ the unit laws

    void astralColumnsCountUtf16Units()
    {
        // THE leg that catches the byte-offset slip (the tree-sitter 2×
        // trap's sibling class): "a😀b" — the emoji is TWO UTF-16 units.
        SearchQuery q;
        q.text = QStringLiteral("b");
        const QList<LineMatch> hits = Engine::scanText(QStringLiteral("a\U0001F600b"), q);
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits.at(0).spans.at(0).startCol, 3);
        QCOMPARE(hits.at(0).spans.at(0).endCol, 4);

        SearchQuery q2;
        q2.text = QStringLiteral("ok");
        const QList<LineMatch> h2 = Engine::scanText(QStringLiteral("\U0001F600\U0001F600 ok"), q2);
        QCOMPARE(h2.at(0).spans.at(0).startCol, 5);

        // the astral character ITSELF as the needle (a surrogate pair):
        SearchQuery q3;
        q3.text = QStringLiteral("\U0001F600");
        const QList<LineMatch> h3 = Engine::scanText(QStringLiteral("x\U0001F600y"), q3);
        QCOMPARE(h3.at(0).spans.at(0).startCol, 1);
        QCOMPARE(h3.at(0).spans.at(0).endCol, 3);
    }

    void wholeWordNestedSlide()
    {
        // The writing-time divergence tripwire (this file's header):
        // needle "a.a" on "xa.a.a" — the occurrence at 1 has a dirty
        // LEFT, the nested occurrence at 3 is CLEAN; the answer is
        // exactly "0:3-6" on EVERY face.
        SearchQuery q;
        q.text = QStringLiteral("a.a");
        q.wholeWord = true;
        const QList<LineMatch> fixed = Engine::scanText(QStringLiteral("xa.a.a"), q);
        QCOMPARE(fmtMatches(fixed), QStringLiteral("0:3-6"));

        q.isRegex = true;
        q.text = QStringLiteral("a\\.a");
        QCOMPARE(fmtMatches(Engine::scanText(QStringLiteral("xa.a.a"), q)),
                 QStringLiteral("0:3-6"));
        QCOMPARE(fmtMatches(refScanText(QStringLiteral("xa.a.a"), q)),
                 QStringLiteral("0:3-6"));
    }

    void wholeWordBoundaryFaces()
    {
        SearchQuery q;
        q.text = QStringLiteral("sum");
        q.wholeWord = true;
        // underscore is a word character (both faces agree): "sum_" fails
        // the right boundary.
        QCOMPARE(fmtMatches(Engine::scanText(QStringLiteral("sum sum_ x sum"), q)),
                 QStringLiteral("0:0-3,11-14"));
        // a CYRILLIC letter glued directly to the needle fails the left
        // boundary (the unit counts, and sum after '-' where the hyphen
        // is NOT a word character rides clean):
        QCOMPARE(fmtMatches(Engine::scanText(QStringLiteral("\u043F\u0440\u0438\u0432\u0435\u0442-sum sum"), q)),
                 QStringLiteral("0:7-10,11-14"));
        // …and the glued-cyrillic face really does refuse its neighbor:
        QCOMPARE(fmtMatches(Engine::scanText(QStringLiteral("\u043F\u0440\u0438\u0432\u0435\u0442sum sum"), q)),
                 QStringLiteral("0:10-13"));
    }

    void caseInsensitiveFoldFace()
    {
        SearchQuery q;
        q.text = QStringLiteral("\u00c4"); // 'A with diaeresis'
        const QList<LineMatch> hits = Engine::scanText(QStringLiteral("xyz \u00e4"), q);
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits.at(0).spans.at(0).startCol, 4);
        // the case face never shifts the UNIT grid:
        SearchQuery q2;
        q2.text = QStringLiteral("AB");
        QCOMPARE(Engine::scanText(QStringLiteral("\U0001F600ab"), q2).at(0).spans.at(0).startCol, 2);
    }

    void nonOverlappingSpansLaw()
    {
        SearchQuery q;
        q.text = QStringLiteral("aa");
        QCOMPARE(fmtMatches(Engine::scanText(QStringLiteral("aaaa"), q)),
                 QStringLiteral("0:0-2,2-4"));
        // multi-line line numbers ride:
        QCOMPARE(fmtMatches(Engine::scanText(QStringLiteral("aa\nx\naa"), q)),
                 QStringLiteral("0:0-2|2:0-2"));
    }

    void regexFacesAndRefusals()
    {
        SearchQuery q;
        q.isRegex = true;
        q.text = QStringLiteral("sum\\d");
        // (the match is "sum<digit>" — FOUR units, not five: the pin
        // spells the truth, my first draft's 0-5 was wrong arithmetic):
        QCOMPARE(fmtMatches(Engine::scanText(QStringLiteral("sum1 sums2 sum99"), q)),
                 QStringLiteral("0:0-4,11-15"));

        // Zero-length regex hits never count (the shared documented law):
        SearchQuery q0;
        q0.isRegex = true;
        q0.text = QStringLiteral("x*");
        QVERIFY(Engine::scanText(QStringLiteral("yyy"), q0).isEmpty());

        // Loud refusals: "(" does not compile — the REFUSAL is non-empty
        // and NOTHING was scanned (never an empty-looking success):
        SearchQuery bad;
        bad.isRegex = true;
        bad.text = QStringLiteral("(sum");
        QVERIFY(!Engine::queryRefusal(bad).isEmpty());
        QString refusal;
        QVERIFY(Engine::scanText(QStringLiteral("(sum"), bad, &refusal).isEmpty());
        QVERIFY(refusal.startsWith(QStringLiteral("query: invalid regex:")));
        // The FILE face on a refused query: the caller-level refusal is
        // the loud face (the session refuses BEFORE opening a single
        // file — pinned below); scanFile itself answers None + empty
        // (the documented miss spelling, never a crash):
        const QString someFile = m_refTexts.constBegin().key();
        const FileScan fs = Engine::scanFile(someFile, bad);
        QCOMPARE(int(fs.skip), int(SkipReason::None));
        QVERIFY(fs.match.lines.isEmpty());
        // empty text also refuses loudly:
        SearchQuery empty;
        QVERIFY(Engine::queryRefusal(empty).startsWith(QStringLiteral("query: empty text")));
    }

    void sessionRefusalFacesLandSynchronously()
    {
        // The CONNECT-BEFORE-START law (car E's mechanism at this door):
        // captures connected BEFORE start(), the refusal lands
        // SYNCHRONOUSLY inside the call, and the scan count is ZERO.
        SearchSession session;
        QSignalSpy doneSpy(&session, &SearchSession::finished);
        SearchQuery bad;
        bad.isRegex = true;
        bad.text = QStringLiteral("(sum");
        session.start(m_dir.path(), bad);
        QCOMPARE(doneSpy.size(), 1);
        QCOMPARE(doneSpy.at(0).at(0).toInt(), 0);
        QVERIFY(doneSpy.at(0).at(4).toString().startsWith(QStringLiteral("query:")));

        QSignalSpy doneSpy2(&session, &SearchSession::finished);
        SearchQuery ok;
        ok.text = QStringLiteral("sum");
        session.start(m_refTexts.constBegin().key(), ok); // a FILE as the
                                                         // root
        QCOMPARE(doneSpy2.size(), 1);
        QVERIFY(doneSpy2.at(0).at(4).toString().startsWith(QStringLiteral("root:")));
    }

    void sessionReentryAndMidRunKnobs()
    {
        SearchSession session;
        session.setMaxFiles(3);
        QSignalSpy doneSpy(&session, &SearchSession::finished);
        SearchQuery q;
        q.text = QStringLiteral("sum");
        session.start(m_dir.path(), q);
        QVERIFY(session.running());
        session.start(m_dir.path(), q); // IGNORED: no second session can
                                        // leak into the same signals
        session.setMaxFiles(99999);     // a mid-run set is a no-op…
        QCOMPARE(session.maxFiles(), 3);
        QVERIFY(waitFinished(doneSpy));
        QCOMPARE(doneSpy.size(), 1);
        QVERIFY(!session.running());
        session.setMaxFiles(99999); // not running: the set LANDS
        QCOMPARE(session.maxFiles(), 99999);
    }

    void walkSkipContractFaces()
    {
        // The walk CONTRACT on its OWN small tree (the caps are honest
        // only on a tree small enough for them — the main corpus would
        // just drown in TooLarge skips):
        QTemporaryDir small;
        QVERIFY(small.isValid());
        writeRaw(small.filePath(QStringLiteral("bin/data.bin")),
                 QByteArray("sum \0sum", 9));            // NUL in the sniff
                                                         // window, under a
                                                         // 12-byte cap
        writeRaw(small.filePath(QStringLiteral("big.txt")),
                 QByteArray("sum sum sum sum"));         // over the cap
        writeRaw(small.filePath(QStringLiteral(".hidden.txt")),
                 QByteArray("sum hidden\n"));
        writeRaw(small.filePath(QStringLiteral(".git/objects.txt")),
                 QByteArray("sum git\n"));
        writeRaw(small.filePath(QStringLiteral("ok.txt")), QByteArray("sum ok\n"));

        SearchSession session;
        session.setMaxFileSize(12);
        QSignalSpy skipSpy(&session, &SearchSession::fileSkipped);
        QSignalSpy doneSpy(&session, &SearchSession::finished);
        SearchQuery q;
        q.text = QStringLiteral("sum");
        session.start(small.path(), q);
        QVERIFY(waitFinished(doneSpy));

        QStringList why;
        for (const QList<QVariant>& a : skipSpy)
            why.append(QStringLiteral("%1:%2")
                           .arg(QFileInfo(a.at(0).toString()).fileName())
                           .arg(a.at(1).toInt()));
        QVERIFY2(why.contains(QStringLiteral("data.bin:%1").arg(int(SkipReason::Binary))),
                 qPrintable(why.join(QLatin1Char(','))));
        QVERIFY2(why.contains(QStringLiteral("big.txt:%1").arg(int(SkipReason::TooLarge))),
                 qPrintable(why.join(QLatin1Char(','))));
        // Default OFF faces: hidden never scanned, .git subtree never
        // seen AT ALL (not even as a skip):
        QCOMPARE(doneSpy.at(0).at(0).toInt(), 3); // big.txt + ok.txt +
                                                  //   data.bin (skipped
                                                  //   files COUNT as
                                                  //   scanned — the walk
                                                  //   opened and judged
                                                  //   them)
        QCOMPARE(doneSpy.at(0).at(3).toInt(), 2); // data.bin + big.txt skipped
        QCOMPARE(doneSpy.at(0).at(1).toInt(), 1); // only ok.txt matched
        QVERIFY(doneSpy.at(0).at(4).toString().isEmpty());
    }

    void listModeFaces()
    {
        // The startList face (LEDGER catch #2, car I2): the SAME walk
        // order/filters/budget/stop faces, names ONLY — contents never
        // read. The expected order is hand-pinned from the order law
        // (sorted files of a dir drain before its subdirs, subdirs in
        // codepoint order — the pump's queue discipline, mirrored in the
        // TEST, never borrowed from the engine).
        QTemporaryDir small;
        QVERIFY(small.isValid());
        writeRaw(small.filePath(QStringLiteral("big.txt")),
                 QByteArray("sum sum sum sum"));         // 17 B — OVER a 12B cap
        writeRaw(small.filePath(QStringLiteral("bin/data.bin")),
                 QByteArray("sum \0sum", 9));            // NUL in the sniff
        writeRaw(small.filePath(QStringLiteral(".hidden.txt")),
                 QByteArray("sum hidden\n"));            // hidden: never listed
        writeRaw(small.filePath(QStringLiteral(".git/objects.txt")),
                 QByteArray("sum git\n"));               // subtree: never listed
        writeRaw(small.filePath(QStringLiteral("ok.txt")), QByteArray("ok\n"));
        writeRaw(small.filePath(QStringLiteral("sub/d.txt")), QByteArray("d\n"));

        SearchSession session;
        session.setMaxFileSize(12); // the cap belongs to READING: a list
                                    // walk judges NO sizes (pinned below
                                    // by big.txt riding with skipped==0)
        QSignalSpy listSpy(&session, &SearchSession::fileListed);
        QSignalSpy skipSpy(&session, &SearchSession::fileSkipped);
        QSignalSpy matchSpy(&session, &SearchSession::fileMatched);
        QSignalSpy doneSpy(&session, &SearchSession::finished);
        session.startList(small.path());
        QVERIFY(waitFinished(doneSpy));

        QStringList names; // project-relative, walk order
        for (const QList<QVariant>& a : listSpy) {
            names << QDir(small.path()).relativeFilePath(a.at(0).toString());
        }
        QStringList want;
        want << QStringLiteral("big.txt") << QStringLiteral("ok.txt")
             << QStringLiteral("bin/data.bin") << QStringLiteral("sub/d.txt");
        QCOMPARE(names, want);
        // Names only: no content decisions happened — the binary and
        // the over-cap file were LISTED, the size cap judged NOTHING,
        // and skipped/matched are honest zeros (the decisions belong to
        // the open, not to the list).
        QCOMPARE(skipSpy.count(), 0);
        QCOMPARE(matchSpy.count(), 0);
        QCOMPARE(doneSpy.at(0).at(0).toInt(), 4); // scanned = LISTED
        QCOMPARE(doneSpy.at(0).at(1).toInt(), 0);
        QCOMPARE(doneSpy.at(0).at(2).toInt(), 0);
        QCOMPARE(doneSpy.at(0).at(3).toInt(), 0);
        QVERIFY(doneSpy.at(0).at(4).toString().isEmpty());

        { // the budget cap binds on the LIST face too:
            SearchSession s2;
            s2.setMaxFiles(2);
            QSignalSpy listSpy2(&s2, &SearchSession::fileListed);
            QSignalSpy doneSpy2(&s2, &SearchSession::finished);
            s2.startList(small.path());
            QVERIFY(waitFinished(doneSpy2));
            QCOMPARE(listSpy2.count(), 2); // PARTIAL list, never retracted
            QVERIFY(doneSpy2.at(0).at(4).toString().startsWith(
                QStringLiteral("budget:")));
        }
        { // the root refusal lands SYNCHRONOUSLY inside startList (the
          // same connect-before-start law — no qWait between):
            SearchSession s3;
            QSignalSpy listSpy3(&s3, &SearchSession::fileListed);
            QSignalSpy doneSpy3(&s3, &SearchSession::finished);
            s3.startList(small.filePath(QStringLiteral("no-such-dir")));
            QCOMPARE(doneSpy3.count(), 1); // SYNCHRONOUS, checked the
                                           // very next statement
            QCOMPARE(listSpy3.count(), 0);
            QVERIFY(doneSpy3.at(0).at(4).toString().startsWith(
                QStringLiteral("root: not a directory:")));
        }
        { // reentry + cancel ride the LIST face the same way: a start
          // while running is IGNORED (one finished total), and the
          // cancel stop is the honest "cancelled" with the partial list
          // kept.
            SearchSession s4;
            QSignalSpy doneSpy4(&s4, &SearchSession::finished);
            QSignalSpy listSpy4(&s4, &SearchSession::fileListed);
            s4.startList(m_dir.path());
            SearchQuery q;
            q.text = QStringLiteral("sum");
            s4.start(m_dir.path(), q); // IGNORED (running) — emits nothing
            QVERIFY(s4.running());
            s4.cancel();
            QVERIFY(waitFinished(doneSpy4));
            QCOMPARE(doneSpy4.count(), 1);
            QVERIFY(doneSpy4.at(0).at(4).toString() == QLatin1String("cancelled"));
            // cancel lands between pumps: at most the first chunk got
            // listed before the stop (kChunkFiles per tick) — partial
            // counts as scanned, never retracted:
            QVERIFY(listSpy4.count() <= SearchSession::kChunkFiles);
            QCOMPARE(doneSpy4.at(0).at(0).toInt(), listSpy4.count());
        }
        { // the BOTH-WAYS audit of the widening: the QUERY walk (start)
          // never emits fileListed — the I1 signal contract is UNCHANGED
          // on the query face.
            SearchSession s5;
            QSignalSpy listSpy5(&s5, &SearchSession::fileListed);
            QSignalSpy doneSpy5(&s5, &SearchSession::finished);
            SearchQuery q;
            q.text = QStringLiteral("sum");
            s5.start(small.path(), q);
            QVERIFY(waitFinished(doneSpy5));
            QCOMPARE(listSpy5.count(), 0);
            QVERIFY(doneSpy5.at(0).at(1).toInt() >= 1); // the QUERY face
                                                        // still works
        }
    }

    void walkUnskippedAndDepthFaces()
    {
        { // hidden VISIBLE + .git un-skipped:
            SearchSession session;
            session.setSkipHidden(false);
            session.setSkipDirNames({});
            QSignalSpy doneSpy(&session, &SearchSession::finished);
            SearchQuery q;
            q.text = QStringLiteral("sum");
            session.start(m_dir.path(), q);
            QVERIFY(waitFinished(doneSpy));
            const int scanned = doneSpy.at(0).at(0).toInt();
            // The default-contract walk saw m_refTexts.size(); this one
            // MUST see strictly more (the hidden file, the hidden dir,
            // the .git file, the gitdir files all ride in):
            QVERIFY2(scanned > m_refTexts.size(),
                     qPrintable(QStringLiteral("scanned %1 vs %2")
                                    .arg(scanned).arg(m_refTexts.size())));
        }
        { // depth 0 = the ROOT dir's own files only:
            SearchSession session;
            session.setMaxDepth(0);
            QSignalSpy doneSpy(&session, &SearchSession::finished);
            SearchQuery q;
            q.text = QStringLiteral("sum");
            session.start(m_dir.path(), q);
            QVERIFY(waitFinished(doneSpy));
            QDir root(m_dir.path());
            const int rootFiles = root.entryList(QDir::Files | QDir::NoDotAndDotDot).size();
            QCOMPARE(doneSpy.at(0).at(0).toInt(), rootFiles);
        }
    }

    void budgetCapFace()
    {
        SearchSession session;
        session.setMaxFiles(5);
        QSignalSpy doneSpy(&session, &SearchSession::finished);
        SearchQuery q;
        q.text = QStringLiteral("sum");
        session.start(m_dir.path(), q);
        QVERIFY(waitFinished(doneSpy));
        QVERIFY(doneSpy.at(0).at(4).toString().startsWith(QStringLiteral("budget:")));
        QCOMPARE(doneSpy.at(0).at(0).toInt(), 5);
    }

    void cancelStopsHonestly()
    {
        // A wide tree so the walk spans several chunks; cancel rides the
        // FIRST fileMatched. The stop face: reason "cancelled", counts
        // already SENT stay sent, and it is NEVER the empty (completed)
        // face.
        QTemporaryDir big;
        QVERIFY(big.isValid());
        for (int i = 0; i < 120; ++i) {
            writeRaw(big.filePath(QStringLiteral("f%1.txt").arg(i, 3, 10, QLatin1Char('0'))),
                     ("sum line\n" + QString("more lines here\n").repeated(3)).toUtf8());
        }
        SearchSession session;
        QObject sink;
        int seen = 0;
        bool cancelling = false;
        connect(&session, &SearchSession::fileMatched, &sink, [&]() {
            ++seen;
            if (!cancelling) {
                cancelling = true;
                session.cancel();
            }
        });
        QSignalSpy doneSpy(&session, &SearchSession::finished);
        SearchQuery q;
        q.text = QStringLiteral("sum");
        session.start(big.path(), q);
        QVERIFY(waitFinished(doneSpy));
        QCOMPARE(doneSpy.at(0).at(4).toString(), QStringLiteral("cancelled"));
        QVERIFY(seen >= 1);
        QVERIFY(doneSpy.at(0).at(0).toInt() < 120); // never completed
        QCOMPARE(doneSpy.at(0).at(1).toInt(), seen); // the partials are
                                                     // not retracted
    }

    void crlfPreviewLaw()
    {
        // Columns speak the RAW '\n'-slice (the CR stays a unit at the
        // line's end); the PREVIEW drops the CR (print-only courtesy).
        SearchQuery q;
        q.text = QStringLiteral("sum");
        const QList<LineMatch> hits = Engine::scanText(QStringLiteral("a\r\nsum\r\n"), q);
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits.at(0).line, 1);
        QCOMPARE(hits.at(0).spans.at(0).startCol, 0);
        QCOMPARE(hits.at(0).spans.at(0).endCol, 3);
        QCOMPARE(hits.at(0).lineText, QStringLiteral("sum"));
        // an empty text is ONE empty line (the core's >=1 law) — finding
        // nothing there is an honest empty, NOT a refusal:
        QString refusal;
        QVERIFY(Engine::scanText(QString(), q, &refusal).isEmpty());
        QVERIFY(refusal.isEmpty());
    }

    void previewCapFace()
    {
        QString line = QStringLiteral("x ");
        line += QStringLiteral("y").repeated(kPreviewCap); // long tail…
        line += QStringLiteral(" sum");                    // …with the hit AFTER
        SearchQuery q;
        q.text = QStringLiteral("sum");
        const QList<LineMatch> hits = Engine::scanText(line, q);
        QCOMPARE(hits.size(), 1);
        // the span rides the RAW coordinates ("x " + cap y's + " sum":
        // 2 + cap + the space = cap + 3):
        QCOMPARE(hits.at(0).spans.at(0).startCol, kPreviewCap + 3);
        // …and the preview is cap units + the ellipsis:
        QCOMPARE(hits.at(0).lineText.size(), kPreviewCap + 1);
        QVERIFY(hits.at(0).lineText.endsWith(QChar(0x2026)));
        QCOMPARE(hits.at(0).lineText.left(kPreviewCap), line.left(kPreviewCap));
    }

    void emptyAndMissingFaces()
    {
        writeRaw(m_dir.filePath(QStringLiteral("zero.txt")), QByteArray());
        SearchQuery q;
        q.text = QStringLiteral("sum");
        const FileScan fs = Engine::scanFile(m_dir.filePath(QStringLiteral("zero.txt")), q);
        QCOMPARE(int(fs.skip), int(SkipReason::None)); // a 0-byte file is
                                                       // scanned as empty
                                                       // text, not skipped
        QVERIFY(fs.match.lines.isEmpty());
        const FileScan missing = Engine::scanFile(m_dir.filePath(QStringLiteral("nope.txt")), q);
        QCOMPARE(int(missing.skip), int(SkipReason::IoError));
    }

    void cleanupTestCase() { m_dir.remove(); }

private:
    QTemporaryDir m_dir;
    QHash<QString, QString> m_refTexts; // absolute path -> decoded text:
                                        // the reference's view of the
                                        // corpus under the DEFAULT
                                        // session contract
};

QTEST_GUILESS_MAIN(TestSearch)
#include "tst_search.moc"
