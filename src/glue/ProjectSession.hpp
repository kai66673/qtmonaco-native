#pragma once

// G2 of the glue shelf (PLAN "The glue shelf + ProjectSession", decided
// 2026-11-21): the project-level DOCUMENT SESSION — the registry of open
// documents (uri → editor + languageId) that used to live three times over
// (the demo's tabs, the spike's map, every consumer's own) plus the two
// decisions that belong beside it: suffix→language resolution and the
// .qmn/lsp.json config fold (the hub only ever saw `servers`; the top-level
// `languages` map was nobody's). Scope is DELIBERATELY the session + hub:
// NO disk I/O beyond reading that one config file, NO file watching, NO
// workspace folders (PLAN boundary, user-decided).
//
// The session NEVER OWNS editors (it observes destroyed(), the hub's
// auto-unregister precedent) and never shows UI — a QTabWidget is a
// presentation detail, and the documentOpened/documentClosed signals are
// exactly the seam a tab strip rides. The hub is NOT owned either:
// attachHub()/hub() — one hub per session, attached BEFORE the opens (the
// config REPLAY on a late attach is the documented exception, below).

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QStringList>

namespace qmn::view {
class CodeEditor;
class CodeDocument; // D5: uriForDocument's key (forward decl only — the
                    // door never needs the type's guts here)
} // namespace qmn::view

namespace qmn::glue {

class LspHub;

class ProjectSession : public QObject {
    Q_OBJECT
public:
    explicit ProjectSession(QObject* parent = nullptr);

    // --- the hub (NOT owned) ---------------------------------------------
    // Attaching pushes the session's workspace root into the hub and
    // REPLAYS the last config body the session read (a hub armed after
    // openPath/config still gets its `servers` — the demo's --lsp timing
    // exactly). Documents opened BEFORE an attach stay session-only: the
    // session never retro-opens (a lazy server spawn behind someone's back
    // is not a session's call). A hub dying while attached simply
    // unattaches (destroyed()); nothing in the session dies with it.
    void attachHub(LspHub* hub);
    LspHub* hub() const { return hub_; }

    // --- workspace root + config -----------------------------------------
    // One root, both sides: setWorkspaceRoot forwards to an attached hub
    // (the ${WORKSPACE_ROOT} expansion lives hub-side), and loadConfig()
    // reads <root>/.qmn/lsp.json EXACTLY like the hub's own door (same
    // path, malformed JSON refuses the WHOLE file) — then folds the
    // top-level "languages" {suffix: id} map here and hands the whole
    // body to an attached LspHub::loadConfigFromJson (the hub reads only
    // "servers"). One read feeds both; the parsed body is remembered for
    // the late-attach replay above.
    void setWorkspaceRoot(const QString& absolutePath);
    QString workspaceRoot() const { return workspaceRoot_; }
    bool loadConfig(); // false: no root, no file, or a malformed file
    bool loadConfigFromJson(const QJsonObject& root); // the body door

    // --- language resolution ---------------------------------------------
    // The table is seeded with the spike/demo suffix map (py, json,
    // js/mjs/cjs, c, h/cpp/cc/cxx/hpp/hh/hxx — .h resolves to cpp, this
    // widget's home is C++/Qt) and mutates through BOTH
    // setLanguageOverride and the config "languages" fold: an EMPTY id
    // REMOVES the mapping (a .h/.txt file honestly stays plain text).
    // Case- and leading-dot-insensitive on lookup and on the override key.
    QString languageForSuffix(const QString& suffix) const;
    void setLanguageOverride(const QString& suffix, const QString& languageId);

    // --- documents --------------------------------------------------------
    // The whole open in one call: the uriOrPath may be a file:// URI or a
    // plain path (a path normalizes to QUrl::fromLocalFile — the hub's uri
    // frame); resolution picks the language (languageOverride wins over
    // the suffix table) and ONLY then calls CodeEditor::setLanguage — an
    // UNresolved suffix never touches the editor's language (the user's
    // own setLanguage stays honored; an unmapped file simply stays what it
    // was). Registration is session-side ALWAYS; an attached hub then
    // serves the languages its specs claim (its documented honest false
    // no-op for the rest), and with popupsOnOpen (the default, the M8
    // popup-trigger demo path) the triggers arm on a hub-ACCEPTED open.
    // False: null editor/empty uri, or the uri is ALREADY registered to a
    // DIFFERENT DOCUMENT (one document per uri — the LSP didOpen-twice
    // rule, pinned).
    //
    // Car D (D5) ALIAS RULE — one entry per uri, EVERY view of the
    // registered DOCUMENT shares it:
    //   - the SAME editor re-opening the same uri: idempotent true;
    //   - a DIFFERENT editor whose document() IS the registered document
    //     (an attached second view, car D's ctor): an ALIAS JOIN — the
    //     entry's view list grows, the hub gets the same uri (its own VIEW
    //     JOIN: no second didOpen, one edit stream), the popup triggers arm
    //     for the new view — and documentOpened does NOT re-fire (the
    //     DOCUMENT opened exactly once; openCount/openUris stay one entry
    //     per uri);
    //   - a different DOCUMENT claiming the uri: the honest false (the
    //     squatter refusal, pinned);
    //   - an editor whose document is ALREADY registered under ANOTHER
    //     uri: refused — one document, one uri (the mirror half of the
    //     rule; a second uri would open a second syncer on one document).
    // The OWNER view answers editorForUri; the OWNER's death hands the
    // entry over to the next surviving view, and the LAST view's death (or
    // closeDocument of ANY view) drops the entry and closes the document.
    bool openDocument(qmn::view::CodeEditor* editor, const QString& uriOrPath,
                      const QString& languageOverride = QString());
    void closeDocument(qmn::view::CodeEditor* editor); // hub didClose +
                                                       // release of the
                                                       // DOCUMENT — any
                                                       // view's door
                                                       // closes it (D5)
    bool closeUri(const QString& uri); // false: nothing registered under it
    int openCount() const { return order_.size(); }
    QStringList openUris() const { return order_; } // insertion order
    QString languageForUri(const QString& uri) const; // "" = unknown uri or plain text
    qmn::view::CodeEditor* editorForUri(const QString& uri) const;
    // Car E (E1): the SIBLING door — EVERY live view of the registered
    // document, OWNER FIRST (views.first() IS the owner: the handover rule
    // re-keys it), attach order. The DAP feed fan-out rides it (DapHub's
    // alias paint); the LSP hub keeps its own view list (its JOIN primed
    // the didOpen wire — the door shapes differ on purpose). Empty list:
    // nothing registered under the uri. The same uri normalization as
    // editorForUri.
    QList<qmn::view::CodeEditor*> editorsForUri(const QString& uri) const;
    // Linear scan — a session holds tens of documents, not a piece tree.
    QString uriForEditor(const qmn::view::CodeEditor* editor) const;

    // Arm the M8 popup triggers (completion on typing, hover on idle) on
    // every hub-served open; ON by default, the honest off-switch for
    // apps that drive the popups themselves.
    void setPopupsOnOpen(bool on) { popupsOnOpen_ = on; }
    bool popupsOnOpen() const { return popupsOnOpen_; }

signals:
    void documentOpened(const QString& uri);
    void documentClosed(const QString& uri);

private:
    struct Entry {
        // Car D (D5): the document registry, not the editor registry —
        // `editor` is the OWNER view (views.first(), handover rule), and
        // `views` carries EVERY live view over the one document (never a
        // dead pointer: each view's destroyed() relay removes its entry,
        // pointer comparison only).
        qmn::view::CodeEditor* editor = nullptr;
        QList<qmn::view::CodeEditor*> views;
        QString languageId;
    };

    static QString normalizeUri(const QString& uriOrPath);
    static QString normalizeSuffix(const QString& suffix);
    static QString suffixForUri(const QString& uri);
    QJsonObject readConfigFile() const; // {} = no root/no file/malformed
    void dropUri(const QString& uri); // erases + emits; never touches the hub
    void onViewDestroyed(const QString& uri,
                         qmn::view::CodeEditor* view); // D5: remove the
                                                       // view; owner death
                                                       // HANDS OVER, the
                                                       // last view's death
                                                       // drops the entry
    QString uriForDocument(const qmn::view::CodeDocument* document)
        const; // D5: the uri ANY view of this document shares

    LspHub* hub_ = nullptr;
    QString workspaceRoot_;
    QHash<QString, Entry> docs_;
    QStringList order_;                        // insertion order of the uris
    QHash<QString, QString> suffixToLanguage_; // seeded table (config + overrides mutate it)
    QJsonObject lastConfigRoot_;               // the replay body for a late attach
    bool popupsOnOpen_ = true;
};

} // namespace qmn::glue
