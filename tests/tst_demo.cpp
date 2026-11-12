// Drives the qmn-demo app (PLAN post-1.0 polish) fully offscreen: the
// tests enter through the SAME programmatic doors the menu slots call
// (MainWindow::openPath / applyEditorSettings / closeTabAt,
// EditorSettingsDialog::setSettings / settings), never exec()ing a
// QFileDialog or the dialog itself - an exec() would block ctest.
// Assertions are on model/widget state, never on pixels.

#include "app/EditorSettings.hpp"
#include "app/EditorSettingsDialog.hpp"
#include "app/MainWindow.hpp"

#include "view/CodeEditor.hpp"
#include "view/CodeEditorOptions.hpp"

#include <QByteArray>
#include <QFile>
#include <QFontDatabase>
#include <QTabBar>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using qmn::app::collectSettings;
using qmn::app::demoDefaultCodeFont;
using qmn::app::EditorSettings;
using qmn::app::EditorSettingsDialog;
using qmn::app::MainWindow;
using qmn::view::CodeEditor;
using qmn::view::CodeEditorOptions;

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
};

QTEST_MAIN(TestDemo)
#include "tst_demo.moc"
