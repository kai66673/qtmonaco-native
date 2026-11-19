#pragma once

// M8 machine 5 (PLAN GATE 2): the WorkspaceEdit DECODER. Layer rule: QtCore
// ONLY (this header does not even pull src/protocol) — a pure function over a
// QJsonValue, so it is testable with no session, no transport, no view.
//
// An LSP WorkspaceEdit arrives in THREE shapes, and a real server uses all of
// them (clangd the third one constantly, which is why PLAN pins all three):
//
//   1. "changes":    { "<uri>": [TextEdit, ...], ... }
//   2. "documentChanges": [ {textDocument:{uri,version?}, edits:[...]},
//                           {kind:"rename-file", oldUri, newUri}, ... ]
//   3. NO "edit" at all: a codeAction whose WORKSPACE EDIT hides in
//      "command":"command.arguments[0]" (clangd's tweak shape).
//
// A feature REPLY adds a fourth, sibling shape: textDocument/formatting (and
// its range/onType siblings) answer a BARE TextEdit[] — no uri, no version.
// `decodeTextEditList` reads it and names the document from the outside, so
// the picky TextEdit parsing lives in exactly one place.
//
// This decoder flattens all three into ONE ordered record list, in the WIRE
// frame (positions are {line, character} units of the NEGOTIATED encoding —
// converting them to model offsets is the caller's PositionConverter turn,
// because only the caller's DocumentSyncer owns the text mirror). The
// ORDERING RULE stays the caller's too, and so does the undo bracketing: the
// applier sorts per document descending and applies each document's batch as
// ONE undo step (the view's applyTextEdits door owns the walk).
//
// WHAT THE CLIENT CANNOT DO is reported, never dropped silently: file-level
// documentChanges (create/rename/delete) need a workspace/file model this
// editor does not have, and a `version` pin on a versioned documentChange
// means the server's view of that document must match ours. The applier's
// rule is ALL-OR-NOTHING, and these records are why.

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

namespace qmn::lsp {

// One TextEdit of a WorkspaceEdit, still in the wire frame. Range is
// end-EXCLUSIVE like every coordinate in this project; `documentVersion` is
// the pin carried by a versioned documentChange (-1 = the shape carried no
// version, which the `changes` map never does).
struct WireTextEdit {
    QString uri;
    int startLine = 0;
    int startCol = 0; // wire `character` units
    int endLine = 0;
    int endCol = 0;
    QString newText;
    int documentVersion = -1;
};

// A documentChange that is NOT a text edit. `oldUri`/`newUri` follow the LSP
// field names per kind: create carries newUri, rename both, delete oldUri.
struct WireFileOperation {
    enum class Kind { Create, Rename, Delete };
    Kind kind = Kind::Create;
    QString oldUri;
    QString newUri;
};

// The flat reading of one WorkspaceEdit. `problems` is the honest audit trail
// (malformed structure, unsupported shapes): empty == well-formed. Reading is
// best-effort — what could be parsed IS in the lists even when a problem was
// recorded, so the caller can show the user WHY it refused.
struct DecodedWorkspaceEdit {
    QVector<WireTextEdit> textEdits;
    QVector<WireFileOperation> fileOperations;
    QStringList problems;

    bool wellFormed() const { return problems.isEmpty(); }
    // The refusal reasons an applier reports, in one pass over the records.
    QStringList unsupportedKinds() const;
};

namespace WorkspaceEditWire {

// Shape 1 + 2 (a WorkspaceEdit object as such). A JSON value that is not an
// object records a problem and returns an empty reading.
DecodedWorkspaceEdit decode(const QJsonValue& workspaceEdit);

// Shape 3: a CodeAction object. Its `edit` wins; when there is no `edit`, the
// `command.arguments[0]` object is read as the WorkspaceEdit (clangd's apply-
// tweak form — the command name is NOT matched, because the whole point of
// the shape is that the payload, not the name, carries the work).
DecodedWorkspaceEdit decodeCodeAction(const QJsonObject& codeAction);

// Shape 4 (machine 5, the feature-reply half): a BARE TextEdit[] — what
// textDocument/formatting / rangeFormatting / onTypeFormatting ANSWER (their
// result type is TextEdit[], not a WorkspaceEdit: a bare edit list has no uri,
// so the CALLER names the document it belongs to). `null` is the spec's own
// "the server changed nothing" answer: an empty, well-formed reading, not a
// problem. Anything that is not an array or null is a problem.
DecodedWorkspaceEdit decodeTextEditList(const QJsonValue& textEdits,
                                        const QString& uri);

} // namespace WorkspaceEditWire

} // namespace qmn::lsp
