// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/atom_properties.h"
#include "model/i18n.h"
#include "model/numbers.h"
#include <QRegularExpression>
#include <cmath>
#include <openbabel/elements.h>

namespace mf {
AtomProperties atomMassInput(const QString &text) {
    AtomProperties properties;
    auto entry = text.trimmed();
    if (entry.isEmpty()) {
        properties.isotope = 0;
    } else if (QRegularExpression("^[+-]?[0-9]+$").match(entry).hasMatch()) {
        bool ok;
        const int isotope = entry.toInt(&ok);
        if (!ok || isotope <= 0)
            fail(tr("質量数は正の整数にしてください。"));
        properties.isotope = isotope;
    } else {
        const auto mass = fortranNumber(entry);
        if (!mass || *mass <= 0)
            fail(tr("質量は正の値（amu）を入力してください。"));
        properties.mass = *mass;
    }
    return properties;
}
Atom withAtomProperties(Atom atom, const AtomProperties &p) {
    if (p.charge) {
        if (*p.charge < MinimumAtomCharge || *p.charge > MaximumAtomCharge)
            fail(tr("形式電荷は−99〜20で入力してください。"));
        atom.charge = *p.charge;
    }
    if (p.radical) {
        if (*p.radical < 0 || *p.radical > MaximumAtomRadical)
            fail(tr("不対電子の数は0〜8で入力してください。"));
        atom.radical = *p.radical;
    }
    if (p.isotope && p.mass)
        fail(tr("同位体の質量数と任意の質量は同時に指定できません。"));
    if (p.isotope) {
        if (*p.isotope < 0 || (*p.isotope && (*p.isotope < element(atom.element).number ||
                                              OpenBabel::OBElements::GetExactMass(
                                                  element(atom.element).number, *p.isotope) <= 0)))
            fail(tr("この元素の同位体質量数を確認してください。任意の質量は小数で入力します。"));
        atom.isotope = *p.isotope;
        atom.mass = 0;
    }
    if (p.mass) {
        if (!std::isfinite(*p.mass) || *p.mass <= 0)
            fail(tr("質量は正の値（amu）を入力してください。"));
        atom.mass = *p.mass;
        atom.isotope = 0;
    }
    if (p.ghost)
        atom.ghost = *p.ghost;
    if (p.fixed)
        atom.fixed = *p.fixed;
    if (p.name) {
        static const QRegularExpression control("[\\x00-\\x1f\\x7f]");
        if (p.name->size() > 16 || p.name->contains(control))
            fail(tr("原子名は制御文字を含まない16文字以内で指定してください。"));
        atom.name = *p.name;
    }
    return atom;
}
void setAtomProperties(Document &document, const Ids &ids, const AtomProperties &p) {
    const auto atoms = document.indices(ids);
    QVector<Atom> changed;
    for (int i : atoms)
        changed << withAtomProperties(document.atoms[i], p);
    for (int k = 0; k < atoms.size(); ++k)
        document.atoms[atoms[k]] = changed[k];
}
} // namespace mf
