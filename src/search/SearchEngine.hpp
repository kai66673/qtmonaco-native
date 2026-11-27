#pragma once

// The search engine (GATE 4 car I1, PLAN): the project-wide + in-buffer
// text search behind the IDE's Find-in-Files panel.
//
// Layer rule (the core/diff class): QtCore ONLY — no QtGui, no QtWidgets,
// no view includes. The engine is headless, fuzzable, benchmarkable, and
// its ctest entry (tst_search) is guiless like textbuffer/diff. The app
// (qmn-ide-demo) links qmn::search directly — it is a REAL export set
// (unlike internal_diff/internal_tree_sitter: the app IS the consumer,
// nothing in the shipped view links it).
//
// Wrappability from birth (AGENTS: public doors speak plain Qt types;
// car E's law: a Python-facing callback route rides a SIGNAL, never a
// std::function): every record below is a plain-Qt struct, every result
// route of SearchSession is a signal. A future wheel crossing is then a
// three-copies entry like any other, not a surgery.
//
// Offsets: every column/offset here is the repo-wide law — 0-based
// UTF-16 code units, [start, end) — exactly like CodeEditor and LSP
// positions. Files on disk are read as UTF-8 and live internally as
// UTF-16 (QString); a match after an astral character must therefore
// speak UTF-16 UNITS, not bytes (tst_search pins this — the byte-offset
// slip class the tree-sitter 2× record warned about).

#include <QMetaType>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace qmn::search {

// What to look for. `isRegex` switches the WHOLE face: the fixed-string
// path never compiles a QRegularExpression at all (the perf path), the
// regex face compiles once per scan and rides UseUnicodePropertiesOption
// (\w \b are Unicode-aware classes there).
struct SearchQuery {
    QString text;
    bool caseSensitive = false;
    bool wholeWord = false;
    bool isRegex = false;
};

// One [startCol, endCol) hit INSIDE one line, 0-based UTF-16 units.
struct MatchSpan {
    int startCol = 0;
    int endCol = 0;
};

// All hits on one 0-based `line`, plus a PREVIEW of the line's text
// (truncated at kPreviewCap with a trailing '…' — a preview for the
// panel, NOT the offset source: spans refer to the RAW line, the
// between-'\n' slice; the line's own trailing '\r' of a CRLF file stays
// part of the raw slice for column purposes and is STRIPPED from the
// preview, which is a print-only courtesy).
struct LineMatch {
    int line = 0;
    QList<MatchSpan> spans;
    QString lineText;
};

// All matched lines of one file, `filePath` absolute and lexically
// cleaned (QFileInfo::absoluteFilePath — no symlink resolution,
// deterministic). Lines are ordered, spans per line are ordered.
struct FileMatch {
    QString filePath;
    QList<LineMatch> lines;
};

// A walk never reads an unreasonable file (the walk CONTRACT, all three
// faces pinned by tst_search): a NUL inside the first 8 KiB is BINARY,
// a file larger than the size cap is TOO_LARGE (stat first, no open),
// an open/read failure is IO_ERROR. A file with a NUL only DEEPER than
// the sniff window is searched as text (the documented heuristic — the
// sniff is a window, not the whole truth).
enum class SkipReason { None, Binary, TooLarge, IoError };

struct FileScan {
    SkipReason skip = SkipReason::None; // None = the file WAS scanned
                                        // (a None + empty match = a clean
                                        // miss, NOT a skip)
    FileMatch match;
    QString ioError; // only meaningful with IoError (the Qt read error)
};

// Preview truncation cap in UTF-16 units (tst pins the face).
inline constexpr int kPreviewCap = 240;

// The pure faces — no state, no event loop, directly callable (the
// property tests call these; the session is a thin walk over them).
class Engine {
public:
    // Empty = the query is usable. Non-empty = the LOUD refusal text
    // (empty text; a regex that does not compile — the QRegularExpression
    // patternError rides in the text). A refused query scans NOTHING;
    // an invalid regex is NEVER an empty-looking success.
    static QString queryRefusal(const SearchQuery& query);

    // The in-buffer face: every matched line of `text` (the '\n'-split
    // law — an empty `text` is ONE empty line, the core's lineCount>=1
    // sibling law). Zero-length regex matches do not count as hits
    // (documented; they would loop the iterator for no panel value).
    // `refusalOut` (when given) receives queryRefusal() and the result
    // is empty on a refusal.
    static QList<LineMatch> scanText(const QString& text, const SearchQuery& query,
                                     QString* refusalOut = nullptr);

    // The single-file face (stat the size cap first, open, sniff, decode
    // UTF-8, scan). maxBytes <= 0 falls back to the session default cap.
    static FileScan scanFile(const QString& filePath, const SearchQuery& query,
                             qint64 maxBytes = -1);

    static constexpr qint64 kDefaultMaxFileSize = 5LL * 1024 * 1024;
};

// The project walk — the QObject face the panel drives. The walk is
// INCREMENTAL and event-loop-paced (a chunk of files per zero-timer
// tick), so a big tree never freezes the UI, cancel() lands between
// chunks, and every finish reports itself HONESTLY: the stopReason of
// finished() is empty ONLY when the walk COMPLETED; "query: …",
// "root: …", "cancelled" and "budget: …" each say what actually
// happened, and a cancelled walk never retracts the fileMatched signals
// already sent (and never reports its stop as a clean zero-hit success).
//
// CONNECT-BEFORE-START: the refusal faces (bad query, bad root) fire
// finished() SYNCHRONOUSLY inside start() — a capture connected AFTER
// the start() call misses the reason (car E's connect-order law, the
// same mechanism, written at the door that has it).
class SearchSession : public QObject {
    Q_OBJECT
public:
    explicit SearchSession(QObject* parent = nullptr);
    ~SearchSession() override;

    // Walk knobs: set BEFORE start() (a mid-walk set is a documented
    // no-op — the session captured its knobs at start).
    void setMaxFileSize(qint64 bytes);   // default Engine::kDefaultMaxFileSize
    void setMaxFiles(int files);         // default kDefaultMaxFiles; -1 = no cap
    void setSkipHidden(bool skip);       // default true (dot-prefixed entries)
    void setSkipDirNames(const QStringList& names); // default {".git"} — a
                                                 // dir NAME match at ANY depth
                                                 // skips the whole subtree
    void setMaxDepth(int depth);         // default -1 = unlimited; 0 = the
                                         // root dir only, 1 = +one level …
    qint64 maxFileSize() const;
    int maxFiles() const;
    bool running() const; // a running() start() is IGNORED (the panel
                          // cancels first; an ignored start emits nothing —
                          // pinned, no second session can leak into the
                          // same signals)

    static constexpr int kDefaultMaxFiles = 200'000;
    static constexpr int kChunkFiles = 8; // files per pump tick (the
                                          // event-loop pace; tests drive
                                          // small trees in one tick)

    // Refusal face (finished, stopReason non-empty, scanned 0) or the
    // walk. Synchronous refusal emits land inside THIS call.
    void start(const QString& rootPath, const SearchQuery& query);
    void cancel();

signals:
    void fileMatched(const qmn::search::FileMatch& match);
    void fileSkipped(const QString& filePath, qmn::search::SkipReason reason);
    void progress(int filesScanned);
    void finished(int filesScanned, int filesWithMatches, int totalMatches,
                  int skipped, const QString& stopReason);
    // stopReason faces (frozen prefixes — the panel and tst read THESE,
    // not a vibe): "" = completed; "query: …" = refused query;
    // "root: …" = not a directory / unreadable root; "cancelled" = the
    // cancel leg; "budget: …" = the maxFiles cap stopped the walk.

private:
    struct Walk; // the in-flight walk state (impl-private)
    void pumpChunk();
    void finish(const QString& stopReason);

    std::unique_ptr<Walk> walk_;
    QTimer pump_;
    // Knobs (duplicated into Walk at start):
    qint64 maxFileSize_ = Engine::kDefaultMaxFileSize;
    int maxFiles_ = kDefaultMaxFiles;
    bool skipHidden_ = true;
    QStringList skipDirNames_ { QStringLiteral(".git") };
    int maxDepth_ = -1;
};

} // namespace qmn::search

Q_DECLARE_METATYPE(qmn::search::SearchQuery)
Q_DECLARE_METATYPE(qmn::search::MatchSpan)
Q_DECLARE_METATYPE(qmn::search::LineMatch)
Q_DECLARE_METATYPE(qmn::search::FileMatch)
Q_DECLARE_METATYPE(qmn::search::SkipReason)
