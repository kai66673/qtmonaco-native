"""Python-facing package for the native Qt editor widget (qtmonaco-native).

The Shiboken-generated extension module is called ``_qmn``, and because the C++
classes live in the namespace ``qmn::view``, its own import path is
``_qmn.qmn.view.CodeEditor`` (Shiboken 6.12 mirrors C++ namespaces as Python
namespaces). This package flattens that to ``qmn.CodeEditor`` and carries the
qtmonaco-compatible surface: the ``Monaco`` class mirrors the public API of
``qtmonaco.Monaco`` (the QWebEngineView-based editor this widget replaces as a
drop-in).

The 1-based/0-based conversion is deliberately done HERE, in Python, not in
C++ (PLAN.md, AGENTS.md): qtmonaco is 1-based (``set_cursor``, ``delete_line``,
``set_highlighted_lines``, ``insert_text``'s default column) while C++
``TextPosition`` is 0-based UTF-16 code units. Error semantics mirror
qtmonaco too: the edit doors raise ``ValueError`` on a read-only editor (the
C++ doors stay silent by the M19 contract, so the shim pre-checks
``options().isReadOnly()`` before calling them).

Deliberate, documented divergences (PLAN.md "Decided divergences"):

* ``set_vim_mode_enabled`` exists but only WARNS - vim mode is out of scope
  forever, and a warning (not ``AttributeError``) keeps drop-in callers alive.
* ``set_minimap_enabled`` maps onto the overview ruler; no Chromium-style
  minimap is ever built.
* ``open_diffs`` and the LSP hooks are NOT present at all (Gate 2): an
  ``AttributeError`` here is honest, a silent no-op would not be.

Attribute access (not ``from _qmn.qmn.view import ...``) is on purpose: the
namespace shims are attributes created by the extension at import time, not
real submodules, so the import-statement form would go through the import
machinery and fail.
"""

import warnings

# Imported first on purpose: the generated wrapper resolves its QWidget base
# through shiboken's converter registry, so PySide6.QtWidgets must already be
# loaded when _qmn initializes.
import PySide6.QtWidgets  # noqa: F401
import _qmn as _ext

from PySide6.QtCore import Signal

CodeEditor = _ext.qmn.view.CodeEditor
TextPosition = _ext.qmn.view.TextPosition


class Monaco(CodeEditor):
    """The native drop-in for ``qtmonaco.Monaco`` (1-based, qtmonaco-shaped).

    Subclasses the bound C++ widget, so every native door (``setPlainText``,
    ``undo``, ``options()``...) stays reachable for consumers who want the
    0-based native layer; the methods below reproduce the qtmonaco surface.

    Signals (Python-declared, like qtmonaco's): ``text_changed(str)`` is
    relayed from the C++ ``textChanged`` ping with a last-value echo guard
    (qtmonaco's ``set_text`` returns early on an identical value, so an
    identical relaunch must not ping twice); ``language_changed`` and
    ``theme_changed`` fire from the setters below.
    """

    text_changed = Signal(str)
    language_changed = Signal(str)
    theme_changed = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._current_cursor = {"line": 1, "column": 1}  # qtmonaco's shape
        self._language = self.language()
        self._theme = "vs-dark"  # qtmonaco's default label (v1: label only)
        self._last_text = self.toPlainText()
        self.cursorMoved.connect(self._relay_cursor)
        self.textChanged.connect(self._relay_text)

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

    def _check_writable(self, what):
        if self.options().isReadOnly():
            raise ValueError(f"Editor is in read-only mode, cannot {what}.")

    # --- text in/out ---------------------------------------------------------

    def set_text(self, value, language=None, uri=None):
        """Set the whole document. qtmonaco-compatible signature and order:
        same-value returns first, then the read-only ``ValueError``, then the
        ``TypeError``. ``uri`` is accepted and IGNORED natively (models/URIs
        are Gate 2). ``language`` (optional) routes through ``set_language``.
        """
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

    def get_theme(self):
        """The theme NAME. v1 stores the label Python-side only: no palette
        API has crossed the freeze yet, and qtmonaco only ever round-tripped
        the string through Chromium anyway."""
        return self._theme

    def set_theme(self, theme):
        self._theme = theme
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


__all__ = ["CodeEditor", "TextPosition", "Monaco"]
