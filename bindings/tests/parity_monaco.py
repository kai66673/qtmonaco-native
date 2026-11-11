"""Gate 1 parity checks: qmn.Monaco vs the qtmonaco.Monaco public API contract.

Drives the Python shim (bindings/python/qmn/__init__.py) against the semantics
frozen from qtmonaco's own monaco.py + backend/src/main.ts: 1-based cursors,
{"line":..,"column":..} cursor dicts, ValueError on read-only edits, the
set_text early-return ORDER (same-value, then readonly, then TypeError), the
insert_text line/column rules, and the reveal flag mapping.

The 1-based/0-based conversion lives entirely in the shim, so the assertions
below are written in qtmonaco's 1-based terms and the raw C++ getters (0-based)
are only poked to pin the conversion down.
"""

import os
import sys
import warnings

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


def check_parity() -> int:
    import qmn
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QApplication

    app = QApplication.instance() or QApplication([])

    ed = qmn.Monaco()
    ed.resize(600, 400)

    # --- construction + initial cursor (qtmonaco: {"line":1,"column":1}) ----
    assert ed.current_cursor == {"line": 1, "column": 1}, ed.current_cursor
    assert ed.get_text() == ""
    assert ed.get_language() == ""
    assert ed.get_theme() == "vs-dark"  # qtmonaco's default label

    # --- set_text / get_text + the text_changed relay ------------------------
    seen = []
    ed.text_changed.connect(seen.append)
    ed.set_text("alpha\nbravo\ncharlie")
    assert ed.get_text() == "alpha\nbravo\ncharlie"
    assert seen == ["alpha\nbravo\ncharlie"], seen

    # same-value early return: no signal, exactly like qtmonaco's
    # "if self._value == value: return" sitting BEFORE the readonly check.
    ed.set_text("alpha\nbravo\ncharlie")
    assert seen == ["alpha\nbravo\ncharlie"], seen

    # user typing relays too (the C++ textChanged ping, echo-guarded):
    # one announcement per real value change, undo back to the same value
    # announces once (the value really changed twice: away and back).
    ed.insert_text("X")  # at the current cursor: line 1, column 1
    assert seen[-1] == "Xalpha\nbravo\ncharlie", seen[-1]
    ed.undo()
    assert ed.get_text() == "alpha\nbravo\ncharlie"
    assert seen[-1] == "alpha\nbravo\ncharlie", seen[-1]
    assert seen.count("alpha\nbravo\ncharlie") == 2, seen

    # set_text's uri is accepted and IGNORED natively (Gate 2 stays Gate 2).
    ed.set_text("one", uri="file:///ignored.py")
    assert ed.get_text() == "one"

    # direct native mutation (bypassing set_text) still relays: the shim
    # promise is that text_changed announces EVERY value change.
    ed.setPlainText("one\ntwo")
    assert seen[-1] == "one\ntwo", seen[-1]
    # identical native re-set: the C++ ping fires, the echo guard eats it.
    before = len(seen)
    ed.setPlainText("one\ntwo")
    assert len(seen) == before, seen

    # --- error semantics (order: readonly, then TypeError) -------------------
    ed.set_readonly(True)
    assert ed.options().isReadOnly()
    for call in (
        lambda: ed.set_text("nope"),
        lambda: ed.insert_text("nope"),
        lambda: ed.delete_line(1),
    ):
        try:
            call()
            raise AssertionError("expected ValueError on read-only edit")
        except ValueError:
            pass
    # same-value still returns FIRST even while read-only (qtmonaco's order)
    ed.set_text("one\ntwo")
    ed.set_readonly(False)
    try:
        ed.set_text(42)
        raise AssertionError("expected TypeError for a non-string")
    except TypeError:
        pass
    assert ed.get_text() == "one\ntwo"

    # --- insert_text rules ----------------------------------------------------
    ed.set_text("hello world")
    ed.set_cursor(1, 1)
    seen_before = len(seen)
    ed.insert_text("XX ")  # no line/column: rides the CURRENT cursor
    assert ed.get_text() == "XX hello world"
    # caret ends just past the insert (Monaco executeEdits), 1-based dict
    assert ed.current_cursor == {"line": 1, "column": 4}, ed.current_cursor
    assert len(seen) == seen_before + 1, seen

    # line given, column omitted -> column 1 (qtmonaco's default rule)
    ed.set_text("a\nb\nc")
    ed.insert_text(">", line=2)
    assert ed.get_text() == "a\n>b\nc", ed.get_text()
    # column without line -> ValueError (qtmonaco's guard, verbatim)
    try:
        ed.insert_text("z", column=3)
        raise AssertionError("expected ValueError for column without line")
    except ValueError:
        pass
    assert ed.get_text() == "a\n>b\nc"
    # both: 1-based insert lands there
    ed.insert_text("Q", line=3, column=2)
    assert ed.get_text() == "a\n>b\ncQ", ed.get_text()

    # --- delete_line (1-based, None = cursor's line, out-of-range silent) ----
    ed.set_text("a\nb\nc")
    ed.delete_line(2)
    assert ed.get_text() == "a\nc", ed.get_text()
    ed.set_cursor(1, 1)
    ed.delete_line()  # None -> the CURRENT line
    assert ed.get_text() == "c", ed.get_text()
    before = len(seen)
    ed.delete_line(99)  # out of range: silent, no signal (nothing changed)
    assert ed.get_text() == "c"
    assert len(seen) == before, seen
    # undo granularity = one action (setPosition between the two deletes
    # froze the delete chain, so they are two groups, not one coalesced run):
    # one undo brings back the cursor-line delete, the next the 1-based one.
    ed.undo()
    assert ed.get_text() == "a\nc", ed.get_text()
    ed.undo()
    assert ed.get_text() == "a\nb\nc", ed.get_text()

    # --- current_cursor follows every move, 1-based ---------------------------
    ed.set_text("one\ntwo\nthree")
    ed.set_cursor(2, 3)
    assert ed.current_cursor == {"line": 2, "column": 3}, ed.current_cursor
    # the raw native layer underneath stays 0-based (the shim converts)
    assert (ed.position().line, ed.position().col) == (1, 2)
    # clamped like qtmonaco's max(int(line),1) guard
    ed.set_cursor(0, 0)
    assert ed.current_cursor == {"line": 1, "column": 1}, ed.current_cursor
    # typing through the C++ layer updates the dict too (cursorMoved relay)
    ed.set_cursor(1, 4)
    ed.insert_text("X")
    assert ed.current_cursor == {"line": 1, "column": 5}, ed.current_cursor

    # --- set_cursor + reveal flag mapping (exact offsets, no vision) ---------
    lines = "\n".join(f"line{i}" for i in range(100))
    ed.set_text(lines)
    lh, pad, h = ed.lineHeight(), ed.padding(), ed.height()

    ed.set_cursor(50 + 1, 1, "center")  # model line 50, 1-based 51
    assert ed.verticalScrollOffset() == 50 * lh + lh // 2 + pad - h // 2, (
        ed.verticalScrollOffset(),
    )
    ed.set_cursor(70 + 1, 1, "top")
    assert ed.verticalScrollOffset() == 70 * lh - pad, ed.verticalScrollOffset()
    # any other truthy flag -> minimal reveal
    ed.set_cursor(10 + 1, 1, True)
    assert ed.verticalScrollOffset() == 10 * lh - pad, ed.verticalScrollOffset()
    # a falsy flag: no reveal call at all (setPosition's own ensure-visible
    # still ran — that is qtmonaco's behavior too: the caret move precedes
    # the reveal branch in backend main.ts).
    ed.set_cursor(1 + 1, 1, False)
    assert ed.current_cursor == {"line": 2, "column": 1}

    # --- line decorations (1-based inclusive <-> 0-based door) ---------------
    ed.set_highlighted_lines(2, 3)
    assert ed.highlightedLinesStart() == 1, ed.highlightedLinesStart()
    assert ed.highlightedLinesEnd() == 2, ed.highlightedLinesEnd()
    ed.clear_highlighted_lines()
    assert ed.highlightedLinesStart() == -1

    # --- options surface ------------------------------------------------------
    for name, enum_name in (
        ("on", "On"), ("off", "Off"), ("relative", "Relative"),
        ("interval", "Interval"),
    ):
        ed.set_line_numbers_mode(name)
        got = ed.lineNumbersMode()
        want = getattr(qmn.CodeEditor.LineNumbersMode, enum_name)
        assert got == want, (name, got)
    try:
        ed.set_line_numbers_mode("vertical")
        raise AssertionError("expected ValueError for an unknown mode")
    except ValueError:
        pass

    ed.set_minimap_enabled(True)  # the documented map: overview ruler
    assert ed.options().overviewRulerEnabled() is True
    ed.set_minimap_enabled(False)
    assert ed.options().overviewRulerEnabled() is False

    ed.set_scroll_beyond_last_line_enabled(True)
    assert ed.options().scrollBeyondLastLineEnabled() is True
    ed.set_scroll_beyond_last_line_enabled(False)

    # --- decided divergences --------------------------------------------------
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        ed.set_vim_mode_enabled(True)  # must NOT raise AttributeError
    assert len(caught) == 1 and "vim mode" in str(caught[0].message), caught
    # Gate 2 stays unbound ON PURPOSE (PLAN.md): open_diffs and every LSP hook
    # of qtmonaco are absent, so callers get an honest AttributeError instead of
    # a silent no-op that would pretend the feature exists.
    for gate2 in (
        "open_diffs",
        "set_lsp_header",
        "get_lsp_header",
        "add_action",
        "update_workspace_configuration",
    ):
        assert not hasattr(ed, gate2), gate2

    # --- language routing -----------------------------------------------------
    langs = []
    ed.language_changed.connect(langs.append)
    ed.set_text("x = 1", language="python")
    assert ed.get_language() == "python", ed.get_language()
    assert langs == ["python"], langs
    assert "python" in ed.availableLanguages(), ed.availableLanguages()
    ed.set_language("javascript")
    assert ed.get_language() == "javascript", ed.get_language()
    assert langs[-1] == "javascript", langs
    # Gate 1 registry truth: python/json/javascript only. An UNKNOWN id
    # falls back to plain text natively (get_language goes empty, like the
    # engine's documented contract) but the Python signal still announces
    # what was asked — qtmonaco echoes the requested value unvalidated.
    ed.set_language("typescript")
    assert ed.get_language() == "", ed.get_language()
    assert langs[-1] == "typescript", langs
    assert "typescript" not in ed.availableLanguages(), ed.availableLanguages()

    # --- undo/redo ride along after programmatic edits ------------------------
    ed.set_text("keep")
    ed.insert_text("A", line=1, column=1)
    assert ed.get_text() == "Akeep"
    assert ed.canUndo()
    ed.undo()
    assert ed.get_text() == "keep"
    assert ed.canRedo()
    ed.redo()
    assert ed.get_text() == "Akeep"

    # --- theme round-trip ------------------------------------------------------
    themes = []
    ed.theme_changed.connect(themes.append)
    ed.set_theme("light")
    assert ed.get_theme() == "light"
    assert themes == ["light"], themes

    print("Monaco parity checks OK (%d text_changed announcements seen)" % len(seen))
    return 0


if __name__ == "__main__":
    sys.exit(check_parity())
