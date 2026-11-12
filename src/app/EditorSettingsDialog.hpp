#pragma once

// qmn-demo settings dialog: edits one EditorSettings template. The main
// window seeds it (from its template, or from the focused editor when one
// is open), and on ACCEPT the chosen settings go to every open editor —
// and become the template for future tabs. The dialog itself only maps
// controls to the struct: setSettings()/settings() are the two doors, so
// the mapping is testable without ever exec()ing the dialog.

#include <QDialog>

#include "EditorSettings.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QSpinBox;

namespace qmn::app {

class EditorSettingsDialog : public QDialog {
    Q_OBJECT
public:
    EditorSettingsDialog(QWidget* parent, const EditorSettings& initial);

    void setSettings(const EditorSettings& s); // push values into controls
    EditorSettings settings() const;          // pull the control state out

private:
    QComboBox* lineNumbersCombo_;
    QComboBox* currentLineCombo_;
    QSpinBox* rulerSpin_;
    QSpinBox* tabStopSpin_;
    QCheckBox* autoIndentCheck_;
    QCheckBox* tabFocusCheck_;
    QCheckBox* overviewRulerCheck_;
    QCheckBox* scrollPastEndCheck_;
    QCheckBox* autoCloseCheck_;
    QCheckBox* readOnlyCheck_;
    QCheckBox* indentGuidesCheck_;
    // M20 font/zoom trio. The dialog edits only family + pixel size +
    // factor; seedFont_ (pushed by setSettings) carries the font's REST
    // (weight, style, the no-hinting strategy the editor's metrics rule
    // depends on) through settings(), so a seed->controls->collect round
    // trip yields a QFont EQUAL to the seed, not merely similar. (A
    // QFontComboBox currentFont() is not usable wholesale: it carries
    // its own size and style.) The template's fonts are pixel-sized by
    // the demo contract (the library default recipe is too).
    QFontComboBox* fontCombo_;
    QSpinBox* fontPxSpin_;
    QDoubleSpinBox* zoomSpin_;
    QFont seedFont_;
};

} // namespace qmn::app
