#pragma once

// M23 (Gate 1.5): per-language property overrides. ONE small child QObject
// per language id, obtained through CodeEditor::languageProperties(id) (the
// editor owns and lazily creates them). The resolution model is Monaco's
// `[languageId]` settings: LANGUAGE override (when set) > editor options
// template > factory default. The editor re-reads these properties on every
// language switch and on every change here, so overrides fan out live.
//
// Tri-state WITHOUT std::optional crossing the API boundary (Shiboken):
// a set/clear pair + a has-getter, and the UNSET sentinel is -1 on both the
// getter and the signal. Plain Qt types only; Q_OBJECT; no templates/STL —
// Shiboken-wrappable like the rest of the public view surface.
//
// The built-in registry starts EMPTY (M23 DoD): nothing here changes any
// default behavior, per-language defaults are an IDE-side decision later.

#include <QObject>

namespace qmn::view {

class LanguageProperties : public QObject {
    Q_OBJECT
public:
    // Not for standalone construction by consumers: instances are the
    // editor's children, keyed by normalized language id (the
    // HighlightEngine rule: trimmed + lowercased). The ctor stays public
    // only because Shiboken/qtmeta conventions need it; parenting here is
    // how the editor owns them.
    explicit LanguageProperties(QObject* parent = nullptr);

    // Tab stop width override (M14 semantics, see CodeEditor::setTabStop-
    // Width: soft-tab insert count AND the literal '\t' render grid, plus
    // autoIndent stop math and M21 indent-guide stops). Clamped >= 1 like
    // the editor knob. clearTabStopWidth() restores the un-set state; until
    // a set happens, hasTabStopWidth() is false and the getter reads -1.
    void setTabStopWidth(int columns);
    void clearTabStopWidth();
    bool hasTabStopWidth() const { return tabStopWidth_ >= 0; }
    int tabStopWidth() const { return tabStopWidth_; } // -1 = override OFF

Q_SIGNALS:
    // Emitted after a real change: set carries the NEW clamped value, a
    // clear (of a set state) carries -1. Same-value sets and clearing an
    // already-unset property do NOT emit.
    void tabStopWidthChanged(int columns);

private:
    int tabStopWidth_ = -1; // tri-state: -1 = unset, else the override
};

} // namespace qmn::view
