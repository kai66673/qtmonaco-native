#include "view/CodeDocument.hpp"

#include "view/CodeEditor.hpp"
#include "view/HighlightEngine.hpp"

namespace qmn::view {

CodeDocument::CodeDocument(CodeEditor* owner)
    : QObject(owner), // the options()/theme() lifetime pattern: parent = owner
      views_(1, owner)
{
    // D2: the document multiplexes its OWN edit stream from its own core
    // observer slot. The editor's ctor registers its per-view pass next
    // and re-asserts the mux last, so the pass order is [view..., mux].
    installMuxSlot();
}

CodeDocument::CodeDocument()
    : QObject(nullptr)
{
    // Standalone (M10): zero views, so the mux slot is the buffer's ONLY
    // observer — the trio fires exactly like an attached document's, minus
    // the (empty) per-view pass and tail. Since D2 this is also the
    // consumer-owned document the CodeEditor(CodeDocument*, parent) ctor
    // attaches views to.
    installMuxSlot();
}

// D4: the parse engine subscribes to buffer_, so it must die first — the
// dtor body runs before any member (buffer_ last, declaration order).
CodeDocument::~CodeDocument()
{
    if (parseToken_)
        buffer_.removeEditObserver(parseToken_);
    delete parse_; // ~HighlightEngine drops tree/parser/queries; the
                   // windows_ map dies with it (borrower keys are never
                   // dereferenced, so no dangling read was possible)
    parse_ = nullptr;
    parseToken_ = 0;
}

// --- the shared parse (car D machine D4) ----------------------------------
// Created on first use (a never-highlighted document never pays a
// parser). The engine rides the buffer's observer list DIRECTLY — every
// primitive mutation hits the tree before any later parse (the ts
// invariant), whatever the entry door — and lands LAST except the mux,
// which is re-asserted to the true tail right after.
HighlightEngine* CodeDocument::ensureParse()
{
    if (parse_)
        return parse_;
    parse_ = new HighlightEngine();
    parse_->attach(&buffer_);
    parseToken_ = buffer_.addEditObserver([this](const qmn::core::TextBuffer::EditEvent& ev) {
        parse_->onEdit(ev);
    });
    reassertMuxLast(); // the standing invariant, named here in the D2 record
    return parse_;
}

void CodeDocument::dropViewWindows(CodeEditor* view)
{
    if (parse_)
        parse_->dropViewport(view);
}

// --- the mux slot (car D machine D2) -------------------------------------
// The lambda defers the trio inside a replace bracket (see beginReplace/
// endReplace) and otherwise fires it straight away — every non-replace
// mutation stays the v1.11 synchronous pass, byte-identically.

void CodeDocument::installMuxSlot()
{
    muxToken_ = buffer_.addEditObserver([this](const qmn::core::TextBuffer::EditEvent& ev) {
        if (replaceDepth_ > 0) {
            deferredEdits_.append(ev);
            return;
        }
        emitEditTrio(ev);
    });
}

void CodeDocument::reassertMuxLast()
{
    // Move the mux slot to the END of the observer list (remove + append:
    // tokens are opaque, the fresh token replaces the old one in the same
    // breath). Called by every observer attach — a standing invariant.
    buffer_.removeEditObserver(muxToken_);
    installMuxSlot();
}

void CodeDocument::attachView(CodeEditor* view)
{
    views_.append(view); // order = attach order = per-view pass order
}

void CodeDocument::detachView(CodeEditor* view)
{
    buffer_.removeEditObserver(view->viewObserverToken_); // this view's pass
    if (parse_)
        parse_->dropViewport(view); // D4: release its window from the set
    views_.removeAll(view);
}

void CodeDocument::beginReplace()
{
    ++replaceDepth_;
}

void CodeDocument::endReplace()
{
    if (--replaceDepth_ > 0)
        return; // inner bracket of a re-entrant replace: the outer one fires
    // The hooks have run (every view's per-view pass inside the mutation,
    // the acting view's caret/scroll/undo-mark/fold/wrap resets after it);
    // the eaten whole-doc events go out NOW, one trio each, in order.
    const auto deferred = std::exchange(deferredEdits_, {});
    for (const auto& ev : deferred)
        emitEditTrio(ev);
}

// --- the origin tag (car D machine D3) ------------------------------------
// Every mutating door brackets its action here (CodeEditor::MutationAction
// is the RAII; the header contract carries the rules). The diff lands on
// the views at END because that is when the shared undo stack is FINAL —
// a group only lands when the outermost beginEditGroup closes, and a
// per-event mirror would see mid-bracket state (the design record's mark
// alignment rides this timing).

void CodeDocument::beginAction(CodeEditor* view)
{
    // Nesting: an inner door of the same action re-enters with the same
    // view (depth handles it, the actor is unchanged); a cross-view
    // re-entrant mutation (an editApplied handler typing through another
    // view) becomes the inner actor and restores on exit. Arming the
    // mirror is idempotent — the OUTERMOST arm keeps the pre-action frame,
    // which is the frame every group of this action pairs with.
    actorStack_.append(actor_);
    actor_ = view;
    ++actionDepth_;
    for (CodeEditor* v : views_)
        if (v != view)
            v->armForeignMirror();
}

void CodeDocument::endAction()
{
    actor_ = actorStack_.takeLast();
    --actionDepth_;
    if (actionDepth_ > 0)
        return; // an inner door closed: the action (and its armed mirror)
                // goes on — the stack state is not final yet
    // Diff-based, so it is self-correcting: the acting view's own
    // capture/reconcile pairs already advanced its mirror (the diff finds
    // nothing left for it), and every other view aligns on its own last
    // seen serial/depth pair. Self-gated inside the view.
    for (CodeEditor* v : views_)
        v->mirrorForeignAction();
}

void CodeDocument::emitEditTrio(const qmn::core::TextBuffer::EditEvent& ev)
{
    // The flag transition leads, then the LSP-shaped edit delta, then the
    // detail-free document ping — then every attached view's own
    // textChanged tail, in attach order (the v1.11 pair with ONE view).
    if (!modified_) {
        modified_ = true;
        emit modificationChanged(true);
    }
    emit editApplied(ev.start, ev.end, ev.inserted);
    emit textChanged();
    for (CodeEditor* v : views_)
        v->notifyDocumentEditTail();
}

void CodeDocument::setPlainText(const QString& text)
{
    // With views attached the replace routes through the FIRST view's
    // replace machinery (caret reset, fold drop, scroll + undo-mark
    // bookkeeping); its setPlainText carries the begin/endReplace brackets,
    // and every view's per-view pass still fires off the one whole-doc
    // event. A STANDALONE document (M10: the diff car's original side)
    // brackets the core's silent door itself; its mux still fires the
    // signal trio — once, AFTER the bracket closes.
    if (!views_.isEmpty()) {
        views_.first()->setPlainText(text);
        return;
    }
    beginReplace();
    buffer_.setText(text);
    endReplace();
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

} // namespace qmn::view
