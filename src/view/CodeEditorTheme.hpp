#pragma once

// M24 (Gate 1.5): the theme object. ONE child QObject carrying every ink
// the editor paints: a fixed ROLE enum covering all canvas chrome (base,
// text, cursor, selection, highlighted-lines band, current-line wash/frame,
// gutter digits + separator, fold markers + hover region, indent guides,
// long-line ruler, find match/current, breakpoint, the four severities,
// inlay hints, drop caret, and the overview ruler's full chrome set) plus
// a TokenKind -> QColor map replacing the fixed table behind
// HighlightEngine::colorFor (which survives as the fallback: unset token
// kinds and the "invalid kind -> default pen" rule stay exactly as they
// are; the sweep just puts every painted ink behind one lookup).
//
// Resolution contract (LIVE, every paint — ink() reads the editor's
// QPalette on the spot, exactly like the M19 mirror reads live state):
// explicit theme value (used VERBATIM, its own alpha included) > today's
// source per role. The default "Palette" theme therefore IS the pre-M24
// behavior: live QPalette roles with the historical alpha nudges baked
// into the fallback (selection = Highlight@90, guides = Text@40, ...),
// and the historical fixed chrome constants (fold gray, severity/find/
// breakpoint family, the ruler chrome) — all of which now live in THIS
// file, so "what the default paints" has exactly one definition. Pixel-
// identity of the DEFAULT theme is a regression tripwire enforced by the
// untouched paint tests.
//
// Presets: Default = the empty map (palette + fixed fallbacks); Light /
// Dark = explicit whole-map presets (Dark's TOKEN map is byte-identical
// to HighlightEngine::colorFor's table — a test asserts that equality —
// so Default -> Dark keeps token ink and only pins the chrome GROUND to
// explicit VS-Dark-family values). A manual role/token edit flips the
// tracked kind() to Custom; applyPreset only ever sets the three named
// kinds.
//
// Ownership: the editor creates it in its ctor (parented child, it holds
// a back-pointer like CodeEditorOptions), exposes it through
// CodeEditor::theme(), and repaints (with a geo-cache wipe, since TOKEN
// ink rides the cached format ranges) on every themeChanged.
//
// Shiboken contract: plain QObject, plain Qt types only (QColor + the
// nested Q_ENUM enums by name); no templates, no STL, no overloads.

#include <QHash>
#include <QObject>

#include "view/CodeEditor.hpp" // the nested TokenKind/Severity this object speaks

namespace qmn::view {

class CodeEditorTheme : public QObject {
    Q_OBJECT
public:
    // Every painted ink on the editor canvas (paintEvent passes only).
    // The find bar and the drag-preview chip are SEPARATE widgets and
    // keep riding plain QPalette — they are not this canvas.
    enum class Role {
        Base,                 // canvas background (fallback: QPalette::Base)
        Text,                 // default glyph pen + the IME preedit underline
        Cursor,               // typing caret + secondary carets (2px bars)
        Selection,            // selection fill (fallback: Highlight@90)
        HighlightedLinesBand, // set_highlighted_lines wash (Highlight@45)
        CurrentLineBackground,// caret-line wash (Text@30)
        CurrentLineFrame,     // BackgroundFrame mode outline (Text@90)
        GutterDigits,         // dim line numbers (PlaceholderText)
        GutterActiveDigits,   // the caret line's number (Text)
        GutterSeparator,      // the column hairline (Text@45)
        FoldMarkers,          // the triangles (fixed cool gray)
        FoldRegion,           // M22 hover tint (fixed cool gray, alpha 46)
        IndentGuides,         // M21 per-stop guides (Text@40)
        LongLineRuler,        // M18 ruler column (Text@40)
        FindMatch,            // M6 quads: all matches (fixed orange, muted;
        FindCurrentMatch,     // the canvas QUAD wash layers its own paint-time
                              // alpha 80/130 on top — see CodeEditor.cpp)
        Breakpoint,           // gutter dot + ruler band (fixed red)
        DiagnosticError,
        DiagnosticWarning,
        DiagnosticInformation,
        DiagnosticHint,       // wavy underlines + gutter dots + ruler bands
        InlayHints,           // hint text (PlaceholderText)
        DropCaret,            // DnD target caret (fallback: QPalette::Text)
        OverviewBackground,   // the ruler strip's opaque ground (fixed)
        OverviewStripe,       // per-line indent stripes (fixed, alpha 110)
        OverviewHighlightedBand, // highlighted-lines band over the strip
        OverviewViewportFill, // viewport frame fill (alpha 26)
        OverviewViewportFrame,// viewport frame pen (alpha 110)
    };
    Q_ENUM(Role)

    // Preset MAPS (see applyPreset). Custom is what kind() reports once a
    // manual edit moved the object off a preset map; it is never an
    // applyPreset argument meaning.
    enum class Preset { Default = 0, Light, Dark, Custom };
    Q_ENUM(Preset)

    // editor_ is the back-link the LIVE fallback reads (palette()); the
    // editor owns this object as a parented child and outlives it.
    explicit CodeEditorTheme(CodeEditor* editor, QObject* parent = nullptr);

    // THE paint-time entry point: explicit value if set (VERBATIM, alpha
    // included), else the documented per-role fallback — palette reads
    // happen here, at paint time, so the default theme follows a live
    // QPalette change just as the pre-M24 widget did.
    QColor ink(Role role) const;

    // The M5 severity family resolved through the four Diagnostic* roles
    // (the wavy underline, gutter dot and overview-ruler band all ride
    // this one door; the pre-M24 fixed constants are the fallbacks).
    QRgb severityInk(CodeEditor::Severity severity) const;

    // Explicit-override doors. color()/tokenColor() report ONLY what was
    // explicitly set: an INVALID QColor means "not overridden, the
    // fallback paints". setColor applies VERBATIM (alpha included — the
    // caller owns the whole color); passing an invalid color clears the
    // key. Same-value sets and clears of un-set keys stay silent; every
    // real change emits themeChanged() exactly once and moves kind() to
    // Custom (that flip is part of the same emission, never a second one).
    QColor color(Role role) const { return colors_.value(role); }
    void setColor(Role role, const QColor& color);
    void clearColor(Role role);
    bool hasColor(Role role) const { return colors_.contains(role); }

    // Token map: an un-set kind falls back to HighlightEngine::colorFor
    // (the "invalid kind -> default pen" rule is unchanged — the caller
    // still skips invalid colors).
    QColor tokenColor(CodeEditor::TokenKind kind) const {
        return tokens_.value(kind);
    }
    void setTokenColor(CodeEditor::TokenKind kind, const QColor& color);
    void clearTokenColor(CodeEditor::TokenKind kind);
    bool hasTokenColor(CodeEditor::TokenKind kind) const {
        return tokens_.contains(kind);
    }

    // Replace the WHOLE map with a preset's values (Default = clear
    // everything back to the fallback behavior). One themeChanged per
    // applyPreset (always — callers read it as "repaint"), kind() becomes
    // the preset. Manual set/clear anywhere afterwards moves kind() to
    // Custom.
    void applyPreset(Preset preset);
    Preset kind() const { return kind_; }

Q_SIGNALS:
    // Any real change (role edit, token edit, preset apply). The editor
    // repaints on it (token ink is cached in the geo slots — the editor
    // wipes those too); consumers can seed their own UI.
    void themeChanged();

private:
    CodeEditor* editor_; // owned elsewhere: the editor outlives its child
    QHash<Role, QColor> colors_;   // explicit role overrides only
    QHash<CodeEditor::TokenKind, QColor> tokens_; // explicit token overrides
    Preset kind_ = Preset::Default;
};

} // namespace qmn::view
