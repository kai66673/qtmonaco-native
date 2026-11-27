#include "search/SearchEngine.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQueue>
#include <QRegularExpression>

#include <algorithm>

// QtCore ONLY (the layer rule at the header's top): no QtGui/QtWidgets
// include here, ever — the walk is headless by contract.

namespace qmn::search {

namespace {

// The word-boundary class for the fixed-string wholeWord face. With
// UseUnicodePropertiesOption on the regex face, (?<!\w)/(?!\w) agrees
// with this by construction (letters, numbers, underscore) — the two
// faces must answer the SAME boundary question, tst_search pins the
// face-pair on one corpus.
bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

bool boundaryOk(const QString& line, int start, int end)
{
    if (start > 0 && isWordChar(line.at(start - 1)))
        return false;
    if (end < line.size() && isWordChar(line.at(end)))
        return false;
    return true;
}

QRegularExpression buildRegex(const SearchQuery& query)
{
    // The regex face compiles ONCE per scan (callers check isValid via
    // queryRefusal first; the wholeWord wrap keeps a refusal at the same
    // gate — a wrapped invalid pattern stays invalid in the same way).
    // UseUnicodePropertiesOption is load-bearing: it makes \w Unicode-
    // aware, matching the fixed-string face's QChar-based boundaries.
    QString pattern = query.wholeWord
        ? QStringLiteral("(?<!\\w)(?:%1)(?!\\w)").arg(query.text)
        : query.text;
    QRegularExpression::PatternOptions options
        = QRegularExpression::UseUnicodePropertiesOption;
    if (!query.caseSensitive)
        options |= QRegularExpression::CaseInsensitiveOption;
    return QRegularExpression(pattern, options);
}

QList<LineMatch> scanLine(const QString& line, const SearchQuery& query,
                          const QRegularExpression* re)
{
    QList<LineMatch> out; // AT MOST one LineMatch per line (spans ride in)
    QList<MatchSpan> spans;

    if (re) {
        QRegularExpressionMatchIterator it = re->globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            // Zero-length matches are NOT hits (documented law): they
            // would loop for panel value nothing and the naive reference
            // (fixed-string semantics) never produces them.
            const int len = int(m.capturedLength(0));
            if (len <= 0)
                continue;
            const int start = int(m.capturedStart(0));
            spans.append({start, start + len});
        }
    } else {
        int from = 0;
        const Qt::CaseSensitivity cs
            = query.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
        for (;;) {
            const int idx = line.indexOf(query.text, from, cs);
            if (idx < 0)
                break;
            const int end = idx + query.text.size();
            // REJECTED occurrences SLIDE (idx+1), accepted ones consume
            // (end) — the non-overlap law binds ACCEPTED matches only,
            // and this distinction is load-bearing: jumping past a
            // rejected window can skip a nested occurrence whose own
            // boundaries are clean (tst_search's "a.a" / "xa.a.a"
            // tripwire walks exactly this face; the naive reference
            // agrees with the slide, never with the jump).
            if (!query.wholeWord || boundaryOk(line, idx, end)) {
                spans.append({idx, end});
                from = end;
            } else {
                from = idx + 1;
            }
        }
    }

    if (spans.isEmpty())
        return out;

    LineMatch lm;
    lm.line = 0; // the caller stamps the number (this helper is line-local)
    lm.spans = spans;
    QString preview = line;
    if (preview.endsWith(QLatin1Char('\r')))
        preview.chop(1); // the print-only courtesy (header: columns speak
                         // the RAW slice, the preview drops the CR)
    if (preview.size() > kPreviewCap)
        preview = preview.left(kPreviewCap) + QChar(0x2026); // cap + '…'
    lm.lineText = preview;
    return {lm};
}

} // namespace

QString Engine::queryRefusal(const SearchQuery& query)
{
    if (query.text.isEmpty())
        return QStringLiteral("query: empty text");
    if (query.isRegex) {
        const QRegularExpression re = buildRegex(query);
        if (!re.isValid())
            return QStringLiteral("query: invalid regex: %1 (at offset %2)")
                .arg(re.errorString())
                .arg(re.patternErrorOffset());
    }
    return QString();
}

QList<LineMatch> Engine::scanText(const QString& text, const SearchQuery& query,
                                  QString* refusalOut)
{
    const QString refusal = queryRefusal(query);
    if (!refusal.isEmpty()) {
        if (refusalOut)
            *refusalOut = refusal;
        return {};
    }
    if (refusalOut)
        refusalOut->clear();

    // The regex object is built once for the WHOLE text (per-scan cost,
    // never per-line).
    QRegularExpression re;
    const QRegularExpression* rePtr = nullptr;
    if (query.isRegex) {
        re = buildRegex(query);
        rePtr = &re;
    }

    QList<LineMatch> out;
    int pos = 0;
    int lineNo = 0;
    // The '\n'-split law, streamed (no whole-file QStringList): an empty
    // text walks the ONE empty line (the core's lineCount>=1 sibling),
    // finding nothing there — an honest empty result, never a refusal.
    for (;;) {
        const int nl = text.indexOf(QLatin1Char('\n'), pos);
        const QString line = (nl < 0) ? text.mid(pos) : text.mid(pos, nl - pos);
        QList<LineMatch> hits = scanLine(line, query, rePtr);
        for (LineMatch& hm : hits) {
            hm.line = lineNo;
            out.append(std::move(hm));
        }
        if (nl < 0)
            break;
        pos = nl + 1;
        ++lineNo;
    }
    return out;
}

FileScan Engine::scanFile(const QString& filePath, const SearchQuery& query,
                          qint64 maxBytes)
{
    const qint64 cap = (maxBytes > 0) ? maxBytes : kDefaultMaxFileSize;

    const QString refusal = queryRefusal(query);
    if (!refusal.isEmpty()) // a refused query scans NOTHING, file face
        return {SkipReason::None, {}, {}}; // None + empty match = a clean
                                           // miss (no scan happened; the
                                           // caller-level refusal is the
                                           // loud face, tst pins both)

    FileScan fs;
    const QFileInfo fi(filePath);
    if (!fi.exists()) {
        fs.skip = SkipReason::IoError;
        fs.ioError = QStringLiteral("file does not exist");
        return fs;
    }
    if (fi.size() > cap) {
        fs.skip = SkipReason::TooLarge; // stat FIRST: an oversized file
                                        // is never opened
        return fs;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        fs.skip = SkipReason::IoError;
        fs.ioError = file.errorString();
        return fs;
    }
    // One byte past the cap catches a file that GREW between the stat
    // and the read (the cap stays a hard wall, the walk never buffers
    // an unbounded read).
    const QByteArray bytes = file.read(cap + 1);
    if (file.size() > cap) {
        fs.skip = SkipReason::TooLarge;
        return fs;
    }
    if (bytes.left(8192).contains('\0')) {
        fs.skip = SkipReason::Binary; // the sniff WINDOW (header: a NUL
                                      // deeper than 8 KiB rides on as text)
        return fs;
    }

    fs.match = {fi.absoluteFilePath(),
                scanText(QString::fromUtf8(bytes), query)};
    return fs; // skip NONE — a None + empty match.lines is a clean miss
}

// ------------------------------------------------------------- session

struct SearchSession::Walk {
    // Knobs captured at start (a mid-walk setter is a no-op — the walk
    // alone is the truth mid-run).
    SearchQuery query;
    qint64 maxFileSize = Engine::kDefaultMaxFileSize;
    int maxFiles = kDefaultMaxFiles;
    bool skipHidden = true;
    QStringList skipDirNames;
    int maxDepth = -1;

    struct DirEntry { QString path; int depth; };
    QQueue<DirEntry> dirs;
    QQueue<QString> files;
    int scanned = 0;
    int matchedFiles = 0;
    int totalMatches = 0;
    int skipped = 0;
    bool cancelRequested = false;
};

SearchSession::SearchSession(QObject* parent)
    : QObject(parent)
{
    pump_.setSingleShot(true);
    connect(&pump_, &QTimer::timeout, this, &SearchSession::pumpChunk);
}

SearchSession::~SearchSession() = default;

void SearchSession::setMaxFileSize(qint64 bytes)
{
    if (!running())
        maxFileSize_ = bytes;
}

void SearchSession::setMaxFiles(int files)
{
    if (!running())
        maxFiles_ = files;
}

void SearchSession::setSkipHidden(bool skip)
{
    if (!running())
        skipHidden_ = skip;
}

void SearchSession::setSkipDirNames(const QStringList& names)
{
    if (!running())
        skipDirNames_ = names;
}

void SearchSession::setMaxDepth(int depth)
{
    if (!running())
        maxDepth_ = depth;
}

qint64 SearchSession::maxFileSize() const { return maxFileSize_; }
int SearchSession::maxFiles() const { return maxFiles_; }
bool SearchSession::running() const { return walk_ != nullptr; }

void SearchSession::start(const QString& rootPath, const SearchQuery& query)
{
    if (running())
        return; // an ignored start emits NOTHING (pinned: no second
                // session leaks into the same signals)

    const QString refusal = Engine::queryRefusal(query);
    if (!refusal.isEmpty()) {
        // The loud face lands SYNCHRONOUSLY (the header's
        // connect-before-start law — car E's mechanism, this door).
        emit finished(0, 0, 0, 0, refusal);
        return;
    }

    const QFileInfo rootInfo(rootPath);
    if (!rootInfo.isDir()) {
        emit finished(0, 0, 0, 0,
                      QStringLiteral("root: not a directory: %1").arg(rootPath));
        return;
    }
    if (!rootInfo.isReadable()) {
        emit finished(0, 0, 0, 0,
                      QStringLiteral("root: not readable: %1")
                          .arg(rootInfo.absoluteFilePath()));
        return;
    }

    walk_ = std::make_unique<Walk>();
    walk_->query = query;
    walk_->maxFileSize = maxFileSize_;
    walk_->maxFiles = maxFiles_;
    walk_->skipHidden = skipHidden_;
    walk_->skipDirNames = skipDirNames_;
    walk_->maxDepth = maxDepth_;
    walk_->dirs.push_back({rootInfo.absoluteFilePath(), 0});
    pump_.start(0);
}

void SearchSession::cancel()
{
    if (walk_)
        walk_->cancelRequested = true;
}

void SearchSession::pumpChunk()
{
    if (!walk_)
        return;
    Walk* w = walk_.get();

    if (w->cancelRequested) {
        finish(QStringLiteral("cancelled")); // the honest stop face: the
                                             // fileMatched signals already
                                             // sent stay sent, the counts
                                             // ride finished()
        return;
    }

    for (int budget = kChunkFiles; budget > 0; --budget) {
        if (w->files.isEmpty()) {
            if (w->dirs.isEmpty()) {
                finish(QString()); // the walk COMPLETED
                return;
            }
            // Expand one directory at a time (the queue keeps the walk
            // incremental; per-directory sorting pins a deterministic
            // order — the sort is codepoint-order, locale-INDEPENDENT
            // (QDir's localeAware Name sort is not, so the sort is here)).
            const Walk::DirEntry dir = w->dirs.dequeue();
            QDir::Filters filters = QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot
                                    | QDir::Readable;
            if (!w->skipHidden)
                filters |= QDir::Hidden; // hidden comes ONLY with the flag
            QFileInfoList entries = QDir(dir.path).entryInfoList(filters);
            std::sort(entries.begin(), entries.end(),
                      [](const QFileInfo& a, const QFileInfo& b) {
                          return a.fileName() < b.fileName();
                      });
            for (const QFileInfo& e : entries) {
                if (e.isDir()) {
                    if (w->skipDirNames.contains(e.fileName()))
                        continue; // the whole SUBTREE drops (the .git law)
                    if (w->maxDepth >= 0 && dir.depth + 1 > w->maxDepth)
                        continue;
                    w->dirs.push_back({e.absoluteFilePath(), dir.depth + 1});
                } else {
                    w->files.enqueue(e.absoluteFilePath());
                }
            }
            ++budget; // the expansion consumed no file slot — retry the
                      // loop head to pick files from the fresh queue
            continue;
        }

        if (w->maxFiles >= 0 && w->scanned >= w->maxFiles) {
            finish(QStringLiteral("budget: file count cap %1 hit")
                       .arg(w->maxFiles));
            return;
        }

        const QString path = w->files.dequeue();
        const FileScan fs = Engine::scanFile(path, w->query, w->maxFileSize);
        ++w->scanned;
        switch (fs.skip) {
        case SkipReason::Binary:
        case SkipReason::TooLarge:
        case SkipReason::IoError:
            ++w->skipped;
            emit fileSkipped(path, fs.skip);
            break;
        case SkipReason::None:
            if (!fs.match.lines.isEmpty()) {
                ++w->matchedFiles;
                for (const LineMatch& lm : fs.match.lines)
                    w->totalMatches += lm.spans.size();
                emit fileMatched(fs.match);
            }
            break;
        }
    }

    emit progress(w->scanned);
    pump_.start(0);
}

void SearchSession::finish(const QString& stopReason)
{
    if (!walk_)
        return;
    Walk* w = walk_.get();
    const int scanned = w->scanned;
    const int matchedFiles = w->matchedFiles;
    const int totalMatches = w->totalMatches;
    const int skipped = w->skipped;
    walk_.reset(); // RETIRE the walk BEFORE the emit: a handler may
                   // restart the session right inside this signal, and a
                   // live walk_ there would make running() lie.
    emit finished(scanned, matchedFiles, totalMatches, skipped, stopReason);
}

} // namespace qmn::search
