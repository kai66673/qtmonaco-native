#pragma once

// qmn-demo (the post-1.0 full demo app): the settings the "Настройки"
// dialog edits, as ONE plain struct — the demo-side template of the M19
// CodeEditorOptions surface (its thirteen knobs since M21; M20 added the
// M17 font/zoom pair with a "Шрифт и масштаб" group, M24 grew the
// fourteenth: the theme preset riding the CodeEditorTheme door).
// The defaults below MUST mirror the CodeEditor's own documented ones;
// the demo test suite tripwires that against a fresh editor's live
// options()/theme(), so drift fails CI rather than surprising a user.

#include <QFont>

#include "view/CodeEditor.hpp"
#include "view/CodeEditorTheme.hpp"

namespace qmn::app {

using qmn::view::CodeEditor;
using qmn::view::CodeEditorTheme; // M24: the theme preset knob's type

// The CodeEditor ctor's exact shipped font recipe (fixed system font,
// 14px, no hinting). ONE definition shared by the template default and
// the drift tripwire — QFont::operator== compares definitions, so a
// recipe DRIFT in the library makes the demo test fail, not a surprise.
QFont demoDefaultCodeFont();

struct EditorSettings {
    // Defaults = CodeEditor defaults (verified by defaultSettingsMirrorFreshEditorDefaults).
    CodeEditor::LineNumbersMode lineNumbersMode = CodeEditor::LineNumbersMode::On;
    CodeEditor::CurrentLineMode currentLineMode = CodeEditor::CurrentLineMode::Background;
    int longLineRulerColumn = 0;      // 0 = off
    int tabStopWidth = 4;             // soft-tab stop grid
    bool autoIndentEnabled = true;    // Enter copies leading whitespace
    bool tabChangesFocus = false;     // Tab rides the focus chain instead
    bool overviewRulerEnabled = true; // the right-edge overview strip
    bool scrollBeyondLastLineEnabled = false;
    bool autoCloseEnabled = true;     // M18 pair typing
    bool readOnly = false;            // refuse USER edits (M19-only knob)
    bool indentGuidesEnabled = true;  // M21 ambient tab-stop guides (options-only)
    // M20: the M17 pair. codeFont is the zoom-1.0 BASE exactly as the
    // editor stores it (pixel-sized by the demo recipe — see
    // demoDefaultCodeFont); zoomFactor is the live editor factor (the
    // editor's clamps are its own, the dialog spin just pre-nudges).
    QFont codeFont = demoDefaultCodeFont();
    double zoomFactor = 1.0;
    // M24 (14th knob): the theme PRESET riding the CodeEditorTheme door
    // (not an options knob). Default = the live-palette theme (today's
    // behavior); a manual role edit on a live editor reports kind Custom,
    // which the dialog combo clamps back to the Default row (the demo
    // itself never produces Custom).
    CodeEditorTheme::Preset themePreset = CodeEditorTheme::Preset::Default;

    bool operator==(const EditorSettings&) const = default;
};

// The two doors: apply writes every knob through the editor's M19
// options object (so the *Changed signals fire like any other change
// door); collect reads a live editor back into a struct.
void applySettings(CodeEditor* editor, const EditorSettings& settings);
EditorSettings collectSettings(CodeEditor* editor);

} // namespace qmn::app
