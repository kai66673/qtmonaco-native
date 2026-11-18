#include "lsp/DocumentSyncer.hpp"

#include <QJsonArray>

namespace qmn::lsp {

DocumentSyncer::DocumentSyncer(LspClient* client, QObject* parent)
    : QObject(parent)
    , client_(client)
{
    debounce_.setSingleShot(true);
    connect(&debounce_, &QTimer::timeout, this, &DocumentSyncer::flush);
    cap_.setSingleShot(true);
    // The typing cap: a burst that never pauses still flushes, so a fast
    // typist cannot starve the server (PLAN: incremental sync stays live).
    connect(&cap_, &QTimer::timeout, this, &DocumentSyncer::flush);
    // Machine 2's contract: ready() re-fires on EVERY (re-)READY; an open
    // document re-syncs in full — the didOpen itself carries the mirror, so
    // a server restart never leaves the pair looking at different text.
    connect(client_, &LspClient::ready, this, &DocumentSyncer::onReady);
}

void DocumentSyncer::setUri(const QString& uri)
{
    uri_ = uri;
}

void DocumentSyncer::setDebounceMs(int ms)
{
    debounceMs_ = ms;
}

void DocumentSyncer::setTypingCapMs(int ms)
{
    capMs_ = ms;
}

void DocumentSyncer::open(const QString& text, const QString& languageId)
{
    disarmTimers();
    mirror_ = text;
    language_ = languageId;
    pending_.clear();
    fullSyncNeeded_ = false;
    suppress_ = false;
    version_ = 0; // didOpen carries version 0; every didChange bumps per flush
    open_ = true;
    encoding_ = PositionConverter::fromWire(client_->positionEncoding()); // the
    // NEGOTIATED wire frame, cached once at open
    if (client_->state() == LspClient::State::Ready)
        sendDidOpen(); // the open IS the full sync: no queued changes
    // Not Ready yet: the document stays registered, and the next ready()
    // runs sendDidOpen through the self-heal path — the mirror travels
    // inside it, so deltas fed while the server was down ride along.
}

void DocumentSyncer::close()
{
    if (!open_)
        return;
    open_ = false;
    if (client_->state() == LspClient::State::Ready) {
        QJsonObject params;
        params.insert(QStringLiteral("textDocument"),
                      QJsonObject{{QStringLiteral("uri"), uri_}});
        client_->notify(QStringLiteral("textDocument/didClose"), params);
    }
    disarmTimers();
    pending_.clear();
    fullSyncNeeded_ = false;
}

void DocumentSyncer::save()
{
    flush(); // the server must be current BEFORE didSave (spec ordering)
    if (open_ && client_->state() == LspClient::State::Ready) {
        QJsonObject params;
        params.insert(QStringLiteral("textDocument"),
                      QJsonObject{{QStringLiteral("uri"), uri_}});
        client_->notify(QStringLiteral("textDocument/didSave"), params);
    }
}

void DocumentSyncer::applyDelta(int start, int end, const QString& inserted)
{
    if (suppress_) {
        // The echo of an edit the SERVER primed: the mirror advances (the
        // text really changed), but the delta must never travel back.
        mirror_.replace(start, end - start, inserted);
        return;
    }
    pending_.append(
        Change{PositionConverter::toPosition(mirror_, start, encoding_),
               PositionConverter::toPosition(mirror_, end, encoding_),
               inserted});
    mirror_.replace(start, end - start, inserted);
    armBurst();
    restartDebounce();
}

void DocumentSyncer::beginServerEdit()
{
    flush(); // the user's queued deltas must reach the server FIRST: the
             // server primed its edit against that state
    suppress_ = true;
}

void DocumentSyncer::endServerEdit()
{
    suppress_ = false;
    fullSyncNeeded_ = true; // one range-less contentChange tells the server
                            // its echo landed exactly as we replayed it
    flush();
}

void DocumentSyncer::forceFullSync()
{
    fullSyncNeeded_ = true;
    restartDebounce();
}

void DocumentSyncer::flush()
{
    debounce_.stop();
    cap_.stop();
    burstActive_ = false;
    if (!open_ || (!fullSyncNeeded_ && pending_.isEmpty()))
        return;
    if (client_->state() != LspClient::State::Ready)
        return; // the next ready() re-didOpens the mirror whole: no partial
                // stream to resume, nothing lost, nothing queued twice

    QJsonArray changes;
    int changeCount = 0;
    if (fullSyncNeeded_) {
        QJsonObject change;
        change.insert(QStringLiteral("text"), mirror_);
        changes.append(change);
        changeCount = 1;
    } else {
        for (const Change& change : std::as_const(pending_)) {
            QJsonObject entry;
            entry.insert(QStringLiteral("range"),
                         QJsonObject{
                             {QStringLiteral("start"),
                              QJsonObject{{QStringLiteral("line"),
                                           change.start.line},
                                          {QStringLiteral("character"),
                                           change.start.character}}},
                             {QStringLiteral("end"),
                              QJsonObject{{QStringLiteral("line"),
                                           change.end.line},
                                          {QStringLiteral("character"),
                                           change.end.character}}}});
            entry.insert(QStringLiteral("text"), change.text);
            changes.append(entry);
        }
        changeCount = pending_.size();
    }
    ++version_; // the version identifies the WHOLE batch, not each delta
    QJsonObject params;
    params.insert(
        QStringLiteral("textDocument"),
        QJsonObject{{QStringLiteral("uri"), uri_},
                    {QStringLiteral("version"), static_cast<double>(version_)}});
    params.insert(QStringLiteral("contentChanges"), changes);
    client_->notify(QStringLiteral("textDocument/didChange"), params);
    pending_.clear();
    fullSyncNeeded_ = false;
    emit flushed(version_, changeCount);
}

void DocumentSyncer::onReady()
{
    if (!open_)
        return;
    sendDidOpen(); // the self-heal: didOpen IS a full sync, so any deltas
                   // that piled up (or were fed) while the session was down
                   // travel inside the mirror text
    encoding_ = PositionConverter::fromWire(client_->positionEncoding());
    pending_.clear(); // the mirror just went out whole; old-frame deltas must
                      // never replay against the fresh server state
    fullSyncNeeded_ = false;
    disarmTimers();
}

void DocumentSyncer::sendDidOpen()
{
    QJsonObject params;
    params.insert(QStringLiteral("textDocument"),
                  QJsonObject{{QStringLiteral("uri"), uri_},
                              {QStringLiteral("languageId"), language_},
                              {QStringLiteral("version"),
                               static_cast<double>(version_)},
                              {QStringLiteral("text"), mirror_}});
    client_->notify(QStringLiteral("textDocument/didOpen"), params);
}

void DocumentSyncer::armBurst()
{
    if (!burstActive_) {
        burstActive_ = true;
        cap_.start(capMs_);
    }
}

void DocumentSyncer::restartDebounce()
{
    debounce_.start(debounceMs_);
}

void DocumentSyncer::disarmTimers()
{
    debounce_.stop();
    cap_.stop();
    burstActive_ = false;
}

} // namespace qmn::lsp
