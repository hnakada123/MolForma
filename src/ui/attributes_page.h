// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ui/property_page.h"
class QLineEdit;
class QSpinBox;
namespace mf {
// "属性" of the properties of "1 分子": the formal charge, unpaired electrons and isotope or mass of the
// selected atoms, and whether they are ghosts or fixed. Every button is an edit of the document
// through the editor context.
class AttributesPage : public PropertyPage {
  public:
    AttributesPage(EditorContext &editorContext, UiServices &uiContext, QWidget *parent = nullptr);
    // The fields with the attributes of the one atom selected; blank ("変更なし") for none or several,
    // never left from an earlier atom.
    void showSelection();
    // Fixes atoms or frees them: the edit of the buttons here, and of the menu of the 3D view.
    void fixAtoms(const Ids &ids, bool fixed);

  private:
    QSpinBox *formal, *radical;
    QLineEdit *mass;
};
} // namespace mf
