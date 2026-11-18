#pragma once

#include <QJsonObject>
#include <QTimer>
#include <QVector>

#include "lsp/LspClient.hpp"
#include "lsp/PositionConverter.hpp"

namespace qmn::lsp {

// The document synchronization machine (PLAN.md GATE 2 / M8, machine 3).
// Layer rule: this layer depends ONLY on src/protocol — never view, never
// core. So the syncer NEVER sees CodeDocument/CodeEditor: the glue feeds it
// the document's edit stream through `applyDelta(start, end, inserted)` — a
// door byte-identical to CodeDocument::editApplied (PRE-OP UTF-16 offsets) —
// and owns the didOpen/didChange/didSave/didClose conversation.
//
// What the syncer owns (all PLAN-pinned):
//   - its OWN text mirror: every delta is replayed into it immediately, so
//     the syncer always holds the current text AND each queued delta's range
//     is computed against the mirror state the SERVER will be in just before
//     it applies that change (the stream is a faithful sequential replay, so
//     mirror-at-edit-k == server-at-change-k). No core, no re-read.
//   - the batch: pending deltas go out as ONE textDocument/didChange whose
//     contentChanges array the server applies SEQUENTIALLY (spec), no
//     coalescing by design. Version increments per FLUSH, not per delta.
//   - the 400 ms debounce + a typing cap: edits that trail in merge into one
//     didChange; a burst that keeps typing still flushes at the cap, so a
//     fast typist does not starve the server.
//   - FULL fallback (a single range-less contentChange): on any desync
//     signal and after a server-primed WorkspaceEdit. Between beginServerEdit
//     / endServerEdit the deltas update the mirror but are NOT queued (the
//     syncer suppresses the echo of edits it is applying on the server's
//     behalf), then endServerEdit sends one full-text sync — the
//     suppress-lsp-echo + force-full-sync pair, living on the syncer
//     side so the view stays dumb (PLAN wording, near-verbatim).
//   - the re-didOpen self-heal: LspClient::ready() re-fires on every (re-)
//     READY (machine 2's contract), and an open document re-syncs in full so
//     a server restart does not leave it looking at stale text.
//
// Position frames: ranges use the NEGOTIATED encoding (cached from
// LspClient::positionEncoding() at open time) via PositionConverter; the
// document-frame mirror is always UTF-16.
//
// Shiboken note: not bound (the wheel wraps the view; LspClient itself is
// unbound here) — but it stays Q_OBJECT, plain-Qt-doors clean.
class DocumentSyncer : public QObject {
    Q_OBJECT
public:
    static constexpr int kDefaultDebounceMs = 400;
    static constexpr int kTypingCapMs = 1200;

    // `client` must outlive the syncer (the glue owns both; the ready()
    // connect uses `this` as context so it is gone with the syncer).
    explicit DocumentSyncer(LspClient* client, QObject* parent = nullptr);

    // The document identity. The uri is the LSP document key (a plain
    // string, machine 2's decision); languageId rides didOpen.
    void setUri(const QString& uri);
    QString uri() const { return uri_; }
    QString languageId() const { return language_; }

    // The negotiated encoding, re-read from the client at each open. Public
    // so the smart fakeserver slot can pin that the two frames agree.
    PositionConverter::Encoding encoding() const { return encoding_; }

    bool isOpen() const { return open_; }
    qint64 version() const { return version_; }
    QString mirrorText() const { return mirror_; } // the syncer's current text
    bool hasPendingChanges() const { return fullSyncNeeded_ || !pending_.isEmpty(); }

    // --- the conversation (all notifications) -----------------------------
    void open(const QString& text, const QString& languageId); // didOpen
    void close();                                              // didClose
    void save(); // didSave (flushes first so the server is current)

    // One primitive edit (CodeDocument::editApplied's shape). Ranges are
    // queued in the server coordinate frame; the mirror advances immediately.
    void applyDelta(int start, int end, const QString& inserted);

    // The server-primed-edit suppression pair.
    void beginServerEdit(); // stop queueing echoes (the mirror still advances)
    void endServerEdit();   // resume + send ONE full-text sync now

    // Mark the next sync FULL (a desync detector's door; the glue calls it
    // when the server reports the range stream stopped matching). The send
    // itself rides the debounce, like any delta.
    void forceFullSync();

    // Flush the pending batch now (the debounce timer calls this too).
    void flush();

    // Test seams for the deterministic debounce pins.
    void setDebounceMs(int ms);
    void setTypingCapMs(int ms);

signals:
    // Emitted after a didChange is actually sent (the version that went out
    // + how many contentChanges it carried). The glue's diagnostics/desync
    // bookkeeping rides this.
    void flushed(qint64 version, int changeCount);

private:
    struct Change {
        LspPosition start;
        LspPosition end;
        QString text;
    };

    void onReady(); // the re-didOpen self-heal
    void sendDidOpen();
    void armBurst();  // first delta of a burst: start the cap clock
    void restartDebounce();
    void disarmTimers();

    LspClient* client_;
    QString uri_;
    QString language_;
    QString mirror_;
    PositionConverter::Encoding encoding_ = PositionConverter::Encoding::Utf16;

    bool open_ = false;
    qint64 version_ = 0;
    QVector<Change> pending_;
    bool fullSyncNeeded_ = false;
    bool suppress_ = false;

    QTimer debounce_;
    QTimer cap_;
    bool burstActive_ = false;

    int debounceMs_ = kDefaultDebounceMs;
    int capMs_ = kTypingCapMs;
};

} // namespace qmn::lsp
