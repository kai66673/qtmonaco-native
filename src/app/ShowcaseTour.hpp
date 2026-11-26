#pragma once

// S1-4 (GATE 3 car S1): the SHOWCASE TOUR — the shared pin table of
// PLAN's machine S1-4, THE artifact both demos mirror (one table, two
// drivers: this C++ one and pydemo's S2 Python one, which re-declares
// the SAME corpus literals and prints the SAME table shape; the S2 close
// diffs the two printed tables leg-for-leg). The tour is a scripted walk
// of the DEMO's OWN doors (openPath / openDiffPair / loadGrammarLibrary /
// setLanguageForSuffix / applyEditorSettings / setLspEnabled /
// setDapAdapter / debugStep / debugContinue / debugStop / closeTabAt —
// never a private seam), prints one probe-style line per pin
// (`PASS n: leg — detail`, the tools/probe_dap.sh shape), prints skips as
// skips, and EXITS via its return value: false at the FIRST failed pin
// (the print lands before the return), true when every run leg passed.
// The table itself is pinned in CI by tst_demo's
// `tourTableRunsGreenMinusTheServerLegs` slot (which forces the clangd
// and live-adapter legs off); the `qmn-demo --tour` binary flag is the
// LIVE ritual and NEVER a ctest entry (PLAN's knot).
//
// Qt6::Test NEVER links into qmn_demo_app (only test binaries see it):
// the waits ride the local spin helper inside ShowcaseTour.cpp
// (processEvents + 20 ms tick + deadline) and the keys ride raw
// QKeyEvents through QApplication::sendEvent.

#include <QString>
#include <QStringList>

namespace qmn::app {

class MainWindow;

// The frozen CORPUS (PLAN's corpus block, resolved to a self-consistent
// shape at build — the block's own literals drifted: its 4-line tour.py
// never carried the `sum ` leg-2 selection nor leg 1's lineCount()==5,
// and its "model lines 1 and 4" note disagreed with the demo's stored
// breakpoint PAIR {2,6} (MainWindow.cpp, MODEL lines = wire lines 3/7);
// the frozen truth lives in the k… literals in ShowcaseTour.cpp and S2
// copies it byte-for-byte as Python literals — the corpus IS the drift
// check). FOUR files:
//   tour.py     5 lines, NO trailing newline (leg 1 pins lineCount()==5);
//               line 2 is `x = sum (values)` so {2,4} + four Shift+Right
//               selects exactly 'sum ' (leg 2);
//   diff_orig / diff_mod — PLAN's exact pair, two replace-hunks at
//               MODIFIED lines 1 and 3 (0-based), strict-edge F7 walk;
//   demo.c      one line — `bad(` on a line of its own makes clangd's
//               diagnostic face unambiguous.
// There is NO separate DAP source (the S1-4 design decision settled in
// code-reading): tour.py ITSELF is the leg-8 session's registered
// document (tab 0 never closes; the auto-start rides editorAt(0)), so
// the dapfake steps2 frames carry tour.py's abs path.
// The stored pair {2,6} rides a 5-line file LEGALLY on the read-back
// face: line 6 is past EOF, kept verbatim by the store (no dot painted
// there) — leg 8 pins that read-back exactly. probe.pyext/future.pyext
// are leg-5 DERIVED copies of tour.py's content (same golden table), not
// frozen corpus entries.
// writeTourCorpus lays the files out under `dir` (UTF-8, LF) and returns
// their paths, names in struct order.
struct TourCorpus {
    QString tourPy;
    QString diffOrig;
    QString diffMod;
    QString demoC;

    QStringList all() const;
};

TourCorpus writeTourCorpus(const QString& dir);

// What the host of the tour supplies; the table itself is fixed. The
// ctest face (tst_demo) builds this from its compile-time fixture
// defines and forces every PATH-discovered leg off; the live CLI face
// (`qmn-demo --tour`) resolves the fixtures from the build tree and the
// adapters/clangd from PATH. An empty optional path = the leg SKIPS with
// a printed reason — the tour NEVER silently drops a leg it cannot run.
struct TourConfig {
    QString corpusDir;          // REQUIRED: corpus files get written here

    QString dapfakePath;        // the steps2 stand-in (leg 8's EXACT-PIN
                                // leg; empty = SKIP with reason)
    QString pyGrammarLib;       // the second compiled python parser .so
    QString pyQueryScm;         // the vendored python highlights.scm
    QString fakeGrammarLib;     // the zeroed fixture for the refusal face
                                // (empty = that sub-face prints its own
                                // SKIP, the leg stays alive on the rest)

    bool clangdEnabled = false; // row 10 (the tour opens demo.c and the
                                // hub claims it — presence-shaped: clangd
                                // drifts, probe_clangd.sh owns exact pins)
    QString lldbDapPath;        // rows 12/13, the live legs (presence-
                                // shaped per PLAN: stop-reason seen +
                                // caret follows a real stop — when
                                // present the FULL door walk runs; empty
                                // = SKIP)
    QString gdbPath;
    QString liveDapSkipReason;  // why rows 12/13 skip when a path is
                                // EMPTY: each face states its own truth
                                // (the ctest face forces the legs off;
                                // the CLI's QMN_TOUR_NO_LIVE_DAP=1 says
                                // "scoped off") — empty falls back to
                                // "not found on PATH", the honest
                                // discovery face
};

// Runs the whole table over `window` (a bare MainWindow the caller
// owns). True = every run pin passed; false = the first FAIL already
// printed. Waits ride the spin helper, so the caller must NOT be inside
// a blocking exec() when calling this.
bool runShowcaseTour(const TourConfig& config, MainWindow& window);

} // namespace qmn::app
