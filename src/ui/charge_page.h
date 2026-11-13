// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ui/property_page.h"
class QLabel;
class QListWidget;
namespace mf {
// "電荷" of the properties of "1 分子": the charge and multiplicity of the molecule as the calculation
// has them (settings of "2 計算条件", shown here with the way there) against the formal charges of the
// atoms, and the external point charges, which are added, changed and removed here.
class ChargePage : public PropertyPage {
  public:
    ChargePage(EditorContext &editorContext, UiServices &uiContext, WorkspaceActions &workspace,
               QWidget *parent = nullptr);
    // Shows the document of the editor context: its point charges and what its charges come to.
    void refresh();
    // "電荷 / e": the charge of a point charge added here or put down with the point-charge tool, whose
    // option bar shows the same value.
    QDoubleSpinBox *valueField() const {
        return chargeValue;
    }

  private:
    WorkspaceActions &workspace;
    QListWidget *pointList;
    QLabel *pointEmpty; // What to do while the list is empty.
    // The calculation's charge and point-charge settings ("2 計算条件"), shown.
    QLabel *chargeSummary, *chargeState, *pointChargeState, *inputState;
    QDoubleSpinBox *chargeFields[3]{}, *chargeValue = nullptr;
};
} // namespace mf
