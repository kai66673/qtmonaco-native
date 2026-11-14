// M4 highlight-engine micro-benchmark (PLAN.md Phase D2 threading
// sub-decision: GUI-thread reparse per keystroke vs background thread -
// decide by numbers, not taste).
//
// Standalone g++ build, NOT part of CMake (same style as bench_model.cpp).
// Needs libqmn_view.a, libqmn_core.a AND libqmn_ts.a (qmn_ts is PRIVATE to
// qmn_view, so standalone consumers must link it explicitly), plus Qt:
//
//   g++ -O3 -std=c++20 tools/bench_highlight.cpp -Isrc \
//       build/src/view/libqmn_view.a build/src/core/libqmn_core.a \
//       build/third_party/tree-sitter/libqmn_ts.a \
//       -isystem third_party/tree-sitter/runtime/include \
//       -isystem /usr/include/qt6 -isystem /usr/include/qt6/QtCore \
//       -isystem /usr/include/qt6/QtGui -isystem /usr/include/qt6/QtWidgets \
//       -lQt6Widgets -lQt6Gui -lQt6Core -o bench_highlight
//
// What it measures (all with the engine wired exactly like CodeEditor:
// one EditEvent per core mutation -> onEdit -> deferred refreshIfNeeded):
//   1. full parse + full-document query after setText (first-paint cost),
//   2. per-keystroke refresh at random offsets (incremental reparse +
//      full-document query exec - the actual keystroke cost),
//   3. the no-incremental baseline (forced full parse every keystroke),
//   4. edit bookkeeping alone (ts_tree_edit + line-index shift),
//   5. pathological single-piece document (no piece splits for a while).

#include "core/TextBuffer.hpp"
#include "view/HighlightEngine.hpp"

#include <QElapsedTimer>
#include <QString>
#include <QFile>
#include <algorithm>
#include <cstdio>
#include <numeric>
#include <vector>

using qmn::core::TextBuffer;
using qmn::view::HighlightEngine;

static QString syntheticPython(int lines)
{
    QString out;
    int block = 0;
    while (out.count(QLatin1Char('\n')) < lines) {
        out += QStringLiteral("def func%1(arg_a, arg_b):\n").arg(block);
        out += QStringLiteral("    total = arg_a + arg_b  # accumulate func%1\n").arg(block);
        out += QStringLiteral("    if total > 100:\n        return \"value-%1\" %% total\n").arg(block);
        out += QStringLiteral("    return [x for x in range(arg_a, arg_b)]\n\n");
        ++block;
    }
    return out;
}

int main(int argc, char** argv)
{
    QString text;
    if (argc > 1) {
        QFile f(argv[1]);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::fprintf(stderr, "cannot open %s\n", argv[1]);
            return 1;
        }
        text = QString::fromUtf8(f.readAll());
    } else {
        text = syntheticPython(argc > 2 ? QString(argv[2]).toInt() : 4000);
    }

    const int units = text.size();
    std::printf("doc: %d UTF-16 units (~%d KB), %d lines, pieces after setText: see below\n",
                units, units / 500, text.count(QLatin1Char('\n')) + 1);

    TextBuffer buffer;
    HighlightEngine engine;
    engine.attach(&buffer);
    buffer.setEditObserver([&engine](const TextBuffer::EditEvent& ev) { engine.onEdit(ev); });

    QElapsedTimer t;

    // 1) First-paint cost: setText -> setLanguage -> refresh (cold parse +
    //    full-document query).
    buffer.setText(text);
    engine.setLanguage(QStringLiteral("python"));
    t.start();
    engine.refreshIfNeeded();
    const double coldMs = double(t.nsecsElapsed()) / 1e6;
    std::printf("cold full parse+query:      %8.2f ms  (%zu spans)\n", coldMs, engine.spans().size());

    // 2) Random single-char inserts, each followed by a refresh: exactly
    //    what one keystroke costs on the GUI thread today.
    constexpr int kEdits = 300;
    std::vector<double> perEdit;
    perEdit.reserve(kEdits);
    quint64 seed = 0xC0FFEE; // uint64 LCG: signed overflow would be UB
    double bookkeepingTotal = 0.0;
    for (int i = 0; i < kEdits; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        const int off = int(seed % quint64(std::max(1, buffer.charCount() - 1)));
        t.start();
        buffer.insertAtOffset(off, QStringLiteral("x")); // observer: onEdit only
        bookkeepingTotal += double(t.nsecsElapsed()) / 1e6;
        engine.refreshIfNeeded();
        perEdit.push_back(double(t.nsecsElapsed()) / 1e6);
    }
    std::sort(perEdit.begin(), perEdit.end());
    std::printf("edit+refresh x%d:          mean %7.3f ms  p50 %7.3f  p95 %7.3f  max %7.3f ms\n",
                kEdits,
                std::accumulate(perEdit.begin(), perEdit.end(), 0.0) / perEdit.size(),
                perEdit[kEdits / 2], perEdit[kEdits * 95 / 100], perEdit.back());
    std::printf("edit bookkeeping alone:    mean %7.3f ms/edit (ts_tree_edit + line index)\n",
                bookkeepingTotal / kEdits);

    // 3) No-incremental baseline: force a cold parse per keystroke by
    //    bouncing through the plain-text state. Incremental savings =
    //    (3) vs (2).
    std::vector<double> coldPerEdit;
    for (int i = 0; i < 30; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        const int off = int(seed % quint64(std::max(1, buffer.charCount() - 1)));
        t.start();
        buffer.insertAtOffset(off, QStringLiteral("y"));
        engine.setLanguage(QString());
        engine.setLanguage(QStringLiteral("python"));
        engine.refreshIfNeeded();
        coldPerEdit.push_back(double(t.nsecsElapsed()) / 1e6);
    }
    std::sort(coldPerEdit.begin(), coldPerEdit.end());
    std::printf("cold-every-keystroke x30:  mean %7.3f ms  median %7.3f ms\n",
                std::accumulate(coldPerEdit.begin(), coldPerEdit.end(), 0.0) / coldPerEdit.size(),
                coldPerEdit[coldPerEdit.size() / 2]);

    // 4) Pathological: giant single piece (setText again; inserts near the
    //    FRONT exercise the engine's line-index tail shift on a ~lines-long
    //    vector and force piece splits at the front).
    buffer.setText(text);
    engine.setLanguage(QStringLiteral("python"));
    t.restart();
    engine.refreshIfNeeded();
    std::printf("cold re-run after setText:  %8.2f ms\n", double(t.nsecsElapsed()) / 1e6);
    {
        t.start();
        for (int i = 0; i < 20; ++i)
            buffer.insertAtOffset(0, QStringLiteral("x"));
        engine.refreshIfNeeded();
        std::printf("20 front-inserts+refresh:   %8.2f ms total\n", double(t.nsecsElapsed()) / 1e6);
    }
    return 0;
}
