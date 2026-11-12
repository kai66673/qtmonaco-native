#include "view/LanguageProperties.hpp"

#include <algorithm>

namespace qmn::view {

LanguageProperties::LanguageProperties(QObject* parent)
    : QObject(parent)
{
}

void LanguageProperties::setTabStopWidth(int columns)
{
    columns = std::max(1, columns);
    if (columns == tabStopWidth_)
        return;
    tabStopWidth_ = columns;
    emit tabStopWidthChanged(tabStopWidth_);
}

void LanguageProperties::clearTabStopWidth()
{
    if (tabStopWidth_ < 0)
        return; // already un-set: silent, same-value discipline
    tabStopWidth_ = -1;
    emit tabStopWidthChanged(-1); // -1: the override went OFF
}

} // namespace qmn::view
