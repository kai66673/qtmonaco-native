#pragma once

// M19 (Phase E3): the settings object. ONE child QObject carrying every
// editor knob, so consumers (and the future PySide6 binding) can discover,
// read and change settings through ONE object instead of scattered setters.
//
// Design: the CodeEditor stays the single source of truth — every knob is
// STORED in the editor (its existing setters keep their validation and
// invalidation machinery verbatim), the options object is the mirror and
// the notify bus: a setter here delegates to the editor, and the editor's
// setter ends with emitting the matching *Changed signal — so a change is
// announced no matter which door it came in (options object OR the flat
// CodeEditor setter, which stay one-line-compatible delegates). Getters
// always read live editor state; the options can never go stale.
//
// Two knobs exist ONLY here (they have no flat setter — M19 rule: new
// settings live only on the options object): autoCloseEnabled (the M18
// pair-typing toggle) and readOnly (blocks every USER edit gesture:
// typing, backspace/delete, Enter, Tab indentation, paste, cut, drops,
// find/replace actions and undo/redo; programmatic setPlainText,
// navigation, selection and copy stay fully alive).
//
// The editor owns the object (child of the widget); CodeEditor::options()
// returns it. One instance per editor, created in the editor constructor.
//
// Shiboken contract: plain QObject, plain Qt types only (bool/int + the
// editor's own Q_ENUM enums by nested name); no templates, no STL, no
// overloads. Signals carry the changed VALUE (Qt6 syntax).

#include <QObject>

#include "view/CodeEditor.hpp" // the nested Q_ENUM enums this object speaks

namespace qmn::view {

class CodeEditorOptions : public QObject {
    Q_OBJECT
public:
    explicit CodeEditorOptions(CodeEditor* editor, QObject* parent = nullptr);

    // --- mirroring knobs: each READs/WRITEs the editor's live state ------
    // (defaults and validation semantics are documented on the CodeEditor
    // setters themselves; this object adds NO semantics of its own.)

    void setLineNumbersMode(CodeEditor::LineNumbersMode mode);
    CodeEditor::LineNumbersMode lineNumbersMode() const;

    void setCurrentLineMode(CodeEditor::CurrentLineMode mode);
    CodeEditor::CurrentLineMode currentLineMode() const;

    // convenience delegate of the editor's currentLineHighlight(bool):
    void setCurrentLineHighlight(bool enabled);
    bool currentLineHighlight() const;

    void setLongLineRulerColumn(int column); // 0 = off
    int longLineRulerColumn() const;

    void setTabStopWidth(int columns); // clamped >= 1
    int tabStopWidth() const;

    void setAutoIndentEnabled(bool enabled);
    bool autoIndentEnabled() const;

    void setTabChangesFocus(bool enabled);
    bool tabChangesFocus() const;

    void setOverviewRulerEnabled(bool enabled); // the "minimap" knob
    bool overviewRulerEnabled() const;

    void setScrollBeyondLastLineEnabled(bool enabled);
    bool scrollBeyondLastLineEnabled() const;

    // --- M19-only knobs (no flat CodeEditor setter) ----------------------

    // M18 pair typing on/off (default ON). OFF, the pair chars behave as
    // plain single-char typing. Does NOT gate the textual "backspace
    // between an empty pair removes both" rule (that never claims the
    // key does anything pairing-typed created would do).
    void setAutoCloseEnabled(bool enabled);
    bool autoCloseEnabled() const;

    // Read-only mode (default OFF): every USER edit gesture is rejected
    // (typing incl. IME, Enter, Tab/Shift+Tab indentation, Backspace and
    // Delete including word variants, paste, cut, DnD drops, find/replace
    // actions, undo/redo). Caret movement, selection, copy, find and
    // programmatic document replacement keep working.
    void setReadOnly(bool readOnly);
    bool isReadOnly() const;

Q_SIGNALS:
    // Emitted by the editor's setters (NOT here) after the change has
    // been applied — so handlers can read the NEW value through this
    // object or the editor, whichever they hold. Same-value sets do NOT
    // emit (the setters are no-ops then).
    void lineNumbersModeChanged(CodeEditor::LineNumbersMode mode);
    void currentLineModeChanged(CodeEditor::CurrentLineMode mode);
    void longLineRulerColumnChanged(int column);
    void tabStopWidthChanged(int columns);
    void autoIndentEnabledChanged(bool enabled);
    void tabChangesFocusChanged(bool enabled);
    void overviewRulerEnabledChanged(bool enabled);
    void scrollBeyondLastLineEnabledChanged(bool enabled);
    void autoCloseEnabledChanged(bool enabled);
    void readOnlyChanged(bool readOnly);

private:
    CodeEditor* editor_; // owned elsewhere: the editor outlives its child
};

} // namespace qmn::view
