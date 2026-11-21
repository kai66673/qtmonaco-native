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

    # --- M25 document object (Gate 2, car 1; bound with M25) ------------------
    # The risky crossings here: the editor-owned QObject* out of the document()
    # door, and the 3-arg editApplied signal (int/int/str marshalling).
    doc = editor.document()
    assert isinstance(doc, qmn.CodeDocument)  # the shim export crosses too
    assert doc is not None and doc.toPlainText() == TEXT
    assert doc.lineCount() == 3 and doc.lineText(1) == "bravo"
    assert doc.lineText(99) == ""  # clamped public getter

    doc_hits = []
    doc.editApplied.connect(lambda s, e, t: doc_hits.append(("delta", s, e, t)))
    doc.textChanged.connect(lambda: doc_hits.append("changed"))
    doc.modificationChanged.connect(lambda m: doc_hits.append(("mod", m)))
    doc.documentUriChanged.connect(lambda u: doc_hits.append(("uri", u)))

    assert doc.isModified() is True  # the setPlainText above dirtied it (load semantics)
    doc.markSaved()                  # the consumer load pattern: set-then-markSaved
    assert doc.isModified() is False and doc_hits == [("mod", False)], doc_hits
    doc_hits.clear()
    doc.setDocumentUri("untitled:smoke")  # the LSP-facing key
    doc.setDocumentUri("untitled:smoke")  # same value: NO emission
    assert doc.documentUri() == "untitled:smoke"
    assert doc_hits == [("uri", "untitled:smoke")], doc_hits

    # The pre-op-frame delta stream (the frame the LSP syncer and diff car
    # will ride), and v1 modified semantics: any edit dirties, only
    # markSaved() clears.
    doc_hits.clear()
    doc.setPlainText("ab")  # routes through the editor's replace machinery
    assert editor.toPlainText() == "ab"  # the delegation crossed the wheel too
    assert doc_hits == [("mod", True), ("delta", 0, len(TEXT), "ab"), "changed"], doc_hits
    assert doc.isModified()
    doc_hits.clear()
    doc.markSaved()
    assert not doc.isModified() and doc_hits == [("mod", False)], doc_hits

    # --- M25 soft wrap knob (the 14th options knob, new to the wheel with M25) --
    # The FIRST knob-door smoke (the 13 older knobs predate the bindings
    # suite): default OFF crosses, the setter delegates editor-wards, the
    # *Changed signal crosses back, a same-value set does not re-emit.
    opt = editor.options()
    wrap_hits = []
    opt.softWrapEnabledChanged.connect(wrap_hits.append)
    assert opt.softWrapEnabled() is False  # the shipped default
    opt.setSoftWrapEnabled(True)
    assert opt.softWrapEnabled() is True and wrap_hits == [True], wrap_hits
    opt.setSoftWrapEnabled(True)  # same value: no emission
    assert wrap_hits == [True], wrap_hits
    # The row doors still answer in slot units from Python (wrap ON, one
    # line, hidden editor: the unit contract, not geometry - a hidden
    # editor's paint budget may legitimately be 0 slots).
    assert editor.firstVisibleLine() == 0
    assert editor.modelLineForVisibleIndex(0) == 0
    opt.setSoftWrapEnabled(False)
    assert wrap_hits == [True, False], wrap_hits

    copies = loaded_libs("libQt6Core.so")
    print(f"qmn module: {qmn.__file__}")
    print(f"signals:    {seen}")
    print(f"QtCore:     {sorted(copies)}")
    assert len(copies) == 1, f"more than one QtCore in the process: {sorted(copies)}"

    # --- G3 glue shelf: the wheel's FIRST types beyond the view -------------
    # The risky crossings (PLAN "G3 design pass"): QJsonObject/QJsonValue as
    # DIRECT params and ServerSpec's QJsonObject member - JSON has NEVER
    # crossed this wheel before, these pins are why. PINNED REALITY
    # (2026-11-21 run): PySide6 6.12's own typesystem declares QJsonObject a
    # PRIMITIVE type - the Python DICT is the JSON object. No QJsonObject
    # class exists to import at all (QJsonArray/QJsonValue/QJsonDocument do);
    # the generated doors convert dict <-> dict DEEPLY, both directions and
    # through struct members. The shim fallback the design pass sketched is
    # NOT needed. Same NO-SERVER discipline as tst_glue: only languages NO
    # built-in spec claims get opened (the container has no clangd; a
    # claimed open could only lazy-start into failure).
    hub = qmn.LspHub()
    session = qmn.ProjectSession()
    session.attachHub(hub)
    assert session.hub() is hub  # the raw NON-owning pointer crosses back
    assert hub.specNameForLanguage("cpp") == "clangd"  # the built-in pin

    # Honest absence (the G3 drop list): the std::function feature doors and
    # the unbound-type doors never reached Python - AttributeError on the
    # class, never a silent no-op.
    for missing in ("requestHover", "requestCompletion", "requestRename",
                    "setTransportFactory", "clientForSpec", "syncerFor",
                    "applyWorkspaceEdit"):
        assert not hasattr(hub, missing), f"dropped door crossed: {missing}"

    # JSON IN: the config body travels as a plain dict (the primitive-type
    # pin; the design pass expected to build QJsonObject by ctor calls -
    # better reality, pinned not assumed).
    assert session.loadConfigFromJson({"languages": {"g3ext": "json"}}) is True
    assert session.languageForSuffix("g3ext") == "json"
    assert session.loadConfigFromJson({}) is False  # the empty body door too

    # ServerSpec: the nested record, and its QJsonObject member both ways -
    # a dict IN (primitive-type pin) and the whole spec back OUT as a dict
    # through the serverSpec pin door (name/program/args/languages/
    # initializeTemplate keys, deep values).
    spec = qmn.LspHub.ServerSpec()
    spec.name = "g3server"
    spec.program = "g3-echo"
    spec.languages = ["g3lang"]
    spec.initializeTemplate = {"initializationOptions": {"g3": True}}
    hub.setServerSpec(spec)
    assert hub.specNameForLanguage("g3lang") == "g3server"
    pinned = hub.serverSpec("g3server")
    assert isinstance(pinned, dict), type(pinned)
    assert pinned["program"] == "g3-echo", pinned
    assert pinned["languages"] == ["g3lang"], pinned
    assert pinned["initializeTemplate"] == {
        "initializationOptions": {"g3": True}}, pinned

    # The one-call open on an UNCLAIMED language: session-side registration,
    # the hub's documented false no-op, triggers stay DISARMED (the hub never
    # accepted the document). Path door normalizes to the file:// uri key.
    other = qmn.CodeEditor()
    opened, closed_uris = [], []
    session.documentOpened.connect(opened.append)
    session.documentClosed.connect(closed_uris.append)
    assert session.openDocument(other, "/tmp/g3-smoke/unclaimed.py") is True
    assert opened == ["file:///tmp/g3-smoke/unclaimed.py"], opened
    assert other.language() == "python"
    assert session.openCount() == 1
    assert session.openUris() == ["file:///tmp/g3-smoke/unclaimed.py"]
    assert session.languageForUri("/tmp/g3-smoke/unclaimed.py") == "python"
    assert session.editorForUri("file:///tmp/g3-smoke/unclaimed.py") is other
    assert session.popupsOnOpen() is True
    assert hub.popupTriggersEnabled(other) is False  # never accepted, never armed
    assert session.closeUri("file:///tmp/g3-smoke/unclaimed.py") is True
    assert session.openCount() == 0
    assert closed_uris == ["file:///tmp/g3-smoke/unclaimed.py"], closed_uris
    session.closeDocument(other)  # unregistered now: the silent no-op
    assert closed_uris == ["file:///tmp/g3-smoke/unclaimed.py"], closed_uris

    # The hub relays cross as ordinary PySide signals (the answer route
    # clientForSpec()->respond... is C++-only - documented).
    assert hasattr(hub, "serverShowMessage") and hasattr(hub, "applyEditRefused")

    network = loaded_libs("libQt6Network.so")
    print(f"QtNetwork:  {sorted(network)}")
    assert len(network) == 1, f"more than one QtNetwork in the process: {sorted(network)}"

    app.processEvents()
    print("OK: qmn binding smoke test passed")
    return 0
