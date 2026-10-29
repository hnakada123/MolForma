#pragma once
#include "editing/coordinates.h"
#include <QFrame>
class QLabel;
namespace mf {
// The same candidate controls above the molecule and below the other workspace tabs.
class CandidateControls : public QFrame {
    Q_OBJECT
  public:
    explicit CandidateControls(bool banner, QWidget *parent = nullptr);
    void refresh(const QString &name, const AtomChanges &changes);
    void applyChromeStyle();
  signals:
    void confirmRequested();
    void cancelRequested();

  private:
    QLabel *title = nullptr, *text = nullptr, *icon = nullptr;
};
} // namespace mf
