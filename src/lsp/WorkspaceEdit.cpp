#include "lsp/WorkspaceEdit.hpp"

#include <QJsonArray>

namespace qmn::lsp {

QStringList DecodedWorkspaceEdit::unsupportedKinds() const
{
    QStringList out;
    out.reserve(static_cast<int>(fileOperations.size()));
    for (const WireFileOperation& op : fileOperations) {
        switch (op.kind) {
        case WireFileOperation::Kind::Create:
            out << QStringLiteral("create-file %1").arg(op.newUri);
            break;
        case WireFileOperation::Kind::Rename:
            out << QStringLiteral("rename-file %1 -> %2").arg(op.oldUri, op.newUri);
            break;
        case WireFileOperation::Kind::Delete:
            out << QStringLiteral("delete-file %1").arg(op.oldUri);
            break;
        }
    }
    return out;
}

namespace {

// One leg of a Position as the wire sent it. Absent or not-a-number is a
// problem, never a silent 0: a truncated Position must refuse the edit rather
// than quietly rewrite line 0 (the decoder's whole job is to be picky so the
// applier never has to guess).
bool positionLeg(const QJsonValue& positionValue, const QString& leg, int* out)
{
    const QJsonValue value = positionValue.toObject().value(leg);
    if (!value.isDouble())
        return false;
    *out = int(value.toDouble());
    return true;
}

// One TextEdit object. `uri`/`version` come from the enclosing shape (the
// "changes" map key, or the versioned documentChange) — a TextEdit carries
// neither, which is exactly why the flat record does.
void readTextEdit(const QJsonValue& editValue, const QString& uri, int version,
                  const QString& where, DecodedWorkspaceEdit& out)
{
    if (!editValue.isObject()) {
        out.problems.append(where + QStringLiteral(": TextEdit is not an object"));
        return;
    }
    const QJsonObject edit = editValue.toObject();
    const QJsonValue newTextValue = edit.value(QStringLiteral("newText"));
    if (!newTextValue.isString()) {
        out.problems.append(where + QStringLiteral(": TextEdit has no string \"newText\""));
        return;
    }

    WireTextEdit te;
    te.uri = uri;
    te.documentVersion = version;
    te.newText = newTextValue.toString();
    const QJsonObject range = edit.value(QStringLiteral("range")).toObject();
    const QJsonValue start = range.value(QStringLiteral("start"));
    const QJsonValue end = range.value(QStringLiteral("end"));
    bool parsed = true;
    parsed = positionLeg(start, QStringLiteral("line"), &te.startLine) && parsed;
    parsed = positionLeg(start, QStringLiteral("character"), &te.startCol) && parsed;
    parsed = positionLeg(end, QStringLiteral("line"), &te.endLine) && parsed;
    parsed = positionLeg(end, QStringLiteral("character"), &te.endCol) && parsed;
    if (!parsed) {
        out.problems.append(where + QStringLiteral(": TextEdit range is not a Position pair"));
        return;
    }
    out.textEdits.append(te);
}

// A documentChanges entry that is a file-level operation ("kind" is the
// discriminator: create / rename / delete). True when `entry` WAS a file op.
bool readFileChange(const QJsonObject& entry, DecodedWorkspaceEdit& out)
{
    const QString kind = entry.value(QStringLiteral("kind")).toString();
    if (kind.isEmpty())
        return false;
    WireFileOperation op;
    if (kind == QLatin1String("create")) {
        op.kind = WireFileOperation::Kind::Create;
        op.newUri = entry.value(QStringLiteral("uri")).toString();
    } else if (kind == QLatin1String("rename")) {
        op.kind = WireFileOperation::Kind::Rename;
        op.oldUri = entry.value(QStringLiteral("oldUri")).toString();
        op.newUri = entry.value(QStringLiteral("newUri")).toString();
    } else if (kind == QLatin1String("delete")) {
        op.kind = WireFileOperation::Kind::Delete;
        op.oldUri = entry.value(QStringLiteral("uri")).toString();
    } else {
        out.problems.append(QStringLiteral("documentChanges: unknown kind \"%1\"").arg(kind));
        return true;
    }
    out.fileOperations.append(op);
    return true;
}

// Shapes 1 + 2 read together: a WorkspaceEdit may carry either map, and a
// server that sends both gets both applied (documentChanges after "changes",
// the spec's own ordering note).
DecodedWorkspaceEdit decodeObject(const QJsonObject& edit)
{
    DecodedWorkspaceEdit out;

    const QJsonValue changesValue = edit.value(QStringLiteral("changes"));
    if (!changesValue.isUndefined() && !changesValue.isNull()) {
        if (!changesValue.isObject()) {
            out.problems.append(QStringLiteral("\"changes\" is not an object"));
        } else {
            const QJsonObject changes = changesValue.toObject();
            // QMap-ordered iteration (QJsonObject's own storage): deterministic
            // document order, which is what makes the applier's per-document
            // undo sequence pinnable in a test.
            for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
                const QString where = QStringLiteral("changes[%1]").arg(it.key());
                if (!it.value().isArray()) {
                    out.problems.append(where + QStringLiteral(" is not an array"));
                    continue;
                }
                for (const QJsonValue& te : it.value().toArray())
                    readTextEdit(te, it.key(), -1, where, out);
            }
        }
    }

    const QJsonValue docChangesValue = edit.value(QStringLiteral("documentChanges"));
    if (!docChangesValue.isUndefined() && !docChangesValue.isNull()) {
        if (!docChangesValue.isArray()) {
            out.problems.append(QStringLiteral("\"documentChanges\" is not an array"));
        } else {
            const QJsonArray entries = docChangesValue.toArray();
            for (int i = 0; i < entries.size(); ++i) {
                const QString where = QStringLiteral("documentChanges[%1]").arg(i);
                if (!entries.at(i).isObject()) {
                    out.problems.append(where + QStringLiteral(" is not an object"));
                    continue;
                }
                const QJsonObject entry = entries.at(i).toObject();
                if (readFileChange(entry, out))
                    continue;
                const QJsonObject doc = entry.value(QStringLiteral("textDocument")).toObject();
                if (doc.isEmpty()) {
                    out.problems.append(where + QStringLiteral(" has no \"textDocument\""));
                    continue;
                }
                if (entry.contains(QStringLiteral("operations"))) {
                    // The LSP 3.18 PlainTextDocumentEdit shape (insert/delete/
                    // replace OPERATIONS instead of TextEdits): not v1.
                    out.problems.append(where
                                        + QStringLiteral(" uses \"operations\", not \"edits\""));
                    continue;
                }
                const QString uri = doc.value(QStringLiteral("uri")).toString();
                if (uri.isEmpty()) {
                    out.problems.append(where + QStringLiteral(" textDocument has no uri"));
                    continue;
                }
                // The version pin (LSP: the edit is binding only on the client
                // document at THIS version). Absent/null = unpinned; a number
                // is binding, and the applier refuses a mismatch.
                const QJsonValue versionValue = doc.value(QStringLiteral("version"));
                const int version = versionValue.isDouble() ? int(versionValue.toDouble()) : -1;
                if (!entry.value(QStringLiteral("edits")).isArray()) {
                    out.problems.append(where + QStringLiteral(" has no \"edits\" array"));
                    continue;
                }
                for (const QJsonValue& te : entry.value(QStringLiteral("edits")).toArray())
                    readTextEdit(te, uri, version, where, out);
            }
        }
    }

    // {} is a legal EMPTY WorkspaceEdit (nothing to do, nothing refused), so a
    // document that carries neither key is a problem only when it carries
    // something ELSE instead — that is the shape the client cannot read.
    const bool carriesEither =
        !changesValue.isUndefined() || !docChangesValue.isUndefined();
    if (!carriesEither && !edit.isEmpty() && out.problems.isEmpty()) {
        out.problems.append(
            QStringLiteral("WorkspaceEdit has neither \"changes\" nor \"documentChanges\""));
    }
    return out;
}

} // namespace

namespace WorkspaceEditWire {

DecodedWorkspaceEdit decode(const QJsonValue& workspaceEdit)
{
    if (workspaceEdit.isUndefined() || workspaceEdit.isNull()) {
        DecodedWorkspaceEdit out;
        out.problems.append(QStringLiteral("WorkspaceEdit is missing"));
        return out;
    }
    if (!workspaceEdit.isObject()) {
        DecodedWorkspaceEdit out;
        out.problems.append(QStringLiteral("WorkspaceEdit is not an object"));
        return out;
    }
    return decodeObject(workspaceEdit.toObject());
}

DecodedWorkspaceEdit decodeCodeAction(const QJsonObject& codeAction)
{
    const QJsonValue editValue = codeAction.value(QStringLiteral("edit"));
    if (!editValue.isUndefined() && !editValue.isNull())
        return decode(editValue);

    // The clangd shape: the action's whole work is the WorkspaceEdit hidden in
    // command.arguments[0]. The command NAME is deliberately not matched —
    // server-specific tweak names change; the payload is what carries the edit.
    const QJsonArray args
        = codeAction.value(QStringLiteral("command")).toObject()
              .value(QStringLiteral("arguments")).toArray();
    if (!args.isEmpty() && args.at(0).isObject())
        return decode(args.at(0));

    DecodedWorkspaceEdit out;
    out.problems.append(
        QStringLiteral("codeAction carries neither \"edit\" nor \"command.arguments[0]\""));
    return out;
}

DecodedWorkspaceEdit decodeTextEditList(const QJsonValue& textEdits,
                                        const QString& uri)
{
    DecodedWorkspaceEdit out;
    if (textEdits.isNull() || textEdits.isUndefined())
        return out; // the spec's own "the server changed nothing" answer
    if (!textEdits.isArray()) {
        out.problems.append(QStringLiteral("TextEdit list is not an array"));
        return out;
    }
    if (uri.isEmpty()) {
        // A bare TextEdit[] carries no uri of its own: naming the document is
        // the CALLER's job, and an unnamed edit list cannot be applied.
        out.problems.append(
            QStringLiteral("TextEdit list names no document"));
        return out;
    }
    const QJsonArray edits = textEdits.toArray();
    for (int i = 0; i < edits.size(); ++i) {
        readTextEdit(edits.at(i), uri, -1,
                     QStringLiteral("textEdits[%1]").arg(i), out);
    }
    return out;
}

} // namespace WorkspaceEditWire

} // namespace qmn::lsp
