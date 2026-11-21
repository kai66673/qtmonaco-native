#include "ProjectSession.hpp"

#include "LspHub.hpp"

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
        // A DIFFERENT editor claiming the uri: the honest refusal (one
        // document per uri — a real server rejects the second didOpen).
        return existing->editor == editor;
    }

    const QString language
        = !languageOverride.isEmpty()
              ? languageOverride
              : languageForSuffix(suffixForUri(uri));
    // An unresolved suffix never fights the app: the editor keeps whatever
    // language it already has (the demo/user setLanguage stays honored).
    if (!language.isEmpty())
        editor->setLanguage(language);

    docs_.insert(uri, Entry { editor, language });
    order_ << uri;
    // The hub's auto-unregister precedent: an editor dying under us drops
    // the session entry too (the hub releases its OWN side through its own
    // destroyed() hookup — this lambda must not touch the dying editor).
    connect(editor, &QObject::destroyed, this, [this, uri] { dropUri(uri); });

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

QString ProjectSession::uriForEditor(const qmn::view::CodeEditor* editor) const
{
    if (!editor)
        return {};
    for (const QString& uri : order_) {
        if (docs_.value(uri).editor == editor)
            return uri;
    }
    return {};
}

void ProjectSession::dropUri(const QString& uri)
{
    if (docs_.remove(uri) == 0)
        return; // already gone (the destroyed() + closeDocument both-fire)
    order_.removeAll(uri);
    emit documentClosed(uri);
}

} // namespace qmn::glue
