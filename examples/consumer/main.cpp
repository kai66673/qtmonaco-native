// C++ consumer smoke (PLAN.md Phase F gate: "C++ consumer smoke"). Built by
// examples/consumer against the INSTALLED package only (find_package(qmn)),
// so it proves the exported qmn::core/qmn::view targets, the header layout,
// the internal-tree-sitter link re-fix and the automatic a11y registration
// all hold outside the build tree. Headless: run with QT_QPA_PLATFORM=
// offscreen. Exit 0 = every check passed.

#include <QAccessible>
#include <QApplication>
#include <QClipboard>

#include <cstdio>

#include "view/CodeEditor.hpp"
#include "view/CodeEditorOptions.hpp" // options(): only fwd-declared above

namespace {
int failures = 0;
} // namespace

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAILED: %s (line %d)\n", #cond, __LINE__);\
            ++failures;                                                    \
        }                                                                  \
    } while (false)

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    qmn::view::CodeEditor editor;
    const QString doc = QStringLiteral("def greet(name):\n    return f\"hi {name}\"\n");
    editor.setPlainText(doc);
    CHECK(editor.toPlainText() == doc);
    CHECK(editor.lineCount() == 3); // trailing newline opens the third line

    // Vendored tree-sitter really links through the package (the PRIVATE-
    // static export re-fix): language doors + a forced engine pass.
    CHECK(qmn::view::CodeEditor::availableLanguages().contains(QStringLiteral("python")));
    editor.setLanguage(QStringLiteral("python"));
    CHECK(editor.language() == QStringLiteral("python"));
    const auto spans = editor.highlightSpans(0, doc.size());
    CHECK(!spans.isEmpty());

    // Caret + undo ride the exported doors (clipboard paste: the offscreen
    // QPA clipboard is in-process, precedent: the widget test suite).
    editor.setPosition({0, 0});
    QApplication::clipboard()->setText(QStringLiteral("# "));
    editor.paste();
    CHECK(editor.toPlainText().startsWith(QStringLiteral("# def greet")));
    CHECK(editor.canUndo());
    editor.undo();
    CHECK(editor.toPlainText() == doc);

    // The M19 options object is reachable and wired through the package.
    CHECK(editor.options() != nullptr);
    editor.options()->setReadOnly(true);
    CHECK(editor.options()->isReadOnly());
    editor.options()->setReadOnly(false);

    // Decoration doors compile+link across the export boundary.
    editor.setHighlightedLines(0, 0);
    editor.clearHighlightedLines();

    // M11: the a11y factory installs itself from the CodeEditor ctor, so a
    // plain consumer gets the full text interface with zero app wiring.
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(&editor);
    CHECK(iface != nullptr);
    if (iface) {
        CHECK(iface->role() == QAccessible::EditableText);
        CHECK(iface->textInterface() != nullptr);
        if (iface->textInterface())
            CHECK(iface->textInterface()->characterCount() == doc.size());
    }

    if (failures) {
        std::fprintf(stderr, "qmn consumer smoke: %d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("qmn consumer smoke OK (installed package, %d units, %d spans)\n",
                doc.size(), spans.size());
    return 0;
}
