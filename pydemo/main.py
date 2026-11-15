#!/usr/bin/env python3
"""qmn-pydemo - the Python analog of the C++ qmn-demo (src/app), on wheels.

A door-for-door mirror of the C++ tabbed demo: the same Russian window/menu/
status-bar UX, the same settings-template semantics (the dialog seeds from
what the user is looking at, and accepting pushes the template into every
open editor AND into future tabs), the same CLI contract (`--exit-after-ms`
for headless smoke runs, failures land in the status bar, never a blocking
dialog), and the SAME widget underneath: the C++ CodeEditor through the
shiboken wrappers of the shipped qmn_native wheel.

The settings surface is the FULL fifteen knobs of the C++ demo: the 14
option knobs ride `options()` (M19/M20/M21/M25) and the 15th, the M24 theme
preset, rides `theme()` — `CodeEditorTheme` crosses the wheel bindings from
wheel 1.6.1 on, so the theme combo is the real thing here too. The 14th
option knob, the M25 soft-wrap toggle, needs a wheel that carries M25
(>= 1.7.0; the path dep into dist/ is always current).
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from importlib.metadata import PackageNotFoundError, version
from pathlib import Path

from PySide6.QtCore import QTimer
from PySide6.QtGui import QAction, QFont, QFontDatabase, QKeySequence
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFileDialog,
    QFontComboBox,
    QFormLayout,
    QGroupBox,
    QLabel,
    QMainWindow,
    QSpinBox,
    QTabWidget,
    QVBoxLayout,
)

# The wheel's Python shim (bindings/python/qmn/__init__.py): it imports
# PySide6.QtWidgets BEFORE the _qmn extension (the wrapper resolves its
# QWidget base through shiboken's converter registry) and flattens
# _qmn.qmn.view.* to qmn.CodeEditor / qmn.CodeEditorTheme.
from qmn import CodeEditor, CodeEditorTheme

APP_TITLE = "qtmonaco-native \u2014 \u0434\u0435\u043c\u043e\u043d\u0441\u0442\u0440\u0430\u0446\u0438\u044f (Python)"


def language_for_suffix(suffix: str) -> str:
    """Suffix->grammar map, identical to the C++ demo/spike; an unmapped
    suffix stays plain text (setLanguage's documented fallback)."""
    return {
        "py": "python",
        "json": "json",
        "js": "javascript",
        "mjs": "javascript",
        "cjs": "javascript",
        "c": "c",
        "h": "cpp",
        "cpp": "cpp",
        "cc": "cpp",
        "cxx": "cpp",
        "hpp": "cpp",
        "hh": "cpp",
        "hxx": "cpp",
    }.get(suffix.lower(), "")


def read_text_file(path: str) -> tuple[str, str]:
    """(text, error), error "" on success. UTF-8 only, CRLF/CR normalized to
    LF - the same stance as the C++ demo loader (encoding detection stays a
    future milestone there too)."""
    candidate = Path(path)
    if not candidate.is_file():
        return "", "\u044d\u0442\u043e \u043d\u0435 \u0444\u0430\u0439\u043b"
    try:
        raw = candidate.read_bytes()
    except OSError as exc:
        return "", str(exc)
    # errors="replace" mirrors QString::fromUtf8's U+FFFD substitution.
    text = raw.decode("utf-8", "replace")
    return text.replace("\r\n", "\n").replace("\r", "\n"), ""


def demo_default_code_font() -> QFont:
    """The C++ demoDefaultCodeFont() recipe: the CodeEditor ctor's shipped
    font, pixel-sized (the demo contract throughout is pixel sizes)."""
    font = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
    font.setPixelSize(14)
    font.setHintingPreference(QFont.HintingPreference.PreferNoHinting)
    return font


LineNumbersMode = CodeEditor.LineNumbersMode
CurrentLineMode = CodeEditor.CurrentLineMode

# The dialog's closed enum rows, same order/labels as the C++ dialog (the
# label table is what a human reads; the order must match the enums).
LINE_MODES = [
    LineNumbersMode.Off,
    LineNumbersMode.On,
    LineNumbersMode.Relative,
    LineNumbersMode.Interval,
]
LINE_MODE_LABELS = [
    "\u0412\u044b\u043a\u043b\u044e\u0447\u0435\u043d\u044b",  # Выключены
    "\u041e\u0431\u044b\u0447\u043d\u044b\u0435",  # Обычные
    "\u041e\u0442\u043d\u043e\u0441\u0438\u0442\u0435\u043b\u044c\u043d\u044b\u0435",  # Относительные
    "\u0418\u043d\u0442\u0435\u0440\u0432\u0430\u043b\u044c\u043d\u044b\u0435",  # Интервальные
]
CURRENT_LINE_MODES = [
    CurrentLineMode.Off,
    CurrentLineMode.Background,
    CurrentLineMode.BackgroundFrame,
]
CURRENT_LINE_LABELS = [
    "\u041d\u0435\u0442",  # Нет
    "\u0424\u043e\u043d",  # Фон
    "\u0424\u043e\u043d \u0438 \u0440\u0430\u043c\u043a\u0430",  # Фон и рамка
]

ThemePreset = CodeEditorTheme.Preset

# M24 theme rows, the C++ dialog's table: Custom is deliberately NOT a row
# (a live editor only reaches it through manual role edits this demo never
# offers), and _index_of clamps such a seed to the Default row.
THEME_PRESETS = [ThemePreset.Default, ThemePreset.Light, ThemePreset.Dark]
THEME_LABELS = [
    "\u041f\u0430\u043b\u0438\u0442\u0440\u0430 (\u043f\u043e \u0443\u043c\u043e\u043b\u0447\u0430\u043d\u0438\u044e)",  # Палитра (по умолчанию)
    "\u0421\u0432\u0435\u0442\u043b\u0430\u044f",  # Светлая
    "\u0422\u0451\u043c\u043d\u0430\u044f",  # Тёмная
]


@dataclass
class EditorSettings:
    """Mirror of the C++ EditorSettings template, all fifteen knobs.
    Defaults = CodeEditor's shipped defaults, like the C++ tripwire test
    demands."""

    line_numbers_mode: LineNumbersMode = LineNumbersMode.On
    current_line_mode: CurrentLineMode = CurrentLineMode.Background
    long_line_ruler_column: int = 0  # 0 = off
    tab_stop_width: int = 4
    auto_indent_enabled: bool = True
    tab_changes_focus: bool = False
    overview_ruler_enabled: bool = True
    scroll_beyond_last_line_enabled: bool = False
    auto_close_enabled: bool = True
    read_only: bool = False
    indent_guides_enabled: bool = True
    soft_wrap_enabled: bool = False  # M25 word wrap (14th options knob)
    code_font: QFont = field(default_factory=demo_default_code_font)
    zoom_factor: float = 1.0
    theme_preset: ThemePreset = ThemePreset.Default


def apply_settings(editor: CodeEditor, settings: EditorSettings) -> None:
    """The options() object is the demo's one door in (as in C++): every
    setter delegates to the editor and the editor emits the *Changed
    signal. The M24 theme preset rides theme() next, mirroring the C++
    applySettings rule exactly."""
    options = editor.options()
    options.setLineNumbersMode(settings.line_numbers_mode)
    options.setCurrentLineMode(settings.current_line_mode)
    options.setLongLineRulerColumn(settings.long_line_ruler_column)
    options.setTabStopWidth(settings.tab_stop_width)
    options.setAutoIndentEnabled(settings.auto_indent_enabled)
    options.setTabChangesFocus(settings.tab_changes_focus)
    options.setOverviewRulerEnabled(settings.overview_ruler_enabled)
    options.setScrollBeyondLastLineEnabled(settings.scroll_beyond_last_line_enabled)
    options.setAutoCloseEnabled(settings.auto_close_enabled)
    options.setReadOnly(settings.read_only)
    options.setIndentGuidesEnabled(settings.indent_guides_enabled)
    options.setSoftWrapEnabled(settings.soft_wrap_enabled)  # M25 (14th options knob)
    options.setCodeFont(settings.code_font)
    options.setZoomFactor(settings.zoom_factor)

    # M24 (14th knob): same-kind seeds skip applyPreset WHOLE - otherwise
    # every dialog accept would wipe the map and repaint. Any other kind
    # (a Custom left by manual edits included) applies the preset as-is.
    theme = editor.theme()
    if theme.kind() != settings.theme_preset:
        theme.applyPreset(settings.theme_preset)


def settings_from(editor: CodeEditor) -> EditorSettings:
    """Seeds the dialog from LIVE editor state (options never goes stale by
    the M19 contract; its getters are the template doors, M23-safe)."""
    options = editor.options()
    return EditorSettings(
        line_numbers_mode=options.lineNumbersMode(),
        current_line_mode=options.currentLineMode(),
        long_line_ruler_column=options.longLineRulerColumn(),
        tab_stop_width=options.tabStopWidth(),
        auto_indent_enabled=options.autoIndentEnabled(),
        tab_changes_focus=options.tabChangesFocus(),
        overview_ruler_enabled=options.overviewRulerEnabled(),
        scroll_beyond_last_line_enabled=options.scrollBeyondLastLineEnabled(),
        auto_close_enabled=options.autoCloseEnabled(),
        read_only=options.isReadOnly(),
        indent_guides_enabled=options.indentGuidesEnabled(),
        soft_wrap_enabled=options.softWrapEnabled(),  # M25
        code_font=options.codeFont(),
        zoom_factor=options.zoomFactor(),
        theme_preset=editor.theme().kind(),  # M24: live kind, Custom included
    )


def _index_of(table: list, value) -> int:
    """Defensive row lookup (the C++ indexOfMode): an unknown value clamps
    to the first row - the bound enums are closed, so this only guards a
    library that renumbers rows on us."""
    try:
        return table.index(value)
    except ValueError:
        return 0


class SettingsDialog(QDialog):
    """Настройки->Параметры редактора… - the C++ EditorSettingsDialog's
    rows, the M24 theme combo included (wheel >= 1.6.1)."""

    def __init__(self, parent, initial: EditorSettings):
        super().__init__(parent)
        self.setWindowTitle("\u041f\u0430\u0440\u0430\u043c\u0435\u0442\u0440\u044b \u0440\u0435\u0434\u0430\u043a\u0442\u043e\u0440\u0430")
        self._seed = initial

        line_numbers = QComboBox(self)
        line_numbers.addItems(LINE_MODE_LABELS)
        line_numbers.setCurrentIndex(_index_of(LINE_MODES, initial.line_numbers_mode))
        current_line = QComboBox(self)
        current_line.addItems(CURRENT_LINE_LABELS)
        current_line.setCurrentIndex(_index_of(CURRENT_LINE_MODES, initial.current_line_mode))
        ruler = QSpinBox(self)
        ruler.setRange(0, 999)
        ruler.setValue(initial.long_line_ruler_column)
        theme = QComboBox(self)
        theme.addItems(THEME_LABELS)
        theme.setCurrentIndex(_index_of(THEME_PRESETS, initial.theme_preset))

        appearance = QGroupBox("\u0412\u043d\u0435\u0448\u043d\u0438\u0439 \u0432\u0438\u0434", self)
        appearance_form = QFormLayout(appearance)
        appearance_form.addRow("\u041d\u043e\u043c\u0435\u0440\u0430 \u0441\u0442\u0440\u043e\u043a:", line_numbers)
        appearance_form.addRow("\u041f\u043e\u0434\u0441\u0432\u0435\u0442\u043a\u0430 \u0442\u0435\u043a\u0443\u0449\u0435\u0439 \u0441\u0442\u0440\u043e\u043a\u0438:", current_line)
        appearance_form.addRow("\u041b\u0438\u043d\u0438\u044f-\u043e\u0433\u0440\u0430\u043d\u0438\u0447\u0438\u0442\u0435\u043b\u044c (\u0441\u0442\u043e\u043b\u0431\u0435\u0446):", ruler)
        appearance_form.addRow("\u0422\u0435\u043c\u0430:", theme)  # M24 (14th knob)

        font = QFontComboBox(self)
        family = initial.code_font.family()
        row = font.findText(family)
        if row < 0:  # aliases like "monospace" are rows of their own text
            font.addItem(family)
            row = font.count() - 1
        font.setCurrentIndex(row)
        font_px = QSpinBox(self)
        font_px.setRange(1, 128)  # the demo contract is pixel-sized fonts
        font_px.setValue(initial.code_font.pixelSize())
        zoom = QDoubleSpinBox(self)
        # The C++ dialog uses CodeEditor::kZoomFactorMin/Max; those static
        # constants do not cross the wheel, so the values are mirrored here
        # (the editor's own clamp stays the real authority).
        zoom.setRange(0.25, 4.0)
        zoom.setSingleStep(0.05)
        zoom.setValue(initial.zoom_factor)

        font_box = QGroupBox("\u0428\u0440\u0438\u0444\u0442", self)
        font_form = QFormLayout(font_box)
        font_form.addRow("\u0428\u0440\u0438\u0444\u0442:", font)
        font_form.addRow("\u0420\u0430\u0437\u043c\u0435\u0440:", font_px)
        font_form.addRow("\u041c\u0430\u0441\u0448\u0442\u0430\u0431:", zoom)

        tab_stop = QSpinBox(self)
        tab_stop.setRange(1, 32)  # the editor clamps >= 1; 32 is demo sanity
        tab_stop.setValue(initial.tab_stop_width)
        auto_indent = QCheckBox("\u0410\u0432\u0442\u043e\u043e\u0442\u0441\u0442\u0443\u043f \u043f\u0440\u0438 Enter", self)
        auto_indent.setChecked(initial.auto_indent_enabled)
        auto_close = QCheckBox("\u0410\u0432\u0442\u043e\u0437\u0430\u043a\u0440\u044b\u0442\u0438\u0435 \u0441\u043a\u043e\u0431\u043e\u043a \u0438 \u043a\u0430\u0432\u044b\u0447\u0435\u043a", self)
        auto_close.setChecked(initial.auto_close_enabled)
        indent_guides = QCheckBox("\u0412\u0435\u0440\u0442\u0438\u043a\u0430\u043b\u044c\u043d\u044b\u0435 \u043b\u0438\u043d\u0438\u0438 \u043e\u0442\u0441\u0442\u0443\u043f\u043e\u0432", self)
        indent_guides.setChecked(initial.indent_guides_enabled)
        soft_wrap = QCheckBox("\u041f\u0435\u0440\u0435\u043d\u043e\u0441 \u0434\u043b\u0438\u043d\u043d\u044b\u0445 \u0441\u0442\u0440\u043e\u043a \u043f\u043e \u0441\u043b\u043e\u0432\u0430\u043c", self)
        soft_wrap.setChecked(initial.soft_wrap_enabled)  # M25 (14th options knob)
        tab_focus = QCheckBox("Tab \u043f\u0435\u0440\u0435\u0432\u043e\u0434\u0438\u0442 \u0444\u043e\u043a\u0443\u0441 (\u043d\u0435 \u043e\u0442\u0441\u0442\u0443\u043f)", self)
        tab_focus.setChecked(initial.tab_changes_focus)
        overview_ruler = QCheckBox("\u041b\u0438\u043d\u0435\u0439\u043a\u0430 \u043e\u0431\u0437\u043e\u0440\u0430 \u0441\u043f\u0440\u0430\u0432\u0430", self)
        overview_ruler.setChecked(initial.overview_ruler_enabled)
        scroll_past_end = QCheckBox("\u041f\u0440\u043e\u043a\u0440\u0443\u0442\u043a\u0430 \u0437\u0430 \u043f\u043e\u0441\u043b\u0435\u0434\u043d\u0435\u0439 \u0441\u0442\u0440\u043e\u043a\u043e\u0439", self)
        scroll_past_end.setChecked(initial.scroll_beyond_last_line_enabled)
        read_only = QCheckBox("\u0422\u043e\u043b\u044c\u043a\u043e \u0447\u0442\u0435\u043d\u0438\u0435", self)
        read_only.setChecked(initial.read_only)

        text = QGroupBox("\u0422\u0435\u043a\u0441\u0442 \u0438 \u043f\u043e\u0432\u0435\u0434\u0435\u043d\u0438\u0435", self)
        text_form = QFormLayout(text)
        text_form.addRow("\u0428\u0438\u0440\u0438\u043d\u0430 \u0442\u0430\u0431\u0443\u043b\u044f\u0446\u0438\u0438:", tab_stop)
        text_form.addRow(auto_indent)
        text_form.addRow(auto_close)
        text_form.addRow(indent_guides)
        text_form.addRow(soft_wrap)  # M25 (14th options knob)
        text_form.addRow(tab_focus)
        text_form.addRow(overview_ruler)
        text_form.addRow(scroll_past_end)
        text_form.addRow(read_only)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel,
            self,
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)

        layout = QVBoxLayout(self)
        layout.addWidget(appearance)
        layout.addWidget(font_box)
        layout.addWidget(text)
        layout.addWidget(buttons)

        # Named through attributes so settings() can read the rows back.
        self._line_numbers, self._current_line, self._ruler = line_numbers, current_line, ruler
        self._theme_combo = theme
        self._font, self._font_px, self._zoom = font, font_px, zoom
        self._tab_stop = tab_stop
        self._checks = {
            "auto_indent_enabled": auto_indent,
            "auto_close_enabled": auto_close,
            "indent_guides_enabled": indent_guides,
            "soft_wrap_enabled": soft_wrap,  # M25
            "tab_changes_focus": tab_focus,
            "overview_ruler_enabled": overview_ruler,
            "scroll_beyond_last_line_enabled": scroll_past_end,
            "read_only": read_only,
        }

    def settings(self) -> EditorSettings:
        result = EditorSettings(
            line_numbers_mode=LINE_MODES[self._line_numbers.currentIndex()],
            current_line_mode=CURRENT_LINE_MODES[self._current_line.currentIndex()],
            long_line_ruler_column=self._ruler.value(),
            tab_stop_width=self._tab_stop.value(),
            code_font=self._seed_font_result(),
            zoom_factor=self._zoom.value(),
            theme_preset=THEME_PRESETS[self._theme_combo.currentIndex()],  # M24
        )
        for name, check in self._checks.items():
            setattr(result, name, check.isChecked())
        return result

    def _seed_font_result(self) -> QFont:
        """The C++ rule: the seed font is PATCHED, never rebuilt - only the
        two edited fields move, family switching only on a real row change
        (aliases like "monospace" must not get rewritten on the way out)."""
        font = QFont(self._seed.code_font)
        row_text = self._font.currentText()
        if row_text and row_text != self._seed.code_font.family():
            chosen = self._font.currentFont().family()
            if chosen:
                font.setFamily(chosen)
        font.setPixelSize(self._font_px.value())
        return font


class MainWindow(QMainWindow):
    """The C++ MainWindow: tabs of CodeEditors + Файл/Настройки menus + a
    caret/selection status readout."""

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle(APP_TITLE)
        self.resize(1100, 700)
        self._settings = EditorSettings()  # the template for new tabs
        self._last_dir = ""

        self._tabs = QTabWidget(self)
        self._tabs.setTabsClosable(True)
        self._tabs.setDocumentMode(True)
        self.setCentralWidget(self._tabs)
        self._tabs.tabCloseRequested.connect(self._close_tab_at)
        self._tabs.currentChanged.connect(self._on_tab_changed)

        file_menu = self.menuBar().addMenu("&\u0424\u0430\u0439\u043b")
        open_action = QAction("&\u041e\u0442\u043a\u0440\u0442\u044c\u2026", self)
        open_action.setShortcut(QKeySequence.StandardKey.Open)
        open_action.triggered.connect(self._choose_files)
        file_menu.addAction(open_action)
        file_menu.addSeparator()
        close_action = QAction("&\u0417\u0430\u043a\u0440\u044b\u0442\u044c \u0432\u043a\u043b\u0430\u0434\u043a\u0443", self)
        close_action.setShortcut(QKeySequence.StandardKey.Close)
        close_action.triggered.connect(lambda: self._close_tab_at(self._tabs.currentIndex()))
        file_menu.addAction(close_action)

        settings_menu = self.menuBar().addMenu("&\u041d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438")
        settings_action = QAction("\u041f\u0430\u0440\u0430\u043c\u0435\u0442\u0440\u044b &\u0440\u0435\u0434\u0430\u043a\u0442\u043e\u0440\u0430\u2026", self)
        # Portable shortcut text instead of Qt.Modifier|Qt.Key bit math:
        # PySide6 6.12 exposes those as Python enums of different types.
        settings_action.setShortcut(QKeySequence("Ctrl+,"))
        settings_action.triggered.connect(self._edit_settings)
        settings_menu.addAction(settings_action)

        self._status_label = QLabel(self)
        self.statusBar().addPermanentWidget(self._status_label)
        self._update_status_label()

    # --- opening and tabs (openPath/addEditorTab/closeTabAt mirrors) ---------

    def open_path(self, path: str) -> bool:
        text, error = read_text_file(path)
        if error:
            # Non-modal on purpose: a status-bar failure keeps the app (and
            # the offscreen smoke run) free of blocking dialogs.
            self.statusBar().showMessage(
                "\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u043e\u0442\u043a\u0440\u044b\u0442\u044c \u00ab{}\u00bb: {}".format(path, error)
            )
            return False
        self._last_dir = str(Path(path).resolve().parent)
        self._add_editor_tab(path, text)
        return True

    def _choose_files(self) -> None:
        paths, _ = QFileDialog.getOpenFileNames(
            self, "\u041e\u0442\u043a\u0440\u044b\u0442\u044c \u0444\u0430\u0439\u043b", self._last_dir,
            "\u0412\u0441\u0435 \u0444\u0430\u0439\u043b\u044b (*)",
        )
        for path in paths:
            self.open_path(path)

    def _add_editor_tab(self, path: str, text: str) -> None:
        editor = CodeEditor()
        editor.setPlainText(text)
        language = language_for_suffix(Path(path).suffix.lstrip("."))
        if language:
            editor.setLanguage(language)
        apply_settings(editor, self._settings)  # the tab inherits the template

        index = self._tabs.addTab(editor, Path(path).name)
        self._tabs.tabBar().setTabData(index, path)  # QTabWidget has no tabData
        editor.cursorMoved.connect(self._update_status_label)
        editor.selectionChanged.connect(self._update_status_label)
        self._tabs.setCurrentIndex(index)

    def _close_tab_at(self, index: int) -> None:
        page = self._tabs.widget(index)
        if page is None:
            return
        self._tabs.removeTab(index)  # does NOT delete: deleteLater does
        page.deleteLater()
        self._on_tab_changed()

    def _current_editor(self):
        widget = self._tabs.currentWidget()
        return widget if isinstance(widget, CodeEditor) else None

    # --- titles and status (setCurrentTabTitle/updateStatusLabel mirrors) ---

    def _on_tab_changed(self, _index: int = -1) -> None:
        self._set_current_tab_title()
        self._update_status_label()

    def _set_current_tab_title(self) -> None:
        title = APP_TITLE
        index = self._tabs.currentIndex()
        if index >= 0:
            title = "{} \u2014 {}".format(self._tabs.tabText(index), APP_TITLE)
        self.setWindowTitle(title)

    def _update_status_label(self, _pos=None) -> None:
        editor = self._current_editor()
        if editor is None:
            self._status_label.clear()
            return
        # 1-based display (user-facing), mirroring qtmonaco's convention;
        # selectedLength() is O(log P) with no text copy, so per-keystroke
        # updates are cheap (the same reasoning as the C++ demo).
        position = editor.position()
        text = "\u0421\u0442\u0440. {}, \u043a\u043e\u043b. {}".format(position.line + 1, position.col + 1)
        if editor.hasSelection():
            text += " \u00b7 \u0432\u044b\u0434\u0435\u043b\u0435\u043d\u043e {}".format(editor.selectedLength())
        self._status_label.setText(text)

    # --- settings dialog (editSettings/applyEditorSettings mirrors) ----------

    def _edit_settings(self) -> None:
        # Seed from what the user is looking at; fall back to the stored
        # template when no editor is open.
        editor = self._current_editor()
        initial = settings_from(editor) if editor is not None else self._settings
        dialog = SettingsDialog(self, initial)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            self._apply_template(dialog.settings())

    def _apply_template(self, settings: EditorSettings) -> None:
        self._settings = settings  # template for future tabs...
        for i in range(self._tabs.count()):
            widget = self._tabs.widget(i)
            if isinstance(widget, CodeEditor):
                apply_settings(widget, settings)  # ...and every open editor now
        self._update_status_label()


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="qmn-pydemo",
        description="qtmonaco-native demo (Python): \u0432\u043a\u043b\u0430\u0434\u043a\u0438-\u0440\u0435\u0434\u0430\u043a\u0442\u043e\u0440\u044b "
        "+ \u0434\u0438\u0430\u043b\u043e\u0433 \u043d\u0430\u0441\u0442\u0440\u043e\u0435\u043a "
        "(\u0424\u0430\u0439\u043b->\u041e\u0442\u043a\u0440\u044b\u0442\u044c\u2026, "
        "\u041d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438->\u041f\u0430\u0440\u0430\u043c\u0435\u0442\u0440\u044b "
        "\u0440\u0435\u0434\u0430\u043a\u0442\u043e\u0440\u0430\u2026)",
    )
    parser.add_argument("files", nargs="*", help="text files to open as tabs")
    parser.add_argument(
        "--exit-after-ms", type=int, default=None, metavar="MS",
        help="quit automatically after the given delay (headless smoke runs)",
    )
    args = parser.parse_args()

    app = QApplication(sys.argv)
    QApplication.setApplicationName("qmn-pydemo")
    try:
        QApplication.setApplicationVersion(version("qmn-native"))
    except PackageNotFoundError:  # e.g. run from a source tree without the wheel
        QApplication.setApplicationVersion("unknown")

    window = MainWindow()
    for path in args.files:
        window.open_path(path)  # CLI failures land in the status bar, like menu opens
    window.show()

    if args.exit_after_ms is not None:
        QTimer.singleShot(args.exit_after_ms, app.quit)
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
