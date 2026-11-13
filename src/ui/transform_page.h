// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ui/property_page.h"
namespace mf {
// "変形" of the properties of "1 分子": the selected atoms moved, turned, mirrored and copied, the
// stereo centers inverted or recorded, and the molecule laid along its axes. Every button is an edit
// of the document through the editor context.
class TransformPage : public PropertyPage {
    Q_OBJECT
  public:
    TransformPage(EditorContext &editorContext, UiServices &uiContext, QWidget *parent = nullptr);
    // "回転角 / °": also the angle a part is turned by about its bond as it is put on an atom or a site.
    double twist() const;
  signals:
    // An edit of the page moved or added atoms: the 3D view shall keep the molecule in sight.
    void moved();

  private:
    QDoubleSpinBox *translateFields[3]{}, *duplicateFields[3]{};
    QDoubleSpinBox *twistAngle = nullptr;
};
} // namespace mf
