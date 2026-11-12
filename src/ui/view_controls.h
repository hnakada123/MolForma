// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QFrame>
#include <QList>
#include <QString>
class QToolButton;

namespace mf {
class MoleculeView;
// The controls of a 3D view, the same over every view: +X/+Y/+Z (that axis toward the viewer, the
// whole molecule shown), "全体を表示" (the whole molecule without turning it), "回転" (sliders for
// the angles, following the mouse) and a "表示" menu. Object names of the parts come from names.
class ViewControls : public QFrame {
  public:
    struct Names {
        QString axes[3], fit, rotate, menu, popup, sliders[3];
    };
    // rotate / menu false: without those buttons (the coordinate table's small view).
    ViewControls(MoleculeView *view, const Names &names, bool rotate = true, bool menu = true,
                 QWidget *parent = nullptr);
    QToolButton *menuButton() const {
        return display;
    }
    // Buttons and sliders, for callers that enable them together.
    QList<QWidget *> controls() const {
        return parts;
    }
    void applyStyle(const QPalette &palette);

  private:
    QToolButton *rotation = nullptr, *display = nullptr;
    QFrame *popup = nullptr;
    QList<QWidget *> parts;
};
// The mouse controls of a 3D view, the hint under every view.
QString viewControlsHint();
} // namespace mf
