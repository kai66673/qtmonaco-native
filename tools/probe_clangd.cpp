// M8 close-car ritual (PLAN, decided 2026-11-18): the live-clangd probe.
// Not a CI gate — CI has no clangd and clangd versions drift; this binary
// is the manual proof that the WHOLE stack agrees with a REAL server: the
// hub's built-in clangd spec, the lazy spawn + handshake, the syncer mirror
// against clangd's own parser, the diagnostics decode, hover/completion/
// formatting/rename replies, and since GATE 4 car I3 the three depth doors —
// definition, references, and the TENTH one (documentSymbol, the outline's
// ask: the hierarchical shape we CLAIMED is what the leg pins, with the
// symbol SET left as clangd's call) — the version-pinned applier round trip against
// clangd's REAL documentChanges (not a fakeserver echo), and the save door —
// including the machine-6 willSaveWaitUntil detour IF the live clangd
// advertises it (the observed branch is printed; both are legal).
//
// State checks only (no vision): every step prints a PASS line, the run
// ends in PROBE-OK (exit 0) or PROBE-FAIL <step> (exit 1) — teardown then
// rides the hub's stop barrier like any destructor. The build+run line lives
// in tools/probe_clangd.sh (ad-hoc g++ like tools/bench_model.cpp).
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QUrl>

#include <functional>

#include "glue/LspHub.hpp"
#include "lsp/DocumentSyncer.hpp"
#include "lsp/LspClient.hpp"
#include "protocol/RpcClient.hpp"
#include "view/CodeDocument.hpp" // document()->isModified() (forward-decl
#include "view/CodeEditor.hpp"   // in CodeEditor.hpp is not enough here)

namespace {

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}
QTextStream& err()
{
    static QTextStream s(stderr);
    return s;
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!predicate()) {
        if (clock.elapsed() > timeoutMs)
            return predicate();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return true;
}

// The reply carrier, exactly the tst's FeatureProbe shape.
struct FeatureProbe {
    bool arrived = false;
    qmn::protocol::RpcClient::Reply reply;

    qmn::glue::LspHub::FeatureHandler handler()
    {
        return [this](const qmn::protocol::RpcClient::Reply& r) {
            arrived = true;
            reply = r;
        };
    }
};

QString jsonDump(const QJsonValue& value)
{
    if (value.isArray())
        return QString::fromUtf8(
            QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    if (value.isObject())
        return QString::fromUtf8(
            QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    // Scalars: wrap in an array to stay valid JSON, then unwrap the print.
    const QJsonArray wrap{value};
    const QString printed = QString::fromUtf8(
        QJsonDocument(wrap).toJson(QJsonDocument::Compact));
    return printed.mid(1, printed.size() - 2);
}

QString dumpsDiags(const qmn::view::CodeEditor& editor)
{
    QStringList lines;
    for (const auto& d : editor.diagnostics())
        lines << QStringLiteral("%1:%2-%3:%4 [%5] %6")
                     .arg(d.startLine)
                     .arg(d.startCol)
                     .arg(d.endLine)
                     .arg(d.endCol)
                     .arg(int(d.severity))
                     .arg(d.message);
    return QStringLiteral("diagnostics: [%1]").arg(lines.join(QLatin1String(" | ")));
}

int g_step = 0;
bool step(const QString& what)
{
    out() << "PASS " << ++g_step << ": " << what << Qt::endl;
    return true;
}

bool runProbe(qmn::view::CodeEditor& editor, qmn::glue::LspHub& hub,
              const QString& workspaceRoot, const QString& uri)
{
    // The drill document: every line a deliberate reason to answer. Line 4
    // guarantees a semantic ERROR diagnostic (clangd needs none of our
    // headers: the file is self-contained); line 2/5 spacing is what
    // textDocument/formatting collapses; line 3 is a clean local (decl +
    // use) — rename's unambiguous target; line 7 is a half-typed
    // identifier — completion's natural target. NOTE (live clangd 23.1.5
    // taught this): the half-typed alphaLoc on line 7 gets clangd NAME
    // RECOVERY against the alphaLocal declaration, so renaming alphaLocal
    // hits THREE sites, not two — rename therefore drills gammaTotal, a
    // name no recovery heuristic can reach. Positions are 0-based UTF-16.
    const QString text = QStringLiteral(
        "int alphaLocal = 1;\n"
        "int gammaTotal = 7;\n"
        "int   main()\n"
        "{\n"
        "    undeclared_xyz();\n"
        "    return alphaLocal + gammaTotal +    1;\n"
        "}\n"
        "int rebind = alphaLoc\n");

    editor.setPlainText(text);
    editor.setLanguage(QStringLiteral("cpp")); // tree-sitter cpp rides too
    if (!hub.openDocument(&editor, uri, QStringLiteral("cpp")))
        return err() << "PROBE-FAIL open: no built-in spec claimed cpp\n",
               false;
    qmn::lsp::LspClient* client = hub.clientForSpec(QStringLiteral("clangd"));
    if (!client)
        return err() << "PROBE-FAIL open: the clangd spec has no client\n",
               false;
    if (!waitUntil([&] { return client->isReady(); }, 60000))
        return err() << "PROBE-FAIL handshake: clangd never reached Ready\n",
               false;
    step(QStringLiteral("handshake: clangd Ready (workspace %1)").arg(workspaceRoot));

    // Machine 4 half: publishDiagnostics decoded into the view door.
    const bool gotError = waitUntil(
        [&] {
            for (const auto& d : editor.diagnostics())
                if (d.severity == qmn::view::CodeEditor::Severity::Error
                    && d.message.contains(QStringLiteral("undeclared_xyz")))
                    return true;
            return false;
        },
        60000);
    if (!gotError)
        return err() << "PROBE-FAIL diagnostics: no Error naming undeclared_xyz. "
                     << dumpsDiags(editor) << Qt::endl,
               false;
    const auto hasErrorOnLine4 = [&] {
        for (const auto& d : editor.diagnostics())
            if (d.severity == qmn::view::CodeEditor::Severity::Error
                && d.message.contains(QStringLiteral("undeclared_xyz"))
                && d.startLine == 4)
                return true;
        return false;
    };
    if (!hasErrorOnLine4())
        return err() << "PROBE-FAIL diagnostics: the undeclared_xyz error is "
                     << "not on line 4 (frame decode drift). "
                     << dumpsDiags(editor) << Qt::endl,
               false;
    step(QStringLiteral("diagnostics: %1 decoded, the undeclared_xyz error "
                        "lands on line 4 (utf-16 frame agrees)")
             .arg(editor.diagnostics().size()));

    // Machine 5 half: the feature doors and the applier, against a server
    // that computes its own offsets. Col 12 of "    return alphaLocal ..."
    // sits inside the identifier (cols 11..20).
    {
        FeatureProbe hover;
        if (hub.requestHover(&editor, qmn::view::TextPosition(5, 12),
                             hover.handler())
            <= 0)
            return err() << "PROBE-FAIL hover: door refused locally\n", false;
        if (!waitUntil([&] { return hover.arrived; }, 20000) || hover.reply.isError)
            return err() << "PROBE-FAIL hover: "
                         << (hover.arrived ? hover.reply.message
                                           : QStringLiteral("never answered"))
                         << Qt::endl,
                   false;
        if (!jsonDump(hover.reply.result).contains(QStringLiteral("alphaLocal")))
            return err() << "PROBE-FAIL hover: reply does not name alphaLocal: "
                         << jsonDump(hover.reply.result) << Qt::endl,
                   false;
        step(QStringLiteral("hover: clangd hovers alphaLocal at (5,12)"));
    }

    {
        FeatureProbe completion;
        // (7,21) = right after the half-typed "alphaLoc" (the line is 21
        // utf-16 units long; one unit past the word is what typing gives).
        if (hub.requestCompletion(&editor, qmn::view::TextPosition(7, 21),
                                  completion.handler())
            <= 0)
            return err() << "PROBE-FAIL completion: door refused locally\n",
               false;
        if (!waitUntil([&] { return completion.arrived; }, 20000)
            || completion.reply.isError)
            return err() << "PROBE-FAIL completion: "
                         << (completion.arrived ? completion.reply.message
                                                : QStringLiteral("never answered"))
                         << Qt::endl,
                   false;
        QJsonArray items;
        if (completion.reply.result.isArray())
            items = completion.reply.result.toArray();
        else if (completion.reply.result.isObject())
            items = completion.reply.result.toObject()
                        .value(QStringLiteral("items"))
                        .toArray();
        bool namesLocal = false;
        for (const QJsonValue& item : items) {
            // clangd decorates some labels (a leading space = "insert a
            // separator"): match on the TRIMMED label, the decoration is
            // clangd's payload choice, not a frame disagreement.
            if (item.toObject().value(QStringLiteral("label")).toString().trimmed()
                == QLatin1String("alphaLocal"))
                namesLocal = true;
        }
        if (!namesLocal)
            return err() << "PROBE-FAIL completion: no item labeled alphaLocal "
                         << "(items=" << items.size() << "): "
                         << jsonDump(completion.reply.result).left(400)
                         << Qt::endl,
                   false;
        step(QStringLiteral("completion: %1 items, alphaLocal among them "
                            "(half-typed at (7,21))")
                 .arg(items.size()));
    }

    {
        FeatureProbe fmt;
        if (hub.requestFormatting(&editor, fmt.handler()) <= 0)
            return err() << "PROBE-FAIL formatting: door refused locally\n",
                   false;
        if (!waitUntil([&] { return fmt.arrived; }, 20000) || fmt.reply.isError)
            return err() << "PROBE-FAIL formatting: "
                         << (fmt.arrived ? fmt.reply.message
                                         : QStringLiteral("never answered"))
                         << Qt::endl,
                   false;
        if (!fmt.reply.result.isArray() || fmt.reply.result.toArray().isEmpty())
            return err() << "PROBE-FAIL formatting: clangd answered a messy "
                         << "document with " << jsonDump(fmt.reply.result)
                         << Qt::endl,
                   false;
        const qmn::glue::LspHub::ApplyReport applied =
            hub.applyTextEditList(&editor, fmt.reply.result, client);
        if (!applied.applied || applied.documentsChanged != 1)
            return err() << "PROBE-FAIL formatting apply: "
                         << applied.refused.join(QLatin1String("; ")) << Qt::endl,
                   false;
        if (!editor.toPlainText().contains(QStringLiteral("int main()")))
            return err() << "PROBE-FAIL formatting apply: the collapsed "
                         << "\"int main()\" never landed: "
                         << editor.toPlainText() << Qt::endl,
                   false;
        editor.undo(); // the formatting reply is ONE undo group, like every
                       // applier action — and it reverses WHOLE.
        if (editor.toPlainText() != text)
            return err() << "PROBE-FAIL formatting undo: one group did not "
                         << "restore the drill text\n",
                   false;
        step(QStringLiteral("formatting: %1 real clang-format edits applied "
                            "(%2 edits) and reversed by ONE undo")
                 .arg(fmt.reply.result.toArray().size())
                 .arg(applied.editsApplied));
    }
    {
        // GATE 4 car I3 (definition, LIVE): the caret on gammaTotal's
        // DECLARATION must answer a location naming THIS uri — both faces of
        // the reply-shape law (Location's "uri", LocationLink's "targetUri")
        // are accepted, exactly as the hub's door comment freezes them.
        // Presence-shaped on purpose: the range's exact columns are clangd's
        // call and drift across versions; naming the right DOCUMENT is not.
        FeatureProbe definition;
        if (hub.requestDefinition(&editor, qmn::view::TextPosition(1, 6),
                                 definition.handler())
            <= 0)
            return err() << "PROBE-FAIL definition: door refused locally\n",
                   false;
        if (!waitUntil([&] { return definition.arrived; }, 20000)
            || definition.reply.isError)
            return err() << "PROBE-FAIL definition: "
                         << (definition.arrived ? definition.reply.message
                                                : QStringLiteral("never answered"))
                         << Qt::endl,
                   false;
        const QJsonArray defLocations = definition.reply.result.toArray();
        if (defLocations.isEmpty())
            return err() << "PROBE-FAIL definition: clangd answered nothing at "
                            "the caret on gammaTotal: "
                         << jsonDump(definition.reply.result) << Qt::endl,
                   false;
        int ours = 0;
        for (const QJsonValue& v : defLocations) {
            const QJsonObject o = v.toObject();
            const QString target = o.contains(QStringLiteral("targetUri"))
                                       ? o.value(QStringLiteral("targetUri"))
                                             .toString()
                                       : o.value(QStringLiteral("uri")).toString();
            if (target == uri)
                ++ours;
        }
        if (ours == 0)
            return err() << "PROBE-FAIL definition: no answer names this "
                            "document: "
                         << jsonDump(definition.reply.result) << Qt::endl,
                   false;
        step(QStringLiteral("definition: %1 location(s), %2 naming this uri")
                 .arg(defLocations.size())
                 .arg(ours));
    }

    {
        // GATE 4 car I3 (references, LIVE): the SAME caret, includeDeclaration
        // TRUE (the IDE panel's law) — gammaTotal is decl + use, so AT LEAST
        // two locations on this uri (a floor, not an exact count: clangd
        // versions add cross-file sites and index noise).
        FeatureProbe references;
        if (hub.requestReferences(&editor, qmn::view::TextPosition(1, 6), true,
                                 references.handler())
            <= 0)
            return err() << "PROBE-FAIL references: door refused locally\n",
                   false;
        if (!waitUntil([&] { return references.arrived; }, 20000)
            || references.reply.isError)
            return err() << "PROBE-FAIL references: "
                         << (references.arrived ? references.reply.message
                                                : QStringLiteral("never answered"))
                         << Qt::endl,
                   false;
        const QJsonArray refLocations = references.reply.result.toArray();
        int ours = 0;
        for (const QJsonValue& v : refLocations) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("uri")).toString() == uri
                || o.value(QStringLiteral("targetUri")).toString() == uri)
                ++ours;
        }
        if (ours < 2)
            return err() << "PROBE-FAIL references: gammaTotal is decl + use, "
                            "clangd answered "
                         << ours << " site(s) on this uri of "
                         << refLocations.size() << ": "
                         << jsonDump(references.reply.result) << Qt::endl,
                   false;
        step(QStringLiteral("references: %1 location(s), %2 on this uri")
                 .arg(refLocations.size())
                 .arg(ours));
    }

    {
        // GATE 4 car I3 (the TENTH door, LIVE): textDocument/documentSymbol
        // against clangd's real parser. The shape we CLAIMED is the pin —
        // hierarchicalDocumentSymbolSupport:true means EVERY entry carries
        // name + selectionRange, so a clangd answering flat
        // SymbolInformation[] fails HERE (the fakeserver's flat leg pins the
        // app-side rebuild; this pins a conforming server at the door). The
        // symbol SET is clangd's call, so the name check is a floor: the
        // drill doc's own two decls must appear SOMEWHERE in the tree.
        FeatureProbe symbols;
        if (hub.requestDocumentSymbol(&editor, symbols.handler()) <= 0)
            return err() << "PROBE-FAIL documentSymbol: door refused locally\n",
                   false;
        if (!waitUntil([&] { return symbols.arrived; }, 20000)
            || symbols.reply.isError)
            return err() << "PROBE-FAIL documentSymbol: "
                         << (symbols.arrived ? symbols.reply.message
                                             : QStringLiteral("never answered"))
                         << Qt::endl,
                   false;
        if (!symbols.reply.result.isArray()
            || symbols.reply.result.toArray().isEmpty())
            return err() << "PROBE-FAIL documentSymbol: clangd answered "
                         << jsonDump(symbols.reply.result) << Qt::endl,
                   false;
        int count = 0;
        bool hierarchical = true;
        QStringList names;
        std::function<void(const QJsonArray&)> walk = [&](const QJsonArray& arr) {
            for (const QJsonValue& v : arr) {
                const QJsonObject obj = v.toObject();
                ++count;
                if (!obj.contains(QStringLiteral("selectionRange")))
                    hierarchical = false;
                names << obj.value(QStringLiteral("name")).toString();
                walk(obj.value(QStringLiteral("children")).toArray());
            }
        };
        walk(symbols.reply.result.toArray());
        if (!hierarchical)
            return err() << "PROBE-FAIL documentSymbol: the answer is NOT the "
                            "hierarchical shape we claimed (an entry without "
                            "selectionRange): "
                         << jsonDump(symbols.reply.result) << Qt::endl,
                   false;
        for (const QString& wanted :
             {QStringLiteral("main"), QStringLiteral("alphaLocal")})
            if (!names.contains(wanted))
                return err() << "PROBE-FAIL documentSymbol: clangd's tree never "
                                "names \""
                             << wanted << "\"; names were: "
                             << names.join(QStringLiteral(", ")) << Qt::endl,
                       false;
        step(QStringLiteral("documentSymbol: clangd's real tree, %1 entries, "
                            "hierarchical, naming main + alphaLocal")
                 .arg(count));
    }

    {
        // The version pin against a REAL server: clangd pins the document
        // version IT parsed; the applier must agree with the syncer's
        // number, not with the fakeserver's echo.
        FeatureProbe rename;
        // gammaTotal (decl at (1,4..13), use at (5,25..34)) — a name clangd
        // name recovery cannot fuse to anything else (see the drill-doc
        // note): decl + use, exactly two sites.
        if (hub.requestRename(&editor, qmn::view::TextPosition(1, 6),
                              QStringLiteral("deltaTotal"), rename.handler())
            <= 0)
            return err() << "PROBE-FAIL rename: door refused locally\n", false;
        if (!waitUntil([&] { return rename.arrived; }, 20000)
            || rename.reply.isError)
            return err() << "PROBE-FAIL rename: "
                         << (rename.arrived ? rename.reply.message
                                            : QStringLiteral("never answered"))
                         << Qt::endl,
                   false;
        const qmn::glue::LspHub::ApplyReport applied =
            hub.applyReplyEdit(rename.reply.result, client);
        if (!applied.applied || applied.documentsChanged != 1)
            return err() << "PROBE-FAIL rename apply (version pin drift?): "
                         << applied.refused.join(QLatin1String("; "))
                         << " | edit=" << jsonDump(rename.reply.result)
                         << Qt::endl,
                   false;
        const QString renamed = editor.toPlainText();
        if (renamed.count(QStringLiteral("deltaTotal")) != 2
            || renamed.contains(QStringLiteral("gammaTotal")))
            return err() << "PROBE-FAIL rename apply: expected deltaTotal "
                         << "twice (decl + use) and no gammaTotal left, got "
                         << renamed.count("deltaTotal") << ":\n"
                         << renamed << Qt::endl,
                   false;
        editor.undo();
        if (editor.toPlainText() != text)
            return err() << "PROBE-FAIL rename undo: one group did not "
                         << "restore the drill text\n",
                   false;
        step(QStringLiteral("rename: clangd's version-pinned documentChanges "
                            "applied (%1 edits, version %2 agreed), reversed "
                            "by ONE undo")
                 .arg(applied.editsApplied)
                 .arg(hub.syncerFor(&editor)->version()));
    }

    // Machine 6 half: the save door against a real capability table. What
    // clangd really advertises decides the path; BOTH are printed honestly —
    // the probe pins the agreement, not a guessed capability.
    {
        // Dirty the document through a PUBLIC door (insertTextAtCaret is the
        // editor's internal typing seam, not API): one insert = one edit =
        // one undo group, and the syncer mirror rides editApplied.
        editor.insertTextAt(qmn::view::TextPosition(8, 0),
                            QStringLiteral("// save marker\n"));
        const QJsonValue syncCap =
            client->serverCapabilities().value(QStringLiteral("textDocumentSync"));
        const bool detourOffered = syncCap.isObject()
            && syncCap.toObject().value(QStringLiteral("willSaveWaitUntil")).toBool();
        out() << "clangd textDocumentSync: " << jsonDump(syncCap) << Qt::endl;
        hub.saveDocument(&editor);
        const bool synchronous = !editor.document()->isModified();
        if (detourOffered && synchronous)
            return err() << "PROBE-FAIL save: clangd advertised "
                         << "willSaveWaitUntil but the save ran synchronously "
                         << "— the capability gate ignored the real table\n",
                   false;
        if (!detourOffered && !synchronous)
            return err() << "PROBE-FAIL save: no willSaveWaitUntil advertised, "
                         << "yet the save went async — the gate invented a "
                         << "detour\n",
                   false;
        if (!waitUntil([&] { return !editor.document()->isModified(); }, 15000))
            return err() << "PROBE-FAIL save: isModified never cleared\n", false;
        editor.undo(); // drop the marker again for the teardown to sync
        hub.syncerFor(&editor)->flush();
        step(QStringLiteral("save: %1 path (observed), markSaved agreed, "
                            "didSave survived a real clangd")
                 .arg(detourOffered ? QStringLiteral("willSaveWaitUntil detour")
                                    : QStringLiteral("plain synchronous")));
    }

    // ---- GATE 3 car C (M8b) legs: signatureHelp, foldingRange, inlay
    // hints and the diagnostic authority, against the SAME live clangd.
    // The betaFn/betaCaller block lands at line 7 — BEFORE the drill's
    // half-typed rebind line — for a reason live clangd 23.1.5 taught the
    // hard way (2026-11-22): at cpp, the unterminated `int rebind =
    // alphaLoc` swallows the next line's `void betaFn(...);` declaration
    // into its error recovery, so the call has no REAL declaration —
    // hover still answers via clangd's name recovery, but signatureHelp
    // answers EMPTY signatures at every position. With the block before
    // the broken line, both trigger kinds answer fully. The block also
    // re-arms the edit-edge feeds (foldingRange, inlayHint, pull) exactly
    // like a user's typing. As in the save leg, what clangd REALLY
    // advertises decides each branch; both readings print honestly.
    {
        editor.insertTextAt(qmn::view::TextPosition(7, 0),
                            QStringLiteral("void betaFn(int firstArg, "
                                           "int secondArg);\n"
                                           "void betaCaller(void) {\n"
                                           "    int betaUse = betaFn(1, 2);\n"
                                           "}\n"));
        hub.syncerFor(&editor)->flush(); // THE app-side frame rule (M8
                                         // convention): the explicit feature
                                         // doors do NOT flush the syncer —
                                         // the app controls its own typing
                                         // rhythm. Live clangd proved it the
                                         // hard way: un-flushed, it parsed
                                         // the OLD document and answered
                                         // empty signatures at the fresh
                                         // call position. The hub's OWN
                                         // auto feeds
                                         // (fold/hint/pull/sighelp triggers)
                                         // all flush at ask time — that
                                         // half is hub-side, pinned in tst.
        const QJsonObject caps = client->serverCapabilities();

        // C2: the signatureHelp door. (9,27) sits right after the ',' of
        // the betaCaller call — TriggerForCharacter riding clangd's own
        // comma.
        {
            FeatureProbe sig;
            if (hub.requestSignatureHelp(
                    &editor, qmn::view::TextPosition(9, 27), sig.handler(),
                    QStringLiteral(","))
                <= 0)
                return err()
                       << "PROBE-FAIL signatureHelp: door refused locally\n",
                       false;
            if (!waitUntil([&] { return sig.arrived; }, 20000)
                || sig.reply.isError)
                return err()
                       << "PROBE-FAIL signatureHelp: "
                       << (sig.arrived ? sig.reply.message
                                       : QStringLiteral("never answered"))
                       << Qt::endl,
                       false;
            if (!jsonDump(sig.reply.result).contains(QStringLiteral("betaFn")))
                return err()
                       << "PROBE-FAIL signatureHelp: the answer does not name "
                          "betaFn (and the block IS parsed now — hover the "
                          "call to check): "
                       << jsonDump(sig.reply.result).left(400) << Qt::endl,
                       false;
            step(QStringLiteral("signatureHelp: clangd names betaFn and its "
                                "two params at the typed-comma position "
                                "(9,27) inside betaCaller, triggerCharacter "
                                "comma rode the context; the FIRST attempt "
                                "answered empty because the broken rebind "
                                "line swallowed the betaFn declaration via "
                                "name recovery — the block now lands before "
                                "it"));
        }

        // C3: the fold feed. An advertised provider makes clangd the fold
        // AUTHORITY from its first answer — the fed bit is the agreement
        // proof, and the ranges must actually cover main()'s body (the
        // drill's brace block lives on lines 2..6).
        if (caps.contains(QStringLiteral("foldingRangeProvider"))) {
            if (!waitUntil([&] { return hub.foldsFedFromServer(&editor); },
                           20000))
                return err()
                       << "PROBE-FAIL foldingRange: clangd advertised "
                          "foldingRangeProvider but the fold feed never "
                          "fed the door\n",
                       false;
            const QVector<qmn::view::CodeEditor::FoldRange> folds
                = editor.foldableRanges();
            QStringList shown;
            bool coversMain = false;
            for (const qmn::view::CodeEditor::FoldRange& f : folds) {
                shown << QStringLiteral("%1..%2").arg(f.startLine)
                                                      .arg(f.endLine);
                if (f.startLine <= 3 && f.endLine >= 5) // main()'s brace
                    coversMain = true;                  // fold, NOT the
                                                        // betaCaller one
            }
            if (!coversMain)
                return err() << "PROBE-FAIL foldingRange: no SERVER fold "
                                "covers main()'s body (a range must sit on "
                                "2..6); server ranges: ["
                             << shown.join(QLatin1String(", "))
                             << "]\n",
                       false;
            step(QStringLiteral("foldingRange: %1 server folds installed "
                                "(grammar @fold silenced by the C1 seam), "
                                "main()'s body among them: [%2]")
                     .arg(folds.size())
                     .arg(shown.join(QLatin1String(", "))));
        } else {
            step(QStringLiteral("foldingRange: clangd advertises NO "
                                "foldingRangeProvider (observed) — the "
                                "grammar keeps fold authority, the hub "
                                "asks NOTHING"));
        }

        // C4: the inlay-hint feed through the already-bound M5 door. The
        // betaFn(1, call is the drill: a DEFAULT-ON clangd paints the
        // parameter NAME before the argument.
        if (caps.contains(QStringLiteral("inlayHintProvider"))
            || client->isRegistered(QStringLiteral("textDocument/inlayHint"))) {
            const bool gotParamHint = waitUntil(
                [&] {
                    for (const qmn::view::CodeEditor::InlayHint& h
                         : editor.inlayHints())
                        if (h.text.contains(QStringLiteral("firstArg")))
                            return true;
                    return false;
                },
                30000);
            if (!gotParamHint) {
                QStringList shown;
                for (const qmn::view::CodeEditor::InlayHint& h
                     : editor.inlayHints())
                    shown << QStringLiteral("%1:%2[%3]")
                                 .arg(h.line)
                                 .arg(h.col)
                                 .arg(h.text);
                return err() << "PROBE-FAIL inlayHint: provider live and a "
                                "betaFn(1, call typed, yet no firstArg "
                                "param-name hint ever landed; painted: ["
                             << shown.join(QLatin1String(", ")) << "]\n",
                       false;
            }
            step(QStringLiteral("inlayHint: %1 hints on the door, the "
                                "firstArg param-name hint proves clangd's "
                                "answer decoded through the M5 feed (zero "
                                "new view API)")
                     .arg(editor.inlayHints().size()));
        } else {
            step(QStringLiteral("inlayHint: neither advertised nor registered "
                                "(observed) — no hint feed expected, the "
                                "app's own table (if any) stands"));
        }

        // C5: the diagnostic authority. One more public-door edit (closing
        // the call) rides the 600 ms debounce — the open/READY edges long
        // passed. Advertised/registered pull: the PULL generation must
        // paint the decoded report (and the push route DROPS behind it);
        // no authority: the pre-C5 push stance stands untouched and the
        // fed bit stays false forever.
        const bool pullLive = caps.contains(QStringLiteral("diagnosticProvider"))
            || client->isRegistered(QStringLiteral("textDocument/diagnostic"));
        editor.insertTextAt(qmn::view::TextPosition(12, 0),
                            QStringLiteral("// pull-edge drill\n")); // one
                                                  // more edit edge for the
                                                  // 600 ms debounce
        if (pullLive) {
            const bool pullPainted = waitUntil(
                [&] { return hub.diagnosticsFedFromServer(&editor)
                             && hasErrorOnLine4(); },
                20000);
            if (!pullPainted)
                return err()
                       << "PROBE-FAIL pull diagnostics: clangd claims the "
                          "provider, yet the PULL generation never painted "
                          "the decoded report. "
                       << dumpsDiags(editor) << Qt::endl,
                       false;
            step(QStringLiteral("pull diagnostics: clangd holds the "
                                "authority — textDocument/diagnostic "
                                "answered, %1 marks decoded by the ONE "
                                "shared decoder, fed bit true, push "
                                "suppressed")
                     .arg(editor.diagnostics().size()));
        } else {
            waitUntil([] { return false; }, 1500); // a 1500 ms settle past
                                                   // the pull debounce: an
                                                   // ask WOULD have painted
                                                   // the fed bit by now
            if (hub.diagnosticsFedFromServer(&editor))
                return err()
                       << "PROBE-FAIL pull diagnostics: NO provider was "
                          "ever claimed, yet a PULL generation painted — "
                          "the gate invented an authority\n",
                       false;
            if (!hasErrorOnLine4())
                return err()
                       << "PROBE-FAIL pull diagnostics: the push route's "
                          "marks vanished without any pull authority to "
                          "replace them. "
                       << dumpsDiags(editor) << Qt::endl,
                       false;
            step(QStringLiteral("pull diagnostics: clangd claims NO "
                                "diagnosticProvider (observed) — the push "
                                "route holds the marks, fed bit false, zero "
                                "asks (the pre-C5 stance, untouched)"));
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");

    // The version line is part of the ritual record (the CHANGELOG entry
    // cites it): the probe proves agreement with THIS clangd.
    QProcess versionProbe;
    versionProbe.start(QStringLiteral("clangd"), {QStringLiteral("--version")});
    if (!versionProbe.waitForFinished(10000)) {
        err() << "PROBE-FAIL clangd: not on PATH (or hung on --version)\n";
        return 1;
    }
    const QString versionLine = QString::fromUtf8(
                                    versionProbe.readAllStandardOutput())
                                    .trimmed()
                                    .split(QLatin1Char('\n'))
                                    .value(0);
    out() << "clangd version: " << versionLine << Qt::endl;

    QApplication app(argc, argv);

    QTemporaryDir workspace; // the file lives inside: clangd's CDB probe
    if (!workspace.isValid()) {
        err() << "PROBE-FAIL setup: no temporary workspace\n";
        return 1;
    }
    const QString filePath = workspace.filePath(QStringLiteral("probe.cpp"));
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        err() << "PROBE-FAIL setup: cannot write " << filePath << "\n";
        return 1;
    }
    file.write("// live clangd probe document (self-contained on purpose)\n");
    file.close();

    qmn::view::CodeEditor editor; // declared BEFORE the hub: the hub's
    qmn::glue::LspHub hub;         // destructor says didClose while alive
    hub.setWorkspaceRoot(workspace.path());

    const bool ok = runProbe(editor, hub, workspace.path(),
                             QUrl::fromLocalFile(filePath).toString());
    // Teardown rides the scope: ~LspHub = didClose + shutdown/exit + the
    // bounded stop barrier. QProcess noise here would be the barrier lying.
    if (ok)
        out() << "PROBE-OK (" << g_step << " steps, clangd " << versionLine
              << ")" << Qt::endl;
    return ok ? 0 : 1;
}
