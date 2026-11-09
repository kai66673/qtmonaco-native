#include "TextBuffer.hpp"

#include <QDebug>

#include <algorithm>
#include <cassert>

namespace qmn::core {

namespace {
constexpr QChar kNewline = u'\n';
}

TextBuffer::TextBuffer()
{
    nodes_.resize(1); // index 0 = null sentinel
    buffers_.emplace_back(); // buffers_[0] = "" placeholder
}

quint32 TextBuffer::nextRand()
{
    // xorshift32: deterministic across runs, good enough for treap balancing.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

int TextBuffer::pieceLines(const Piece& p) const
{
    const QString& buf = buffers_[p.buf];
    int count = 0;
    for (int i = p.start; i < p.start + p.len; ++i) {
        if (buf.at(i) == kNewline)
            ++count;
    }
    return count;
}

int TextBuffer::newNode(const Piece& p)
{
    assert(p.len > 0);
    int i;
    if (freeHead_ != kNull) {
        i = freeHead_;
        freeHead_ = nodes_[i].left; // reuse `left` as the free-list link
    } else {
        i = static_cast<int>(nodes_.size());
        nodes_.emplace_back();
    }
    Node& n = nodes_[static_cast<std::size_t>(i)];
    n.piece = p;
    n.prio = nextRand();
    n.left = n.right = kNull;
    n.chars = p.len;
    n.lines = pieceLines(p);
    n.subChars = p.len;
    n.subLines = n.lines;
    n.subCount = 1;
    return i;
}

void TextBuffer::freeNode(int i)
{
    Node& n = nodes_[static_cast<std::size_t>(i)];
    n.left = freeHead_;
    n.right = kNull;
    freeHead_ = i;
}

void TextBuffer::recycleTree(int root)
{
    // Iterative pre-order: read children first, then release the node.
    std::vector<int> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        if (i == kNull)
            continue;
        const Node& n = nodes_[static_cast<std::size_t>(i)];
        stack.push_back(n.left);
        stack.push_back(n.right);
        freeNode(i);
    }
}

void TextBuffer::update(int i)
{
    Node& n = nodes_[static_cast<std::size_t>(i)];
    n.subChars = n.chars + subChars(n.left) + subChars(n.right);
    n.subLines = n.lines + subLines(n.left) + subLines(n.right);
    n.subCount = 1 + subCount(n.left) + subCount(n.right);
}

int TextBuffer::mergeTrees(int a, int b)
{
    if (a == kNull)
        return b;
    if (b == kNull)
        return a;
    if (nodes_[static_cast<std::size_t>(a)].prio > nodes_[static_cast<std::size_t>(b)].prio) {
        nodes_[static_cast<std::size_t>(a)].right = mergeTrees(nodes_[static_cast<std::size_t>(a)].right, b);
        update(a);
        return a;
    }
    nodes_[static_cast<std::size_t>(b)].left = mergeTrees(a, nodes_[static_cast<std::size_t>(b)].left);
    update(b);
    return b;
}

void TextBuffer::splitChars(int root, int k, int& a, int& b)
{
    if (root == kNull) {
        a = b = kNull;
        return;
    }
    // Note: never hold Node& across recursive calls / newNode — the pool may
    // reallocate. Always re-index nodes_ after calls.
    const int l = nodes_[static_cast<std::size_t>(root)].left;
    const int r = nodes_[static_cast<std::size_t>(root)].right;
    const int leftChars = subChars(l);
    const int ownChars = nodes_[static_cast<std::size_t>(root)].chars;

    if (k < leftChars) {
        int newLeft = kNull;
        splitChars(l, k, a, newLeft);
        nodes_[static_cast<std::size_t>(root)].left = newLeft;
        b = root;
        update(b);
        return;
    }
    if (k > leftChars + ownChars) {
        int newRight = kNull;
        splitChars(r, k - leftChars - ownChars, newRight, b);
        nodes_[static_cast<std::size_t>(root)].right = newRight;
        a = root;
        update(a);
        return;
    }
    if (k == leftChars) {
        nodes_[static_cast<std::size_t>(root)].left = kNull;
        a = l;
        b = root;
        update(b);
        return;
    }
    if (k == leftChars + ownChars) {
        nodes_[static_cast<std::size_t>(root)].right = kNull;
        b = r;
        a = root;
        update(a);
        return;
    }

    // The cut point falls inside this node's piece: split the piece in two.
    const int i = k - leftChars; // 0 < i < ownChars
    const Piece p = nodes_[static_cast<std::size_t>(root)].piece;
    const quint32 oldPrio = nodes_[static_cast<std::size_t>(root)].prio;
    const Piece lp{p.buf, p.start, i};
    const Piece rp{p.buf, p.start + i, p.len - i};
    const int ln = newNode(lp);
    const int rn = newNode(rp);
    // Inherit the cut node's heap priority: fresh random priorities could
    // exceed ancestors when the split recursion unwinds and re-attaches
    // these subtrees below existing nodes (which merge() would not see).
    nodes_[static_cast<std::size_t>(ln)].prio = oldPrio;
    nodes_[static_cast<std::size_t>(rn)].prio = oldPrio;
    freeNode(root); // only this node; its subtrees were already captured above
    a = mergeTrees(l, ln);
    b = mergeTrees(rn, r);
}

TextBuffer::Piece TextBuffer::appendToAddBuffer(const QString& text)
{
    const int start = static_cast<int>(buffers_[static_cast<std::size_t>(addBuf_)].size());
    buffers_[static_cast<std::size_t>(addBuf_)].append(text);
    return Piece{addBuf_, start, static_cast<int>(text.size())};
}

void TextBuffer::setText(const QString& text)
{
    recycleTree(root_);
    root_ = kNull;
    buffers_.resize(2); // drop all buffers except the placeholder + old add buffer
    buffers_[0] = QString();
    buffers_[1] = QString();
    addBuf_ = 1;
    if (!text.isEmpty()) {
        const Piece p = appendToAddBuffer(text);
        root_ = newNode(p);
    }
}

void TextBuffer::insertAtOffset(int offset, const QString& text)
{
    if (text.isEmpty())
        return;
    offset = std::clamp(offset, 0, charCount());
    int a = kNull;
    int b = kNull;
    splitChars(root_, offset, a, b);
    const Piece p = appendToAddBuffer(text);
    const int mid = newNode(p);
    root_ = mergeTrees(mergeTrees(a, mid), b);
}

void TextBuffer::removeRange(int startOffset, int endOffset)
{
    if (startOffset > endOffset)
        std::swap(startOffset, endOffset);
    startOffset = std::clamp(startOffset, 0, charCount());
    endOffset = std::clamp(endOffset, 0, charCount());
    if (startOffset == endOffset)
        return;
    int mid = kNull;
    int c = kNull;
    splitChars(root_, endOffset, mid, c);
    int a = kNull;
    int doomed = kNull;
    splitChars(mid, startOffset, a, doomed);
    recycleTree(doomed);
    root_ = mergeTrees(a, c);
}

int TextBuffer::charCount() const
{
    return subChars(root_);
}

int TextBuffer::lineCount() const
{
    return 1 + subLines(root_);
}

int TextBuffer::offsetOfNthNewline(int k) const
{
    // k is 1-based, in [1, subLines(root_)].
    int cur = root_;
    int charBase = 0;
    while (cur != kNull) {
        const Node& n = nodes_[static_cast<std::size_t>(cur)]; // re-index below after descent
        const int leftLines = subLines(n.left);
        if (k <= leftLines) {
            cur = n.left;
            continue;
        }
        k -= leftLines;
        const int start = charBase + subChars(n.left);
        if (k <= n.lines) {
            const QString& buf = buffers_[static_cast<std::size_t>(n.piece.buf)];
            int seen = 0;
            for (int j = 0; j < n.chars; ++j) {
                if (buf.at(n.piece.start + j) == kNewline) {
                    ++seen;
                    if (seen == k)
                        return start + j;
                }
            }
            Q_UNREACHABLE();
        }
        k -= n.lines;
        charBase = start + n.chars;
        cur = n.right;
    }
    Q_UNREACHABLE();
}

int TextBuffer::offsetOfLine(int line) const
{
    if (line < 0 || line >= lineCount()) {
        Q_ASSERT_X(false, "offsetOfLine", "line out of range");
        return 0;
    }
    if (line == 0)
        return 0;
    return offsetOfNthNewline(line) + 1;
}

QString TextBuffer::lineText(int line) const
{
    if (line < 0 || line >= lineCount()) {
        Q_ASSERT_X(false, "lineText", "line out of range");
        return {};
    }
    const int start = offsetOfLine(line);
    const int end = (line + 1 < lineCount()) ? offsetOfNthNewline(line + 1) : charCount();
    QString t = textRange(start, end);
    if (!t.isEmpty() && t.back() == kNewline)
        t.chop(1);
    return t;
}

TextBuffer::LineCol TextBuffer::lineColAt(int offset) const
{
    offset = std::clamp(offset, 0, charCount());
    int line = 0;
    int lineStart = 0; // char offset of the current line start
    std::vector<int> stack;
    int cur = root_;
    int base = 0; // char offset where the current piece starts
    while (cur != kNull || !stack.empty()) {
        while (cur != kNull) {
            stack.push_back(cur);
            cur = nodes_[static_cast<std::size_t>(cur)].left;
        }
        cur = stack.back();
        stack.pop_back();
        const Node& n = nodes_[static_cast<std::size_t>(cur)];
        const QString& buf = buffers_[static_cast<std::size_t>(n.piece.buf)];
        const int pieceStart = base;
        for (int j = 0; j < n.chars; ++j) {
            if (pieceStart + j == offset)
                return {line, offset - lineStart};
            if (buf.at(n.piece.start + j) == kNewline) {
                ++line;
                lineStart = pieceStart + j + 1;
            }
        }
        base += n.chars;
        cur = n.right;
    }
    return {line, offset - lineStart};
}

int TextBuffer::offsetOf(int line, int col) const
{
    const int lineStart = offsetOfLine(line);
    // The caret may sit at the end of a line, i.e. right before its '\n'.
    const int lineEnd = (line + 1 < lineCount()) ? offsetOfNthNewline(line + 1) : charCount();
    return lineStart + std::clamp(col, 0, lineEnd - lineStart);
}

QString TextBuffer::text() const
{
    return textRange(0, charCount());
}

QString TextBuffer::textRange(int startOffset, int endOffset) const
{
    if (startOffset > endOffset)
        std::swap(startOffset, endOffset);
    startOffset = std::clamp(startOffset, 0, charCount());
    endOffset = std::clamp(endOffset, 0, charCount());

    QString out;
    out.reserve(endOffset - startOffset);
    std::vector<int> stack;
    int cur = root_;
    int base = 0; // char offset of the next piece in in-order sequence
    while (cur != kNull || !stack.empty()) {
        while (cur != kNull) {
            stack.push_back(cur);
            cur = nodes_[static_cast<std::size_t>(cur)].left;
        }
        cur = stack.back();
        stack.pop_back();
        const Node& n = nodes_[static_cast<std::size_t>(cur)];
        const int pieceStart = base;
        const int pieceEnd = base + n.chars;
        if (pieceStart >= endOffset)
            break; // remaining pieces are further right
        if (pieceEnd > startOffset) {
            const int from = std::max(startOffset, pieceStart) - pieceStart;
            const int to = std::min(endOffset, pieceEnd) - pieceStart;
            const QString& buf = buffers_[static_cast<std::size_t>(n.piece.buf)];
            out += buf.mid(n.piece.start + from, to - from);
        }
        base = pieceEnd;
        cur = n.right;
    }
    return out;
}

int TextBuffer::debugNodeCount() const
{
    return subCount(root_);
}

int TextBuffer::checkSubtree(int root, bool& ok) const
{
    auto fail = [&ok](const char* what, int node) {
        ok = false;
        qWarning("invariant: %s (node %d)", what, node);
    };
    if (root == kNull)
        return 0; // null subtree contributes 0 nodes
    const Node& n = nodes_[static_cast<std::size_t>(root)];
    if (n.piece.len <= 0 || n.chars != n.piece.len || n.chars <= 0)
        fail("bad piece size", root);
    const QString& buf = buffers_[static_cast<std::size_t>(n.piece.buf)];
    if (n.piece.buf <= 0 || n.piece.start < 0 || n.piece.start + n.piece.len > static_cast<int>(buf.size()))
        fail("piece out of buffer", root);
    if (n.lines != pieceLines(n.piece))
        fail("stale line count", root);
    const int l = checkSubtree(n.left, ok);
    const int r = checkSubtree(n.right, ok);
    if (n.left != kNull && nodes_[static_cast<std::size_t>(n.left)].prio > n.prio)
        fail("heap order left", root);
    if (n.right != kNull && nodes_[static_cast<std::size_t>(n.right)].prio > n.prio)
        fail("heap order right", root);
    if (n.subChars != n.chars + subChars(n.left) + subChars(n.right) ||
        n.subLines != n.lines + subLines(n.left) + subLines(n.right) ||
        n.subCount != 1 + subCount(n.left) + subCount(n.right))
        fail("stale subtree aggregate", root);
    return l + r + 1;
}

bool TextBuffer::debugCheckInvariants() const
{
    bool ok = true;
    const int counted = checkSubtree(root_, ok);
    if (counted != subCount(root_)) {
        ok = false;
        qWarning("invariant: node count mismatch %d vs %d", counted, subCount(root_));
    }
    return ok;
}

} // namespace qmn::core
