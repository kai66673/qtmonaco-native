// Drives the qmn-demo app (PLAN post-1.0 polish) fully offscreen: the
// tests enter through the SAME programmatic doors the menu slots call
// (MainWindow::openPath / applyEditorSettings / closeTabAt,
// EditorSettingsDialog::setSettings / settings), never exec()ing a
// QFileDialog or the dialog itself - an exec() would block ctest.
// Assertions are on model/widget state, never on pixels.

#include "app/EditorSettings.hpp"
#include "app/EditorSettingsDialog.hpp"
#include "app/MainWindow.hpp"
#include "app/ShowcaseTour.hpp" // S1-4: ctest pins the tour's TABLE

#include "view/CodeEditor.hpp"
#include "view/CodeEditorOptions.hpp"
#include "view/CodeEditorTheme.hpp"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTabBar>
#include <QTemporaryDir>
#include <QStatusBar>
#include <QtTest>

#include <cstdio> // S1-1: the CLI tripwire reads the spawned binary's output

#include <memory>

using qmn::app::collectSettings;
using qmn::app::demoDefaultCodeFont;
using qmn::app::EditorSettings;
using qmn::app::EditorSettingsDialog;
using qmn::app::MainWindow;
using qmn::app::runShowcaseTour; // S1-4
using qmn::app::TourConfig;      // S1-4
using qmn::view::CodeEditor;
using qmn::view::CodeEditorOptions;
using qmn::view::CodeEditorTheme; // M24: the 14th knob's type

// S1-4: the tour prints its pin table through qDebug() (the probe-
// ritual's channel); this ctest slot captures EXACTLY those lines at
// the message handler while the table runs, then restores the handler
// QTest installed. Other debug chatter (none is expected, but the
// filter makes the capture immune to it) never enters the table.
namespace {
QStringList* g_tourLines = nullptr;

void tourTableCapture(QtMsgType type, const QMessageLogContext&,
                      const QString& msg)
{
    if (type == QtDebugMsg && g_tourLines
        && (msg.startsWith(QStringLiteral("PASS "))
            || msg.startsWith(QStringLiteral("SKIP "))
            || msg.startsWith(QStringLiteral("FAIL "))))
        g_tourLines->append(msg);
}
} // namespace

class TestDemo : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> dir_;
    MainWindow* window_ = nullptr;

    // A real family to switch the QFontComboBox through: ANY installed
    // family other than the demo recipe's own. Empty on a fontless box
    // (then the family-switch probe is skipped, the rest still asserts).
    static QStringList fontFamiliesToProbe()
    {
        QStringList out;
        const QString avoid = demoDefaultCodeFont().family();
        const QStringList all = QFontDatabase::families(); // static in Qt 6.4+
        for (const QString& family : all)
            if (family != avoid)
                out << family;
        return out;
    }

    // Plain if/return helper: QVERIFY-style macros cannot be used here
    // (they would `return` with the wrong type).
    QString writeFile(const QString& name, const QByteArray& content)
    {
        const QString path = dir_->filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return {};
        file.write(content);
        file.close();
        return path;
    }

    // Every knob flipped away from the defaults (M20 grew the template
    // with the font/zoom pair; customSettings exercises it too).
    static EditorSettings customSettings()
    {
        EditorSettings s;
        s.lineNumbersMode = CodeEditor::LineNumbersMode::Relative;
        s.currentLineMode = CodeEditor::CurrentLineMode::BackgroundFrame;
        s.longLineRulerColumn = 80;
        s.tabStopWidth = 8;
        s.autoIndentEnabled = false;
        s.tabChangesFocus = true;
        s.overviewRulerEnabled = false;
        s.scrollBeyondLastLineEnabled = true;
        s.autoCloseEnabled = false;
        s.readOnly = true;
        s.indentGuidesEnabled = false; // M21 (13th knob)
        s.softWrapEnabled = true;      // M25 (14th options knob)
        s.themePreset = CodeEditorTheme::Preset::Dark; // M24 (14th knob)
        QFont font = demoDefaultCodeFont(); // same family/strategy (the
        font.setPixelSize(20);              // NO-hinting rule survives)
        s.codeFont = font;
        s.zoomFactor = 1.5;
        return s;
    }

private slots:
    void init()
    {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        window_ = new MainWindow;
    }

    void cleanup()
    {
        delete window_;
        window_ = nullptr;
        dir_.reset();
    }

    void openPathAddsTab()
    {
        const QString path = writeFile(QStringLiteral("hello.txt"), "one\ntwo\n");
        QVERIFY(!path.isEmpty());

        QVERIFY(window_->openPath(path));
        QCOMPARE(window_->editorCount(), 1);
        CodeEditor* editor = window_->editorAt(0);
        QVERIFY(editor);
        QCOMPARE(editor->toPlainText(), QStringLiteral("one\ntwo\n"));
        QCOMPARE(window_->currentEditor(), editor);
        QCOMPARE(window_->tabWidget()->tabText(0), QStringLiteral("hello.txt"));
        QCOMPARE(window_->tabWidget()->currentWidget(), editor);

        // A second open appends and activates; tabData keeps the path.
        const QString other = writeFile(QStringLiteral("second.txt"), "x");
        QVERIFY(window_->openPath(other));
        QCOMPARE(window_->editorCount(), 2);
        QCOMPARE(window_->tabWidget()->currentIndex(), 1);
        QCOMPARE(window_->tabWidget()->tabBar()->tabData(0).toString(), path);
    }

    void openPathFailureIsNonModalAndSilentOnTabs()
    {
        // Missing file and a directory both fail through the isFile gate;
        // either way: false, no new tab, and (no modal dialog to block).
        QVERIFY(!window_->openPath(dir_->filePath(QStringLiteral("missing.txt"))));
        QVERIFY(!window_->openPath(dir_->path()));
        QCOMPARE(window_->editorCount(), 0);
        QVERIFY(window_->editorAt(0) == nullptr);
        QVERIFY(window_->currentEditor() == nullptr);
    }

    void defaultSettingsMirrorFreshEditorDefaults()
    {
        // Tripwire: the demo template's defaults must match the library's
        // documented defaults, read from a FRESH editor's live options().
        CodeEditor fresh;
        const EditorSettings def;
        const CodeEditorOptions* o = fresh.options();
        QVERIFY(def.lineNumbersMode == o->lineNumbersMode());
        QVERIFY(def.currentLineMode == o->currentLineMode());
        QCOMPARE(def.longLineRulerColumn, o->longLineRulerColumn());
        QCOMPARE(def.tabStopWidth, o->tabStopWidth());
        QCOMPARE(def.autoIndentEnabled, o->autoIndentEnabled());
        QCOMPARE(def.tabChangesFocus, o->tabChangesFocus());
        QCOMPARE(def.overviewRulerEnabled, o->overviewRulerEnabled());
        QCOMPARE(def.scrollBeyondLastLineEnabled, o->scrollBeyondLastLineEnabled());
        QCOMPARE(def.autoCloseEnabled, o->autoCloseEnabled());
        QCOMPARE(def.readOnly, o->isReadOnly());
        QCOMPARE(def.indentGuidesEnabled, o->indentGuidesEnabled()); // M21
        QCOMPARE(def.softWrapEnabled, o->softWrapEnabled());         // M25
        QVERIFY(def.themePreset == fresh.theme()->kind()); // M24: default
                                                           // preset = fresh kind
        // M20: the font tripwire is STRICTER than a pixel check — the
        // whole demo recipe (family, size, no-hinting strategy) must
        // equal the library ctor's font definition, or the drift test
        // would be decorative.
        QVERIFY(def.codeFont == o->codeFont());
        QCOMPARE(def.zoomFactor, o->zoomFactor());
    }

    void applySettingsHitsEveryEditor()
    {
        QVERIFY(window_->openPath(writeFile(QStringLiteral("a.txt"), "a\n")));
        QVERIFY(window_->openPath(writeFile(QStringLiteral("b.txt"), "b\n")));

        // M20 numeric proof the font/zoom half APPLIES for real (not just
        // round-trips through the struct): the 20px/1.5 custom pair must
        // grow the caret grid over the shipped 14px/1.0 default.
        const double cellBefore = window_->editorAt(0)->cellWidth();
        QVERIFY(cellBefore > 0.0);

        const EditorSettings custom = customSettings();
        window_->applyEditorSettings(custom);

        QCOMPARE(window_->editorCount(), 2);
        QVERIFY(collectSettings(window_->editorAt(0)) == custom);
        QVERIFY(collectSettings(window_->editorAt(1)) == custom);
        QVERIFY(window_->editorSettings() == custom); // the stored template
        QVERIFY(window_->editorAt(0)->cellWidth() > cellBefore);
        QVERIFY(window_->editorAt(1)->cellWidth() > cellBefore);
    }

    void laterTabsInheritTheTemplate()
    {
        EditorSettings custom = customSettings();
        custom.lineNumbersMode = CodeEditor::LineNumbersMode::Interval;
        window_->applyEditorSettings(custom); // no tabs open yet

        QVERIFY(window_->openPath(writeFile(QStringLiteral("c.txt"), "c\n")));
        QVERIFY(collectSettings(window_->editorAt(0)) == custom);
    }

    void dialogMapsControlsToSettingsRoundTrip()
    {
        // Two whole presets round-trip through the real controls...
        EditorSettingsDialog dialog(window_, window_->editorSettings());
        QVERIFY(dialog.settings() == window_->editorSettings());

        const EditorSettings custom = customSettings();
        dialog.setSettings(custom);
        QVERIFY(dialog.settings() == custom);

        // ...and every enum row maps both ways (the label arrays and the
        // value arrays must stay aligned; row order is the map).
        static constexpr CodeEditor::LineNumbersMode kLineModes[] = {
            CodeEditor::LineNumbersMode::Off,
            CodeEditor::LineNumbersMode::On,
            CodeEditor::LineNumbersMode::Relative,
            CodeEditor::LineNumbersMode::Interval,
        };
        for (const CodeEditor::LineNumbersMode mode : kLineModes) {
            EditorSettings s = customSettings();
            s.lineNumbersMode = mode;
            dialog.setSettings(s);
            QVERIFY(dialog.settings().lineNumbersMode == mode);
        }

        static constexpr CodeEditor::CurrentLineMode kCurrentModes[] = {
            CodeEditor::CurrentLineMode::Off,
            CodeEditor::CurrentLineMode::Background,
            CodeEditor::CurrentLineMode::BackgroundFrame,
        };
        for (const CodeEditor::CurrentLineMode mode : kCurrentModes) {
            EditorSettings s = customSettings();
            s.currentLineMode = mode;
            dialog.setSettings(s);
            QVERIFY(dialog.settings().currentLineMode == mode);
        }

        // M24: theme rows walk both ways. Custom is NOT a row (the demo
        // has no manual role edits), and a Custom seed — reachable only
        // if somebody edits a live editor's theme by hand — clamps to
        // the Default row and reads back as Default.
        static constexpr CodeEditorTheme::Preset kThemePresets[] = {
            CodeEditorTheme::Preset::Default,
            CodeEditorTheme::Preset::Light,
            CodeEditorTheme::Preset::Dark,
        };
        for (const CodeEditorTheme::Preset preset : kThemePresets) {
            EditorSettings s = customSettings();
            s.themePreset = preset;
            dialog.setSettings(s);
            QVERIFY(dialog.settings().themePreset == preset);
        }
        {
            EditorSettings s = customSettings();
            s.themePreset = CodeEditorTheme::Preset::Custom;
            dialog.setSettings(s);
            QVERIFY(dialog.settings().themePreset == CodeEditorTheme::Preset::Default);
        }

        // Spin edges: the ruler special-value text ("нет") still reads
        // back as 0; a tab stop beyond the spin range clamps, and the
        // dialog then hands the CLAMPED value to the editor.
        EditorSettings s = customSettings();
        s.longLineRulerColumn = 0;
        s.tabStopWidth = 99; // above the spin's 32: the control clamps
        dialog.setSettings(s);
        QCOMPARE(dialog.settings().longLineRulerColumn, 0);
        QCOMPARE(dialog.settings().tabStopWidth, 32);

        // M20 edges: out-of-range font px / zoom land on the spins'
        // caps — and the zoom cap is the EDITOR's documented clamp, so
        // the control can never offer a value the setter would refuse.
        s = customSettings();
        QFont huge = demoDefaultCodeFont();
        huge.setPixelSize(9999);
        s.codeFont = huge;
        s.zoomFactor = 99.0;
        dialog.setSettings(s);
        QCOMPARE(dialog.settings().codeFont.pixelSize(), 128); // the spin cap
        QCOMPARE(dialog.settings().zoomFactor, CodeEditor::kZoomFactorMax);
        s.zoomFactor = -5.0;
        dialog.setSettings(s);
        QCOMPARE(dialog.settings().zoomFactor, CodeEditor::kZoomFactorMin);

        // M20 seed carry-through: the dialog edits ONLY family + pixel
        // size, so weight/style/hinting must ride through untouched
        // (QFont equality on the struct's ==), while a family switch
        // lands exactly.
        s = customSettings();
        dialog.setSettings(s);
        QVERIFY(dialog.settings().codeFont == s.codeFont);
        const QStringList families = fontFamiliesToProbe();
        if (!families.isEmpty()) {
            s.codeFont.setFamily(families.first());
            dialog.setSettings(s);
            QCOMPARE(dialog.settings().codeFont.family(), families.first());
            QCOMPARE(dialog.settings().codeFont.pixelSize(), 20);
            QCOMPARE(dialog.settings().zoomFactor, 1.5); // untouched pair
            // And the switched family really reaches the editor door:
            CodeEditor target;
            applySettings(&target, dialog.settings());
            QCOMPARE(collectSettings(&target).codeFont.family(), families.first());
        }
    }

    void settingsDialogAppliesThroughAcceptDoor()
    {
        // The window's accept door is applyEditorSettings; the dialog's
        // job ends at settings(). Verify the pair the slot wires together:
        // collect from the focused editor -> dialog -> apply.
        QVERIFY(window_->openPath(writeFile(QStringLiteral("d.txt"), "d\n")));
        const EditorSettings custom = customSettings();
        window_->applyEditorSettings(custom);

        EditorSettingsDialog dialog(window_, collectSettings(window_->currentEditor()));
        QVERIFY(dialog.settings() == custom); // seeded from the live editor
        window_->applyEditorSettings(dialog.settings());
        QVERIFY(collectSettings(window_->currentEditor()) == custom);
    }

    void grammarGuessFollowsSuffix()
    {
        const QString py = writeFile(QStringLiteral("sample.py"), "a = 1\r\nb = 2\r\n");
        QVERIFY(window_->openPath(py));
        CodeEditor* editor = window_->editorAt(0);
        QVERIFY(editor);
        QCOMPARE(editor->language(), QStringLiteral("python"));
        // CRLF normalized to LF: 2 newlines -> 3 lines (trailing empty).
        QCOMPARE(editor->lineCount(), 3);

        QVERIFY(window_->openPath(writeFile(QStringLiteral("data.json"), "{}")));
        QCOMPARE(window_->editorAt(1)->language(), QStringLiteral("json"));

        // Unmapped suffix stays plain text (setLanguage's documented "").
        QVERIFY(window_->openPath(writeFile(QStringLiteral("plain.txt"), "x")));
        QCOMPARE(window_->editorAt(2)->language(), QString());
    }

    void closingTabShrinksTheSet()
    {
        const QString first = writeFile(QStringLiteral("one.txt"), "1");
        const QString second = writeFile(QStringLiteral("two.txt"), "2");
        QVERIFY(window_->openPath(first));
        QVERIFY(window_->openPath(second));
        QCOMPARE(window_->editorCount(), 2);

        window_->closeTabAt(0); // the tabCloseRequested door
        QCOMPARE(window_->editorCount(), 1);
        QCOMPARE(window_->tabWidget()->tabText(0), QStringLiteral("two.txt"));
        QVERIFY(window_->currentEditor() == window_->editorAt(0));

        // Out-of-range closes are no-ops (the X button never sends those,
        // but the slot must not crash if something else does).
        window_->closeTabAt(7);
        window_->closeTabAt(-1);
        QCOMPARE(window_->editorCount(), 1);
    }

    void diffDoorOpensThreeTabsAndDrivesTheNavThroughTheView()
    {
        // S1-1 tripwire: `--diff A B` opens the trio through the demo
        // doors — plain A, plain B, focused DIFF tab — and the F7/Shift+F7
        // chord drives the hunk walk THROUGH THE VIEW (no direct door
        // calls: this is the KEY side of the tour's diff leg). The corpus
        // is the tour's frozen pair (PLAN S1-4): two replace-hunks at
        // modified lines 1 and 3.
        const QString a = writeFile(QStringLiteral("diff_orig.txt"),
                                    "alpha\none\nbeta\ntwo\ngamma\n");
        const QString b = writeFile(QStringLiteral("diff_mod.txt"),
                                    "alpha\nONE\nbeta\nTWO\ngamma\n");
        QVERIFY(window_->openDiffPair(a, b));
        QCOMPARE(window_->editorCount(), 3);
        QCOMPARE(window_->tabWidget()->currentIndex(), 2); // FOCUS ON DIFF TAB
        CodeEditor* diffTab = window_->currentEditor();
        QVERIFY(diffTab->isDiffMode());
        // The string door re-fed B into the diff tab's OWN document
        // (byte-identical, so the replace feed is a no-op on content) and
        // the two plain tabs stand untouched as witnesses.
        QCOMPARE(diffTab->toPlainText(), QStringLiteral("alpha\nONE\nbeta\nTWO\ngamma\n"));
        QCOMPARE(window_->editorAt(0)->toPlainText(),
                 QStringLiteral("alpha\none\nbeta\ntwo\ngamma\n"));
        QVERIFY(!window_->editorAt(0)->isDiffMode());
        QVERIFY(!window_->editorAt(1)->isDiffMode());

        // The chord walks the hunks on the FOCUSED diff tab (strict-edge
        // rule, no wrap at either edge — the same semantics M10 pinned at
        // the door level, now pinned through the key).
        diffTab->setFocus();
        diffTab->setPosition({0, 0});
        QTest::keyClick(diffTab, Qt::Key_F7);
        QCOMPARE(diffTab->position(), (qmn::view::TextPosition{1, 0}));
        QTest::keyClick(diffTab, Qt::Key_F7);
        QCOMPARE(diffTab->position(), (qmn::view::TextPosition{3, 0}));
        QTest::keyClick(diffTab, Qt::Key_F7);
        QCOMPARE(diffTab->position(), (qmn::view::TextPosition{3, 0}));
        QTest::keyClick(diffTab, Qt::Key_F7, Qt::ShiftModifier);
        QCOMPARE(diffTab->position(), (qmn::view::TextPosition{1, 0}));

        // A failed read opens NOTHING (no half-pair): the missing-B case
        // rides the same status-bar failure route as openPath.
        QCOMPARE(window_->openDiffPair(a, dir_->filePath(QStringLiteral("nope.txt"))), false);
        QCOMPARE(window_->editorCount(), 3);
    }

    void diffCliDoorCollectsThePairAndFailsLoudOnMisShape()
    {
        // S1-1: the PROCESS-LEVEL pin of the `--diff` CLI wiring (the
        // in-process slot above never touches the parser). The pre-fix
        // four-arg addOption spelling collected ONE value and let the
        // trio silently never open, exit 0 — invisible at every other
        // layer, so the CLI is pinned by SPAWNING the real qmn-demo (the
        // same CI-built-binary spawn class as the dapfake slots; the
        // --exit-after-ms self-quit keeps it bounded under offscreen).
        const QString a = writeFile(QStringLiteral("cli_a.txt"), "p\nq r\n");
        const QString b = writeFile(QStringLiteral("cli_b.txt"), "p\nq S\n");
        const QString bin = QString::fromLatin1(QMN_TEST_DEMO_BIN);

        // QProcess is non-movable: the caller owns one and the lambda runs
        // it to completion.
        auto runDemo = [&](QProcess& proc, const QStringList& args) {
            proc.setProcessChannelMode(QProcess::SeparateChannels);
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       QStringLiteral("offscreen")); // never inherit a live display
            proc.setProcessEnvironment(env);
            proc.start(bin, args);
            QVERIFY2(proc.waitForStarted(10000), qPrintable(proc.errorString()));
            QVERIFY(proc.waitForFinished(30000));
        };

        // The GOOD shape: exactly two after the flag -> trio opens, clean
        // exit 0 (the self-quit fires; nothing hangs). The pair-at-FRONT
        // leg is load-bearing: QCommandLineParser::process(QStringList)
        // skips the list's first element as argv[0] (measured on 6.12) —
        // a scan rewrite that forgets to prepend the app name DROPS the
        // leading flag, the self-quit timer vanishes, and the spawned app
        // hangs until waitForFinished times out (this pin caught it).
        QProcess good;
        runDemo(good, {QStringLiteral("--diff"), a, b,
                       QStringLiteral("--exit-after-ms"), QStringLiteral("300")});
        QCOMPARE(good.exitStatus(), QProcess::NormalExit);
        QCOMPARE(good.exitCode(), 0);

        // Mid-list pair + a positional file before it: everything opens,
        // the timer survives the scan rewrite.
        QProcess mid;
        runDemo(mid, {b, QStringLiteral("--diff"), a, b,
                      QStringLiteral("--exit-after-ms"), QStringLiteral("300")});
        QCOMPARE(mid.exitStatus(), QProcess::NormalExit);
        QCOMPARE(mid.exitCode(), 0);

        // The BAD shapes: zero and one values — LOUD stderr (the qWarning
        // route proved invisible on some runs) and exit 1, never a silent
        // zero the pre-fix bug rode on.
        for (const QStringList& bad :
             {QStringList{QStringLiteral("--diff"),
                          QStringLiteral("--exit-after-ms"), QStringLiteral("300")},
              QStringList{QStringLiteral("--diff"), a,
                          QStringLiteral("--exit-after-ms"), QStringLiteral("300")}}) {
            QProcess proc;
            runDemo(proc, bad);
            QCOMPARE(proc.exitStatus(), QProcess::NormalExit);
            QCOMPARE(proc.exitCode(), 1);
            const QString err = QString::fromUtf8(proc.readAllStandardError());
            QVERIFY2(err.contains(QStringLiteral("EXACTLY two files")),
                     qPrintable(err));
        }
    }

    void grammarDoorLoadsRebindsAndRefusesTheBogusLibrary()
    {
        // S1-2: the full grammar door through DEMO doors only, on the two
        // fixture libraries (the tst_codeeditor precedent). The corpus is
        // one frozen python snippet opened TWICE: as witness.py (the
        // built-in `python` tab — the golden table) and probe.pyext (the
        // target id's tab). The engine's pinned truth FIRST: an UNKNOWN id
        // at setLanguage leaves language() EMPTY (HighlightEngine.cpp:
        // "language() stays empty") — so "pyext-bound before the load"
        // honestly means language()=="" + zero spans, and the door's
        // REBIND face fires on tabs already SPEAKING the id: the same-id
        // RELOAD (upsert) leg pins that face with вкладок: 1.
        const QString content =
            "import demo\n\n"
            "def greet(name):\n"
            "    return f\"hi {name}\"  # greet\n";
        const QStringList baseline = CodeEditor::availableLanguages();
        QVERIFY(!baseline.contains(QStringLiteral("pyext")));

        QVERIFY(window_->openPath(writeFile(QStringLiteral("witness.py"),
                                            content.toUtf8())));
        CodeEditor* witness = window_->currentEditor();
        QCOMPARE(witness->language(), QStringLiteral("python"));
        const int len = witness->toPlainText().size();
        const auto golden = witness->highlightSpans(0, len);
        QVERIFY(!golden.isEmpty()); // the built-in table is ALIVE first

        QVERIFY(window_->openPath(writeFile(QStringLiteral("probe.pyext"),
                                            content.toUtf8())));
        CodeEditor* probe = window_->currentEditor();
        QVERIFY(window_->editorCount() == 2);
        QCOMPARE(probe->language(), QString()); // .pyext unmapped: plain
        QVERIFY(probe->highlightSpans(0, probe->toPlainText().size()).isEmpty());

        // --- the load face ------------------------------------------------
        QVERIFY(window_->loadGrammarLibrary(
            QStringLiteral(QMN_TEST_PY_GRAMMAR), QStringLiteral("pyext"),
            QString(), QStringLiteral("tree_sitter_python"),
            QStringLiteral(QMN_TEST_PY_QUERY))); // the vendored py query
        // (language defaults to the id; the symbol is EXPLICIT because the
        // stem says `pygrammar` while the export says `tree_sitter_python`)
        QVERIFY(CodeEditor::availableLanguages() == baseline + QStringList{QStringLiteral("pyext")});
        QCOMPARE(window_->statusBar()->currentMessage(),
                 QStringLiteral("Грамматика «pyext» загружена (язык «pyext», вкладок: 0)"));
        // The probe tab is STILL plain: reconcileRegistry early-returns on
        // the plain-text state ("nothing attached can care"), so a
        // registration never ATTACHES a language to a tab that never
        // spoke the id — it only reconciles tabs ALREADY speaking it:
        QCOMPARE(probe->language(), QString());
        probe->setLanguage(QStringLiteral("pyext"));
        QCOMPARE(probe->language(), QStringLiteral("pyext"));
        const auto mine = probe->highlightSpans(0, probe->toPlainText().size());
        // THE span-table pin: the second compiled parser + the SAME vendored
        // query == the built-in python tab's table, row for row (drift here
        // is the drift the tour leg 5 kills).
        QVERIFY2(mine.size() == golden.size() && mine == golden,
                 qPrintable(QStringLiteral("pyext spans=%1 vs python spans=%2")
                                .arg(mine.size()).arg(golden.size())));

        // --- the UPSERT face: same-id RELOAD swaps the query UNDER the
        // open tab through the engine's registry-generation reconcile
        // (the tst_codeeditor live-UPSERT leg's mechanism — no setLanguage
        // call is what lights this). The query rides as a REAL .scm file:
        // the door's tail arg is a query PATH (readTextFile), never query
        // text. The fixture is COMMENT-only; the corpus string is an
        // f-string, so a (string) probe would be vacuously empty HERE —
        // the comment face is the honest one.
        const QString commentsScm =
            writeFile(QStringLiteral("comments_only.scm"), "(comment) @comment\n");
        QVERIFY(window_->loadGrammarLibrary(
            QStringLiteral(QMN_TEST_PY_GRAMMAR), QStringLiteral("pyext"),
            QStringLiteral("pyext"), QStringLiteral("tree_sitter_python"),
            commentsScm));
        QCOMPARE(window_->statusBar()->currentMessage(),
                 QStringLiteral("Грамматика «pyext» загружена (язык «pyext», вкладок: 1)"));
        const auto shrunk = probe->highlightSpans(0, probe->toPlainText().size());
        QVERIFY(!shrunk.isEmpty());
        QVERIFY(shrunk.size() < golden.size());
        bool commentsOnly = true;
        for (const auto& s : shrunk)
            commentsOnly = commentsOnly && s.kind == CodeEditor::TokenKind::Comment;
        QVERIFY2(commentsOnly, "query swap not live on the open tab");
        QCOMPARE(probe->language(), QStringLiteral("pyext")); // id kept

        // --- the FUTURE-TAB face: the session's table (G2) through the
        // new forwarding door
        window_->setLanguageForSuffix(QStringLiteral("pyext"),
                                      QStringLiteral("pyext"));
        QVERIFY(window_->openPath(writeFile(QStringLiteral("future.pyext"),
                                            content.toUtf8())));
        CodeEditor* future = window_->currentEditor();
        QCOMPARE(future->language(), QStringLiteral("pyext"));
        const auto futureSpans = future->highlightSpans(0, future->toPlainText().size());
        QVERIFY2(futureSpans == shrunk,
                 QStringLiteral("future tab did not claim the loaded id").toUtf8().constData());

        // --- the refusal faces, each false + exact status + NO registry
        // change: the LIBRARY face...
        QVERIFY(!window_->loadGrammarLibrary(QStringLiteral("/no/such/qmn-grammar.so"),
                                             QStringLiteral("nope")));
        QCOMPARE(window_->statusBar()->currentMessage(),
                 QStringLiteral("Не удалось загрузить библиотеку грамматики "
                                "«/no/such/qmn-grammar.so»"));
        // ...the QUERY face (the library LOADS first — keeping the handle
        // is the view's documented stance — but the table must not move):
        QVERIFY(!window_->loadGrammarLibrary(QStringLiteral(QMN_TEST_PY_GRAMMAR),
                                             QStringLiteral("pyext2"), QString(),
                                             QStringLiteral("tree_sitter_python"),
                                             dir_->filePath(QStringLiteral("nope.scm"))));
        QCOMPARE(window_->statusBar()->currentMessage(),
                 QStringLiteral("Не удалось открыть «%1»: это не файл")
                     .arg(dir_->filePath(QStringLiteral("nope.scm"))));
        QVERIFY(!CodeEditor::availableLanguages().contains(QStringLiteral("pyext2")));
        // ...and the VALIDATION face: libbogus.so LOADS with a non-zero
        // address (tree_sitter_bogus resolves), yet the view's door
        // refuses the zeroed language at its own scratch-parser check —
        // the demo's status line tells it.
        QVERIFY(!window_->loadGrammarLibrary(QStringLiteral(QMN_TEST_FAKE_GRAMMAR),
                                             QStringLiteral("bogus")));
        QCOMPARE(window_->statusBar()->currentMessage(),
                 QStringLiteral("Библиотека «%1» отклонена: не подходящая грамматика")
                     .arg(QStringLiteral(QMN_TEST_FAKE_GRAMMAR)));
        QVERIFY(CodeEditor::availableLanguages()
                == baseline + QStringList{QStringLiteral("pyext")}); // nothing else landed
        QCOMPARE(probe->highlightSpans(0, probe->toPlainText().size()).size(),
                 shrunk.size()); // the live tabs unchanged by the refusals
        QCOMPARE(probe->language(), QStringLiteral("pyext")); // no half-
                                                              // applied state anywhere

        // --- teardown: unregister (the view's static door — the registry
        // is process-wide and tst_demo runs every slot in ONE process:
        // leaving pyext registered would poison the S1-4 tour slot's
        // before-face) + the probe tab falls back to plain on its next
        // engine pass, exactly as tst_codeeditor pinned at the view.
        QVERIFY(CodeEditor::unregisterGrammar(QStringLiteral("pyext")));
        window_->setLanguageForSuffix(QStringLiteral("pyext"), QString());
        QVERIFY(probe->highlightSpans(0, probe->toPlainText().size()).isEmpty());
        QCOMPARE(probe->language(), QString());
        QVERIFY(CodeEditor::availableLanguages() == baseline);
    }

    void grammarCliDoorZipsOccurrencesAndFailsLoud()
    {
        // S1-1's spawn class again: the CLI zip is invisible in-process.
        // The POSITIVE leg omits NOTHING: the symbol flag is exactly what
        // the stem derivation would get WRONG here (libpygrammar.so ->
        // tree_sitter_pygrammar resolves to 0), so a silent companion
        // drop — the S1-1 zip trap in its other direction — flips this
        // leg to exit 1 by itself, no extra pin needed for that face.
        const QString bin = QString::fromLatin1(QMN_TEST_DEMO_BIN);
        auto runDemo = [&](QProcess& proc, const QStringList& args) {
            proc.setProcessChannelMode(QProcess::SeparateChannels);
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       QStringLiteral("offscreen"));
            proc.setProcessEnvironment(env);
            proc.start(bin, args);
            QVERIFY2(proc.waitForStarted(10000), qPrintable(proc.errorString()));
            QVERIFY(proc.waitForFinished(30000));
        };

        QProcess good;
        runDemo(good, {QStringLiteral("--grammar"), QStringLiteral(QMN_TEST_PY_GRAMMAR),
                       QStringLiteral("--grammar-id"), QStringLiteral("pyext"),
                       QStringLiteral("--grammar-symbol"),
                       QStringLiteral("tree_sitter_python"),
                       QStringLiteral("--grammar-query"),
                       QStringLiteral(QMN_TEST_PY_QUERY),
                       QStringLiteral("--grammar-suffix"),
                       QStringLiteral("pyext:pyext"),
                       QStringLiteral("--exit-after-ms"), QStringLiteral("300")});
        QCOMPARE(good.exitStatus(), QProcess::NormalExit);
        QCOMPARE(good.exitCode(), 0);

        // The stem-derived NEGATIVE (no --grammar-symbol): loadGrammar
        // misses tree_sitter_pygrammar, the door's library face fires,
        // stderr + exit 1 — never the old silent zero.
        QProcess stem;
        runDemo(stem, {QStringLiteral("--grammar"), QStringLiteral(QMN_TEST_PY_GRAMMAR),
                       QStringLiteral("--grammar-id"), QStringLiteral("pyext"),
                       QStringLiteral("--grammar-query"),
                       QStringLiteral(QMN_TEST_PY_QUERY),
                       QStringLiteral("--exit-after-ms"), QStringLiteral("300")});
        QCOMPARE(stem.exitStatus(), QProcess::NormalExit);
        QCOMPARE(stem.exitCode(), 1);
        const QString err = QString::fromUtf8(stem.readAllStandardError());
        QVERIFY2(err.contains(QStringLiteral("--grammar failed")), qPrintable(err));

        // Malformed --grammar-suffix pairs (no colon, empty halves) are
        // LOUD too.
        QProcess bad;
        runDemo(bad, {QStringLiteral("--grammar-suffix"), QStringLiteral("pyext"),
                      QStringLiteral("--exit-after-ms"), QStringLiteral("300")});
        QCOMPARE(bad.exitStatus(), QProcess::NormalExit);
        QCOMPARE(bad.exitCode(), 1);
        QVERIFY(QString::fromUtf8(bad.readAllStandardError())
                    .contains(QStringLiteral("<suffix:<id>")));
    }

    void lspDoorOffByDefaultAndUnclaimedLanguagesNeverSpawn()
    {
        // The --lsp door (the M8 popup-trigger demo path): the DEFAULT is
        // OFF, and this remains the one demo slot that touches the hub at
        // all — no OTHER drive may spawn a real server (clangd is not a CI
        // guarantee). Even armed, only languages a built-in spec claims
        // register: python/txt honestly stay plain editors, so this slot
        // proves the hub never spawns anything here.
        QVERIFY(!window_->lspEnabled());
        QVERIFY(window_->openPath(writeFile(QStringLiteral("plain.txt"), "x")));
        QVERIFY(!window_->lspEnabled()); // openPath never turns the door on

        window_->setLspEnabled(true);
        QVERIFY(window_->lspEnabled());
        QVERIFY(window_->openPath(
            writeFile(QStringLiteral("unclaimed.py"), "x = 1\n")));
        window_->closeTabAt(0); // closeDocument for an UNregistered tab:
                                // the documented no-op
        QCOMPARE(window_->editorCount(), 1); // the plain.txt tab stays
        window_->setLspEnabled(false);
        QVERIFY(!window_->lspEnabled());
        window_->setLspEnabled(true); // idempotent both ways: never a
        window_->setLspEnabled(true); // second hub, never an early death
        window_->setLspEnabled(false);
        QVERIFY(!window_->lspEnabled());
    }

    // ---- the --dap door (car 4, machine D4) --------------------------------
    // The recorded difference from the --lsp rule above: dapfake IS built by
    // CI (tests/dapfake), so these two slots drive REAL spawns through the
    // demo door. tst_demo still names NO glue/dap type — every truth is read
    // through the demo's own doors + the editor's M5 decorations.

    static int dapDiagCount(const CodeEditor* ed)
    {
        int n = 0;
        for (const CodeEditor::Diagnostic& diag : ed->diagnostics())
            if (diag.source == QLatin1String("dap"))
                ++n;
        return n;
    }

    static bool dapDiagAt(const CodeEditor* ed, int line, const QString& message)
    {
        for (const CodeEditor::Diagnostic& diag : ed->diagnostics())
            if (diag.source == QLatin1String("dap") && diag.startLine == line
                && diag.message == message)
                return true;
        return false;
    }

    void dapDoorOffByDefaultAndTripwireFeedsTheGutterThroughTheDemo()
    {
        // The full chain through DEMO doors only: CLI door -> hub -> real
        // dapfake spawn -> setBreakpoints (wire 1-based) -> reply + the
        // `breakpoint` EVENT -> the editor's M5 gutter feeds. The hub's
        // EDIT-RESEND demand (dapfake's disconnect arm refuses to exit 0
        // after fewer than 2 setBreakpoints calls) is paid here too.
        QVERIFY(!window_->dapEnabled()); // the DEFAULT is OFF
        const QString file = writeFile(
            QStringLiteral("daptrip.txt"),
            "l0\nl1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\n");
        QVERIFY(window_->openPath(file));
        QVERIFY(!window_->dapEnabled()); // openPath never turns the door on

        // Arm the door AFTER the tab exists: the late-arm leg auto-starts
        // the session on the first tab, and the expected source.path argv
        // pins the hub's uri->localPath resolution end-to-end.
        window_->setDapAdapter(QString::fromLatin1(QMN_TEST_DAP_SERVER),
                               {QStringLiteral("breakpoints"),
                                QFileInfo(file).absoluteFilePath()});
        QVERIFY(window_->dapEnabled());

        QTRY_COMPARE_WITH_TIMEOUT(
            window_->statusBar()->currentMessage(),
            QStringLiteral("DAP: сессия запущена"), 30000);

        CodeEditor* ed = window_->editorAt(0);
        QVERIFY(ed);
        // The MERGED post-event truth (the D3 lesson: the `breakpoint`
        // event lands in the same batch as the reply — the intermediate
        // dots {2,6}-without-diag must never satisfy the pin): the demo
        // stored model lines {2,6}; dapfake answers both verified at the
        // SAME line, then the first call's FIRST id (model 2) is pulled
        // unverified ("dapfake pulled the dot"):
        QTRY_VERIFY_WITH_TIMEOUT(
            ed->breakpointLines() == (QVector<int>{6})
                && dapDiagAt(ed, 2, QStringLiteral("dapfake pulled the dot")),
            15000);

        // The edit-RESEND leg: a line-anchored store re-speaks the same
        // model lines after a document edit; call 2 answers both verified
        // with fresh ids — so the pulled dot COMES BACK and the dap row
        // goes away (the hub's producer-keyed diag merge, not a wipe).
        QCOMPARE(ed->applyTextEdits({{0, 0, 0, 0, QStringLiteral("z")}}), 1);
        QTRY_VERIFY_WITH_TIMEOUT(
            ed->breakpointLines() == (QVector<int>{2, 6})
                && dapDiagCount(ed) == 0,
            15000);

        // The clean teardown through the door (empty adapterProgram = OFF):
        // orderly stopDebugging -> disconnect -> dapfake's arm is satisfied
        // (2 setBreakpoints calls) and it exits 0; the dtor's bounded pump
        // does the rest while the status bar still speaks.
        window_->setDapAdapter(QString());
        QVERIFY(!window_->dapEnabled());
        QCOMPARE(window_->statusBar()->currentMessage(),
                 QStringLiteral("DAP: адаптер остановлен"));
    }

    void dapDoorShowsTheLaunchRefusalInTheStatusBar()
    {
        // launchreject mode: the READY leg's `launch` gets the DAP error
        // envelope (a real adapter refusing the program is the everyday
        // shape). The hub RELAYS launchRejected and KEEPS the session (the
        // recorded split: relaunch-vs-stop is the app's call) — the demo
        // door tells the refusal in the status bar and never self-kills.
        QVERIFY(window_->openPath(
            writeFile(QStringLiteral("daprej.txt"), "r0\nr1\nr2\nr3\n")));
        window_->setDapAdapter(QString::fromLatin1(QMN_TEST_DAP_SERVER),
                               {QStringLiteral("launchreject")});
        QVERIFY(window_->dapEnabled());

        QTRY_COMPARE_WITH_TIMEOUT(
            window_->statusBar()->currentMessage(),
            QStringLiteral("DAP: пуск отклонён: dapfake cannot run this program"),
            30000);
        QVERIFY(window_->dapEnabled()); // the refusal never kills the door
        QCOMPARE(window_->editorCount(), 1); // and never kills the tab

        window_->setDapAdapter(QString());
        QVERIFY(!window_->dapEnabled());
    }

    void dapControlDoorsStepStopAndReapThroughTheDemo()
    {
        // Machine S1-3 through the DEMO doors: the steps2 walk — pause →
        // step → pause at the NEW frame (band/caret/hints move; OWNER-only
        // caret is fine, the tour opens one tab) → continue → ended. The
        // exact Russian status strings are the tour's pin literals, so the
        // demo face pins them HERE byte-for-byte; and dapfake's terminate-
        // before-disconnect arm is the far-side receipt of debugStop's
        // terminate-first routing (a disconnect that outran terminate would
        // exit 2 and land the sessionEnded string as a transport-death
        // reason, failing the "(stopped)" pin below).
        const QString file = writeFile(
            QStringLiteral("dapctrl.txt"),
            "top line\n    int y = 8\n    return x + y\n"
            "x revisited\nlast line\n");
        QVERIFY(window_->openPath(file));
        CodeEditor* ed = window_->editorAt(0);
        QVERIFY(ed);
        window_->setDapAdapter(QString::fromLatin1(QMN_TEST_DAP_SERVER),
                               {QStringLiteral("steps2"),
                                QFileInfo(file).absoluteFilePath(),
                                QStringLiteral("3"), QStringLiteral("6"),
                                QStringLiteral("4")});
        QVERIFY(window_->dapEnabled()); // armed LATE: the late-arm leg
                                        // auto-starts on the first tab

        // The demo's stored pair {2,6} re-pushed on Ready (store-BEFORE-
        // ready, the hub's legal order); steps2 answers ALL verified at the
        // same line — the dots are witnesses for this walk.
        QTRY_VERIFY_WITH_TIMEOUT(
            ed->breakpointLines() == (QVector<int>{2, 6}), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(
            window_->statusBar()->currentMessage(),
            QStringLiteral("DAP: пауза в потоке 7 (breakpoint)"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(ed->highlightedLinesStart(), 2, 5000);
        QCOMPARE(ed->position().line, 2); // frame (3, 6) -> model (2, 5)
        QCOMPARE(ed->position().col, 5);
        QTRY_COMPARE_WITH_TIMEOUT(ed->inlayHints().size(), 2, 5000);

        QVERIFY(window_->debugStep()); // the everyday Step button = `next`
        QTRY_COMPARE_WITH_TIMEOUT(
            window_->statusBar()->currentMessage(),
            QStringLiteral("DAP: пауза в потоке 7 (step)"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(ed->highlightedLinesStart(), 3, 5000);
        QTRY_COMPARE(ed->position().line, 3);
        QCOMPARE(ed->position().col, 5); // the frame COLUMN rode unchanged
        QTRY_COMPARE_WITH_TIMEOUT(ed->inlayHints().size(), 1, 5000);
        QCOMPARE(ed->inlayHints().at(0).line, 3);
        QCOMPARE(ed->inlayHints().at(0).col, 1); // one past the x on the
                                                 // new frame line (y is off it)
        QCOMPARE(ed->inlayHints().at(0).text, QStringLiteral(" = 42"));

        QVERIFY(window_->debugContinue());
        QTRY_COMPARE_WITH_TIMEOUT(window_->statusBar()->currentMessage(),
                                  QStringLiteral("DAP: продолжено"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(ed->highlightedLinesStart(), -1, 5000);
        QVERIFY(ed->inlayHints().isEmpty()); // cleared in the same chain

        QVERIFY(window_->debugStop()); // terminate-first where the caps allow
        QTRY_COMPARE_WITH_TIMEOUT(
            window_->statusBar()->currentMessage(),
            QStringLiteral("DAP: сессия завершена (stopped)"), 5000);

        // The refusal faces AFTER the end, through the same doors (the hub
        // stands, the session does not): honest status strings, no wire.
        QVERIFY(!window_->debugStep());
        QCOMPARE(window_->statusBar()->currentMessage(),
                 QStringLiteral("DAP: шаг отклонён: нет паузы в готовой сессии"));
        QVERIFY(!window_->debugContinue());
        QCOMPARE(window_->statusBar()->currentMessage(),
                 QStringLiteral("DAP: продолжение отклонено: "
                                "нет паузы в готовой сессии"));
        QVERIFY(!window_->debugStop());
        QCOMPARE(window_->statusBar()->currentMessage(),
                 QStringLiteral("DAP: остановка отклонена: нет активной сессии"));

        // The disarm face; the steps2 terminate arm already granted the
        // orderly exit, so this teardown rides an already-dead session.
        window_->setDapAdapter(QString());
        QVERIFY(!window_->dapEnabled());
        QCOMPARE(window_->statusBar()->currentMessage(),
                 QStringLiteral("DAP: адаптер остановлен"));
    }

    void debugRideQueueWalksTheSessionWithoutAnyManualPress()
    {
        // The S1-3 CLI face IN PROCESS: the flags' queue (a startup door
        // call could only ever refuse — the session is async, so the CLI
        // queues presses into the hub's event stream instead). Nothing
        // here presses a door by hand: the ride machine must step,
        // continue and terminate itself off the hub's own signals, and
        // the SAME status-string sequence must land. dapfake's steps2
        // terminate arm is again the far-side receipt of the order.
        const QString file = writeFile(
            QStringLiteral("dapride.txt"),
            "top line\n    int y = 8\n    return x + y\n"
            "x revisited\nlast line\n");
        QVERIFY(window_->openPath(file));
        CodeEditor* ed = window_->editorAt(0);
        QVERIFY(ed);
        window_->setDapAdapter(QString::fromLatin1(QMN_TEST_DAP_SERVER),
                               {QStringLiteral("steps2"),
                                QFileInfo(file).absoluteFilePath(),
                                QStringLiteral("3"), QStringLiteral("6"),
                                QStringLiteral("4")});
        QVERIFY(window_->dapEnabled());
        window_->queueDebugRide({QStringLiteral("step"),
                                 QStringLiteral("continue"),
                                 QStringLiteral("stop")});

        QTRY_COMPARE_WITH_TIMEOUT(
            window_->statusBar()->currentMessage(),
            QStringLiteral("DAP: пауза в потоке 7 (breakpoint)"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(
            window_->statusBar()->currentMessage(),
            QStringLiteral("DAP: пауза в потоке 7 (step)"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(ed->position().line, 3, 15000); // the ride's
        //  step moved the caret itself — the ONLY hand on this session is
        //  the queue
        QTRY_COMPARE_WITH_TIMEOUT(window_->statusBar()->currentMessage(),
                                  QStringLiteral("DAP: продолжено"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(
            window_->statusBar()->currentMessage(),
            QStringLiteral("DAP: сессия завершена (stopped)"), 15000);

        window_->setDapAdapter(QString());
        QVERIFY(!window_->dapEnabled());
        QCOMPARE(window_->statusBar()->currentMessage(),
                 QStringLiteral("DAP: адаптер остановлен"));
    }

    void debugCliRideFlagsNeedTheAdapterAndRideClean()
    {
        // The CLI FLAGS themselves (the spawn class again): the parser
        // registration is pinned by BOTH faces — an unregistered/misbound
        // flag (the Qt6 silent-dead-door trap class) flips the POSITIVE
        // leg to exit 1 right at the parser, and the flags-without---dap
        // misshape is LOUD by its own door (stderr + exit 1). The ride's
        // SEMANTICS are pinned in-process by the queue slot and on the
        // wire by tst_dap — this slot pins the CLI shape only.
        const QString bin = QString::fromLatin1(QMN_TEST_DEMO_BIN);
        auto runDemo = [&](QProcess& proc, const QStringList& args) {
            proc.setProcessChannelMode(QProcess::SeparateChannels);
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       QStringLiteral("offscreen"));
            proc.setProcessEnvironment(env);
            proc.start(bin, args);
            QVERIFY2(proc.waitForStarted(10000), qPrintable(proc.errorString()));
            QVERIFY(proc.waitForFinished(30000));
        };

        // Flags without --dap: loud refusal at startup, never a silent
        // zero with three doors that can only refuse.
        QProcess nodap;
        runDemo(nodap, {QStringLiteral("--debug-step"),
                        QStringLiteral("--exit-after-ms"),
                        QStringLiteral("300")});
        QCOMPARE(nodap.exitStatus(), QProcess::NormalExit);
        QCOMPARE(nodap.exitCode(), 1);
        QVERIFY2(QString::fromUtf8(nodap.readAllStandardError())
                     .contains(QStringLiteral("--debug-step")),
                 qPrintable(QString::fromUtf8(nodap.readAllStandardError())));

        // The full walk through the REAL CLI: adapter + file + all three
        // flags. The parser ACCEPTS every flag (else exit 1 before the
        // window even exists) and the app dies on its own timer,
        // normally, exit 0.
        const QString file = writeFile(
            QStringLiteral("dapcli.txt"),
            "top line\n    int y = 8\n    return x + y\n"
            "x revisited\nlast line\n");
        QProcess good;
        runDemo(good, {QStringLiteral("--dap"),
                       QString::fromLatin1(QMN_TEST_DAP_SERVER),
                       QStringLiteral("--dap-arg"), QStringLiteral("steps2"),
                       QStringLiteral("--dap-arg"), file,
                       QStringLiteral("--dap-arg"), QStringLiteral("3"),
                       QStringLiteral("--dap-arg"), QStringLiteral("6"),
                       QStringLiteral("--dap-arg"), QStringLiteral("4"),
                       file,
                       QStringLiteral("--debug-step"),
                       QStringLiteral("--debug-continue"),
                       QStringLiteral("--debug-stop"),
                       QStringLiteral("--exit-after-ms"),
                       QStringLiteral("4000")});
        QCOMPARE(good.exitStatus(), QProcess::NormalExit);
        QCOMPARE(good.exitCode(), 0);
    }

    void tourCliDoorFailsLoudOnBadTailAndMissingNamedAdapter()
    {
        // Machine S1-4's process-level pin of the `--tour` CLI wiring —
        // deliberately the LOUD FACES ONLY, so no spawn ever runs the
        // table (the tour stays a live ritual, never a ctest entry; the
        // in-process slot below pins the table itself). Both legs must
        // exit BEFORE the first printed pin: the tail-shape refusal
        // rides the parser, the named-adapter refusal rides the config
        // resolution — a face that printed a table first would mean the
        // refusal landed after the run started.
        const QString bin = QString::fromLatin1(QMN_TEST_DEMO_BIN);
        auto runTour = [&](QProcess& proc, const QStringList& args,
                           const QProcessEnvironment& env) {
            proc.setProcessChannelMode(QProcess::SeparateChannels);
            proc.setProcessEnvironment(env);
            proc.start(bin, args);
            QVERIFY2(proc.waitForStarted(10000), qPrintable(proc.errorString()));
            QVERIFY(proc.waitForFinished(30000));
        };

        QProcessEnvironment base = QProcessEnvironment::systemEnvironment();
        base.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));

        // Four tail paths: the refusal, then exit 1, no table.
        QProcessEnvironment fourTail = base; // no tour env: nothing to resolve
        QProcess four;
        runTour(four, {QStringLiteral("--tour"), QStringLiteral("a"),
                       QStringLiteral("b"), QStringLiteral("c"),
                       QStringLiteral("d")}, fourTail);
        QCOMPARE(four.exitStatus(), QProcess::NormalExit);
        QCOMPARE(four.exitCode(), 1);
        const QString fourErr = QString::fromUtf8(four.readAllStandardError());
        QVERIFY2(fourErr.contains(QStringLiteral("AT MOST three tail paths")),
                 qPrintable(fourErr));
        QVERIFY(!fourErr.contains(QStringLiteral("PASS 1:")));

        // A NAMED-but-missing adapter: loud stderr + exit 1 (the
        // "a missing named path fails loud" law), still no table line.
        QProcessEnvironment named = base;
        named.insert(QStringLiteral("QMN_TOUR_NO_LIVE_DAP"), QStringLiteral("1"));
        named.insert(QStringLiteral("QMN_TOUR_GDB"), QStringLiteral("/no/such/gdb"));
        QProcess bad;
        runTour(bad, {QStringLiteral("--tour")}, named);
        QCOMPARE(bad.exitStatus(), QProcess::NormalExit);
        QCOMPARE(bad.exitCode(), 1);
        const QString badErr = QString::fromUtf8(bad.readAllStandardError());
        QVERIFY2(badErr.contains(QStringLiteral("QMN_TOUR_GDB names a program")),
                 qPrintable(badErr));
        QVERIFY(!badErr.contains(QStringLiteral("PASS 1:")));
    }

    void tourTableRunsGreenMinusTheServerLegs()
    {
        // Machine S1-4's ctest face: `runShowcaseTour` over the REAL
        // MainWindow, fixtures from the compile-time defines, and the
        // three PATH-discovered legs (clangd, lldb-dap, gdb) FORCED off
        // — the tour NEVER spawns them from ctest (the --lsp no-
        // guarantee law stays in force; dapfake IS spawned, the CI-
        // built far-side oracle). The printed table IS the product, so
        // it is captured at the message handler and pinned leg-for-leg:
        // 14 lines, frozen order, 11 PASS + 3 SKIP, zero FAIL, return
        // true. A table that drifts fails HERE; the LIVE ritual (never
        // ctest) is the face that keeps the server legs. Slot ORDER is
        // load-bearing: this slot runs LAST, after the grammar slots'
        // own unregister-pyext teardown, so leg 6's BEFORE-face (an
        // honest "pyext absent") holds; the tour leaves pyext
        // registered behind itself and nothing runs after to be
        // poisoned.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TourConfig config;
        config.corpusDir = dir.path();
        config.dapfakePath = QString::fromLatin1(QMN_TEST_DAP_SERVER);
        config.pyGrammarLib = QString::fromLatin1(QMN_TEST_PY_GRAMMAR);
        config.pyQueryScm = QString::fromLatin1(QMN_TEST_PY_QUERY);
        config.fakeGrammarLib = QString::fromLatin1(QMN_TEST_FAKE_GRAMMAR);
        config.clangdEnabled = false; // the tour's demo.c stays a plain
                                      // C tab here — no server, ever
        // lldbDapPath/gdbPath stay EMPTY on purpose: the live legs
        // print their honest SKIPs (the host ritual owns their faces;
        // tools/probe_dap.sh owns the exact live pins). The reason
        // states the TRUTH of this face — forced off, not missing.
        config.liveDapSkipReason = QStringLiteral(
            "%1: the ctest face forces the PATH legs off (the --lsp "
            "no-guarantee law) — the host ritual owns these rows");

        QStringList lines;
        g_tourLines = &lines;
        const QtMessageHandler previous = qInstallMessageHandler(tourTableCapture);
        const bool green = runShowcaseTour(config, *window_);
        qInstallMessageHandler(previous);
        g_tourLines = nullptr;

        QVERIFY2(green,
                 qPrintable(lines.filter(QStringLiteral("FAIL ")).join(QLatin1Char('\n'))));
        const QStringList frozen{
            QStringLiteral("PASS 1: corpus — "),
            QStringLiteral("PASS 2: boot — "),
            QStringLiteral("PASS 3: model — "),
            QStringLiteral("PASS 4: highlight — "),
            QStringLiteral("PASS 5: diff — "),
            QStringLiteral("PASS 6: grammar — "),
            QStringLiteral("PASS 7: grammar-load — "),
            QStringLiteral("PASS 8: grammar-refusal — "),
            QStringLiteral("PASS 9: settings — "),
            QStringLiteral("SKIP 10: lsp-clangd — "),
            QStringLiteral("PASS 11: dap-dapfake — "),
            QStringLiteral("SKIP 12: dap-lldb-dap — "),
            QStringLiteral("SKIP 13: dap-gdb — "),
            QStringLiteral("PASS 14: teardown — ")};
        QCOMPARE(lines.size(), frozen.size());
        for (int i = 0; i < frozen.size(); ++i)
            QVERIFY2(lines.at(i).startsWith(frozen.at(i)),
                     qPrintable(QStringLiteral("line %1: '%2'").arg(i + 1).arg(lines.at(i))));
        // The golden 15-span table is THE artifact S2's Python driver
        // must reproduce byte-for-byte (S2's close diffs the printed
        // tables leg-for-leg) — so the literal itself is frozen HERE,
        // and leg 7's detail says the pyext tabs EQUAL it.
        QVERIFY2(lines.at(3).contains(QStringLiteral(
                     "0-6:Keyword 7-10:Variable 11-17:Variable "
                     "18-19:Operator 21-22:Number 24-25:Number "
                     "27-28:Variable 29-30:Operator 31-34:FunctionBuiltin "
                     "36-42:Variable 44-47:Keyword 48-53:Function "
                     "61-67:Keyword 68-69:Variable 71-78:Comment")),
                 qPrintable(lines.at(3)));
        QVERIFY(lines.at(6).contains(QStringLiteral("EQUAL the golden table")));
        // The diff leg's frozen hop walk rides the detail too (the
        // door's 3-tab shape inside the tour's 4-tab arithmetic), and
        // the EXACT-PIN leg (dapfake) carries its frozen face: tour.py
        // as the session doc, the stored pair past EOF, the two carets,
        // the first-wins hint, the byte-exact status sequence.
        QVERIFY(lines.at(4).contains(QStringLiteral("1->3->3->1->1")));
        QVERIFY(lines.at(4).contains(QStringLiteral("tour-side count 4")));
        QVERIFY(lines.at(10).contains(QStringLiteral("dots {2,6}")));
        QVERIFY(lines.at(10).contains(QStringLiteral("caret {2,5}")));
        QVERIFY(lines.at(10).contains(QStringLiteral("caret {3,5}")));
        QVERIFY(lines.at(10).contains(QStringLiteral("= 42")));
        QVERIFY(lines.at(10).contains(QStringLiteral("sequence exact")));
        // The 15-knob face rode and was RESTORED (leg 6 closed with the
        // baseline back in force); and the DAP caret law's witness: the
        // teardown found every leg tab closed, the session doc included.
        QVERIFY(lines.at(8).contains(QStringLiteral("baseline restored")));
        QVERIFY(lines.at(13).contains(QStringLiteral("tab count 0")));
        // The SKIP reasons state THIS face's truth (forced off, never
        // the "not found" lie) — the honesty law is part of the frozen
        // table, so it is pinned.
        QVERIFY(lines.at(11).contains(QStringLiteral("forces the PATH legs off")));
        QVERIFY(lines.at(12).contains(QStringLiteral("forces the PATH legs off")));
    }
};

QTEST_MAIN(TestDemo)
#include "tst_demo.moc"
