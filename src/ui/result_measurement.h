// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <QFrame>
class QLabel;
class QPushButton;
namespace mf {
class MoleculeView;
class UiServices;
// The measurement of the result viewer, a card over its 3D view: the atoms clicked in order (2–4: a
// distance, an angle or a dihedral) and the measurements kept on the view. Both are held by atom
// number, so that they follow frames and vibration snapshots (new atom IDs); every structure the
// viewer shows goes into the view through setDocument.
class ResultMeasurement : public QFrame {
    Q_OBJECT
  public:
    ResultMeasurement(MoleculeView *view, UiServices &uiContext, QWidget *parent = nullptr);
    // Shows a structure in the view, with the picked atoms selected and the kept measurements on it.
    void setDocument(const Document &document);
    // The atoms picked (from 0), as if clicked in this order; shown with the next structure.
    void setPicked(const QVector<int> &atoms);
    // Forgets the picked atoms and the kept measurements (another result); shown with the next structure.
    void clear();
    // "画面に表示" and "消去": of use only while a structure is shown.
    QList<QWidget *> controls() const;
  signals:
    // An atom of the view was clicked, by number (from 0), before it is picked or put back.
    void atomClicked(int atom);

  private:
    MoleculeView *view;
    UiServices &uiContext;
    QLabel *measurementLabel;
    QPushButton *pin, *clearMeasure;
    QVector<int> picked;
    QVector<QVector<int>> pinned;
    void pickAtom(const QString &id);
};
} // namespace mf
