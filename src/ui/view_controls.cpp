// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/view_controls.h"
#include "foundation/i18n.h"
#include "ui/chrome_layouts.h"
#include "ui/line_icons.h"
#include "ui/molecule_view.h"
#include "ui/theme.h"
#include <QHBoxLayout>
#include <QSlider>
#include <QToolButton>

namespace mf {
QString viewControlsHint() {
    return tr("左ドラッグ: 視点を回転 · 右ドラッグ: 視点を平行移動 · ホイール: 拡大縮小");
}

ViewControls::ViewControls(MoleculeView *view, const Names &names, bool rotate, bool menu, QWidget *parent)
    : QFrame(parent) {
    setProperty("viewControls", true);
    setProperty("compactButtons", true); // OverlayHost shows only the icons when narrow.
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(3, 3, 3, 3);
    row->setSpacing(1);
    for (int i = 0; i < 3; ++i) {
        auto *b = new QToolButton;
        b->setObjectName(names.axes[i]);
        b->setText(QString("+") + QString("XYZ")[i]);
        b->setToolTip(tr("%1軸の正の側から見て、分子全体を表示します。").arg(QString("XYZ")[i]));
        row->addWidget(b);
        parts << b;
        connect(b, &QToolButton::clicked, this, [view, i] { view->viewFrom(i); });
    }
    auto *divider = new QFrame;
    divider->setFrameShape(QFrame::VLine);
    divider->setFrameShadow(QFrame::Sunken);
    row->addWidget(divider);
    const auto button = [&](const QString &text, const QString &icon, const QString &name) {
        auto *b = new QToolButton;
        b->setObjectName(name);
        b->setText(text);
        b->setIcon(lineIcon(icon));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        row->addWidget(b);
        parts << b;
        return b;
    };
    auto *fit = button(tr("全体を表示"), "fit", names.fit);
    fit->setToolTip(tr("向きを変えずに、分子全体が収まるように表示します。"));
    connect(fit, &QToolButton::clicked, view, &MoleculeView::fit);
    if (rotate) {
        rotation = button(tr("回転"), "rotate", names.rotate);
        rotation->setToolTip(tr("左右・上下・画面内の回転角をスライダーで指定します。"));
        // The angles as sliders in a popup under "回転"; the mouse rotation moves them too.
        popup = new QFrame(this, Qt::Popup);
        popup->setObjectName(names.popup);
        auto *angles = new FormLayout(popup);
        angles->setContentsMargins(14, 12, 14, 12);
        QSlider *sliders[3];
        const auto start = view->angles();
        for (int i = 0; i < 3; ++i) {
            sliders[i] = new QSlider(Qt::Horizontal);
            sliders[i]->setObjectName(names.sliders[i]);
            sliders[i]->setRange(-180, 180);
            sliders[i]->setValue(qRound(start[i]));
            sliders[i]->setMinimumWidth(200);
            angles->addRow(QStringList{tr("左右回転"), tr("上下回転"), tr("画面内回転")}[i], sliders[i]);
            parts << sliders[i];
        }
        connect(rotation, &QToolButton::clicked, this, [this] {
            popup->adjustSize();
            popup->move(
                rotation->mapToGlobal(QPoint(rotation->width() - popup->width(), rotation->height() + 4)));
            popup->show();
        });
        for (int i = 0; i < 3; ++i)
            connect(sliders[i], &QSlider::valueChanged, this,
                    [view, a = sliders[0], b = sliders[1], c = sliders[2]] {
                        view->setAngles(a->value(), b->value(), c->value());
                    });
        // Mouse rotation and +X/+Y/+Z move the sliders, so touching one continues from the view.
        connect(view, &MoleculeView::orientationChanged, this,
                [view, a = sliders[0], b = sliders[1], c = sliders[2]] {
                    const auto angles = view->angles();
                    const QSignalBlocker ba(a), bb(b), bc(c);
                    a->setValue(qRound(angles.x()));
                    b->setValue(qRound(angles.y()));
                    c->setValue(qRound(angles.z()));
                });
    }
    if (menu) {
        display = button(tr("表示"), "eye", names.menu);
        display->setToolTip(tr("ラベル・水素・表示スタイルなどの表示を切り替えます。"));
        display->setPopupMode(QToolButton::InstantPopup);
    }
}
void ViewControls::applyStyle(const QPalette &palette) {
    setStyleSheet(chromeRules(R"(
        QFrame[viewControls="true"] { background: @base; border: 1px solid @line; border-radius: 8px; }
        QToolButton { border: none; border-radius: 5px; padding: 3px 7px; color: @text; background: transparent; }
        QToolButton:hover { background: @hover; }
        QToolButton:disabled { color: @muted; }
        QToolButton::menu-indicator { width: 0; }
    )",
                              palette));
    if (popup)
        popup->setStyleSheet(chromeRules("QFrame { background: @base; border: 1px solid @line; }", palette));
}
} // namespace mf
