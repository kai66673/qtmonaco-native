#include "EditorSettingsDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
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

    rulerSpin_ = new QSpinBox(this);
    rulerSpin_->setRange(0, 999);
    rulerSpin_->setSpecialValueText(QStringLiteral("нет"));

    tabStopSpin_ = new QSpinBox(this);
    tabStopSpin_->setRange(1, 32); // the editor clamps >= 1; 32 is demo sanity

    autoIndentCheck_ = new QCheckBox(QStringLiteral("Автоотступ при Enter"), this);
    tabFocusCheck_ = new QCheckBox(QStringLiteral("Tab переводит фокус (не отступ)"), this);
    overviewRulerCheck_ = new QCheckBox(QStringLiteral("Линейка обзора справа"), this);
    scrollPastEndCheck_ = new QCheckBox(QStringLiteral("Прокрутка за последней строкой"), this);
    autoCloseCheck_ = new QCheckBox(QStringLiteral("Автозакрытие скобок и кавычек"), this);
    readOnlyCheck_ = new QCheckBox(QStringLiteral("Только чтение"), this);

    auto* appearance = new QGroupBox(QStringLiteral("Внешний вид"), this);
    auto* appearanceForm = new QFormLayout(appearance);
    appearanceForm->addRow(QStringLiteral("Номера строк:"), lineNumbersCombo_);
    appearanceForm->addRow(QStringLiteral("Подсветка текущей строки:"), currentLineCombo_);
    appearanceForm->addRow(QStringLiteral("Линия-ограничитель (столбец):"), rulerSpin_);

    auto* text = new QGroupBox(QStringLiteral("Текст и отступы"), this);
    auto* textForm = new QFormLayout(text);
    textForm->addRow(QStringLiteral("Ширина табуляции:"), tabStopSpin_);
    textForm->addRow(autoIndentCheck_);
    textForm->addRow(autoCloseCheck_);

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
    return s;
}

} // namespace qmn::app
