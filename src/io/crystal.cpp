// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/crystal.h"
#include "editing/chemistry.h"
#include "foundation/i18n.h"
#include "io/mmcif.h"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <queue>

namespace mf {
namespace {
constexpr int maxCellAtoms = 6000;
// A number of a CIF, without its standard uncertainty: "10.234(3)" reads 10.234.
std::optional<double> cifNumber(const QString &text) {
    const auto plain = text.section('(', 0, 0).trimmed();
    bool ok = false;
    const double value = plain.toDouble(&ok);
    if (!ok || !std::isfinite(value))
        return std::nullopt;
    return value;
}
double required(const CifBlock &block, const QByteArray &tag) {
    const auto value = cifNumber(block.items.value(tag));
    if (!value)
        fail(tr("CIFに %1 がありません。").arg(QString::fromLatin1(tag)));
    return *value;
}
// The element of a type symbol ("O2-", "Fe3+", "D") or of a label ("C12", "Cl1", "H4A").
QString elementOf(const QString &type, const QString &label) {
    const auto letters = [](const QString &text) {
        QString out;
        for (const auto ch : text) {
            if (!ch.isLetter())
                break;
            out += ch;
        }
        return out;
    };
    auto word = letters(type);
    if (word.isEmpty()) {
        word = letters(label);
        // A label starts with its element: a second letter in lower case belongs to it (Cl1), one in upper
        // case does not (CA1 is a carbon).
        word = word.size() > 1 && word[1].isLower() ? word.left(2) : word.left(1);
    }
    word = word.left(2);
    if (word.size() == 2)
        word = word[0].toUpper() + word.mid(1).toLower();
    else
        word = word.toUpper();
    if (word == "D" || word == "T")
        return "H";
    for (const auto &e : elements())
        if (e.symbol == word)
            return word;
    if (word.size() == 2) // "Ca" from "CA1" would be calcium: the first letter alone is the element.
        for (const auto &e : elements())
            if (e.symbol == word.left(1))
                return e.symbol;
    return {};
}
// An atom of the unit cell: an image of an atom of the asymmetric unit under an operation.
struct CellAtom {
    int source = 0, operation = 0;
    Vec fraction = Vec::Zero();
};
struct Bond {
    int other;
    Eigen::Vector3i shift; // The other atom is at its fraction + shift.
};
// The unit cell's atoms with their bonds, and its molecules: connected atoms with the image of each that
// makes the molecule whole.
struct Cell {
    QVector<CellAtom> atoms;
    QVector<QVector<Bond>> bonds;
    QVector<QVector<int>> molecules;
    QVector<Eigen::Vector3i> images;
    bool polymeric = false;
    int identity = 0;
};
bool kept(const CrystalAtom &a, const CrystalSelection &selection) {
    return selection.minor || std::abs(a.group) <= 1;
}
Cell buildCell(const Crystal &crystal, const CrystalSelection &selection) {
    const Mat m = crystal.matrix();
    QVector<std::pair<Mat, Vec>> operations;
    for (const auto &text : crystal.operations)
        operations << crystalOperation(text);
    if (operations.isEmpty())
        operations << std::pair{Mat::Identity(), Vec::Zero()};
    Cell cell;
    cell.identity = -1;
    for (int k = 0; k < operations.size(); ++k) {
        Vec t = operations[k].second;
        for (int i = 0; i < 3; ++i)
            t[i] -= std::round(t[i]);
        if ((operations[k].first - Mat::Identity()).cwiseAbs().maxCoeff() < 1e-6 && t.norm() < 1e-6) {
            cell.identity = k;
            break;
        }
    }
    if (cell.identity < 0)
        fail(tr("CIFの対称操作に恒等操作（x,y,z）がありません。"));
    // The shortest Cartesian vector of a difference of fractions, over the neighboring cells.
    const auto nearest = [&](const Vec &difference, Eigen::Vector3i &shift) {
        Vec d = difference;
        Eigen::Vector3i base = Eigen::Vector3i::Zero();
        for (int i = 0; i < 3; ++i) {
            base[i] = int(-std::round(d[i]));
            d[i] += base[i];
        }
        double best = 1e30;
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z) {
                    const double length = (m * (d + Vec(x, y, z))).norm();
                    if (length < best) {
                        best = length;
                        shift = base + Eigen::Vector3i(x, y, z);
                    }
                }
        return best;
    };
    for (int i = 0; i < crystal.atoms.size(); ++i) {
        if (!kept(crystal.atoms[i], selection))
            continue;
        for (int k = 0; k < operations.size(); ++k) {
            Vec f = operations[k].first * crystal.atoms[i].fraction + operations[k].second;
            for (int axis = 0; axis < 3; ++axis)
                f[axis] -= std::floor(f[axis]);
            // An atom on a special position maps onto itself.
            bool same = false;
            Eigen::Vector3i shift = Eigen::Vector3i::Zero();
            for (const auto &other : std::as_const(cell.atoms))
                if (other.source == i && nearest(f - other.fraction, shift) < .3) {
                    same = true;
                    break;
                }
            if (same)
                continue;
            cell.atoms << CellAtom{i, k, f};
            if (cell.atoms.size() > maxCellAtoms)
                fail(tr("単位格子の原子が多すぎます（%1個まで）。").arg(maxCellAtoms));
        }
    }
    if (cell.atoms.isEmpty())
        fail(tr("CIFに読み込める原子がありません。"));
    // Bonds by distance, as for an XYZ file: the sum of the covalent radii and 0.45 Å.
    const int n = int(cell.atoms.size());
    cell.bonds.resize(n);
    QVector<double> radii;
    for (const auto &a : std::as_const(cell.atoms))
        radii << element(crystal.atoms[a.source].element).radius;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const bool hydrogens = crystal.atoms[cell.atoms[i].source].element == "H" &&
                                   crystal.atoms[cell.atoms[j].source].element == "H";
            const double limit = hydrogens ? .8 : radii[i] + radii[j] + .45;
            Eigen::Vector3i shift = Eigen::Vector3i::Zero();
            if (nearest(cell.atoms[j].fraction - cell.atoms[i].fraction, shift) > limit)
                continue;
            // Every image of j close to i: a small cell may hold more than one.
            for (int x = -1; x <= 1; ++x)
                for (int y = -1; y <= 1; ++y)
                    for (int z = -1; z <= 1; ++z) {
                        const Eigen::Vector3i s = shift + Eigen::Vector3i(x, y, z);
                        const double length =
                            (m * (cell.atoms[j].fraction + s.cast<double>() - cell.atoms[i].fraction)).norm();
                        if (length <= limit && length > .1) {
                            cell.bonds[i] << Bond{j, s};
                            cell.bonds[j] << Bond{i, -s};
                        }
                    }
        }
    // Molecules: the image of each atom follows the bonds from the first.
    cell.images.fill(Eigen::Vector3i::Zero(), n);
    QVector<bool> seen(n, false);
    for (int start = 0; start < n; ++start) {
        if (seen[start])
            continue;
        QVector<int> molecule{start};
        seen[start] = true;
        std::queue<int> next;
        next.push(start);
        while (!next.empty()) {
            const int i = next.front();
            next.pop();
            for (const auto &bond : std::as_const(cell.bonds[i])) {
                const Eigen::Vector3i image = cell.images[i] + bond.shift;
                if (!seen[bond.other]) {
                    seen[bond.other] = true;
                    cell.images[bond.other] = image;
                    molecule << bond.other;
                    next.push(bond.other);
                } else if (cell.images[bond.other] != image)
                    cell.polymeric = true; // It meets itself in another cell: an endless chain or network.
            }
        }
        cell.molecules << molecule;
    }
    return cell;
}
// The fractional centroid of a molecule as it is made whole.
Vec centroidOf(const Cell &cell, const QVector<int> &molecule) {
    Vec sum = Vec::Zero();
    for (int i : molecule)
        sum += cell.atoms[i].fraction + cell.images[i].cast<double>();
    return sum / molecule.size();
}
// The molecules to read: those of the cell, or a formula unit: the fewest that hold every atom of the
// asymmetric unit (preferring those of its own atoms, the identity operation), with as many copies of each
// as the others need. A molecule on a special position holds its atoms twice or more (an anion on an
// inversion center), so that a molecule in a general position comes that many times (its two cations).
QVector<int> chosenMolecules(const Crystal &crystal, const Cell &cell, const CrystalSelection &selection) {
    QVector<int> order(cell.molecules.size());
    std::iota(order.begin(), order.end(), 0);
    if (selection.cell)
        return order;
    const auto own = [&](int molecule) {
        int count = 0;
        for (int i : cell.molecules[molecule])
            count += cell.atoms[i].operation == cell.identity;
        return count;
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return own(a) > own(b); });
    const auto sources = [&](int molecule) {
        QSet<int> set;
        for (int i : cell.molecules[molecule])
            set.insert(cell.atoms[i].source);
        return set;
    };
    QSet<int> covered, wanted;
    for (int i = 0; i < crystal.atoms.size(); ++i)
        if (kept(crystal.atoms[i], selection))
            wanted.insert(i);
    QVector<int> chosen;
    for (int molecule : order) {
        auto added = sources(molecule);
        if (added.subtract(covered).isEmpty())
            continue;
        chosen << molecule;
        covered.unite(sources(molecule));
        if (covered.size() == wanted.size())
            break;
    }
    // Copies: each molecule as many times as the multiplicity of the others asks.
    QVector<int> multiplicity;
    int common = 1;
    for (int molecule : chosen) {
        // The most images of one atom: an atom on the symmetry element itself appears once.
        QHash<int, int> images;
        int most = 1;
        for (int i : cell.molecules[molecule])
            most = std::max(most, ++images[cell.atoms[i].source]);
        multiplicity << most;
        common = std::lcm(common, most);
    }
    const Mat m = crystal.matrix();
    const Vec first = chosen.isEmpty() ? Vec::Zero() : centroidOf(cell, cell.molecules[chosen.first()]);
    const auto distance = [&](int molecule) { // To the first molecule, over the nearest images.
        Vec d = centroidOf(cell, cell.molecules[molecule]) - first;
        d -= d.array().round().matrix();
        return (m * d).norm();
    };
    QVector<int> result;
    QSet<int> taken(chosen.begin(), chosen.end());
    for (int k = 0; k < chosen.size(); ++k) {
        result << chosen[k];
        const int copies = common / multiplicity[k] - 1;
        if (copies <= 0)
            continue;
        const auto set = sources(chosen[k]);
        QVector<int> equivalent;
        for (int molecule = 0; molecule < cell.molecules.size(); ++molecule)
            if (!taken.contains(molecule) && sources(molecule) == set &&
                cell.molecules[molecule].size() == cell.molecules[chosen[k]].size())
                equivalent << molecule;
        std::stable_sort(equivalent.begin(), equivalent.end(),
                         [&](int a, int b) { return distance(a) < distance(b); });
        for (int i = 0; i < std::min<int>(copies, int(equivalent.size())); ++i) {
            result << equivalent[i];
            taken.insert(equivalent[i]);
        }
    }
    return result;
}
} // namespace

Mat Crystal::matrix() const {
    const double rad = M_PI / 180;
    const double ca = std::cos(alpha * rad), cb = std::cos(beta * rad), cg = std::cos(gamma * rad),
                 sg = std::sin(gamma * rad);
    const double volume = std::sqrt(1 - ca * ca - cb * cb - cg * cg + 2 * ca * cb * cg);
    Mat m;
    m << a, b * cg, c * cb, 0, b * sg, c * (ca - cb * cg) / sg, 0, 0, c * volume / sg;
    return m;
}
std::pair<Mat, Vec> crystalOperation(const QString &text) {
    auto parts = text.toLower().remove(' ').remove('\'').remove('"').split(',');
    if (parts.size() != 3)
        fail(tr("CIFの対称操作を読めません: %1").arg(text));
    Mat r = Mat::Zero();
    Vec t = Vec::Zero();
    static const QRegularExpression term("([+-]?)([^+-]+)");
    for (int row = 0; row < 3; ++row) {
        if (parts[row].isEmpty())
            fail(tr("CIFの対称操作を読めません: %1").arg(text));
        int consumed = 0;
        for (auto it = term.globalMatch(parts[row]); it.hasNext();) {
            const auto match = it.next();
            consumed += int(match.capturedLength());
            const double sign = match.captured(1) == "-" ? -1 : 1;
            auto body = match.captured(2);
            const int axis = body.endsWith('x') ? 0 : body.endsWith('y') ? 1 : body.endsWith('z') ? 2 : -1;
            if (axis >= 0) {
                body.chop(1);
                body.remove('*');
            }
            double value = 1;
            if (!body.isEmpty()) {
                bool ok = false;
                if (body.contains('/')) {
                    const double top = body.section('/', 0, 0).toDouble(&ok);
                    bool bottomOk = false;
                    const double bottom = body.section('/', 1, 1).toDouble(&bottomOk);
                    ok = ok && bottomOk && bottom != 0;
                    value = ok ? top / bottom : 0;
                } else
                    value = body.toDouble(&ok);
                if (!ok)
                    fail(tr("CIFの対称操作を読めません: %1").arg(text));
            }
            if (axis >= 0)
                r(row, axis) += sign * value;
            else
                t[row] += sign * value;
        }
        if (consumed != parts[row].size())
            fail(tr("CIFの対称操作を読めません: %1").arg(text));
    }
    return {r, t};
}
Crystal readCrystal(const QByteArray &text) {
    const auto block = readCifBlock(text);
    Crystal c;
    c.name = block.items.value("_chemical_name_systematic").trimmed();
    if (c.name.isEmpty())
        c.name = block.items.value("_chemical_name_common").trimmed();
    if (c.name.isEmpty())
        c.name = block.name;
    c.formula = block.items.value("_chemical_formula_sum").simplified();
    for (const auto *tag : {"_space_group_name_h-m_alt", "_symmetry_space_group_name_h-m"})
        if (c.spaceGroup.isEmpty())
            c.spaceGroup = block.items.value(tag).simplified();
    c.a = required(block, "_cell_length_a");
    c.b = required(block, "_cell_length_b");
    c.c = required(block, "_cell_length_c");
    c.alpha = cifNumber(block.items.value("_cell_angle_alpha")).value_or(90);
    c.beta = cifNumber(block.items.value("_cell_angle_beta")).value_or(90);
    c.gamma = cifNumber(block.items.value("_cell_angle_gamma")).value_or(90);
    for (const double length : {c.a, c.b, c.c})
        if (length <= 0 || length > 1000)
            fail(tr("CIFの格子定数が不正です。"));
    for (const double angle : {c.alpha, c.beta, c.gamma})
        if (angle <= 0 || angle >= 180)
            fail(tr("CIFの格子定数が不正です。"));
    const double ca = std::cos(c.alpha * M_PI / 180), cb = std::cos(c.beta * M_PI / 180),
                 cg = std::cos(c.gamma * M_PI / 180);
    if (1 - ca * ca - cb * cb - cg * cg + 2 * ca * cb * cg <= 1e-8)
        fail(tr("CIFの格子定数が不正です。"));
    for (const auto *tag : {"_space_group_symop_operation_xyz", "_symmetry_equiv_pos_as_xyz"})
        if (c.operations.isEmpty())
            c.operations = block.column(tag);
    c.operations.removeAll(QString());
    if (c.operations.isEmpty())
        c.operations << "x,y,z"; // P1, as a file without operations is taken.
    const auto labels = block.column("_atom_site_label"), types = block.column("_atom_site_type_symbol"),
               xs = block.column("_atom_site_fract_x"), ys = block.column("_atom_site_fract_y"),
               zs = block.column("_atom_site_fract_z"), occupancies = block.column("_atom_site_occupancy"),
               groups = block.column("_atom_site_disorder_group"),
               flags = block.column("_atom_site_calc_flag");
    if (xs.isEmpty() || xs.size() != ys.size() || xs.size() != zs.size())
        fail(tr("CIFに原子の分率座標（_atom_site_fract_x・y・z）がありません。"));
    for (int i = 0; i < xs.size(); ++i) {
        CrystalAtom a;
        a.label = labels.value(i);
        if (flags.value(i).compare("dum", Qt::CaseInsensitive) == 0)
            continue; // A dummy atom of the refinement.
        a.element = elementOf(types.value(i), a.label);
        if (a.element.isEmpty())
            fail(tr("CIFの原子 %1 の元素が分かりません。")
                     .arg(a.label.isEmpty() ? QString::number(i + 1) : a.label));
        const auto x = cifNumber(xs[i]), y = cifNumber(ys[i]), z = cifNumber(zs[i]);
        if (!x || !y || !z)
            fail(tr("CIFの原子 %1 の分率座標を読めません。").arg(a.label));
        a.fraction = Vec(*x, *y, *z);
        a.occupancy = cifNumber(occupancies.value(i)).value_or(1);
        a.group = int(cifNumber(groups.value(i)).value_or(0));
        if (a.occupancy <= 0)
            continue;
        c.atoms << a;
    }
    if (c.atoms.isEmpty())
        fail(tr("CIFに読み込める原子がありません。"));
    return c;
}
CrystalContents crystalContents(const Crystal &crystal, const CrystalSelection &selection) {
    const auto cell = buildCell(crystal, selection);
    CrystalContents contents;
    for (const auto &a : crystal.atoms) {
        contents.disorder |= a.group != 0;
        contents.asymmetricAtoms += kept(a, selection);
    }
    contents.cellMolecules = int(cell.molecules.size());
    contents.cellAtoms = int(cell.atoms.size());
    contents.molecules = int(chosenMolecules(crystal, cell, selection).size());
    contents.polymeric = cell.polymeric;
    return contents;
}
Document crystalStructure(const Crystal &crystal, const CrystalSelection &selection) {
    const auto cell = buildCell(crystal, selection);
    const Mat m = crystal.matrix();
    const auto chosen = chosenMolecules(crystal, cell, selection);
    // Each molecule whole; the first in the cell, the others in the images nearest to it (or each in the
    // cell, for the whole cell).
    QVector<std::pair<QString, Vec>> atoms;
    Vec first = Vec::Zero();
    for (int k = 0; k < chosen.size(); ++k) {
        const auto &molecule = cell.molecules[chosen[k]];
        QVector<Vec> fractions;
        for (int i : molecule)
            fractions << cell.atoms[i].fraction + cell.images[i].cast<double>();
        const Vec centroid = centroidOf(cell, molecule);
        Vec shift = -centroid.array().floor().matrix();
        if (k > 0 && !selection.cell) {
            double best = 1e30;
            const Vec base = (first - centroid).array().round().matrix();
            for (int x = -1; x <= 1; ++x)
                for (int y = -1; y <= 1; ++y)
                    for (int z = -1; z <= 1; ++z) {
                        const Vec s = base + Vec(x, y, z);
                        const double distance = (m * (centroid + s - first)).norm();
                        if (distance < best) {
                            best = distance;
                            shift = s;
                        }
                    }
        }
        if (k == 0)
            first = centroid + shift;
        // The atoms in the order of the asymmetric unit.
        QVector<int> order(molecule.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return std::pair{cell.atoms[molecule[a]].operation != cell.identity,
                             cell.atoms[molecule[a]].source} <
                   std::pair{cell.atoms[molecule[b]].operation != cell.identity,
                             cell.atoms[molecule[b]].source};
        });
        for (int o : order)
            atoms << std::pair{crystal.atoms[cell.atoms[molecule[o]].source].element,
                               m * (fractions[o] + shift)};
    }
    QString xyz = QString("%1\n%2\n").arg(atoms.size()).arg(crystal.name.simplified().left(200));
    for (const auto &[symbol, position] : atoms)
        xyz += QString("%1 %2 %3 %4\n")
                   .arg(symbol)
                   .arg(position.x(), 0, 'f', 6)
                   .arg(position.y(), 0, 'f', 6)
                   .arg(position.z(), 0, 'f', 6);
    auto d = xyzStructure(xyz);
    auto meta = d.data["metadata"].toObject();
    meta["source_format"] = "cif";
    meta["crystal"] = QJsonObject{{"a", crystal.a},
                                  {"b", crystal.b},
                                  {"c", crystal.c},
                                  {"alpha", crystal.alpha},
                                  {"beta", crystal.beta},
                                  {"gamma", crystal.gamma},
                                  {"space_group", crystal.spaceGroup},
                                  {"formula", crystal.formula},
                                  {"contents", selection.cell ? "cell" : "molecules"},
                                  {"disorder", selection.minor ? "all" : "major"},
                                  {"molecules", int(chosen.size())}};
    d.data["metadata"] = meta;
    return d;
}
} // namespace mf
