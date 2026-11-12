#pragma once

// qmn-demo (the post-1.0 full demo app): the settings the "Настройки"
// dialog edits, as ONE plain struct — the demo-side template of the M19
// CodeEditorOptions surface (its ten knobs; code font and zoom stay
// per-editor editor chords, deliberately NOT part of the template).
// The defaults below MUST mirror the CodeEditor's own documented ones;
// the demo test suite tripwires that against a fresh editor's live
// options(), so drift fails CI rather than surprising a user.

#include "view/CodeEditor.hpp"

namespace qmn::app {

using qmn::view::CodeEditor;

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

    bool operator==(const EditorSettings&) const = default;
};

// The two doors: apply writes every knob through the editor's M19
// options object (so the *Changed signals fire like any other change
// door); collect reads a live editor back into a struct.
void applySettings(CodeEditor* editor, const EditorSettings& settings);
EditorSettings collectSettings(CodeEditor* editor);

} // namespace qmn::app
