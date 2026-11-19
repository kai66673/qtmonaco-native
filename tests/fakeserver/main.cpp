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
//            decoding joins with machine 6's popups.
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
                             // didOpen/didChange of an open document
bool g_features = false;     // argv flag (rides sync): the machine-5 provider
                             // set + feature-request answers + the
                             // server-primed write path
bool g_completionOnly = false; // argv flag (rides features): advertise ONLY
                               // completionProvider, WITHOUT resolveProvider:
                               // the nested capability gate's refusal pin
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

void answerError(const QJsonObject& request, int code)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), QStringLiteral("server-side check"));
    QJsonObject m;
    m.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    m.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
    m.insert(QStringLiteral("error"), error);
    send(m);
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
        QJsonArray registrations;
        registrations.append(unreg);
        steps.push_back(
            {QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 9007},
                         {QStringLiteral("method"),
                          QStringLiteral("client/unregisterCapability")},
                         {QStringLiteral("params"),
                          QJsonObject{{QStringLiteral("registrations"),
                                       registrations}}}},
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
                    diags.append(QJsonObject{
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
                             .arg(line)}});
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
    if (triggerKind == 2 && triggerCharacter != QLatin1String("."))
        fail(QStringLiteral("completion triggered by \"%1\": the only "
                            "advertised triggerCharacters is [\".\"]")
                 .arg(triggerCharacter));
    // The proof rides LEGAL fields: labels this test pins verbatim, so the
    // frame the hub built is visible in the reply itself.
    QJsonArray items;
    items.append(QJsonObject{
        {QStringLiteral("label"), QStringLiteral("alpha")},
        {QStringLiteral("kind"), 14}, // Variable: the kind table rides along
        {QStringLiteral("data"),
         QJsonObject{{QStringLiteral("origin"), QStringLiteral("list")}}}});
    items.append(QJsonObject{{QStringLiteral("label"), QStringLiteral("tk=")
                                                               + QString::number(triggerKind)}});
    if (triggerKind == 2)
        items.append(QJsonObject{{QStringLiteral("label"),
                                  QStringLiteral("tc=") + triggerCharacter}});
    items.append(QJsonObject{{QStringLiteral("label"),
                              QStringLiteral("off=") + QString::number(offset)}});
    send(response(id, QJsonObject{{QStringLiteral("isIncomplete"), false},
                                  {QStringLiteral("items"), items}}));
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

void featureHover(const QJsonValue& /*id*/, const QJsonValue& /*params*/)
{
    // hoverProvider is advertised FALSE in features mode: the hub's gate must
    // refuse this door LOCALLY. Traffic reaching this handler is a gate bug.
    fail(QStringLiteral("textDocument/hover reached the server although "
                        "hoverProvider is false"));
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
        // window/showMessageRequest: the tst answers the FIRST variant
        // through the hub relay; the other variant stays silent and the
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
    if (g_completionOnly) {
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
    } else if (g_features) {
        // The machine-5 provider set. One deliberately OFF shape rides it:
        // hoverProvider FALSE (the hub must refuse hover locally; a hover
        // request that reaches this server exits 2 below). Everything else
        // is fully advertised so a missing capability can never mask a
        // frame bug.
        g_advertisesResolve = true;
        capabilities.insert(QStringLiteral("hoverProvider"), false);
        capabilities.insert(QStringLiteral("definitionProvider"),
                            QJsonObject{}); // object shape: the truthy-door pin
        capabilities.insert(QStringLiteral("referencesProvider"), true);
        QJsonObject completion;
        completion.insert(QStringLiteral("triggerCharacters"),
                          QJsonArray{QStringLiteral(".")});
        completion.insert(QStringLiteral("resolveProvider"), true);
        capabilities.insert(QStringLiteral("completionProvider"), completion);
        capabilities.insert(QStringLiteral("codeActionProvider"), true);
        capabilities.insert(QStringLiteral("documentFormattingProvider"), true);
        capabilities.insert(QStringLiteral("renameProvider"),
                            QJsonObject{{QStringLiteral("prepareProvider"),
                                         false}});
    } else {
        capabilities.insert(QStringLiteral("hoverProvider"), true);
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
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("test/assertModel")) {
                    handleAssertModel(id, params);
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("test/documentCount")) {
                    handleDocumentCount(id);
                } else if (g_mode == QLatin1String("sync")
                           && method == QLatin1String("test/initializeParams")) {
                    handleInitializeParams(id);
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
                } else if ((g_features)
                           && method == QLatin1String("textDocument/completion")) {
                    featureCompletion(id, params.toObject());
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
                 && enc != QLatin1String("completiononly"))
            fail(QStringLiteral("sync mode: bad encoding \"%1\"").arg(enc));
        for (int i = 2; i < args.size(); ++i) {
            if (args.at(i) == QLatin1String("crashonce"))
                g_crashOnce = true;
            else if (args.at(i) == QLatin1String("diags"))
                g_publishDiags = true;
            else if (args.at(i) == QLatin1String("features"))
                g_features = true;
            else if (args.at(i) == QLatin1String("completiononly"))
                g_completionOnly = g_features = true; // rides the features
                // machinery, but the provider set is ONLY completionProvider
                // without resolveProvider — the nested gate's refusal pin
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
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        std::fprintf(stderr, "FAKESERVER-FAIL watchdog timeout (mode %s)\n",
                     qPrintable(g_mode));
        std::fflush(stderr);
        _exit(3);
    });
    watchdog.start(g_mode == QLatin1String("deaf") ? 25'000 : 20'000);

    runLoop();
    return 0; // never reached: every path ends in exit()/fail()
}
