// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/attributes_page.h"
#include "editing/atom_properties.h"
#include "foundation/i18n.h"
#include <QtWidgets>

namespace mf {
AttributesPage::AttributesPage(EditorContext &editorServices, UiServices &uiServices, QWidget *parent)
    : PropertyPage(editorServices, uiServices, parent) {
    auto *f = form();
    section(f, QT_TR_NOOP("原子の属性"));
    formal = new QSpinBox;
    formal->setObjectName("atomFormalCharge");
    formal->setRange(MinimumAtomCharge - 1, MaximumAtomCharge);
    formal->setSpecialValueText(tr("変更なし"));
    f->addRow(tr("形式電荷"), formal);
    radical = new QSpinBox;
    radical->setObjectName("atomRadical");
    radical->setRange(-1, MaximumAtomRadical);
    radical->setSpecialValueText(tr("変更なし"));
    f->addRow(tr("不対電子"), radical);
    formal->setToolTip(tr("水素補完などの構造準備に使用します。計算の全電荷は「2 計算条件」で設定します。"));
    radical->setToolTip(tr("原子のラジカル属性です。計算のスピン多重度は「2 計算条件」で設定します。"));
    mass = new QLineEdit;
    mass->setObjectName("atomMass");
    mass->setPlaceholderText(tr("整数: 質量数 / 小数: amu"));
    f->addRow(tr("同位体 / 質量"), mass);
    for (QWidget *field : std::initializer_list<QWidget *>{formal, radical, mass})
        enableWithSelection(field, 1);
    auto *attributes = button(f, tr("属性を適用"), [this] {
        auto ids = requireSelection();
        int q = formal->value(), r = radical->value();
        QString m = mass->text().trimmed();
        AtomProperties properties;
        if (q != MinimumAtomCharge - 1)
            properties.charge = q;
        if (r != -1)
            properties.radical = r;
        if (!m.isEmpty() || ids.size() == 1) {
            const auto parsed = atomMassInput(m);
            properties.isotope = parsed.isotope;
            properties.mass = parsed.mass;
        }
        editorContext.edit(tr("原子属性を変更"), [=](Document &d) { setAtomProperties(d, ids, properties); });
    });
    attributes->setObjectName("applyAtomAttributes");
    enableWithSelection(attributes, 1);
    section(f, QT_TR_NOOP("ゴースト・固定"));
    // The names of the coordinate table and the 3D view's menu, for the same edits.
    for (bool ghost : {true, false}) {
        auto *b = button(
            f, ghost ? tr("ゴーストにする") : tr("ゴーストを解除"),
            [this, ghost] {
                auto ids = requireSelection();
                editorContext.edit(ghost ? tr("選択原子をゴーストにする") : tr("選択原子のゴーストを解除"),
                                   [=](Document &d) {
                                       AtomProperties properties;
                                       properties.ghost = ghost;
                                       setAtomProperties(d, ids, properties);
                                   });
            },
            ghost ? "ghostSelectedAtoms" : "unghostSelectedAtoms");
        b->setToolTip(tr("基底関数だけを置く原子（NWChemのbq）です。BSSE補正などで使います。"));
        enableWithSelection(b, 1);
    }
    for (bool fixed : {true, false}) {
        auto *b = button(
            f, fixed ? tr("選択原子を固定") : tr("固定を解除"),
            [this, fixed] { fixAtoms(requireSelection(), fixed); },
            fixed ? "fixSelectedAtoms" : "unfixSelectedAtoms");
        b->setToolTip(tr("編集時の移動・削除を防ぎ、NWChemの構造最適化でも座標を固定します。"));
        enableWithSelection(b, 1);
    }
}
void AttributesPage::fixAtoms(const Ids &ids, bool fixed) {
    AtomProperties properties;
    properties.fixed = fixed;
    editorContext.edit(fixed ? tr("選択原子を固定") : tr("選択原子の固定を解除"),
                       [=](Document &d) { setAtomProperties(d, ids, properties); });
}
void AttributesPage::showSelection() {
    const auto &d = editorContext.displayDocument();
    const auto &selected = editorContext.selection();
    if (selected.size() == 1 && d.contains(selected.first())) {
        const auto &a = d.atom(selected.first());
        formal->setValue(a.charge);
        radical->setValue(a.radical);
        mass->setText(a.isotope    ? QString::number(a.isotope)
                      : a.mass > 0 ? QString::number(a.mass, 'g', 12)
                                   : "");
        mass->setPlaceholderText(tr("整数: 質量数 / 小数: amu"));
    } else {
        formal->setValue(MinimumAtomCharge - 1);
        radical->setValue(-1);
        mass->clear();
        mass->setPlaceholderText(selected.isEmpty() ? tr("原子を選択してください")
                                                    : tr("混在（空欄は変更なし）"));
    }
}
} // namespace mf
