#pragma once

#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QMetaType>
#include <QPoint>
#include <QScrollBar>
#include <QString>
#include <QStringList>
#include <QTextLayout>
#include <QTextLine>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include "core/TextBuffer.hpp"

#include <array>
#include <functional>
#include <memory>

// M16 seams, pointer-only in the API (the menu/DnD machinery lives in the .cpp).
QT_FORWARD_DECLARE_CLASS(QMenu)
QT_FORWARD_DECLARE_CLASS(QMimeData)
QT_FORWARD_DECLARE_CLASS(QContextMenuEvent)
QT_FORWARD_DECLARE_CLASS(QDragEnterEvent)
QT_FORWARD_DECLARE_CLASS(QDragMoveEvent)
QT_FORWARD_DECLARE_CLASS(QDragLeaveEvent)
QT_FORWARD_DECLARE_CLASS(QDropEvent)

// Used by a private method signature only (the paint pass lives in the .cpp).
QT_FORWARD_DECLARE_CLASS(QPainter)
#include <utility>

class QInputMethodEvent;

namespace qmn::view {

class HighlightEngine; // M4 private seam; defined in view/HighlightEngine.hpp
class CodeEditorOptions; // M19 settings object; view/CodeEditorOptions.hpp
class CodeEditorAccessible; // M11 a11y bridge; view/CodeEditorAccessible.hpp
class LanguageProperties; // M23 language overrides; view/LanguageProperties.hpp
class CodeEditorTheme; // M24 ink themes; view/CodeEditorTheme.hpp
class CodeDocument; // M25 model owner; view/CodeDocument.hpp

// 0-based text position. col is counted in UTF-16 code units.
struct TextPosition {
    int line = 0;
    int col = 0;

    // A real constructor (not aggregate init) is what the Python binding needs
    // to expose TextPosition(line, col) the way qtmonaco's set_cursor does.
    // Brace uses like TextPosition{1, 0} keep working unchanged.
    TextPosition() = default;
    TextPosition(int line, int col) : line(line), col(col) {}

    bool operator==(const TextPosition&) const = default;
};

// (Q_DECLARE_METATYPE for this type is at the bottom of this header, at global
// scope, where Qt requires it - see the note there.)

// Editor widget: a raw QWidget painting visible lines with QPainter.
//
// Deliberately included: viewport-culled monospace painting, caret with
// blinking, keyboard editing (text, Enter, Backspace, Delete, arrows,
// Home/End, PageUp/PageDown), grapheme-cluster-aware arrows and editing
// (QTextBoundaryFinder, ICU-backed: ZWJ sequences, flags, combining marks
// move and delete as one unit), Ctrl+arrows word navigation and Ctrl+Del
// word deletion, mouse caret placement and drag selection,
// Shift-navigation + Ctrl+A selections with replace-on-type semantics,
// clipboard copy/cut/paste (whole-line when no selection, Monaco-style),
// X11 primary selection mirroring + middle-click paste, IME composition
// (preedit rendered at the caret, only committed text reaches the model),
// vertical scrolling, per-keystroke latency instrumentation, a
// line-number gutter (off/on/relative/interval) with breakpoint and
// diagnostic glyphs, whole-line highlight bands, diagnostic wavy
// underlines and inlay hints (M5), find/replace with a built-in Ctrl+F
// bar plus match highlights (M6), and
// tree-sitter-backed syntax highlighting with a grammar-less fallback
// (M4: python/json/javascript, colors painted through the same cached
// QTextLine layout as the plain path).
//
// `col` stays UTF-16 code units everywhere (LSP semantics); the
// caret may sit inside a cluster only via setPlainText/setPosition/mouse
// clicks, and arrows/edits snap outward from there without ever splitting
// a cluster.
//
// Geometry has a single source of truth: every visible line gets a
// laid-out QTextLine (per-line LRU cache below), and caret x, selection
// x, painting and mouse hit-testing all read the same glyph-engine
// advances. Wide clusters (emoji ZWJ sequences, flags) therefore never
// drift the caret away from the drawn text; pure-ASCII lines keep exact
// col*cellWidth positions.
//
// Deliberately NOT included (follow-up milestones): soft wrap, horizontal
// scrolling, column memory for Up/Down, double-click word selection. See
// README roadmap.
class CodeEditor : public QWidget {
    Q_OBJECT
public:
    struct PerfStats {
        qint64 count = 0;
        qint64 sumNs = 0; // keyPressEvent logic (model edit + scheduling)
        qint64 maxNs = 0;
        qint64 meanNs() const { return count ? sumNs / count : 0; }
    };

    explicit CodeEditor(QWidget* parent = nullptr);
    ~CodeEditor() override;

    void setPlainText(const QString& text);
    QString toPlainText() const;

    void setPosition(TextPosition pos);
    TextPosition position() const { return caret_; }
    int lineCount() const { return buffer_.lineCount(); }

    // Selection: the caret is always the active end; anchor_ is the inactive
    // end. Both ends are always fully resolved text positions, so painting
    // and hit-testing never need to reason about "virtual" columns.
    bool hasSelection() const { return hasSelection_; }
    QString selectedText() const;
    int selectedLength() const; // UTF-16 code units, O(log P), no copy
    void selectAll();
    void clearSelection();

    // Clipboard (M1b). With no selection, copy/cut target the whole current
    // line (newline included) — Monaco semantics. paste() inserts the
    // clipboard text, replacing the selection, with CRLF normalized to '\n'.
    void copy();
    void cut();
    void paste();

    // Block indent/outdent (M14, also Tab / Shift+Tab in the key handler).
    // Every line any caret's selection touches is shifted by one tab stop
    // (indent: stop-many spaces at col 0; outdent: up to one stop of
    // leading whitespace removed). The selection END line is excluded when
    // the end sits at col 0. All carets end collapsed; one call = one
    // undo step, multi-caret included.
    void blockIndent();
    void blockOutdent();

    // Undo/redo (M3). One step = one user action: typing runs coalesce into
    // a burst step (broken by Enter, caret moves and selection-replacing
    // edits), paste and over-selection typing are single compound steps.
    // undo() restores the caret/selection as it stood before the action;
    // redo() restores the state from when it was undone. setPlainText()
    // clears the history. Keys: Ctrl+Z, Ctrl+Shift+Z / Ctrl+Y.
    void undo();
    void redo();
    bool canUndo() const { return buffer_.canUndo(); }
    bool canRedo() const { return buffer_.canRedo(); }

    // Programmatic edit doors backing the parity binding (Phase F:
    // qtmonaco `insert_text`/`delete_line`). They ride the SAME user-edit
    // machinery as typed keys (undo bracketing, caret normalization,
    // surrogate safety) — and honor the M19 readOnly lock at entry (the
    // single-caret deleteSelection path reaches the buffer directly, so
    // these doors guard themselves). The Python layer pre-checks the flag
    // and raises ValueError there (qtmonaco's error semantics); these
    // doors stay silent, even the caret stays put.
    // insertTextAt moves the caret to just past the inserted text
    // (Monaco's executeEdits behavior); deleteLineAt removes the whole
    // line including its '\n' and parks the caret at the start of where
    // it was (clamped). Both no-op on invalid input.
    void insertTextAt(TextPosition pos, const QString& text);
    void deleteLineAt(int line);

    // M8 machine 5: the BATCH range-replacement door behind every edit a
    // SERVER primes (workspace/applyEdit, codeAction, formatting, rename).
    // It is replaceAll()'s walk made general: the batch is applied bottom-up
    // (DESCENDING by start offset), so the ranges still to come never shift,
    // and the whole batch is ONE undo group — one Ctrl+Z reverses a whole
    // server action. Ranges are the model frame (line + utf-16 column) and
    // end-EXCLUSIVE, exactly like Diagnostic/MatchRange; clamping is total
    // (line clamped into the document, column into the line, an inverted pair
    // swapped — M4's diagnostics discipline), so a generous range is read the
    // way a tolerant editor reads it and never faults. Multi-caret collapses
    // to one caret parked just past the TOPMOST applied edit (the replaceAll
    // rule); an empty batch applies nothing and pushes NO undo step. Returns
    // how many edits were applied — 0 when the batch is empty or the M19
    // readOnly lock holds (front-door silence, the caret stays put).
    struct TextEdit {
        int startLine = 0;
        int startCol = 0;
        int endLine = 0;
        int endCol = 0;
        QString newText;

        bool operator==(const TextEdit&) const = default; // test round-trips
    };
    int applyTextEdits(const QVector<TextEdit>& edits);

    // The qtmonaco set_cursor `moveToPosition` door. revealAt is reveal-
    // ONLY (never moves the caret — the binding calls setPosition first):
    // M13 fold-unfold of the target line, then vertical scroll per mode.
    // The three modes mirror the three Monaco reveal primitives (measured
    // against monaco.d.ts 0.52.2):
    //  - Center: revealPositionInCenter — ALWAYS scrolls, line's cell
    //    middle at the viewport middle.
    //  - NearTop: revealPositionNearTop — ALWAYS scrolls, line flush at
    //    the top edge, even when already visible (Monaco's doc-comment
    //    carries no "if outside viewport" clause; the conditional variant
    //    is the separate revealRangeNearTopIfOutsideViewport).
    //  - Minimal: revealPosition — conditional minimal edge-flush, no-op
    //    when visible (also the ensureCaretVisible leg used by caret moves).
    // All clamp through setVScroll (blank-space rules included).
    enum class RevealMode { Center, NearTop, Minimal };
    Q_ENUM(RevealMode)
    void revealAt(TextPosition pos, RevealMode mode);

    // Multiple carets (Phase D1). Alt+Click adds a caret, Alt+Drag drags a
    // selection for the caret under its start; typed edits, Enter,
    // Backspace/Delete, paste, cut and the navigation keys then act on
    // EVERY caret (one key press = one bracketed undo step; multi-caret
    // typing deliberately does NOT coalesce across presses). The "primary"
    // caret is the one position() reports and the one the mouse/keyboard
    // last owned; a plain click, setPosition, selectAll or Escape
    // collapses back to it. Carets normalize VS Code-style on add, drag
    // end, navigation and edits: duplicate carets fold, a caret inside
    // another slot's selection is dropped (the extra goes if the caret is
    // primary), and intersecting selections keep the LEFT one.
    int additionalCaretCount() const { return static_cast<int>(extraCarets_.size()); }
    QVector<TextPosition> additionalCarets() const; // caret positions, doc order
    void clearAdditionalCarets(); // collapse to the primary caret

    // Syntax highlighting (M4, Phase D2). Backed by the vendored
    // tree-sitter engine behind a private seam (view/HighlightEngine.hpp):
    // the public surface here stays plain-Qt so it remains
    // Shiboken-wrappable and tree-sitter never leaks through headers.
    // setLanguage("") or an unknown id disables highlighting; the widget
    // then paints everything in the default text color (same look as
    // before M4 - this is the grammar-less fallback path).
    // availableLanguages() = ids with a parser + highlights query.
    // qtmonaco parity note: the Python binding exposes this as
    // set_language()/get_language() with the same id strings.
    enum class TokenKind : quint16 {
        // Semantic roles mapped 1:1 from tree-sitter capture names (the
        // union of the vendored grammars' highlights.scm). Color mapping
        // is the caller's/theme's job; the view only assigns roles.
        Text = 0, // explicit: "default text color"; never emitted as a span
        Comment, Keyword, Number, String, StringSpecial, StringSpecialKey,
        Escape, Embedded, Constant, ConstantBuiltin, Constructor, Function,
        FunctionBuiltin, FunctionMethod, Type, Variable, VariableBuiltin,
        Property, Operator, Punctuation, PunctuationBracket,
        PunctuationDelimiter, PunctuationSpecial,
    };
    Q_ENUM(TokenKind)

    void setLanguage(const QString& language);
    QString language() const;
    static QStringList availableLanguages();

    // M23 language-scoped property overrides (Monaco's [languageId] model):
    // LANGUAGE override (when set) > editor options template > factory
    // default. languageProperties(id) hands out the per-language child
    // QObject, lazily created and OWNED by the editor (one per id); ids
    // normalize like setLanguage's (trimmed + lowercased), so the object
    // an IDE configures is exactly the one a later setLanguage matches.
    // The empty id addresses the plain-text (no-grammar) state. Changes
    // here AND language switches re-resolve every tab-width consumer live
    // (soft-tab insert, literal '\t' render grid, autoIndent math, M21
    // guide stops); clearTabStopWidth falls back to the template. The
    // built-in registry starts EMPTY: nothing changes until a consumer
    // sets an override. Full contract: view/LanguageProperties.hpp.
    LanguageProperties* languageProperties(const QString& language);

    // Merged, non-overlapping token spans intersecting [startOffset,
    // endOffset) (clamped), sorted by start - document UTF-16 offsets.
    // Forces a reparse+query if dirty. Primary consumer: tests (numeric
    // mapping assertions); the widget paints from the same engine state.
    struct HighlightSpan {
        int start = 0;
        int end = 0;
        TokenKind kind = TokenKind::Text;

        bool operator==(const HighlightSpan&) const = default; // test comparisons
    };
    QVector<HighlightSpan> highlightSpans(int startOffset, int endOffset);

    // Decorations + gutter (M5, Phase D3). Three layers, matching the
    // Gate-2 consumers that must be expressible WITHOUT new view primitives
    // (PLAN.md): (1) gutter glyph layer - breakpoint dots, diagnostic
    // severity icons (fold triangles join in M13), (2) whole-line layer -
    // highlighted-line bands, current-line band, (3) inline text layer -
    // diagnostic wavy underlines, inlay hints. All APIs are batch
    // replace-all: add = set, update = set again, remove = set without the
    // item - which keeps the state round-trip trivial and LSP/DAP glue a
    // thin consumer. Positions are the same 0-based UTF-16 (line, col) as
    // everywhere; stored decorations are NOT clamped to the document
    // (edits move lineCount - out-of-range items simply never paint).
    //
    // qtmonaco parity (binding converts 1-based): set_line_numbers_mode <->
    // setLineNumbersMode, set_highlighted_lines/clear_highlighted_lines <->
    // setHighlightedLines/clearHighlightedLines.

    // (1) Gutter line numbers. Default On (Monaco parity).
    enum class LineNumbersMode { Off, On, Relative, Interval };
    Q_ENUM(LineNumbersMode)
    void setLineNumbersMode(LineNumbersMode mode);
    LineNumbersMode lineNumbersMode() const { return lineNumbersMode_; }

    // What the gutter displays for a 0-based line: On = line+1; Relative =
    // distance to the caret line (the caret line itself shows its absolute
    // number); Interval = the absolute number only on 1-based multiples of
    // 10, empty string elsewhere (Monaco's interval behavior). Empty
    // string also means "draw nothing here" (Off mode, invalid line).
    QString lineNumberText(int line) const;

    // Gutter geometry. gutterWidth() = 0 in Off mode; otherwise digit
    // width + padding, plus the fold zone (M13: a fixed strip along the
    // gutter's right edge where the fold triangles live — reserved
    // whenever the gutter is on, so folding never shifts the numbers),
    // plus one extra reserved slot while any breakpoint exists (adding a
    // breakpoint must not shift already-drawn numbers).
    // contentLeft() = widget x where text begins: padding_ + gutterWidth().
    // xForCol()/positionAt()/painting all use it, so callers composing
    // widget x read contentLeft(), not padding().
    int gutterWidth() const;
    int contentLeft() const { return padding_ + gutterWidth(); }
    // The M13 fold zone: fixed-width strip, 0 in Off mode. Its x is
    // foldZoneLeft(): [contentLeft()-foldZoneWidth(), contentLeft()) in v1,
    // pinned to the gutter's LEFT edge while M10 diff mode carries the
    // original column (see foldZoneLeft()). Clicks inside it toggle the
    // fold of that row's line and never move the caret.
    int foldZoneWidth() const;
    // The zone's widget-x left edge (v1: contentLeft()-foldZoneWidth();
    // diff mode: padding_ + the breakpoint slot) — paint, hit and hover
    // all read the ONE door, so the frames can never drift.
    int foldZoneLeft() const;

    // (1) Breakpoint glyphs: one filled dot per line, in the reserved
    // slot. Getter returns the sorted, deduplicated lines.
    void setBreakpointLines(const QVector<int>& lines);
    QVector<int> breakpointLines() const { return breakpointLines_; }

    // (3) Diagnostics: wavy underline over [start..end] (clamped to the
    // line when painting) + a severity icon in the gutter anchored at
    // startLine. Stored sorted by start. Message is carried for consumers
    // (hover UI is Gate-2 glue); it is not rendered by the view in v1.
    enum class Severity { Error, Warning, Information, Hint };
    Q_ENUM(Severity)
    struct Diagnostic {
        int startLine = 0;
        int startCol = 0;
        int endLine = 0;
        int endCol = 0;
        Severity severity = Severity::Error;
        QString message;
        // The producer label (the LSP `source` field, "clangd" & friends),
        // carried OPAQUELY since the post-1.8.0 shared-HOVER sliver: the
        // painter ignores it; the LSP glue feeds it from the wire and its
        // merged hover popup prints it as `[source]`. Empty = unlabeled.
        QString source;

        bool operator==(const Diagnostic&) const = default; // test round-trips
    };
    void setDiagnostics(const QVector<Diagnostic>& diagnostics);
    QVector<Diagnostic> diagnostics() const { return diagnostics_; }

    // (3) Inlay hints: `text` painted in hint color at (line, col) WITHOUT
    // re-flowing the following text - the caller (e.g. a DAP consumer)
    // anchors hints into existing whitespace. v1 renders inline after the
    // text pass; adding real re-flow later is a rendering refinement, NOT
    // an API change. Getter returns the sorted hints.
    struct InlayHint {
        int line = 0;
        int col = 0;
        QString text;

        bool operator==(const InlayHint&) const = default;
    };
    void setInlayHints(const QVector<InlayHint>& hints);
    QVector<InlayHint> inlayHints() const { return inlayHints_; }

    // (2) Whole-line bands, drawn UNDER selections. Inclusive 0-based
    // range (the binding subtracts 1 from qtmonaco's 1-based pair; end<
    // start is swapped). Getters report -1 when unset.
    void setHighlightedLines(int startLine, int endLine);
    void clearHighlightedLines();
    int highlightedLinesStart() const { return highlightStart_; }
    int highlightedLinesEnd() const { return highlightEnd_; }

    // (2) Current-line band (Monaco's renderLineHighlight). M18: the
    // mode enum replaces the old bool; the bool setter survives as a
    // delegate (true→Background, false→Off) and currentLineHighlight()
    // reads "mode != Off". Default Background (the band ships on now;
    // the wash ink was strengthened in the same pass). BackgroundFrame
    // adds a 1px outline around the band.
    enum class CurrentLineMode { Off, Background, BackgroundFrame };
    Q_ENUM(CurrentLineMode)
    void setCurrentLineMode(CurrentLineMode mode);
    CurrentLineMode currentLineMode() const { return currentLineMode_; }
    void setCurrentLineHighlight(bool enabled)
        { setCurrentLineMode(enabled ? CurrentLineMode::Background
                                     : CurrentLineMode::Off); }
    bool currentLineHighlight() const
        { return currentLineMode_ != CurrentLineMode::Off; }

    // M18 long-line ruler (VS Code's editor.rulers): one 1px vertical
    // guide at a fixed TEXT column. It rides h-scroll and is chrome
    // independent (its x = contentLeft() + col*cellWidth() - hScroll,
    // drawn only inside the text viewport — never over the gutter or
    // the overview ruler, and it never changes any scroll range).
    // 0 or negative = off; default off.
    void setLongLineRulerColumn(int column);
    int longLineRulerColumn() const { return longLineRulerCol_; }
    // M18 auto-close pairs (behavior; the on/off toggle lives on
    // options(), M19): typing one of ()[]{}""''``  with a selection
    // WRAPS it; at a bare caret it inserts the pair and parks the caret
    // between; typing a closer that is already right of the caret JUMPS
    // over it instead (no insert, no undo entry); a quote inside or
    // right beside a word char inserts plain (the apostrophe guard,
    // VS Code model — brackets always pair). Backspace with the caret
    // between an empty pair removes BOTH sides. Fans out per caret like
    // every other edit; one bracketed undo group per multi press.

    // M19 settings object: ONE child QObject mirroring every editor knob
    // (gutter mode, current line, long-line ruler, tab stop, auto-indent,
    // tab-changes-focus, overview ruler, scroll-past-end) PLUS the two
    // M19-only settings (auto-close toggle, read-only mode), each with a
    // *Changed signal. The editor is the single source of truth: options
    // setters delegate here (existing validation/invalidation stays), and
    // the editor emits the signal — so the flat setters and the options
    // object are two doors into the same room, and options never go
    // stale. Owned by the editor (parented child); one per editor.
    // Full contract: view/CodeEditorOptions.hpp.
    CodeEditorOptions* options() const { return options_; }

    // M24 theme object: ONE child QObject holding every painted ink of
    // this canvas — an explicit QColor per chrome Role and per TokenKind,
    // unset keys falling back LIVE to the pre-M24 sources (QPalette roles
    // with their historical alpha nudges, and the fixed chrome constants;
    // token fallback stays HighlightEngine::colorFor's table, invalid kind
    // = default pen, unchanged). Presets Default/Light/Dark swap whole
    // maps (applyPreset); any manual edit moves kind() to Custom. Every
    // real change emits themeChanged, and the editor repaints on it with
    // a geo-cache wipe (token ink rides the cached format ranges). Full
    // contract: view/CodeEditorTheme.hpp.
    CodeEditorTheme* theme() const { return theme_; }

    // M25 document object (Gate 2): the editor-owned text model — the
    // TextBuffer now lives in it (the sanctioned ownership deviation,
    // AGENTS.md), plus the URI door and the modified flag. ALWAYS non-null
    // (the ctor creates one and attaches it; same lifetime pattern as
    // options()/theme()). The editor's text doors keep working verbatim;
    // they now operate on the document's buffer. One editor per document
    // (v1); full contract: view/CodeDocument.hpp.
    CodeDocument* document() const { return document_; }

    // Indentation + auto-indent (M14). SOFT TABS ONLY: Tab typing never
    // puts literal '\t' into the buffer; literal tabs (paste, setPlainText)
    // stay in the text and RENDER on the same stop grid. All three knobs
    // are freeze-bound (Phase F binds the qtmonaco indent surface).
    //
    // Tab stop width: Tab advances the caret to the next column that is a
    // multiple of this width (counted on the stop grid from the line
    // start, so a literal '\t' left of the caret counts as its own grid
    // advance), inserting exactly that many spaces; Shift+Tab is the VS
    // Code "unindent": it retires the touched whole lines to the next
    // lower stop boundary. Default 4 (Monaco's tabSize). Also drives the
    // RENDER width of literal '\t' in the text (stops at every N cells
    // from the line start), so typed and pasted indentation line up on
    // one grid. Clamped to >= 1.
    //
    // M23 layering: this flat setter (and the options() mirror) set the
    // GLOBAL TEMPLATE; tabStopWidth() reads the EFFECTIVE value — the
    // active language's override wins while set (see languageProperties);
    // with no override the two coincide. The options object and the demo
    // EditorSettings round-trip keep reading/writing the TEMPLATE, so a
    // language override can never leak a per-language value into global
    // saved settings.
    void setTabStopWidth(int columns);
    int tabStopWidth() const { return tabStopWidth_; } // the EFFECTIVE stop

    // Enter auto-indent (default ON): the new line copies the current
    // line's leading whitespace; a trailing opener `{([:` right before the
    // caret (whitespace skipped) adds one more stop — Enter AT col 0 never
    // does. When the caret sits flush inside an empty pair it SPLITS:
    // `{|}`, `}|{` (also () and []), `{|` at EOL — which CONJURES the
    // missing closer — and `|}` at col 0; the closer parks on its own line
    // at the parent indent and the caret stays on the child line.
    // Alt+Enter and typed/pasted '\n' never auto-indent (plain newline).
    void setAutoIndentEnabled(bool enabled);
    bool autoIndentEnabled() const { return autoIndent_; }

    // Tab as focus traversal (default false): with ON, Tab/Shift+Tab are
    // ignored and ride Qt's focus chain — the a11y escape hatch out of
    // the editor. With OFF, Tab indents (see above).
    void setTabChangesFocus(bool enabled);
    bool tabChangesFocus() const { return tabChangesFocus_; }

    // Find & replace (M6, Phase D4). The engine lives in the widget (a
    // drop-in of Chromium-Monaco must not lose its find UX); the built-in
    // Ctrl+F / Ctrl+H bar is the primary UI, every action below is also
    // public so consumers can drive their own find surface. Ctrl+F opens
    // the find row, Ctrl+H the replace row, F3/Shift+F3 cycle matches even
    // while the bar is closed (the query persists); Enter in the find row
    // = next, Shift+Enter = previous, Enter in the replace row replaces
    // the current match then advances. The confirm step for replace-all
    // is the button itself: Enter never fires it, and the whole batch
    // lands as ONE undoable D1 action (undo once restores everything).
    //
    // Engine semantics (v1): the search is PER-LINE - matches never cross
    // a line boundary (a pattern containing a newline matches nothing).
    // Literal queries are escaped into the same QRegularExpression path.
    // Match bounds snap outward over split surrogate pairs and duplicate
    // matches merge (PCRE2 in code-unit mode steps through a surrogate
    // pair one unit at a time), so replacing a match never orphans half a
    // pair. An invalid pattern clears the match set (findPatternValid()
    // tells "invalid" from "no matches"). Matches recompute lazily: an
    // edit marks the set stale and the next paint (or find action)
    // refreshes it - the same at-most-once-per-keypress cadence as the
    // M4 highlight engine; highlights and the match count stay live
    // through edits. Painting is viewport-only: the match list is sorted
    // and the paint pass binary-searches the window.
    struct MatchRange {
        int startLine = 0;
        int startCol = 0;
        int endLine = 0; // == startLine while the engine is per-line
        int endCol = 0;  // [start, end) half-open, UTF-16 code units

        bool operator==(const MatchRange&) const = default; // test round-trips
    };

    // Flags mirror the bar's toggles. Default: MatchCase (Monaco parity).
    // The Q_DECLARE_FLAGS/Q_FLAG trio is the same modern Qt6 flags pattern
    // PySide6 itself wraps, so the binding gets a natural Python flag set.
    // The enumerators must be EXPLICIT bit masks: Qt6 QFlags stores the
    // enumerator value as the mask as-is (no auto 1<<n shift), so an
    // implicit first entry (value 0) would make MatchCase an empty mask and
    // testFlag(MatchCase) mean "no flags at all" - measured in this Qt
    // 6.12's qflags.h. Powers of two are also what PySide6 exposes.
    enum class FindFlag { MatchCase = 1 << 0, WholeWord = 1 << 1, UseRegex = 1 << 2 };
    Q_DECLARE_FLAGS(FindFlags, FindFlag)
    Q_FLAG(FindFlags)

    void setFindText(const QString& text); // recomputes matches
    QString findText() const { return findText_; }
    void setFindFlags(FindFlags flags); // recomputes matches
    FindFlags findFlags() const { return findFlags_; }
    bool findPatternValid() const { return findPatternValid_; }

    // Match set state. matches() is the full sorted list (model ranges);
    // note it can be stale after an edit until the next paint/action -
    // matchCount()/currentMatch() are the refresh-on-read entry points.
    // currentMatch() reports the EFFECTIVE current match: the index in the
    // list only while the selection covers it, -1 otherwise (a click or
    // typing moves on, Monaco-style). A fresh set has none until a find
    // action selects one.
    QVector<MatchRange> matches() const { return matches_; }
    int matchCount(); // refreshes a stale set (paint is viewport-only)
    int currentMatch();
    // Select + reveal the match at index (no wrap checks outside range);
    // false when the index is stale (e.g. after a refresh emptied the set).
    bool gotoMatch(int index);
    bool findNext();     // first match after the selection end, wrapping
    bool findPrevious(); // first match before the selection start, wrapping

    // Replacements. replaceNext(): current match, or next at/after the
    // caret when none; one bracketed edit (own undo step), caret lands at
    // the replacement end and the set refreshes. replaceAll(): whole
    // document, replacements applied back-to-front so offsets stay valid,
    // ONE undo group restores the batch; returns the replaced count.
    bool replaceNext(const QString& replacement);
    int replaceAll(const QString& replacement);

    // Built-in bar (lazy child widgets). showFindBar()/showReplaceBar()
    // open it and focus the respective field, selectively selecting the
    // remembered query; Esc or the X button hides it and returns focus
    // (the query and highlights persist until clearFind()).
    void showFindBar();
    void showReplaceBar();
    void hideFindBar();
    bool findBarVisible() const;
    // Clears query + flags + matches (the bar text fields included);
    // does not hide the bar.
    void clearFind();

    // --- M8 machine 6: hover + completion popups (PLAN's popup contract) --
    // View-INTERNAL popup widgets — the M6 FindBar precedent: lazy child
    // QWidgets defined inside CodeEditor.cpp (internal linkage; only the
    // records below and these doors cross the Shiboken boundary). The view
    // NEVER asks a server for anything: all triggering is GLUE-side — an app
    // wires the hub's feature replies to these doors (PLAN). Content is
    // PLAIN TEXT (no markdown renderer, PLAN): the app hands the string it
    // already extracted from the hover markup.
    //
    // Anchors ride the EXISTING geometry (PLAN: no new mapping primitives):
    // x from xForCol, y from the row-slot math every painted row uses (the
    // CodeEditorAccessible characterRect formula). A popup FLIPS above its
    // anchor row when it would run past the bottom edge, then clamps into
    // the editor rect; a fully visible popup is never moved by the clamp
    // (that identity is what makes anchor assertions exact). Scroll and
    // resize RE-PLACE (popups track their frozen anchors).
    //
    // Auto-hide (PLAN): ANY key press in the editor, ANY mouse press, and
    // focus leaving the editor — WITH ONE EXCEPTION since the post-1.8.0
    // ACCEPT sliver: the completion list OWNS certain keys and its own rows
    // (see showCompletionList below). A press on the editor, anywhere on the
    // hover popup, or on the completion popup's chrome around its rows still
    // dismisses BOTH. An EMPTY payload on either show door HIDES (a glue
    // re-trigger with nothing to show is the same dismissal). Neither door
    // ever takes focus from the editor (WA_ShowWithoutActivating + NoFocus
    // on the popup and its content).
    struct CompletionItem {
        QString label;  // the row's primary text
        QString detail; // appended after two spaces when set (plain text)
        // The ACCEPT payload (post-1.8.0 sliver): what the FEATURE GLUE
        // applies when the user accepts this row (see completionAccepted
        // below). The view carries it OPAQUELY and never applies it itself
        // (the display-only contract): the glue decodes it from whatever the
        // server sent (an LSP `textEdit`, insert range first), or synthesizes
        // a zero-width insertion at the request caret, and silently refuses
        // an all-zero (payload-less) accept. Defaults all-zero = no payload.
        TextEdit textEdit;

        bool operator==(const CompletionItem&) const = default; // test round-trips
    };
    struct HoverRange {
        int startLine = 0;
        int startCol = 0; // model frame: line + utf-16 columns, [start, end)
        int endLine = 0;  // — the same frame Diagnostic/TextEdit use
        int endCol = 0;

        bool operator==(const HoverRange&) const = default;
    };
    // The list anchors at the CARET's position AT SHOW TIME — later caret
    // moves never drag it (the dismissal is a hide, not a follow). An empty
    // list hides. Re-showing while visible replaces the rows IN PLACE and
    // re-highlights the FIRST row.
    //
    // ACCEPT (post-1.8.0 sliver, the VS Code shape): while the list is
    // visible the view routes Up/Down/PageUp/PageDown/Home/End to the ROWS
    // (the caret does NOT move), Enter/Tab accept the current row, and a
    // click-release ON A ROW accepts it (a press alone only SELECTS that row,
    // no dismissal). Every accept route hides the list and emits
    // completionAccepted exactly once — the view NEVER inserts anything
    // itself; applying item.textEdit is the glue's job (and re-triggering
    // after the edit rides the glue's debounce). Escape hides ONLY the
    // completion list (a visible hover popup survives); any other key keeps
    // the m6 rule: dismiss both popups and type as normal.
    void showCompletionList(const QVector<CompletionItem>& items);
    void hideCompletionList();
    bool completionListVisible() const;
    // The hover aligns with the range START (x, via xForCol) and hangs below
    // the row of the range END — the whole anchor stays uncovered. Empty
    // text hides; re-showing replaces the text in place.
    void showHover(const HoverRange& range, const QString& plainText);
    void hideHover();
    bool hoverVisible() const;

    // Overview ruler (M7, Phase E1) — the minimap substitute: a right-edge
    // strip in which the WHOLE document is scaled to pixels (model px
    // space [0, 2*padding + lineCount*lineHeight) maps linearly onto the
    // strip height). It paints sampled line stripes (one stripe per strip
    // pixel-row budget, never more — the document is never rendered
    // off-screen: cost is O(strip height), not O(document)), the marker
    // layers (breakpoints, diagnostics, highlighted lines, search matches
    // — subsampled to the strip when huge) and the viewport rectangle.
    // Click/drag inside the strip scrolls (the clicked line centers in the
    // viewport); it never moves the caret. The vertical scrollbar
    // overlays the strip's right edge, VS Code-style. The Python binding
    // maps qtmonaco's set_minimap_enabled here — never onto a
    // Chromium-style token minimap.
    static constexpr int kOverviewRulerWidth = 44; // px; M24 themes paint its
                                                   // inks, the METRIC stays a
                                                   // constant (chrome geometry
                                                   // is not a theme knob)
    void setOverviewRulerEnabled(bool enabled);
    bool overviewRulerEnabled() const { return rulerEnabled_; }
    int overviewRulerWidth() const { return rulerEnabled_ ? kOverviewRulerWidth : 0; }
    QRect overviewRulerGeometry() const; // empty rect when disabled
    // Pure mapping math (numeric contract, drives the click gesture):
    // doc line under a widget-space y inside the strip; -1 when disabled.
    int rulerLineAt(int y) const;
    // Rectangle marking the visible window inside the strip (empty when
    // disabled). The DoD "ruler geometry state" entry point.
    QRect overviewRulerViewportRect() const;

    // Horizontal scrolling (M9, Phase E1) — the h-scroll half of the Gate-1
    // viewport story (soft wrap stays deferred to Gate 2). The text window
    // runs from contentLeft() to the overview-ruler chrome, and the offset
    // is in pixels, the same integer space vScroll lives in (the monospace
    // grid tests assert exact offsets against cellWidth arithmetic).
    void setHorizontalScrollOffset(int pixels); // clamped (see max…)
    int horizontalScrollOffset() const { return hScroll_; }
    int horizontalViewportWidth() const; // visible text px, ruler excluded
    // The clamp contract: max offset = docWidth + padding - window. The
    // doc width is AMBIENT: a line's measured px width joins the extent
    // when painting/caret/hit-testing visits it, and the extent is the max
    // over visited lines — the view never walks the whole document to find
    // the widest line (same no-O(document) rule as the ruler, so the bar
    // grows while the doc is being visited; stale widths after edits
    // decay the same ambient way). Reset on setPlainText.
    int maxHorizontalScroll() const;
    // Parity hook: qtmonaco's set_scroll_beyond_last_line_enabled
    // (Monaco's scrollBeyondLastLine option). On, the doc pixel space
    // grows by height() - lineHeight_, letting the LAST line scroll up to
    // the viewport top (and the ruler mapping spans that taller space).
    // Default OFF — a deliberate divergence from Monaco's ON default that
    // keeps every existing end-of-scroll contract (ruler mapping
    // included) bit-stable; revisit the default before the API freeze.
    void setScrollBeyondLastLineEnabled(bool enabled);
    bool scrollBeyondLastLineEnabled() const { return scrollBeyondLastLine_; }

    // Code folding (M13, Phase E2). A fold is an inclusive pair of MODEL
    // lines (0-based) with startLine < endLine; collapsing it hides
    // (startLine, endLine] — the header line never disappears (js/json
    // closing braces hide with their body, python blocks end at the last
    // body line). Foldable starts come from the engine's @fold captures
    // whenever a fold grammar is active; the view's indent rule (the next
    // visible line is deeper than this one) is the fallback for
    // grammar-LESS modes only — in js/json/python nesting is what the
    // brackets/syntax say, and leading whitespace alone must never fake
    // python-style blocks there (owner report; see PLAN E2).
    //
    // Fold state is VIEW state: not undoable in v1, kept index-correct
    // across edits on a best-effort sweep (a fold whose body was deleted
    // vanishes), and cleared entirely by setPlainText/set_language-level
    // document replaces. A collapsed fold also AUTO-EXPANDS when an edit
    // kills its source (the grammar capture or indent pair that makes the
    // line foldable is gone): the body reappears, mirroring the way the
    // same edit just drops the marker of an unfolded fold — the asymmetry
    // that otherwise leaves ▸ + hidden rows on a block nothing folds.
    // Hidden lines have no visible index: use the view-line map below
    // before addressing any geometry.
    struct FoldRange {
        FoldRange() = default;
        FoldRange(int startLine_, int endLine_) : startLine(startLine_), endLine(endLine_) {}
        int startLine = 0;
        int endLine = 0;
        bool operator==(const FoldRange&) const = default;
    };

    // Foldable ranges as the view currently knows them: the active source
    // (grammar captures, or the indent fallback in grammar-less modes),
    // sorted by start, duplicate starts resolved. Starts ride the paint
    // cache (viewport-scoped on big documents: an out-of-window fold is
    // discovered exactly when scrolling reveals it) but the ENDS are
    // always re-resolved exactly at call time — same tightest end
    // foldLine uses, paying whatever parse escalation foldsFull() needs
    // (an explicit-caller cost, never a paint cost). Non-const for that.
    QVector<FoldRange> foldableRanges();
    bool isFoldable(int line) const; // would foldLine(line) find a fold?
    bool isFolded(int line) const;   // line starts a currently collapsed fold

    // Toggle on a foldable/collapsed START line. Targets hidden inside
    // another fold do nothing (return false): the caret-based entry points
    // unfold to reveal first, the mouse can only ever hit visible lines.
    // Returns true if the fold state changed. Keyboard: Ctrl+Shift+[ / ]
    // fold/unfold at the caret, Ctrl+Alt+[ / ] collapse/expand everything.
    bool foldLine(int line);
    bool unfoldLine(int line);
    bool toggleFold(int line);
    // Explicit whole-document passes (O(document) is acceptable here —
    // never on a paint path): collapse every outermost fold, then every
    // fold; expandAll unfolds everything.
    void collapseAll();
    void expandAll();

    // --- M10 diff view (Gate 2; semantics: PLAN GATE 2 §M10) ---------------
    // Diff mode puts TWO documents in ONE widget (the "one editor per
    // document" contract untouched): the editor's OWN attached document IS
    // the modified side, so caret/undo/find/fold/textChanged/toPlainText/
    // readOnly speak it natively; the original side is a STANDALONE
    // (detached) CodeDocument and is never editable through any door of
    // this widget. The pointer door requires a detached `original` and
    // keeps it NON-OWNING (a destroyed() hookup clears diff mode); a
    // `modified` pointer different from document() is a one-shot text feed
    // through the editor's replace machinery. The string door feeds both
    // (original into a lazy editor-owned child standalone doc). `language`
    // is ONE language for the whole diff view (routes through setLanguage;
    // "" = plain).
    //
    // While diff mode is active the visible* row doors below speak the
    // ALIGNMENT ROW axis (blank cells are weight-1 pseudo-rows, a
    // collapsed fold zeroes the rows whose MODIFIED line it hides, soft
    // wrap is inert) — and they speak MODIFIED line numbers (the caret
    // space; original-side geometry is the paint side's business).
    void setDiffDocuments(CodeDocument* original, CodeDocument* modified,
                          const QString& language);
    void setDiffDocuments(const QString& original, const QString& modified,
                          const QString& language);
    void clearDiffDocuments();
    bool isDiffMode() const;
    // Hunk navigation: revealAt + caret to the change's MODIFIED-side line.
    // Strict-edge rule: from any line at/inside a hunk, nextChange goes to
    // the NEXT hunk (previousChange mirrors); NO wrap-around at the
    // document edges — out of hunks it is a no-op (documented divergence
    // from Monaco's cyclic navigation).
    void nextChange();
    void previousChange();
    // M10 machine 3: the ORIGINAL column is a read-only canvas with ONE
    // sanctioned data path — click/drag/double/triple selection in
    // ORIGINAL-document coordinates + copy(). This door reports that
    // selection's text ('' = none): non-empty while an original-column
    // selection is live, and the ONLY way original text leaves the column
    // (keys, edits, cut and paste never touch it — the modified side stays
    // the editable truth). The selection is dropped by any modified-side
    // gesture (click, key press) or by copying.
    QString originalSelectionText() const;
    // The chrome block's sub-widths (0 while diff mode is off): the read
    // window, the ORIGINAL number column and the connector gap — the one
    // definition diffFrame() lays out and gutterWidth() sums, exposed so
    // integrators (and the pixel tests) sample the exact frame without
    // duplicating the math. No mapping/geometry PRIMITIVE is added: the
    // frames stay the view's business (Gate-1.5 additive precedent).
    int diffChromeTextCol() const; // the ORIGINAL read window's width
    int diffChromeDigitsCol() const; // the ORIGINAL number column's width
    int diffChromeGap() const; // the connector strip between the columns
    int diffChromeWidth() const; // the sum above (the gutter's diff term)

    // The ONE view-line map (M13): visible slots are numbered
    // [0, visibleLineTotal()) top-to-bottom; a hidden (folded-away) model
    // line has visible index -1. Paint, caret navigation, hit-testing,
    // scroll clamps and the ruler all route through it, so model space and
    // screen space can never drift apart across a fold boundary.
    int visibleLineTotal() const;
    int visibleIndexOfModelLine(int line) const; // -1 = hidden
    int modelLineForVisibleIndex(int visibleIndex) const; // clamped both ways

    int lineHeight() const { return lineHeight_; }
    double cellWidth() const { return cellWidth_; }
    int padding() const { return padding_; } // vertical/top-edge padding

    // Viewport state read-out (the same window paintEvent and the
    // highlight policy consume). Read-only: scrolling itself happens via
    // wheel/scrollbar. Public for consumers, bindings and the policy test.
    // firstVisibleLine() reports the MODEL line of the top visible slot
    // (fold-aware since M13: across a collapsed fold it jumps to the line
    // after the hidden run; hidden lines are never reported);
    // visibleLineCount() is the slot budget the paint loop walks (a
    // scroll-space measure, not the hidden-adjusted total — see
    // visibleLineTotal() for that).
    int firstVisibleLine() const;
    int visibleLineCount() const;
    int verticalScrollOffset() const { return vScroll_; } // px, clamped

    // Widget-space x of the caret position at (line, col), following the
    // glyph engine's cluster advances (not a col * cellWidth grid), gutter
    // included (so xForCol(line, 0) == contentLeft() while hScroll is 0).
    // Shifted LEFT by the current horizontalScrollOffset() — it is the
    // widget x the user would see (off-screen positions go negative or past
    // the right edge, exactly like the painted glyph). A col
    // inside a grapheme cluster is snapped to a cluster edge by the engine;
    // the text position itself is untouched. Out-of-range line/col clamp.
    // Public because decorations (M5: diagnostics, inlay hints) need the
    // same geometry as the caret.
    double xForCol(int line, int col) const;

    // Text position for a widget-space point (used by mouse handling and
    // wrappable for bindings). Cols inside a cluster resolve to the nearer
    // cluster edge; x beyond the line's laid-out width resolves to EOL.
    TextPosition positionAt(const QPoint& widgetPos) const;

    // --- M17 code font + zoom ----------------------------------------------
    // The EFFECTIVE code font = codeFont() scaled by zoomFactor():
    // setCodeFont() stores the BASE, the zoom multiplies it (one knob each,
    // VS Code model). Changing either re-measures cell/line metrics and
    // wipes the glyph cache (the font is NOT part of the geo-cache key);
    // everything derived (gutter, fold zone, hit testing, paint baseline,
    // scroll ranges) re-reads the live metrics, and QWidget::font() always
    // mirrors the EFFECTIVE font (scaled to an integer pixel size, hinting
    // stripped). A raw QWidget::setFont still works: changeEvent recognizes
    // the FOREIGN font and REBASES it (new baseFont_ at zoom 1.0), so
    // codeFont() never lies about what is drawn and later zoomIn/Out scale
    // from there. codeFont() returns the BASE (as last set), not the zoomed
    // font — zoomed geometry is observable via cellWidth()/lineHeight()/
    // QWidget::font(). Shiboken-clean: QFont/double only, no templates.
    // M20: both knobs also mirror on CodeEditorOptions (setCodeFont/
    // setZoomFactor/codeFont/zoomFactor + codeFontChanged/zoomFactorChanged
    // on the notify bus); the editor emits from EVERY door — the flat
    // setters, the zoom chords riding setZoomFactor, and the foreign
    // QWidget::setFont rebase (each changed half announced once, same-
    // value sets stay silent per the M19 no-op rule).
    void setCodeFont(const QFont& font);
    QFont codeFont() const { return baseFont_; }
    // Zoom factor over the base font, clamped to [kZoomFactorMin,
    // kZoomFactorMax]; zoomIn/Out multiply/divide by kZoomFactorStep,
    // resetZoom() == setZoomFactor(1.0). Chords (VS Code): Ctrl+= /
    // Ctrl+- / Ctrl+0 plus Ctrl+mouse-wheel (here that is the FONT zoom —
    // there is no Chromium-style UI chrome zoom).
    static constexpr double kZoomFactorStep = 1.25;
    static constexpr double kZoomFactorMin = 0.25;
    static constexpr double kZoomFactorMax = 4.0;
    void setZoomFactor(double factor);
    double zoomFactor() const { return zoomFactor_; }
    void zoomIn() { setZoomFactor(zoomFactor_ * kZoomFactorStep); }
    void zoomOut() { setZoomFactor(zoomFactor_ / kZoomFactorStep); }
    void resetZoom() { setZoomFactor(1.0); }

    // --- M8 parity wrap (2026-11-19): custom actions + stored config ------
    // addContextAction(id, label): appends a custom item after the built-in
    // block of the M16 context menu; triggering it fires
    // contextActionTriggered(id). IDEMPOTENT by id: re-adding an id only
    // REPLACES its label, the entry keeps its slot (one entry per id; the
    // trigger key never changes). Actions carry no state — the consumer owns
    // every verb. An EMPTY id is the honest silent local refuse (an action
    // that could never announce itself). NOTE the deliberate name: NOT
    // `addAction`, which would hide QWidget's addAction overloads — the
    // qtmonaco door `add_action` maps here in the Python shim. qtmonaco's
    // `language` precondition is a SHIM parameter and deliberately absent
    // from this door (the native language scoping does the job — the
    // PLAN-decided divergence).
    void addContextAction(const QString& id, const QString& label);
    // The ids in registration order — the cheap readable seam (ids are the
    // trigger keys; labels are visible in the menu itself). QStringList, no
    // new record type crosses the API.
    QStringList contextActionIds() const;

    // The stored workspace-configuration buffer — the view door behind
    // qtmonaco's `update_workspace_configuration(dict)`. The buffer is a
    // compact-JSON QString; the view NEVER parses or applies it. The ONE
    // consumer is the app-side LSP glue (LspHub): it pushes the buffer to
    // the server as `workspace/didChangeConfiguration` after the session's
    // READY and RE-pushes on this signal — qtmonaco's "buffered until
    // init" semantics live HUB-side (lazy start: the buffer waits on the
    // view until the client comes up), not here. Default empty = never set
    // = the spec default (the glue sends NOTHING for an empty buffer).
    void setStoredWorkspaceConfiguration(const QString& settingsJson);
    QString storedWorkspaceConfiguration() const { return storedConfig_; }

    PerfStats perfStats() const { return perf_; }
    void resetPerfStats() { perf_ = {}; }

signals:
    void cursorMoved(TextPosition pos);
    void selectionChanged();
    // Every document mutation, programmatic and typed, undo/redo replay
    // included (it rides the core EditEvent observer: exactly one emit per
    // primitive mutation, none on no-op sets). The detail-free change ping
    // the parity binding mirrors into qtmonaco's `text_changed(str)`.
    void textChanged();
    // M8 parity wrap: the id of a triggered custom context-menu action
    // (the relay source for qtmonaco's `context_menu_action_triggered`).
    void contextActionTriggered(const QString& id);
    // The stored workspace-configuration buffer fired EXACTLY on a real
    // change (the M23 changed-not-touched discipline). This is the signal
    // the LSP glue turns into a live didChangeConfiguration re-push.
    void workspaceConfigurationChanged();
    // The row the user ACCEPTED in a visible completion list (Enter/Tab on
    // the current row, or a click-release on that row — see
    // showCompletionList). `index` is the row index; `item` is the row's
    // record and carries the textEdit payload OPAQUELY: applying it
    // (applyTextEdits) and re-triggering are the glue's job — the view never
    // mutates the document through accept (the display-only contract).
    void completionAccepted(int index, const CompletionItem& item);

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    // M16 context menu + text DnD.
    void contextMenuEvent(QContextMenuEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    // Post-1.0 DnD polish: the target caret must not linger when the drag
    // leaves or cancels over us (QWidget's default impl IGNORES the event;
    // this one clears the mark and accepts).
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    // M22: the fold-hover region must not linger when the pointer exits the
    // widget with no further move events to re-resolve it (leave clears —
    // and per the dragLeave lesson the clear never RELIES on this arriving:
    // press and every fold/edit mutation clear it too).
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;
    QSize sizeHint() const override;

private:
    // --- multiple carets (Phase D1) ---------------------------------------
    // A caret plus its optional selection. The primary caret keeps living
    // in the classic caret_/anchor_/hasSelection_ fields (single-caret
    // semantics unchanged); extraCarets_ holds the additional slots in
    // document order. All fan-out helpers treat the union.
    struct CaretSlot {
        TextPosition caret;
        TextPosition anchor;
        bool hasSelection = false;
    };

    // --- undo mirror (M3) ------------------------------------------------
    // The core owns the op log (see core/TextBuffer.hpp); the view stacks
    // caret/selection states 1:1 underneath it: undoMarks_[i] is the state
    // before core undo-group i (front-aligned after core cap-drops), and
    // redoMarks_[j] is the state an undone group left behind. Alignment is
    // reconciled after every mutating action via (undoDepth, undoPushSerial)
    // deltas, which distinguish "coalesced into top" from "pushed, oldest
    // dropped at the cap" from "nothing recorded".
    struct UndoMark {
        TextPosition caret;
        TextPosition anchor;
        bool hasSelection = false;
        // Multi-caret (D1): the additional slots as they stood before the
        // action. A multi-caret key press is ONE bracketed core group, so
        // one mark covers all carets and undo/redo restores every caret.
        QVector<CaretSlot> extras;
    };
    UndoMark captureEditMark() const;
    void reconcileUndoMarks(const UndoMark& pre); // after any mutating action
    void applyEditMark(const UndoMark& m);        // restore caret/selection
    void breakUndoChain(); // freeze coalescing on deliberate caret motion

    // One fan-out key's edit for a single caret, computed against the
    // PRE-ACTION document. Applying the edits right-to-left keeps every
    // not-yet-applied edit's offsets valid, and a slot's own final caret is
    // its edit's end (payload end minus caretTrim), so it is only shifted
    // by edits strictly LEFT of it when those apply.
    struct SlotEdit {
        int start = 0;
        int end = 0;
        QString text; // replacement (empty = pure delete)
        // Code units trimmed off the payload's END when parking this slot's
        // caret (M14 pair-split Enter): the caret lands at the payload's end
        // minus this much; 0 for every plain replace/insert.
        int caretTrim = 0;
    };
    // The per-slot edit for one fan-out key; return false if the slot edits
    // nothing (e.g. Backspace at the document start).
    using SlotEditFn = std::function<bool(const CaretSlot&, SlotEdit&)>;
    // Fans one action out to all carets (primary included) as ONE bracketed
    // core group. Returns true if anything was edited; also normalizes.
    bool applyCaretsFanOut(const SlotEditFn& editFn);

    // --- indentation machinery (M14; semantics on the public blockIndent/
    // --- indent setters). Lines are collected per slot: caret line always,
    // --- plus every line a selection touches — the selection's END line is
    // --- excluded when the end sits at col 0 (the selection visually ends
    // --- ON it, VS Code's line-count convention). Indent inserts one stop
    // --- of spaces at col 0 of each touched line; outdent removes up to one
    // --- stop of LEADING whitespace (a '\t' counts as the advance to its own
    // --- stop, so mixed-tab files survive the first outdent). All carets
    // --- collapse; one action = one bracketed core group = one undo step.
    bool blockTouchesLines() const; // any caret's selection spans lines?
    QVector<int> blockTouchedLines() const; // sorted+deduped, end-col-0 rule
    void applyBlockEdits(const QVector<SlotEdit>& edits); // bracket + caret remap
    // Single caret, no multi-line selection and no extra carets: Tab
    // inserts spaces to the next stop; a selection under the key is
    // replaced (typing semantics).
    void tabInsertSpaces();
    // The text Enter should insert at one caret of `line` (auto-indent
    // policy, see setAutoIndentEnabled). The pair-split case returns
    // "\n" + childIndent + "\n" + parentIndent.
    QString autoIndentNewline(int line, int col) const;
    static int autoIndentCaretTrim(const QString& payload); // pair-split caret park
    // Indent-edit arithmetic over line pieces (statics: pure functions of
    // (text, stop); the '\t'-aware stop grid is anchored at the line start).
    static QString leadingWsPrefix(const QString& line);
    static int indentWidthOf(const QString& line, int stop);
    static SlotEdit makeIndentEdit(const qmn::core::TextBuffer& buf, int line, int count);
    static int prefixCutForOneStop(const QString& prefix, int stop);
    static SlotEdit makeOutdentEdit(const qmn::core::TextBuffer& buf, int line, int stop);
    static int visualColOf(const QString& line, int col, int stop); // caret col -> stop grid
    // One shared caret-movement target fn applied to every caret (nav fan-
    // out; no document change). `extend` grows each caret's own selection;
    // `freezeChain` breaks the core's coalescing chain — real navigation
    // does, M18's pair-Skip hop deliberately does not (it is part of the
    // typing flow, and skipping over a closer must not fence undo chunks).
    void moveCarets(const std::function<TextPosition(const CaretSlot&)>& targetFn,
                    bool extend, bool freezeChain = true);
    // VS Code-style caret cleanup after adds/moves: fold duplicate carets,
    // drop carets strictly inside another slot's selection, and resolve
    // intersecting selections by keeping the LEFT one and collapsing the
    // right slot's selection to its caret. Never drops the primary.
    void normalizeCarets();
    QVector<CaretSlot> allCarets() const; // extras + primary, doc order
    // Sorted [start,end) buffer offsets of a slot's selection (equal ends
    // when the slot has none) — the offset view the normalization rules use.
    static std::pair<int, int> selRangeOf(const qmn::core::TextBuffer& buf, const CaretSlot& s);

    int totalHeight() const;
    void setVScroll(int pixels);
    void updateScrollBarRange();
    void ensureCaretVisible();
    // M19-only settings: stored here (single source of truth) but exposed
    // ONLY through the options object — no flat public setter (M19 rule:
    // new settings live only on the options object). handleAutoClosePair
    // and the user-edit entry points read the flags directly.
    void setAutoCloseEnabled(bool enabled);
    bool autoCloseEnabled() const { return autoCloseEnabled_; }
    void setReadOnly(bool readOnly);
    bool isReadOnly() const { return readOnly_; }
    // M21 indent guides (default ON): ambient per-line tab-stop guides in
    // the leading whitespace; semantics on paintIndentGuides below.
    void setIndentGuidesEnabled(bool enabled);
    bool indentGuidesEnabled() const { return indentGuidesEnabled_; }
    // M25: the soft-wrap knob lives here (editor PRIVATE) exactly like the
    // M19-M21 knobs; CodeEditorOptions owns the public door + the signal.
    // See the machinery block near modelLineForY for the semantics (PLAN
    // §M25) and the frozen-surface note about the row doors' UNITS.
    void setSoftWrapEnabled(bool enabled);
    bool softWrapEnabled() const { return softWrapEnabled_; }
    friend class CodeEditorOptions;
    friend class CodeEditorAccessible; // M11: the a11y bridge reads the live
                                       // model/caret/geometry state directly

    void insertTextAtCaret(const QString& text, int caretTrim = 0);
    // M18 auto-close pairs: claim a printable press of one ()[]{}""''``
    // char (public contract next to the API docs above). Returns false for
    // "ordinary typing please" — non-pair chars, and slots where every
    // decision is Plain (the plain path then fans out exactly that char).
    bool handleAutoClosePair(QChar ch);
    void backspace();
    void deleteForward();
    TextPosition clampPos(TextPosition pos) const;
    void setCaret(TextPosition pos); // always collapses the selection
    void moveCaret(TextPosition target, bool extend);
    void setSelection(TextPosition anchor, TextPosition caret);
    bool deleteSelection(); // true if a selection was removed
    // Sorted ends of the current selection (valid only if hasSelection_).
    TextPosition selStart() const;
    TextPosition selEnd() const;
    void pasteText(const QString& text); // sanitize + insert + primary update
    void updatePrimarySelection(); // mirror an active selection to X11 primary
    int lineLength(int line) const;

    // --- M15 multi-click / Ctrl+D ----------------------------------------
    // Ctrl+D: next occurrence of the selection text / caret's word becomes
    // a new SELECTED slot (word-only, never wraps; semantics at the impl).
    void addNextOccurrenceCaret();
    // Commit the triple+-click whole-line span [lineSelAnchor_..lineSelEnd_].
    void applyLineSpan();

    // --- M16 context menu + text DnD --------------------------------------
    // Menu factory behind contextMenuEvent (split out as a test seam: the
    // exec() inside contextMenuEvent would block an offscreen test run).
    // Both sit under protected so the test subclass can reach the seam;
    // the Python binding surface keeps them invisible (internal classes).
protected:
    QMenu* createContextMenu();
    // Right-click caret rules: a click outside the selection re-places the
    // caret first (Qt convention), one inside keeps the selection intact.
    void prepareContextMenuAt(const QPoint& widgetPos);
private:
    // Drag-out of the live selection (armed by a press inside it, fired at
    // startDragDistance): mime = plain text + our move marker; exec prefers
    // Move, and a Move accepted elsewhere still owes the source-side delete.
    void startSelectionDrag();
    // True iff the mime carries our move marker AND it names THIS widget AND
    // the marker's offsets are exactly the live primary selection (a marker
    // from a sibling editor or a round-tripped external copy degrades to a
    // text-only copy drop).
    bool internalMoveSource(const QMimeData* mime, int& srcStart, int& srcEnd) const;
    // Copy-drop insertion at an offset (not at the caret): one bracketed
    // edit, caret at the inserted end, primary mirror like pasteText.
    void insertAtDrop(const QString& text, int dropOff);
    // Post-1.0 DnD polish. Shift-aware acceptance shared by the three drag
    // protocol stages: a held Shift answers the protocol with COPY even
    // when Move was proposed — and the answered action is what the source
    // sees, which keeps a self-drop's post-exec Move-delete disarmed.
    void acceptTextDrop(QDropEvent* event);
    // The drop-target caret: set at the positionAt mapping of the current
    // drag point (the same mapping dropEvent re-derives, so the mark is
    // exactly where a release would land), cleared on drop/leave. Paint
    // state only — the drop itself never reads it.
    void showDropCaretAt(const QPoint& widgetPos);
    void clearDropCaret();
    // M1 drag auto-scroll: past the viewport edges scroll and map the point
    // back inside. Shared by mouseMoveEvent and (M16) dragMoveEvent.
    QPoint edgeAutoScroll(QPoint pos);

    // Per-line glyph-engine layout cache. A laid-out QTextLine is the sole
    // authority for caret x, selection x and mouse hit-testing (measured on
    // Qt 6.12: emoji ZWJ families occupy ~1.8 cells, not their code-unit
    // count, so grid arithmetic drifts the caret on such lines).
    //
    // Key = (line index, laid-out text equality): a QTextLine depends only
    // on (text, font, infinite width), so an identical string needs no
    // relayout - no buffer revision needed. Font changes are NOT part of the
    // key; changeEvent(FontChange) wipes the cache instead.
    //
    // QTextLayout is copy/move-DELETED and a default-constructed QTextLine
    // makes x()/cursorToX() SEGFAULT (measured, no eng guard in Qt 6.12), so
    // slots hold the layout in place inside the fixed array and `valid`
    // gates every QTextLine use.
    struct GeoSlot {
        int keyLine = -1; // -1 = empty slot
        QString text;     // what `line` was laid out for (possibly truncated)
        QTextLayout layout;
        QTextLine line;
        quint64 stamp = 0; // LRU bookkeeping
        bool valid = false; // false for empty lines: never touch line
    };
    static constexpr int kGeoSlots = 128; // >= a 4K-tall viewport of lines;
                                          // overflow only costs relayouts
    // Laid out with NoWrap and setLineWidth(Q_INFINITY): the line takes its
    // natural width, horizontal scrolling (M7) is the view's problem. Very
    // long lines are laid out only up to geoMaxUnits() code units.
    const GeoSlot* hitLine(int line) const;
    int geoMaxUnits() const;
    double xInLine(const GeoSlot* s, int col) const; // line-space x (no padding)

    // M4: per-line QTextCharFormat ranges for the highlight engine's
    // merged spans, in line-local laid-out coordinates, snapped so a
    // range edge never splits a grapheme cluster (shared boundaries of
    // adjacent spans snap identically, keeping the partition exact).
    // Empty vector = "no colors for this line, plain path".
    QVector<QTextLayout::FormatRange> formatRangesForLine(int line, const GeoSlot* gs) const;

    mutable std::array<GeoSlot, kGeoSlots> geo_{};
    mutable quint64 geoClock_ = 0;
    void resetGeoCache(); // wipe slots in place (QTextLayout is uncopyable)
    // M17: push scaleFontForZoom(baseFont_, zoomFactor_) into
    // QWidget::setFont — the FontChange branch in changeEvent does the real
    // re-measure + cache wipe; this call only lands the right font on it.
    void applyCodeFont();

    // M25 (Gate 2): the TextBuffer moved into the editor-owned CodeDocument
    // (the sanctioned ownership deviation). `document_` is declared BEFORE
    // `buffer_` because member-init order follows declaration order and
    // buffer_ binds to the document's buffer already in the ctor's init
    // list. Keeping the name `buffer_` as a REFERENCE member kept all
    // ~230 call sites — the header inlines, the .cpp machinery and the
    // accessible friend paths — byte-identical.
    CodeDocument* document_;
    qmn::core::TextBuffer& buffer_;
    // M4 highlight engine (declared right after the model it observes).
    std::unique_ptr<HighlightEngine> highlight_;
    QVector<UndoMark> undoMarks_; // aligned with buffer_ undo groups
    QVector<UndoMark> redoMarks_; // aligned with buffer_ redo groups
    qint64 lastPushSerial_ = 0;   // last seen buffer_.undoPushSerial()
    TextPosition caret_;
    TextPosition anchor_; // inactive end of the selection
    bool hasSelection_ = false;
    // D1 additional caret slots in document order (the primary caret is
    // NOT inside this list; allCarets() merges the two views).
    QVector<CaretSlot> extraCarets_;
    // Which extra slot an Alt+Drag currently resizes (index into
    // extraCarets_; -1 = no Alt drag on an extra), plus whether the drag
    // targets the primary caret instead (Alt+Press landed on it).
    int altDragSlot_ = -1;
    bool altDragPrimary_ = false;

    // M15 multi-click + word-drag state. The chain lives across the DblClick
    // events Qt delivers for a fast click run (a re-press at mousePress
    // resets it); wordSelLo_/Hi_ pin the double-clicked word for the drag.
    bool wordDrag_ = false;
    TextPosition wordSelLo_, wordSelHi_;
    int multiClickDepth_ = 0;                  // chained DblClick count (2 = line mode)
    int lineSelAnchor_ = 0, lineSelEnd_ = 0;   // triple+-click line span, inclusive
    int lineSelDir_ = 1;                       // repeat wraps: +1 down, -1 up
    qint64 lastDblMs_ = 0;                     // event timestamps, chaining window
    int lastDblLine_ = -1, lastDblX_ = 0;

    // M16 DnD state. pressInSelection_ arms the drag gesture: a plain press
    // strictly inside the live selection keeps the selection (no collapse),
    // moves under startDragDistance are swallowed, and a release without
    // drag collapses at the release point. dndSelfDragLive_ marks "inside
    // our own QDrag::exec" (drop-side copy/move decisions); dndDropHandled_
    // says the internal drop already performed the whole move, so the
    // post-exec MoveAction branch must not delete the source again.
    bool pressInSelection_ = false;
    QPoint pressPos_;
    bool dndSelfDragLive_ = false;
    bool dndDropHandled_ = false;
    // Post-1.0 DnD polish: the target caret of the drag currently hovering
    // over us (document coordinates — edge auto-scroll moves the viewport,
    // never the mark). Painted with the caret's 2px rule but WITHOUT the
    // blink/caretOn_ gate; see showDropCaretAt/clearDropCaret.
    TextPosition dropCaret_;
    bool dropCaretActive_ = false;
    QString preedit_; // IME composition string, display-only: never in buffer_
    // M8 parity wrap: the custom context-menu actions in INSERTION order
    // (addContextAction idempotency replaces the label in place, never the
    // slot). Ids are the trigger keys of contextActionTriggered.
    struct ContextActionEntry {
        QString id;
        QString label;
    };
    QVector<ContextActionEntry> contextActions_;
    QString storedConfig_; // the raw compact-JSON buffer (header comment at
                           // the door: the view never parses it)
    QScrollBar* vBar_ = nullptr;
    bool scrollbarSync_ = false;

    // M17 font policy (semantics: public API comments): the effective font
    // is scaleFontForZoom(baseFont_, zoomFactor_) and widget font() mirrors
    // it; baseFont_ itself keeps the caller's hinting/size knobs untouched.
    QFont font_;
    QFont baseFont_;
    double zoomFactor_ = 1.0;
    double wheelZoomAcc_ = 0.0; // Ctrl+wheel: sub-notch trackpad deltas accrue
    double cellWidth_ = 8.0;
    int lineHeight_ = 18;
    int ascent_ = 14;

    // --- M14 indentation state (semantics: public API comments) -----------
    int tabStopWidthOption_ = 4; // GLOBAL template (M23; options mirror)
    int tabStopWidth_ = 4;  // EFFECTIVE stop: override > template (M23);
                            // soft-tab stop + literal-tab render grid
    // M23 language override registry (semantics: languageProperties docs):
    // normalized-id keyed (HighlightEngine rule: trimmed + lowercased),
    // children of the editor, created on demand — the built-in registry
    // starts EMPTY. Every change here re-resolves the effective stop.
    QHash<QString, LanguageProperties*> languageProps_;
    void refreshTabStopWidth(); // re-resolve EFFECTIVE = override > template;
                                // on a real change: geo wipe + scroll range +
                                // update (the existing setTabStopWidth fan-out)
    bool autoIndent_ = true; // Enter copies leading whitespace (+ stop)
    bool tabChangesFocus_ = false; // Tab rides Qt's focus chain instead

    int vScroll_ = 0; // pixels
    int padding_ = 4;

    // --- M5 decoration + gutter state (semantics: public API comments) ---
    LineNumbersMode lineNumbersMode_ = LineNumbersMode::On;
    QVector<int> breakpointLines_; // sorted + deduped (binary_search on paint)
    QVector<Diagnostic> diagnostics_; // sorted by (startLine, startCol)
    QVector<InlayHint> inlayHints_; // sorted by (line, col)
    int highlightStart_ = -1; // inclusive whole-line range; -1 = unset
    int highlightEnd_ = -1;
    CurrentLineMode currentLineMode_ = CurrentLineMode::Background; // M18 default on
    int longLineRulerCol_ = 0; // M18 long-line ruler column; 0 = off
    bool autoCloseEnabled_ = true; // M19 (options-only knob)
    bool readOnly_ = false; // M19 (options-only knob): user edits refused
    bool indentGuidesEnabled_ = true; // M21 (options-only knob)
    CodeEditorOptions* options_ = nullptr; // M19; ctor parents it to this
    CodeEditorTheme* theme_ = nullptr;     // M24; ctor parents it to this
    int gutterInnerPad() const; // gutter digit/glyph padding (M5)

    // --- M6 find/replace state (semantics: public API comments) ---
    // The bar is a QWidget* only here: its class (FindBar) is defined
    // later in CodeEditor.cpp and talks to the editor purely through the
    // public API - internal linkage never crosses the Shiboken boundary.
    QWidget* findBar_ = nullptr; // lazy child; show/hide via findBar_*()
    QString findText_;
    FindFlags findFlags_ = FindFlag::MatchCase;
    // Lazy engine cache: an edit only marks it stale (the paint and every
    // entry point refresh on demand), so the mutable mirrors the M4
    // highlight engine's at-most-once-per-keypress reparse cadence.
    mutable QVector<MatchRange> matches_; // sorted by (startLine, startCol)
    mutable int currentMatch_ = -1;
    mutable bool findDirty_ = false; // edits mark stale; paint/entry refresh
    mutable bool findPatternValid_ = true;
    mutable bool findRepainting_ = false; // set only around the refresh in
                                          // paintEvent: a recompute there
                                          // must not schedule another paint
    void refreshFindMatches(); // no-op unless findDirty_ (not const: a
                               // refresh schedules a repaint via update())
    void recomputeFindMatches(); // full per-line scan (the DoD math)
    void syncFindBarLabel() const; // bar count label after state moves
    bool replaceRange(const MatchRange& m, const QString& replacement);

    // --- M8 machine 6 popup state (semantics: public API comments) ---
    // QWidget* members only, again the FindBar pattern: the popup classes
    // live in CodeEditor.cpp and receive their GEOMETRY — the anchor math
    // reads the private slot/scroll state on THIS side, the widgets never
    // do. Anchors freeze at show time; scroll/resize re-place against them.
    QWidget* hoverPopup_ = nullptr;      // lazy child via showHover/hideHover
    QWidget* completionPopup_ = nullptr; // lazy child via the completion doors
    HoverRange hoverAnchor_;             // the range the visible hover tracks
    TextPosition completionAnchor_;      // the caret frozen at show time
    QVector<CompletionItem> completionRows_; // the SHOWN rows: the accept
                                             // signal's payload source
    void acceptCompletionRow(int row);   // hide BEFORE emitting (a handler's
                                         // re-show rides a closed popup)
    void placeHoverPopup();      // anchor math + bounds clamp (shared rect
    void placeCompletionPopup(); // helper lives beside the classes in .cpp)
    void repositionPopups();       // setVScroll/setHScroll/resizeEvent hooks
    void hidePopupsForInteraction(); // keyPress/mousePress/focusOut auto-hide

    // --- M7 overview ruler state (semantics: public API comments) ---
    bool rulerEnabled_ = true; // Monaco-minimap-parity default (the
                               // set_minimap_enabled hook defaults on too)
    bool rulerDrag_ = false;   // left/middle press inside the strip
    void paintOverviewRuler(QPainter& p) const; // last pass: it OVERPAINTS
                                                // the text's right edge
    void rulerScrollTo(int y); // center the strip point's line in the view

    // --- M9 horizontal scroll state (semantics: public API comments) ---
    int hScroll_ = 0;                    // px, clamped like vScroll_
    bool scrollBeyondLastLine_ = false;  // Monaco-parity hook, see public docs
    mutable int docWidthPx_ = 0;         // ambient extent of visited lines
    bool hScrollbarSync_ = false;        // setHScroll <-> hBar_ reentry guard
    QScrollBar* hBar_ = nullptr;
    void setHScroll(int pixels); // the one clamping writer of hScroll_

    // --- M13 folding state (semantics: public API comments) --------------
    // Collapsed folds, model lines. Kept sorted by (start asc, end desc);
    // strictly-contained entries are dropped at collapse time, so any
    // remaining overlap is only partial (stale-edit residue) and rides the
    // run merge below. Ends are clamped to the document on rebuild.
    struct FoldState {
        int start = 0;
        int end = 0;
    };
    QVector<FoldState> collapsed_;
    // Hidden runs derived from collapsed_: disjoint, NON-TOUCHING (runs
    // that abut or overlap are merged — so the line after a run's last is
    // always visible or past the document), sorted, each carrying the
    // count of hidden lines before it. The view-line map is binary search
    // over this table; visibleLines_ caches lineCount() - hidden total.
    struct HiddenRun {
        int first = 0;        // first hidden model line
        int last = 0;         // last hidden model line (inclusive)
        int hiddenBefore = 0; // hidden lines in earlier runs
    };
    static constexpr int kIndentWindow = 256; // indentOfModelLine read cap
    QVector<HiddenRun> hiddenRuns_;
    int foldVisibleTotal_ = 0; // cached visibleLineTotal() (set by rebuildHiddenRuns)
    int foldDocLines_ = 0;     // lineCount() at the last fold sweep (edit diff)
    void rebuildHiddenRuns();  // collapsed_ -> hiddenRuns_/visibleLines_
    // true = the fold map rebuilt (hiddenRuns_ indices moved): the M25 wrap
    // layer then resyncs its weight half (retuneWrapForFolds) in the same op.
    bool shiftFoldStateForEdit(const qmn::core::TextBuffer::EditEvent& ev);
    // Foldable starts known for the painted window: engine @fold captures
    // (exact ends) where a fold grammar is active, else the indent rule
    // (start-only; ends resolved lazily by foldEndForLine on the actual
    // fold request, keeping paint O(view)). Never both — see
    // indentFoldsActive(). Mutable like the highlight engine: paintEvent
    // keeps it warm.
    mutable QVector<FoldState> foldables_; // sorted by start, one per start
    void recomputeFoldables(int firstModel, int lastModel);
    int foldEndForLine(int line); // exact end (engine full query, else indent scan)
    int indentOfModelLine(int line) const; // leading ws units; -1 = blank line
    // M21: one visible line's indent guides, painted in the text slot loop
    // (semantics on the definition). Reads the knob flag at call site only.
    void paintIndentGuides(QPainter& p, int line, int yTop) const;
    // True when the indent heuristic is the fold source: no grammar loaded
    // (plain/unknown language), or the language's composed fold query failed
    // to build (then the engine contributes nothing and the fallback keeps
    // folding alive). In grammar-fold modes (js/json/python) leading
    // whitespace must NOT fake python-style nesting (owner report).
    bool indentFoldsActive() const;
    // Does a COLLAPSED fold's indent source still exist, checked against
    // MODEL lines (the fold itself hid line+1, so recomputeFoldables'
    // visible-adjacency rule can never fire on its own start)? Same rule
    // as foldEndForLine's scan, first step only: the first non-blank
    // model line below the header must out-indent it. False means an
    // edit killed the block and the fold auto-expands.
    bool indentFoldSourceAlive(int header) const;
    // Unfold every collapsed fold strictly containing line (outermost
    // first). True if anything opened. Caret choke points use this (a
    // caret/selection target is never hidden); paint never reveals.
    bool revealLine(int line);
    int nextVisibleModelLine(int line) const; // slot+1 walk (caret jump over folds)
    int prevVisibleModelLine(int line) const; // slot-1 walk
    int slotOfModelLine(int line) const;      // visibleIndexOfModelLine, clamped >= 0
    // Model line whose slot row contains widget y (-1 outside text rows).
    int modelLineForY(int y) const;

    // --- M25 soft wrap (Gate 2; semantics: PLAN §M25) ------------------------
    // The unit change (the sanctioned contract shift): with wrap ON the row
    // doors above speak VISUAL SLOTS. The line-space fold machinery above
    // stays exactly as M13 built it; wrap rides on top as ONE additive rule:
    //   weight(line) = hidden ? 0 : wrapSlotCount(line)
    //   slot(line)   = Σ weight, L < line
    // so a fold's hidden run becomes a whole block of zero-weight lines and
    // a wrapped line's slots stay contiguous — fold × wrap is ONE story with
    // no second table, and every line boundary is a slot boundary (the fold
    // range [slot(start+1), slot(end+1)) can only cut between slots, never
    // "into" a line: hidden line == all its slots hidden).
    //
    // Coverage: measured lines cost their exact counts, UNMEASURED visible
    // lines count as exactly 1 — the slot total is always honest for the
    // wrap-OFF world, and wrap ON costs nothing on lines the viewport never
    // visits (the fold perf story, kept). Viewport entry measures eagerly
    // (caret lines answer exactly: revealAt/ensureCaretVisible measure their
    // own line first); a budgeted forward walker escalates the rest inside
    // the HighlightEngine's latency precedent (kWrapBudgetMs) — resize
    // storms cost the budget, never an O(document) sweep.
    bool softWrapEnabled_ = false; // the 14th knob (options() door)
    int appliedWrapWidth_ = 0;     // the h-window width the updateScrollBarRange
                                   // watcher last saw (change detection only:
                                   // plans key their own width, live checks
                                   // read horizontalViewportWidth() directly)
    static constexpr double kWrapBudgetMs = 8.0; // measured-wrap latency
                                                 // budget (kParseBudgetMs
                                                 // precedent, round 1)
    struct WrapInfo {
        quint32 textLen = 0;    // laid-out line length at measure time
        int widthKey = -1;      // horizontalViewportWidth() the breaks used
        quint32 epoch = 0;      // wrapPlanEpoch_ at measure time: a glyph-
                                // cache wipe (font/zoom/tab-stop) kills plans
        QVector<int> starts;    // code-unit STARTS of continuation rows
                                // (sorted, every one < lineLength; the
                                // number of slots this line takes is
                                // starts.size() + 1; empty = fits)
    };
    QHash<int, WrapInfo> wrap_;            // model line -> measured row plan
    QVector<int> wrapWeight_;              // per line: hidden? 0 : slot count
    QVector<int> wrapPrefix_;              // prefix sums: wrapPrefix_[L] ==
                                           // slot(L); size lineCount()+1
    int wrapTotal_ = 0;                    // cached wrapPrefix_.back()
    int wrapDocLines_ = 0;                 // lineCount() at the last wrap seam
    quint32 wrapPlanEpoch_ = 1;            // resetGeoCache bumps: plans die
    int wrapCursor_ = 0;                   // walker frontier (model line)
    // The previous run's hidden-mask snapshot (superset semantics, so the
    // restore half of a retune is proportional to the fold, not the doc).
    QVector<HiddenRun> wrapZeroed_;
    // True when model line is inside one of hiddenRuns_ (the fold authority,
    // unchanged: hidden stays hidden, its rows simply all disappear).
    bool lineIsHiddenByFold(int line) const;
    // The slot doors of the wrap map (the visible* doors route here when the
    // knob is on): the line's FIRST slot (-1 hidden/out of range), and the
    // inverse (total and clamped, exactly like modelLineForVisibleIndex).
    int wrapIndexOfLine(int line) const;
    int wrapLineForSlot(int slot) const;
    // The ONE wrap answer for a visible line: measured exact, unmeasured 1.
    int wrapSlotCount(int line) const;
    // The live plan of a line, or null when absent/stale (epoch, width or
    // line length moved): the validity rule every consumer shares.
    const WrapInfo* liveWrapPlan(int line) const;
    // Measure (or re-measure after a width/geo-cache change) one line;
    // returns its slot count. Overbudget lines break on the monospace cell
    // grid — the established far-right geometry-only trade (the geo
    // comment above geoMaxUnits() records why that tail is honest).
    int measureWrap(int line);
    // Grow/shrink the weight+prefix tables to lineCount() (appended lines are
    // provisional 1) and keep wrapTotal_ current.
    void syncWrapTables();
    // ONE suffix pass of the prefix table, from `from` to the end: every
    // batched mutation runs exactly one of these (per-line rebuilds would
    // make a window of measurements cost a window of sweeps).
    void rebuildPrefixFrom(int from);
    // Resync the weight half after a fold-map change: drop dead plans,
    // restore lines the map just un-hid (via wrapZeroed_), zero the current
    // hidden runs, then one batched prefix rebuild. Keeps wrapZeroed_ ==
    // hiddenRuns_ for the next call.
    void retuneWrapForFolds();
    // The wrap half of the edit observer (runs right after the fold half, so
    // hiddenRuns_ is already current): shift/drop plan keys and array entries
    // about the seam, one batched rebuild. See its comment for the frame.
    void shiftWrapStateForEdit(const qmn::core::TextBuffer::EditEvent& ev);
    // The budgeted walker: measure forward from wrapCursor_ while inside
    // kWrapBudgetMs up to capModelLine. The viewport entry calls it with the
    // visible window plus two viewports of headroom, so a wheel-down near the
    // bottom edge never paints blind rows (the HighlightEngine Mode-Viewport
    // lesson in row shape).
    void advanceWrapFrontier(int capModelLine);
    // Eager coverage of a MODEL-line range (the caret doors use it: a caret
    // line answers its slot exactly, never provisionally).
    void ensureWrapLinesCovered(int fromLine, int toLine);
    // Paint/scroll entry: cover the rows about to be shown, then escalate
    // within the budget. Called before the paint loop reads the row window,
    // so totals it changes are re-read by the same pass (the wrap-OFF
    // early-out never comes here at all).
    void prepareWrapViewport(int firstSlot, int rows);
    // Wrap-aware row cursor for the paint/hit/caret geometry (machine 2;
    // with segStart=0 every expression it feeds reduces to the v1.6.1
    // form — the wrap-off identity). Invalid when wrap is off or the slot
    // is hidden.
    struct RowPlan {
        int slot = -1;
        int line = -1;
        int segStart = 0; // first code unit painted on this row
        int segEnd = 0;   // exclusive ink end
        int segs = 1;     // the line's total slots
        int segIdx = 0;   // 0 = the line's first row (gutter/number lives here)
    };
    RowPlan rowPlan(int slot) const;
    // The row slot a position's ink rides (the line's first slot + the
    // segment holding col; unmeasured lines: the first slot). -1 hidden,
    // exactly like the fold map's door. The caret scroll math, the paint
    // caret and positionAt all address rows through this one door.
    int slotOfPosition(TextPosition p) const;
    // The segment holding col in a line's plan (0 unmeasured): col == a
    // break start belongs to the row it starts.
    int segIndexOfCol(int line, int col) const;
    // Visual-row Up/Down (M25 machine 2, the frozen comment's "column
    // memory"): the step is ONE slot (hidden fold lines own no slots, so
    // the fold jump rides the map for free); the target col keeps the
    // caret's row-local x on the destination row. Invalid/hidden rows
    // degrade to the v1 model-line step.
    TextPosition rowStepTarget(TextPosition from, bool up) const;

    // M25 machine 2 (paint/hit/caret geometry): xInWrapped mirrors xInLine's
    // phantom-cell extrapolation in the SEGMENT frame (at segStart == 0 it is
    // the v1.6.1 expression exactly — the wrap-off identity rides these
    // helpers, not a parallel implementation).
    double xInWrapped(const GeoSlot* gs, int segStart, int col) const;

    // --- M10 diff mode (semantics: PLAN GATE 2 §M10; the LineDiff include
    // lives in the .cpp ONLY — no diff type crosses this header) ----------
    // State behind a forward declaration: DiffState (defined in the .cpp)
    // holds the engine's hunks/rows, the fold-over-row prefix tables and
    // the modLine->row map. diff_ non-null == diff mode active; the row
    // axis (aligned rows, one live space: diff > wrap > fold/plain) rides
    // the same prefix-sum algebra as M25's wrap map, keyed by ROW.
    struct DiffState;
    std::unique_ptr<DiffState> diff_;
    CodeDocument* diffStandaloneOriginal_ = nullptr; // editor-owned child,
                                                     // string-door feed only
    // The chrome read window's OWN engine instance (M10: the ORIGINAL side
    // is syntax-highlighted exactly like the modified side — its second
    // HighlightEngine rides the same attach/onEdit/setViewport protocol,
    // fed through the original doc's editApplied signal). Created fresh in
    // beginDiffMode, dropped by clearDiffDocuments; borrows the original
    // document's buffer, so it must never outlive the diff session.
    std::unique_ptr<HighlightEngine> origHighlight_;
    QTimer diffRecomputeTimer_; // single-shot live-update debounce
    static constexpr int kDiffDebounceMs = 120;
    // Shared tail of both doors (after the texts/documents are settled):
    // language, first synchronous recompute, axis, scroll reset.
    void beginDiffMode(const QString& language);
    // FULL recompute of the alignment (engine auto budget), then
    // rebuildDiffRowAxis() + caret-anchored scroll restore (the caret keeps
    // its screen row; vanished-line carets degrade to nearest-line clamp).
    void recomputeDiff();
    // The edit seams (modified observer pass + the original's own
    // editApplied): restart the single-shot debounce.
    void scheduleDiffRecompute();
    // Re-key the fold-over-row prefix tables from rows_ + hiddenRuns_
    // (fold mutations ride this WITHOUT a recompute; rows mutations ride
    // it after every recompute).
    void rebuildDiffRowAxis();
    // The diff half of the three visible* doors (line == MODIFIED line).
    int diffVisibleIndexOfModLine(int line) const;
    int diffModLineForSlot(int slot) const;
    // The alignment row under a visible slot (-1 out of range): the
    // blank-cell-aware door for the paint/hit side (machine 3).
    int diffRowForSlot(int slot) const;
    // Guard the external original pointer: destroyed() -> clear the mode.
    void watchDiffOriginal(CodeDocument* doc);

    // --- M10 machine 3: the two-column chrome (paint + interaction) ------
    // The ORIGINAL side enters the widget through the gutter: gutterWidth()
    // grows by diffChromeWidth() (the original's number column + text
    // column + the connector gap), and the fold zone — v1's right-edge
    // strip — flips to the gutter's LEFT edge (foldZoneLeft()), the chrome
    // block taking its old place. The MODIFIED numbers stay right-aligned
    // exactly where textLeft - pad puts them; the text side keeps its v1
    // geometry doors untouched. The chrome block is a read-only canvas:
    // clicks there run the ORIGINAL selection/copy gesture (the original
    // never becomes editable: keys and edits ride the modified side).
    // x-frame (machine-3 contract, painted + clicked with the same math;
    // bpPad = the v1 breakpoint slot, fz = foldZoneWidth(), pad = the v1
    // gutterInnerPad()):
    //   [padding_, padding_+bpPad)              breakpoint slot (v1)
    //   [foldZoneLeft(), +fz)                   the fold zone
    //   [+fz, +origDigitsCol)                   ORIGINAL numbers (right-
    //                                           aligned in this column)
    //   [+origDigitsCol, +textCol)              ORIGINAL text window
    //   [+textCol, +connector)                  the CONNECTOR strip
    //                                           (stripRight == the chrome
    //                                           block's end)
    //   [stripRight, textLeft)                  MODIFIED number column
    //                                           (v1 digits + both pads,
    //                                           right-aligned as ever)
    //   [textLeft, …)                           MODIFIED text (v1 verbatim)
    // The hunk-kind code per row (rowKind, built with the prefix tables):
    // rowKind[r] = -1 unchanged | 0 replace (both cells, side by side)
    // | 1 pure insert (blank ORIGINAL cell) | 2 pure delete (blank
    // MODIFIED cell) — the paint map of the five diff roles.
    int origDiffDigits() const; // the ORIGINAL doc's digit count (chrome col)
    // The v1 gutter WITHOUT the diff term (digits + pads + bp slot + fold
    // zone): gutterWidth() = this + diffChromeWidth(). diffChromeTextCol()
    // needs it to split the remaining width 50/50 between the two text
    // windows (the live-check fix of 2026-11-17: a flat width() fraction
    // made the ORIGINAL half visibly narrower than the MODIFIED side).
    int gutterV1Width() const;
    // The chrome text pass's FormatRanges (the v1 formatRangesForLine
    // protocol applied to origHighlight_ + the ORIGINAL buffer): token
    // colors ride ol.layout.draw's selections, whitespace and uncolored
    // runs keep the GutterDigits pen (the default), and an inactive engine
    // means the plain single-pen path.
    QVector<QTextLayout::FormatRange> origChromeFormatRanges(
        int line, const QString& laidOut) const;
    // Paint pass split (the under pass runs after the selection quads so
    // bands + the original column's ground/text/selection sit UNDER the
    // gutter digits and the modified text; the over pass runs after the
    // text loop: connectors, the delete hatch, the gutter marks — all on
    // the chrome's opaque Base ground).
    void drawDiffChromeUnder(QPainter& p) const;
    void drawDiffChromeOver(QPainter& p) const;
    // Sub-x-frame of the original column (frame members shared by the
    // paint and the click routing).
    struct DiffFrame {
        int origNumLeft = 0;  // the ORIGINAL digits column's left edge
        int origNumRight = 0; // its right edge (the digits' right anchor)
        int origTextLeft = 0; // the ORIGINAL text window's left edge
        int origTextRight = 0;
        int stripLeft = 0;    // the connector strip's left edge
        int stripRight = 0;   // its right edge (== the chrome block's end)
        int firstSlot = 0;    // the visible-slot window the paint walks
        int lastSlot = -1;
        bool valid = false;   // diff mode off / degenerate frame
    };
    DiffFrame diffFrame() const;
    // The alignment row under a widget y (-1 = no row): the y half of the
    // chrome hit test. diffOriginalHit adds the x half: the col rides the
    // ORIGINAL line's own one-line layout, inside the fixed read window
    // (which never rides hScroll_ — the modified side owns the h-scroll).
    int diffChromeRowAtY(int y) const; // the alignment row under y (-1 none)
    bool diffChromeHit(const QPoint& pt) const; // x inside the chrome block?
    TextPosition diffOriginalHit(const QPoint& pt) const;
    void drawDiffConnectors(QPainter& p, const DiffFrame& f) const;
    // The original-column gesture state machine (press/drag/double/triple,
    // the v1 click-chain rules in ORIGINAL coordinates) — routed by the
    // mouse handlers when the press x lands inside the chrome block
    // (left of the modified digits, right of the fold zone).
    void diffOriginalPress(const QPoint& pt, bool shift);
    void diffOriginalExtend(const QPoint& pt);
    void diffOriginalDoubleClick(const QMouseEvent* event);
    void selectDiffOriginalWord(const TextPosition& hit);
    void selectDiffOriginalLines(int line);
    // The ONE writer of the ORIGINAL selection: anchor + caret (v1's
    // anchor_/caret_ mirror) and the offset-sorted A/B pair the paint and
    // originalSelectionText read; a zero-width pair is a caret-point.
    void setDiffOriginalSel(const TextPosition& anchor,
                            const TextPosition& caret);
    // Any live original-column selection is dropped by every modified-side
    // gesture/key and by clearDiffDocuments(); copy() routes to it first.
    void clearDiffOriginalSelection();

    // --- M22 fold-gutter hover (semantics: PLAN M22) ----------------------
    // While the fold strip is live, mouse tracking follows the pointer and
    // resolves the INNERMOST fold containing the hovered row (a row that
    // itself starts a fold resolves to that fold). The state is anchored in
    // MODEL rows (the M16 drop-caret pattern: scrolling keeps the painted
    // region on the right rows for free); every hover move re-resolves, and
    // edits (the buffer edit observer), fold toggles (the fold mutation
    // doors), presses (the dragLeave lesson: the clear must not RELY on a
    // leave event) and Leave all invalidate it. Paint only maps the stored
    // range onto slot space — no resolver runs on the paint path. No public
    // surface: tests prove the region via pixels, click state and
    // hasMouseTracking.
    static constexpr int kFoldHoverScanCap = 2048; // indent-mode upward hunt
                                                   // for a covering fold start
    int hoverFoldRow_ = -1;   // hovered strip row (-1: pointer off the strip)
    int hoverFoldStart_ = -1; // innermost fold covering that row…
    int hoverFoldEnd_ = -1;   // …(-1 when the row is covered by no fold)
    // Resolves that fold: a fold STARTING at the row wins; else the NEAREST
    // start above the row whose (tightest, foldLine-consistent) end still
    // covers it. Grammar mode reverse-walks the engine fold table; indent
    // mode scans model rows above (bounded by the cap).
    bool innermostFoldAt(int row, FoldState* out);
    void updateFoldHoverState(const QPoint& pt); // hover move: re-resolve
    void clearFoldHoverState();                  // invalidate (+repaint if set)

    QTimer blinkTimer_;
    bool caretOn_ = true;

    QElapsedTimer perfTimer_;
    PerfStats perf_;
};

// moc rejects the generated binary operators inside a class body, and Qt's
// own in-class flag types (QFont::StyleStrategy &co.) pair Q_DECLARE_FLAGS
// in-class with this macro at namespace scope - same here, so
// FindFlag::MatchCase | FindFlag::UseRegex composes naturally for C++
// consumers (Shiboken exposes the Q_FLAG(FindFlags) meta-flag regardless).
Q_DECLARE_OPERATORS_FOR_FLAGS(CodeEditor::FindFlags)

} // namespace qmn::view

// Needed by the Python binding only: PySide's generic signal connector resolves
// signal parameter types through QMetaType, so cursorMoved(TextPosition) is not
// connectable from Python without this. Qt insists the declaration sit at global
// scope - putting it inside qmn::view does not compile. Harmless for C++ users.
Q_DECLARE_METATYPE(qmn::view::TextPosition)
// Same reason for the M13 fold record (foldableRanges() returns a
// QVector<FoldRange> the binding's container converter must resolve).
Q_DECLARE_METATYPE(qmn::view::CodeEditor::FoldRange)
// Same reason for the post-1.8.0 ACCEPT signal's payload (the record rides
// completionAccepted as a SIGNAL parameter).
Q_DECLARE_METATYPE(qmn::view::CodeEditor::CompletionItem)
