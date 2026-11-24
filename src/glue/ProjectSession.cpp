#include "ProjectSession.hpp"

#include "LspHub.hpp"

#include "view/CodeDocument.hpp" // D5: document()->identity for the alias rule
#include "view/CodeEditor.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUrl>

namespace qmn::glue {

ProjectSession::ProjectSession(QObject* parent)
    : QObject(parent)
{
    // The spike/demo suffix map, verbatim (PLAN: the session takes it over
    // from the copies; the spike KEEPS its own — it is a bench harness and
    // stays qmn_view-only). .h resolves to cpp: this widget's home is
    // C++/Qt IDEs, and the cpp grammar parses plain C headers near
    // identically. An unmapped suffix simply stays plain text.
    suffixToLanguage_ = {
        {QStringLiteral("py"), QStringLiteral("python")},
        {QStringLiteral("json"), QStringLiteral("json")},
        {QStringLiteral("js"), QStringLiteral("javascript")},
        {QStringLiteral("mjs"), QStringLiteral("javascript")},
        {QStringLiteral("cjs"), QStringLiteral("javascript")},
        {QStringLiteral("c"), QStringLiteral("c")},
        {QStringLiteral("h"), QStringLiteral("cpp")},
        {QStringLiteral("cpp"), QStringLiteral("cpp")},
        {QStringLiteral("cc"), QStringLiteral("cpp")},
        {QStringLiteral("cxx"), QStringLiteral("cpp")},
        {QStringLiteral("hpp"), QStringLiteral("cpp")},
        {QStringLiteral("hh"), QStringLiteral("cpp")},
        {QStringLiteral("hxx"), QStringLiteral("cpp")},
    };
}

// ------------------------------------------------------------------- hub
void ProjectSession::attachHub(LspHub* hub)
{
    if (hub_ == hub)
        return;
    if (hub_)
        disconnect(hub_, nullptr, this, nullptr);
    hub_ = hub;
    if (!hub_)
        return;
    // A hub dying mid-session just unattaches (its own ~LspHub already
    // didClosed every document it served).
    connect(hub_, &QObject::destroyed, this, [this] { hub_ = nullptr; });
    if (!workspaceRoot_.isEmpty())
        hub_->setWorkspaceRoot(workspaceRoot_);
    // The late-attach replay (the --lsp-after-openPath path): the hub has
    // never seen the body the session folded, so hand it the cached one.
    // The languages re-fold is idempotent; a hub that never had config
    // simply keeps its built-ins.
    if (!lastConfigRoot_.isEmpty())
        hub_->loadConfigFromJson(lastConfigRoot_);
}

// ------------------------------------------------- workspace root + config
void ProjectSession::setWorkspaceRoot(const QString& absolutePath)
{
    workspaceRoot_ = absolutePath;
    if (hub_)
        hub_->setWorkspaceRoot(absolutePath);
}

QJsonObject ProjectSession::readConfigFile() const
{
    if (workspaceRoot_.isEmpty())
        return {};
    QFile file(QDir::cleanPath( // the hub's path, verbatim (one config file)
        workspaceRoot_ + QStringLiteral("/.qmn/lsp.json")));
    if (!file.open(QIODevice::ReadOnly))
        return {}; // no config: silent, the tables stand
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return {}; // malformed: refuse the WHOLE file (the hub's rule too)
    return doc.object();
}

bool ProjectSession::loadConfig()
{
    const QJsonObject root = readConfigFile();
    if (root.isEmpty())
        return false;
    return loadConfigFromJson(root);
}

bool ProjectSession::loadConfigFromJson(const QJsonObject& root)
{
    if (root.isEmpty())
        return false;
    lastConfigRoot_ = root; // the late-attach replay body
    bool folded = false;
    const QJsonObject languages = root.value(QStringLiteral("languages")).toObject();
    for (auto it = languages.begin(); it != languages.end(); ++it) {
        const QString suffix = normalizeSuffix(it.key());
        if (suffix.isEmpty())
            continue;
        const QString id = it.value().toString();
        if (id.isEmpty())
            suffixToLanguage_.remove(suffix); // the honest unmapping
        else
            suffixToLanguage_[suffix] = id;
        folded = true;
    }
    if (hub_ && !root.value(QStringLiteral("servers")).toObject().isEmpty()) {
        hub_->loadConfigFromJson(root); // the hub reads only "servers"
        folded = true;
    }
    return folded;
}

// ----------------------------------------------------- language resolution
QString ProjectSession::normalizeSuffix(const QString& suffix)
{
    QString key = suffix.toLower();
    while (key.startsWith(QLatin1Char('.'))) // ".py" is the same key as "py"
        key.remove(0, 1);
    return key;
}

QString ProjectSession::languageForSuffix(const QString& suffix) const
{
    return suffixToLanguage_.value(normalizeSuffix(suffix));
}

void ProjectSession::setLanguageOverride(const QString& suffix,
                                         const QString& languageId)
{
    const QString key = normalizeSuffix(suffix);
    if (key.isEmpty())
        return;
    if (languageId.isEmpty())
        suffixToLanguage_.remove(key);
    else
        suffixToLanguage_[key] = languageId;
}

// ---------------------------------------------------------------- documents
QString ProjectSession::normalizeUri(const QString& uriOrPath)
{
    // A scheme already present (file://, or a foreign uri we just pass
    // through) stays as given — the hub speaks uris, and rewriting a
    // foreign scheme is nobody's business but its owner's.
    if (uriOrPath.contains(QLatin1String("://")))
        return uriOrPath;
    return QUrl::fromLocalFile(QFileInfo(uriOrPath).absoluteFilePath()).toString();
}

QString ProjectSession::suffixForUri(const QString& uri)
{
    const QString path = uri.startsWith(QLatin1String("file://"))
                             ? QUrl(uri).toLocalFile()
                             : uri;
    return QFileInfo(path).suffix();
}

bool ProjectSession::openDocument(qmn::view::CodeEditor* editor,
                                  const QString& uriOrPath,
                                  const QString& languageOverride)
{
    if (!editor || uriOrPath.isEmpty())
        return false;
    const QString uri = normalizeUri(uriOrPath);

    const auto existing = docs_.find(uri);
    if (existing != docs_.end()) {
        // The same editor re-opened under the same uri: idempotent true.
        if (existing->editor == editor)
            return true;
        // Car D (D5) ALIAS JOIN: a DIFFERENT editor whose document IS the
        // registered document (an attached second view, car D's ctor)
        // shares the entry — the hub's own VIEW JOIN takes the same uri
        // (no didClose, no second didOpen, one edit stream; LspHub.hpp's
        // door comment carries the wire rule). documentOpened does NOT
        // re-fire: the DOCUMENT opened exactly once, and openCount/
        // openUris stay one entry per uri (a tab strip rides document-
        // OPENED, not view-opened — the second window is the app's call).
        if (existing->editor && editor->document()
            && existing->editor->document() == editor->document()) {
            Entry e = existing.value(); // QHash value semantics: edit the
            e.views.append(editor);     // copy, write back
            docs_.insert(uri, e);
            connect(editor, &QObject::destroyed, this,
                    [this, uri, editor] { onViewDestroyed(uri, editor); });
            if (hub_) {
                // The hub joins the SAME registration (its own document
                // test); the triggers arm for the new view too — the arm
                // is per-document state, the door resolves through the
                // view (the hub's syncerFor widens both ways, D5).
                if (hub_->openDocument(editor, uri, e.languageId)
                    && popupsOnOpen_)
                    hub_->setPopupTriggers(editor, true);
            }
            return true;
        }
        // A different DOCUMENT claiming the uri: the honest refusal (one
        // document per uri — a real server rejects the second didOpen).
        return false;
    }

    // The ALIAS RULE's mirror half: this document is already registered
    // under ANOTHER uri — refuse. One document, one uri: a second uri
    // would open a second syncer (and a second didOpen) on one edit
    // stream, and the registry would answer the same text under two keys.
    if (editor->document() && !uriForDocument(editor->document()).isEmpty())
        return false;

    const QString language
        = !languageOverride.isEmpty()
              ? languageOverride
              : languageForSuffix(suffixForUri(uri));
    // An unresolved suffix never fights the app: the editor keeps whatever
    // language it already has (the demo/user setLanguage stays honored).
    if (!language.isEmpty())
        editor->setLanguage(language);

    Entry entry;
    entry.editor = editor;
    entry.views.append(editor); // the owner IS views.first() (the
                                // handover rule's invariant)
    entry.languageId = language;
    docs_.insert(uri, entry);
    order_ << uri;
    // The hub's auto-unregister precedent, D5 shape: EVERY view of the
    // document gets its own relay, and the relay does pointer
    // bookkeeping ONLY (the dying editor is never dereferenced — the
    // hub releases its OWN side through its own destroyed() hookups).
    connect(editor, &QObject::destroyed, this,
            [this, uri, editor] { onViewDestroyed(uri, editor); });

    if (hub_) {
        // The documented no-op path: no spec claims this language, the hub
        // says false, and the document simply lives session-side.
        if (hub_->openDocument(editor, uri, language) && popupsOnOpen_)
            hub_->setPopupTriggers(editor, true); // the M8 trigger pair
    }

    emit documentOpened(uri);
    return true;
}

void ProjectSession::closeDocument(qmn::view::CodeEditor* editor)
{
    if (!editor)
        return;
    // D5: the uri resolves through DOCUMENT identity, so this door closes
    // the DOCUMENT whichever view it is knocked with (one document, one
    // close — the hub's own closeDocument releases the whole registration
    // including every joined view's taps and paints).
    const QString uri = uriForEditor(editor);
    if (uri.isEmpty())
        return; // unregistered: the silent no-op
    if (hub_)
        hub_->closeDocument(editor); // didClose while the server is alive
    dropUri(uri);
}

bool ProjectSession::closeUri(const QString& uri)
{
    const QString key = normalizeUri(uri);
    const auto it = docs_.find(key);
    if (it == docs_.end())
        return false;
    if (hub_ && it->editor)
        hub_->closeDocument(it->editor);
    dropUri(key);
    return true;
}

QString ProjectSession::languageForUri(const QString& uri) const
{
    const auto it = docs_.find(normalizeUri(uri));
    return it == docs_.end() ? QString() : it->languageId;
}

qmn::view::CodeEditor* ProjectSession::editorForUri(const QString& uri) const
{
    const auto it = docs_.find(normalizeUri(uri));
    return it == docs_.end() ? nullptr : it->editor;
}

QList<qmn::view::CodeEditor*> ProjectSession::editorsForUri(const QString& uri) const
{
    // E1: the sibling door for feed fan-out. The entry's views list IS the
    // answer — owner first is its standing invariant (views.first() ==
    // editor; onViewDestroyed's handover re-keys both together), so no
    // re-sorting here would be honest bookkeeping anyway.
    const auto it = docs_.find(normalizeUri(uri));
    return it == docs_.end() ? QList<qmn::view::CodeEditor*>() : it->views;
}

QString ProjectSession::uriForDocument(const qmn::view::CodeDocument* document)
    const
{
    if (!document)
        return {};
    for (const QString& uri : order_) {
        const Entry& e = docs_.value(uri);
        // D5: document IDENTITY is the registry's real key-side fact —
        // every view of the document answers the same uri through it.
        if (e.editor && e.editor->document() == document)
            return uri;
    }
    return {};
}

QString ProjectSession::uriForEditor(const qmn::view::CodeEditor* editor)
    const
{
    if (!editor)
        return {};
    return uriForDocument(editor->document()); // D5: widened to document
                                               // identity — ANY view of the
                                               // registered document (the
                                               // owner or a joined alias)
                                               // answers its uri
}

// The dying-view bookkeeping (D5) — see the destroyed() hookup above: the
// LAST view drops the entry (the hub auto-releases its own side on the
// same edge), an OWNER death with joiners alive HANDS OVER.
void ProjectSession::onViewDestroyed(const QString& uri,
                                     qmn::view::CodeEditor* view)
{
    auto it = docs_.find(uri);
    if (it == docs_.end())
        return; // closeDocument dropped it first
    Entry e = it.value();
    e.views.removeAll(view); // the dying view is only ever COMPARED
    if (e.views.isEmpty()) {
        dropUri(uri);
        return;
    }
    if (e.editor == view)
        e.editor = e.views.first(); // the HANDOVER (the hub hands its own
                                    // registration over on the same edge)
    docs_.insert(uri, e);
}

void ProjectSession::dropUri(const QString& uri)
{
    if (docs_.remove(uri) == 0)
        return; // already gone (the destroyed() + closeDocument both-fire)
    order_.removeAll(uri);
    emit documentClosed(uri);
}

} // namespace qmn::glue
