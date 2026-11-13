// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/selection_controls.h"
#include "foundation/error.h"
#include "foundation/i18n.h"
#include <QWidget>

namespace mf {
void SelectionControls::add(QWidget *widget, int minimum, int maximum) {
    controls.append({widget, minimum, maximum, widget->toolTip()});
}
void SelectionControls::refresh(int count) const {
    for (const auto &c : std::as_const(controls)) {
        const bool ready = count >= c.minimum && (!c.maximum || count <= c.maximum);
        c.widget->setEnabled(ready);
        const QString need =
            c.minimum == c.maximum ? (c.minimum == 1 ? tr("原子を1つ選択すると使えます。")
                                                     : tr("%1原子を順に選択すると使えます。").arg(c.minimum))
            : c.maximum            ? tr("%1〜%2原子を順に選択すると使えます。").arg(c.minimum).arg(c.maximum)
                                   : tr("原子を選択すると使えます。");
        c.widget->setToolTip(ready ? c.tooltip : c.tooltip.isEmpty() ? need : c.tooltip + "\n" + need);
    }
}
Ids requiredSelection(const Ids &selected, int count) {
    if (selected.isEmpty() || (count && selected.size() != count))
        fail(count ? tr("%n原子を選択してください。", nullptr, count) : tr("原子を選択してください。"));
    return selected;
}
} // namespace mf
