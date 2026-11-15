#pragma once

// M25 (Gate 2): the document object. ONE editor-owned QObject that OWNS the
// TextBuffer: the editor's model moved out of the widget into this class (the
// sanctioned ownership surgery recorded in AGENTS.md — the view keeps a
// reference member `buffer_` into the document's buffer, so no call site,
// mapping door or undo mechanism changed).
//
// What the document carries that the view must not:
//   - the TEXT itself (undo/redo stay on the buffer, i.e. reachable through
//     the editor's existing doors; the document is the owner, not a facade),
//   - the URI (the LSP-facing document key; a plain QString, not a QUrl —
//     Gate 2's protocol layer speaks strings),
//   - the MODIFIED flag, and
//   - the edit stream: `editApplied(start, end, inserted)` per primitive
//     mutation, offsets in the PRE-OP frame — exactly the LSP textDocument/
//     didChange `contentChanges` semantics, and the diff car's re-key signal.
//
// Signal architecture (single source of truth): the core buffer fires ONE
// EditEvent per primitive piece-table mutation — inserts, deletes, undo/redo
// op applications and setText as ONE whole-document op (core TextBuffer.hpp
// contract). The attached editor owns the single core observer slot (its
// highlight engine's seam) and multiplexes every event into this object via
// the private noteEdit() below. Order per mutation: mutation -> noteEdit
// (modificationChanged?, editApplied, textChanged) -> editor's own
// textChanged ping. Consequence, v1 contract: a document's text signals fire
// only while it is editor-ATTACHED (every document v1 constructs is); a
// detached document (the M10 diff car's original side — none exists before
// that car) has no observer installed, so its text doors are silent and its
// observation is the future car's decision.
//
// Modified semantics (decided interim, documented divergence from
// QTextDocument/Monaco "dirty iff != baseline"): the flag turns TRUE on any
// edit and turns FALSE ONLY through an explicit markSaved(). Undo-back-to-
// baseline auto-reset needs a core undo-position hook, deferred; the real
// file I/O is a consumer concern (markSaved() is the door they call after
// their own save).
//
// v1 CONTRACT — ONE EDITOR PER DOCUMENT: a document is created by exactly
// one editor's ctor (parent = owner, lifetime pattern = options()/theme())
// and is permanently attached to it. Two editors on one buffer would fight
// over the single core edit-observer slot (and the caret/fold state each
// view keeps); multi-view editors are a separate future machine that needs
// core multi-observer support first. The diff car does NOT need it: it puts
// TWO documents in ONE widget, not two widgets on one document.
//
// Language stays on the VIEW (a render property; the M23 LanguageProperties
// machinery keys off the view's language id) — moving languageId onto the
// document is deliberately NOT part of Gate 2.
//
// Shiboken contract (the wheel binds this class with the M25/M10 cars):
// plain QObject, plain Qt types only; the core EditEvent NEVER crosses the
// public surface (noteEdit is private+friend); there is no public way to
// touch the TextBuffer itself (HighlightEngine attaches through the editor,
// which is its friend).
//
// C++ consumers NEVER construct this class themselves: the public ctor takes
// the owning CodeEditor and is what the editor's ctor calls; the object is
// reachable only through CodeEditor::document().

#include <QObject>
#include <QString>

#include "core/TextBuffer.hpp"

namespace qmn::view {

class CodeEditor;

class CodeDocument : public QObject {
    Q_OBJECT
public:
    // Editor-owned: parented to `owner` (the QObject lifetime pattern of
    // options()/theme()), permanently attached to it (v1 contract above).
    explicit CodeDocument(CodeEditor* owner);

    // --- text doors (the document owns the text) --------------------------
    // setPlainText routes through the attached editor's replace machinery
    // (caret reset, fold drop, scroll + undo-mark bookkeeping) so the view
    // stays consistent no matter which door the replace came in at.
    void setPlainText(const QString& text);
    QString toPlainText() const;
    int lineCount() const; // always >= 1, even for the empty document
    // Line text WITHOUT the trailing '\n'; out-of-range line -> empty string.
    QString lineText(int line) const;

    // --- identity (LSP-facing: the URI is the document key) ---------------
    // Free-form string: schemes are the consumer's business ("file:///...",
    // "untitled:foo.py", ...); empty = no name assigned yet.
    void setDocumentUri(const QString& uri);
    QString documentUri() const;

    // --- modified flag (v1 semantics: header docs above) ------------------
    bool isModified() const;
    void markSaved(); // the ONLY way back to unmodified in v1

signals:
    // Document-level detail-free text ping: one emit per primitive
    // mutation (mirror of the editor's textChanged, fired from the same
    // observer pass, just before it).
    void textChanged();
    // The LSP-shaped edit stream (contract: header docs above). PRE-OP
    // offsets; `inserted` empty for a pure delete; end == start for a pure
    // insert; one emit per primitive mutation INCLUDING undo/redo replay.
    void editApplied(int start, int end, const QString& inserted);
    void documentUriChanged(const QString& uri); // fires only on a real change
    void modificationChanged(bool modified);     // fires only on transitions

private:
    friend class CodeEditor; // construction, buffer access, the noteEdit seam

    // The multiplexer seam: the attached editor's core edit-observer calls
    // this once per primitive mutation (after the mutation, before the
    // editor's textChanged ping — the order the ctor's observer keeps).
    void noteEdit(const qmn::core::TextBuffer::EditEvent& ev);

    qmn::core::TextBuffer buffer_;
    CodeEditor* editor_; // the one attached editor (v1: never null)
    QString uri_;
    bool modified_ = false;
};

} // namespace qmn::view
