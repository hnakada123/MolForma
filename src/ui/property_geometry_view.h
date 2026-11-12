// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/result_loader.h"
#include "ui/editor_context.h"
#include <QWidget>
class QLabel;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
namespace mf {
class MoleculeView;
class FieldSlice;
class PropertyGeometryView : public QWidget {
    Q_OBJECT
  public:
    PropertyGeometryView(UiServices &uiContext, QWidget *parent = nullptr);
    void setResult(const ResultData &data);
    void openColorField(const QString &path);

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    bool fitPending = true;
    UiServices &uiContext;
    ResultData data, field;
    ResultLoader loader;
    MoleculeView *view;
    FieldSlice *slice;
    QWidget *fieldControls, *tensorControls;
    QLabel *message, *source;
    QComboBox *axis, *units;
    QSpinBox *plane;
    QDoubleSpinBox *low, *high, *dipoleScale, *tensorScale;
    QCheckBox *dipole, *tensor;
    void refresh();
};
} // namespace mf
