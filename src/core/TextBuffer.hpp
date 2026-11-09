#pragma once

#include <QString>
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
    // swaps them. Inserting/removing empty ranges is a no-op.
    void insertAtOffset(int offset, const QString& text);
    void removeRange(int startOffset, int endOffset);

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

    std::vector<QString> buffers_; // index 0 is the empty placeholder
    int addBuf_ = 0;               // buffers_ index the add buffer appends to
    std::vector<Node> nodes_;      // nodes_[0] is the null sentinel
    int freeHead_ = kNull;
    int root_ = kNull;
    quint32 rng_ = 0x9e3779b9u;

    quint32 nextRand();
};

} // namespace qmn::core
