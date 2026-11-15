#include "view/CodeDocument.hpp"

#include "view/CodeEditor.hpp"

namespace qmn::view {

CodeDocument::CodeDocument(CodeEditor* owner)
    : QObject(owner), // the options()/theme() lifetime pattern: parent = owner
      editor_(owner)
{
}

void CodeDocument::setPlainText(const QString& text)
{
    // The attached path routes through the editor's replace machinery (caret
    // reset, fold drop, scroll + undo-mark bookkeeping); the observer pass
    // then signals through this document like for any other mutation. A
    // DETACHED document (v1: none exists; the M10 diff car will create the
    // first ones) mutates its buffer with the core's silent door.
    if (editor_)
        editor_->setPlainText(text);
    else
        buffer_.setText(text);
}

QString CodeDocument::toPlainText() const
{
    return buffer_.text();
}

int CodeDocument::lineCount() const
{
    return buffer_.lineCount(); // core invariant: always >= 1
}

QString CodeDocument::lineText(int line) const
{
    if (line < 0 || line >= buffer_.lineCount())
        return QString();
    return buffer_.lineText(line);
}

void CodeDocument::setDocumentUri(const QString& uri)
{
    if (uri_ == uri)
        return;
    uri_ = uri;
    emit documentUriChanged(uri_);
}

QString CodeDocument::documentUri() const
{
    return uri_;
}

bool CodeDocument::isModified() const
{
    return modified_;
}

void CodeDocument::markSaved()
{
    if (!modified_)
        return;
    modified_ = false;
    emit modificationChanged(false);
}

// The multiplexer seam (private, CodeEditor friend): called by the attached
// editor's single core edit-observer once per primitive mutation. The flag
// transition leads, then the LSP-shaped edit delta, then the detail-free
// ping — the editor's own textChanged() fires after this returns.
void CodeDocument::noteEdit(const qmn::core::TextBuffer::EditEvent& ev)
{
    if (!modified_) {
        modified_ = true;
        emit modificationChanged(true);
    }
    emit editApplied(ev.start, ev.end, ev.inserted);
    emit textChanged();
}

} // namespace qmn::view
