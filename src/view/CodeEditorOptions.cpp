#include "view/CodeEditorOptions.hpp"

// The implementation is deliberately thin: every setter is a one-line
// delegation to the editor (single source of truth, single validation
// point, single invalidation point), every getter reads the editor back.
// The *Changed signals are emitted by the EDITOR's setters (see the emit
// trail in CodeEditor.cpp), which is what makes the flat API and the
// options object two doors into the same room.

namespace qmn::view {

CodeEditorOptions::CodeEditorOptions(CodeEditor* editor, QObject* parent)
    : QObject(parent), editor_(editor)
{
}

void CodeEditorOptions::setLineNumbersMode(CodeEditor::LineNumbersMode mode)
{
    editor_->setLineNumbersMode(mode);
}

CodeEditor::LineNumbersMode CodeEditorOptions::lineNumbersMode() const
{
    return editor_->lineNumbersMode();
}

void CodeEditorOptions::setCurrentLineMode(CodeEditor::CurrentLineMode mode)
{
    editor_->setCurrentLineMode(mode);
}

CodeEditor::CurrentLineMode CodeEditorOptions::currentLineMode() const
{
    return editor_->currentLineMode();
}

void CodeEditorOptions::setCurrentLineHighlight(bool enabled)
{
    editor_->setCurrentLineHighlight(enabled);
}

bool CodeEditorOptions::currentLineHighlight() const
{
    return editor_->currentLineHighlight();
}

void CodeEditorOptions::setLongLineRulerColumn(int column)
{
    editor_->setLongLineRulerColumn(column);
}

int CodeEditorOptions::longLineRulerColumn() const
{
    return editor_->longLineRulerColumn();
}

void CodeEditorOptions::setTabStopWidth(int columns)
{
    editor_->setTabStopWidth(columns);
}

int CodeEditorOptions::tabStopWidth() const
{
    return editor_->tabStopWidth();
}

void CodeEditorOptions::setAutoIndentEnabled(bool enabled)
{
    editor_->setAutoIndentEnabled(enabled);
}

bool CodeEditorOptions::autoIndentEnabled() const
{
    return editor_->autoIndentEnabled();
}

void CodeEditorOptions::setTabChangesFocus(bool enabled)
{
    editor_->setTabChangesFocus(enabled);
}

bool CodeEditorOptions::tabChangesFocus() const
{
    return editor_->tabChangesFocus();
}

void CodeEditorOptions::setOverviewRulerEnabled(bool enabled)
{
    editor_->setOverviewRulerEnabled(enabled);
}

bool CodeEditorOptions::overviewRulerEnabled() const
{
    return editor_->overviewRulerEnabled();
}

void CodeEditorOptions::setScrollBeyondLastLineEnabled(bool enabled)
{
    editor_->setScrollBeyondLastLineEnabled(enabled);
}

bool CodeEditorOptions::scrollBeyondLastLineEnabled() const
{
    return editor_->scrollBeyondLastLineEnabled();
}

void CodeEditorOptions::setCodeFont(const QFont& font)
{
    editor_->setCodeFont(font);
}

QFont CodeEditorOptions::codeFont() const
{
    return editor_->codeFont();
}

void CodeEditorOptions::setZoomFactor(double factor)
{
    editor_->setZoomFactor(factor);
}

double CodeEditorOptions::zoomFactor() const
{
    return editor_->zoomFactor();
}

void CodeEditorOptions::setAutoCloseEnabled(bool enabled)
{
    editor_->setAutoCloseEnabled(enabled);
}

bool CodeEditorOptions::autoCloseEnabled() const
{
    return editor_->autoCloseEnabled();
}

void CodeEditorOptions::setReadOnly(bool readOnly)
{
    editor_->setReadOnly(readOnly);
}

bool CodeEditorOptions::isReadOnly() const
{
    return editor_->isReadOnly();
}

} // namespace qmn::view
