#pragma once

// qmn-demo (the post-1.0 full demo app): the settings the "Настройки"
// dialog edits, as ONE plain struct — the demo-side template of the M19
// CodeEditorOptions surface (its twelve knobs since M20 added the M17
// code font and zoom; the dialog gained a "Шрифт и масштаб" group for
// them: family + pixel size + a zoom factor clamped by the editor).
// The defaults below MUST mirror the CodeEditor's own documented ones;
// the demo test suite tripwires that against a fresh editor's live
// options(), so drift fails CI rather than surprising a user.

#include <QFont>

#include "view/CodeEditor.hpp"

namespace qmn::app {

using qmn::view::CodeEditor;

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

    bool operator==(const EditorSettings&) const = default;
};

// The two doors: apply writes every knob through the editor's M19
// options object (so the *Changed signals fire like any other change
// door); collect reads a live editor back into a struct.
void applySettings(CodeEditor* editor, const EditorSettings& settings);
EditorSettings collectSettings(CodeEditor* editor);

} // namespace qmn::app
