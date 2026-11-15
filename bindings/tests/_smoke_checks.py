"""Shared Phase B checks, used by both smoke drivers (build tree and wheel).

No path setup here: smoke_qmn.py puts the build-tree paths on sys.path first,
smoke_wheel.py runs against an installed wheel and needs nothing.
"""

import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

TEXT = "alpha\nbravo\ncharlie"


def loaded_libs(basename_part: str) -> set:
    """Distinct on-disk files backing a loaded library name."""
    with open(f"/proc/{os.getpid()}/maps") as fh:
        return {line.split()[-1] for line in fh if basename_part in line}


def run_checks() -> int:
    import qmn
    from PySide6.QtWidgets import QApplication

    app = QApplication.instance() or QApplication([])

    editor = qmn.CodeEditor()
    editor.setPlainText(TEXT)
    assert editor.toPlainText() == TEXT, editor.toPlainText()
    assert editor.lineCount() == 3, editor.lineCount()

    # C++ side is 0-based; qtmonaco is 1-based. The wrapper adds the 1
    # deliberately - so pin down what the raw bound layer really does.
    editor.setPosition(qmn.TextPosition(2, 3))
    pos = editor.position()
    assert (pos.line, pos.col) == (2, 3), (pos.line, pos.col)
    assert pos.line == 2, "third line must read as index 2 in C++"

    editor.selectAll()
    assert editor.hasSelection()
    assert editor.selectedLength() == len(TEXT), editor.selectedLength()
    assert editor.selectedText() == TEXT

    editor.clearSelection()
    assert not editor.hasSelection()

    # Signals: the real question is whether PySide's generic signal connector
    # can marshal our TextPosition value type into a Python callable.
    seen = []
    editor.cursorMoved.connect(lambda p: seen.append(("cursor", p.line, p.col)))
    editor.selectionChanged.connect(lambda: seen.append(("selection",)))
    editor.setPosition(qmn.TextPosition(1, 0))
    editor.selectAll()
    assert seen, "no signal reached Python"

    # --- M24 theme object (the 14th knob, bound for the wheel from 1.6.1) ----
    # The risky conversions all live here: nested enums both directions,
    # QColor in AND out, QRgb out (severityInk), and the themeChanged signal.
    from PySide6.QtGui import QColor

    theme = editor.theme()
    Role, Preset = qmn.CodeEditorTheme.Role, qmn.CodeEditorTheme.Preset
    assert theme.kind() == Preset.Default, theme.kind()
    assert not theme.hasColor(Role.Base)
    assert theme.color(Role.Base).isValid() is False  # invalid = "fallback paints"

    theme_hits = []
    theme.themeChanged.connect(lambda: theme_hits.append(1))

    theme.applyPreset(Preset.Dark)
    assert theme.kind() == Preset.Dark and theme_hits == [1], theme_hits
    assert theme.hasColor(Role.Base) and theme.ink(Role.Base).isValid()

    theme.setColor(Role.Base, QColor("magenta"))  # a real QColor crosses IN
    assert theme_hits == [1, 1], theme_hits      # setColor -> one more emission
    assert theme.kind() == Preset.Custom         # manual edit flips the tracker
    assert theme.color(Role.Base).name() == "#ff00ff"  # and back OUT verbatim

    ink = theme.severityInk(qmn.CodeEditor.Severity.Error)  # QRgb is an int
    assert isinstance(ink, int) and ink != 0

    theme.clearColor(Role.Base)
    assert theme_hits == [1, 1, 1], theme_hits
    theme.applyPreset(Preset.Default)  # empty map = the fallback behavior again
    assert theme.kind() == Preset.Default and not theme.hasColor(Role.Base)

    copies = loaded_libs("libQt6Core.so")
    print(f"qmn module: {qmn.__file__}")
    print(f"signals:    {seen}")
    print(f"QtCore:     {sorted(copies)}")
    assert len(copies) == 1, f"more than one QtCore in the process: {sorted(copies)}"

    app.processEvents()
    print("OK: qmn binding smoke test passed")
    return 0
