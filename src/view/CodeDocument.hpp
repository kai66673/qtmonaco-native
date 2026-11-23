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
// Since car D the document may also carry MANY views (attach ctor below);
// the class header carries the contract.
//
// Signal architecture (single source of truth): the core buffer fires ONE
// EditEvent per primitive piece-table mutation — inserts, deletes, undo/redo
// op applications and setText as ONE whole-document op (core TextBuffer.hpp
// contract). CAR D (machine D2) made the document itself the multiplexer:
// every CodeDocument installs its OWN core observer slot and RE-ASSERTS it
// as the LAST slot of the buffer's observer list (the v1.11 signal order —
// every view's per-view pass first, the document trio last — is then
// byte-identical). Each attached editor registers its own per-view pass
// (highlight/fold/wrap/diff/a11y) ahead of the mux; the mux pass fires the
// trio and then every view's detail-free textChanged tail, in attach order.
// Order per mutation: mutation -> [view passes in attach order] ->
// (modificationChanged?, editApplied, document textChanged) -> each view's
// own textChanged ping — one trio on the wire per mutation, no matter how
// many views share the document.
// THE REPLACE EXCEPTION (the deferral knot): setText fires its whole-doc
// event synchronously INSIDE the mutation, before the replace machinery's
// bookkeeping (every view's fold/wrap sweeps, the acting view's caret/
// scroll/undo-mark resets) has run. So the replace doors bracket the
// mutation with beginReplace()/endReplace() and the mux DEFERS the trio
// past the hooks — the trio still fires EXACTLY ONCE, in the same
// relative order, with zero stale-state windows for editApplied handlers.
// THE ORIGIN TAG (machine D3): every text-mutating CodeEditor door wraps
// its action in a MutationAction RAII that brackets it here
// (beginAction/endAction, nestable — inner doors re-enter). The document
// remembers WHICH view owns the in-flight action; the view ctors'
// per-view pass reads the tag to tell SELF edits (run exactly the v1.11
// self path — the single-view case is byte-identical, the whole existing
// suite is the tripwire) from FOREIGN ones (the view did not author the
// mutation, so it maps what it maintains instead:
// CodeEditor::reconcileForeignEdit + the mark mirror). At action BEGIN
// the other views ARM their mirror (a snapshot of their caret state —
// the pre-action frame every group this action pushes pairs with); at
// action END (when the shared undo stack's state is final) every view
// DIFFS its own mark stacks against the buffer and aligns — self-authored
// or not, every view reconciles every group, and the alignment Q_ASSERT
// stays a load-bearing guard for a view that never typed. A missed tag
// reads as a foreign edit and double-shifts a caret: LOUD, not silent
// (the design record's pinned failure mode).
// STANDALONE documents (the M10 diff car's detached originals/feeds — the
// no-editor ctor below) carry exactly the same mux slot; with zero views
// attached there is no per-view pass and no tail, and their signals fire
// exactly like an attached document's, minus nothing — the mux pass is
// shared. (This replaces the pre-M10 note that "detached documents are
// silent": that car is here and made the decision its header had reserved.)
//
// Modified semantics (decided interim, documented divergence from
// QTextDocument/Monaco "dirty iff != baseline"): the flag turns TRUE on any
// edit and turns FALSE ONLY through an explicit markSaved(). Undo-back-to-
// baseline auto-reset needs a core undo-position hook, deferred; the real
// file I/O is a consumer concern (markSaved() is the door they call after
// their own save).
//
// CAR D CONTRACT (machine D2 replaced the old "ONE EDITOR PER DOCUMENT"
// v1 rule): attach is a CTOR FACT — a view binds its `buffer_` reference
// member to the document's buffer at construction and can never switch
// documents (there is no attach/detach door on an existing CodeEditor).
// ONE document may now carry any number of views (CodeEditor(doc, parent));
// the views' per-view passes run in ATTACH ORDER and the document's mux
// slot rides the LAST core-observer slot (re-asserted whenever another
// observer lands, a standing invariant). LIFETIME IS A CONTRACT, not
// enforced machinery: the document must OUTLIVE every attached view —
// reparenting (document()->setParent(...)) is the legal extension, and an
// editor-owned document (parent = its first view) dies with that first
// view, so other views attached to it must die first.
// The diff car does NOT need multi-view at all: it puts TWO documents in
// ONE widget, not two widgets on one document — that shape is unchanged.
//
// STANDALONE (M10): the no-editor ctor creates a document attached to
// NOTHING — the diff view's original side, fed by its consumer; since D2
// its meaning widened to "ZERO views so far", it may gain views through
// the attach ctor. Standalone documents own their buffer mutations
// silently-through-signals (their own mux slot), never reach a replace
// machinery before D2 attached them, and gain every edit door an attached
// document has once a view attaches. Attached vs standalone is decided at
// CONSTRUCTION and never changes (no attach/detach doors in v1; views
// attach at ctor and detach only by dying).
//
// Language stays on the VIEW (a render property; the M23 LanguageProperties
// machinery keys off the view's language id) — moving languageId onto the
// document is deliberately NOT part of Gate 2.
//
// Shiboken contract (the wheel binds this class with the M25/M10 cars):
// plain QObject, plain Qt types only; the core EditEvent NEVER crosses the
// public surface (the mux pass and its replace brackets are all
// private+friend); there is no public way to touch the TextBuffer itself
// (since car D4 the DOCUMENT owns its HighlightEngine and attaches it to
// its own buffer — the engine stays view-internal, behind friend/private
// doors like everything else on this list).
//
// Attached documents: C++ consumers NEVER construct those — the editor's
// ctor calls that one; the object is reachable through CodeEditor::
// document(). Standalone documents (M10, the diff original side) ARE
// consumer-constructible: the no-editor ctor below.

#include <QObject>
#include <QString>
#include <QVector>

#include "core/TextBuffer.hpp"

class TestCodeEditor; // car D4's test-oracle friend, GLOBAL scope (the
                      // friend below is ::-qualified: an unqualified
                      // friend declaration inside qmn::view would name
                      // qmn::view::TestCodeEditor, a different class)

namespace qmn::view {

class CodeEditor;
class HighlightEngine; // view-internal (HighlightEngine.hpp is not an
                       // installed header; forward decl + unique_ptr-
                       // style raw pointer only cross this seam)

class CodeDocument : public QObject {
    Q_OBJECT
public:
    // Editor-owned: parented to `owner` (the QObject lifetime pattern of
    // options()/theme()), permanently attached to it (contract above — the
    // document then dies with that first view unless reparented).
    explicit CodeDocument(CodeEditor* owner);

    // Standalone (M10): zero views at construction (the diff view's
    // detached original side). Installs its own core mux observer — the
    // header contract above carries the rules. Since D2 this ctor also
    // means "consumer-owned document a view may ATTACH to later" via
    // CodeEditor(CodeDocument*, parent).
    CodeDocument();

    // D4: the engine subscription rides the buffer's observer list, so
    // the engine must be torn down BEFORE the buffer (out-of-line dtor).
    ~CodeDocument() override;

    // --- text doors (the document owns the text) --------------------------
    // setPlainText routes through a view's replace machinery (caret reset,
    // fold drop, scroll + undo-mark bookkeeping) so every view stays
    // consistent no matter which door the replace came in at; with views
    // attached it routes through the FIRST one, with zero views it brackets
    // the core setText with the replace machinery itself (header contract).
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
    // mutation (mirror of the editor's textChanged, fired from the mux
    // pass just before every view's own ping — header contract above).
    void textChanged();
    // The LSP-shaped edit stream (contract: header docs above). PRE-OP
    // offsets; `inserted` empty for a pure delete; end == start for a pure
    // insert; one emit per primitive mutation INCLUDING undo/redo replay.
    void editApplied(int start, int end, const QString& inserted);
    void documentUriChanged(const QString& uri); // fires only on a real change
    void modificationChanged(bool modified);     // fires only on transitions

private:
    friend class CodeEditor; // construction, buffer access, and the doors
                             // below (attach/detach, replace brackets)
    friend class ::TestCodeEditor; // car D4's parse-state oracles (the
                                 // shared-engine tripwires read parse()
                                 // — coverage window, parseCount())

    // --- the mux slot (car D, machine D2): the document IS the muxer ----
    // Installs the document's OWN core observer. The mux must ride the
    // LAST slot of the buffer's observer list, so every view's per-view
    // pass (fold/wrap/find/diff/a11y, and D3's caret mapping) and the
    // shared parse (D4) run before the trio — that is what keeps the
    // v1.11 signal order byte-identical for any number of views.
    // reassertMuxLast() belongs to EVERY observer attach (the view ctors;
    // ensureParse, D4): a standing invariant, not a ctor trick.
    void installMuxSlot();
    void reassertMuxLast();

    // View-list plumbing, called by the two CodeEditor ctors and the
    // dtor (attach is a ctor fact — the bound reference member reseats
    // never; a view leaves the list only by dying).
    void attachView(CodeEditor* view);
    void detachView(CodeEditor* view);

    // The replace brackets (header contract): the view's setPlainText and
    // the zero-view setPlainText wrap the core setText with them; the mux
    // DEFERS the trio while the depth is > 0 and fires every deferred
    // event exactly once when the OUTERMOST bracket closes — after the
    // hooks ran, so editApplied handlers never see a stale-state window.
    void beginReplace();
    void endReplace();

    // --- the origin tag (car D, machine D3) -------------------------------
    // The MutationAction RAII in every mutating CodeEditor door brackets
    // its action through these doors (contract: header docs above). The
    // tag nests (an inner door of the same action re-enters; a re-entrant
    // cross-view call — an editApplied handler mutating through another
    // view — flips it for the inner scope and restores it); beginAction
    // arms every OTHER view's foreign mirror with the pre-action frame,
    // endAction fires the mirror diff once the shared stack is final.
    // Not for Python (the wheel binds NO door reaching a CodeEditor*;
    // friend-only machinery like the replace brackets).
    void beginAction(CodeEditor* view);
    void endAction();

    // --- the shared parse (car D, machine D4) -----------------------------
    // The DOCUMENT owns the one HighlightEngine over its buffer (the design
    // record: one buffer, one tree-sitter tree). Views borrow it through
    // these friend-only doors — never public API (the engine type is view-
    // internal, tree-sitter-shaped, and stays out of every installed
    // header and out of the wheel).
    // existingParse(): the engine, or nullptr when it was never created
    // (the language-less, never-painted state — the no-clobber rule for a
    // BORROWER, e.g. a diff view looking at a document it does not own:
    // reading must never materialize a parse in someone else's document).
    HighlightEngine* existingParse() const { return parse_; }
    // ensureParse(): create-on-first-use. Attaches the engine to buffer_
    // and registers it as a core observer LAST — then re-asserts the mux
    // slot, which is the standing invariant every observer attach owes
    // (comment at installMuxSlot): the final pass order per mutation is
    // [view passes in attach order, parse, mux].
    HighlightEngine* ensureParse();
    // A dying view's window entry: the engine's viewport SET is keyed by
    // view address; detachView releases the entry so the union never
    // carries a dead surface (and an address can never alias a stale
    // window: detach happens in the view's dtor body).
    void dropViewWindows(CodeEditor* view);

    // The trio itself: modificationChanged? -> editApplied -> document
    // textChanged -> every view's textChanged tail, in attach order.
    void emitEditTrio(const qmn::core::TextBuffer::EditEvent& ev);

    qmn::core::TextBuffer buffer_;
    QVector<CodeEditor*> views_; // attach order; empty for STANDALONE docs
                                 // (M10: the diff car's originals/feeds)
    // D4: the shared parse, created LAZILY (a plain-text or never-painted
    // document never pays a parser), riding buffer_ as a core observer
    // registered at creation time — which makes it the LAST subscription,
    // so installMuxSlot's slot moves ahead of it (the standing invariant,
    // comment above). Its windows_ entries die with it.
    HighlightEngine* parse_ = nullptr;
    int parseToken_ = 0; // the engine's core-observer token (0 = none)
    int muxToken_ = 0; // the mux's core-observer token (never 0 once set)
    int replaceDepth_ = 0; // > 0 while the replace hooks are running
    QVector<qmn::core::TextBuffer::EditEvent> deferredEdits_; // trio(s) the
                                                              // brackets ate
    // The origin tag (D3): actor_ is the view owning the in-flight action
    // (null = no action, or the zero-view document-issued replace); the
    // stack restores the outer actor when a nested action ends.
    CodeEditor* actor_ = nullptr;
    QVector<CodeEditor*> actorStack_;
    int actionDepth_ = 0;
    QString uri_;
    bool modified_ = false;
};

} // namespace qmn::view
