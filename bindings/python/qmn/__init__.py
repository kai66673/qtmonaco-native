"""Python-facing package for the native Qt editor widget (qtmonaco-native).

The Shiboken-generated extension module is called ``_qmn``, and because the C++
classes live in the namespaces ``qmn::view`` and (since G3) ``qmn::glue``, its
own import path is ``_qmn.qmn.view.CodeEditor`` /
``_qmn.qmn.glue.LspHub`` (Shiboken 6.12 mirrors C++ namespaces as Python
namespaces). This package flattens that to ``qmn.CodeEditor`` /
``qmn.LspHub`` and carries the qtmonaco-compatible surface: the ``Monaco``
class mirrors the public API of ``qtmonaco.Monaco`` (the QWebEngineView-based
editor this widget replaces as a drop-in).

The 1-based/0-based conversion is deliberately done HERE, in Python, not in
C++ (PLAN.md, AGENTS.md): qtmonaco is 1-based (``set_cursor``, ``delete_line``,
``set_highlighted_lines``, ``insert_text``'s default column) while C++
``TextPosition`` is 0-based UTF-16 code units. Error semantics mirror
qtmonaco too: the edit doors raise ``ValueError`` on a read-only editor (the
C++ doors stay silent by the M19 contract, so the shim pre-checks
``options().isReadOnly()`` before calling them).

Deliberate, documented divergences (PLAN.md "Decided divergences"):

* ``set_theme`` round-trips the label like qtmonaco, AND routes the known
  Monaco names through the M24 theme object: "light"/"vs" apply the Light
  preset, "vs-dark" applies Dark; anything else (``hc-black`` included -
  no high-contrast preset exists) paints nothing extra, label only.
* ``set_vim_mode_enabled`` exists but only WARNS - vim mode is out of scope
  forever, and a warning (not ``AttributeError``) keeps drop-in callers alive.
* ``set_minimap_enabled`` maps onto the overview ruler; no Chromium-style
  minimap is ever built.
* ``open_diffs`` shipped with M10 (Gate 2): the two-column native diff view,
  with the uri/options params accepted-and-ignored (see its docstring).
* The M8 parity wrap (1.8.0) added ``add_action`` and
  ``update_workspace_configuration``. qtmonaco's other LSP hooks existed only
  to drive ITS built-in Chromium client and have no counterpart here. The
  native LSP client and hub are C++ glue — since G3 the hub and the document
  session DO ship in the wheel (``qmn.LspHub`` / ``qmn.ProjectSession``), but
  as plain objects the app builds and owns, never as ``Monaco`` methods
  (parity stays parity); the view's popup doors (``showCompletionList`` /
  ``showHover``) remain bound for apps that own a client of their own.
  ``set_lsp_header`` / ``get_lsp_header`` stay absent ON PURPOSE: qtmonaco's
  "header" is text prepended to the document ON THE WIRE ONLY, while every
  native LSP path (diagnostics, hover, completion, rename, formatting,
  willSaveWaitUntil) rests on the one invariant "the server's document IS the
  editor's document" - a per-path offset tax on all six paths for a hack no
  native stack needs. Extra context for a server belongs in
  ``initializationOptions``, in the workspace configuration (below), or in a
  second document.

Attribute access (not ``from _qmn.qmn.view import ...``) is on purpose: the
namespace shims are attributes created by the extension at import time, not
real submodules, so the import-statement form would go through the import
machinery and fail.
"""

import json
import warnings

# Imported first on purpose: the generated wrapper resolves its QWidget base
# through shiboken's converter registry, so PySide6.QtWidgets must already be
# loaded when _qmn initializes. QtNetwork likewise: since G3 the extension
# carries a DT_NEEDED libQt6Network (the glue shelf reaches protocol's
# TcpTransport), and the one-Qt-at-runtime rule means that soname must
# resolve against the ALREADY-LOADED PySide6-bundled copy - QtWidgets does
# not load QtNetwork on its own. NEVER load Qt by absolute path (AGENTS).
import PySide6.QtWidgets  # noqa: F401
import PySide6.QtNetwork  # noqa: F401
import _qmn as _ext

from PySide6.QtCore import Signal

CodeEditor = _ext.qmn.view.CodeEditor
CodeEditorTheme = _ext.qmn.view.CodeEditorTheme
CodeDocument = _ext.qmn.view.CodeDocument  # M25 (editor-owned, like the theme)
TextPosition = _ext.qmn.view.TextPosition
# G3: the glue shelf (qmn::glue). The hub/session are PLAIN objects the app
# builds and owns - deliberately NOT Monaco methods (parity stays parity);
# LspHub.ServerSpec rides as a nested attribute of LspHub.
LspHub = _ext.qmn.glue.LspHub
ProjectSession = _ext.qmn.glue.ProjectSession


class Monaco(CodeEditor):
    """The native drop-in for ``qtmonaco.Monaco`` (1-based, qtmonaco-shaped).

    Subclasses the bound C++ widget, so every native door (``setPlainText``,
    ``undo``, ``options()``...) stays reachable for consumers who want the
    0-based native layer; the methods below reproduce the qtmonaco surface.

    Signals (Python-declared, like qtmonaco's): ``text_changed(str)`` is
    relayed from the C++ ``textChanged`` ping with a last-value echo guard
    (qtmonaco's ``set_text`` returns early on an identical value, so an
    identical relaunch must not ping twice); ``language_changed`` and
    ``theme_changed`` fire from the setters below;
    ``context_menu_action_triggered(str)`` relays the C++
    ``contextActionTriggered`` (an app's custom context-menu action, the M8
    parity wrap); ``workspace_config_updated(bool, str)`` keeps qtmonaco's
    shape but reports the LOCAL accept only — see
    ``update_workspace_configuration``.
    """

    text_changed = Signal(str)
    language_changed = Signal(str)
    theme_changed = Signal(str)
    context_menu_action_triggered = Signal(str)
    workspace_config_updated = Signal(bool, str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._current_cursor = {"line": 1, "column": 1}  # qtmonaco's shape
        self._language = self.language()
        self._theme = "vs-dark"  # qtmonaco's default label (v1: label only)
        self._last_text = self.toPlainText()
        self.cursorMoved.connect(self._relay_cursor)
        self.textChanged.connect(self._relay_text)
        self.contextActionTriggered.connect(self._relay_context_action)

    # --- relay plumbing ----------------------------------------------------

    def _relay_cursor(self, pos):
        # cursorMoved fires on caret/selection caret moves; the dict frame and
        # the 1->0 base flip live here, exactly once.
        self._current_cursor = {"line": pos.line + 1, "column": pos.col + 1}

    def _relay_text(self):
        # C++ textChanged is the detail-free ping for EVERY document
        # mutation; qtmonaco only ever announced a value change, so the
        # last-value guard keeps the same contract (type "a" then undo back
        # to identical text: one announcement, not two).
        text = self.toPlainText()
        if text == self._last_text:
            return
        self._last_text = text
        self.text_changed.emit(text)

    def _relay_context_action(self, action_id):
        # The C++ door announces by ID (labels live in the menu only), so the
        # relay is the straight qtmonaco name for the same event.
        self.context_menu_action_triggered.emit(action_id)

    def _check_writable(self, what):
        if self.options().isReadOnly():
            raise ValueError(f"Editor is in read-only mode, cannot {what}.")

    # --- text in/out ---------------------------------------------------------

    def set_text(self, value, language=None, uri=None):
        """Set the whole document. qtmonaco-compatible signature and order:
        same-value returns first, then the read-only ``ValueError``, then the
        ``TypeError``. ``uri`` is accepted and IGNORED natively (models/URIs
        are Gate 2). ``language`` (optional) routes through ``set_language``.
        Like qtmonaco, setting a value EXITS diff mode — the flag check sits
        at the top, mirroring qtmonaco's ``_diff_mode = False`` sitting both
        before the early return and before the value assignment.
        """
        if self.isDiffMode():
            self.clearDiffDocuments()
        if self.toPlainText() == value:
            return
        self._check_writable("set value")
        if not isinstance(value, str):
            raise TypeError("Value must be a string.")
        if language is not None:
            self.set_language(language)
        self.setPlainText(value)
        # text_changed comes out of the _relay_text plumbing (the C++ ping),
        # like qtmonaco's bridge announced the change after the JS round trip.

    def get_text(self):
        """The current document (native truth, always in sync)."""
        return self.toPlainText()

    # --- diff view (M10) -----------------------------------------------------

    def open_diffs(self, original, modified, language=None, original_uri=None,
                   modified_uri=None, options=None):
        """Native two-column diff view (the M10 doors). qtmonaco-compatible
        signature and TypeError checks; the modified side becomes the
        editor's document (so ``get_text`` and the typing/editing surface
        keep speaking it, and ``text_changed`` announces it once).

        Documented divergences (PLAN.md §M10): ``original_uri``/
        ``modified_uri`` are accepted and IGNORED (like ``set_text``'s uri;
        the original side is an editor-owned standalone document), and
        ``options`` (Monaco's JS diff-editor dict) is validated and IGNORED —
        every key including ``renderSideBySide:false`` (inline/unified mode
        is out of v1, the plan says it honestly) — the native knobs are the
        regular ``options()`` doors, and the read-only state keeps applying
        to the MODIFIED side throughout the diff (the ORIGINAL column is
        always read-only). The C++ pointer door
        ``setDiffDocuments(CodeDocument*, ...)`` stays C++-only: Python never
        builds a CodeDocument.
        """
        if not isinstance(original, str):
            raise TypeError("Original value must be a string.")
        if not isinstance(modified, str):
            raise TypeError("Modified value must be a string.")
        if options is not None and not isinstance(options, dict):
            raise TypeError("Options must be a dictionary.")
        if language is None:
            # qtmonaco's "None keeps the language" rule lives HERE: the C++
            # beginDiffMode ALWAYS routes its language through setLanguage
            # (which has no same-value skip), so handing it "" would really
            # mean plain text. Passing the current id keeps the grammar
            # (an explicit language="" still means plain — the user asked).
            language = self.get_language()
        else:
            self._language = language
        self.setDiffDocuments(original, modified, language)
        # text_changed(modified) comes out of the _relay_text plumbing (the
        # string door feeds document_->setPlainText, the C++ ping); when
        # modified equals the current text there is no value change and no
        # announcement — documented divergence from qtmonaco's unconditional
        # emit, pinned in parity_monaco.
        # (language_changed is deliberately NOT emitted here: set_language's
        # door would paint the old grammar on the old text before the diff
        # session begins; open_diffs passes ONE language to the session.)

    def close_diffs(self):
        """Exit the diff view (not in qtmonaco's API — the view can only
        leave diff mode there by re-setting the value; ours has the native
        clearDiffDocuments door, no-op when already closed)."""
        self.clearDiffDocuments()

    # --- programmatic edits ----------------------------------------------------

    def insert_text(self, text, line=None, column=None):
        """Insert at the current cursor, or at 1-based ``line`` (and
        ``column``; qtmonaco's rules: line without column means column 1,
        column without line is a ``ValueError``)."""
        self._check_writable("insert text")
        if not isinstance(text, str):
            raise TypeError("Text must be a string.")
        if line is not None and column is None:
            column = 1
        elif column is not None and line is None:
            raise ValueError("Column must be provided if line is specified.")
        if line is None:
            pos = self.position()  # the CURRENT 0-based caret, converted
        else:
            pos = TextPosition(max(int(line), 1) - 1, max(int(column), 1) - 1)
        self.insertTextAt(pos, text)

    def delete_line(self, line=None):
        """Delete the 1-based ``line``, or the cursor's current line when
        ``line`` is None. The line goes with its newline; out-of-range lines
        are a silent no-op (qtmonaco's Monaco backend behaved the same way).
        """
        self._check_writable("delete line")
        target = self.position().line if line is None else int(line) - 1
        self.deleteLineAt(target)

    # --- cursor ---------------------------------------------------------------

    @property
    def current_cursor(self):
        """``{"line": int, "column": int}``, 1-based - qtmonaco's shape."""
        return self._current_cursor

    def set_cursor(self, line, column=1, move_to_position=None):
        """Move the caret to 1-based (line, column); ``move_to_position`` is
        qtmonaco's reveal flag: "center", "top", anything else = plain
        reveal (the backend main.ts mapping, native equivalents)."""
        pos = TextPosition(max(int(line), 1) - 1, max(int(column), 1) - 1)
        self.setPosition(pos)  # cursorMoved feeds current_cursor natively
        if move_to_position == "center":
            self.revealAt(pos, CodeEditor.RevealMode.Center)
        elif move_to_position == "top":
            self.revealAt(pos, CodeEditor.RevealMode.NearTop)
        elif move_to_position:
            self.revealAt(pos, CodeEditor.RevealMode.Minimal)

    # --- language / theme ---------------------------------------------------------

    def get_language(self):
        """The language id ("" = plain text). Native truth, not a cache."""
        return self.language()

    def set_language(self, language):
        """Rides the C++ highlighter door ("" or an unknown id = plain
        text); emits ``language_changed`` like qtmonaco."""
        self._language = language
        super().setLanguage(language)
        self.language_changed.emit(language)

    # qtmonaco/Monaco theme names -> our preset maps. Unknown names (the
    # hc-black family, typos) round-trip as label only - documented
    # divergence above.
    _THEME_PRESETS = {
        "light": CodeEditorTheme.Preset.Light,
        "vs": CodeEditorTheme.Preset.Light,
        "vs-dark": CodeEditorTheme.Preset.Dark,
    }

    def get_theme(self):
        """The theme NAME (qtmonaco's label round-trip); known labels also
        drive the paint through the M24 theme object (see ``set_theme``)."""
        return self._theme

    def set_theme(self, theme):
        self._theme = theme
        preset = self._THEME_PRESETS.get(theme)
        if preset is not None:
            t = self.theme()
            # Same-kind skip, the demo's applySettings rule: applyPreset
            # WIPES the whole explicit map, so a redundant set must not
            # silently undo manual role/token tweaks.
            if t.kind() != preset:
                t.applyPreset(preset)
        self.theme_changed.emit(theme)

    # --- editor options ------------------------------------------------------------

    def set_readonly(self, read_only):
        self.options().setReadOnly(bool(read_only))

    def set_minimap_enabled(self, enabled):
        """Maps onto the OVERVIEW RULER (the native stand-in; a Chromium-
        style minimap is never built - PLAN.md)."""
        self.options().setOverviewRulerEnabled(bool(enabled))

    def set_scroll_beyond_last_line_enabled(self, enabled):
        self.options().setScrollBeyondLastLineEnabled(bool(enabled))

    def set_line_numbers_mode(self, mode):
        """qtmonaco's literal: "on", "off", "relative", "interval";
        anything else raises."""
        table = {
            "on": CodeEditor.LineNumbersMode.On,
            "off": CodeEditor.LineNumbersMode.Off,
            "relative": CodeEditor.LineNumbersMode.Relative,
            "interval": CodeEditor.LineNumbersMode.Interval,
        }
        try:
            value = table[mode]
        except KeyError:
            raise ValueError(f"Unknown line numbers mode: {mode!r}") from None
        self.setLineNumbersMode(value)

    # --- line decorations -----------------------------------------------------------

    def set_highlighted_lines(self, start_line, end_line):
        """1-based inclusive pair (qtmonaco); the C++ door is 0-based and
        swaps a reversed pair on its own."""
        self.setHighlightedLines(int(start_line) - 1, int(end_line) - 1)

    def clear_highlighted_lines(self):
        self.clearHighlightedLines()

    # --- M8 parity wrap: custom actions + workspace configuration ----------

    def add_action(self, action_id, label, language="python"):
        """Appends a custom item to the context menu (qtmonaco's
        ``add_action``); triggering it fires ``context_menu_action_triggered``
        with the id. IDEMPOTENT by id: the same id replaces its label and
        keeps its slot.

        ``language`` is accepted-and-ignored (the PLAN-decided divergence):
        qtmonaco encodes it as a Monaco context-key precondition, while the
        native editor is already scoped per document — an app that wants
        per-language items registers them per editor (``set_language``), and
        the native door (``addContextAction``) takes no such parameter.
        ``contextActionIds()`` (the native door) lists the ids on record.
        """
        if not isinstance(action_id, str) or not action_id:
            raise ValueError("action_id must be a non-empty string.")
        if not isinstance(label, str):
            raise TypeError("label must be a string.")
        self.addContextAction(action_id, label)

    def update_workspace_configuration(self, settings):
        """Buffers the workspace configuration on the WIDGET (qtmonaco handed
        the dict to its Chromium bridge). The dict goes to the native door as
        sorted compact JSON, and the widget keeps it OPAQUE — it never parses
        or applies it.

        The one reader is the app-side LSP glue (the C++ ``LspHub``, not part
        of this wheel): it pushes the buffer to a live server as
        ``workspace/didChangeConfiguration`` once the handshake is done and
        re-pushes on every real change, so qtmonaco's "buffered until init"
        semantics hold — in a bare ``Monaco`` with no hub the buffer simply
        waits. ``workspace_config_updated(True, "")`` therefore reports that
        the buffer was ACCEPTED LOCALLY, not that a server confirmed it: v1
        has no server round-trip to report (a decided divergence, PLAN).
        """
        if not isinstance(settings, dict):
            raise TypeError("settings must be a dict.")
        try:
            payload = json.dumps(settings, sort_keys=True,
                                 separators=(",", ":"))
        except (TypeError, ValueError) as exc:
            self.workspace_config_updated.emit(False, str(exc))
            raise TypeError(f"settings are not JSON-serializable: {exc}") from exc
        self.setStoredWorkspaceConfiguration(payload)
        self.workspace_config_updated.emit(True, "")

    # --- decided divergences ----------------------------------------------------------

    def set_vim_mode_enabled(self, enabled):
        """Out of scope FOREVER (AGENTS.md): exposed only so drop-in callers
        do not hit ``AttributeError``; nothing happens beyond the warning.
        """
        warnings.warn(
            "qtmonaco-native does not implement vim mode and never will; "
            "this call does nothing.",
            stacklevel=2,
        )


__all__ = ["CodeEditor", "CodeEditorTheme", "CodeDocument", "LspHub",
           "Monaco", "ProjectSession", "TextPosition"]
