#pragma once

#include <QString>
#include <functional>
#include <vector>

namespace qmn::core {

// Headless text model for the editor.
//
// Representation: a piece table (pieces reference immutable buffers of UTF-16
// code units) organized as an implicit treap keyed by sequence position.
// Each node stores subtree aggregates (chars, '\n' count), which gives
// O(log P) line <-> offset conversion and O(log P) insert/delete, where P is
// the number of pieces.
//
// All offsets in this class are [start, end) ranges measured in UTF-16 code
// units, matching Qt's QString and LSP position semantics. Note that removing
// a single code unit may split a surrogate pair; the view layer is
// responsible for keeping edits on grapheme boundaries (spike: not yet done).
//
// This file must depend on QtCore ONLY. No QtGui/QtWidgets.
class TextBuffer {
public:
    struct LineCol {
        int line = 0; // 0-based
        int col = 0;  // 0-based, UTF-16 units from line start
    };

    TextBuffer();

    // Replace the whole document with a single piece.
    void setText(const QString& text);

    QString text() const;
    QString textRange(int startOffset, int endOffset) const;

    int charCount() const; // UTF-16 code units
    int lineCount() const; // always >= 1, even for the empty document

    // Line text without the trailing '\n'. line must be in [0, lineCount).
    QString lineText(int line) const;

    // UTF-16 offset of the first code unit of `line` (the code unit right
    // after the preceding '\n'). line must be in [0, lineCount).
    int offsetOfLine(int line) const;

    // Line/col for an offset in [0, charCount]. Offsets pointing at a '\n'
    // resolve to the position *before* that newline. Worst-case O(n).
    LineCol lineColAt(int offset) const;

    // Offset of (line, col), clamped to the line length (never past a '\n').
    // line must be in [0, lineCount).
    int offsetOf(int line, int col) const;

    // Edits. Offsets are clamped; removeRange(start, end) with start > end
    // swaps them. Inserting/removing empty ranges is a no-op. Both record
    // onto the undo log (see undo section); setText replaces the document
    // and clears the history.
    void insertAtOffset(int offset, const QString& text);
    void removeRange(int startOffset, int endOffset);

    // --- edit observation + zero-copy piece reads (M4, view seam) --------
    // Every primitive piece-table mutation (insertAtOffset/removeRange,
    // undo/redo op applications, and setText as ONE whole-document op)
    // fires exactly one EditEvent AFTER the mutation. Offsets are in the
    // PRE-op frame; inserted is empty for a pure delete and end == start
    // for a pure insert. The stream replays the document's whole edit
    // history, so an observer can track incremental state (e.g. a
    // tree-sitter tree via ts_tree_edit) without ever re-reading the
    // document. Single observer — the view's highlight engine.
    struct EditEvent {
        int start = 0;
        int end = 0;
        QString inserted;
    };
    void setEditObserver(std::function<void(const EditEvent&)> observer);

    // Zero-copy window for pull-model readers (the tree-sitter input
    // callback). pieceAt(offset) locates the piece containing `offset`
    // (O(log pieces)) and returns the data from there to the piece's end.
    // data is nullptr when the offset is not inside the document. The
    // pointer is borrowed: valid only until the next mutation — readers
    // must not interleave with edits (same-thread discipline).
    struct PieceView {
        const char16_t* data = nullptr;
        int start = 0; // document offset of data[0]
        int len = 0;   // UTF-16 units from `offset` to the piece end
    };
    PieceView pieceAt(int offset) const;

    // --- undo/redo (M3) -----------------------------------------------
    // Op log with inverses. One undo step = one group: a group of ops is
    // applied/reverted atomically, so the view brackets compound actions
    // (e.g. delete-selection + insert) with begin/endEditGroup.
    //
    // Consecutive UNBRACKETED single edits coalesce: adjacent pure inserts
    // with no newline in either text (so typing is one word-ish undo step,
    // matching editor conventions), and adjacent pure deletions (so
    // repeated Backspace undoes as a chunk). Coalesced edits keep
    // undoDepth() unchanged - the caller reconciles via undoDepth().
    // The model has no caret, so caret moves cannot break chains by
    // themselves: the view calls breakUndoCoalescing() on deliberate caret
    // motion so a click + type is its own undo step even at an adjacent
    // offset. begin/endEditGroup nest (depth-counted); begin also starts a
    // fresh chain and end fences it against later coalescing.
    //
    // History is capped by op count and by log bytes (both measured before
    // appending), dropping the OLDEST entries first: the cap keeps memory
    // bounded for monster documents. Documented divergence from PLAN.md's
    // "snapshot compaction": cap-drop instead of periodic snapshots; the
    // log stores text, so the common typing case is bounded by the cap
    // alone, and snapshot machinery is not needed in v1.
    //
    // undo()/redo() return false and do nothing when the stack is empty or
    // a group is open. Redo truncates on the first new recorded edit.
    bool undo();
    bool redo();
    bool canUndo() const { return !undoStack_.empty(); }
    bool canRedo() const { return !redoStack_.empty(); }
    int undoDepth() const { return static_cast<int>(undoStack_.size()); }
    int redoDepth() const { return static_cast<int>(redoStack_.size()); }
    void beginEditGroup();
    void endEditGroup();
    void breakUndoCoalescing(); // freeze the top group against merging
    void clearUndoHistory();

    // Monotonic count of groups ever pushed onto the undo stack: grows on
    // every push (cap-drops included), unchanged by coalescing, undo/redo
    // moves and clears. Paired with undoDepth() it lets the view keep its
    // caret marks aligned with the core stack without guessing.
    qint64 undoPushSerial() const { return pushSerial_; }

    static constexpr int kMaxUndoGroups = 2048;
    static constexpr qint64 kMaxUndoLogBytes = 32ll * 1024 * 1024;

    int debugNodeCount() const;
    bool debugCheckInvariants() const;

private:
    struct Piece {
        int buf = 0;
        int start = 0;
        int len = 0; // UTF-16 units; always > 0 for a live node
    };

    struct Node {
        Piece piece;
        quint32 prio = 0;
        int left = kNull;
        int right = kNull;
        int chars = 0;     // own code units
        int lines = 0;     // own '\n' count
        int subChars = 0;  // subtree
        int subLines = 0;  // subtree
        int subCount = 0;  // subtree node count
    };

    static constexpr int kNull = 0;

    int pieceChars(const Piece& p) const { return p.len; }
    int pieceLines(const Piece& p) const;

    int newNode(const Piece& p);
    void freeNode(int i);
    void recycleTree(int root);
    void update(int i);
    int subChars(int i) const { return i == kNull ? 0 : nodes_[i].subChars; }
    int subLines(int i) const { return i == kNull ? 0 : nodes_[i].subLines; }
    int subCount(int i) const { return i == kNull ? 0 : nodes_[i].subCount; }

    int mergeTrees(int a, int b);
    // Splits so that `a` receives the first k chars (k may cut a piece).
    void splitChars(int root, int k, int& a, int& b);

    // k-th '\n' (1-based), k in [1, subLines(root)]. Returns its offset.
    int offsetOfNthNewline(int k) const;

    Piece appendToAddBuffer(const QString& text);

    int checkSubtree(int root, bool& ok) const;

    // --- undo log internals (public semantics documented above) --------
    // One op = one recorded insertAtOffset/removeRange. Ops are pure:
    // inserts carry `inserted` with start == end, deletes carry `removed`
    // for [start, end). Offsets refer to the state right before the op.
    struct EditOp {
        int start = 0;
        int end = 0;
        QString removed;
        QString inserted;
    };

    struct UndoGroup {
        std::vector<EditOp> ops;
        // true when built/grown from single unbracketed edits; bracketed
        // groups never coalesce (the view brackets compound actions).
        bool coalescable = false;
    };

    // Piece-table mutations without logging (used by the logging entry
    // points and by undo()/redo()).
    void applyInsert(int offset, const QString& text);
    void applyRemove(int startOffset, int endOffset);

    void recordEdit(EditOp op); // truncates redo, coalesces when possible
    static bool tryCoalesce(UndoGroup& top, const EditOp& nu);
    static qint64 opsBytes(const std::vector<EditOp>& ops);
    void enforceCapacity(); // drop-oldest of undoStack_ against both caps

    static constexpr int kMaxCoalesceUnits = 64; // merged-insert text cap

    std::vector<UndoGroup> undoStack_;
    std::vector<UndoGroup> redoStack_;
    std::vector<EditOp> openGroup_; // accumulated between begin/endEditGroup
    int groupDepth_ = 0;            // begin/endEditGroup nest depth
    qint64 logBytes_ = 0;           // stored removed/inserted text across both stacks
    qint64 pushSerial_ = 0;

    std::vector<QString> buffers_; // index 0 is the empty placeholder
    int addBuf_ = 0;               // buffers_ index the add buffer appends to
    std::vector<Node> nodes_;      // nodes_[0] is the null sentinel
    int freeHead_ = kNull;
    int root_ = kNull;
    quint32 rng_ = 0x9e3779b9u;
    std::function<void(const EditEvent&)> editObserver_; // single observer (M4)

    quint32 nextRand();
};

} // namespace qmn::core
