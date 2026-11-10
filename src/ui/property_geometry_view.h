// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/result_loader.h"
#include <QWidget>
class QLabel;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
namespace mf {
class MainWindow;
class MoleculeView;
class FieldSlice;
class PropertyGeometryView : public QWidget {
    Q_OBJECT
  public:
    explicit PropertyGeometryView(MainWindow *window);
    void setResult(const ResultData &data);
    void openColorField(const QString &path);

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    bool fitPending = true;
    MainWindow *window;
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
