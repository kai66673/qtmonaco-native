"""Gate 1 parity checks: qmn.Monaco vs the qtmonaco.Monaco public API contract.

Drives the Python shim (bindings/python/qmn/__init__.py) against the semantics
frozen from qtmonaco's own monaco.py + backend/src/main.ts: 1-based cursors,
{"line":..,"column":..} cursor dicts, ValueError on read-only edits, the
set_text early-return ORDER (same-value, then readonly, then TypeError), the
insert_text line/column rules, the reveal flag mapping, and the M10 diff view
(open_diffs/close_diffs: modified-side value speech, one relay announcement,
the chrome geometry doors, the read-only ORIGINAL column driven by real
mouse gestures, the set_text diff-exit order, and the pinned same-modified
no-announcement divergence).

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
    from PySide6.QtCore import QPoint, Qt
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtTest import QTest
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
    # What STILL stays unbound on purpose (PLAN.md): qtmonaco's two "header"
    # hooks, whose whole job was to prepend text to the document ON THE WIRE
    # for its Chromium client. Every native LSP path (diagnostics, hover,
    # completion, rename, formatting, willSaveWaitUntil) rests on the
    # invariant "the server's document IS the editor's document", so a wire-
    # only header would be a +H/-H offset tax on all six paths at once — for a
    # hack no native stack needs (extra server context belongs in
    # initializationOptions, in the workspace configuration below, or in a
    # second document). An honest AttributeError beats a silent no-op.
    for gate2 in (
        "set_lsp_header",
        "get_lsp_header",
    ):
        assert not hasattr(ed, gate2), gate2

    # --- M8 parity wrap (1.8.0): custom actions + workspace config ---------
    # The two hooks whose semantics survive the native stack. The trigger
    # itself is a Qt-menu event that would block an offscreen run in
    # QMenu::exec, so parity pins the registration + the id bookkeeping here;
    # the trigger relay itself is pinned in tst_codeeditor (menuAction +
    # trigger() on the built menu, no exec).
    triggers = []
    ed.context_menu_action_triggered.connect(triggers.append)
    ed.add_action("add_scan", "Add Scan")
    ed.add_action("fmt", "Format File", language="cpp")  # accepted, ignored
    ed.add_action("add_scan", "Re-scan")  # idempotent by id: slot kept
    assert ed.contextActionIds() == ["add_scan", "fmt"], ed.contextActionIds()
    assert triggers == [], triggers  # the menu never opened in this process
    try:
        ed.add_action("", "no id")
        raise AssertionError("expected ValueError for an empty id")
    except ValueError:
        pass

    config = []
    # NOT `connect(config.append)`: this signal carries TWO args and
    # `list.append` takes one, so the slot raises TypeError INSIDE the emit —
    # PySide prints it and keeps going, the list stays empty and every pin
    # below reads as "the signal never fired" (this run caught that).
    ed.workspace_config_updated.connect(
        lambda ok, message: config.append((ok, message)))
    ed.update_workspace_configuration({"python": {"fmtOnSave": True}, "b": 2})
    # The dict lands on the native door as SORTED COMPACT JSON: that exact
    # string is what the C++ LspHub pushes as workspace/didChangeConfiguration
    # (the tst_lsp slot pins the wire echo byte for byte).
    assert ed.storedWorkspaceConfiguration() == \
        '{"b":2,"python":{"fmtOnSave":true}}', ed.storedWorkspaceConfiguration()
    assert config == [(True, "")], config  # the LOCAL accept (documented divergence)
    try:
        ed.update_workspace_configuration("not a dict")
        raise AssertionError("expected TypeError for a non-dict")
    except TypeError:
        pass
    try:
        ed.update_workspace_configuration({"bad": object()})
        raise AssertionError("expected TypeError for an unserializable dict")
    except TypeError:
        pass
    # TWO entries only: a non-dict raises BEFORE anything is announced (the
    # buffer is untouched, no False-frame either), while an unserializable dict
    # announces the refusal and then raises.
    assert len(config) == 2 and config[1][0] is False and config[1][1], config

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

    # From 1.6.1 the known Monaco labels also ride the M24 preset door (the
    # documented map); the label round-trip above stays qtmonaco-identical.
    Preset = qmn.CodeEditorTheme.Preset
    assert ed.theme().kind() == Preset.Light, ed.theme().kind()
    ed.set_theme("vs-dark")
    assert ed.theme().kind() == Preset.Dark, ed.theme().kind()
    ed.set_theme("hc-black")  # no high-contrast preset: label only, paint intact
    assert ed.theme().kind() == Preset.Dark, ed.theme().kind()
    assert ed.get_theme() == "hc-black"
    assert themes == ["light", "vs-dark", "hc-black"], themes

    # --- the M10 diff view (open_diffs / close_diffs) --------------------------
    # The fixture words sit at known columns (line 0 "alpha" cols 4..8, line
    # 1 "world" word chars cols 12..16), the line COUNT is equal on both
    # sides (the modified number column's digit width stays stable, so the
    # gutter grows by exactly diffChromeWidth()), and the session rides the
    # python grammar on both columns.
    diff_orig = "def alpha:\n    return 'world'"
    diff_mod = "def alpha:\n    return 'hi'"
    gutter_before, content_before = ed.gutterWidth(), ed.contentLeft()
    assert ed.diffChromeWidth() == 0  # closed: the chrome term is zero
    assert ed.foldZoneLeft() >= ed.padding()  # v1 shape, the door answers
    ed.open_diffs(diff_orig, diff_mod, language="python")
    assert ed.isDiffMode() is True
    assert ed.diffChromeWidth() > 0, ed.diffChromeWidth()
    assert ed.gutterWidth() == gutter_before + ed.diffChromeWidth(), (
        ed.gutterWidth(), gutter_before, ed.diffChromeWidth(),
    )
    assert ed.contentLeft() == content_before + ed.diffChromeWidth()
    # geometry doors in cells: textCol is the 50/50 parity split, never 0
    assert ed.diffChromeTextCol() > 0 and ed.diffChromeDigitsCol() > 0
    assert ed.diffChromeWidth() == ed.diffChromeTextCol() \
        + ed.diffChromeDigitsCol() + ed.diffChromeGap()
    # get_text speaks MODIFIED (qtmonaco: self._value = modified); the
    # announcement is the relay echo of the native feed — exactly one.
    assert ed.get_text() == diff_mod, ed.get_text()
    assert seen[-1] == diff_mod, seen[-1]
    assert seen.count(diff_mod) == 1, seen
    # the requested grammar rides the session (open_diffs emits NO
    # language_changed: set_language would paint the old grammar first):
    assert ed.get_language() == "python"
    assert langs[-1] == "typescript", langs
    # hunk navigation (F7/F8's doors): strict-edge hunks, silent at the edges
    ed.nextChange()
    ed.previousChange()
    # The read-only ORIGINAL column is driven by REAL mouse gestures (there
    # is no programmatic ORIGINAL selection door on purpose): warm the paint
    # cache, then chained double-clicks replay the v1 click chain in
    # ORIGINAL space (word on the first click; a second click at the same
    # spot would grow to the line — we stop at the word).
    ed.grab()
    cell = ed.cellWidth()
    lh = ed.lineHeight()
    win_x0 = ed.foldZoneLeft() + ed.foldZoneWidth() + ed.diffChromeDigitsCol() \
        + ed.diffChromeGap()
    y0 = ed.padding() + lh // 2  # row 0 (diff mode starts at the top)
    x_world = win_x0 + int(13 * cell + cell / 2 + 0.4)  # 'o' of 'world' (word
    # chars sit at cols 12..16: a click ON the quote (col 11) would select
    # only the quote — wordSelRange runs by class, quotes are class Other)
    x_alpha = win_x0 + int(4 * cell + cell / 2 + 0.4)   # inside 'alpha' (col 4)
    LEFT = Qt.MouseButton.LeftButton
    NO_MOD = Qt.KeyboardModifier.NoModifier
    QTest.mouseDClick(ed, LEFT, NO_MOD, QPoint(x_world, y0 + lh))
    assert ed.originalSelectionText() == "world", ed.originalSelectionText()
    QTest.mouseDClick(ed, LEFT, NO_MOD, QPoint(x_alpha, y0))
    assert ed.originalSelectionText() == "alpha", ed.originalSelectionText()
    # the ORIGINAL selection feeds the copy path even though the shim knows
    # nothing about it (the C++ copy() reads the original selection first):
    clip = QGuiApplication.clipboard()
    clip.setText("sentinel")
    ed.copy()
    assert clip.text() == "alpha", repr(clip.text())
    assert ed.originalSelectionText() == "alpha"  # copy never clears it
    # the ORIGINAL side is never editable: the shim's read-only pre-check
    # raises the same ValueError qtmonaco raises (the C++ doors stay silent).
    ed.set_readonly(True)
    for edit_call in (lambda: ed.insert_text("Z", line=2, column=1),
                      lambda: ed.delete_line(1)):
        try:
            edit_call()
            raise AssertionError("expected ValueError on a read-only edit")
        except ValueError:
            pass
    ed.set_readonly(False)
    assert ed.get_text() == diff_mod  # nothing was written anywhere
    # typing on the MODIFIED side still works mid-diff; the live recompute
    # seam (120 ms debounce) must not drop the session. column=1 is 1-based:
    # the Z lands at col 0, BEFORE the indent (qtmonaco's insert rule).
    ed.insert_text("Z", line=2, column=1)
    assert ed.get_text() == "def alpha:\nZ    return 'hi'", ed.get_text()
    QTest.qWait(400)  # let the debounced recompute land
    assert ed.isDiffMode() is True
    # PROGRAMMATIC doors never drop the read-side selection — only real
    # modified-side gestures/keys reach the C++ clear seam (mousePress/
    # keyPress/mouseDoubleClick/cut call it; setPosition and the edit doors
    # deliberately do not):
    assert ed.originalSelectionText() == "alpha", ed.originalSelectionText()
    # A real KEY on the modified side DOES drop it; Down at the last line
    # just re-places the caret there (the diff-mode modified-line walk).
    QTest.keyClick(ed, Qt.Key.Key_Down)
    assert ed.originalSelectionText() == "", ed.originalSelectionText()
    clip.setText("sentinel")
    ed.copy()  # selection gone: the v1 whole-line fallback runs on the
    # MODIFIED side (caret at {1,1} after the insert, on the LAST line:
    # no trailing newline to give)
    assert clip.text() == "Z    return 'hi'", repr(clip.text())
    # undo speaks the modified buffer MID-diff too (the doc is the truth;
    # the string-door feed wiped the pre-diff history, so the stack here is
    # exactly [feed boundary, the Z push] — one undo lands on diff_mod):
    ed.undo()
    assert ed.get_text() == diff_mod and ed.isDiffMode() is True
    assert seen.count(diff_mod) == 2, seen  # the undo's echo of the value
    # set_text EXITS diff mode, flag-before-early-return, exactly like
    # qtmonaco's `_diff_mode = False` sitting above the same-value guard.
    before = len(seen)
    ed.set_text(diff_mod)  # value EQUALS now: no announcement...
    assert len(seen) == before, seen
    assert ed.isDiffMode() is False  # ...but the diff session still closed
    assert ed.diffChromeWidth() == 0
    ed.set_text(diff_mod)  # same value again: the flag path is inert now
    assert len(seen) == before, seen
    # reopen WITHOUT the language param: the current grammar is kept (the
    # shim re-passes the current id — the C++ beginDiffMode always routes
    # setLanguage, so "" would really mean plain; the same-modified re-feed
    # announces NOTHING — the documented divergence from qtmonaco's
    # unconditional text_changed(modified) emit, pinned here):
    ed.open_diffs(diff_orig, diff_mod)
    assert ed.isDiffMode() is True
    assert ed.get_language() == "python"
    assert langs[-1] == "typescript", langs
    assert len(seen) == before, seen
    # the reopened session starts with a clean read side: the ORIGINAL
    # selection is gone (session teardown), and with the MODIFIED caret bare
    # at {0,0} the v1 whole-line copy fallback speaks (line 0 is not last:
    # the trailing newline rides along, Monaco-style):
    clip.setText("sentinel")
    assert ed.originalSelectionText() == ""
    ed.copy()
    assert clip.text() == "def alpha:\n", repr(clip.text())
    # close_diffs: the exit door qtmonaco never had (there, re-setting the
    # value is the only way out); closing twice is a silent no-op.
    ed.close_diffs()
    assert ed.isDiffMode() is False
    ed.close_diffs()
    assert ed.get_text() == diff_mod
    # a bare open_diffs on a read-only editor is LEGAL (no edit door was
    # blocked): qtmonaco pushes the same readOnly flag into its diff editor,
    # where the ORIGINAL side is read-only by design too, and the C++
    # string-door feed is not a user edit.
    before = len(seen)
    ed.set_readonly(True)
    ed.open_diffs("a", "b")
    assert ed.isDiffMode() is True and ed.get_text() == "b"
    assert seen[-1] == "b" and len(seen) == before + 1, seen  # value speaks
    ed.close_diffs()
    ed.set_readonly(False)
    # The string-door feed is the undo HISTORY boundary: the C++ replace
    # machinery (CodeEditor::setPlainText) CLEARS the core undo/redo stacks,
    # so one undo after the closed "b" diff is silent — the doc stays "b".
    # (Same net effect as Monaco's model.setValue: a full replace resets the
    # undo stack.)
    ed.undo()
    assert ed.get_text() == "b", ed.get_text()
    assert len(seen) == before + 1, seen  # only the "b" feed spoke
    # TypeErrors ride qtmonaco's exact checks; the options DICT is validated
    # and its keys (renderSideBySide:false included) are documented-IGNORED.
    for bad in (lambda: ed.open_diffs(None, "m"), lambda: ed.open_diffs("o", 42),
                lambda: ed.open_diffs("o", "m", options=1)):
        try:
            bad()
            raise AssertionError("expected TypeError from open_diffs")
        except TypeError:
            pass
    assert ed.isDiffMode() is False  # the TypeErrors never opened a session
    ed.open_diffs("o", "m", options={"renderSideBySide": False})  # legal
    assert ed.isDiffMode() is True
    assert ed.get_text() == "m"
    assert seen[-1] == "m" and len(seen) == before + 2, seen  # the feed speaks
    ed.close_diffs()
    assert ed.isDiffMode() is False
    # the diff_mod announcements in this whole section: the first open feed
    # and the mid-diff undo echo — nothing else ever spoke it.
    assert seen.count(diff_mod) == 2, seen

    print("Monaco parity checks OK (%d text_changed announcements seen)" % len(seen))
    return 0


if __name__ == "__main__":
    sys.exit(check_parity())
