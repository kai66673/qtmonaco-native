#!/usr/bin/env bash
# M8 close-car ritual (PLAN, decided 2026-11-18): build + run the live-
# clangd probe. NEVER a CI gate (CI has no clangd; clangd versions drift) —
# but M8 may not CLOSE without a green run, recorded in the CHANGELOG.
#
# Ad-hoc g++ like tools/bench_model.cpp: needs the built archives, and the
# Qt6 include/lib paths below are ARCH Linux paths (adjust elsewhere). -no-pie
# because the host archives are built without PIC on purpose (AGENTS.md: the
# per-keystroke perf budgets were measured on non-PIC archives).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

if ! command -v clangd >/dev/null; then
    echo "PROBE-FAIL clangd: not on PATH — there is nothing to probe" >&2
    exit 1
fi
if [ ! -f build/src/glue/libqmn_glue.a ]; then
    echo "build/src/glue/libqmn_glue.a missing: build first" >&2
    echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build" >&2
    exit 1
fi

g++ -O2 -std=c++20 -no-pie \
    tools/probe_clangd.cpp \
    -Isrc \
    -isystem /usr/include/qt6 -isystem /usr/include/qt6/QtCore \
    -isystem /usr/include/qt6/QtGui -isystem /usr/include/qt6/QtWidgets \
    -isystem /usr/include/qt6/QtNetwork \
    build/src/glue/libqmn_glue.a \
    build/src/lsp/libqmn_lsp.a \
    build/src/protocol/libqmn_protocol.a \
    build/src/view/libqmn_view.a \
    build/src/diff/libqmn_diff.a \
    build/src/core/libqmn_core.a \
    build/third_party/tree-sitter/libqmn_ts.a \
    -lQt6Widgets -lQt6Gui -lQt6Network -lQt6Core \
    -o build/probe_clangd

# --exit-after-ms is the qmn-spike idiom for headless runs; the probe pumps
# its own bounded waits and needs none. Any display is irrelevant: the .cpp
# pins offscreen itself.
exec build/probe_clangd "$@"
