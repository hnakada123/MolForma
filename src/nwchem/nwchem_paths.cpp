// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_paths.h"
#include "foundation/i18n.h"
#include "model/geometry.h"
#include "model/topology.h"
#include "nwchem/nwchem_advanced.h"
#include <QLocale>
#include <algorithm>
#include <cmath>
namespace mf {
namespace nwchem_detail {
QString shortest(double n) {
    return QString::number(n, 'g', QLocale::FloatingPointShortest);
}
QString coordinateError(const Document &d, const Ids &ids, double value) {
    if (ids.size() < 2 || ids.size() > 4)
        return tr("2〜4原子を指定してください。");
    if (QSet<QString>(ids.begin(), ids.end()).size() != ids.size())
        return tr("同じ原子を重複して指定できません。");
    for (const auto &id : ids) {
        if (!d.contains(id))
            return tr("原子が見つかりません。指定し直してください。");
        if (d.atom(id).ghost)
            return tr("ゴースト中心は指定できません。");
    }
    if (!std::isfinite(value))
        return tr("値は有限の数値で指定してください。");
    if (ids.size() == 2 && (value < 0.1 || value > 50))
        return tr("距離は0.1〜50 Åで指定してください。");
    if (ids.size() == 3 && (value <= 0 || value >= 180))
        return tr("角度は0°より大きく180°未満で指定してください。");
    if (ids.size() == 4 && (value < -180 || value > 180))
        return tr("二面角は−180〜180°で指定してください。");
    return {};
}
// A constrained ZCOORD internal coordinate; distances are in the GEOMETRY units.
QString zcoordLine(const Document &d, const Ids &ids, double value, double scale, const QString &name) {
    QStringList numbers;
    for (const auto &id : ids)
        numbers << QString::number(d.index(id) + 1);
    const char *kind = ids.size() == 2 ? "bond" : ids.size() == 3 ? "angle" : "torsion";
    return QString("%1 %2 %3 %4 constant")
        .arg(kind, numbers.join(' '), shortest(ids.size() == 2 ? value / scale : value), name);
}
QStringList constraintLines(const Document &d, const QJsonObject &s, double scale) {
    QStringList lines;
    const auto constraints = s["constraints"].toArray();
    for (int i = 0; i < constraints.size(); ++i) {
        const auto c = constraints[i].toObject();
        const auto ids = idsFromJson(c["atoms"]);
        if (c["free"].toBool()) {
            // A coordinate added to those of AUTOZ, which the optimization moves (NWChem names it
            // "User00n").
            QStringList numbers;
            for (const auto &id : ids)
                numbers << QString::number(d.index(id) + 1);
            lines << QString("%1 %2").arg(ids.size() == 2   ? "bond"
                                          : ids.size() == 3 ? "angle"
                                                            : "torsion",
                                          numbers.join(' '));
        } else
            lines << zcoordLine(d, ids, c["value"].toDouble(), scale, QString("mfc%1").arg(i + 1));
    }
    return lines;
}
bool constrained(const QJsonObject &s) {
    const auto list = s["constraints"].toArray();
    return std::any_of(list.begin(), list.end(),
                       [](const QJsonValue &c) { return !c.toObject()["free"].toBool(); });
}
// The value of a scan coordinate (1 or 2) at a point.
double scanValue(const QJsonObject &s, int step, int coordinate) {
    const QString prefix = coordinate == 1 ? "scan_" : "scan2_";
    const int points = std::max(2, s[prefix + "steps"].toInt());
    const double start = s[prefix + "start"].toDouble(), end = s[prefix + "end"].toDouble();
    // Rounded to 1e-9, so that inputs read 1.55 rather than 1.5499999999999998.
    return std::round((start + (end - start) * step / (points - 1)) * 1e9) / 1e9;
}
bool twoDimensionalScan(const QJsonObject &s) {
    return !s["scan2_atoms"].toArray().isEmpty();
}
// How far a coordinate is from a value (Å or degrees; dihedral angles across ±180°).
double coordinateDeviation(const Document &d, const Ids &ids, double value) {
    const double now = measure(d, ids);
    if (ids.size() < 4)
        return std::abs(now - value);
    const double turn = std::fmod(std::abs(now - value), 360.0);
    return std::min(turn, 360 - turn);
}
// Geometry of one scan point (with its point of the second coordinate): the later atoms' side moved to
// the scanned values, as when the dimension is set in the editor. The first coordinate is set again
// when setting the second moved it.
Document scanGeometry(const Document &d, const QJsonObject &s, int step, int step2) {
    Document point = d;
    const auto first = idsFromJson(s["scan_atoms"]);
    setDimension(point, first, scanValue(s, step));
    if (twoDimensionalScan(s)) {
        setDimension(point, idsFromJson(s["scan2_atoms"]), scanValue(s, step2, 2));
        if (coordinateDeviation(point, first, scanValue(s, step)) > 1e-4)
            setDimension(point, first, scanValue(s, step));
    }
    return point;
}
// Counterpoise monomers: the bonded molecules of the real atoms, in atom order (atom indices).
QVector<QVector<int>> bsseMonomers(const Document &d) {
    const auto molecule = Topology(d).molecules();
    QVector<int> monomer(d.atoms.size(), -1); // By molecule; molecules of ghosts only are none.
    QVector<QVector<int>> monomers;
    for (int i = 0; i < d.atoms.size(); ++i)
        if (!d.atoms[i].ghost && monomer[molecule[i]] < 0) {
            monomer[molecule[i]] = monomers.size();
            monomers.append(QVector<int>());
        }
    for (int i = 0; i < d.atoms.size(); ++i)
        if (const int m = monomer[molecule[i]]; m >= 0)
            monomers[m] << i;
    return monomers;
}
int monomerCharge(const Document &d, const QVector<int> &atoms) {
    int charge = 0;
    for (int i : atoms)
        charge += d.atoms[i].charge;
    return charge;
}
} // namespace nwchem_detail
using namespace nwchem_detail;
QVector<NwchemFragment> nwchemFragments(const Document &d) {
    QVector<NwchemFragment> result;
    const auto list = nwchemSettings(d)["fragment_guess"].toArray();
    if (list.isEmpty())
        return result;
    QHash<QString, int> index;
    for (int i = 0; i < d.atoms.size(); ++i)
        index[d.atoms[i].id] = i;
    for (const auto &value : list) {
        const auto o = value.toObject();
        NwchemFragment fragment;
        for (const auto &id : idsFromJson(o["atoms"]))
            if (const auto it = index.constFind(id); it != index.cend())
                fragment.atoms << *it;
        std::sort(fragment.atoms.begin(), fragment.atoms.end());
        fragment.charge = o["charge"].toInt();
        fragment.multiplicity = o["multiplicity"].toInt(1);
        result << fragment;
    }
    return result;
}
QVector<QPair<int, int>> nwchemSpinSpinPairs(const Document &d) {
    const bool all = nwchemSettings(d)["spinspin_pairs"] == "all";
    QVector<int> atoms;
    for (int i = 0; i < d.atoms.size(); ++i)
        if (!d.atoms[i].ghost &&
            (all || d.atoms[i].element == "H" || d.atoms[i].element == "F" || d.atoms[i].element == "P"))
            atoms << i;
    QVector<QPair<int, int>> pairs;
    if (atoms.size() > 1000) // More than any limit allows; their number is all that matters then.
        return QVector<QPair<int, int>>(atoms.size() * (atoms.size() - 1) / 2);
    for (int i = 0; i < atoms.size(); ++i)
        for (int j = i + 1; j < atoms.size(); ++j)
            pairs << qMakePair(atoms[i], atoms[j]);
    return pairs;
}
bool nwchemAutoz(const Document &d) {
    const auto s = nwchemSettings(d);
    return NwchemCalculation(s).optimization() && s["constraints"].toArray().isEmpty() &&
           !nwchemAdvanced(d)["driver_cartesian"].toBool() &&
           std::none_of(d.atoms.begin(), d.atoms.end(), [](const Atom &a) { return a.fixed || a.ghost; });
}
QString nwchemCoordinateLabel(const Document &d, const Ids &ids) {
    QStringList numbers;
    for (const auto &id : ids)
        numbers << (d.contains(id) ? QString::number(d.index(id) + 1) : QString("?"));
    const auto atoms = numbers.join("–");
    return ids.size() == 2   ? tr("距離 %1").arg(atoms)
           : ids.size() == 3 ? tr("角度 %1").arg(atoms)
                             : tr("二面角 %1").arg(atoms);
}
QVector<QVector<int>> nwchemBsseMonomers(const Document &d) {
    return bsseMonomers(d);
}
NwchemNebEnd nwchemNebEnd(const Document &d) {
    NwchemNebEnd result;
    const auto stored = d.data["nwchem"].toObject()["neb_end"].toObject();
    if (stored.isEmpty()) {
        result.problem = tr("NEBの終点の構造を「2 計算条件」の「IRC・スキャン・NEB」で設定してください。");
        return result;
    }
    Document end = d;
    int missing = 0;
    for (auto &a : end.atoms) {
        const auto value = stored.value(a.id).toArray();
        const Vec position(value.at(0).toDouble(qQNaN()), value.at(1).toDouble(qQNaN()),
                           value.at(2).toDouble(qQNaN()));
        if (value.size() != 3 || !position.allFinite())
            ++missing;
        else
            a.position = position;
    }
    if (missing)
        result.problem = tr("NEBの終点に位置のない原子が %n 個あります（終点を設定した後に原子を加えた場合"
                            "など）。終点を設定し直してください。",
                            nullptr, missing);
    else
        result.document = end;
    return result;
}
QJsonObject nwchemNebEndSetting(const Document &d, const Document &end, bool fit) {
    if (end.atoms.size() != d.atoms.size())
        fail(tr("終点の構造の原子数（%1）が分子（%2）と異なります。")
                 .arg(end.atoms.size())
                 .arg(d.atoms.size()));
    for (int i = 0; i < d.atoms.size(); ++i) {
        const auto &a = d.atoms[i], &b = end.atoms[i];
        if (a.element != b.element || a.ghost != b.ghost)
            fail(tr("原子%1の元素（%2 / 終点 %3）が一致しません。原子の順番が同じ構造を選んでください。")
                     .arg(i + 1)
                     .arg(a.element, b.element));
        if (!b.position.allFinite())
            fail(tr("終点の構造の座標が有限ではありません。"));
    }
    Superposition placement;
    if (fit && d.atoms.size() >= 2)
        placement = superpose(end, d);
    QJsonObject positions;
    for (int i = 0; i < d.atoms.size(); ++i)
        positions[d.atoms[i].id] = vectorToJson(placement.rotation * end.atoms[i].position + placement.shift);
    return positions;
}
QVector<Document> nwchemNebInitialPath(const Document &start, const Document &end, int beads) {
    QVector<Document> path;
    beads = std::max(2, beads);
    for (int k = 0; k < beads; ++k) {
        Document bead = start;
        const double t = double(k) / (beads - 1);
        for (int i = 0; i < bead.atoms.size() && i < end.atoms.size(); ++i)
            bead.atoms[i].position = (1 - t) * start.atoms[i].position + t * end.atoms[i].position;
        path << bead;
    }
    return path;
}
} // namespace mf
