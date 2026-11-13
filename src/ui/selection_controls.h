// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <QString>
#include <QVector>
class QWidget;
namespace mf {
// The controls of "1 分子" that need atoms selected: enabled by the number of selected atoms, and while
// disabled their tooltip says how many are needed.
class SelectionControls {
  public:
    // minimum to maximum atoms (maximum 0: any number from minimum). The tooltip the widget has now is
    // the one it keeps.
    void add(QWidget *widget, int minimum, int maximum = 0);
    void refresh(int selected) const;

  private:
    struct Control {
        QWidget *widget;
        int minimum, maximum;
        QString tooltip;
    };
    QVector<Control> controls;
};
// The selection for an edit that needs one, of `count` atoms if given: fails with what to select.
Ids requiredSelection(const Ids &selected, int count = 0);
} // namespace mf
