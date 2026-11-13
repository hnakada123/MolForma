// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <QWidget>
#include <functional>
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QTableWidget;
namespace mf {
class ChartView;
class UiServices;
// "励起状態" of the result viewer: the excited states of a result, the orbital pairs of the chosen
// state and the absorption spectrum (UV/visible) broadened for display. The view owns the axis and the
// width of the spectrum; results/spectrum.h owns its peaks.
class ResultExcitationsView : public QWidget {
    Q_OBJECT
  public:
    // jobMolden: the MOLDEN file of the job the shown log belongs to (empty if none), which the note
    // over the orbital pairs names; asked whenever the note is written.
    ResultExcitationsView(UiServices &uiContext, std::function<QString()> jobMolden,
                          QWidget *parent = nullptr);
    void setResult(const ResultData &result);
    // Chooses a state by its row (out of range: nothing).
    void selectState(int index);
  signals:
    // An orbital of a pair listed for the chosen state was double-clicked: its spin ("Alpha", "Beta")
    // and NWChem's number.
    void orbitalRequested(const QString &spin, int number);

  private:
    std::function<QString()> jobMolden;
    ResultData data;
    QTableWidget *excitations;
    // The orbital pairs of the chosen excited state.
    QTableWidget *contributions;
    QWidget *contributionsBlock;
    QLabel *contributionsTitle;
    ChartView *spectrum;
    QComboBox *spectrumAxis;
    QDoubleSpinBox *broadening;
    void showSpectrum();
    void showContributions();
    void openTransitionOrbital(int row, int column);
};
} // namespace mf
