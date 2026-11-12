#include "view/CodeEditorTheme.hpp"

// M24: the theme object — the override maps, the LIVE fallback table (the
// single definition of "what the default theme paints": the pre-M24
// QPalette reads with their historical alpha nudges, and the pre-M24
// fixed chrome constants), the tri-state discipline (same-value sets /
// un-set clears silent, one themeChanged per real change), and the two
// explicit preset maps.

#include <QPalette>

namespace qmn::view {

namespace {

using Role = CodeEditorTheme::Role;
using Kind = CodeEditor::TokenKind;

// The historical nudge: palette color + a forced alpha (the exact
// setAlpha calls the paint code used before M24, kept verbatim so the
// default theme stays pixel-identical).
QColor withAlpha(QColor c, int a)
{
    c.setAlpha(a);
    return c;
}

// VS Code Dark+ family. The token map below MUST stay value-identical to
// HighlightEngine::colorFor (the M24 spec: "Dark's token map = today's
// fixed one") — a test asserts the equality per kind, drift fails CI.
// The chrome values PIN what today's offscreen dark palette + fixed
// constants already paint; the shared warm/red state inks (severity,
// find, breakpoint) and the overview chrome keep today's exact
// constants, so Default vs Dark differs in the chrome GROUND, not in the
// state semantics.
const struct {
    Role role;
    QColor color;
} kDarkChrome[] = {
    {Role::Base, QColor(0x1E, 0x1E, 0x1E)},
    {Role::Text, QColor(0xD4, 0xD4, 0xD4)},
    {Role::Cursor, QColor(0xAE, 0xAF, 0xAD)},
    {Role::Selection, QColor(0x26, 0x4F, 0x78)},
    {Role::HighlightedLinesBand, QColor(96, 144, 196, 60)},
    {Role::CurrentLineBackground, QColor(255, 255, 255, 18)}, // VS Code #FFFFFF12
    {Role::CurrentLineFrame, QColor(255, 255, 255, 60)},
    {Role::GutterDigits, QColor(0x85, 0x85, 0x85)},
    {Role::GutterActiveDigits, QColor(0xC6, 0xC6, 0xC6)},
    {Role::GutterSeparator, QColor(0x3C, 0x3C, 0x3C)},
    {Role::FoldMarkers, QColor(0xC0, 0xC0, 0xC0)},
    {Role::FoldRegion, QColor(150, 150, 150, 46)}, // today's M22 tint
    {Role::IndentGuides, QColor(0x40, 0x40, 0x40)},
    {Role::LongLineRuler, QColor(0x5A, 0x5A, 0x5A)},
    {Role::FindMatch, QColor(196, 136, 30)},      // today's fixed family
    {Role::FindCurrentMatch, QColor(246, 169, 0)},
    {Role::Breakpoint, QColor(220, 61, 66)},
    {Role::DiagnosticError, QColor(241, 76, 76)},
    {Role::DiagnosticWarning, QColor(255, 204, 0)},
    {Role::DiagnosticInformation, QColor(117, 190, 255)},
    {Role::DiagnosticHint, QColor(127, 127, 127)},
    {Role::InlayHints, QColor(0x9E, 0x9E, 0x9E)},
    {Role::DropCaret, QColor(0xD4, 0xD4, 0xD4)},
    {Role::OverviewBackground, QColor(22, 22, 22)},          // today's ruler chrome
    {Role::OverviewStripe, QColor(140, 140, 140, 110)},
    {Role::OverviewHighlightedBand, QColor(96, 144, 196, 90)},
    {Role::OverviewViewportFill, QColor(210, 210, 210, 26)},
    {Role::OverviewViewportFrame, QColor(190, 190, 190, 110)},
};

// VS Code Light+ family (chrome + the classic light token colors). The
// warm/red state-ink family darkens for legibility on white grounds.
const struct {
    Role role;
    QColor color;
} kLightChrome[] = {
    {Role::Base, QColor(0xFF, 0xFF, 0xFF)},
    {Role::Text, QColor(0x00, 0x00, 0x00)},
    {Role::Cursor, QColor(0x00, 0x00, 0x00)},
    {Role::Selection, QColor(0xAD, 0xD6, 0xFF)},
    {Role::HighlightedLinesBand, QColor(96, 144, 196, 60)},
    {Role::CurrentLineBackground, QColor(0, 0, 0, 18)},
    {Role::CurrentLineFrame, QColor(0, 0, 0, 60)},
    {Role::GutterDigits, QColor(0x23, 0x78, 0x93)}, // VS Code Light+ line numbers
    {Role::GutterActiveDigits, QColor(0x0B, 0x21, 0x6C)},
    {Role::GutterSeparator, QColor(0xA0, 0xA0, 0xA0)},
    {Role::FoldMarkers, QColor(0x5A, 0x5A, 0x5A)},
    {Role::FoldRegion, QColor(120, 120, 120, 60)}, // darkens a white ground
    {Role::IndentGuides, QColor(0xD4, 0xD4, 0xD4)},
    {Role::LongLineRuler, QColor(0x99, 0x99, 0x99)},
    {Role::FindMatch, QColor(196, 136, 30)},       // legible mid-orange on white
    {Role::FindCurrentMatch, QColor(246, 169, 0)},
    {Role::Breakpoint, QColor(220, 61, 66)},
    {Role::DiagnosticError, QColor(0xE5, 0x14, 0x00)},
    {Role::DiagnosticWarning, QColor(0xBF, 0x88, 0x24)},
    {Role::DiagnosticInformation, QColor(0x1B, 0x6E, 0xC2)},
    {Role::DiagnosticHint, QColor(0x7A, 0x7A, 0x7A)},
    {Role::InlayHints, QColor(0x7A, 0x7A, 0x7A)},
    {Role::DropCaret, QColor(0x00, 0x00, 0x00)},
    {Role::OverviewBackground, QColor(0xEA, 0xEA, 0xEA)},
    {Role::OverviewStripe, QColor(90, 90, 90, 110)},
    {Role::OverviewHighlightedBand, QColor(96, 144, 196, 90)},
    {Role::OverviewViewportFill, QColor(0, 0, 0, 26)},
    {Role::OverviewViewportFrame, QColor(0, 0, 0, 110)},
};

// Byte-for-byte the table HighlightEngine::colorFor ships (see the
// tripwire test). Text is deliberately absent: Text means "default pen"
// (the invalid-QColor rule), it never appears as a span and is never a
// themeable token.
const struct {
    Kind kind;
    QColor color;
} kDarkTokens[] = {
    {Kind::Comment, QColor(0x6A, 0x99, 0x55)},
    {Kind::Keyword, QColor(0x56, 0x9C, 0xD6)},
    {Kind::Number, QColor(0xB5, 0xCE, 0xA8)},
    {Kind::String, QColor(0xCE, 0x91, 0x78)},
    {Kind::StringSpecial, QColor(0xD7, 0xBA, 0x7D)},
    {Kind::StringSpecialKey, QColor(0x9C, 0xDC, 0xFE)},
    {Kind::Escape, QColor(0xD7, 0xBA, 0x7D)},
    {Kind::Embedded, QColor(0x9C, 0xDC, 0xFE)},
    {Kind::Constant, QColor(0x4F, 0xC1, 0xFF)},
    {Kind::ConstantBuiltin, QColor(0x56, 0x9C, 0xD6)},
    {Kind::Constructor, QColor(0x4E, 0xC9, 0xB0)},
    {Kind::Function, QColor(0xDC, 0xDC, 0xAA)},
    {Kind::FunctionBuiltin, QColor(0xDC, 0xDC, 0xAA)},
    {Kind::FunctionMethod, QColor(0xDC, 0xDC, 0xAA)},
    {Kind::Type, QColor(0x4E, 0xC9, 0xB0)},
    {Kind::Variable, QColor(0x9C, 0xDC, 0xFE)},
    {Kind::VariableBuiltin, QColor(0x9C, 0xDC, 0xFE)},
    {Kind::Property, QColor(0x9C, 0xDC, 0xFE)},
    {Kind::Operator, QColor(0xD4, 0xD4, 0xD4)},
    {Kind::Punctuation, QColor(0xD4, 0xD4, 0xD4)},
    {Kind::PunctuationBracket, QColor(0xD4, 0xD4, 0xD4)},
    {Kind::PunctuationDelimiter, QColor(0xD4, 0xD4, 0xD4)},
    {Kind::PunctuationSpecial, QColor(0xD4, 0xD4, 0xD4)},
};

// VS Code Light+ token colors.
const struct {
    Kind kind;
    QColor color;
} kLightTokens[] = {
    {Kind::Comment, QColor(0x00, 0x80, 0x00)},
    {Kind::Keyword, QColor(0x00, 0x00, 0xFF)},
    {Kind::Number, QColor(0x09, 0x86, 0x58)},
    {Kind::String, QColor(0xA3, 0x15, 0x15)},
    {Kind::StringSpecial, QColor(0xEE, 0x00, 0x0B)},
    {Kind::StringSpecialKey, QColor(0x00, 0x00, 0xFF)},
    {Kind::Escape, QColor(0xEE, 0x00, 0x0B)},
    {Kind::Embedded, QColor(0x80, 0x00, 0x80)},
    {Kind::Constant, QColor(0x00, 0x70, 0xC1)},
    {Kind::ConstantBuiltin, QColor(0x00, 0x00, 0xFF)},
    {Kind::Constructor, QColor(0x79, 0x5E, 0x26)},
    {Kind::Function, QColor(0x79, 0x5E, 0x26)},
    {Kind::FunctionBuiltin, QColor(0x79, 0x5E, 0x26)},
    {Kind::FunctionMethod, QColor(0x79, 0x5E, 0x26)},
    {Kind::Type, QColor(0x26, 0x7F, 0x99)},
    {Kind::Variable, QColor(0x00, 0x10, 0x80)},
    {Kind::VariableBuiltin, QColor(0x00, 0x70, 0xC1)},
    {Kind::Property, QColor(0x00, 0x10, 0x80)},
    {Kind::Operator, QColor(0x00, 0x00, 0x00)},
    {Kind::Punctuation, QColor(0x00, 0x00, 0x00)},
    {Kind::PunctuationBracket, QColor(0x00, 0x00, 0x00)},
    {Kind::PunctuationDelimiter, QColor(0x00, 0x00, 0x00)},
    {Kind::PunctuationSpecial, QColor(0x00, 0x00, 0x00)},
};

} // namespace

CodeEditorTheme::CodeEditorTheme(CodeEditor* editor, QObject* parent)
    : QObject(parent), editor_(editor)
{
}

// THE fallback table — the single definition of what the DEFAULT (empty-
// map) theme paints. Every entry is the pre-M24 source of that ink,
// kept verbatim (measured pixel-identity: the untouched paint suite is
// the tripwire). Palette reads happen HERE, at paint time, so live
// palette changes keep flowing exactly as they did before M24; the
// fixed entries are the historical cool/warm chrome constants.
QColor CodeEditorTheme::ink(Role role) const
{
    if (const QColor c = colors_.value(role); c.isValid())
        return c;
    const QPalette pal = editor_->palette();
    switch (role) {
    case Role::Base: return pal.color(QPalette::Base);
    case Role::Text: return pal.color(QPalette::Text);
    case Role::Cursor: return pal.color(QPalette::Text);
    case Role::Selection: return withAlpha(pal.color(QPalette::Highlight), 90);
    case Role::HighlightedLinesBand: return withAlpha(pal.color(QPalette::Highlight), 45);
    case Role::CurrentLineBackground: return withAlpha(pal.color(QPalette::Text), 30);
    case Role::CurrentLineFrame: return withAlpha(pal.color(QPalette::Text), 90);
    case Role::GutterDigits: return pal.color(QPalette::PlaceholderText);
    case Role::GutterActiveDigits: return pal.color(QPalette::Text);
    case Role::GutterSeparator: return withAlpha(pal.color(QPalette::Text), 45);
    case Role::FoldMarkers: return QColor(150, 150, 150);
    case Role::FoldRegion: return QColor(150, 150, 150, 46);
    case Role::IndentGuides: return withAlpha(pal.color(QPalette::Text), 40);
    case Role::LongLineRuler: return withAlpha(pal.color(QPalette::Text), 40);
    case Role::FindMatch: return QColor(196, 136, 30);
    case Role::FindCurrentMatch: return QColor(246, 169, 0);
    case Role::Breakpoint: return QColor(220, 61, 66);
    case Role::DiagnosticError: return QColor(241, 76, 76);
    case Role::DiagnosticWarning: return QColor(255, 204, 0);
    case Role::DiagnosticInformation: return QColor(117, 190, 255);
    case Role::DiagnosticHint: return QColor(127, 127, 127);
    case Role::InlayHints: return pal.color(QPalette::PlaceholderText);
    case Role::DropCaret: return pal.color(QPalette::Text);
    case Role::OverviewBackground: return QColor(22, 22, 22);
    case Role::OverviewStripe: return QColor(140, 140, 140, 110);
    case Role::OverviewHighlightedBand: return QColor(96, 144, 196, 90);
    case Role::OverviewViewportFill: return QColor(210, 210, 210, 26);
    case Role::OverviewViewportFrame: return QColor(190, 190, 190, 110);
    }
    return QColor(); // unreachable: the enum is closed
}

QRgb CodeEditorTheme::severityInk(CodeEditor::Severity severity) const
{
    switch (severity) {
    case CodeEditor::Severity::Error: return ink(Role::DiagnosticError).rgb();
    case CodeEditor::Severity::Warning: return ink(Role::DiagnosticWarning).rgb();
    case CodeEditor::Severity::Information: return ink(Role::DiagnosticInformation).rgb();
    case CodeEditor::Severity::Hint: return ink(Role::DiagnosticHint).rgb();
    }
    return ink(Role::DiagnosticError).rgb(); // the pre-M24 fallthrough
}

void CodeEditorTheme::setColor(Role role, const QColor& color)
{
    if (!color.isValid()) {
        clearColor(role); // an invalid color is the tri-state's OFF switch
        return;
    }
    if (colors_.value(role) == color)
        return; // same-value set stays silent
    colors_.insert(role, color);
    kind_ = Preset::Custom; // a manual edit moved off any preset map
    emit themeChanged();
}

void CodeEditorTheme::clearColor(Role role)
{
    if (!colors_.remove(role))
        return; // un-set key: silent, same-value discipline
    kind_ = Preset::Custom;
    emit themeChanged();
}

void CodeEditorTheme::setTokenColor(CodeEditor::TokenKind kind, const QColor& color)
{
    if (!color.isValid()) {
        clearTokenColor(kind); // invalid = the token's OFF switch
        return;
    }
    if (tokens_.value(kind) == color)
        return;
    tokens_.insert(kind, color);
    kind_ = Preset::Custom;
    emit themeChanged();
}

void CodeEditorTheme::clearTokenColor(CodeEditor::TokenKind kind)
{
    if (!tokens_.remove(kind))
        return;
    kind_ = Preset::Custom;
    emit themeChanged();
}

void CodeEditorTheme::applyPreset(Preset preset)
{
    colors_.clear();
    tokens_.clear();
    switch (preset) {
    case Preset::Default:
        break; // empty map = the fallback table (today's behavior)
    case Preset::Light:
        for (const auto& e : kLightChrome)
            colors_.insert(e.role, e.color);
        for (const auto& e : kLightTokens)
            tokens_.insert(e.kind, e.color);
        break;
    case Preset::Dark:
        for (const auto& e : kDarkChrome)
            colors_.insert(e.role, e.color);
        for (const auto& e : kDarkTokens)
            tokens_.insert(e.kind, e.color);
        break;
    case Preset::Custom:
        break; // not an applyPreset argument meaning: a cleared map + the
               // kind recorded as requested (kind_ below) is the whole story
    }
    kind_ = preset;
    emit themeChanged(); // always: callers treat it as "repaint please"
}

} // namespace qmn::view
