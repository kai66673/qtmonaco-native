#!/usr/bin/env bash
# Car 4 close-car ritual (PLAN, D4 record): the LIVE-ADAPTER DAP probe.
# Never a CI gate (CI ships no debuggers; adapters drift) — but car 4 may
# not CLOSE without a green run, recorded in the CHANGELOG. PLAN-decided
# order: lldb-dap FIRST, gdb -i=dap SECOND (the host ships both; gdb 18.1
# speaks DAP natively). A missing adapter prints SKIP and fails the run —
# the ritual is honest, an adapter's refusal is printed as the adapter's.
#
# Ad-hoc g++ like tools/bench_model.cpp: needs the built archives, and the
# Qt6 include/lib paths below are ARCH Linux paths (adjust elsewhere).
# -no-pie because the host archives are built without PIC on purpose
# (AGENTS.md: the per-keystroke perf budgets were measured on non-PIC).
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [ ! -f build/src/glue/libqmn_glue.a ] || [ ! -f build/src/dap/libqmn_dap.a ]; then
    echo "build/src/{glue,dap} archives missing: build first" >&2
    echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build" >&2
    exit 1
fi

g++ -O2 -std=c++20 -no-pie \
    tools/probe_dap.cpp \
    -Isrc \
    -isystem /usr/include/qt6 -isystem /usr/include/qt6/QtCore \
    -isystem /usr/include/qt6/QtGui -isystem /usr/include/qt6/QtWidgets \
    -isystem /usr/include/qt6/QtNetwork \
    build/src/glue/libqmn_glue.a \
    build/src/lsp/libqmn_lsp.a \
    build/src/dap/libqmn_dap.a \
    build/src/protocol/libqmn_protocol.a \
    build/src/view/libqmn_view.a \
    build/src/diff/libqmn_diff.a \
    build/src/core/libqmn_core.a \
    build/third_party/tree-sitter/libqmn_ts.a \
    -lQt6Widgets -lQt6Gui -lQt6Network -lQt6Core \
    -o build/probe_dap

overall=0
run() { # $1 = label, rest = adapter argv
    echo "== $1 =="
    if ! command -v "$2" >/dev/null 2>&1; then
        echo "SKIP: $2 not on PATH — nothing to probe (the ritual needs it: FAIL)" >&2
        overall=1
        return
    fi
    build/probe_dap "${@:2}" || overall=1
    echo
}

# lldb-dap FIRST (the reference implementation), gdb -i=dap SECOND.
run "lldb-dap" lldb-dap
run "gdb -i=dap" gdb -i=dap

if [ "$overall" -eq 0 ]; then
    echo "PROBE-RITUAL-OK: both adapters agreed"
else
    echo "PROBE-RITUAL-FAIL (see the per-adapter output above)" >&2
fi
exit "$overall"
