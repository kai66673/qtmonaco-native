#include "EditorSettingsDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace qmn::app {

namespace {

// Enum<->row maps: EXPLICIT value arrays, so a future enum reshuffle in
// the library cannot silently renumber the combo rows (the round-trip
// test walks every row, but the label text is what a human reads).
constexpr CodeEditor::LineNumbersMode kLineModes[] = {
    CodeEditor::LineNumbersMode::Off,
    CodeEditor::LineNumbersMode::On,
    CodeEditor::LineNumbersMode::Relative,
    CodeEditor::LineNumbersMode::Interval,
};
const char* const kLineModeLabels[] = {
    "Выключены", "Обычные", "Относительные", "Интервальные",
};

constexpr CodeEditor::CurrentLineMode kCurrentLineModes[] = {
    CodeEditor::CurrentLineMode::Off,
    CodeEditor::CurrentLineMode::Background,
    CodeEditor::CurrentLineMode::BackgroundFrame,
};
const char* const kCurrentLineLabels[] = {
    "Нет", "Фон", "Фон и рамка",
};

// M24 theme rows. Custom is deliberately NOT a row: a live editor can
// only reach it through manual role edits the demo never offers, and
// indexOfMode below clamps such a seed to the Default row (the same
// defensive clamp the M19 enum arrays use).
constexpr CodeEditorTheme::Preset kThemePresets[] = {
    CodeEditorTheme::Preset::Default,
    CodeEditorTheme::Preset::Light,
    CodeEditorTheme::Preset::Dark,
};
const char* const kThemeLabels[] = {
    "Палитра (по умолчанию)", "Светлая", "Тёмная",
};

template <typename Mode, int N>
int indexOfMode(const Mode (&values)[N], Mode mode)
{
    for (int i = 0; i < N; ++i)
        if (values[i] == mode)
            return i;
    return 0; // unknown value: clamp to the first row (defensive, M19 enums are closed)
}

} // namespace

EditorSettingsDialog::EditorSettingsDialog(QWidget* parent, const EditorSettings& initial)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Настройки редактора"));

    lineNumbersCombo_ = new QComboBox(this);
    for (const char* label : kLineModeLabels)
        lineNumbersCombo_->addItem(QString::fromUtf8(label));
    currentLineCombo_ = new QComboBox(this);
    for (const char* label : kCurrentLineLabels)
        currentLineCombo_->addItem(QString::fromUtf8(label));
    themeCombo_ = new QComboBox(this); // M24 (14th knob)
    for (const char* label : kThemeLabels)
        themeCombo_->addItem(QString::fromUtf8(label));

    rulerSpin_ = new QSpinBox(this);
    rulerSpin_->setRange(0, 999);
    rulerSpin_->setSpecialValueText(QStringLiteral("нет"));

    tabStopSpin_ = new QSpinBox(this);
    tabStopSpin_->setRange(1, 32); // the editor clamps >= 1; 32 is demo sanity

    // M20: the "Шрифт и масштаб" trio. The zoom spin's range IS the
    // editor's documented clamp window, so the control never fights the
    // setter; decimals 2 + step 0.05 keep values readable (any double
    // in range applies verbatim; the editor clamps defensively anyway).
    // The px spin caps at 128: beyond that the demo window cannot show
    // even one line and the number stops meaning anything.
    fontCombo_ = new QFontComboBox(this);
    fontPxSpin_ = new QSpinBox(this);
    fontPxSpin_->setRange(1, 128);
    fontPxSpin_->setSuffix(QStringLiteral(" px"));
    zoomSpin_ = new QDoubleSpinBox(this);
    zoomSpin_->setRange(CodeEditor::kZoomFactorMin, CodeEditor::kZoomFactorMax);
    zoomSpin_->setDecimals(2);
    zoomSpin_->setSingleStep(0.05);

    autoIndentCheck_ = new QCheckBox(QStringLiteral("Автоотступ при Enter"), this);
    tabFocusCheck_ = new QCheckBox(QStringLiteral("Tab переводит фокус (не отступ)"), this);
    overviewRulerCheck_ = new QCheckBox(QStringLiteral("Линейка обзора справа"), this);
    scrollPastEndCheck_ = new QCheckBox(QStringLiteral("Прокрутка за последней строкой"), this);
    autoCloseCheck_ = new QCheckBox(QStringLiteral("Автозакрытие скобок и кавычек"), this);
    readOnlyCheck_ = new QCheckBox(QStringLiteral("Только чтение"), this);
    indentGuidesCheck_ = new QCheckBox(QStringLiteral("Вертикальные линии отступов"), this);

    auto* appearance = new QGroupBox(QStringLiteral("Внешний вид"), this);
    auto* appearanceForm = new QFormLayout(appearance);
    appearanceForm->addRow(QStringLiteral("Номера строк:"), lineNumbersCombo_);
    appearanceForm->addRow(QStringLiteral("Подсветка текущей строки:"), currentLineCombo_);
    appearanceForm->addRow(QStringLiteral("Линия-ограничитель (столбец):"), rulerSpin_);
    appearanceForm->addRow(QStringLiteral("Тема:"), themeCombo_); // M24

    auto* font = new QGroupBox(QStringLiteral("Шрифт и масштаб"), this);
    auto* fontForm = new QFormLayout(font);
    fontForm->addRow(QStringLiteral("Шрифт:"), fontCombo_);
    fontForm->addRow(QStringLiteral("Размер:"), fontPxSpin_);
    fontForm->addRow(QStringLiteral("Масштаб:"), zoomSpin_);

    auto* text = new QGroupBox(QStringLiteral("Текст и отступы"), this);
    auto* textForm = new QFormLayout(text);
    textForm->addRow(QStringLiteral("Ширина табуляции:"), tabStopSpin_);
    textForm->addRow(autoIndentCheck_);
    textForm->addRow(autoCloseCheck_);
    textForm->addRow(indentGuidesCheck_); // M21 (13th knob)

    auto* behavior = new QGroupBox(QStringLiteral("Поведение"), this);
    auto* behaviorForm = new QFormLayout(behavior);
    behaviorForm->addRow(tabFocusCheck_);
    behaviorForm->addRow(overviewRulerCheck_);
    behaviorForm->addRow(scrollPastEndCheck_);
    behaviorForm->addRow(readOnlyCheck_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    // Explicit Russian labels: the standard-button texts would be Qt's
    // own (English in a bare build, no catalog shipped here).
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Принять"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(font);
    layout->addWidget(appearance);
    layout->addWidget(text);
    layout->addWidget(behavior);
    layout->addWidget(buttons);

    setSettings(initial);
}

void EditorSettingsDialog::setSettings(const EditorSettings& s)
{
    lineNumbersCombo_->setCurrentIndex(indexOfMode(kLineModes, s.lineNumbersMode));
    currentLineCombo_->setCurrentIndex(indexOfMode(kCurrentLineModes, s.currentLineMode));
    rulerSpin_->setValue(s.longLineRulerColumn);
    tabStopSpin_->setValue(s.tabStopWidth);
    autoIndentCheck_->setChecked(s.autoIndentEnabled);
    tabFocusCheck_->setChecked(s.tabChangesFocus);
    overviewRulerCheck_->setChecked(s.overviewRulerEnabled);
    scrollPastEndCheck_->setChecked(s.scrollBeyondLastLineEnabled);
    autoCloseCheck_->setChecked(s.autoCloseEnabled);
    readOnlyCheck_->setChecked(s.readOnly);
    indentGuidesCheck_->setChecked(s.indentGuidesEnabled); // M21
    // M24: a Custom kind seed (only manual role edits can leave one on a
    // live editor) clamps to the Default row via indexOfMode.
    themeCombo_->setCurrentIndex(indexOfMode(kThemePresets, s.themePreset));

    // M20: remember the WHOLE seed (see the seedFont_ note in the header).
    // Deliberately NO setCurrentFont: a family like fontconfig's
    // "monospace" ALIAS resolves (inside QFontComboBox's own lookup) to
    // some concrete family, silently REWRITING the seed on the way back
    // out. So the row selection works on the model's TEXT rows: exact
    // hit, else the seed's own text is added as a row and selected.
    // settings() then distinguishes "still on the seed's row" (family
    // untouched) from a user switch (family taken from currentFont()).
    seedFont_ = s.codeFont;
    const QString family = s.codeFont.family();
    int fontRow = fontCombo_->findText(family);
    if (fontRow < 0) {
        fontCombo_->addItem(family);
        fontRow = fontCombo_->count() - 1;
    }
    fontCombo_->setCurrentIndex(fontRow);
    fontPxSpin_->setValue(s.codeFont.pixelSize()); // the demo contract is
                                                   // pixel-sized fonts
    zoomSpin_->setValue(s.zoomFactor);             // clamped to the range
}

EditorSettings EditorSettingsDialog::settings() const
{
    EditorSettings s;
    s.lineNumbersMode = kLineModes[lineNumbersCombo_->currentIndex()];
    s.currentLineMode = kCurrentLineModes[currentLineCombo_->currentIndex()];
    s.longLineRulerColumn = rulerSpin_->value();
    s.tabStopWidth = tabStopSpin_->value();
    s.autoIndentEnabled = autoIndentCheck_->isChecked();
    s.tabChangesFocus = tabFocusCheck_->isChecked();
    s.overviewRulerEnabled = overviewRulerCheck_->isChecked();
    s.scrollBeyondLastLineEnabled = scrollPastEndCheck_->isChecked();
    s.autoCloseEnabled = autoCloseCheck_->isChecked();
    s.readOnly = readOnlyCheck_->isChecked();
    s.indentGuidesEnabled = indentGuidesCheck_->isChecked(); // M21
    s.themePreset = kThemePresets[themeCombo_->currentIndex()]; // M24
    // M20: the seed font patched, never rebuilt — weight/style/hinting
    // ride through, only the two edited fields move (round-trip QFont
    // equality lives or dies here). "Still on the seed family's row"
    // keeps the seed family VERBATIM (aliases like "monospace" must not
    // be rewritten); only a row switch pulls a new family, and even
    // then via currentFont(), which for real list rows carries it.
    QFont font = seedFont_;
    const int row = fontCombo_->currentIndex();
    const QString rowText = row >= 0 ? fontCombo_->itemText(row) : QString();
    if (!rowText.isEmpty() && rowText != seedFont_.family()) {
        const QString chosen = fontCombo_->currentFont().family();
        if (!chosen.isEmpty())
            font.setFamily(chosen);
    }
    font.setPixelSize(fontPxSpin_->value()); // the spin always has a value
    s.codeFont = font;
    s.zoomFactor = zoomSpin_->value();
    return s;
}

} // namespace qmn::app
