#include "ShowcaseTour.hpp"

#include "MainWindow.hpp"
#include "EditorSettings.hpp"

#include "view/CodeEditor.hpp"
#include "view/CodeEditorOptions.hpp"
#include "view/CodeEditorTheme.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QStatusBar>
#include <QTabBar>
#include <QThread>

#include <cstdio>
#include <functional>

// The tour prints through qDebug (PLAN: capturable offscreen, stdout on
// a terminal). No QtTest anywhere in this TU — qmn_demo_app never links
// it (only test binaries do), so the waits and the keys ride the plain
// machinery below.

namespace qmn::app {

namespace {

// ------------------------------------------------------- the print/pin state

// The probe-style table: `PASS n: leg — detail` / `SKIP n: leg — reason`
// / `FAIL n: leg — detail`. A FAIL stops the whole tour (return false
// travels out of runShowcaseTour); the print lands BEFORE the return, so
// the failed pin is always on the wire. Skips continue the walk.
struct TourPrint {
    int counter = 0;

    void pass(const QString& leg, const QString& detail)
    {
        ++counter;
        qDebug().noquote() << QStringLiteral("PASS %1: %2 — %3")
                                  .arg(counter).arg(leg, detail);
    }
    void skip(const QString& leg, const QString& reason)
    {
        ++counter;
        qDebug().noquote() << QStringLiteral("SKIP %1: %2 — %3")
                                  .arg(counter).arg(leg, reason);
    }
    bool fail(const QString& leg, const QString& detail) // always false,
    {                                                   // so the caller
        ++counter;                                      // can return it
        qDebug().noquote() << QStringLiteral("FAIL %1: %2 — %3")
                                  .arg(counter).arg(leg, detail);
        return false;
    }
};

// ------------------------------------------------------------- the spin law

// PLAN's local spin helper: processEvents + 20 ms tick + a deadline. It
// is the ONLY waiting machinery here (QTest::qWait/spin macros live in
// Qt6::Test, which this library must never link).
bool spinUntil(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (predicate())
            return true;
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    return predicate();
}

// Spin until the status bar shows one of the awaited truths (or the
// deadline). Returns the message observed at exit.
QString spinForMessage(QStatusBar* bar, const QStringList& awaited,
                       int timeoutMs)
{
    QString observed;
    spinUntil([&] {
        observed = bar->currentMessage();
        return awaited.contains(observed);
    }, timeoutMs);
    return observed;
}

// --------------------------------------------------------- key synthesis

// The tour's keystrokes ride RAW QKeyEvents through sendEvent (QTest is
// off-limits in this library). keyPressEvent is what consumes chords and
// caret motion alike; a bare F7 and Shift+Right are both plain-key faces.
void sendKey(QWidget* target, int key, Qt::KeyboardModifiers mods = {})
{
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QApplication::sendEvent(target, &press);
    QApplication::sendEvent(target, &release);
}

QString tokenKindName(CodeEditor::TokenKind kind)
{
    switch (kind) {
    case CodeEditor::TokenKind::Comment: return QStringLiteral("Comment");
    case CodeEditor::TokenKind::Keyword: return QStringLiteral("Keyword");
    case CodeEditor::TokenKind::Number: return QStringLiteral("Number");
    case CodeEditor::TokenKind::String: return QStringLiteral("String");
    case CodeEditor::TokenKind::StringSpecial: return QStringLiteral("StringSpecial");
    case CodeEditor::TokenKind::StringSpecialKey: return QStringLiteral("StringSpecialKey");
    case CodeEditor::TokenKind::Escape: return QStringLiteral("Escape");
    case CodeEditor::TokenKind::Embedded: return QStringLiteral("Embedded");
    case CodeEditor::TokenKind::Constant: return QStringLiteral("Constant");
    case CodeEditor::TokenKind::ConstantBuiltin: return QStringLiteral("ConstantBuiltin");
    case CodeEditor::TokenKind::Constructor: return QStringLiteral("Constructor");
    case CodeEditor::TokenKind::Function: return QStringLiteral("Function");
    case CodeEditor::TokenKind::FunctionBuiltin: return QStringLiteral("FunctionBuiltin");
    case CodeEditor::TokenKind::FunctionMethod: return QStringLiteral("FunctionMethod");
    case CodeEditor::TokenKind::Type: return QStringLiteral("Type");
    case CodeEditor::TokenKind::Variable: return QStringLiteral("Variable");
    case CodeEditor::TokenKind::VariableBuiltin: return QStringLiteral("VariableBuiltin");
    case CodeEditor::TokenKind::Property: return QStringLiteral("Property");
    case CodeEditor::TokenKind::Operator: return QStringLiteral("Operator");
    case CodeEditor::TokenKind::Punctuation: return QStringLiteral("Punctuation");
    case CodeEditor::TokenKind::PunctuationBracket: return QStringLiteral("PunctuationBracket");
    case CodeEditor::TokenKind::PunctuationDelimiter: return QStringLiteral("PunctuationDelimiter");
    case CodeEditor::TokenKind::PunctuationSpecial: return QStringLiteral("PunctuationSpecial");
    case CodeEditor::TokenKind::Text: return QStringLiteral("Text");
    }
    return QStringLiteral("?");
}

// One line per span so the two drivers' tables diff row-for-row (S2
// prints the SAME shape: `start end Kind`).
QString spanRows(const QVector<CodeEditor::HighlightSpan>& spans)
{
    QString rows;
    for (const auto& span : spans)
        rows += QStringLiteral("%1-%2:%3 ").arg(span.start)
            .arg(span.end).arg(tokenKindName(span.kind));
    return rows.trimmed();
}

int tabHolding(MainWindow& window, const QString& path)
{
    QTabBar* bar = window.tabWidget()->tabBar();
    for (int i = 0; i < bar->count(); ++i)
        if (bar->tabData(i).toString() == path)
            return i;
    return -1;
}

void closeEveryTab(MainWindow& window)
{
    while (window.editorCount() > 0)
        window.closeTabAt(0);
}

// The frozen tour.py — FIVE lines and deliberately NO trailing newline
// (a trailing '\n' would make lineCount()==6 and break leg 1's pin). The
// shape is load-bearing for leg 2: line 2 (0-based) is `x = sum (values)`
// so a {2,4} caret + four Shift+Right selects the literal `sum ` (the
// trailing space rides cols 4..7); PLAN's corpus block drifted (its
// 4-line block never carried that literal), so THIS is the frozen truth
// S2 copies byte-for-byte. It is deliberately NOT an executable script
// (no shebang, no exec bit): the live-adapter legs' launch face is the
// honest FAIL face the owner accepted (probe_dap.sh's real launches own
// the exact live numbers).
const char* kTourPy =
    "import sys\n"
    "values = [1, 2]\n"
    "x = sum (values)\n"
    "def greet():\n"
    "    return x  # greet";

const char* kDiffOrig = "alpha\none\nbeta\ntwo\ngamma\n";
const char* kDiffMod = "alpha\nONE\nbeta\nTWO\ngamma\n";
const char* kDemoC = "int main(void) { return 0; }\n";

bool writeCorpusFile(const QString& path, const char* content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(content);
    file.close();
    return true;
}

} // namespace

QStringList TourCorpus::all() const
{
    return { tourPy, diffOrig, diffMod, demoC };
}

TourCorpus writeTourCorpus(const QString& dir)
{
    TourCorpus corpus;
    corpus.tourPy = dir + QStringLiteral("/tour.py");
    corpus.diffOrig = dir + QStringLiteral("/diff_orig.txt");
    corpus.diffMod = dir + QStringLiteral("/diff_mod.txt");
    corpus.demoC = dir + QStringLiteral("/demo.c");
    return corpus;
}

bool runShowcaseTour(const TourConfig& config, MainWindow& window)
{
    TourPrint t;

    // -------------------------------------------------------------- corpus
    if (config.corpusDir.isEmpty())
        return t.fail(QStringLiteral("corpus"), QStringLiteral("no corpusDir in the config"));
    QDir().mkpath(config.corpusDir); // the CLI's QMN_TOUR_OUT face may
                                     // name a dir that doesn't exist yet
    const TourCorpus corpus = writeTourCorpus(config.corpusDir);
    bool wrote = true;
    wrote = wrote && writeCorpusFile(corpus.tourPy, kTourPy);
    wrote = wrote && writeCorpusFile(corpus.diffOrig, kDiffOrig);
    wrote = wrote && writeCorpusFile(corpus.diffMod, kDiffMod);
    wrote = wrote && writeCorpusFile(corpus.demoC, kDemoC);
    if (!wrote)
        return t.fail(QStringLiteral("corpus"), QStringLiteral("a corpus file could not be written under %1").arg(config.corpusDir));
    t.pass(QStringLiteral("corpus"), QStringLiteral("4 frozen files under %1").arg(config.corpusDir));

    // ---------------------------------------------------------------- leg 1: boot
    if (!window.openPath(corpus.tourPy))
        return t.fail(QStringLiteral("boot"), QStringLiteral("openPath(tour.py) returned false"));
    CodeEditor* pyTab = window.currentEditor();
    if (!pyTab || window.editorCount() != 1)
        return t.fail(QStringLiteral("boot"), QStringLiteral("editorCount=%1").arg(window.editorCount()));
    if (pyTab->lineCount() != 5)
        return t.fail(QStringLiteral("boot"), QStringLiteral("lineCount=%1, expected 5").arg(pyTab->lineCount()));
    if (pyTab->position() != (qmn::view::TextPosition{0, 0}))
        return t.fail(QStringLiteral("boot"), QStringLiteral("position after open is not {0,0}"));
    t.pass(QStringLiteral("boot"), QStringLiteral("1 tab, lineCount 5, caret {0,0}"));

    // ---------------------------------------------------------------- leg 2: model
    pyTab->setPosition({2, 4});
    if (pyTab->position() != (qmn::view::TextPosition{2, 4}))
        return t.fail(QStringLiteral("model"), QStringLiteral("setPosition({2,4}) did not stick"));
    const QString preEdit = pyTab->toPlainText();
    // The 'sum ' selection: four real Shift+Right keystrokes from {2,4}
    // (the tour SHOWS the selection face, and the keys are the tour's own
    // chord machinery — no word-selection law is leaned on, the selection
    // is exactly cols 4..8).
    sendKey(pyTab, Qt::Key_Right, Qt::ShiftModifier);
    sendKey(pyTab, Qt::Key_Right, Qt::ShiftModifier);
    sendKey(pyTab, Qt::Key_Right, Qt::ShiftModifier);
    sendKey(pyTab, Qt::Key_Right, Qt::ShiftModifier);
    if (pyTab->selectedText() != QStringLiteral("sum "))
        return t.fail(QStringLiteral("model"), QStringLiteral("selectedText()='%1', expected 'sum '").arg(pyTab->selectedText()));
    // The edit rides the M8 m5 write door (PLAN's leg-2 wording: an
    // applyTextEdits INSERT): a lone 's' at {2,7} turns `sum ` into
    // `sums ` — one batch, one undo step, the frozen post string below.
    // (Col 7, not 6: `sum ` rides cols 4..7 with the SPACE at 6, so an
    // insert at 6 would give `susm ` — the first tour run caught exactly
    // that off-by-one.)
    const CodeEditor::TextEdit insertS{2, 7, 2, 7, QStringLiteral("s")};
    if (pyTab->applyTextEdits({insertS}) != 1)
        return t.fail(QStringLiteral("model"), QStringLiteral("applyTextEdits did not apply exactly one edit"));
    const QString postEdit =
        "import sys\n"
        "values = [1, 2]\n"
        "x = sums (values)\n"
        "def greet():\n"
        "    return x  # greet";
    if (pyTab->toPlainText() != postEdit)
        return t.fail(QStringLiteral("model"), QStringLiteral("post-edit text is not the frozen string: '%1'").arg(pyTab->toPlainText()));
    pyTab->undo();
    if (pyTab->toPlainText() != preEdit)
        return t.fail(QStringLiteral("model"), QStringLiteral("undo did not restore the pre-edit bytes"));
    t.pass(QStringLiteral("model"), QStringLiteral("setPosition {2,4}, selection 'sum ', insertTextEdits 's'->`sums `, undo byte-identical"));

    // ------------------------------------------------------- leg 3: highlight
    const int textLen = pyTab->toPlainText().size();
    const auto golden = pyTab->highlightSpans(0, textLen);
    if (golden.isEmpty())
        return t.fail(QStringLiteral("highlight"), QStringLiteral("zero spans on the python tab (grammar not alive?)"));
    t.pass(QStringLiteral("highlight"), QStringLiteral("%1 spans: %2").arg(golden.size()).arg(spanRows(golden)));

    // ------------------------------------------------------------- leg 4: diff
    if (!window.openDiffPair(corpus.diffOrig, corpus.diffMod))
        return t.fail(QStringLiteral("diff"), QStringLiteral("openDiffPair returned false"));
    // PLAN's "editorCount()==3" counts the DIFF LEG's tabs; the tour's
    // tour.py witness (legs 1-3, still open until leg 8) rides on top, so
    // the exact face is 4 tabs with the DIFF tab focused last (recorded as
    // a corpus-shape delta in PLAN's S1-4 close record).
    if (window.editorCount() != 4 || window.tabWidget()->currentIndex() != 3)
        return t.fail(QStringLiteral("diff"), QStringLiteral("tabs=%1 current=%2, expected 4 and the diff tab focused").arg(window.editorCount()).arg(window.tabWidget()->currentIndex()));
    CodeEditor* diffTab = window.currentEditor();
    if (!diffTab->isDiffMode())
        return t.fail(QStringLiteral("diff"), QStringLiteral("the focused tab is not a diff tab"));
    diffTab->setFocus();
    diffTab->setPosition({0, 0});
    struct Hop { Qt::KeyboardModifiers mods; int expectedLine; bool shifted; };
    // Two separated replace-hunks at MODIFIED lines 1 and 3: F7 lands on
    // each in order, the third press is the strict-edge no-op (no wrap),
    // Shift+F7 mirrors, its second press no-ops at the other edge.
    const Hop hops[] = {
        {Qt::NoModifier, 1, false},
        {Qt::NoModifier, 3, false},
        {Qt::NoModifier, 3, false},
        {Qt::ShiftModifier, 1, true},
        {Qt::ShiftModifier, 1, true},
    };
    for (const Hop& hop : hops) {
        sendKey(diffTab, Qt::Key_F7, hop.mods);
        if (diffTab->position().line != hop.expectedLine)
            return t.fail(QStringLiteral("diff"), QStringLiteral("F7 walk landed at line %1, expected %2").arg(diffTab->position().line).arg(hop.expectedLine));
    }
    if (diffTab->toPlainText() != QLatin1String(kDiffMod))
        return t.fail(QStringLiteral("diff"), QStringLiteral("the diff tab's text is not the modified corpus"));
    t.pass(QStringLiteral("diff"), QStringLiteral("the door's 3 tabs (tour-side count 4, the witnesses still open), diff focused, F7 walk 1->3->3->1->1, text == modified corpus"));

    // ---------------------------------------------------------- leg 5: grammar
    const QString probePath = config.corpusDir + QStringLiteral("/probe.pyext");
    const QString futurePath = config.corpusDir + QStringLiteral("/future.pyext");
    if (!writeCorpusFile(probePath, kTourPy))
        return t.fail(QStringLiteral("grammar"), QStringLiteral("probe.pyext could not be written"));
    if (CodeEditor::availableLanguages().contains(QStringLiteral("pyext")))
        return t.fail(QStringLiteral("grammar"), QStringLiteral("the BEFORE face: 'pyext' is ALREADY registered (a dirty registry)"));
    if (!window.openPath(probePath))
        return t.fail(QStringLiteral("grammar"), QStringLiteral("openPath(probe.pyext) returned false"));
    CodeEditor* probeTab = window.currentEditor();
    probeTab->setLanguage(QStringLiteral("pyext"));
    if (!probeTab->language().isEmpty()
        || !probeTab->highlightSpans(0, probeTab->toPlainText().size()).isEmpty())
        return t.fail(QStringLiteral("grammar"), QStringLiteral("the BEFORE face: an unknown id left language()='%1' or spans non-empty").arg(probeTab->language()));
    t.pass(QStringLiteral("grammar"), QStringLiteral("before-face: pyext absent, a pre-set tab stays plain (language() empty, zero spans)"));
    if (config.pyGrammarLib.isEmpty() || config.pyQueryScm.isEmpty()) {
        t.skip(QStringLiteral("grammar-load"), QStringLiteral("no --grammar fixture configured (set the pygrammar .so + the vendored python .scm)"));
    } else {
        if (!window.loadGrammarLibrary(config.pyGrammarLib,
                                       QStringLiteral("pyext"),
                                       QStringLiteral("pyext"),
                                       QStringLiteral("tree_sitter_python"),
                                       config.pyQueryScm))
            return t.fail(QStringLiteral("grammar-load"), QStringLiteral("loadGrammarLibrary returned false"));
        if (window.statusBar()->currentMessage()
            != QStringLiteral("Грамматика «pyext» загружена (язык «pyext», вкладок: 0)"))
            return t.fail(QStringLiteral("grammar-load"), QStringLiteral("status='%1'").arg(window.statusBar()->currentMessage()));
        // The registration never resurrects the plain probe (the engine's
        // documented drop — the S1-2 pin rides door-for-door), so the tour
        // RE-CALLS setLanguage, exactly like the wheel face will.
        if (!probeTab->language().isEmpty())
            return t.fail(QStringLiteral("grammar-load"), QStringLiteral("the plain probe tab was resurrected without a setLanguage call"));
        probeTab->setLanguage(QStringLiteral("pyext"));
        const auto mine = probeTab->highlightSpans(0, probeTab->toPlainText().size());
        if (mine.size() != golden.size() || mine != golden)
            return t.fail(QStringLiteral("grammar-load"), QStringLiteral("pyext spans != the golden python table (%1 vs %2): %3").arg(mine.size()).arg(golden.size()).arg(spanRows(mine)));
        // The session's suffix table (G2): a FUTURE .pyext tab claims the
        // id through the door, no setLanguage call. SAME content as the
        // probe, so the golden table pins both faces identically.
        window.setLanguageForSuffix(QStringLiteral("pyext"), QStringLiteral("pyext"));
        if (!writeCorpusFile(futurePath, kTourPy))
            return t.fail(QStringLiteral("grammar-load"), QStringLiteral("future.pyext could not be written"));
        if (!window.openPath(futurePath))
            return t.fail(QStringLiteral("grammar-load"), QStringLiteral("openPath(future.pyext) returned false"));
        CodeEditor* futureTab = window.currentEditor();
        if (futureTab->language() != QStringLiteral("pyext")
            || futureTab->highlightSpans(0, futureTab->toPlainText().size()) != golden)
            return t.fail(QStringLiteral("grammar-load"), QStringLiteral("the future .pyext tab did not equal the golden table (language='%1')").arg(futureTab->language()));
        t.pass(QStringLiteral("grammar-load"), QStringLiteral("load==вкладок: 0, probe stays plain until setLanguage, spans EQUAL the golden table, future .pyext tab too: %1").arg(spanRows(mine)));
        if (!config.fakeGrammarLib.isEmpty()) {
            const int before = CodeEditor::availableLanguages().size();
            if (window.loadGrammarLibrary(config.fakeGrammarLib, QStringLiteral("bogus")))
                return t.fail(QStringLiteral("grammar-refusal"), QStringLiteral("the zeroed fixture was ACCEPTED"));
            if (window.statusBar()->currentMessage()
                != QStringLiteral("Библиотека «%1» отклонена: не подходящая грамматика").arg(config.fakeGrammarLib))
                return t.fail(QStringLiteral("grammar-refusal"), QStringLiteral("status='%1'").arg(window.statusBar()->currentMessage()));
            if (CodeEditor::availableLanguages().size() != before)
                return t.fail(QStringLiteral("grammar-refusal"), QStringLiteral("the registry moved on a refused library"));
            t.pass(QStringLiteral("grammar-refusal"), QStringLiteral("bogus library false + exact status, registry unmoved"));
        } else {
            t.skip(QStringLiteral("grammar-refusal"), QStringLiteral("no bogus fixture configured"));
        }
        // Teardown of THIS leg: the registry is process-wide, so the tour
        // leaves exactly what it found (the ctest face's sibling grammar
        // slot pins the same discipline).
        CodeEditor::unregisterGrammar(QStringLiteral("pyext"));
        window.setLanguageForSuffix(QStringLiteral("pyext"), QString());
    }

    // --------------------------------------------------------- leg 6: settings
    const EditorSettings baseline = window.editorSettings();
    EditorSettings changed = baseline;
    changed.tabStopWidth = 7;
    changed.longLineRulerColumn = 80;
    changed.softWrapEnabled = true;
    changed.themePreset = CodeEditorTheme::Preset::Dark;
    window.applyEditorSettings(changed);
    bool knobsRode = true;
    for (int i = 0; i < window.editorCount(); ++i) {
        qmn::view::CodeEditorOptions* opt = window.editorAt(i)->options();
        if (opt->tabStopWidth() != 7 || opt->longLineRulerColumn() != 80
            || !opt->softWrapEnabled())
            knobsRode = false;
    }
    if (!knobsRode)
        return t.fail(QStringLiteral("settings"), QStringLiteral("tabStopWidth/longLineRuler/softWrap did not read back through options() on every tab"));
    if (window.editorSettings().themePreset != CodeEditorTheme::Preset::Dark
        || window.currentEditor()->theme()->kind() != CodeEditorTheme::Preset::Dark)
        return t.fail(QStringLiteral("settings"), QStringLiteral("the M24 preset did not land (theme kind())"));
    const EditorSettings collected = collectSettings(window.currentEditor());
    if (collected.tabStopWidth != 7 || collected.longLineRulerColumn != 80
        || !collected.softWrapEnabled || collected.themePreset != CodeEditorTheme::Preset::Dark)
        return t.fail(QStringLiteral("settings"), QStringLiteral("collectSettings read back tabStop=%1 ruler=%2 softWrap=%3 preset=%4")
                          .arg(collected.tabStopWidth).arg(collected.longLineRulerColumn)
                          .arg(collected.softWrapEnabled).arg(int(collected.themePreset)));
    window.applyEditorSettings(baseline); // the 15-knob surface rides UNCHANGED
    t.pass(QStringLiteral("settings"), QStringLiteral("4-knob subset (tabStop 7, ruler 80, softWrap on, Dark preset) rode options()+theme() and collected back, baseline restored"));

    // ------------------------------------------------------ leg 7: lsp-clangd
    if (!config.clangdEnabled) {
        t.skip(QStringLiteral("lsp-clangd"), QStringLiteral("clangd leg not enabled (the ctest face forces it off; the live face resolves clangd from PATH)"));
    } else {
        window.setLspEnabled(true); // BEFORE the open: the session hands a
                                    // just-attached hub its config replay,
                                    // but opens only reach a hub through
                                    // session_->openDocument while it is
                                    // ATTACHED (the --lsp-after-open path
                                    // never didOpen's the old tabs)
        if (!window.openPath(corpus.demoC))
            return t.fail(QStringLiteral("lsp-clangd"), QStringLiteral("openPath(demo.c) returned false"));
        const int demoIndex = window.editorCount() - 1;
        CodeEditor* cTab = window.editorAt(demoIndex);
        if (cTab->language() != QStringLiteral("c"))
            return t.fail(QStringLiteral("lsp-clangd"), QStringLiteral("demo.c did not resolve to the built-in 'c' language (language='%1')").arg(cTab->language()));
        cTab->insertTextAt({0, 0}, QStringLiteral("bad(\n"));
        bool diagnosed = spinUntil([&] { return !cTab->diagnostics().isEmpty(); }, 20000);
        const QString lspStatus = window.statusBar()->currentMessage();
        const int diagCount = cTab->diagnostics().size(); // read BEFORE the
        window.setLspEnabled(false); // disarm may clear the decorations
        if (!diagnosed)
            return t.fail(QStringLiteral("lsp-clangd"), QStringLiteral("no diagnostic decoration within 20 s of injecting 'bad(' (status='%1')").arg(lspStatus));
        if (lspStatus.isEmpty())
            return t.fail(QStringLiteral("lsp-clangd"), QStringLiteral("the status bar never spoke a server truth"));
        t.pass(QStringLiteral("lsp-clangd"), QStringLiteral("clangd claimed the tab, 'bad(' produced %1 diagnostic decoration(s)").arg(diagCount));
    }

    // ------------------------------------------------------------- leg 8: DAP
    // The session's document is tour.py ITSELF (the S1-4 decision): the
    // door's auto-start rides editorAt(0), and tour.py has been tab 0
    // since leg 1 and never closed — every leg since just ADDED tabs.
    // This identity pin is the witness that the auto-start will arm the
    // right document (a tab-reorder or accidental close would land the
    // frames in `кадр вне открытых файлов`).
    if (window.editorAt(0) != pyTab)
        return t.fail(QStringLiteral("dap"), QStringLiteral("tab 0 is not the tour.py witness the session must register (editorAt(0) != the leg-1 editor)"));
    CodeEditor* dapTab = pyTab;

    if (config.dapfakePath.isEmpty()) {
        t.skip(QStringLiteral("dap-dapfake"), QStringLiteral("no dapfake configured (the CI/tour stand-in; the live probe ritual owns real adapters)"));
    } else {
        // QStatusBar::messageChanged catches EVERY truth in order; the
        // spin helper's polls can race a fast pair into ONE processEvents
        // burst (the initialize response and a later stopped event are
        // separate bursts, but a collector makes the ORDER an exact pin,
        // not a poll-time lucky observation). The receiver object retires
        // the connection at THIS scope's exit — later legs (and the live
        // walks) must never append to a dead `faces`.
        QObject faceSink;
        QStringList faces;
        QObject::connect(window.statusBar(), &QStatusBar::messageChanged,
                         &faceSink, [&faces](const QString& m) {
                             if (!m.isEmpty())
                                 faces << m;
                         });
        window.setDapAdapter(config.dapfakePath,
                             { QStringLiteral("steps2"),
                               QFileInfo(corpus.tourPy).absoluteFilePath(),
                               QStringLiteral("3"), QStringLiteral("6"),
                               QStringLiteral("4") });
        if (!window.dapEnabled())
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("arming returned an OFF door"));
        QString msg = spinForMessage(window.statusBar(),
                                     { QStringLiteral("DAP: пауза в потоке 7 (breakpoint)") },
                                     30000);
        if (msg != QStringLiteral("DAP: пауза в потоке 7 (breakpoint)"))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("status='%1'").arg(msg));
        // The stored PAIR {2,6} (the demo's fixed truth in MODEL lines —
        // wire lines 3 and 7) reads back VERBATIM through the view's
        // decoration door even though line 6 is past this 5-line file's
        // EOF (the store keeps what it was given, no dot is painted
        // there) — the S1-4 record's corrected arithmetic, pinned exactly.
        if (!spinUntil([&] { return dapTab->breakpointLines() == (QVector<int>{2, 6}); }, 5000))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("breakpointLines()='%1', expected the stored pair {2,6}").arg(dapTab->breakpointLines().size()));
        if (!spinUntil([&] { return dapTab->highlightedLinesStart() == 2; }, 5000))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("the stopped band sits at %1, expected the frame line (2)").arg(dapTab->highlightedLinesStart()));
        if (dapTab->position() != (qmn::view::TextPosition{2, 5})) // frame
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("caret at %1:%2 after the breakpoint stop, expected the frame (2,5)").arg(dapTab->position().line).arg(dapTab->position().col)); // (3,6) 1-based
        // The value-hint feed on `x = sum (values)`: exactly ONE hint —
        // x is the only wanted name on the frame line — and its " = 42"
        // is the FIRST-WINS value (dapfake's x=WRONG-DUP duplicate,
        // y=13 and zzUnused all never surface). Same walk, same first-
        // wins proof as the tst_demo S1-3 slot; the COUNT rides this
        // corpus's line content (tst's frame line carried x AND y).
        if (!spinUntil([&] { return dapTab->inlayHints().size() == 1; }, 5000))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("%1 value hints on the frame line, expected exactly one").arg(dapTab->inlayHints().size()));
        if (dapTab->inlayHints().at(0) != (qmn::view::CodeEditor::InlayHint{2, 1, QStringLiteral(" = 42")}))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("hint at {%1,%2}='%3', expected {2,1}=' = 42'").arg(dapTab->inlayHints().at(0).line).arg(dapTab->inlayHints().at(0).col).arg(dapTab->inlayHints().at(0).text));
        if (!window.debugStep())
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("debugStep() refused at a stopped pause"));
        msg = spinForMessage(window.statusBar(),
                             { QStringLiteral("DAP: пауза в потоке 7 (step)") }, 15000);
        if (msg != QStringLiteral("DAP: пауза в потоке 7 (step)"))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("after step status='%1'").arg(msg));
        if (!spinUntil([&] { return dapTab->highlightedLinesStart() == 3; }, 5000))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("the stopped band sits at %1 after the step, expected the step-frame line (3)").arg(dapTab->highlightedLinesStart()));
        if (dapTab->position() != (qmn::view::TextPosition{3, 5})) // step
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("caret at %1:%2 after the step, expected the step frame (3,5)").arg(dapTab->position().line).arg(dapTab->position().col)); // frame (4,6)
        // The step frame is `def greet():` — no wanted name rides it, so
        // the feed lands ZERO hints (the old band/hints for the previous
        // stop are gone in the same chain, not left standing).
        if (!spinUntil([&] { return dapTab->inlayHints().isEmpty(); }, 5000))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("%1 hints survived onto the step frame (def/greet carry no variable)").arg(dapTab->inlayHints().size()));
        if (!window.debugContinue())
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("debugContinue() refused at a stopped pause"));
        msg = spinForMessage(window.statusBar(), { QStringLiteral("DAP: продолжено") }, 15000);
        if (msg != QStringLiteral("DAP: продолжено"))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("after continue status='%1'").arg(msg));
        if (dapTab->highlightedLinesStart() != -1)
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("the band survived the continue (%1)").arg(dapTab->highlightedLinesStart()));
        if (!window.debugStop())
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("debugStop() refused on a live session"));
        msg = spinForMessage(window.statusBar(),
                             { QStringLiteral("DAP: сессия завершена (stopped)") }, 15000);
        if (msg != QStringLiteral("DAP: сессия завершена (stopped)"))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("session end status='%1'").arg(msg));
        // The WHOLE spoken sequence, byte-exact and in order (a skipped
        // or extra truth is a fail — the collector makes overtaking
        // invisible to the pins, so this is the exact walk PLAN's leg 8
        // demands; "(stopped)" is dapfake's terminate-first arm, the
        // far-side receipt that its exit stays 0).
        const QStringList expected{
            QStringLiteral("DAP: адаптер %1 — сессия начнётся с первой вкладкой")
                .arg(config.dapfakePath),
            QStringLiteral("DAP: сессия запущена"),
            QStringLiteral("DAP: пауза в потоке 7 (breakpoint)"),
            QStringLiteral("DAP: пауза в потоке 7 (step)"),
            QStringLiteral("DAP: продолжено"),
            QStringLiteral("DAP: сессия завершена (stopped)") };
        if (faces != expected)
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("the spoken status sequence is not the frozen one: got [%1]").arg(faces.join(QStringLiteral(" | "))));
        // The refusal faces AFTER the end, through the same doors (the
        // hub stands, the session does not) — the tst_demo S1-3 pins,
        // door-for-door on the tour side.
        if (window.debugStep())
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("debugStep() answered true on an ended session"));
        if (window.statusBar()->currentMessage()
            != QStringLiteral("DAP: шаг отклонён: нет паузы в готовой сессии"))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("step-refusal status='%1'").arg(window.statusBar()->currentMessage()));
        if (window.debugContinue())
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("debugContinue() answered true on an ended session"));
        if (window.statusBar()->currentMessage()
            != QStringLiteral("DAP: продолжение отклонено: нет паузы в готовой сессии"))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("continue-refusal status='%1'").arg(window.statusBar()->currentMessage()));
        if (window.debugStop())
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("debugStop() answered true on an ended session"));
        if (window.statusBar()->currentMessage()
            != QStringLiteral("DAP: остановка отклонена: нет активной сессии"))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("stop-refusal status='%1'").arg(window.statusBar()->currentMessage()));
        window.setDapAdapter(QString());
        if (window.dapEnabled())
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("the disarm left the door armed"));
        if (window.statusBar()->currentMessage()
            != QStringLiteral("DAP: адаптер остановлен"))
            return t.fail(QStringLiteral("dap-dapfake"), QStringLiteral("disarm status='%1'").arg(window.statusBar()->currentMessage()));
        t.pass(QStringLiteral("dap-dapfake"), QStringLiteral("exact status walk: запущена -> пауза 7 (breakpoint) [band 2, caret {2,5}, dots {2,6} verbatim past EOF, hint {2,1}=' = 42' first-wins] -> step [пауза 7 (step), band 3, caret {3,5}, hints 0 on def greet()] -> продолжено [band cleared] -> завершена (stopped) -> three exact refusals -> адаптер остановлен; sequence exact"));
    }

    auto runLiveLeg = [&](const QString& name, const QString& program,
                          const QStringList& args) -> bool {
        // PLAN's presence pins, the FULL door walk (S1-4 decision): a
        // REAL stop must arrive, the caret must follow it, and the typed
        // doors must ride that stop. The launch face is the honest weak
        // point: the armed auto-start launches tour.py — a 5-line script
        // with no shebang and no exec bit — so an adapter that cannot
        // exec it answers `launch` with its own error (or never stops);
        // the hub relays that as `пуск отклонён`/exited and the leg then
        // FAILS with the adapter's face as the detail — the outcome the
        // owner accepted for a host ritual over tour.py itself (the exact
        // live pins stay tools/probe_dap.sh's, whose launches use REAL
        // binaries). Scoping the ritual to a green table is the
        // operator's call through the CLI (an empty path prints the leg
        // as an honest SKIP).
        window.setDapAdapter(program, args);
        if (!window.dapEnabled())
            return t.fail(name, QStringLiteral("arming an existing binary (%1) returned an OFF door").arg(program));
        spinUntil([&] {
            const QString m = window.statusBar()->currentMessage();
            return m.startsWith(QStringLiteral("DAP: пауза"))
                || m.startsWith(QStringLiteral("DAP: пуск отклонён"))
                || m.startsWith(QStringLiteral("DAP: сессия завершена"));
        }, 45000);
        const QString face = window.statusBar()->currentMessage();
        if (!face.startsWith(QStringLiteral("DAP: пауза"))) {
            const QString endFace = (window.debugStop(),
                                     window.statusBar()->currentMessage());
            window.setDapAdapter(QString());
            return t.fail(name, QStringLiteral("no real stop within 45 s; the adapter's face was '%1' (session ended '%2') — the tour launches tour.py itself (no shebang, no exec bit), unlaunchable by a real adapter; the exact live pins are tools/probe_dap.sh's, and QMN_TOUR_NO_LIVE_DAP=1 prints this leg as a skip")
                                     .arg(face, endFace));
        }
        // A REAL stop: the caret-follow + band witnesses, then the typed
        // doors on a real thread, then the end. Presence-shaped details
        // (adapters drift; probe_dap.sh owns their exact numbers).
        const int caretLine = dapTab->position().line;
        const int bandLine = dapTab->highlightedLinesStart();
        if (bandLine < 0) {
            window.debugStop();
            window.setDapAdapter(QString());
            return t.fail(name, QStringLiteral("a stop arrived but the stopped band never painted (band=%1)").arg(bandLine));
        }
        if (!window.debugStep()) {
            window.debugStop();
            window.setDapAdapter(QString());
            return t.fail(name, QStringLiteral("debugStep() refused at a real stop"));
        }
        spinUntil([&] {
            const QString m = window.statusBar()->currentMessage();
            return m.startsWith(QStringLiteral("DAP: пауза"))
                || m.startsWith(QStringLiteral("DAP: сессия завершена"));
        }, 20000);
        const int steppedLine = dapTab->position().line;
        if (!window.debugContinue()) {
            window.debugStop();
            window.setDapAdapter(QString());
            return t.fail(name, QStringLiteral("debugContinue() refused at a real stop"));
        }
        if (!window.debugStop()) {
            window.setDapAdapter(QString());
            return t.fail(name, QStringLiteral("debugStop() refused on a live session"));
        }
        const QString endFace = window.statusBar()->currentMessage();
        window.setDapAdapter(QString());
        if (!endFace.startsWith(QStringLiteral("DAP: сессия завершена")))
            return t.fail(name, QStringLiteral("the session never reported its end (status='%1')").arg(endFace));
        t.pass(name, QStringLiteral("real stop: '%1' caret at line %2 (band %3); step -> caret line %4; end '%5'")
                         .arg(face).arg(caretLine).arg(bandLine).arg(steppedLine).arg(endFace));
        return true;
    };

    const QString liveSkip = config.liveDapSkipReason.isEmpty()
        ? QStringLiteral("%1 not found on PATH")
        : config.liveDapSkipReason;
    if (config.lldbDapPath.isEmpty())
        t.skip(QStringLiteral("dap-lldb-dap"),
               liveSkip.arg(QStringLiteral("lldb-dap")));
    else if (!runLiveLeg(QStringLiteral("dap-lldb-dap"), config.lldbDapPath, {}))
        return false;

    if (config.gdbPath.isEmpty())
        t.skip(QStringLiteral("dap-gdb"), liveSkip.arg(QStringLiteral("gdb")));
    else if (!runLiveLeg(QStringLiteral("dap-gdb"), config.gdbPath,
                         { QStringLiteral("-i=dap") }))
        return false;

    // -------------------------------------------------------- leg 9: teardown
    closeEveryTab(window); // tour.py + the probe/future witnesses + the
                           // diff trio (+ the demo.c tab, on the live
                           // face) all leave through the door
    if (window.editorCount() != 0)
        return t.fail(QStringLiteral("teardown"), QStringLiteral("tab count %1 after closing every leg tab, expected 0").arg(window.editorCount()));
    if (window.dapEnabled() || window.lspEnabled())
        return t.fail(QStringLiteral("teardown"), QStringLiteral("a hub still stands at the end of the tour (dap=%1 lsp=%2)").arg(window.dapEnabled()).arg(window.lspEnabled()));
    t.pass(QStringLiteral("teardown"), QStringLiteral("every leg tab closed, every hub disarmed, tab count 0 (a dapfake that survived its terminate arm would leave the leg-8 '(stopped)' pin unearned)"));
    return true;
}

} // namespace qmn::app
