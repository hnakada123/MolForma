// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/coordinates.h"
#include "editing/atom_properties.h"
#include "model/geometry.h"
#include "model/i18n.h"
#include <QRegularExpression>
#include <cmath>

namespace mf {
static double finiteNumber(QString text) {
    // Also accept Fortran exponents commonly found in quantum chemistry output.
    text.replace(QChar('D'), QChar('e'), Qt::CaseInsensitive);
    bool ok;
    double value = text.trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(value))
        fail(tr("有限の数値を入力してください。小数点は . を使用します。"));
    return value;
}
static void setSymbol(Atom &atom, QString text) {
    text = text.trimmed();
    bool ghost = text.startsWith('@');
    if (ghost)
        text.remove(0, 1);
    if (!text.isEmpty())
        text = text.left(1).toUpper() + text.mid(1).toLower();
    int isotope = text == "D" ? 2 : text == "T" ? 3 : 0;
    if (isotope)
        text = "H";
    element(text);
    if (atom.element != text || isotope) {
        atom.isotope = isotope;
        atom.mass = 0;
    }
    atom.element = text;
    atom.ghost = ghost;
}
QString coordinateText(const Atom &atom, CoordinateColumn column, CoordinateUnit unit) {
    if (column == CoordinateColumn::Element)
        return atom.element;
    if (column >= CoordinateColumn::X && column <= CoordinateColumn::Z)
        return QString::number(atom.position[int(column) - 1] / coordinateScale(unit), 'g', 14);
    if (column == CoordinateColumn::Mass) {
        if (atom.isotope)
            return QString::number(atom.isotope);
        if (atom.mass <= 0)
            return {};
        QString text = QString::number(atom.mass, 'g', QLocale::FloatingPointShortest);
        if (!text.contains('.') && !text.contains('e'))
            text += ".0";
        return text;
    }
    if (column == CoordinateColumn::Charge)
        return QString::number(atom.charge);
    if (column == CoordinateColumn::Radical)
        return QString::number(atom.radical);
    if (column == CoordinateColumn::Name)
        return atom.name;
    return (column == CoordinateColumn::Ghost ? atom.ghost : atom.fixed) ? "1" : "0";
}
void setCoordinateCell(Document &document, const QString &id, CoordinateColumn column, const QString &text,
                       CoordinateUnit unit) {
    auto &atom = document.atom(id);
    if (column == CoordinateColumn::Element) {
        const QString previous = atom.element;
        const bool ghost = atom.ghost;
        // The separate checkbox owns ghost status; @ also explicitly enables it.
        setSymbol(atom, text);
        atom.ghost |= ghost;
        if (atom.element != previous || atom.ghost != ghost) {
            atom.stereo.clear();
            document.invalidateAssignments();
            auto residues = document.data["residues"].toArray();
            for (int i = 0; i < residues.size(); ++i) {
                auto residue = residues[i].toObject();
                for (const auto &v : residue["atoms"].toObject())
                    if (v.toString() == id)
                        residue["modified"] = true;
                residues[i] = residue;
            }
            document.data["residues"] = residues;
        }
    } else if (column >= CoordinateColumn::X && column <= CoordinateColumn::Z) {
        const int axis = int(column) - 1;
        // Do not round-trip a displayed value if the user did not change it.
        if (text.trimmed() == coordinateText(atom, column, unit))
            return;
        const double value = finiteNumber(text) * coordinateScale(unit);
        if (!std::isfinite(value))
            fail(tr("座標が大きすぎます。"));
        if (atom.fixed && value != atom.position[axis])
            fail(tr("固定原子の座標は変更できません。先に固定を解除してください。"));
        atom.position[axis] = value;
    } else {
        AtomProperties properties;
        if (column == CoordinateColumn::Mass)
            properties = atomMassInput(text);
        else if (column == CoordinateColumn::Ghost)
            properties.ghost = text == "1";
        else if (column == CoordinateColumn::Fixed)
            properties.fixed = text == "1";
        else if (column == CoordinateColumn::Charge || column == CoordinateColumn::Radical) {
            bool ok;
            const int value = text.trimmed().toInt(&ok);
            if (!ok)
                fail(column == CoordinateColumn::Charge ? tr("形式電荷は整数で入力してください。")
                                                        : tr("不対電子の数は整数で入力してください。"));
            (column == CoordinateColumn::Charge ? properties.charge : properties.radical) = value;
        } else if (column == CoordinateColumn::Name)
            properties.name = text.trimmed();
        setAtomProperties(document, {id}, properties);
    }
}
Atom coordinateAtom(const QString &symbol, const QStringList &xyz, const QString &mass, CoordinateUnit unit,
                    bool ghost, bool fixed) {
    if (xyz.size() != 3)
        fail(tr("X・Y・Zを入力してください。"));
    Atom atom;
    setSymbol(atom, symbol);
    for (int i = 0; i < 3; ++i) {
        atom.position[i] = finiteNumber(xyz[i]) * coordinateScale(unit);
        if (!std::isfinite(atom.position[i]))
            fail(tr("座標が大きすぎます。"));
    }
    if (!mass.trimmed().isEmpty())
        atom = withAtomProperties(atom, atomMassInput(mass));
    atom.ghost |= ghost;
    atom.fixed = fixed;
    return atom;
}
void replaceCoordinateStructure(Document &document, const Document &structure) {
    auto errors = structure.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    // Removal invalidates old atom references and protects fixed atoms. Keep the
    // project envelope and non-structural settings, including unknown future fields.
    document.remove(document.ids());
    document.atoms = structure.atoms;
    document.bonds = structure.bonds;
    for (auto key : {"parts", "residues", "chains", "centers"})
        document.data[key] = structure.data[key];
    document.invalidateAssignments();
}
AtomChanges atomChanges(const Document &before, const Document &after) {
    AtomChanges changes;
    QHash<QString, const Atom *> old;
    for (const auto &a : before.atoms)
        old.insert(a.id, &a);
    const bool shared = std::any_of(after.atoms.begin(), after.atoms.end(),
                                    [&](const Atom &a) { return old.contains(a.id); });
    const int common = std::min(before.atoms.size(), after.atoms.size());
    changes.byOrder = !shared && common > 0;
    for (int i = 0; changes.byOrder && i < common; ++i)
        changes.byOrder = before.atoms[i].element == after.atoms[i].element &&
                          before.atoms[i].ghost == after.atoms[i].ghost;
    if (changes.byOrder) {
        for (int i = 0; i < after.atoms.size(); ++i)
            if (i >= before.atoms.size())
                changes.added.insert(after.atoms[i].id);
            else if ((before.atoms[i].position - after.atoms[i].position).norm() > 1e-3)
                changes.moved.insert(after.atoms[i].id);
        for (int i = after.atoms.size(); i < before.atoms.size(); ++i)
            changes.removed << before.atoms[i];
        return changes;
    }
    for (const auto &a : after.atoms) {
        const auto *original = old.take(a.id);
        if (!original)
            changes.added.insert(a.id);
        else if ((original->position - a.position).norm() > 1e-4)
            changes.moved.insert(a.id);
    }
    for (const auto &a : before.atoms)
        if (old.contains(a.id))
            changes.removed << a;
    return changes;
}
std::pair<double, double> applyResultGeometry(Document &document, const Document &result) {
    if (result.atoms.size() != document.atoms.size())
        fail(tr("結果の原子数（%1）が編集中の分子（%2）と異なります。")
                 .arg(result.atoms.size())
                 .arg(document.atoms.size()));
    double squares = 0, largest = 0;
    for (int i = 0; i < document.atoms.size(); ++i) {
        const auto &from = document.atoms[i], &to = result.atoms[i];
        if (from.element != to.element || from.ghost != to.ghost)
            fail(tr("原子%1の元素（%2%3 / 結果 %4%5）が一致しません。原子の順番が同じ計算結果を選んでくだ"
                    "さい。")
                     .arg(i + 1)
                     .arg(from.ghost ? "@" : "", from.element, to.ghost ? "@" : "", to.element));
        const double moved = (to.position - from.position).norm();
        if (!std::isfinite(moved))
            fail(tr("結果の座標が有限ではありません。"));
        // NWChem prints coordinates with eight decimals; anything larger means a different frame.
        if (from.fixed && moved > 1e-3)
            fail(tr("固定原子%1の位置が結果と %2 Å 異なります。同じ座標系の計算結果か確認してください。")
                     .arg(i + 1)
                     .arg(moved, 0, 'f', 4));
        squares += moved * moved;
        largest = std::max(largest, moved);
    }
    for (int i = 0; i < document.atoms.size(); ++i)
        if (!document.atoms[i].fixed)
            document.atoms[i].position = result.atoms[i].position;
    return {document.atoms.isEmpty() ? 0 : std::sqrt(squares / document.atoms.size()), largest};
}
QVector<ZMatrixRow> zMatrix(const Document &d) {
    const int n = d.atoms.size();
    QVector<ZMatrixRow> rows(n);
    const auto position = [&](int i) { return d.atoms[i].position; };
    const auto bonded = [&](int a, int b) { return d.bond(d.atoms[a].id, d.atoms[b].id) != nullptr; };
    // Angle at b between a and c, in degrees.
    const auto angle = [&](int a, int b, int c) {
        const Vec u = (position(a) - position(b)).normalized(), v = (position(c) - position(b)).normalized();
        return std::acos(std::clamp(u.dot(v), -1., 1.)) * 180 / M_PI;
    };
    // An earlier atom, preferring one bonded to `to`, then the nearest; never in `excluded`, and
    // not collinear with `line` (angle within 2° of 0° or 180°).
    const auto reference = [&](int before, int to, QVector<int> excluded, int line) {
        int best = -1;
        double bestScore = 1e300;
        for (int k = 0; k < before; ++k) {
            if (excluded.contains(k))
                continue;
            if (line >= 0) {
                const double a = angle(line, to, k);
                if (a < 2 || a > 178)
                    continue;
            }
            const double score = (bonded(k, to) ? 0 : 1000) + (position(k) - position(to)).norm();
            if (score < bestScore) {
                bestScore = score;
                best = k;
            }
        }
        return best;
    };
    for (int i = 1; i < n; ++i) {
        auto &row = rows[i];
        row.bond = reference(i, i, {i}, -1);
        if (row.bond < 0)
            continue;
        row.distance = (position(i) - position(row.bond)).norm();
        if (i < 2)
            continue;
        row.angle = reference(i, row.bond, {i, row.bond}, i);
        if (row.angle < 0)
            continue;
        row.angleValue = angle(i, row.bond, row.angle);
        if (i < 3)
            continue;
        row.dihedral = reference(i, row.angle, {i, row.bond, row.angle}, row.bond);
        if (row.dihedral >= 0)
            row.dihedralValue = measure(
                d, {d.atoms[i].id, d.atoms[row.bond].id, d.atoms[row.angle].id, d.atoms[row.dihedral].id});
    }
    return rows;
}
QString zMatrixText(const Document &d) {
    const auto rows = zMatrix(d);
    QString text;
    for (int i = 0; i < rows.size(); ++i) {
        const auto &r = rows[i];
        text += (d.atoms[i].ghost ? "bq" : "") + d.atoms[i].element;
        if (r.bond >= 0)
            text += QString(" %1 %2").arg(r.bond + 1).arg(r.distance, 0, 'f', 6);
        if (r.angle >= 0)
            text += QString(" %1 %2").arg(r.angle + 1).arg(r.angleValue, 0, 'f', 4);
        if (r.dihedral >= 0)
            text += QString(" %1 %2").arg(r.dihedral + 1).arg(r.dihedralValue, 0, 'f', 4);
        text += '\n';
    }
    return text;
}
} // namespace mf
