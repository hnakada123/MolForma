// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/transform_page.h"
#include "editing/coordinates.h"
#include "editing/operations.h"
#include "editing/stereochemistry.h"
#include "foundation/i18n.h"
#include <QtWidgets>

namespace mf {
static QString value(QComboBox *b) {
    return b->currentData().toString();
}
static Vec fields(QDoubleSpinBox *const *spins) {
    return {spins[0]->value(), spins[1]->value(), spins[2]->value()};
}
TransformPage::TransformPage(EditorContext &editorServices, UiServices &uiServices, QWidget *parent)
    : PropertyPage(editorServices, uiServices, parent) {
    auto *f = form();
    section(f, QT_TR_NOOP("平行移動（選択原子）"));
    for (int i = 0; i < 3; ++i) {
        translateFields[i] = number(f, QString("Δ") + QString("XYZ")[i] + " / Å");
        translateFields[i]->setObjectName(QString("translate") + QString("XYZ")[i]);
    }
    enableWithSelection(button(
                            f, tr("選択を平行移動"),
                            [this] {
                                auto ids = requireSelection();
                                Vec v = fields(translateFields);
                                editorContext.edit(tr("平行移動"), [=](Document &d) {
                                    transform(d, ids, Mat::Identity(), v);
                                });
                            },
                            "translateSelection"),
                        1);
    section(f, QT_TR_NOOP("回転"));
    // Rotation axis and mirror plane pass through the selected atoms' center, or the origin.
    auto *pivot = choices(f, tr("回転・鏡映の中心"), {tr("選択の中心"), tr("原点")}, {"selection", "origin"});
    pivot->setObjectName("transformPivot");
    const auto around = [pivot](const Document &d, const Ids &ids) {
        return value(pivot) == "origin" ? Vec(Vec::Zero()) : centroid(d, ids);
    };
    auto *axis = choices(f, tr("回転軸"), {"Z", "X", "Y"}, {"2", "0", "1"});
    axis->setObjectName("rotationAxis");
    twistAngle = number(f, tr("回転角 / °"), 0, -360, 360);
    twistAngle->setObjectName("rotationAngle");
    enableWithSelection(button(
                            f, tr("選択を回転"),
                            [this, axis, around] {
                                auto ids = requireSelection();
                                int ax = value(axis).toInt();
                                double a = twistAngle->value();
                                editorContext.edit(tr("選択を回転"), [=](Document &d) {
                                    transform(d, ids, rotation(Vec::Unit(ax), a), Vec::Zero(),
                                              around(d, ids));
                                });
                            },
                            "rotateSelection"),
                        1);
    section(f, QT_TR_NOOP("立体"));
    enableWithSelection(button(f, tr("不斉中心を反転"),
                               [this] {
                                   auto id = requireSelection(1).first();
                                   editorContext.edit(tr("不斉中心を反転"),
                                                      [=](Document &d) { invertCenter(d, id); });
                               }),
                        1, 1);
    enableWithSelection(button(f, tr("E/Zを切替"),
                               [this] {
                                   auto ids = requireSelection(2);
                                   editorContext.edit(tr("E/Zを切替"),
                                                      [=](Document &d) { switchEZ(d, ids); });
                               }),
                        2, 2);
    auto *stereo = button(f, tr("立体を記録（R/S・E/Z）"), [this] {
        editorContext.edit(tr("立体配置を記録"), [](Document &d) { recordStereo(d); });
        // The 3D view labels them too: "C1 (S)", "(E)" on the double bond.
        QStringList found;
        for (int i = 0; i < editorContext.document().atoms.size(); ++i)
            if (!editorContext.document().atoms[i].stereo.isEmpty())
                found << QString("%1%2: %3")
                             .arg(editorContext.document().atoms[i].element)
                             .arg(i + 1)
                             .arg(editorContext.document().atoms[i].stereo);
        for (const auto &b : editorContext.document().bonds)
            if (!b.stereo.isEmpty())
                found << QString("%1%2=%3%4: %5")
                             .arg(editorContext.document().atom(b.a).element)
                             .arg(editorContext.document().index(b.a) + 1)
                             .arg(editorContext.document().atom(b.b).element)
                             .arg(editorContext.document().index(b.b) + 1)
                             .arg(b.stereo);
        uiContext.showMessage(found.isEmpty() ? tr("記録する不斉中心・E/Zの二重結合はありません。")
                                              : tr("立体配置を記録しました: %1").arg(found.join(", ")),
                              10000);
    });
    stereo->setEnabled(hasStereochemistryBackend());
    if (!hasStereochemistryBackend())
        stereo->setToolTip(tr("RDKitを含むビルドで使えます。"));
    section(f, QT_TR_NOOP("向き・位置"));
    auto *orientation = button(
        f, tr("向きをそろえる（原点・+X・XY）"),
        [this] {
            auto ids = requireSelection();
            editorContext.edit(tr("向きをそろえる"), [=](Document &d) { orient(d, ids); });
            emit moved();
        },
        "orientSelection");
    orientation->setToolTip(
        tr("選択順に、1番目を原点、2番目を+X軸上、3番目をXY平面上へ動かします（分子全体）。"
           "1原子ならその原子が原点に来るよう平行移動します。"));
    enableWithSelection(orientation, 1, 3);
    button(f, tr("重心を原点へ"), [this] {
        auto ids = editorContext.selection();
        editorContext.edit(tr("重心を原点へ"), [=](Document &d) { centerMass(d, ids, false); });
        emit moved();
    });
    button(f, tr("主慣性軸へ整列"), [this] {
        auto ids = editorContext.selection();
        editorContext.edit(tr("主慣性軸へ整列"), [=](Document &d) { centerMass(d, ids, true); });
        emit moved();
    });
    section(f, QT_TR_NOOP("鏡映・複製"));
    auto *plane = choices(f, tr("鏡映面"), {"YZ", "XZ", "XY"}, {"0", "1", "2"});
    plane->setObjectName("mirrorPlane");
    auto *copy = new QCheckBox(tr("鏡像の写しを作る"));
    copy->setObjectName("mirrorCopy");
    f->addRow(copy);
    auto *mirrorButton = button(
        f, tr("指定平面で鏡映"),
        [this, plane, copy, pivot, around] {
            auto ids = requireSelection();
            int ax = value(plane).toInt();
            bool makeCopy = copy->isChecked();
            const bool origin = value(pivot) == "origin";
            editorContext.edit(tr("鏡映"), [=](Document &d) {
                if (makeCopy)
                    mirrorCopy(d, ids, ax, origin);
                else
                    mirror(d, ids, ax, around(d, ids));
            });
            emit moved();
        },
        "mirrorSelection");
    enableWithSelection(mirrorButton, 1);
    for (int i = 0; i < 3; ++i) {
        duplicateFields[i] = number(f, tr("複製 %1 / Å").arg(QString("XYZ")[i]), i == 0 ? 3 : 0);
        duplicateFields[i]->setObjectName(QString("duplicate") + QString("XYZ")[i]);
    }
    auto *duplicateButton = button(
        f, tr("指定間隔で複製"),
        [this] {
            auto ids = requireSelection();
            Vec v = fields(duplicateFields);
            editorContext.edit(tr("複製"), [=](Document &d) { duplicate(d, ids, v); });
            emit moved();
        },
        "duplicateSelection");
    enableWithSelection(duplicateButton, 1);
}
double TransformPage::twist() const {
    return twistAngle->value();
}
} // namespace mf
