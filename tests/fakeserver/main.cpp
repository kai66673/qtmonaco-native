// The LSP fakeserver (tests support binary, NOT a ctest entry): a QProcess-
// spawned stdio server speaking our own JsonFraming (tests/CMakeLists wires
// tst_lsp's QMN_TEST_LSP_SERVER to this target). PROTOCOL rides stdout,
// diagnostics/corpus-cannery checks ride stderr — the ProcessTransport keeps
// those channels separate, and fakeserver exercises that split from the far
// side of the pipe.
//
// Modes (argv[1], pinned with the machine-2 tst_lsp slots):
//   basic    handshake + post-ready notification channels + a
//            showMessageRequest (expects the client's null answer); unknown
//            REQUEST methods are echoed (the feature-door round-trip),
//            unknown NOTIFICATIONS must be ignored — the stream must stay
//            alive past them (the battery's final probe is the liveness
//            oracle, any stray client response is an id-mismatch exit 2).
//   stderr   stderr marker at startup + a PROTOCOL logMessage afterwards:
//            the two channels must never cross on the client.
//   crash    answers the handshake, then _exit(7): every session dies, so
//            the client's ONE auto-restart + FAILED + restartServer() arms
//            the whole crash/restart ladder.
//   deaf     starts and stays alive but NEVER answers initialize (the 10 s
//            handshake budget is what has to fail it; shutdown/exit are
//            answered, the deafness is specific to the handshake), exits 0
//            on the exit wire or on stdin EOF.
//   requests handshake, then the server->client battery (always-answer +
//            capability-registry oracles, see runBattery); validates every
//            client answer itself, exits 0 only when all answers matched,
//            exits 2 with a stderr explanation otherwise. "noapply" (argv[2])
//            makes the applyEdit step wait for the TEST's respondApplyEdit
//            door instead of the client answering itself.
//   sync     a stateful text server for machines 3+4 (document syncer +
//            position converter + the hub): keeps an INDEPENDENT server-side
//            UTF-16 text model PER URI (M4: one client carries many
//            documents; didChange/didClose/didSave for a closed or unknown
//            uri are structural lies, exit 2), applies
//            didOpen/didChange (ranges decoded per the
//            encoding named in argv[2]: utf-8|utf-16|utf-32, advertised in
//            the initialize result) / didSave / didClose, and answers the
//            TEST-ONLY request "test/assertModel" (uri-qualified, the
//            no-uri form keeps meaning the lone open document) with
//            {ok,model,version,saves,open} plus "test/documentCount" with
//            {count}. A TEXT mismatch is an honest ok:false the test
//            asserts against — NOT an exit 2 (a shared decoder would let
//            client and server bugs cancel out, so the decoder here is its
//            own spec implementation, and the diags ENCODER below is its
//            own too). Exit 2 stays reserved for structural
//            lies: version not strictly increasing, didChange without
//            didOpen, uri confusion, empty contentChanges. "crashonce"
//            _exit(7)s right after the FIRST handshake
//            of THIS PROCESS (the flag is per-process state: the test
//            harness hands it to spawn #0 only). The syncer's re-didOpen
//            self-heal then rides the client's auto-restart. "diags"
//            publishes textDocument/publishDiagnostics (the "bug"-marker
//            scan, ranges in the NEGOTIATED frame) after every open/change
//            — machine 4's decoder path through the hub. "features" rides the
//            sync model and advertises the machine-5 provider set, answering
//            the feature requests with PROOF objects: this server decodes the
//            caret/positions in ITS OWN frame and reports the offset it
//            landed on, so a client encoder bug shows up as an off-target
//            proof instead of passing quietly. Two deliberately OFF provider
//            shapes ride it: "hoverProvider:false" and a completionProvider
//            object WITHOUT resolveProvider (the "completiononly" flag
//            advertises that provider alone) — both must be refused LOCALLY
//            by the hub, and a request for either that reaches THIS server is
//            a gate bug: exit 2. In features mode a didSave whose text
//            contains "APPLYEDIT" primes a workspace/applyEdit (the server
//            applies the same edit to its OWN model first, then compares the
//            client's next full sync: APPLYCHECK-OK), "APPLYREFUSE" primes an
//            edit that MUST be refused (APPLYREFUSE-OK), "MSGREQ-FIRST" and
//            "MSGREQ-NULL" prime a window/showMessageRequest whose answer this
//            server validates (MSGREQ-OK). Feature replies are PROOF objects,
//            not spec-shaped payloads: machine 5 pins the FRAME the hub sends
//            and the write path; the real Hover/Location/CompletionList
//            decoding joins with machine 6's popups. The "config" flag rides
//            the sync model and watches machine 7's
//            workspace/didChangeConfiguration feed: a config that arrives
//            BEFORE the `initialized` notification, or a params/settings
//            object that is absent or empty, is a structural lie (exit 2),
//            and every accepted settings object echoes back as a
//            window/showMessage "CONFIG-<n> <compact-json>" so the test sees
//            the payload and the order of what actually crossed the wire.
//            GATE 3 car C adds the fold authority: "foldingrange" advertises
//            foldingRangeProvider (after reading the CLIENT's
//            lineFoldingOnly claim back — a client that asks for folds without
//            declaring it is exit 2) and answers every ask from its OWN text
//            mirror, so an edit changes the answer; "foldempty" answers the
//            empty array, "foldfail=<N>" refuses asks past N (the claim that
//            fell through), "folddelay=<N>" holds the answer so a test can
//            close the document over an ask still in flight. A foldingRange
//            ask that reaches a server WITHOUT the flag is a hub gate bug:
//            exit 2, like the other OFF providers. Car C adds the hint
//            authority next to the fold one: "inlayhint" advertises
//            inlayHintProvider (after reading the CLIENT's textDocument.
//            inlayHint claim back — absent is exit 2) and answers every ask
//            from its own text mirror, echoing the LAST ask's range end
//            through "test/hintState" (that end naming the CURRENT mirror IS
//            the flush-before-ask proof, far-side); "hintnull" answers NULL
//            (the empty-set authority), "hintfail=<N>" refuses past N (the
//            hint door's ASYMMETRY with folds: the last set stands),
//            "hintmany=<N>" answers N synthetic hints (the client cap pin),
//            "hintdelay=<N>" holds the answer for the close-over-in-flight
//            cancel pin. An inlayHint ask without the flag is exit 2 too.
//            Car C (C5) adds the PULL DIAGNOSTICS authority: `pull`
//            advertises diagnosticProvider — after reading the CLIENT's
//            textDocument.diagnostic claim back, INCLUDING its content (a
//            relatedDocumentSupport:true claim is exit 2, the far-side pin
//            that the hub's v1 declare says false) — and answers full
//            reports (`kind:"full"` + resultId + items scanned from its own
//            mirror around "err" tokens, plus decoder noise: an inverted
//            range, a {999,x} out-of-doc range, a severity 9, a bare
//            number). An ask with a previousResultId is exit 2 (the hub
//            only pulls FULL) and an ask whose identifier is not "qmn" is
//            exit 2; an ask reaching a provider-less server is exit 2 like
//            the other OFF providers. "pulldynamic" skips the static
//            provider and registers textDocument/diagnostic via
//            client/registerCapability right after the handshake;
//            "pullunchanged" answers kind "unchanged" from ask 2 on (the
//            paint-stays pin); "pullpublish" rides a CONTRADICTING
//            publishDiagnostics AROUND every answer (on the wire it lands
//            BEFORE the report — the suppression must hold in either
//            order);
//            "pullfail=<N>" refuses past ask N (the fallback: the hub flips
//            to push and stops asking); "pullid=<S>" sets the resultId.
//            "test/pullState" echoes the answered-ask count and how many
//            mirror lines the last full report marked — the flush-before-
//            ask proof, far side.
//
// The exit code IS the oracle for the answers the client side cannot see
// (the battery's wire shapes); tst_lsp asserts exitCode 0 where the mode's
// contract ends in a clean exit.

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QStringList>
#include <QTimer>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include <unistd.h>

#include "protocol/JsonFraming.hpp"

using qmn::protocol::JsonFraming;

namespace {

QString g_mode;
bool g_noApply = false;
bool g_batteryDone = false;
QVector<QJsonObject> g_recorded; // every inbound message, for the checks

// ---- sync-mode state (machine 3: the text model + the test door) ---------
QString g_syncEncoding = QStringLiteral("utf-16"); // argv[2], advertised
bool g_crashOnce = false;        // argv[3]: kill the FIRST session only
bool g_firstHandshakeDone = false;
// sync-mode document table (M4: keyed by uri — one client per spec carries
// MANY documents, VS Code shape). The assert door answers per-uri.
struct ModelDoc {
    QString languageId;
    QString text;            // the server-side text (UTF-16 units)
    qint64 version = 0;      // last accepted didChange version
    int saves = 0;
};
QMap<QString, ModelDoc> g_docs;
bool g_publishDiags = false; // argv flag: publish diagnostics on every
                             // open/change (didOpen/didChange of an open
                             // document)
bool g_diagSource = false; // argv flag (post-1.8.0 shared-hover sliver):
                           // every published record carries the LSP
                           // `source` label "fakesrv" — and the hub's
                           // codeAction context re-encodes it back, so
                           // machine 5's scan-compare stays symmetric
bool g_features = false;     // argv flag (rides sync): the machine-5 provider
                             // set + feature-request answers + the
                             // server-primed write path
bool g_completionOnly = false; // argv flag (rides features): advertise ONLY
                               // completionProvider, WITHOUT resolveProvider:
                               // the nested capability gate's refusal pin
bool g_wswu = false; // argv flag (rides sync): declares
                     // textDocumentSync.willSaveWaitUntil and answers the
                     // pre-save round trip (M8 machine 6)
bool g_configPin = false; // argv flag (rides sync): watches the M8 machine-7
                          // workspace/didChangeConfiguration feed (ordering +
                          // payload), and echoes every settings object back
                          // as a window/showMessage CONFIG-<n> marker
bool g_hoverOn = false;   // argv flag (rides features): FLIPS hoverProvider
                          // to true and answers hover for real — the popup-
                          // trigger sliver's door. Without it the machine-5
                          // OFF-provider tripwire stands (hover reaching the
                          // server still exits 2).
bool g_sigHelp = false;   // GATE 3 car C (C2): the "signaturehelp" mode's
                          // provider flag — signatureHelpProvider with "("
                          // and "," triggerCharacters, hoverProvider FALSE
                          // (the shared popup keeps one owner per mode), the
                          // SIGSTATE echo door, and the sigdelay hold.
                          // completionon rides it to light BOTH debounces.
QStringList g_completionTriggers; // the ADVERTISED completion trigger set,
                                  // mirrored from the capability block so the
                                  // handler's refusal of an un-advertised
                                  // char can never drift from what the client
                                  // was told (features: ["."]; the car C
                                  // completionon shape: ["s"])
bool g_foldRange = false; // GATE 3 car C (C3): foldingRangeProvider + the
                          // foldReport answer. RIDES sync mode: the fold
                          // oracle reads this server's own text MIRROR (the
                          // didOpen/didChange tracking sync owns), and the
                          // test/* echo doors are sync's too.
bool g_foldEmpty = false; // argv "foldempty" (rides foldingrange): answer the
                          // EMPTY array — the "the authority says nothing is
                          // foldable" pin (the grammar must stay mute)
int g_foldFailAfter = -1; // argv "foldfail=<N>" (rides foldingrange): answer
                          // an ERROR from ask N+1 onward — the claim-fell-
                          // through pin (the hub hands folds back to its
                          // grammar and stops asking)
int g_foldAsks = 0;       // foldingRange asks this server has ANSWERED
int g_foldDelayMs = 0;    // argv "folddelay=<N>": hold the answer N ms so a
                          // test can close the document over an IN-FLIGHT ask
                          // (the $/cancelRequest teardown pin)
bool g_inlayHint = false; // GATE 3 car C (C4): inlayHintProvider + the
                          // featureInlayHint answer. RIDES sync mode exactly
                          // like the fold flags: the hint oracle reads this
                          // server's own text MIRROR, and the hint POSITIONS
                          // are only honest after the sync didChange bytes
                          // have arrived.
bool g_hintNull = false;  // argv "hintnull" (rides inlayhint): answer NULL —
                          // the spec's "no hints", a whole-set authority the
                          // hub must paint as the EMPTY set
int g_hintFailAfter = -1; // argv "hintfail=<N>": answer an ERROR from ask
                          // N+1 onward — the hint door's recorded ASYMMETRY
                          // with folds: the last set STANDS and the next edge
                          // re-arms (no refused bit, nothing to hand back to)
int g_hintMany = 0;       // argv "hintmany=<N>": answer N synthetic hints —
                          // the client-side cap pin (kMaxLspInlayHints)
int g_hintDelayMs = 0;    // argv "hintdelay=<N>": hold the answer (the
                          // close-over-in-flight cancel pin)
int g_hintAsks = 0;       // inlayHint asks this server has ANSWERED
bool g_pullDiag = false;    // GATE 3 car C (C5): the PULL DIAGNOSTICS
// authority flag ("pull", RIDES sync mode like every car-C flag: the pull
// report decodes against the sync frame, so didChange tracking IS the
// answer's input). Advertises diagnosticProvider — after reading the
// client's textDocument.diagnostic claim back: absent, or a
// relatedDocumentSupport claim, is exit 2 — and answers full reports
// scanned from its own mirror ("err" tokens, kind "full" + a resultId).
bool g_pullDyn = false;     // argv "pulldynamic" (rides pull): NO static
// provider — instead a client/registerCapability(textDocument/diagnostic)
// notification rides out right after the handshake, and the hub's
// registry-driven re-arm answers the pull from there.
bool g_pullUnchanged = false; // argv "pullunchanged" (rides pull): answer
// kind "unchanged" from ask 2 on — the paint-stays pin (the hub writes the
// door ZERO times on such an answer).
bool g_pullPublish = false; // argv "pullpublish" (rides pull): a
// CONTRADICTING textDocument/publishDiagnostics rides after every pull
// answer — while the pull is live the hub must SUPPRESS it (no "bug" entry
// ever paints), and once the pull falls through the push route resumes and
// the very same publish paints (the fallback's legible trail).
int g_pullFailAfter = -1;   // argv "pullfail=<N>": answer an ERROR (-32601)
// from ask N+1 on — the FALLBACK pin: the hub flips this document back to
// push, stops asking (the refused bit, no hammering), and clears ONLY a
// pull-generation paint.
int g_pullAsks = 0;         // textDocument/diagnostic asks ANSWERED
int g_pullLastMarkerLines = -1; // how many mirror lines the LAST FULL
// report marked, echoed by test/pullState — a mirror-derived report count
// proves the flush-before-ask rule far-side (an ask that outran didChange
// could not mark a token the server never saw).
QString g_pullResultId = QStringLiteral("r1"); // argv "pullid=<S>": the
// resultId the reports carry (stored by the hub, NEVER sent back — the
// full-pull rule the featureDiagnostic guard pins far-side).
int g_hintLastEndLine = -1; // the LAST ask's range end, echoed by test/
int g_hintLastEndChar = -1; // hintState: the flush-before-ask proof lives in
                            // THESE numbers far-side (an end naming the
                            // CURRENT mirror means the didChange bytes led
                            // the ask)
bool g_reqLog = false;    // argv flag: every feature request logs
                          // "REQ <method>" to stderr (the hub-relayed STDERR
                          // channel: the trigger tests' traffic oracle)
bool g_cancelLog = false; // argv flag: every $/cancelRequest logs
                          // "CANCELREQ <id>" to stderr
int g_completionDelayMs = 0; // argv "completiondelay=<N>": N ms of latency
                             // BEFORE the completion reply, so a test can
                             // supersede an IN-FLIGHT request (the hub's
                             // $/cancelRequest path)
int g_sigDelayMs = 0;        // argv "sigdelay=<N>": the same latency trick
                             // for the signatureHelp reply (car C supersede
                             // pin — an IN-FLIGHT ask the hub must cancel)
// The signatureHelp mode's server-side record (car C, C2): what the LAST
// signatureHelp request carried, decoded in this server's own frame, plus
// the ask counter — the door that makes the hub's WIRE SHAPE (triggerKind,
// triggerCharacter, the position it aimed at) checkable, not inferred.
int g_sigAsks = 0;
int g_sigLastKind = 0;
QString g_sigLastChar;
int g_sigLastOffset = -1;

bool g_initializedSeen = false; // the `initialized` notification landed
int g_configsSeen = 0;          // configs this server has accepted
bool g_advertisesResolve = false; // features && !completiononly, decided at
                                  // initialize; the resolve tripwire's oracle
// Server-PRIMED requests, id -> method: a machine-5 pin lives on the SERVER
// side too (what the CLIENT answered to our own request is traffic the test
// cannot see), so the runLoop's response branch validates it here.
QMap<QString, QString> g_outstanding;
QString g_applyVerify;      // features mode: the text the client's NEXT full
                            // sync must carry after a server-primed applyEdit
                            // (the REFUSED branch needs no such oracle: the
                            // tst pins "no spurious echo" from the VERSION
                            // side, and the model check rides the response)
int g_featureSaveSeq = 0;   // features mode: unique ids for the didSave-primed
                            // server requests (one marker per didSave text)
QJsonObject g_initializeParams; // the params the handshake received (the
                                // M4 test echo door: the hub's built params
                                // ride it back to the test)

// Exactly ONE framing for stdin (the battery's blocking reads and the
// notifier callback never run concurrently: the battery runs inside a
// callback that is parked on the stack).
JsonFraming g_framing;

[[noreturn]] void fail(const QString& what)
{
    std::fprintf(stderr, "FAKESERVER-FAIL %s\n", qPrintable(what));
    std::fflush(stderr);
    _exit(2);
}

void send(const QJsonObject& message)
{
    const QByteArray framed
        = JsonFraming::encode(QJsonDocument(message).toJson(QJsonDocument::Compact));
    const char* bytes = framed.constData();
    int left = framed.size();
    while (left > 0) {
        const ssize_t n = ::write(1, bytes, size_t(left));
        if (n <= 0)
            fail(QStringLiteral("stdout write failed: %1")
                     .arg(QString::fromUtf8(std::strerror(errno))));
        bytes += n;
        left -= int(n);
    }
}

QJsonObject response(const QJsonValue& id, const QJsonValue& result)
{
    QJsonObject m;
    m.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("result"), result);
    return m;
}

void notify(const QString& method, const QJsonValue& params)
{
    QJsonObject m;
    m.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    m.insert(QStringLiteral("method"), method);
    if (!params.isUndefined())
        m.insert(QStringLiteral("params"), params);
    send(m);
}

void answerErrorTo(const QJsonValue& id, int code, const QString& text)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), text);
    QJsonObject m;
    m.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("error"), error);
    send(m);
}

void answerError(const QJsonObject& request, int code)
{
    answerErrorTo(request.value(QStringLiteral("id")), code,
                  QStringLiteral("server-side check"));
}

// --------------------------------------------------------- battery oracles
// Every entry validates one CLIENT answer (the LspClient policy handlers run
// inside the tst process; their wire answers come back here as responses).
struct Step {
    QJsonObject request;
    std::function<void(const QJsonObject&)> checks; // fail() on a lie
};

// The battery shares the runLoop's framing (there is exactly one stdin
// stream; during the blocking battery read the notifier callback is parked,
// so messages that overtake the battery — the client's shutdown/exit — stay
// buffered in the SAME framing and are processed after it returns).
QJsonValue readNextMessage()
{
    for (;;) {
        QByteArray payload;
        while (g_framing.poll(&payload)) {
            const QJsonObject message
                = QJsonDocument::fromJson(payload).object();
            if (message.value(QStringLiteral("method")).isUndefined())
                return QJsonValue(message); // a response for the battery
            g_recorded.append(message); // stray inbound: record, keep reading
        }
        char buffer[4096];
        const ssize_t n = ::read(0, buffer, sizeof(buffer));
        if (n == 0)
            fail(QStringLiteral("EOF while the battery awaited an answer"));
        if (n < 0 && errno != EINTR)
            fail(QStringLiteral("stdin read failed: %1")
                     .arg(QString::fromUtf8(std::strerror(errno))));
        if (n > 0)
            g_framing.feed(QByteArray(buffer, int(n)));
    }
}

void expectResponseFor(const QJsonObject& stepRequest, const QJsonValue& got,
                       std::function<void(const QJsonObject&)> checks)
{
    if (!got.isObject())
        fail(QStringLiteral("battery step \"%1\" got no response")
                 .arg(stepRequest.value(QStringLiteral("method")).toString()));
    const QJsonObject message = got.toObject();
    if (message.value(QStringLiteral("id"))
        != stepRequest.value(QStringLiteral("id")))
        fail(QStringLiteral("battery answer id mismatch for \"%1\"")
                 .arg(stepRequest.value(QStringLiteral("method")).toString()));
    checks(message);
}

QJsonObject regRequest(qint64 id, const QString& methodName)
{
    QJsonObject registration;
    registration.insert(QStringLiteral("id"), QStringLiteral("r%1").arg(id));
    registration.insert(QStringLiteral("method"), methodName);
    QJsonArray registrations;
    registrations.append(registration);
    QJsonObject m;
    m.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("method"), QStringLiteral("client/registerCapability"));
    m.insert(QStringLiteral("params"),
             QJsonObject{{QStringLiteral("registrations"), registrations}});
    return m;
}

void runBattery()
{
    std::vector<Step> steps;

    steps.push_back(
        {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), 9001},
                     {QStringLiteral("method"),
                      QStringLiteral("window/showMessageRequest")},
                     {QStringLiteral("params"),
                      QJsonObject{{QStringLiteral("type"), 2},
                                  {QStringLiteral("message"),
                                   QStringLiteral("battery message")},
                                  {QStringLiteral("actions"),
                                   QJsonArray{QJsonObject{
                                       {QStringLiteral("title"),
                                        QStringLiteral("Fix it")}}}}}}},
         [](const QJsonObject& m) {
             if (!m.contains(QStringLiteral("result"))
                 || !m.value(QStringLiteral("result")).isNull())
                 fail(QStringLiteral("showMessageRequest answer is not null"));
         }});

    steps.push_back(
        {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), 9002},
                     {QStringLiteral("method"),
                      QStringLiteral("workspace/configuration")},
                     {QStringLiteral("params"),
                      QJsonObject{{QStringLiteral("items"),
                                   QJsonArray{QJsonObject{
                                       {QStringLiteral("section"),
                                        QStringLiteral("a")}},
                                   QJsonObject{{QStringLiteral("section"),
                                                QStringLiteral("b")}}}}}}},
         [](const QJsonObject& m) {
             const QJsonArray answers
                 = m.value(QStringLiteral("result")).toArray();
             if (answers.size() != 2 || !answers.at(0).isNull()
                 || !answers.at(1).isNull())
                 fail(QStringLiteral("configuration answer is not [null,null]"));
         }});

    steps.push_back(
        {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), 9003},
                     {QStringLiteral("method"),
                      QStringLiteral("workspace/workspaceFolders")}},
         [](const QJsonObject& m) {
             if (!m.contains(QStringLiteral("result"))
                 || !m.value(QStringLiteral("result")).isNull())
                 fail(QStringLiteral("workspaceFolders answer is not null"));
         }});

    steps.push_back(
        {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), 9004},
                     {QStringLiteral("method"),
                      QStringLiteral("workDoneProgress/create")},
                     {QStringLiteral("params"),
                      QJsonObject{{QStringLiteral("token"),
                                   QStringLiteral("work-1")}}}},
         [](const QJsonObject& m) {
             if (!m.contains(QStringLiteral("result"))
                 || !m.value(QStringLiteral("result")).isNull())
                 fail(QStringLiteral("workDoneProgress/create answer is not null"));
         }});

    // all-or-nothing: the invalid entry must veto the valid one in the SAME
    // request (the client answering -32803 while also registering hover
    // would be the half-applied state lie).
    {
        QJsonObject hover;
        hover.insert(QStringLiteral("method"), QStringLiteral("textDocument/hover"));
        QJsonObject bogus;
        bogus.insert(QStringLiteral("method"),
                     QStringLiteral("textDocument/magic"));
        QJsonArray both;
        both.append(hover);
        both.append(bogus);
        steps.push_back(
            {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 9005},
                         {QStringLiteral("method"),
                          QStringLiteral("client/registerCapability")},
                         {QStringLiteral("params"),
                          QJsonObject{{QStringLiteral("registrations"), both}}}},
             [](const QJsonObject& m) {
                 if (m.contains(QStringLiteral("result")))
                     fail(QStringLiteral("invalid registration was accepted"));
                 const int code = m.value(QStringLiteral("error"))
                                      .toObject()
                                      .value(QStringLiteral("code"))
                                      .toInt();
                 if (code != -32803)
                     fail(QStringLiteral("invalid registration answered %1, "
                                          "not -32803")
                              .arg(code));
             }});
    }

    // The registry trio: register hover (null + the client flips the gate),
    // unregister it (null even under an unknown id: idempotent no-op), and
    // the idempotency round (unregistering nothing still answers null).
    steps.push_back(
        {regRequest(9006, QStringLiteral("textDocument/hover")),
         [](const QJsonObject& m) {
             if (!m.contains(QStringLiteral("result"))
                 || !m.value(QStringLiteral("result")).isNull())
                 fail(QStringLiteral("register hover answer is not null"));
         }});
    {
        QJsonObject unreg;
        unreg.insert(QStringLiteral("id"), QStringLiteral("no-such-id"));
        unreg.insert(QStringLiteral("method"), QStringLiteral("textDocument/hover"));
        QJsonArray unregisterations;
        unregisterations.append(unreg);
        // THE SPEC'S KEY is "unregisterations" — the frame once spelled
        // "registrations", which matched the CLIENT'S bug of the same name
        // (car C (C5)'s dynamic pull leg fixed the reader and this frame
        // followed the spec, not the bug).
        steps.push_back(
            {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 9007},
                         {QStringLiteral("method"),
                          QStringLiteral("client/unregisterCapability")},
                         {QStringLiteral("params"),
                          QJsonObject{{QStringLiteral("unregisterations"),
                                       unregisterations}}}},
             [](const QJsonObject& m) {
                 if (!m.contains(QStringLiteral("result"))
                     || !m.value(QStringLiteral("result")).isNull())
                     fail(QStringLiteral("unregister answer is not null"));
             }});
    }

    // The unknown client METHOD gets the mechanical -32601 (M1's mechanism
    // re-proved through the LspClient session).
    steps.push_back(
        {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), 9008},
                     {QStringLiteral("method"), QStringLiteral("client/nonsense")}},
         [](const QJsonObject& m) {
             const int code = m.value(QStringLiteral("error"))
                                  .toObject()
                                  .value(QStringLiteral("code"))
                                  .toInt();
             if (code != -32601)
                 fail(QStringLiteral("unknown client method answered %1, "
                                      "not -32601")
                          .arg(code));
         }});

    // applyEdit: NOT answered by the client itself — either the tst answers
    // via respondApplyEdit (noapply mode) or a self-answer/stream break is
    // the violation.
    if (g_noApply) {
        steps.push_back(
            {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 9009},
                         {QStringLiteral("method"),
                          QStringLiteral("workspace/applyEdit")},
                         {QStringLiteral("params"),
                          QJsonObject{{QStringLiteral("edit"),
                                       QJsonObject{{QStringLiteral("label"),
                                                    QStringLiteral("battery edit")}}}}}},
             [](const QJsonObject& m) {
                 if (m.value(QStringLiteral("result"))
                         .toObject()
                         .value(QStringLiteral("applied"))
                         .toBool()
                     != true)
                     fail(QStringLiteral("applyEdit door answer is not "
                                          "{applied:true}"));
             }});
    } else {
        notify(QStringLiteral("workspace/applyEdit"),
               QJsonObject{{QStringLiteral("label"), QStringLiteral("x")}});
        steps.push_back(
            {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 9009},
                         {QStringLiteral("method"), QStringLiteral("client/streamprobe")}},
             [](const QJsonObject& m) {
                 if (m.value(QStringLiteral("error"))
                         .toObject()
                         .value(QStringLiteral("code"))
                         .toInt()
                     != -32601)
                     fail(QStringLiteral("stream broke after applyEdit"));
             }});
    }

    // An unknown NOTIFICATION carries no id to answer against, and RpcClient
    // never answers notifications (machine 1 pins the silence). This step
    // proves the stream stayed ALIVE across it: the final probe must still
    // get the mechanical -32601, and any stray client response that had been
    // emitted mid-battery would land in readNextMessage as an id mismatch
    // (an honest exit-2 fail) before this point could pass.
    notify(QStringLiteral("some/unknownNotification"),
           QJsonValue(QJsonValue::Undefined));
    steps.push_back(
        {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), 9010},
                     {QStringLiteral("method"), QStringLiteral("client/finalprobe")}},
         [](const QJsonObject& m) {
             if (m.value(QStringLiteral("error"))
                     .toObject()
                     .value(QStringLiteral("code"))
                     .toInt()
                 != -32601)
                 fail(QStringLiteral("final probe did not get -32601"));
         }});

    for (const Step& step : steps) {
        send(step.request);
        expectResponseFor(step.request, readNextMessage(), step.checks);
    }
    g_batteryDone = true;
    std::fprintf(stderr, "BATTERY-PASS\n");
    std::fflush(stderr);
}

// ----------------------------------------------------------- sync-mode text
// The INDEPENDENT oracle model for the machine-3 slots. This decoder is its
// own spec implementation on purpose: sharing the production converter would
// let a client bug and a server bug cancel into a green test. It walks
// code points from the line start, counting the encoding's units, and stops
// BEFORE a code point the requested `character` would split (the same
// surrogate-boundary discipline the spec asks of servers).
int specOffset(const QString& text, int line, int character)
{
    if (line < 0)
        line = 0;
    if (character < 0)
        character = 0;

    int lineStart = 0;
    for (int l = 0; l < line; ++l) {
        if (lineStart >= text.size())
            break;
        while (lineStart < text.size() && text.at(lineStart) != u'\n')
            ++lineStart;
        if (lineStart < text.size())
            ++lineStart;
    }
    int lineEnd = lineStart;
    while (lineEnd < text.size() && text.at(lineEnd) != u'\n')
        ++lineEnd;

    if (g_syncEncoding == QLatin1String("utf-16")) {
        // utf-16 character units ARE raw UTF-16 code units: offset arithmetic
        // (clamped to the line end) is the spec reading, pair or no pair.
        return qMin(lineStart + character, lineEnd);
    }
    const bool utf32 = g_syncEncoding == QLatin1String("utf-32");
    int consumed = 0;
    int p = lineStart;
    while (p < lineEnd) {
        const auto hi = text.at(p).unicode();
        int codePoint = hi;
        int width = 1; // units in THIS encoding
        if (hi >= 0xD800 && hi <= 0xDBFF && p + 1 < lineEnd
            && text.at(p + 1).unicode() >= 0xDC00
            && text.at(p + 1).unicode() <= 0xDFFF) {
            codePoint
                = 0x10000 + ((int(hi) - 0xD800) << 10)
                  + (int(text.at(p + 1).unicode()) - 0xDC00);
            width = utf32 ? 1 : 4; // a non-BMP cp: one utf-32 unit, 4 utf-8 bytes
        } else if (!utf32) {
            width = hi < 0x80 ? 1 : (hi < 0x800 ? 2 : 3);
        }
        if (consumed + width > character)
            break; // the position would split this code point: stop before it
        consumed += width;
        p += (codePoint > 0xFFFF) ? 2 : 1;
    }
    return p;
}

void publishDiagsFor(const QString& uri); // forward: the handlers below call it
void featureDidSave(const QString& uri, ModelDoc& d); // forward (features mode,
                                                      // machine 5's didSave
                                                      // markers)
void featureWillSaveWaitUntil(const QJsonValue& id,
                              const QJsonObject& params); // forward (M8 m6)
void wswuDidSavePin(const ModelDoc& d); // forward (M8 m6 didSave-time pins)

void syncDidOpen(const QJsonValue& paramsValue)
{
    const QJsonObject params = paramsValue.toObject();
    const QJsonObject doc = params.value(QStringLiteral("textDocument")).toObject();
    const QString uri = doc.value(QStringLiteral("uri")).toString();
    if (uri.isEmpty())
        fail(QStringLiteral("didOpen without a uri"));
    if (g_docs.contains(uri))
        fail(QStringLiteral("didOpen while %1 is still open (the syncer must "
                            "didClose before re-opening)")
                 .arg(uri));
    const double version = doc.value(QStringLiteral("version")).toDouble(-1);
    if (version != 0)
        fail(QStringLiteral("didOpen version is %1, not 0").arg(version));
    ModelDoc& d = g_docs[uri];
    d.languageId = doc.value(QStringLiteral("languageId")).toString();
    d.text = doc.value(QStringLiteral("text")).toString();
    d.version = 0;
    d.saves = 0;
    if (g_publishDiags)
        publishDiagsFor(uri);
}

void syncDidChange(const QJsonValue& paramsValue)
{
    const QJsonObject params = paramsValue.toObject();
    const QJsonObject doc = params.value(QStringLiteral("textDocument")).toObject();
    const QString uri = doc.value(QStringLiteral("uri")).toString();
    auto found = g_docs.find(uri);
    if (found == g_docs.end())
        fail(QStringLiteral("didChange for \"%1\" without a live didOpen")
                 .arg(uri));
    ModelDoc& d = found.value();
    const double version = doc.value(QStringLiteral("version")).toDouble(-1);
    if (version <= double(d.version))
        fail(QStringLiteral("didChange version %1 did not increase past %2 "
                            "(the syncer bumps once per flush)")
                 .arg(version).arg(double(d.version)));
    const QJsonArray changes = params.value(QStringLiteral("contentChanges")).toArray();
    if (changes.isEmpty())
        fail(QStringLiteral("didChange with an empty contentChanges"));
    for (const QJsonValue& changeValue : changes) {
        const QJsonObject change = changeValue.toObject();
        const QString text = change.value(QStringLiteral("text")).toString();
        const QJsonValue range = change.value(QStringLiteral("range"));
        if (range.isUndefined()) {
            d.text = text; // range-less change: full replacement (spec)
            continue;
        }
        const QJsonObject start = range.toObject().value(QStringLiteral("start")).toObject();
        const QJsonObject end = range.toObject().value(QStringLiteral("end")).toObject();
        const int startOffset
            = specOffset(d.text, start.value(QStringLiteral("line")).toInt(),
                         start.value(QStringLiteral("character")).toInt());
        const int endOffset
            = specOffset(d.text, end.value(QStringLiteral("line")).toInt(),
                         end.value(QStringLiteral("character")).toInt());
        if (startOffset > endOffset)
            fail(QStringLiteral("range end precedes start at offset %1")
                     .arg(startOffset));
        d.text.replace(startOffset, endOffset - startOffset, text);
    }
    d.version = qint64(version);
    if (!g_applyVerify.isEmpty()) {
        // The server-primed applyEdit's full-sync echo (features mode): the
        // client's endServerEdit tells this server its replay landed exactly
        // as this file's OWN applier replayed it. A desync here is the whole
        // machine-5 write path breaking, so it goes out LOUD.
        const bool matched = d.text == g_applyVerify;
        std::fprintf(stderr, "APPLYCHECK-%s\n", matched ? "OK" : "FAIL");
        std::fflush(stderr);
        if (!matched) {
            std::fprintf(stderr, "  expected %s\n  model    %s\n",
                         qPrintable(g_applyVerify), qPrintable(d.text));
            std::fflush(stderr);
            _exit(2);
        }
        g_applyVerify.clear();
    }
    if (g_publishDiags)
        publishDiagsFor(uri);
}

void syncDidClose(const QJsonValue& paramsValue)
{
    const QString uri = paramsValue.toObject()
                            .value(QStringLiteral("textDocument"))
                            .toObject()
                            .value(QStringLiteral("uri"))
                            .toString();
    if (!g_docs.remove(uri))
        fail(QStringLiteral("didClose for a document that is not open"));
}

void syncDidSave(const QJsonValue& paramsValue)
{
    const QString uri = paramsValue.toObject()
                            .value(QStringLiteral("textDocument"))
                            .toObject()
                            .value(QStringLiteral("uri"))
                            .toString();
    auto found = g_docs.find(uri);
    if (found == g_docs.end())
        fail(QStringLiteral("didSave for a document that is not open"));
    ++found.value().saves;
    if (g_features && !g_completionOnly)
        featureDidSave(uri, found.value()); // the write-path markers stay OFF
                                           // for the completion-only pin
    if (g_wswu)
        wswuDidSavePin(found.value()); // M8 m6: the ordering pins ride the save
}

// M8 machine 7: the hub's stored-configuration feed. This mode's whole point
// is ORDER and PAYLOAD: a config that reached the server before `initialized`
// means the hub raced its own handshake, and a settings object that is not
// the view's buffer means the push mangled it. Every accepted config echoes
// back through window/showMessage (the hub relays it as serverShowMessage),
// so the test sees the exact object that crossed the wire, in order.
void syncDidChangeConfiguration(const QJsonValue& paramsValue)
{
    if (!g_initializedSeen)
        fail(QStringLiteral("workspace/didChangeConfiguration before "
                             "initialized: the configuration must be buffered "
                             "until the handshake is done"));
    ++g_configsSeen;
    if (!paramsValue.isObject())
        fail(QStringLiteral("workspace/didChangeConfiguration without an "
                             "object params (an empty buffer must send "
                             "NOTHING)"));
    const QJsonValue settings
        = paramsValue.toObject().value(QStringLiteral("settings"));
    if (!settings.isObject() || settings.toObject().isEmpty())
        fail(QStringLiteral("workspace/didChangeConfiguration with an empty "
                             "settings object: \"never configured\" must not "
                             "travel as \"no settings\""));
    // QJsonObject iterates keys sorted, so the Compact rendering is a
    // deterministic string: the test compares it byte for byte.
    const QByteArray rendered
        = QJsonDocument(settings.toObject()).toJson(QJsonDocument::Compact);
    notify(QStringLiteral("window/showMessage"),
           QJsonObject{{QStringLiteral("type"), 3},
                       {QStringLiteral("message"),
                        QStringLiteral("CONFIG-%1 %2")
                            .arg(g_configsSeen)
                            .arg(QString::fromUtf8(rendered))}});
}

// The server's OWN encoder (the mirror of specOffset): model-frame
// (line, col in utf-16 units) -> wire {line, character} in the NEGOTIATED
// frame. Independent from the client's PositionConverter on purpose.
struct WirePos {
    int line;
    int character;
};
WirePos wirePosition(const QString& text, int modelLine, int modelCol)
{
    int lineStart = 0;
    for (int l = 0; l < modelLine; ++l) {
        while (lineStart < text.size() && text.at(lineStart) != u'\n')
            ++lineStart;
        if (lineStart < text.size())
            ++lineStart;
    }
    int lineEnd = lineStart;
    while (lineEnd < text.size() && text.at(lineEnd) != u'\n')
        ++lineEnd;
    int col = 0;
    int character = 0;
    int p = lineStart;
    while (p < lineEnd && col < modelCol) {
        const auto hi = text.at(p).unicode();
        int codePoint = hi;
        int units = 1;
        if (hi >= 0xD800 && hi <= 0xDBFF && p + 1 < lineEnd
            && text.at(p + 1).unicode() >= 0xDC00
            && text.at(p + 1).unicode() <= 0xDFFF) {
            codePoint = 0x10000 + ((int(hi) - 0xD800) << 10)
                        + (int(text.at(p + 1).unicode()) - 0xDC00);
            units = 2;
        }
        if (g_syncEncoding == QLatin1String("utf-32"))
            character += 1;
        else if (g_syncEncoding == QLatin1String("utf-8"))
            character += codePoint > 0xFFFF
                             ? 4
                             : (codePoint < 0x80 ? 1
                                                 : (codePoint < 0x800 ? 2 : 3));
        else
            character += units;
        p += units;
        col += units; // the MODEL frame counts utf-16 CODE UNITS, not code
                      // points: with `++col` this encoder silently ate one
                      // character per pair ahead of the client's decoder
                      // (machine 4's utf-8 diags tripwire caught it)
    }
    return WirePos{modelLine, character};
}

// M4: scan the document for marker words and publish them as diagnostics,
// one per severity level the wire can carry (the decoder's whole severity
// table gets drilled, not just Error). Ranges go out in the NEGOTIATED frame
// through the server's OWN encoder — the columns count utf-8/utf-32 units
// when the encoding says so, which is exactly the trap the glue's decoder
// must pass. Empty array = the clear signal.
struct Marker {
    const char* word;
    int severity; // LSP DiagnosticSeverity
};
const Marker kMarkers[] = {
    {"bug", 1},  {"warn", 2},  {"note", 3}, {"hint", 4},
};
// The scan, split from the publish so the codeAction door can compare the
// hub's re-encoded context.diagnostics against this server's own fresh scan
// of the same text (machine 5's frame pin).
QJsonArray diagsFor(const QString& text)
{
    QJsonArray diags;
    int line = 0;
    int lineStart = 0;
    for (int i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text.at(i) == u'\n') {
            const int lineEnd = i;
            for (int j = lineStart; j < lineEnd; ++j) {
                for (const Marker& marker : kMarkers) {
                    const int length
                        = int(qstrlen(marker.word)); // the markers are ASCII
                    if (text.mid(j, length) != QLatin1String(marker.word))
                        continue;
                    const WirePos start
                        = wirePosition(text, line, j - lineStart);
                    const WirePos end = wirePosition(
                        text, line, j - lineStart + length);
                    QJsonObject record{
                        {QStringLiteral("range"),
                         QJsonObject{{QStringLiteral("start"),
                                      QJsonObject{{QStringLiteral("line"),
                                                   start.line},
                                                  {QStringLiteral("character"),
                                                   start.character}}},
                                     {QStringLiteral("end"),
                                      QJsonObject{{QStringLiteral("line"),
                                                   end.line},
                                                  {QStringLiteral("character"),
                                                   end.character}}}}},
                        {QStringLiteral("severity"), marker.severity},
                        {QStringLiteral("message"),
                         QStringLiteral("%1 at line %2")
                             .arg(QLatin1String(marker.word))
                             .arg(line)}};
                    if (g_diagSource)
                        record.insert(
                            QStringLiteral("source"),
                            QStringLiteral("fakesrv")); // the shared-hover
                                                       // sliver's label pin
                    diags.append(record);
                    j += length - 1;
                    break;
                }
            }
            ++line;
            lineStart = i + 1;
        }
    }
    return diags;
}

void publishDiagsFor(const QString& uri)
{
    notify(QStringLiteral("textDocument/publishDiagnostics"),
           QJsonObject{
               {QStringLiteral("uri"), uri},
               {QStringLiteral("diagnostics"),
                diagsFor(g_docs.value(uri).text)}});
}

// The TEST-ONLY door: answers {ok,model,version,saves,open} for one uri
// (M4 multi-doc: the params may carry "uri"; with no uri and exactly one
// open document that one is the subject — machine 3's callers ride it).
// A mismatch is an honest ok:false for the test to assert — the exit-2 path
// stays reserved for structural wire lies (see syncDidChange et al.).
void handleAssertModel(const QJsonValue& id, const QJsonValue& paramsValue)
{
    const QJsonObject params = paramsValue.toObject();
    QString uri = params.value(QStringLiteral("uri")).toString();
    if (uri.isEmpty() && g_docs.size() == 1)
        uri = g_docs.firstKey(); // the single-document reading
    const bool known = g_docs.contains(uri);
    const ModelDoc doc = known ? g_docs.value(uri) : ModelDoc{};
    const bool textMatches
        = params.contains(QStringLiteral("expectedText"))
          && doc.text == params.value(QStringLiteral("expectedText")).toString();
    const bool versionMatches
        = !params.contains(QStringLiteral("expectedVersion"))
          || params.value(QStringLiteral("expectedVersion")).toDouble()
                 == double(doc.version);
    QJsonObject result;
    result.insert(QStringLiteral("ok"), known && textMatches && versionMatches);
    result.insert(QStringLiteral("model"), doc.text);
    result.insert(QStringLiteral("version"), double(doc.version));
    result.insert(QStringLiteral("saves"), doc.saves);
    result.insert(QStringLiteral("open"), known);
    send(response(id, result));
}

// M4: how many documents THIS server currently holds open (the hub's
// multi-doc pins: open/close accounting on the server side of the wire).
void handleDocumentCount(const QJsonValue& id)
{
    send(response(id, QJsonObject{{QStringLiteral("count"),
                                   g_docs.size()}}));
}

// M4: hands the initialize params right back (the hub's matrix/template
// merge and its ${WORKSPACE_ROOT} expansion are only checkable through the
// wire: this echo is what makes those pins exact instead of inferred).
void handleInitializeParams(const QJsonValue& id)
{
    send(response(id, g_initializeParams));
}

// ------------------------------------------------- the model-edit applier
// features mode applies a WorkspaceEdit to its OWN model with the rules the
// client's view door states: decode every range through specOffset (total
// clamping, the surrogate-safe stop), swap an inverted pair, walk DESCENDING,
// replace. An OVERLAP here is a bug in THIS file's own edit builders, so it
// goes out LOUD: a silent skip would let an apply bug hide behind a model
// that matches for the wrong reason.
void applyWireEdits(ModelDoc& d, const QJsonArray& edits)
{
    struct Applied {
        int from;
        int to;
        QString text;
    };
    QVector<Applied> list;
    list.reserve(edits.size());
    for (const QJsonValue& editValue : edits) {
        const QJsonObject edit = editValue.toObject();
        const QJsonObject range = edit.value(QStringLiteral("range")).toObject();
        const QJsonObject start = range.value(QStringLiteral("start")).toObject();
        const QJsonObject end = range.value(QStringLiteral("end")).toObject();
        int from = specOffset(d.text, start.value(QStringLiteral("line")).toInt(),
                              start.value(QStringLiteral("character")).toInt());
        int to = specOffset(d.text, end.value(QStringLiteral("line")).toInt(),
                            end.value(QStringLiteral("character")).toInt());
        if (from > to)
            qSwap(from, to);
        list.append(
            Applied{from, to, edit.value(QStringLiteral("newText")).toString()});
    }
    std::sort(list.begin(), list.end(), [](const Applied& a, const Applied& b) {
        if (a.from != b.from)
            return a.from > b.from;
        return a.to > b.to;
    });
    for (int i = 1; i < list.size(); ++i) {
        if (list.at(i - 1).from < list.at(i).to)
            fail(QStringLiteral("fakeserver's own TextEdit batch overlaps at "
                                "offset %1 (descending walk)")
                     .arg(list.at(i).to));
    }
    for (const Applied& a : list)
        d.text.replace(a.from, a.to - a.from, a.text);
}

// A TextEdit in the NEGOTIATED wire frame, encoded through THIS server's own
// wirePosition from MODEL coordinates: proof both ways (the client must decode
// these back onto the model offsets the test aims at).
// A Range in the NEGOTIATED wire frame, encoded from MODEL coordinates through
// THIS server's own wirePosition.
QJsonObject rangeWire(const QString& text, int fromLine, int fromCol,
                      int toLine, int toCol)
{
    const WirePos from = wirePosition(text, fromLine, fromCol);
    const WirePos to = wirePosition(text, toLine, toCol);
    return QJsonObject{
        {QStringLiteral("start"),
         QJsonObject{{QStringLiteral("line"), from.line},
                     {QStringLiteral("character"), from.character}}},
        {QStringLiteral("end"),
         QJsonObject{{QStringLiteral("line"), to.line},
                     {QStringLiteral("character"), to.character}}}};
}

QJsonObject textEditWire(const QString& text, int fromLine, int fromCol,
                         int toLine, int toCol, const QString& newText)
{
    return QJsonObject{{QStringLiteral("range"),
                        rangeWire(text, fromLine, fromCol, toLine, toCol)},
                       {QStringLiteral("newText"), newText}};
}

// split('\n'), NEVER mid(text,0,pos): the view counts lines by '\n' only, so
// cutting at a '\r' would make this oracle's columns disagree with the view's
// (the same CRLF discipline machine 3's own line walk follows).
QStringList modelLines(const QString& text) { return text.split(u'\n'); }

// The MODEL (line, col-in-utf-16-units) of a model offset.
void rowColAt(const QStringList& lines, int offset, int* line, int* col)
{
    int seen = 0;
    for (int i = 0; i < lines.size(); ++i) {
        const int span = lines.at(i).size() + 1; // the '\n' rides the span
        if (offset < seen + span || i == lines.size() - 1) {
            *line = i;
            *col = offset - seen;
            return;
        }
        seen += span;
    }
}

// The identifier run around `offset` ([A-Za-z0-9_], the run the client's
// rename door assumes too). A caret just PAST a word's end still means that
// word (the caret sits between units). Empty = no word at that caret.
QString wordAt(const QString& text, int offset, int* startOut, int* endOut)
{
    *startOut = *endOut = -1;
    auto isWord = [](QChar c) { return c == u'_' || c.isLetterOrNumber(); };
    int probe = offset;
    if (probe >= text.size())
        probe = text.size() - 1;
    if (probe < 0 || !isWord(text.at(probe)))
        --probe;
    if (probe < 0 || probe >= text.size() || !isWord(text.at(probe)))
        return QString();
    int start = probe;
    while (start > 0 && isWord(text.at(start - 1)))
        --start;
    int end = probe + 1;
    while (end < text.size() && isWord(text.at(end)))
        ++end;
    *startOut = start;
    *endOut = end;
    return text.mid(start, end - start);
}

// Non-overlapping occurrences of `word` (model offsets, ascending).
QVector<int> occurrences(const QString& text, const QString& word)
{
    QVector<int> out;
    int from = 0;
    for (;;) {
        const int at = text.indexOf(word, from);
        if (at < 0)
            return out;
        out.append(at);
        from = at + qMax(1, word.size());
    }
}

// ------------------------------------------------- feature providers (M5)
// Every reply is built from THIS server's own decode (specOffset) and encode
// (wirePosition) paths — the sync-mode oracle rule carried into the feature
// frames: a client encoder bug lands the reply off-target or trips one of the
// param-shape fail()s below, it never cancels out into a green test. The
// payloads are the SPEC shapes (Location[], CompletionList, CodeAction[],
// TextEdit[], WorkspaceEdit) kept deliberately minimal: what machine 5 pins is
// the FRAME, and machine 6's popups decode the markup for real.
ModelDoc& featureDoc(const QJsonObject& params, const QString& methodName)
{
    const QString uri = params.value(QStringLiteral("textDocument"))
                            .toObject()
                            .value(QStringLiteral("uri"))
                            .toString();
    auto found = g_docs.find(uri);
    if (found == g_docs.end())
        fail(QStringLiteral("%1 names a document this server does not hold "
                            "open: \"%2\"")
                 .arg(methodName, uri));
    return found.value();
}

// The caret a feature request aims at, decoded in THIS server's frame.
int featureOffset(const ModelDoc& d, const QJsonObject& params,
                  const QString& methodName)
{
    const QJsonObject position = params.value(QStringLiteral("position")).toObject();
    if (!position.value(QStringLiteral("line")).isDouble()
        || !position.value(QStringLiteral("character")).isDouble())
        fail(QStringLiteral("%1 carries no numeric Position").arg(methodName));
    return specOffset(d.text, position.value(QStringLiteral("line")).toInt(),
                      position.value(QStringLiteral("character")).toInt());
}

void featureDefinition(const QJsonValue& id, const QJsonObject& params)
{
    ModelDoc& d = featureDoc(params, QStringLiteral("textDocument/definition"));
    const int offset = featureOffset(d, params, QStringLiteral("textDocument/definition"));
    int wordStart = -1;
    int wordEnd = -1;
    const QString word = wordAt(d.text, offset, &wordStart, &wordEnd);
    QJsonArray locations; // Location[] (spec): the word the caret landed ON
    if (!word.isEmpty()) {
        const QStringList lines = modelLines(d.text);
        int line = 0;
        int col = 0;
        rowColAt(lines, wordStart, &line, &col);
        locations.append(QJsonObject{
            {QStringLiteral("uri"),
             params.value(QStringLiteral("textDocument"))
                 .toObject()
                 .value(QStringLiteral("uri"))
                 .toString()},
            {QStringLiteral("range"),
             rangeWire(d.text, line, col, line, col + word.size())}});
    }
    send(response(id, locations));
}

void featureReferences(const QJsonValue& id, const QJsonObject& params)
{
    ModelDoc& d = featureDoc(params, QStringLiteral("textDocument/references"));
    const int offset = featureOffset(d, params, QStringLiteral("textDocument/references"));
    const QJsonObject context = params.value(QStringLiteral("context")).toObject();
    if (!context.contains(QStringLiteral("includeDeclaration")))
        fail(QStringLiteral("textDocument/references context has no "
                            "\"includeDeclaration\""));
    const bool includeDeclaration
        = context.value(QStringLiteral("includeDeclaration")).toBool();
    int wordStart = -1;
    int wordEnd = -1;
    const QString word = wordAt(d.text, offset, &wordStart, &wordEnd);
    const QString uri = params.value(QStringLiteral("textDocument"))
                            .toObject()
                            .value(QStringLiteral("uri"))
                            .toString();
    const QStringList lines = modelLines(d.text);
    const QVector<int> hits = occurrences(d.text, word);
    QJsonArray locations;
    for (int i = 0; i < hits.size(); ++i) {
        if (!includeDeclaration && i == 0)
            continue; // THIS fake's declaration is its first occurrence: the
                      // flag's whole point is that it drops exactly that one
        int line = 0;
        int col = 0;
        rowColAt(lines, hits.at(i), &line, &col);
        locations.append(QJsonObject{
            {QStringLiteral("uri"), uri},
            {QStringLiteral("range"),
             rangeWire(d.text, line, col, line, col + word.size())}});
    }
    send(response(id, locations));
}

void featureCompletion(const QJsonValue& id, const QJsonObject& params)
{
    ModelDoc& d = featureDoc(params, QStringLiteral("textDocument/completion"));
    const int offset = featureOffset(d, params, QStringLiteral("textDocument/completion"));
    const QJsonObject context = params.value(QStringLiteral("context")).toObject();
    const QJsonValue kindValue = context.value(QStringLiteral("triggerKind"));
    if (!kindValue.isDouble())
        fail(QStringLiteral("textDocument/completion context has no "
                            "\"triggerKind\""));
    const int triggerKind = kindValue.toInt();
    const QString triggerCharacter
        = context.value(QStringLiteral("triggerCharacter")).toString();
    if (triggerKind != 1 && triggerKind != 2)
        fail(QStringLiteral("completion triggerKind %1 is neither Invoked (1) "
                            "nor TriggerForCharacter (2)")
                 .arg(triggerKind));
    if (triggerKind == 1 && !triggerCharacter.isEmpty())
        fail(QStringLiteral("completion triggerKind 1 carried a "
                            "\"triggerCharacter\" (\"%1\")")
                 .arg(triggerCharacter));
    if (triggerKind == 2 && triggerCharacter.isEmpty())
        fail(QStringLiteral("completion triggerKind 2 without a "
                            "\"triggerCharacter\""));
    if (triggerKind == 2 && !g_completionTriggers.contains(triggerCharacter))
        fail(QStringLiteral("completion triggered by \"%1\": the advertised "
                            "triggerCharacters set is what the hub must "
                            "classify against")
                 .arg(triggerCharacter));
    // The proof rides LEGAL fields: labels this test pins verbatim, so the
    // frame the hub built is visible in the reply itself.
    // The ACCEPT sliver (post-1.8.0) rides the SAME items with payloads:
    // "alpha" gets a plain {range,newText} textEdit — zero-width at the
    // REQUEST position; the tk= item carries the insert/replace alternative
    // shape (clangd's favorite) with the replace range deliberately
    // ONE UNIT LEFT of the insert one, so a test can tell which range the
    // hub picked (insert wins); the off= item has NO textEdit but an
    // insertText — the hub must synthesize the insertion at the fire-time
    // caret itself.
    const QJsonObject position = params.value(QStringLiteral("position")).toObject();
    auto posObj = [&position](int colShift) {
        int col = position.value(QStringLiteral("character")).toInt() + colShift;
        if (col < 0)
            col = 0;
        return QJsonObject{{QStringLiteral("line"),
                            position.value(QStringLiteral("line")).toDouble()},
                           {QStringLiteral("character"), col}};
    };
    const QJsonObject pointEdit{
        {QStringLiteral("start"), posObj(0)}, {QStringLiteral("end"), posObj(0)}};
    QJsonArray items;
    items.append(QJsonObject{
        {QStringLiteral("label"), QStringLiteral("alpha")},
        {QStringLiteral("kind"), 14}, // Variable: the kind table rides along
        {QStringLiteral("data"),
         QJsonObject{{QStringLiteral("origin"), QStringLiteral("list")}}},
        {QStringLiteral("textEdit"),
         QJsonObject{{QStringLiteral("range"), pointEdit},
                     {QStringLiteral("newText"), QStringLiteral("ALPHA")}}}});
    items.append(QJsonObject{{QStringLiteral("label"), QStringLiteral("tk=")
                                                               + QString::number(triggerKind)},
                             {QStringLiteral("textEdit"),
                              QJsonObject{{QStringLiteral("newText"),
                                           QStringLiteral("TKTXT")},
                                          {QStringLiteral("insert"), pointEdit},
                                          {QStringLiteral("replace"),
                                           QJsonObject{{QStringLiteral("start"),
                                                       posObj(-1)},
                                                      {QStringLiteral("end"),
                                                       posObj(0)}}}}}});
    if (triggerKind == 2)
        items.append(QJsonObject{{QStringLiteral("label"),
                                  QStringLiteral("tc=") + triggerCharacter}});
    items.append(QJsonObject{{QStringLiteral("label"),
                              QStringLiteral("off=") + QString::number(offset)},
                             {QStringLiteral("insertText"),
                              QStringLiteral("OFFTXT")}});
    const QJsonObject result{
        {QStringLiteral("isIncomplete"), false},
        {QStringLiteral("items"), items}};
    if (g_completionDelayMs > 0) {
        // Latency ON PURPOSE (popup triggers): the request stays in flight
        // long enough for the test to supersede it and watch the hub's
        // $/cancelRequest.
        QTimer::singleShot(g_completionDelayMs, [id, result] {
            send(response(id, result));
        });
    } else {
        send(response(id, result));
    }
}

// GATE 3 car C (C2): the signatureHelp answer — the SPEC shape kept minimal
// but deliberately THREE-signature, so picking signatures[0] when the server
// says activeSignature:1 shows up as the wrong popup text. The two labels
// this test asserts verbatim; activeParameter rides along unread (the
// plain-text popup has no underline — PLAN's recorded divergence).
void featureSignatureHelp(const QJsonValue& id, const QJsonObject& params)
{
    ModelDoc& d = featureDoc(params, QStringLiteral("textDocument/signatureHelp"));
    const int offset = featureOffset(d, params,
                                     QStringLiteral("textDocument/signatureHelp"));
    // contextSupport is advertised TRUE and the hub's door ALWAYS builds a
    // context (spec: omitting it is only legal when the server never
    // declares support) — a request without one is an encoder bug.
    if (!params.contains(QStringLiteral("context")))
        fail(QStringLiteral("textDocument/signatureHelp without a \"context\" "
                            "object: the hub advertises contextSupport"));
    const QJsonObject context = params.value(QStringLiteral("context")).toObject();
    const QJsonValue kindValue = context.value(QStringLiteral("triggerKind"));
    if (!kindValue.isDouble())
        fail(QStringLiteral("textDocument/signatureHelp context has no "
                            "\"triggerKind\""));
    const int triggerKind = kindValue.toInt();
    const QString triggerCharacter
        = context.value(QStringLiteral("triggerCharacter")).toString();
    if (triggerKind != 1 && triggerKind != 2)
        fail(QStringLiteral("signatureHelp triggerKind %1 is neither Invoked "
                            "(1) nor TriggerForCharacter (2)")
                 .arg(triggerKind));
    if (triggerKind == 1 && !triggerCharacter.isEmpty())
        fail(QStringLiteral("signatureHelp triggerKind 1 carried a "
                            "\"triggerCharacter\" (\"%1\")")
                 .arg(triggerCharacter));
    if (triggerKind == 2 && triggerCharacter.isEmpty())
        fail(QStringLiteral("signatureHelp triggerKind 2 without a "
                            "\"triggerCharacter\""));
    if (triggerKind == 2 && triggerCharacter != QLatin1String("(")
            && triggerCharacter != QLatin1String(","))
        fail(QStringLiteral("signatureHelp triggered by \"%1\": the "
                            "advertised triggerCharacters set is [\"(\", "
                            "\",\"]").arg(triggerCharacter));
    // A caret INSIDE brackets is the only place this server has a signature
    // for: the "(" run the caret sits in names the call. OUTSIDE one, the
    // spec's null answer rides (the hub must hide the popup).
    QString sigName;
    for (int i = offset - 1; i >= 0; --i) {
        const QChar c = d.text.at(i);
        if (c == QLatin1Char(')'))
            break;
        if (c == QLatin1Char('(')) {
            int start = i;
            while (start > 0 && (d.text.at(start - 1).isLetterOrNumber()
                                 || d.text.at(start - 1) == QLatin1Char('_')))
                --start;
            sigName = d.text.mid(start, i - start);
            break;
        }
    }
    ++g_sigAsks;
    g_sigLastKind = triggerKind;
    g_sigLastChar = triggerCharacter;
    g_sigLastOffset = offset;
    if (sigName.isEmpty()) {
        if (g_sigDelayMs > 0) {
            QTimer::singleShot(g_sigDelayMs, [id] {
                send(response(id, QJsonValue(QJsonValue::Null)));
            });
        } else {
            send(response(id, QJsonValue(QJsonValue::Null)));
        }
        return;
    }
    const QJsonObject report{
        {QStringLiteral("signatures"),
         QJsonArray{
             QJsonObject{{QStringLiteral("label"),
                          sigName + QStringLiteral("(first)")},
                         {QStringLiteral("parameters"),
                          QJsonArray{QJsonObject{
                              {QStringLiteral("label"),
                               QStringLiteral("first")}}}}},
             QJsonObject{{QStringLiteral("label"),
                          sigName + QStringLiteral("(second one, more)")},
                         {QStringLiteral("documentation"),
                          QStringLiteral("the ACTIVE one — the popup must "
                                         "carry this label")}},
             QJsonObject{{QStringLiteral("label"),
                          sigName + QStringLiteral("(third)")}}}},
        {QStringLiteral("activeSignature"), 1},
        {QStringLiteral("activeParameter"), 1}};
    if (g_sigDelayMs > 0) {
        // The supersede hold: the reply is NOT in yet, so the hub has a real
        // in-flight request to $/cancelRequest.
        QTimer::singleShot(g_sigDelayMs, [id, report] {
            send(response(id, report));
        });
    } else {
        send(response(id, report));
    }
}

// The hub's own ask, counted and echoed by the SERVER: the wire-shape proof
// (which triggerKind, which character, which offset) rides fields this
// server decoded from its OWN frame — the hub can only paint what it asked.
void handleSignatureState(const QJsonValue& id)
{
    send(response(id, QJsonObject{
        {QStringLiteral("asks"), g_sigAsks},
        {QStringLiteral("lastKind"), g_sigLastKind},
        {QStringLiteral("lastCharacter"), g_sigLastChar},
        {QStringLiteral("lastOffset"), g_sigLastOffset}}));
}

void handleHintState(const QJsonValue& id)
{
    send(response(id, QJsonObject{
        {QStringLiteral("asks"), g_hintAsks},
        {QStringLiteral("lastEndLine"), g_hintLastEndLine},
        {QStringLiteral("lastEndChar"), g_hintLastEndChar}}));
}

// ------------------------------------------------------------- the folds
// GATE 3 car C (C3): the fold oracle. The ranges come from THIS server's own
// text MIRROR — a brace-depth scan (a line ending in `{` opens, a line whose
// first non-space character is `}` closes it: the shape every fold fixture
// here is written in) — so an EDIT to the document changes the ANSWER, and the
// hub's debounced re-feed is checked end to end instead of against a table the
// test could have hard-coded.
//
// The deliberate NOISE at the end rides every report, and it exists to make
// the client's decode discipline observable: line numbers the document never
// had (the client must CLAMP them), an inverted range and an empty one (both
// must be DROPPED: a fold that hides nothing is not a fold), a duplicate start
// WIDER than the real one (the widest wins — the tie rule of the view door the
// hub feeds), and two malformed entries (a bare number, and an object with no
// `endLine`: both must be skipped without breaking the others). Every entry
// carries a `kind` the line-only client must decode NOWHERE.
QJsonArray foldReport(const ModelDoc& d)
{
    const QStringList lines = d.text.split(QLatin1Char('\n'));
    QJsonArray out;
    QVector<int> openLines; // line numbers of unclosed `{` lines, innermost LAST
    for (int i = 0; i < lines.size(); ++i) {
        const QString trimmed = lines.at(i).trimmed();
        if (trimmed.endsWith(QLatin1Char('{'))) {
            openLines.append(i);
        } else if (trimmed.startsWith(QLatin1Char('}')) && !openLines.isEmpty()) {
            out.append(QJsonObject{{QStringLiteral("startLine"),
                                    openLines.takeLast()},
                                   {QStringLiteral("endLine"), i},
                                   {QStringLiteral("kind"),
                                    QStringLiteral("region")}});
        }
    }
    out.append(QJsonObject{{QStringLiteral("startLine"), -3},
                           {QStringLiteral("endLine"), 1},
                           {QStringLiteral("kind"), QStringLiteral("region")}});
    out.append(QJsonObject{{QStringLiteral("startLine"), 2},
                           {QStringLiteral("endLine"), 40},
                           {QStringLiteral("kind"), QStringLiteral("import")}});
    out.append(QJsonObject{{QStringLiteral("startLine"), 7},
                           {QStringLiteral("endLine"), 99},
                           {QStringLiteral("kind"), QStringLiteral("commentblock")}});
    out.append(QJsonObject{{QStringLiteral("startLine"), 5},
                           {QStringLiteral("endLine"), 3}});
    out.append(QJsonObject{{QStringLiteral("startLine"), 4},
                           {QStringLiteral("endLine"), 4}});
    out.append(QJsonObject{{QStringLiteral("startLine"), 8},
                           {QStringLiteral("endLine"), 100}});
    out.append(QJsonValue(42)); // a bare number in the array
    out.append(QJsonObject{{QStringLiteral("startLine"), 3}}); // no endLine
    return out;
}

void featureFoldingRange(const QJsonValue& id, const QJsonObject& params)
{
    // featureDoc IS the second half of the flush-before-ask pin: a fold ask
    // that arrived for a document the server never saw OPEN exits 2 here.
    ModelDoc& d = featureDoc(params, QStringLiteral("textDocument/foldingRange"));
    // FoldingRangeParams is the BARE `{textDocument}`. A foldingRangeContext
    // describes folding FINER than lines — this client declared it folds lines
    // only, so a context on the wire is a capability lie in the other
    // direction (and the columns this server's report leaves out would be a
    // second one).
    if (params.contains(QStringLiteral("context")))
        fail(QStringLiteral("textDocument/foldingRange carried a \"context\" "
                            "object: the hub declared lineFoldingOnly"));
    ++g_foldAsks;
    if (g_foldEmpty) {
        send(response(id, QJsonArray{})); // the authority: nothing is foldable
        return;
    }
    if (g_foldFailAfter >= 0 && g_foldAsks > g_foldFailAfter) {
        answerErrorTo(id, -32601,
                      QStringLiteral("foldingRange refused after being "
                                     "advertised (foldfail)"));
        return;
    }
    const QJsonArray report = foldReport(d);
    if (g_foldDelayMs > 0) {
        QTimer::singleShot(g_foldDelayMs,
                           [id, report] { send(response(id, report)); });
    } else {
        send(response(id, report));
    }
}

// ------------------------------------------------------------- the hints
// GATE 3 car C (C4): the hint oracle, over THIS server's own text MIRROR —
// the same discipline that makes the fold answer track edits: an inlayHint
// report's POSITIONS speak the synced frame, so the flush-before-ask rule is
// checked end to end (and far-side: the LAST ask's range end echoes through
// test/hintState, an end naming the CURRENT mirror proves the didChange
// bytes LED the ask). The anchor tokens make the answer line up with the
// shared fixture; the fixed NOISE at the end exists to make the client's
// decode discipline observable — an out-of-document position (must CLAMP
// tolerantly the way a generous position reads), parts + both paddings (the
// no-labelSupport client concatenates and bakes the paddings as real
// spaces), an empty label and a position-less and a bare-number entry (all
// DROPPED without breaking the others) — and every sane entry carries a
// `kind` this line/col-only paint client must decode NOWHERE.
QJsonArray hintReport(const ModelDoc& d)
{
    const QStringList lines = d.text.split(QLatin1Char('\n'));
    QJsonArray out;
    for (int i = 0; i < lines.size(); ++i) {
        const QString& text2 = lines.at(i);
        QString tag; // the ANCHOR tokens: the answer follows the mirror
        if (text2.contains(QStringLiteral("alpha")))
            tag = QStringLiteral("A");
        else if (text2.contains(QStringLiteral("beta")))
            tag = QStringLiteral("B");
        else if (text2.contains(QStringLiteral("gamma")))
            tag = QStringLiteral("C");
        if (tag.isEmpty())
            continue;
        out.append(QJsonObject{
            {QStringLiteral("position"),
             QJsonObject{{QStringLiteral("line"), i},
                         {QStringLiteral("character"), text2.length()}}},
            {QStringLiteral("label"), QStringLiteral("RET") + tag},
            {QStringLiteral("kind"), QStringLiteral("type")}});
    }
    // A PARTS label with BOTH paddings: the client claims no labelSupport,
    // so the parts shape is what it gets — and the hub concatenates the
    // values and bakes each padding as exactly one space into the painted
    // text (the recorded M5-record divergence).
    out.append(QJsonObject{
        {QStringLiteral("position"),
         QJsonObject{{QStringLiteral("line"), 0},
                     {QStringLiteral("character"), 0}}},
        {QStringLiteral("label"),
         QJsonArray{QJsonObject{{QStringLiteral("value"),
                                 QStringLiteral("<")}},
                    QJsonObject{{QStringLiteral("value"),
                                 QStringLiteral("h")}},
                    QJsonObject{{QStringLiteral("value"),
                                 QStringLiteral(">")}}}},
        {QStringLiteral("paddingLeft"), true},
        {QStringLiteral("paddingRight"), true},
        {QStringLiteral("kind"), QStringLiteral("parameter")}});
    out.append(QJsonObject{ // a plain label, right-padded
        {QStringLiteral("position"),
         QJsonObject{{QStringLiteral("line"), 0},
                     {QStringLiteral("character"), 11}}},
        {QStringLiteral("label"), QStringLiteral("PAD")},
        {QStringLiteral("paddingRight"), true},
        {QStringLiteral("kind"), QStringLiteral("type")}});
    out.append(QJsonObject{ // OUT OF THE DOCUMENT: the converter reads a
                            // generous position the tolerant way (the LAST
                            // line of the mirror frame), never a fault
        {QStringLiteral("position"),
         QJsonObject{{QStringLiteral("line"), 400},
                     {QStringLiteral("character"), 0}}},
        {QStringLiteral("label"), QStringLiteral("OUT")},
        {QStringLiteral("kind"), QStringLiteral("parameter")}});
    out.append(QJsonObject{ // an empty label: nothing to paint, DROPPED
        {QStringLiteral("position"),
         QJsonObject{{QStringLiteral("line"), 1},
                     {QStringLiteral("character"), 0}}},
        {QStringLiteral("label"), QStringLiteral("")}});
    out.append(QJsonObject{ // no anchor at all: malformed, DROPPED
        {QStringLiteral("label"), QStringLiteral("NOPOS")}});
    out.append(QJsonValue(42)); // a bare number in the array: DROPPED
    return out;
}

void featureInlayHint(const QJsonValue& id, const QJsonObject& params)
{
    // featureDoc IS the flush-before-ask far-side pin again: an ask naming a
    // document this server never saw OPEN exits 2 here.
    ModelDoc& d = featureDoc(params, QStringLiteral("textDocument/inlayHint"));
    // THE v1 ASK IS WHOLE-DOCUMENT: no range is a hub bug (the client
    // declared an inlayHint capability precisely so providers answer the
    // document); a range not STARTING at {0,0} is not the document. The END
    // is not asserted here — its honesty is the test's, read back through
    // test/hintState (the mirror it names IS the flush proof).
    if (!params.contains(QStringLiteral("range")))
        fail(QStringLiteral("textDocument/inlayHint arrived without a "
                            "\"range\": this hub's v1 feed always asks the "
                            "whole document"));
    const QJsonObject start
        = params.value(QStringLiteral("range"))
              .toObject()
              .value(QStringLiteral("start"))
              .toObject();
    if (start.value(QStringLiteral("line")).toInt(-1) != 0
        || start.value(QStringLiteral("character")).toInt(-1) != 0)
        fail(QStringLiteral("the whole-document hint ask does not start at "
                            "{0,0}: range is not the document"));
    g_hintLastEndLine = params.value(QStringLiteral("range"))
                            .toObject()
                            .value(QStringLiteral("end"))
                            .toObject()
                            .value(QStringLiteral("line"))
                            .toInt(-1);
    g_hintLastEndChar = params.value(QStringLiteral("range"))
                            .toObject()
                            .value(QStringLiteral("end"))
                            .toObject()
                            .value(QStringLiteral("character"))
                            .toInt(-1);
    ++g_hintAsks;
    if (g_hintNull) {
        send(response(id, QJsonValue(QJsonValue::Null))); // spec "no hints":
                                                          // a whole-set
                                                          // authority — the
                                                          // hub paints EMPTY
        return;
    }
    if (g_hintFailAfter >= 0 && g_hintAsks > g_hintFailAfter) {
        answerErrorTo(id, -32601, QStringLiteral("inlayHint refused after "
                                                 "being advertised (hintfail)"));
        return;
    }
    QJsonArray report;
    if (g_hintMany > 0) {
        for (int i = 0; i < g_hintMany; ++i)
            report.append(QJsonObject{
                {QStringLiteral("position"),
                 QJsonObject{{QStringLiteral("line"), 0},
                             {QStringLiteral("character"), 0}}},
                {QStringLiteral("label"), QStringLiteral("h%1").arg(i)}});
    } else {
        report = hintReport(d);
    }
    if (g_hintDelayMs > 0) {
        QTimer::singleShot(g_hintDelayMs,
                           [id, report] { send(response(id, report)); });
    } else {
        send(response(id, report));
    }
}

// ------------------------------------------------------- the pull reports
// GATE 3 car C (C5): the PULL oracle. Like the fold/hint oracles the report
// is scanned from THIS server's own text mirror — the token is "err" (the
// PUSH oracle's markers are bug/warn/note/hint, and the fixtures keep the
// two token families DISJOINT: while the pull is live a "bug" token must
// NEVER paint, and once the pull falls through the contradicting publish's
// "bug" entry paints — the two authorities told apart by their paint alone).
// Deliberate wire noise again: a report entry whose range is INVERTED (the
// decoder swaps it the tolerant way), a generous end line far past the
// document (clamps to the mirror's last line), an out-of-range severity 9
// (reads Error), and an entry with NO severity (defaults to Error too) — so
// the ONE shared decoder (push items vs pull items) is pinned by the same
// exact-set compare the push slots already give their side.
QJsonArray pullReportItems(const ModelDoc& d)
{
    const QStringList lines = d.text.split(QLatin1Char('\n'));
    QJsonArray out;
    int marked = 0;
    for (int i = 0; i < lines.size(); ++i) {
        const int at = lines.at(i).indexOf(QStringLiteral("err"));
        if (at < 0)
            continue;
        ++marked;
        out.append(QJsonObject{ // the honest one: severity 1, exact range
            {QStringLiteral("range"),
             QJsonObject{{QStringLiteral("start"),
                          QJsonObject{{QStringLiteral("line"), i},
                                      {QStringLiteral("character"), at}}},
                         {QStringLiteral("end"),
                          QJsonObject{{QStringLiteral("line"), i},
                                      {QStringLiteral("character"),
                                       at + 3}}}}},
            {QStringLiteral("severity"), 1},
            {QStringLiteral("message"),
             QStringLiteral("pull line %1").arg(i)},
            {QStringLiteral("source"), QStringLiteral("fakesrv-pull")}});
    }
    out.append(QJsonObject{ // the INVERTED range {1,10}..{1,2}: the pair
                            // lands INSIDE kPullFixture's line 1, so the
                            // decoder's same-line col swap reads it exactly
                            // as {1,2}..{1,10} (a line-spanning inversion
                            // would swap the LINES only — a messier pin the
                            // push route's decoder shares, but the swap
                            // this noise exercises is the col one)
        {QStringLiteral("range"),
         QJsonObject{{QStringLiteral("start"),
                      QJsonObject{{QStringLiteral("line"), 1},
                                  {QStringLiteral("character"), 10}}},
                     {QStringLiteral("end"),
                      QJsonObject{{QStringLiteral("line"), 1},
                                  {QStringLiteral("character"), 2}}}}},
        {QStringLiteral("severity"), 2},
        {QStringLiteral("message"), QStringLiteral("swapped")},
        {QStringLiteral("source"), QStringLiteral("fakesrv-pull")}});
    out.append(QJsonObject{ // a GENEROUS end past the document: clamps to
                            // the mirror's last line (the flush proof moves
                            // the clamp with every edit)
        {QStringLiteral("range"),
         QJsonObject{{QStringLiteral("start"),
                      QJsonObject{{QStringLiteral("line"), 999},
                                  {QStringLiteral("character"), 4}}},
                     {QStringLiteral("end"),
                      QJsonObject{{QStringLiteral("line"), 999},
                                  {QStringLiteral("character"), 9}}}}},
        {QStringLiteral("message"), QStringLiteral("outdoc")},
        {QStringLiteral("source"), QStringLiteral("fakesrv-pull")}});
    out.append(QJsonObject{ // severity 9: out of range, reads Error
        {QStringLiteral("range"),
         QJsonObject{{QStringLiteral("start"),
                      QJsonObject{{QStringLiteral("line"), 0},
                                  {QStringLiteral("character"), 0}}},
                     {QStringLiteral("end"),
                      QJsonObject{{QStringLiteral("line"), 0},
                                  {QStringLiteral("character"), 1}}}}},
        {QStringLiteral("severity"), 9},
        {QStringLiteral("message"), QStringLiteral("sev9")},
        {QStringLiteral("source"), QStringLiteral("fakesrv-pull")}});
    out.append(QJsonValue(42)); // a bare number in items: decodes to an
                                // empty record (the door takes it verbatim
                                // — the push route's shape of the same
                                // tolerance, pinned on both sides)
    g_pullLastMarkerLines = marked; // the test/pullState echo: an ask that
                                    // outran didChange could not have seen
                                    // this token count
    return out;
}

void featureDiagnostic(const QJsonValue& id, const QJsonObject& params)
{
    const QString docUri = params.value(QStringLiteral("textDocument"))
                               .toObject()
                               .value(QStringLiteral("uri"))
                               .toString(); // publishDiagsFor's key (the
                                            // featureDoc guard below proves
                                            // this server holds it open)
    ModelDoc& d = featureDoc(params, QStringLiteral("textDocument/diagnostic"));
    if (params.contains(QStringLiteral("previousResultId")))
        fail(QStringLiteral("a textDocument/diagnostic ask carried a "
                            "previousResultId: this hub only ever pulls "
                            "FULL (the resultId is stored, never sent)"));
    if (params.value(QStringLiteral("identifier")).toString()
        != QStringLiteral("qmn"))
        fail(QStringLiteral("the pull ask's identifier is not \"qmn\" — the "
                            "hub's report series name got lost"));
    ++g_pullAsks;
    if (g_pullFailAfter >= 0 && g_pullAsks > g_pullFailAfter) {
        answerErrorTo(id, -32601,
                      QStringLiteral("diagnostic refused after being "
                                     "advertised (pullfail)"));
        if (g_pullPublish)
            publishDiagsFor(docUri); // the push route's trail rides RIGHT
                                     // behind the refusal: the hub's
                                     // suppression guard must open and this
                                     // publish must PAINT (the fallback, end
                                     // to end)
        return;
    }
    if (g_pullUnchanged && g_pullAsks >= 2) {
        send(response(id, QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("unchanged")},
            {QStringLiteral("resultId"), g_pullResultId}})); // spec: an
                                                             // unchanged
                                                             // report
                                                             // carries no
                                                             // items
        return;
    }
    QJsonObject report{{QStringLiteral("kind"), QStringLiteral("full")},
                       {QStringLiteral("resultId"), g_pullResultId},
                       {QStringLiteral("items"), pullReportItems(d)}};
    if (g_pullPublish)
        publishDiagsFor(docUri); // the CONTRADICTION: the same server pushes
                                 // while the pull is live — the hub must
                                 // drop it (no "bug" entry ever paints in
                                 // pull mode). The publish rides BEFORE the
                                 // report on the wire: suppression must hold
                                 // in either arrival order, and a hub that
                                 // lets it through paints the "bug" entry
                                 // visibly no matter how the report decoded.
    send(response(id, report));
}

void handlePullState(const QJsonValue& id)
{
    send(response(id, QJsonObject{
        {QStringLiteral("asks"), g_pullAsks},
        {QStringLiteral("markerLines"), g_pullLastMarkerLines}}));
}

void featureResolveItem(const QJsonValue& id, const QJsonValue& paramsValue)
{
    if (!g_advertisesResolve)
        fail(QStringLiteral("completionItem/resolve reached the server: the "
                            "hub must refuse it locally when "
                            "completionProvider has no resolveProvider"));
    if (!paramsValue.isObject())
        fail(QStringLiteral("completionItem/resolve params are not the item"));
    const QJsonObject item = paramsValue.toObject();
    const QString label = item.value(QStringLiteral("label")).toString();
    if (label.isEmpty())
        fail(QStringLiteral("completionItem/resolve item carries no \"label\""));
    if (item.value(QStringLiteral("data")).toObject()
            .value(QStringLiteral("origin"))
            .toString()
            .isEmpty())
        fail(QStringLiteral("completionItem/resolve item lost the \"data\" the "
                            "completion list gave it"));
    QJsonObject resolved = item; // spec: the SAME item, filled in
    resolved.insert(QStringLiteral("detail"),
                    QStringLiteral("resolved:") + label);
    resolved.insert(QStringLiteral("documentation"),
                    QStringLiteral("docs for %1").arg(label));
    send(response(id, resolved));
}

void featureCodeActions(const QJsonValue& id, const QJsonObject& params)
{
    ModelDoc& d = featureDoc(params, QStringLiteral("textDocument/codeAction"));
    const QJsonObject range = params.value(QStringLiteral("range")).toObject();
    const QJsonObject start = range.value(QStringLiteral("start")).toObject();
    const QJsonObject end = range.value(QStringLiteral("end")).toObject();
    if (!start.value(QStringLiteral("line")).isDouble()
        || !end.value(QStringLiteral("line")).isDouble())
        fail(QStringLiteral("textDocument/codeAction carries no Range"));
    const int from
        = specOffset(d.text, start.value(QStringLiteral("line")).toInt(),
                     start.value(QStringLiteral("character")).toInt());
    const int to = specOffset(d.text, end.value(QStringLiteral("line")).toInt(),
                              end.value(QStringLiteral("character")).toInt());
    if (from != to)
        fail(QStringLiteral("codeAction range is not ZERO-WIDTH on this "
                            "server's model: %1 vs %2").arg(from).arg(to));
    // THE machine-5 frame pin for this door: context.diagnostics must be the
    // CURRENT set the hub decoded, re-encoded back into the wire frame. The
    // comparison is against this server's OWN fresh scan of the SAME text:
    // every range, severity and message of a hub that re-encodes through a
    // stale text, a wrong frame or a shifted severity table shows up here.
    const QJsonObject context = params.value(QStringLiteral("context")).toObject();
    if (!context.contains(QStringLiteral("diagnostics")))
        fail(QStringLiteral("textDocument/codeAction context has no "
                            "\"diagnostics\""));
    const QJsonArray mine = diagsFor(d.text);
    const QJsonArray theirs
        = context.value(QStringLiteral("diagnostics")).toArray();
    if (mine != theirs) {
        std::fprintf(stderr,
                     "FAKESERVER-FAIL codeAction context.diagnostics is not "
                     "this server's own scan\n  server %s\n  client   %s\n",
                     qPrintable(QString::fromUtf8(
                         QJsonDocument(mine).toJson(QJsonDocument::Compact))),
                     qPrintable(QString::fromUtf8(QJsonDocument(theirs)
                                                      .toJson(
                                                          QJsonDocument::Compact))));
        std::fflush(stderr);
        _exit(2);
    }
    const QString uri = params.value(QStringLiteral("textDocument"))
                            .toObject()
                            .value(QStringLiteral("uri"))
                            .toString();
    const QStringList lines = modelLines(d.text);
    const int last = lines.size() - 1;
    const int lastCol = lines.at(last).size();
    QJsonArray actions;
    // 1. The plain shape: a WorkspaceEdit on the action's "edit".
    actions.append(QJsonObject{
        {QStringLiteral("title"), QStringLiteral("indent the head")},
        {QStringLiteral("kind"), QStringLiteral("quickfix")},
        {QStringLiteral("edit"),
         QJsonObject{{QStringLiteral("changes"),
                      QJsonObject{{uri,
                                   QJsonArray{textEditWire(
                                       d.text, 0, 0, 0, 0,
                                       QStringLiteral("# act\n"))}}}}}}});
    // 2. clangd's shape: the WorkspaceEdit hides in command.arguments[0]
    //    (the command NAME is noise; the payload carries the work).
    QJsonArray cmdEdits;
    cmdEdits.append(textEditWire(d.text, last, lastCol, last, lastCol,
                                 QStringLiteral(" // cmd")));
    QJsonObject cmdChanges;
    cmdChanges.insert(uri, cmdEdits);
    QJsonObject cmdEdit; // the WorkspaceEdit shape: just a "changes" map
    cmdEdit.insert(QStringLiteral("changes"), cmdChanges);
    QJsonObject cmdArgs; // the payload hides in arguments[0]
    cmdArgs.insert(QStringLiteral("title"), QStringLiteral("apply"));
    cmdArgs.insert(QStringLiteral("command"),
                   QStringLiteral("qmn.fakeserver.append"));
    cmdArgs.insert(QStringLiteral("arguments"), QJsonArray{cmdEdit});
    QJsonObject cmdAction;
    cmdAction.insert(QStringLiteral("title"), QStringLiteral("append the tail"));
    cmdAction.insert(QStringLiteral("kind"), QStringLiteral("refactor"));
    cmdAction.insert(QStringLiteral("command"), cmdArgs);
    actions.append(cmdAction);
    // 3. Structure v1 cannot do: a create-file documentChange.
    actions.append(QJsonObject{
        {QStringLiteral("title"), QStringLiteral("create a sidecar")},
        {QStringLiteral("kind"), QStringLiteral("source")},
        {QStringLiteral("edit"),
         QJsonObject{{QStringLiteral("documentChanges"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("kind"), QStringLiteral("create")},
                          {QStringLiteral("uri"),
                           QStringLiteral("file:///qmn-sidecar.new")}}}}}}});
    // 4. Neither shape at all: the decoder's "carries neither" problem.
    actions.append(QJsonObject{{QStringLiteral("title"),
                                QStringLiteral("nothing to apply")},
                               {QStringLiteral("kind"),
                                QStringLiteral("quickfix")}});
    send(response(id, actions));
}

void featureFormatting(const QJsonValue& id, const QJsonObject& params)
{
    ModelDoc& d
        = featureDoc(params, QStringLiteral("textDocument/formatting"));
    const QJsonObject options = params.value(QStringLiteral("options")).toObject();
    if (!params.value(QStringLiteral("options")).isObject())
        fail(QStringLiteral("textDocument/formatting carries no formatting "
                            "\"options\" object"));
    // The hub's fixed FormattingOptions (the params pin: a hub that sends
    // nothing, or a tab-formatted document, shows up here, not as a silent
    // no-op formatting that "worked").
    if (options.value(QStringLiteral("tabSize")).toInt(-1) != 4
        || !options.value(QStringLiteral("insertSpaces")).toBool())
        fail(QStringLiteral("formatting options are not the hub's fixed "
                            "{tabSize:4, insertSpaces:true}: %1")
                 .arg(QString::fromUtf8(QJsonDocument(options)
                                            .toJson(QJsonDocument::Compact))));
    // The spec's own result type: a BARE TextEdit[] (no uri, no version): the
    // uri is the document this request named, which is why the hub's applier
    // door takes the EDITOR and names it.
    const QStringList lines = modelLines(d.text);
    const int last = lines.size() - 1;
    const int lastCol = lines.at(last).size();
    QJsonArray edits;
    edits.append(textEditWire(d.text, 0, 0, 0, 0, QStringLiteral("# fmt\n")));
    edits.append(
        textEditWire(d.text, last, lastCol, last, lastCol, QStringLiteral(" // fmt")));
    send(response(id, edits));
}

void featureRename(const QJsonValue& id, const QJsonObject& params)
{
    ModelDoc& d = featureDoc(params, QStringLiteral("textDocument/rename"));
    const int offset
        = featureOffset(d, params, QStringLiteral("textDocument/rename"));
    const QString newName = params.value(QStringLiteral("newName")).toString();
    if (newName.isEmpty())
        fail(QStringLiteral("textDocument/rename carries no \"newName\""));
    int wordStart = -1;
    int wordEnd = -1;
    const QString word = wordAt(d.text, offset, &wordStart, &wordEnd);
    const QString uri = params.value(QStringLiteral("textDocument"))
                            .toObject()
                            .value(QStringLiteral("uri"))
                            .toString();
    if (word.isEmpty()) {
        send(response(id, QJsonValue(QJsonValue::Null))); // spec: null = this
        return; // caret renames nothing
    }
    const QStringList lines = modelLines(d.text);
    QJsonArray edits;
    const QVector<int> hits = occurrences(d.text, word);
    for (int at : hits) {
        int line = 0;
        int col = 0;
        rowColAt(lines, at, &line, &col);
        edits.append(textEditWire(d.text, line, col, line, col + word.size(),
                                  newName));
    }
    if (d.text.contains(QLatin1String("RENAMEVERSION"))) {
        // A version pin SEVEN ahead of this server's: the hub holds this
        // document at THIS version, so the WHOLE edit must be refused.
        QJsonArray docChanges;
        docChanges.append(QJsonObject{
            {QStringLiteral("textDocument"),
             QJsonObject{{QStringLiteral("uri"), uri},
                         {QStringLiteral("version"), double(d.version + 7)}}},
            {QStringLiteral("edits"), edits}});
        send(response(id,
                      QJsonObject{{QStringLiteral("documentChanges"),
                                   docChanges}}));
        return;
    }
    QJsonObject changes;
    changes.insert(uri, edits);
    if (d.text.contains(QLatin1String("RENAMEMULTI"))) {
        // The multi-file shape: the same rename in a document this hub does
        // NOT hold open. v1 hands workspace structure to the app, so ALL of
        // it must be refused — this document's own edits included, which is
        // what makes a half-applied rename visible.
        changes.insert(QStringLiteral("file:///qmn-not-open.cpp"),
                       QJsonArray{textEditWire(QStringLiteral("sibling unit"),
                                               0, 0, 0, 8, newName)});
    }
    send(response(id, QJsonObject{{QStringLiteral("changes"), changes}}));
}

void featureHover(const QJsonValue& id, const QJsonValue& paramsValue)
{
    if (!g_hoverOn)
        // hoverProvider is advertised FALSE in plain features mode: the
        // hub's gate must refuse this door LOCALLY. Traffic reaching this
        // handler is a gate bug.
        fail(QStringLiteral("textDocument/hover reached the server although "
                            "hoverProvider is false"));
    // The `hoveron` branch (popup triggers): hover is REAL. The proof rides
    // LEGAL fields: the word the caret sits on, the offset the request
    // carried (this server decoding its OWN wire frame), and the word's
    // range — so the hub's decoded popup text is pinned verbatim by the tst.
    ModelDoc& d = featureDoc(paramsValue.toObject(),
                             QStringLiteral("textDocument/hover"));
    const int offset
        = featureOffset(d, paramsValue.toObject(),
                        QStringLiteral("textDocument/hover"));
    int wordStart = -1;
    int wordEnd = -1;
    const QString word = wordAt(d.text, offset, &wordStart, &wordEnd);
    QJsonValue contents; // both contents shapes ride: MARKUPCONTENT by
                         // default, contents-ARRAY (MarkedString object +
                         // plain string) when the doc says ARRAYHOVER
    if (d.text.contains(QLatin1String("ARRAYHOVER"))) {
        contents = QJsonArray{
            QJsonObject{{QStringLiteral("language"), QStringLiteral("python")},
                        {QStringLiteral("value"),
                         QStringLiteral("lang one")}},
            QStringLiteral("plain two")};
    } else {
        contents = QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("markdown")},
            {QStringLiteral("value"),
             QStringLiteral("HOVER %1 off=%2").arg(word,
                                                   QString::number(offset))}};
    }
    QJsonObject result{{QStringLiteral("contents"), contents}};
    if (!word.isEmpty()) {
        const QStringList lines = modelLines(d.text);
        int line = 0;
        int col = 0;
        rowColAt(lines, wordStart, &line, &col);
        result.insert(QStringLiteral("range"),
                      rangeWire(d.text, line, col, line, col + word.size()));
    }
    send(response(id, result));
}

// The didSave markers (features mode): each primes ONE server-initiated
// request whose answer this server validates itself (the client side of a
// server-primed request is traffic the tst cannot see) and whose effect on
// the model it pins. Version discipline: this server bumped its version on
// the last didChange, exactly like the client's syncer did, so pinning
// version = d.version here pins the SAME number the hub's syncer holds.
void featureDidSave(const QString& uri, ModelDoc& d)
{
    const QString reqId = QStringLiteral("s-f%d-%1")
                              .arg(++g_featureSaveSeq)
                              .arg(uri);
    if (d.text.contains(QLatin1String("APPLYEDIT"))) {
        // The whole write path, one round trip: the server computes a
        // WorkspaceEdit in its own frame, applies it to its OWN model with
        // its own applier, stores the result as the full-sync expectation,
        // and asks the client to do the same. Col 0 of line 0 is col 0 in
        // EVERY negotiated frame: the edit's anchor needs no encoding luck.
        const QJsonArray edits
            = {textEditWire(d.text, 0, 0, 0, 0, QStringLiteral("srv\n"))};
        applyWireEdits(d, edits);
        g_applyVerify = d.text;
        send(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), reqId},
            {QStringLiteral("method"), QStringLiteral("workspace/applyEdit")},
            {QStringLiteral("params"),
             QJsonObject{
                 {QStringLiteral("edit"),
                  QJsonObject{
                      {QStringLiteral("documentChanges"),
                       QJsonArray{QJsonObject{
                           {QStringLiteral("textDocument"),
                            QJsonObject{{QStringLiteral("uri"), uri},
                                        {QStringLiteral("version"),
                                         double(d.version)}}},
                           {QStringLiteral("edits"), edits}}}}}},
                 {QStringLiteral("label"),
                  QStringLiteral("fakeserver's own edit")}}}});
        g_outstanding.insert(reqId, QStringLiteral("workspace/applyEdit"));
        return;
    }
    if (d.text.contains(QLatin1String("APPLYREFUSE"))) {
        // The refusal drill: the same legal edit PLUS a documentChanges
        // entry for a uri this hub does not hold open. v1 hands workspace
        // structure edits to the app, so ALL of it must be refused — the
        // honest applied:false the client answers is checked in
        // checkServerRequestResponse, and the model must not have moved.
        QJsonArray docChanges;
        docChanges.append(QJsonObject{
            {QStringLiteral("textDocument"),
             QJsonObject{{QStringLiteral("uri"), uri},
                         {QStringLiteral("version"), double(d.version)}}},
            {QStringLiteral("edits"),
             QJsonArray{
                 textEditWire(d.text, 0, 0, 0, 0,
                              QStringLiteral("refused\n"))}}});
        docChanges.append(QJsonObject{
            {QStringLiteral("textDocument"),
             QJsonObject{{QStringLiteral("uri"),
                          QStringLiteral("file:///qmn-not-open.cpp")},
                         {QStringLiteral("version"), 0.0}}},
            {QStringLiteral("edits"),
             QJsonArray{textEditWire(QStringLiteral("sibling"), 0, 0, 0, 7,
                                     QStringLiteral("nope"))}}});
        send(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), reqId},
            {QStringLiteral("method"), QStringLiteral("workspace/applyEdit")},
            {QStringLiteral("params"),
             QJsonObject{{QStringLiteral("edit"),
                          QJsonObject{{QStringLiteral("documentChanges"),
                                       docChanges}}}}}});
        g_outstanding.insert(
            reqId, QStringLiteral("workspace/applyEdit-refuse:") + d.text);
        return;
    }
    if (d.text.contains(QLatin1String("MSGREQ"))) {
        // window/showMessageRequest: the tst answers the FIRST variant from
        // inside the hub relay (through the client door or, since car E
        // machine E3, the hub's answerShowMessageRequest door); the other
        // variant stays silent (or only its id gets REFUSED probes) and the
        // client layer's own Null default must answer for it.
        const bool first = d.text.contains(QLatin1String("MSGREQ-FIRST"));
        QJsonArray actions;
        actions.append(QJsonObject{{QStringLiteral("title"),
                                    QStringLiteral("FIRST")}});
        actions.append(QJsonObject{{QStringLiteral("title"),
                                    QStringLiteral("SECOND")}});
        send(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), reqId},
            {QStringLiteral("method"),
             QStringLiteral("window/showMessageRequest")},
            {QStringLiteral("params"),
             QJsonObject{{QStringLiteral("type"), 3},
                         {QStringLiteral("message"),
                          QStringLiteral("pick one")},
                         {QStringLiteral("actions"), actions}}}});
        g_outstanding.insert(reqId, first
                                     ? QStringLiteral(
                                           "showMessageRequest-first")
                                     : QStringLiteral(
                                           "showMessageRequest-null"));
        return;
    }
}

// The pre-save round trip (M8 machine 6): hub.saveDocument asked because this
// server advertised it. The reply shape is the spec's BARE TextEdit[]; this
// file applies the SAME edits to its OWN model first and arms the full-sync
// oracle (g_applyVerify), so the client's endServerEdit echo is checked by
// APPLYCHECK exactly like the machine-5 write path. Col 0 of line 0 is col 0
// in EVERY negotiated frame: the edit's anchor needs no encoding luck.
void featureWillSaveWaitUntil(const QJsonValue& id, const QJsonObject& params)
{
    if (params.value(QStringLiteral("reason")).toInt() != 1)
        fail(QStringLiteral("willSaveWaitUntil reason is %1, expected 1 "
                            "(TextDocumentSaveReason.Manual)")
                 .arg(params.value(QStringLiteral("reason")).toInt()));
    const QString uri = params.value(QStringLiteral("textDocument"))
                            .toObject()
                            .value(QStringLiteral("uri"))
                            .toString();
    auto found = g_docs.find(uri);
    if (found == g_docs.end())
        fail(QStringLiteral("willSaveWaitUntil for a document that is not open"));
    ModelDoc& d = found.value();
    if (!d.text.contains(QLatin1String("WSWU")))
        fail(QStringLiteral("willSaveWaitUntil asked about a document without "
                            "the drill marker"));
    if (d.text.contains(QLatin1String("WSWU-REFUSE"))) {
        // The REFUSE drill. Honest note: the spec says the willSaveWaitUntil
        // result is TextEdit[]|null — a WorkspaceEdit is NOT a legal answer
        // to THIS request. It is the only shape that can carry a refusal the
        // hub's uri/version guards act on (a bare list names no document to
        // refuse), and the hub's decode branch (array vs WorkspaceEdit)
        // accepts both by design. This deviation buys the all-or-nothing
        // refusal proof; the LEGAL bare-list path is proven by the plain
        // WSWU drill above.
        QJsonObject ghostDoc{{QStringLiteral("uri"),
                              QStringLiteral("file:///qmn-not-open.cpp")}};
        QJsonObject change{{QStringLiteral("textDocument"), ghostDoc},
                           {QStringLiteral("version"), 0.0}};
        change.insert(
            QStringLiteral("edits"),
            QJsonArray{textEditWire(QStringLiteral("sibling unit"), 0, 0, 0, 7,
                                    QStringLiteral("nope"))});
        QJsonObject result;
        result.insert(QStringLiteral("documentChanges"), QJsonArray{change});
        send(response(id, result));
        return;
    }
    const QJsonArray edits
        = {textEditWire(d.text, 0, 0, 0, 0, QStringLiteral("wswu-applied\n"))};
    applyWireEdits(d, edits);
    g_applyVerify = d.text;
    send(response(id, edits));
}

// The didSave-time pins (M8 machine 6): the ORDERING full-sync-before-didSave
// is exactly what is observable HERE — if didSave arrives while this server's
// model still holds the PRE-edit text, the hub's save raced ahead of the edit
// round trip (a structural lie: exit 2). A refused batch must leave the model
// untouched by the wswu path.
void wswuDidSavePin(const ModelDoc& d)
{
    if (!d.text.contains(QLatin1String("WSWU")))
        return; // not a drill document: an ordinary save
    if (d.text.contains(QLatin1String("WSWU-REFUSE"))) {
        const bool whole = !d.text.contains(QLatin1String("wswu-applied"));
        std::fprintf(stderr, "WSWU-REFUSE-SAVE-%s\n", whole ? "OK" : "FAIL");
        std::fflush(stderr);
        if (!whole)
            _exit(2); // a refused edit reached the model on ONE side of the
                      // wire anyway: the all-or-nothing gate broke
        return;
    }
    // The ORDERING proof: the reply handler armed g_applyVerify and only the
    // client's full-sync didChange (the endServerEdit echo) may clear it. If
    // the oracle is still armed when didSave lands, the didSave overtook the
    // sync the applier emitted — the server saw the PRE-edit model saved.
    const bool syncedFirst = g_applyVerify.isEmpty();
    std::fprintf(stderr, "WSWU-SAVE-%s\n", syncedFirst ? "OK" : "FAIL");
    std::fflush(stderr);
    if (!syncedFirst)
        _exit(2); // didSave beat the edit round trip
}

// The SERVER-side half of the machine-5 write-path pins: what the client
// answered to OUR request is traffic the tst cannot observe, so this file
// judges it. A wrong answer is a structural wire lie: exit 2.
void checkServerRequestResponse(const QJsonObject& message)
{
    const QString id = message.value(QStringLiteral("id")).toString();
    auto found = g_outstanding.find(id);
    if (found == g_outstanding.end())
        return; // not one of ours: leftover battery traffic, recorded only
    const QString what = found.value();
    g_outstanding.erase(found);
    const bool isError = message.contains(QStringLiteral("error"));
    const QJsonValue result = message.value(QStringLiteral("result"));
    if (what == QLatin1String("workspace/applyEdit")) {
        // LspClient's own answer is the spec's silent-until-told; the hub
        // OVERRIDE is what the drill drives: it must answer applied:true.
        if (isError || !result.toObject().value(QStringLiteral("applied"))
                            .toBool())
            fail(QStringLiteral("the client answered the server's applyEdit "
                                "with applied:false (hub applier broken?)"));
        std::fprintf(stderr, "APPLYREQ-OK\n");
        std::fflush(stderr);
        return;
    }
    if (what.startsWith(QLatin1String("workspace/applyEdit-refuse:"))) {
        const QString untouched
            = what.mid(int(qstrlen("workspace/applyEdit-refuse:")));
        if (isError
            || result.toObject().value(QStringLiteral("applied")).toBool())
            fail(QStringLiteral("the client answered applied:true to an "
                                "applyEdit it was supposed to REFUSE"));
        // The refusal must have kept this server's model whole: the entry
        // for a uri the hub does not hold open poisons the WHOLE edit, this
        // document's own changes included (all-or-nothing is what makes a
        // half-applied rename visible).
        QString modelNow;
        for (auto it = g_docs.cbegin(); it != g_docs.cend(); ++it) {
            if (it.value().text == untouched) {
                modelNow.clear();
                break;
            }
            modelNow = it.value().text;
        }
        if (!modelNow.isEmpty())
            fail(QStringLiteral("the refused applyEdit still moved the "
                                "server model: \"%1\"")
                     .arg(modelNow));
        std::fprintf(stderr, "APPLYREFUSE-OK\n");
        std::fflush(stderr);
        return;
    }
    if (what == QLatin1String("showMessageRequest-first")) {
        if (isError || result.isNull() || result.isUndefined()
            || result.toObject().value(QStringLiteral("title")).toString()
                   != QLatin1String("FIRST"))
            fail(QStringLiteral("the first showMessageRequest answer was not "
                                "the item \"FIRST\" (the client's default or "
                                "a late answer won — first-answer-wins is "
                                "broken)"));
        std::fprintf(stderr, "MSGREQ-OK\n");
        std::fflush(stderr);
        return;
    }
    if (what == QLatin1String("showMessageRequest-null")) {
        if (!isError && !result.isNull() && !result.isUndefined())
            fail(QStringLiteral("somebody answered the showMessageRequest "
                                "this test left silent: the client layer's "
                                "Null default is broken"));
        std::fprintf(stderr, "MSGREQ-NULL-OK\n");
        std::fflush(stderr);
        return;
    }
    if (what == QLatin1String("pull-register")) {
        // Car C (C5): the DYNAMIC registration ride. The client registry
        // must ACCEPT textDocument/diagnostic (LspClient's registrable set)
        // and answer the spec's null. This exchange rides the HANDSHAKE
        // window (the request leaves on `initialized`), so no stderr MARKER
        // here: a tst spy attached after waitReady may not see one. The
        // fail()s below are the far-side tripwire — a rejecting client
        // kills the server and the pull paint never comes.
        if (isError)
            fail(QStringLiteral("the client REJECTED the dynamic "
                                "textDocument/diagnostic registration: "
                                "registrable set broken"));
        if (!result.isNull() && !result.isUndefined())
            fail(QStringLiteral("the registerCapability answer is not the "
                                "spec's null"));
        return;
    }
    if (what.startsWith(QLatin1String("pull-unregister:"))) {
        // Car C (C5): the un-registration's acknowledgment — the client
        // answers null, the registry is empty, and the PUSH trail rides out
        // RIGHT HERE: with the pull authority gone the hub's suppression
        // guard must open and this very publish must paint (the fallback's
        // dynamic-legible trail).
        if (isError || (!result.isNull() && !result.isUndefined()))
            fail(QStringLiteral("the unregisterCapability answer is not the "
                                "spec's null"));
        publishDiagsFor(what.mid(int(qstrlen("pull-unregister:"))));
        std::fprintf(stderr, "PULLUNREG-OK\n");
        std::fflush(stderr);
        return;
    }
}

// ------------------------------------------------------------- the server
void handleInitialize(const QJsonObject& request)
{
    if (!request.value(QStringLiteral("params"))
             .toObject()
             .contains(QStringLiteral("capabilities")))
        fail(QStringLiteral("initialize params lack \"capabilities\""));
    g_initializeParams = request.value(QStringLiteral("params")).toObject();
    QJsonObject capabilities;
    if (g_sigHelp) {
        // GATE 3 car C (C2): the signatureHelp round's provider set. The
        // triggerCharacters list is ADVERTISED here — the hub classifies the
        // unit before the caret against THIS list and only falls back to its
        // own default pair when a provider names none. hoverProvider is
        // FALSE: the hover popup is the shared surface, and a mode where
        // both owners could ask would hide a no-clobber bug behind the other
        // route's traffic. completionon (`completiononly`) rides it for the
        // one slot that pins a single keystroke lighting BOTH debounces.
        capabilities.insert(QStringLiteral("hoverProvider"), false);
        capabilities.insert(
            QStringLiteral("signatureHelpProvider"),
            QJsonObject{{QStringLiteral("triggerCharacters"),
                         QJsonArray{QStringLiteral("("),
                                    QStringLiteral(",")}}});
        if (g_completionOnly) {
            // The BOTH-debounce shape: one "(" press arms the completion AND
            // the signature debounce (the shared typing tap), and the reqlog
            // oracle shows both round trips. "(" is advertised as a
            // completion trigger HERE so the one press is a trigger for both
            // routes (kind 2 each) — the pin is the two asks from one burst.
            QJsonObject completion;
            completion.insert(QStringLiteral("triggerCharacters"),
                              QJsonArray{QStringLiteral("(")});
            capabilities.insert(QStringLiteral("completionProvider"),
                                completion);
            g_completionTriggers = QStringList{QStringLiteral("(")};
        }
    } else if (g_completionOnly) {
        // The nested capability gate's pin: completionProvider WITHOUT
        // resolveProvider, and NOTHING else advertised. The completion door
        // itself must still WORK (its reply is the plain list shape); the
        // resolve door must be refused LOCALLY by the hub — if a resolve
        // request reaches this server, featureResolveItem exits 2.
        g_advertisesResolve = false;
        QJsonObject completion;
        completion.insert(QStringLiteral("triggerCharacters"),
                          QJsonArray{QStringLiteral(".")});
        capabilities.insert(QStringLiteral("completionProvider"), completion);
        g_completionTriggers = QStringList{QStringLiteral(".")};
    } else if (g_features) {
        // The machine-5 provider set. One deliberately OFF shape rides it:
        // hoverProvider FALSE (the hub must refuse hover locally; a hover
        // request that reaches this server exits 2 below) — UNLESS `hoveron`
        // rides too (the popup-trigger sliver: provider true, real answers).
        // Everything else is fully advertised so a missing capability can
        // never mask a frame bug.
        g_advertisesResolve = true;
        capabilities.insert(QStringLiteral("hoverProvider"), g_hoverOn);
        capabilities.insert(QStringLiteral("definitionProvider"),
                            QJsonObject{}); // object shape: the truthy-door pin
        capabilities.insert(QStringLiteral("referencesProvider"), true);
        QJsonObject completion;
        completion.insert(QStringLiteral("triggerCharacters"),
                          QJsonArray{QStringLiteral(".")});
        completion.insert(QStringLiteral("resolveProvider"), true);
        capabilities.insert(QStringLiteral("completionProvider"), completion);
        g_completionTriggers = QStringList{QStringLiteral(".")};
        capabilities.insert(QStringLiteral("codeActionProvider"), true);
        capabilities.insert(QStringLiteral("documentFormattingProvider"), true);
        capabilities.insert(QStringLiteral("renameProvider"),
                            QJsonObject{{QStringLiteral("prepareProvider"),
                                         false}});
    } else {
        capabilities.insert(QStringLiteral("hoverProvider"), true);
    }
    if (g_foldRange) {
        // Car C (C3): the fold authority. Before this server agrees to be
        // one, it reads the CLIENT's claim back: this report is line-only and
        // carries no columns, so a client that asks for folds without
        // declaring textDocument.foldingRange.lineFoldingOnly is holding a
        // shape it never said it folds.
        const QJsonObject clientFolding = g_initializeParams
                                              .value(QStringLiteral("capabilities"))
                                              .toObject()
                                              .value(QStringLiteral("textDocument"))
                                              .toObject()
                                              .value(QStringLiteral("foldingRange"))
                                              .toObject();
        if (!clientFolding.value(QStringLiteral("lineFoldingOnly")).toBool())
            fail(QStringLiteral("a foldingRange provider was asked for folds "
                                "while the client declared no "
                                "textDocument.foldingRange.lineFoldingOnly"));
        capabilities.insert(QStringLiteral("foldingRangeProvider"), true);
    }
    if (g_inlayHint) {
        // Car C (C4): the hint authority. The client declared
        // `inlayHint: {}` — the honest no-claim shape (no labelSupport, so
        // this server answers label PARTS knowing the client concatenates,
        // and no before/afterLines claim, so anchors must be exact). A
        // provider for a client that declared nothing about hints would be
        // a capability conversation nobody had.
        if (!g_initializeParams
                 .value(QStringLiteral("capabilities"))
                 .toObject()
                 .value(QStringLiteral("textDocument"))
                 .toObject()
                 .contains(QStringLiteral("inlayHint")))
            fail(QStringLiteral("an inlayHintProvider was asked for hints "
                                "while the client declared no "
                                "textDocument.inlayHint capability"));
        capabilities.insert(QStringLiteral("inlayHintProvider"), true);
    }
    if (g_pullDiag && !g_pullDyn) {
        // Car C (C5): the PULL DIAGNOSTICS authority. The client declared
        // `diagnostic: {relatedDocumentSupport:false}` — the honest v1 shape
        // (per-document pulls, no related-document interleaving). THIS read
        // is also the tripwire for the claim's CONTENT: a client that said
        // relatedDocumentSupport:true while never pulling related documents
        // would be lying on the wire, so anything other than the false
        // claim is exit 2 — the far-side pin that the hub's initialize
        // really declares relatedDocumentSupport:false.
        const QJsonObject diagClaim
            = g_initializeParams
                  .value(QStringLiteral("capabilities"))
                  .toObject()
                  .value(QStringLiteral("textDocument"))
                  .toObject()
                  .value(QStringLiteral("diagnostic"))
                  .toObject();
        if (!g_initializeParams
                 .value(QStringLiteral("capabilities"))
                 .toObject()
                 .value(QStringLiteral("textDocument"))
                 .toObject()
                 .contains(QStringLiteral("diagnostic")))
            fail(QStringLiteral("a diagnosticProvider was asked to pull "
                                "while the client declared no "
                                "textDocument.diagnostic capability"));
        if (diagClaim.value(QStringLiteral("relatedDocumentSupport"))
                .toBool(false))
            fail(QStringLiteral("the client claimed "
                                "relatedDocumentSupport:true but this hub's "
                                "v1 pulls are per-document only — the "
                                "declare must say false"));
        capabilities.insert(QStringLiteral("diagnosticProvider"),
                            QJsonObject{
                                {QStringLiteral("workDoneProgress"), false},
                                {QStringLiteral("interFileDependencies"),
                                 false}});
    }
    if (g_wswu) {
        // The M8 machine 6 declaration: TextDocumentSyncOptions shape — the
        // integer change form can NEVER carry willSaveWaitUntil, and the
        // hub's save detour opens only for this object with the key true.
        // This server's sync behavior is unchanged either way: the client's
        // syncer never reads this capability (the machine-4 proof).
        capabilities.insert(QStringLiteral("textDocumentSync"),
                            QJsonObject{{QStringLiteral("openClose"), true},
                                        {QStringLiteral("change"), 2}, // Incremental
                                        {QStringLiteral("willSave"), true},
                                        {QStringLiteral("willSaveWaitUntil"),
                                         true}});
    }
    send(response(request.value(QStringLiteral("id")),
                  QJsonObject{{QStringLiteral("capabilities"), capabilities},
                              {QStringLiteral("positionEncoding"),
                               g_syncEncoding},
                              {QStringLiteral("serverInfo"),
                               QJsonObject{{QStringLiteral("name"),
                                            QStringLiteral("qmn-fakeserver")}}}}));
}

void handleInitialized()
{
    g_initializedSeen = true; // M8 machine 7: the ordering pin's reference
                              // point (a config that beats this is exit 2)
    if (g_mode == QLatin1String("sync") && g_crashOnce && !g_firstHandshakeDone) {
        // The self-heal drill: ONLY the first session dies right after the
        // handshake; the client's one auto-restart then re-runs it, ready()
        // re-fires, and the syncer's re-didOpen must carry the mirror whole.
        g_firstHandshakeDone = true;
        std::fprintf(stderr, "SYNC-CRASH bye after the first handshake\n");
        std::fflush(stderr);
        _exit(7);
    }
    if (g_mode == QLatin1String("crash")) {
        // The crash ladder: EVERY session dies right after the handshake
        // (the client's restart/rearm semantics ride the repetition).
        std::fprintf(stderr, "CRASHER: bye after handshake\n");
        std::fflush(stderr);
        _exit(7);
    }
    if (g_mode == QLatin1String("requests")) {
        runBattery();
        return;
    }
    if (g_pullDyn) {
        // Car C (C5): the DYNAMIC registration ride — the server advertises
        // NOTHING at initialize and registers textDocument/diagnostic
        // right after the handshake instead (a REGISTER Capability request,
        // the spec's shape; LspClient's real registry answers it and fires
        // registrationsChanged, and the hub's armPullDiagFeed relay does the
        // rest). Rides the initialized notification, never before: the M8
        // ordering pin says no traffic may beat `initialized`.
        const QString reqId = QStringLiteral("s-pullreg");
        send(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), reqId},
            {QStringLiteral("method"),
             QStringLiteral("client/registerCapability")},
            {QStringLiteral("params"),
             QJsonObject{{QStringLiteral("registrations"),
                          QJsonArray{QJsonObject{
                              {QStringLiteral("id"),
                               QStringLiteral("pull-v1")},
                              {QStringLiteral("method"),
                               QStringLiteral("textDocument/diagnostic")},
                              {QStringLiteral("registerOptions"),
                               QJsonObject{{QStringLiteral(
                                               "workDoneProgress"),
                                            false}}}}}}}}});
        g_outstanding.insert(reqId, QStringLiteral("pull-register"));
    }
    if (g_mode == QLatin1String("basic")) {
        notify(QStringLiteral("window/showMessage"),
               QJsonObject{{QStringLiteral("type"), 3},
                           {QStringLiteral("message"), QStringLiteral("basic hello")}});
        notify(QStringLiteral("window/logMessage"),
               QJsonObject{{QStringLiteral("type"), 4},
                           {QStringLiteral("message"), QStringLiteral("basic log")}});
        notify(QStringLiteral("$/logTrace"),
               QJsonObject{{QStringLiteral("message"), QStringLiteral("basic trace")}});
        notify(QStringLiteral("$/progress"),
               QJsonObject{{QStringLiteral("token"), QStringLiteral("p-1")},
                           {QStringLiteral("value"),
                            QJsonObject{{QStringLiteral("kind"),
                                         QStringLiteral("incremental")},
                                        {QStringLiteral("value"), 7}}}});
        QJsonArray actions;
        actions.append(QJsonObject{{QStringLiteral("title"),
                                    QStringLiteral("Do the thing")}});
        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), QStringLiteral("s-req-1")},
                         {QStringLiteral("method"),
                          QStringLiteral("window/showMessageRequest")},
                         {QStringLiteral("params"),
                          QJsonObject{{QStringLiteral("type"), 1},
                                      {QStringLiteral("message"),
                                       QStringLiteral("basic request")},
                                      {QStringLiteral("actions"), actions}}}});
        // workDoneProgress/create is a REQUEST (spec): the client answers
        // null and re-emits the token as createWorkDoneProgress. Sending it
        // as a notification would leave the client's request handler unrouted.
        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), QStringLiteral("s-req-2")},
                         {QStringLiteral("method"),
                          QStringLiteral("workDoneProgress/create")},
                         {QStringLiteral("params"),
                          QJsonObject{{QStringLiteral("token"),
                                       QStringLiteral("w-1")}}}});
        notify(QStringLiteral("window/showMessage"),
               QJsonObject{{QStringLiteral("type"), 3},
                           {QStringLiteral("message"), QStringLiteral("second hello")}});
        notify(QStringLiteral("some/unknownNotification"),
               QJsonValue(QJsonValue::Undefined));
        return;
    }
    if (g_mode == QLatin1String("stderr")) {
        // The PROTOCOL channel marker; the STDERR marker was written before
        // the handshake. Neither may leak into the other client signal.
        notify(QStringLiteral("window/logMessage"),
               QJsonObject{{QStringLiteral("type"), 4},
                           {QStringLiteral("message"), QStringLiteral("PROTOCOL-MARKER")}});
    }
}

void runLoop()
{
    QSocketNotifier stdinReady(0, QSocketNotifier::Read);
    QObject::connect(&stdinReady, &QSocketNotifier::activated, [&] {
        char buffer[4096];
        const ssize_t n = ::read(0, buffer, sizeof(buffer));
        if (n > 0) {
            g_framing.feed(QByteArray(buffer, int(n)));
            QByteArray payload;
            while (g_framing.poll(&payload)) {
                const QJsonObject message
                    = QJsonDocument::fromJson(payload).object();
                if (message.isEmpty())
                    fail(QStringLiteral("frame is not a JSON object"));
                g_recorded.append(message);
                const QJsonValue methodValue
                    = message.value(QStringLiteral("method"));
                if (methodValue.isUndefined()) {
                    // A RESPONSE from the client. The battery consumes its
                    // own through readNextMessage; a machine-5 server-primed
                    // request's answer is JUDGED here (the tst cannot see
                    // this traffic); anything else is leftover: recorded,
                    // ignored.
                    checkServerRequestResponse(message);
                    continue;
                }
                const QString method = methodValue.toString();
                const QJsonValue id = message.value(QStringLiteral("id"));
                const QJsonValue params = message.value(QStringLiteral("params"));

                if (g_reqLog && !id.isUndefined()
                    && method.startsWith(QLatin1String("textDocument/"))) {
                    // The trigger tests' traffic oracle: EVERY feature
                    // request the hub actually sends shows on the STDERR
                    // channel (a trigger that fires nothing shows nothing).
                    std::fprintf(stderr, "REQ %s\n",
                                 method.toUtf8().constData());
                    std::fflush(stderr);
                }

                if (method == QLatin1String("initialize")) {
                    if (g_mode != QLatin1String("deaf"))
                        handleInitialize(message);
                } else if (method == QLatin1String("initialized")) {
                    handleInitialized();
                } else if (method == QLatin1String("shutdown")) {
                    send(response(id, QJsonValue(QJsonValue::Null)));
                } else if (method == QLatin1String("exit")) {
                    if (g_mode != QLatin1String("requests"))
                        _exit(0);
                } else if (method == QLatin1String("$/cancelRequest")) {
                    if (g_cancelLog) {
                        // The supersede oracle for the popup-trigger tests:
                        // the cancelled request's id on the STDERR channel.
                        std::fprintf(stderr, "CANCELREQ %s\n",
                                     params.toObject()
                                         .value(QStringLiteral("id"))
                                         .toVariant()
                                         .toString()
                                         .toUtf8()
                                         .constData());
                        std::fflush(stderr);
                    }
                    // spec-silent
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("textDocument/didOpen")) {
                    syncDidOpen(params);
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("textDocument/didChange")) {
                    syncDidChange(params);
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("textDocument/didClose")) {
                    syncDidClose(params);
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("textDocument/didSave")) {
                    syncDidSave(params);
                } else if (g_mode == QLatin1String("sync") && g_configPin
                           && method == QLatin1String(
                                  "workspace/didChangeConfiguration")) {
                    syncDidChangeConfiguration(params); // M8 m7: order + payload
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("test/assertModel")) {
                    handleAssertModel(id, params);
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("test/documentCount")) {
                    handleDocumentCount(id);
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("test/initializeParams")) {
                    handleInitializeParams(id);
                } else if (method == QLatin1String("textDocument/willSaveWaitUntil")) {
                    if (!g_wswu)
                        fail(QStringLiteral(
                            "textDocument/willSaveWaitUntil reached a server "
                            "that never advertised it: the hub's save detour "
                            "ignored the capability gate"));
                    featureWillSaveWaitUntil(id, params.toObject());
                } else if ((g_features)
                           && method == QLatin1String("completionItem/resolve")) {
                    featureResolveItem(id, params); // gate tripwires FIRST
                } else if ((g_features)
                           && method == QLatin1String("textDocument/hover")) {
                    featureHover(id, params.toObject()); // the OFF-provider
                                                         // tripwire
                } else if ((g_features)
                           && method == QLatin1String("textDocument/definition")) {
                    featureDefinition(id, params.toObject());
                } else if ((g_features)
                           && method == QLatin1String("textDocument/references")) {
                    featureReferences(id, params.toObject());
                } else if ((g_features || (g_sigHelp && g_completionOnly))
                           && method == QLatin1String("textDocument/completion")) {
                    featureCompletion(id, params.toObject());
                } else if (g_sigHelp
                           && method == QLatin1String(
                                  "textDocument/signatureHelp")) {
                    featureSignatureHelp(id, params.toObject());
                } else if (method == QLatin1String(
                               "textDocument/foldingRange")) {
                    if (!g_foldRange)
                        fail(QStringLiteral(
                            "textDocument/foldingRange reached a server that "
                            "never advertised foldingRangeProvider: the hub's "
                            "fold feed ignored the capability gate"));
                    featureFoldingRange(id, params.toObject());
                } else if (method == QLatin1String(
                               "textDocument/inlayHint")) {
                    if (!g_inlayHint)
                        fail(QStringLiteral(
                            "textDocument/inlayHint reached a server that "
                            "never advertised inlayHintProvider: the hub's "
                            "hint feed ignored the capability gate"));
                    featureInlayHint(id, params.toObject());
                } else if (method == QLatin1String(
                               "textDocument/diagnostic")) {
                    if (!g_pullDiag)
                        fail(QStringLiteral(
                            "textDocument/diagnostic reached a server that "
                            "never took the pull (no `pull` flag, no dynamic "
                            "registration): the hub's pull feed ignored the "
                            "gate"));
                    featureDiagnostic(id, params.toObject());
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("test/pullState")) {
                    handlePullState(id);
                } else if (g_mode == QLatin1String("sync")
                           && id.isUndefined()
                           && method == QLatin1String("test/pullUnregister")) {
                    // Car C (C5): the tst asks the server to DROP the dynamic
                    // pull authority — a client/unregisterCapability REQUEST
                    // rides back (the spec's shape, the client's registry
                    // answers null and fires registrationsChanged), and the
                    // null acknowledgment in checkServerRequestResponse is
                    // where the push trail resumes. An unknown-id notify
                    // reaching this branch IS the point: the hub re-modes the
                    // document the moment the registry empties.
                    if (!g_pullDyn)
                        fail(QStringLiteral("test/pullUnregister reached a "
                                            "server with no dynamic pull "
                                            "registration to drop"));
                    const QString uri = params.toObject()
                                            .value(QStringLiteral("uri"))
                                            .toString();
                    if (!g_docs.contains(uri))
                        fail(QStringLiteral("test/pullUnregister names a "
                                            "document this server does not "
                                            "hold open: \"%1\"").arg(uri));
                    const QString reqId = QStringLiteral("s-pullunreg");
                    send(QJsonObject{
                        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), reqId},
                        {QStringLiteral("method"),
                         QStringLiteral("client/unregisterCapability")},
                        {QStringLiteral("params"),
                         QJsonObject{{QStringLiteral("unregisterations"),
                                      QJsonArray{QJsonObject{
                                          {QStringLiteral("id"),
                                           QStringLiteral("pull-v1")},
                                          {QStringLiteral("method"),
                                           QStringLiteral(
                                               "textDocument/diagnostic")}}}}}}});
                    g_outstanding.insert(reqId,
                                         QStringLiteral("pull-unregister:")
                                             + uri);
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("test/signatureState")) {
                    handleSignatureState(id);
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("test/hintState")) {
                    handleHintState(id);
                } else if ((g_features)
                           && method == QLatin1String("textDocument/codeAction")) {
                    featureCodeActions(id, params.toObject());
                } else if ((g_features)
                           && method == QLatin1String("textDocument/formatting")) {
                    featureFormatting(id, params.toObject());
                } else if ((g_features)
                           && method == QLatin1String("textDocument/rename")) {
                    featureRename(id, params.toObject());
                } else if (!id.isUndefined()) {
                    if (g_mode == QLatin1String("basic")
                        || g_mode == QLatin1String("stderr"))
                        send(response(id, params)); // the echo round-trip
                    else
                        answerError(message, -32601);
                }
                // unknown NOTIFICATIONS (no id): ignored, per spec. A real
                // JSON-RPC client CANNOT answer a notification (it carries
                // no id to answer against); RpcClient's machine-1 test pins
                // the silence. The stream simply stays alive past it.
            }
        } else if (n == 0) {
            stdinReady.setEnabled(false); // Qt caveat: re-enabled notifiers
                                          // busy-loop at EOF
            // The battery may still be awaiting an answer: EOF now IS the
            // hang the watchdog would report, so report it precisely.
            if (!g_batteryDone && g_mode == QLatin1String("requests"))
                fail(QStringLiteral("EOF mid-battery"));
            _exit(0); // stdin closed by the client: honest server shutdown
        }
    });
    QCoreApplication::exec();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2)
        fail(QStringLiteral("usage: fakeserver <mode> [flags]"));
    g_mode = args.at(1);
    g_noApply = args.size() > 2 && args.at(2) == QLatin1String("noapply");
    if (g_mode == QLatin1String("sync")) {
        // argv[2] is the ENCODING in sync mode (a malformed one fails LOUD:
        // silently defaulting to utf-16 would quietly drop utf-8/utf-32
        // coverage from the suite). "crashonce"/"diags" may ride any argv
        // slot after the mode.
        const QString enc
            = args.size() > 2 ? args.at(2) : QStringLiteral("utf-16");
        if (enc == QLatin1String("utf-8") || enc == QLatin1String("utf-16")
            || enc == QLatin1String("utf-32"))
            g_syncEncoding = enc;
        else if (enc != QLatin1String("crashonce")
                 && enc != QLatin1String("diags")
                 && enc != QLatin1String("features")
                 && enc != QLatin1String("completiononly")
                 && enc != QLatin1String("wswu")
                 && enc != QLatin1String("config")
                 && enc != QLatin1String("hoveron")
                 && enc != QLatin1String("reqlog")
                 && enc != QLatin1String("cancellog")
                 && enc != QLatin1String("sighelp")
                 && enc != QLatin1String("completionon")
                 && enc != QLatin1String("foldingrange")
                 && enc != QLatin1String("foldempty")
                 && enc != QLatin1String("inlayhint")
                 && enc != QLatin1String("hintnull")
                 && !enc.startsWith(QLatin1String("foldfail="))
                 && !enc.startsWith(QLatin1String("folddelay="))
                 && !enc.startsWith(QLatin1String("hintfail="))
                 && !enc.startsWith(QLatin1String("hintmany="))
                 && !enc.startsWith(QLatin1String("hintdelay="))
                 && enc != QLatin1String("pull")
                 && enc != QLatin1String("pulldynamic")
                 && enc != QLatin1String("pullunchanged")
                 && enc != QLatin1String("pullpublish")
                 && !enc.startsWith(QLatin1String("pullfail="))
                 && !enc.startsWith(QLatin1String("pullid="))
                 && !enc.startsWith(QLatin1String("completiondelay="))
                 && !enc.startsWith(QLatin1String("sigdelay=")))
            fail(QStringLiteral("sync mode: bad encoding \"%1\"").arg(enc));
        for (int i = 2; i < args.size(); ++i) {
            if (args.at(i) == QLatin1String("crashonce"))
                g_crashOnce = true;
            else if (args.at(i) == QLatin1String("diags"))
                g_publishDiags = true;
            else if (args.at(i) == QLatin1String("diagsource"))
                g_diagSource = true; // rides diags: label every published
                                     // record with `source: "fakesrv"`
            else if (args.at(i) == QLatin1String("features"))
                g_features = true;
            else if (args.at(i) == QLatin1String("completiononly"))
                g_completionOnly = g_features = true; // rides the features
                // machinery, but the provider set is ONLY completionProvider
                // without resolveProvider — the nested gate's refusal pin
            else if (args.at(i) == QLatin1String("wswu"))
                g_wswu = true;
            else if (args.at(i) == QLatin1String("config"))
                g_configPin = true;
            else if (args.at(i) == QLatin1String("hoveron"))
                g_hoverOn = true; // rides features: hoverProvider TRUE +
                                  // a real hover answer (popup triggers)
            else if (args.at(i) == QLatin1String("reqlog"))
                g_reqLog = true;
            else if (args.at(i) == QLatin1String("cancellog"))
                g_cancelLog = true;
            else if (args.at(i) == QLatin1String("sighelp"))
                g_sigHelp = true; // GATE 3 car C (C2): the signatureHelp
                                  // provider set + the featureSignatureHelp
                                  // answer. It RIDES sync mode — the doc
                                  // mirror, the test/* echo doors and the
                                  // crashonce drill are already sync's, and
                                  // the trigger tests need all three.
            else if (args.at(i) == QLatin1String("foldingrange"))
                g_foldRange = true; // GATE 3 car C (C3): the fold authority —
                                    // foldingRangeProvider + foldReport over
                                    // this server's own text mirror. It RIDES
                                    // sync mode for the same reason sighelp
                                    // does: didOpen/didChange tracking IS the
                                    // fold answer's input, and the fold ask
                                    // must find the mirror already current.
            else if (args.at(i) == QLatin1String("foldempty"))
                g_foldEmpty = g_foldRange = true; // rides foldingrange: the
                                                  // EMPTY-array answer (the
                                                  // authority that says nothing
                                                  // is foldable)
            else if (args.at(i).startsWith(QLatin1String("foldfail="))) {
                bool ok = false;
                const int after
                    = args.at(i).mid(int(qstrlen("foldfail="))).toInt(&ok);
                if (!ok || after < 0)
                    fail(QStringLiteral("foldfail wants a non-negative number, "
                                        "got \"%1\"")
                             .arg(args.at(i)));
                g_foldFailAfter = after; // asks 1..N answer folds, the rest are
                                         // REFUSED: the claim-fell-through pin
            } else if (args.at(i).startsWith(QLatin1String("folddelay="))) {
                bool ok = false;
                const int ms
                    = args.at(i).mid(int(qstrlen("folddelay="))).toInt(&ok);
                if (!ok || ms < 0)
                    fail(QStringLiteral("folddelay wants a non-negative number, "
                                        "got \"%1\"")
                             .arg(args.at(i)));
                g_foldDelayMs = ms; // holds the foldingRange answer so a test
                                    // can close the document over an ask still
                                    // in flight
            } else if (args.at(i) == QLatin1String("inlayhint")) {
                g_inlayHint = true; // GATE 3 car C (C4): the hint authority —
                                    // inlayHintProvider + featureInlayHint
                                    // over this server's own text mirror.
                                    // RIDES sync mode like the fold flags:
                                    // the hint POSITIONS speak the synced
                                    // frame, so didChange tracking IS the
                                    // answer's input (and the flush-before-
                                    // ask rule is proved far-side by the
                                    // range the ask carries — see test/
                                    // hintState).
            } else if (args.at(i) == QLatin1String("hintnull")) {
                g_hintNull = g_inlayHint = true; // rides inlayhint: the NULL
                                                 // answer (spec "no hints" —
                                                 // the hub paints the empty
                                                 // set)
            } else if (args.at(i).startsWith(QLatin1String("hintfail="))) {
                bool ok = false;
                const int after
                    = args.at(i).mid(int(qstrlen("hintfail="))).toInt(&ok);
                if (!ok || after < 0)
                    fail(QStringLiteral("hintfail wants a non-negative number, "
                                        "got \"%1\"")
                             .arg(args.at(i)));
                g_hintFailAfter = after; // asks 1..N answer hints, the rest
                                         // are REFUSED: the last set must
                                         // STAND (folds' refused-bit shape
                                         // must NOT show up here)
            } else if (args.at(i).startsWith(QLatin1String("hintmany="))) {
                bool ok = false;
                const int n = args.at(i).mid(int(qstrlen("hintmany="))).toInt(
                    &ok);
                if (!ok || n <= 0)
                    fail(QStringLiteral("hintmany wants a positive number, got "
                                        "\"%1\"")
                             .arg(args.at(i)));
                g_hintMany = n; // N synthetic hints: the kMaxLspInlayHints
                                // cap pin rides the WIRE window
            } else if (args.at(i).startsWith(QLatin1String("hintdelay="))) {
                bool ok = false;
                const int ms
                    = args.at(i).mid(int(qstrlen("hintdelay="))).toInt(&ok);
                if (!ok || ms < 0)
                    fail(QStringLiteral("hintdelay wants a non-negative number, "
                                        "got \"%1\"")
                             .arg(args.at(i)));
                g_hintDelayMs = ms; // holds the inlayHint answer so a test
                                    // can close over an ask still in flight
            } else if (args.at(i) == QLatin1String("pull")) {
                g_pullDiag = true; // the static pull authority: see the
                                   // g_pullDiag declaration above
            } else if (args.at(i) == QLatin1String("pulldynamic")) {
                g_pullDiag = g_pullDyn = true; // rides pull: the provider is
                                               // DYNAMIC — no capability, a
                                               // registerCapability notify
                                               // after the handshake instead
            } else if (args.at(i) == QLatin1String("pullunchanged")) {
                g_pullDiag = g_pullUnchanged = true; // rides pull: kind
                                                     // "unchanged" past ask 1
            } else if (args.at(i) == QLatin1String("pullpublish")) {
                g_pullDiag = g_pullPublish = true; // rides pull: the
                                                   // contradicting push trail
            } else if (args.at(i).startsWith(QLatin1String("pullfail="))) {
                bool ok = false;
                const int after
                    = args.at(i).mid(int(qstrlen("pullfail="))).toInt(&ok);
                if (!ok || after < 0)
                    fail(QStringLiteral("pullfail wants a non-negative number, "
                                        "got \"%1\"")
                             .arg(args.at(i)));
                g_pullDiag = true;
                g_pullFailAfter = after; // asks 1..N answer reports, the rest
                                         // REFUSE: the hub must fall back to
                                         // push and stop asking
            } else if (args.at(i).startsWith(QLatin1String("pullid="))) {
                g_pullDiag = true;
                g_pullResultId
                    = args.at(i).mid(int(qstrlen("pullid="))); // the
                                            // report's resultId string
            }
            else if (args.at(i).startsWith(QLatin1String("completiondelay="))) {
                bool ok = false;
                const int ms
                    = args.at(i).mid(int(qstrlen("completiondelay="))).toInt(&ok);
                if (!ok || ms < 0)
                    fail(QStringLiteral("completiondelay wants a "
                                        "non-negative number, got \"%1\"")
                             .arg(args.at(i)));
                g_completionDelayMs = ms;
            } else if (args.at(i).startsWith(QLatin1String("sigdelay="))) {
                bool ok = false;
                const int ms
                    = args.at(i).mid(int(qstrlen("sigdelay="))).toInt(&ok);
                if (!ok || ms < 0)
                    fail(QStringLiteral("sigdelay wants a non-negative number, "
                                        "got \"%1\"").arg(args.at(i)));
                g_sigDelayMs = ms; // holds the signatureHelp answer so a test
                                   // can supersede an IN-FLIGHT ask
            }
        }
    }
    if (g_mode != QLatin1String("basic") && g_mode != QLatin1String("stderr")
        && g_mode != QLatin1String("crash") && g_mode != QLatin1String("deaf")
        && g_mode != QLatin1String("requests")
        && g_mode != QLatin1String("sync"))
        fail(QStringLiteral("unknown mode \"%1\"").arg(g_mode));

    if (g_mode == QLatin1String("stderr")) {
        // The STDERR channel fires BEFORE the handshake completes: the
        // client must show it on serverStderr, never on serverLogMessage.
        std::fprintf(stderr, "STDERR-MARKER line one\n");
        std::fflush(stderr);
    }

    // A hung test must die LOUD (exit 3 + stderr) instead of hanging ctest.
    // The budgets must stay ABOVE every legitimate test door, or a starved
    // host reads as broken (the 2026-11-21 flake: a loaded full-suite run
    // stretched lsp 35s->150s and the capability-slot server died at the
    // old 20 s while a 30 s QTRY door was still legitimately open — the
    // same philosophy as the af0ff35 timeout bumps; the DOOR budgets were
    // raised THEN — the server-side lifetime was the leftover half). The
    // ordering that must hold: deaf 60000 > the stretched 10 s initialize
    // (the deaf mode's own deadline rides that one) and the 120 s default
    // outlives every QTRY door even on a 3-4x starved host, while a truly
    // hung session still dies LOUD far inside ctest's 1500 s default.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        std::fprintf(stderr, "FAKESERVER-FAIL watchdog timeout (mode %s)\n",
                     qPrintable(g_mode));
        std::fflush(stderr);
        _exit(3);
    });
    watchdog.start(g_mode == QLatin1String("deaf") ? 60'000 : 120'000);

    runLoop();
    return 0; // never reached: every path ends in exit()/fail()
}
