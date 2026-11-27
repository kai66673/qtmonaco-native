// tools/bench_search.cpp — the GATE 4 car I1 perf record (the bench_model
// precedent): standalone g++, NOT a CMake target. Needs libqmn_search.a
// already built by the RELEASE recipe (the bench-hygiene law: hand-run
// archives are a different build — build through CMake, never -c yourself),
// and the Qt6 paths below are Arch Linux paths (adjust elsewhere).
//
//   g++ -fPIC -O3 -std=c++20 tools/bench_search.cpp \
//       build/src/search/libqmn_search.a -Isrc \
//       -isystem /usr/include/qt6/QtCore -isystem /usr/include/qt6 \
//       -lQt6Core -o bench_search && ./bench_search <root-dir>
//
// -fPIC IS LOAD-BEARING here (a drift from the bare bench_model recipe,
// found at the I1 close): THIS TU destructs inline QByteArray temporaries
// (the readAll buffers), and the resulting direct PC32 reference to Qt's
// PROTECTED QByteArray::_empty data gets a copy relocation the linker
// REJECTS in a PIE link ("non-copyable protected symbol"). bench_model's
// TU happens not to reference that data inline, so its bare recipe links;
// the search bench's does not ride on luck.
//
// WHAT IT TIMES (a MEASURED record for the DoD, never a ctest pin — the
// AGENTS vacuous-pass law for wall-clock): THREE interleaved full walks
// of SearchSession over the given root (the event-loop-paced walk face —
// QCoreApplication is QtCore, this stays a guiless single process), plus
// the PURE scanText face (no I/O, the in-buffer path) over one big
// concatenated buffer of every walked text file. The corpus is whatever
// the root holds — the repo's OWN tree is the intended dogfood corpus
// (the recorded COUNT is a measurement, not a pin: trees drift).
//
// The engine's own caps must be OFF for the walk (maxFiles -1) or the
// budget face truncates the walk mid-run and the number measures the cap.

#include "search/SearchEngine.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <functional>

using namespace qmn::search;

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: bench_search <root-dir>\n");
        return 1;
    }
    const QString root = QFileInfo(argv[1]).absoluteFilePath();
    if (!QFileInfo(root).isDir()) {
        std::fprintf(stderr, "bench_search: not a directory: %s\n",
                     root.toLocal8Bit().constData());
        return 1;
    }

    SearchSession session;
    session.setMaxFiles(-1); // the caps OFF: this measures the walk, not
                             // the budget face
    session.setMaxFileSize(Engine::kDefaultMaxFileSize);

    SearchQuery query;
    query.text = QStringLiteral("const"); // a REAL token density on a
                                          // code tree (fixed-string path:
                                          // the perf face, never a regex)
    query.caseSensitive = true;

    // ---- the walk face: three interleaved full runs --------------------
    // ONE finished() connection for the whole bench: it fills the holder
    // and quits the loop; a NULL holder means "refusal face seen, walk
    // did not run" (the refusal also quits — the bench refuses LOUD, it
    // never times an empty run).
    struct WalkResult { int scanned, matchedFiles, matches, skipped; QString stopReason; };
    WalkResult holder;
    bool gotFinished = false;
    QObject::connect(&session, &SearchSession::finished,
                     [&](int s, int f, int m, int k, const QString& reason) {
                         holder = {s, f, m, k, reason};
                         gotFinished = true;
                         app.quit();
                     });

    QElapsedTimer clock;
    struct TimedWalk { qint64 ms; WalkResult r; };
    TimedWalk runs[3];
    for (int run = 0; run < 3; ++run) {
        gotFinished = false;
        holder = {};
        QTimer::singleShot(0, [&] { session.start(root, query); });
        clock.start();
        app.exec(); // finished() is the ONLY quit source
        runs[run] = {clock.elapsed(), holder};
        if (!gotFinished) {
            std::fprintf(stderr, "bench_search: run %d never finished\n", run);
            return 1;
        }
        if (!holder.stopReason.isEmpty()) {
            std::fprintf(stderr, "bench_search: run %d STOPPED: %s — the caps "
                                 "must be OFF for a perf record\n", run,
                         holder.stopReason.toLocal8Bit().constData());
            return 1;
        }
    }

    for (int run = 0; run < 3; ++run)
        std::printf("walk %d: %lld ms  scanned=%d matchedFiles=%d matches=%d skipped=%d\n",
                    run, static_cast<long long>(runs[run].ms),
                    runs[run].r.scanned, runs[run].r.matchedFiles,
                    runs[run].r.matches, runs[run].r.skipped);
    const qint64 bestMs = std::min({runs[0].ms, runs[1].ms, runs[2].ms});
    std::printf("walk BEST of 3: %lld ms for %d files scanned (%lld us/file)\n",
                static_cast<long long>(bestMs), runs[0].r.scanned,
                runs[0].r.scanned
                    ? static_cast<long long>(bestMs) * 1000 / runs[0].r.scanned
                    : 0LL);

    // ---- the pure scanText face: one big buffer, no I/O -----------------
    // The SAME corpus the walk saw (minus skips), concatenated in the
    // engine's own skip-contract shape — what the in-buffer face gets.
    QString big;
    std::function<void(const QString&)> collect = [&](const QString& dir) {
        QDir d(dir);
        for (const QFileInfo& e : d.entryInfoList(QDir::Dirs | QDir::Files
                                                  | QDir::NoDotAndDotDot
                                                  | QDir::Readable)) {
            if (e.fileName() == QLatin1String(".git"))
                continue;
            if (e.fileName().startsWith(QLatin1Char('.')))
                continue;
            if (e.isDir())
                collect(e.absoluteFilePath());
            else if (e.size() <= Engine::kDefaultMaxFileSize) {
                QFile f(e.absoluteFilePath());
                if (f.open(QIODevice::ReadOnly)) {
                    const QByteArray bytes = f.readAll();
                    if (!bytes.contains('\0'))
                        big += QString::fromUtf8(bytes);
                }
            }
        }
    };
    collect(root);
    QString refusal;
    clock.start();
    const QList<LineMatch> hits = Engine::scanText(big, query, &refusal);
    const qint64 pureMs = clock.elapsed();
    int spans = 0;
    for (const LineMatch& lm : hits)
        spans += lm.spans.size();
    std::printf("scanText face: %.1f MB (UTF-16) buffer, %lld ms, "
                "%d matched lines, %d spans, refusal=[%s]\n",
                big.size() * 2.0 / (1024.0 * 1024.0), static_cast<long long>(pureMs),
                hits.size(), spans, refusal.toLocal8Bit().constData());
    return 0;
}
