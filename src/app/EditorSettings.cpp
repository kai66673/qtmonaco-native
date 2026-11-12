#include "EditorSettings.hpp"

#include <QFontDatabase>

#include "view/CodeEditorOptions.hpp"

namespace qmn::app {

using qmn::view::CodeEditorOptions;

QFont demoDefaultCodeFont()
{
    // Bit-for-bit the CodeEditor ctor's shipped recipe (a drift tripwire
    // test compares this against a fresh editor's live codeFont()).
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(14);
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

void applySettings(CodeEditor* editor, const EditorSettings& s)
{
    // The M19 options object is the demo's one door in: every setter
    // delegates to the editor (validation/invalidation unchanged) and the
    // editor emits the matching *Changed signal.
    CodeEditorOptions* o = editor->options();
    o->setLineNumbersMode(s.lineNumbersMode);
    o->setCurrentLineMode(s.currentLineMode);
    o->setLongLineRulerColumn(s.longLineRulerColumn);
    o->setTabStopWidth(s.tabStopWidth);
    o->setAutoIndentEnabled(s.autoIndentEnabled);
    o->setTabChangesFocus(s.tabChangesFocus);
    o->setOverviewRulerEnabled(s.overviewRulerEnabled);
    o->setScrollBeyondLastLineEnabled(s.scrollBeyondLastLineEnabled);
    o->setAutoCloseEnabled(s.autoCloseEnabled);
    o->setReadOnly(s.readOnly);
    o->setIndentGuidesEnabled(s.indentGuidesEnabled); // M21 (13th knob)
    o->setCodeFont(s.codeFont); // M20: the M17 pair rides the same bus
    o->setZoomFactor(s.zoomFactor); // (the editor clamps it to its bounds)
}

EditorSettings collectSettings(CodeEditor* editor)
{
    // Reads LIVE editor state (the options object never goes stale by M19
    // contract), so the dialog can seed itself from any single editor.
    const CodeEditorOptions* o = editor->options();
    EditorSettings s;
    s.lineNumbersMode = o->lineNumbersMode();
    s.currentLineMode = o->currentLineMode();
    s.longLineRulerColumn = o->longLineRulerColumn();
    s.tabStopWidth = o->tabStopWidth();
    s.autoIndentEnabled = o->autoIndentEnabled();
    s.tabChangesFocus = o->tabChangesFocus();
    s.overviewRulerEnabled = o->overviewRulerEnabled();
    s.scrollBeyondLastLineEnabled = o->scrollBeyondLastLineEnabled();
    s.autoCloseEnabled = o->autoCloseEnabled();
    s.readOnly = o->isReadOnly();
    s.indentGuidesEnabled = o->indentGuidesEnabled(); // M21
    s.codeFont = o->codeFont();     // the BASE, exactly as stored
    s.zoomFactor = o->zoomFactor(); // the live (already clamped) factor
    return s;
}

} // namespace qmn::app
