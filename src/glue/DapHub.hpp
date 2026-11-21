#pragma once

// Car 4 machine D3 (PLAN GATE 2 "D3 — DapHub ON THE GLUE SHELF"): the DAP
// glue hub, the sibling of LspHub on the same shelf (the shelf = LspHub +
// ProjectSession + DapHub). This is where the view and the dap layers meet
// — neither knows the other (src/dap speaks only src/protocol; the view
// knows no protocol at all), and the G2 registry (ProjectSession) earns
// its keep HERE: every DAP `source` resolves through the session's uri
// map, and the hub NEVER touches disk (the G2 no-disk-I/O rule; an app
// that wants an outside frame opens an editor and openDocument()s it).
//
// ONE debug session at a time (PLAN-decided: an adapter is NOT a language
// server — no spec matrix, no .qmn/dap.json; program/args/launchArgs ride
// startDebugging()). The hub owns the single DapClient, and:
//
//   - the BREAKPOINT truth lives in the APP (a breakpoint UI is app
//     scope); the hub stores what the app set, sends DAP setBreakpoints
//     (1-BASED lines — the client's declared linesStartAt1 pair makes the
//     conversion total) and re-feeds the M5 decorations from the reply:
//     verified lines ride setBreakpointLines (the DOT means executable,
//     and the verified line may MOVE — the dot lands on the ACTUAL line,
//     the desired→actual map rides the store), unverified ones become
//     Warning Diagnostics (source "dap") — the iide "ignored
//     body.breakpoints" anti-pattern dies here;
//   - breakpoints are LINE-ANCHORED (no anchor layer exists in the model),
//     so an edit of a bound document RESENDS setBreakpoints after the
//     syncer-shaped kBreakpointResendMs debounce — line-number honesty,
//     recorded;
//   - the STOPPED feed: stopped → v1 SINGLE-THREAD (the last threadId; a
//     threads UI is a recorded deferred sliver) → stackTrace → top frame →
//     setPosition + setHighlightedLines on the resolved editor, cleared
//     on `continued`; then `variables` of the FIRST top-frame scope,
//     matched as WORDS on the stopped line, `= value` painted at the
//     word-end col via setInlayHints — the M5 door finally carries the
//     DAP meaning its name was minted with;
//   - adapter death (failed/stopped/terminated-relay) clears EVERY feed
//     and relays; the desired-breakpoint store survives a death (the
//     re-push on the next session's ready() is the same code path).
//
// No text sync at all for DAP: adapters see line numbers, never bytes.
// configurationDone() is an EXPLICIT door (the app says setup ended),
// DEFERRED one-shot while the session's setBreakpoints pushes are still
// unanswered — the live-gdb ordering rule, see setupPending_ below.
// stopDebugging() rides the codec-shaped disconnect barrier; the hub dtor
// pumps that barrier bounded (the M8 machine-4 lesson, paid twice).
//
// The setTransportFactory seam mirrors LspHub's (C++-only, honest
// absence); every ANSWER route is a SIGNAL and every payload is a plain
// Qt type — the D4 note's Shiboken-shaped discipline, so the wheel
// follow-on is a three-copies growth, not a surgery.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

#include "dap/DapClient.hpp"

QT_FORWARD_DECLARE_CLASS(QTimer)

namespace qmn::view {
class CodeEditor;
class CodeDocument;
} // namespace qmn::view

namespace qmn::glue {

class ProjectSession;

class DapHub : public QObject {
    Q_OBJECT
public:
    // Bounded budgets (the LspHub precedents): the dtor pump for the
    // disconnect barrier, and the breakpoint-resend debounce after the
    // LAST edit of a bound document (same intent as DocumentSyncer's
    // change-collection window, applied to line numbers).
    static constexpr int kStopBarrierMs = 3'000;
    static constexpr int kBreakpointResendMs = 400;
    // Value-hint cap per stopped feed (per DESIGN: top-level scope only,
    // first kMaxValueHints variables, first occurrence per NAME).
    static constexpr int kMaxValueHints = 20;

    // The swap-the-pipe seam (the LspHub shape, test-injected; DEFAULT:
    // spawn the adapter program via ProcessTransport). Called ONLY by
    // startDebugging(). C++-only door — the TransportFactory seam precedent.
    using TransportFactory =
        std::function<qmn::protocol::Transport*(const QString& program,
                                                const QStringList& args,
                                                QObject* parent)>;
    void setTransportFactory(TransportFactory factory);

    explicit DapHub(QObject* parent = nullptr);
    ~DapHub() override; // stops a live session through the disconnect
                       // barrier with a BOUNDED pump (never a SIGKILLed
                       // adapter that never saw its disconnect)

    // --- the project (NOT owned) ------------------------------------------
    // Source resolution reads the session's uri map; a session dying while
    // attached simply unattaches (QPointer). Late attach is fine — the
    // map is consulted per frame, nothing is cached from it.
    void attachProject(ProjectSession* session);
    ProjectSession* project() const { return session_; }

    // --- the one session ----------------------------------------------------
    // adapterId = the DAP initialize `adapterID` (the adapter TYPE name:
    // "lldb", "gdb", ...); empty = the program's basename. launchArgs
    // travel into the `launch` request verbatim (plus the hub's fills of
    // request:"launch"/noDebug:false when absent — the spec-required pair;
    // an app that wants `attach` says so here). Returns false if a session
    // is already Starting/Ready/Stopping (one session at a time — the
    // PLAN-decided shape; the previous one must end first).
    bool startDebugging(const QString& adapterProgram,
                        const QStringList& adapterArgs = {},
                        const QJsonObject& launchArgs = {},
                        const QString& adapterId = QString());
    void stopDebugging(); // the disconnect barrier (codecs's shutdownCommand)
    // Capability-gated (`supportsTerminateRequest`): sends DAP `terminate`
    // first, then the barrier. Without the capability it falls back to
    // stopDebugging() and returns false (the honest "no terminate here").
    bool terminateDebugging();

    bool isDebugging() const; // Starting or Ready
    bool isReady() const;
    QJsonObject adapterCapabilities() const; // {} until the handshake

    // The EXPLICIT configurationDone door (the app says setup ended): sent
    // only Ready, SKIPPED when the capabilities say no
    // supportsConfigurationDoneRequest (some adapters default true and
    // never publish the key — the DAP spec's own note). DEFERRED (one shot)
    // while the session's setBreakpoints pushes are still unanswered —
    // ordering it into the breakpoint setup is the live-gdb rule (a
    // configurationDone answered mid-registration starts the inferior
    // before the pending breakpoints resolve).
    void configurationDone();

    // A fire-and-forget command passthrough (the app's continue/next/step
    // buttons): returns the request id, or -1 when not Ready. The answer
    // body is DISCARDED by design — DAP's interesting answers are EVENTS,
    // and they all ride dapEvent. Raw DapClient access (C++-only; its
    // type stays unbound, the same drop-by-type-absence shape as the
    // LspHub's syncerFor door):
    qint64 request(const QString& command, const QJsonValue& arguments);
    qmn::dap::DapClient* client() const { return client_.get(); }

    // --- breakpoints (the app owns the truth) ------------------------------
    // Stores per uri, sends setBreakpoints when Ready, re-feeds the
    // decorations from the reply (and from `breakpoint` EVENTS through the
    // same doors). Empty lines = a legal clear (DAP's deleteBreakpoints
    // shape: setBreakpoints with an empty array). False: null editor or a
    // document with no uri yet (a breakpoint needs an identity).
    bool setBreakpoints(qmn::view::CodeEditor* editor,
                        const QVector<int>& modelLines);
    QVector<int> requestedBreakpoints(const qmn::view::CodeEditor* editor) const;
    // desired→actual for the LAST answered setBreakpoints (an entry only
    // while a reply carried that line); desired→desired while unverified.
    QMap<int, int> breakpointMap(const qmn::view::CodeEditor* editor) const;

signals:
    void sessionStarted();                       // handshake Ready
    void sessionEnded(const QString& reason);    // clean stop or death
    // The `launch` request's answer said success:false (a real adapter
    // refusing the program is the everyday shape). The SESSION stays as it
    // is — deciding relaunch-vs-stop is the app's call, same split as the
    // terminated-event rule; silently swallowing the refusal is the lie
    // this signal kills (the D4 demo door shows it in the status bar).
    void launchRejected(const QString& message);
    // Debug-stop bookkeeping (the single-thread v1 shape):
    void debugStopped(int threadId, const QString& reason);
    void debugContinued();
    // A frame/source the session's open documents do not cover: the app
    // opens an editor + openDocument()s it and the NEXT feed resolves it
    // (the hub never reads disk, never silently opens anything).
    void sourceUnresolved(const QJsonObject& source, const QJsonObject& frame);
    // The client relays, carried up (events incl. terminated/output;
    // reverse requests answered through client()->respond DURING the
    // signal; stderr stays its own channel — the PLAN-pinned split):
    void dapEvent(const QString& name, const QJsonValue& body);
    void dapRequest(const QJsonObject& request);
    void adapterStderr(const QString& line);

private:
    struct BrEntry { // one desired line's last known state
        int desiredLine = 0; // model (0-based)
        int actualLine = 0;  // DAP-verified model line (0-based; ==desired while unverified)
        bool verified = false;
        QString message;     // the reply's message/instruction (unverified tooltip)
        int id = 0;          // the DAP breakpoint id (0 = never assigned); `breakpoint` events key by it
    };

    struct BoundSource { // per normalized uri
        QPointer<qmn::view::CodeEditor> editor; // re-resolvable via the session
        QVector<BrEntry> entries;               // desired order (index-paired with replies)
        QTimer* resend = nullptr;               // kBreakpointResendMs debounce, hub-parented
        QMetaObject::Connection editConn;       // onto the document's textChanged
    };

    // resolved through the session's uri map ONLY (exact string, then
    // ProjectSession's own bare-path normalization inside editorForUri);
    // empty path = unresolved. NEVER disk I/O.
    qmn::view::CodeEditor* editorForSourcePath(const QString& path) const;
    QString uriForEditor(const qmn::view::CodeEditor* editor) const;
    static QString localPathForUri(const QString& uri); // file:// -> path, else verbatim

    BoundSource* boundForUri(const QString& uri);       // may create
    BoundSource* findBound(const QString& uri) const;   // no create
    QString resolveEditorUri(qmn::view::CodeEditor* editor,
                             QString* normalizedOut) const;

    void sendSetBreakpoints(const QString& uri);       // the DAP command
    void settleConfigurationDone();                    // the deferred word
                                                       // when the last setup
                                                       // answer lands
    void applyBreakpointBody(const QString& uri, const QJsonObject& body);
    void markAllUnverified(const QString& uri, const QString& message);
    void feedDecorations(const QString& uri);          // the two M5 doors
    void updateSourceRefs(const QString& uri, const QJsonArray& bodyBreakpoints);
    void handleBreakpointEvent(const QJsonValue& body);
    void handleStopped(const QJsonObject& body);       // the stackTrace leg
    void handleStackTrace(quint64 gen,
                          const qmn::protocol::RpcClient::Reply& reply);
    void handleScopes(qmn::view::CodeEditor* editor, int line, quint64 gen,
                      const qmn::protocol::RpcClient::Reply& reply);
    void handleVariables(qmn::view::CodeEditor* editor, int line, quint64 gen,
                         const qmn::protocol::RpcClient::Reply& reply);
    void clearStopFeed();                              // band + hints
    void clearAllFeeds();                              // + dots + dap diags
    void onAdapterGone(const QString& reason);         // failed/stopped merge

    void waitForStops(); // the bounded dtor pump (the LspHub shape)

    TransportFactory factory_;
    std::unique_ptr<qmn::dap::DapClient> client_;
    QPointer<ProjectSession> session_;

    QHash<QString, BoundSource> bound_; // key: normalized uri
    QHash<int, QString> sourceRefPaths_; // sourceReference -> path (the hub's own store, filled from setBreakpoints replies + loadedSource events)

    // Two generations (the async-reply guard): sessionGen_ bumps on every
    // startDebugging/adapter end (old setBreakpoints replies must not
    // restain a fresh session's feed), stopGen_ on every
    // stopped/continued/death (old stackTrace/scopes/variables chains drop).
    quint64 sessionGen_ = 0;
    quint64 stopGen_ = 0;

    // SETUP serialization (the car-4 close-ritual lesson, pinned against a
    // LIVE gdb): the app's configurationDone word must reach the adapter
    // strictly AFTER the breakpoint pushes are ANSWERED — gdb starts the
    // inferior on an early configurationDone, before the pending
    // breakpoints resolve, and the user's stop never lands. setupPending_
    // counts the outstanding setBreakpoints pushes ONLY; the launch is
    // deliberately OUT of the band (the live-gdb daplog: that adapter
    // answers `launch` only AFTER configurationDone, so gating the word
    // on the launch answer deadlocks the two sides against each other).
    // cfgDoneWaiting_ is the one-shot deferred word.
    int setupPending_ = 0;
    bool cfgDoneWaiting_ = false;

    QString adapterProgram_;
    QStringList adapterArgs_;
    QString adapterId_;
    QJsonObject launchArgs_;

    // the stopped feed (v1 single-thread):
    int lastThreadId_ = 0;
    QPointer<qmn::view::CodeEditor> stopEditor_; // where the band + hints
                                                 // landed (same editor: one
                                                 // resolved frame feeds both)
    bool closingDown_ = false;
};

} // namespace qmn::glue
