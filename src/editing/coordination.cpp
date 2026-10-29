// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/coordination.h"
#include "model/i18n.h"
#include <cmath>
namespace mf {
QStringList geometryNames() {
    return {"linear",
            "trigonal_planar",
            "tetrahedral",
            "square_planar",
            "trigonal_bipyramidal",
            "square_pyramidal",
            "octahedral",
            "pentagonal_bipyramidal",
            "square_antiprismatic",
            "dodecahedral",
            "tricapped_trigonal_prismatic",
            "capped_square_antiprismatic"};
}
QStringList geometryLabels() {
    return {tr("直線"),           tr("三角平面"), tr("四面体"),     tr("平面四角形"),
            tr("三方両錐"),       tr("四角錐"),   tr("八面体"),     tr("五方両錐"),
            tr("四方逆プリズム"), tr("十二面体"), tr("三冠三角柱"), tr("一冠四方逆プリズム")};
}
QVector<Vec> siteVectors(const QString &g) {
    auto sph = [](double t, double p) {
        t *= M_PI / 180;
        p *= M_PI / 180;
        return Vec(std::sin(t) * std::cos(p), std::sin(t) * std::sin(p), std::cos(t));
    };
    QVector<Vec> out;
    int i = geometryNames().indexOf(g);
    if (i == 0)
        out = {Vec::UnitX(), -Vec::UnitX()};
    else if (i == 1 || i == 4) {
        out = {Vec(1, 0, 0), Vec(-.5, std::sqrt(3.) / 2, 0), Vec(-.5, -std::sqrt(3.) / 2, 0)};
        if (i == 4)
            out << Vec::UnitZ() << -Vec::UnitZ();
    } else if (i == 2)
        out = {Vec(1, 1, 1), Vec(1, -1, -1), Vec(-1, 1, -1), Vec(-1, -1, 1)};
    else if (i == 3 || i == 5 || i == 6) {
        out = {Vec::UnitX(), Vec::UnitY(), -Vec::UnitX(), -Vec::UnitY()};
        if (i >= 5)
            out << Vec::UnitZ();
        if (i == 6)
            out << -Vec::UnitZ();
    } else if (i == 7) {
        for (int k = 0; k < 5; ++k)
            out << sph(90, 72 * k);
        out << Vec::UnitZ() << -Vec::UnitZ();
    } else if (i == 8 || i == 11) {
        if (i == 11)
            out << Vec::UnitZ();
        for (int k = 0; k < 4; ++k)
            out << sph(i == 8 ? 57.13 : 68.86, 90 * k);
        for (int k = 0; k < 4; ++k)
            out << sph(i == 8 ? 122.87 : 126.96, 90 * k + 45);
    } else if (i == 9)
        out = {sph(37.33, 0),  sph(37.33, 180), sph(142.67, 90), sph(142.67, 270),
               sph(71.39, 90), sph(71.39, 270), sph(108.61, 0),  sph(108.61, 180)};
    else if (i == 10) {
        for (int k = 0; k < 3; ++k)
            out << sph(44.69, 120 * k);
        for (int k = 0; k < 3; ++k)
            out << sph(135.31, 120 * k);
        for (int k = 0; k < 3; ++k)
            out << sph(90, 120 * k + 60);
    } else
        fail(tr("未知の配位形状です。"));
    for (auto &v : out)
        v.normalize();
    return out;
}
Center center(const Document &d, const QString &id) {
    if (const auto *c = d.findCenter(id))
        return *c;
    fail(tr("配位中心を選択してください。"));
}
void updateCenter(Document &d, const Center &c) {
    for (auto &row : d.centers)
        if (row.atom == c.atom) {
            row = c;
            return;
        }
    d.centers << c;
}
static QVector<CoordinationSite> sitesOf(const QString &geometry, double distance) {
    QVector<CoordinationSite> sites;
    for (const auto &direction : siteVectors(geometry)) {
        CoordinationSite site;
        site.direction = direction;
        site.distance = distance;
        sites << site;
    }
    return sites;
}
// Coordination edits keep the total charge in step with the oxidation states and the ligands' formal
// charges (followingCharge).
QString addCenter(Document &d, const QString &symbol, const QString &geometry, const Vec &position,
                  double distance, int oxidation) {
    if (distance <= 0)
        fail(tr("配位距離は正の値にしてください。"));
    element(symbol);
    Atom a;
    a.element = symbol;
    a.position = d.atoms.isEmpty() ? Vec::Zero() : position;
    Center c;
    c.atom = a.id;
    c.geometry = geometry;
    c.oxidation = oxidation;
    c.sites = sitesOf(geometry, distance);
    followingCharge(d, [&] {
        d.atoms << a;
        updateCenter(d, c);
    });
    return a.id;
}
void setOxidation(Document &d, const QString &id, int oxidation) {
    auto c = center(d, id);
    c.oxidation = oxidation;
    followingCharge(d, [&] { updateCenter(d, c); });
}
static PlacedPart bindLigand(Document &d, const QString &id, const QList<int> &indices, const QJsonObject &t,
                             double twist) {
    auto c = center(d, id);
    auto connections = t["connections"].toArray();
    if (indices.size() != connections.size() || indices.isEmpty() || indices.size() > 2)
        fail(tr("配位子の供与原子数に合う1〜2サイトを選択してください。"));
    QSet<int> distinct;
    QVector<Vec> targets, points, anchors;
    Vec origin = d.atom(id).position;
    for (int i : indices) {
        if (i < 0 || i >= c.sites.size() || distinct.contains(i) || !c.sites[i].donor.isEmpty())
            fail(tr("サイトが占有済み、重複、または範囲外です。"));
        distinct.insert(i);
        targets << origin + c.sites[i].direction * c.sites[i].distance;
    }
    for (auto v : t["atoms"].toArray())
        points << vectorFromJson(v.toObject()["position"]);
    for (auto v : connections)
        anchors << points[v.toInt()];
    Mat r = Mat::Identity();
    Vec shift = Vec::Zero();
    if (indices.size() == 1) {
        Vec average = Vec::Zero();
        for (auto p : points)
            average += p;
        average /= points.size();
        Vec away = average - anchors[0];
        if (away.norm() < 1e-8)
            away = Vec::UnitX();
        Vec dir = unit(targets[0] - origin);
        r = rotation(dir, twist) * alignVectors(away, dir);
        shift = targets[0] - r * anchors[0];
    } else {
        if (unit(targets[0] - origin).dot(unit(targets[1] - origin)) < -.9)
            fail(tr("二座配位子には隣接するシスサイトを指定してください。"));
        if (std::abs((anchors[1] - anchors[0]).norm() - (targets[1] - targets[0]).norm()) > .18)
            fail(tr("供与原子間隔とサイト間隔が合いません。配位距離を調整してください。"));
        auto rt = fitPoints(anchors, targets);
        r = rt.first;
        shift = rt.second;
        Vec average = Vec::Zero();
        for (auto p : points)
            average += r * p + shift;
        average /= points.size();
        Vec mid = (targets[0] + targets[1]) * .5, axis = unit(targets[1] - targets[0]);
        Vec tail = average - mid, away = mid - origin;
        tail -= tail.dot(axis) * axis;
        away -= away.dot(axis) * axis;
        if (tail.norm() > 1e-8 && away.norm() > 1e-8) {
            double angle =
                std::atan2(axis.dot(unit(tail).cross(unit(away))), unit(tail).dot(unit(away))) * 180 / M_PI;
            Mat turn = rotation(axis, angle + twist);
            shift = mid + turn * (shift - mid);
            r = turn * r;
        }
    }
    for (auto &p : points)
        p = r * p + shift;
    QMap<int, QList<int>> neighbors;
    auto templateAtoms = t["atoms"].toArray();
    for (auto v : t["bonds"].toArray()) {
        auto b = v.toObject();
        int a = b["a"].toInt(), c = b["b"].toInt();
        neighbors[a] << c;
        neighbors[c] << a;
    }
    QSet<int> donors;
    for (auto v : connections)
        donors.insert(v.toInt());
    for (int donor : donors) {
        QList<int> heavy, hydrogens;
        for (int n : neighbors[donor]) {
            if (templateAtoms[n].toObject()["element"] != "H")
                heavy << n;
            else if (neighbors[n].size() == 1)
                hydrogens << n;
        }
        if (heavy.size() != 1 || hydrogens.isEmpty())
            continue;
        Vec axis = unit(points[heavy[0]] - points[donor]), lone = Vec::Zero();
        for (int h : hydrogens)
            lone -= unit(points[h] - points[donor]);
        lone -= lone.dot(axis) * axis;
        Vec target = origin - points[donor];
        target -= target.dot(axis) * axis;
        if (lone.norm() < 1e-8 || target.norm() < 1e-8)
            continue;
        double angle =
            std::atan2(axis.dot(unit(lone).cross(unit(target))), unit(lone).dot(unit(target))) * 180 / M_PI;
        Mat turn = rotation(axis, angle);
        for (int h : hydrogens)
            points[h] = points[donor] + turn * (points[h] - points[donor]);
    }
    for (int i = 0; i < points.size(); ++i)
        if (!donors.contains(i) && (points[i] - origin).norm() <
                                       element(d.atom(id).element).radius +
                                           element(templateAtoms[i].toObject()["element"].toString()).radius)
            fail(tr("供与原子以外の原子が配位中心に近すぎます。サイト・距離・回転角を変更してください。"));
    auto p = instantiate(d, t, points);
    auto &meta = d.parts.last().metadata;
    QJsonArray selected;
    for (int i : indices)
        selected.append(i);
    meta["coordination_center"] = id;
    meta["sites"] = selected;
    meta["twist"] = twist;
    for (int k = 0; k < indices.size(); ++k) {
        auto &site = c.sites[indices[k]];
        site.donor = p.mapping[connections[k].toInt()];
        site.part = p.id;
        d.setBond(site.donor, id, "dative");
    }
    updateCenter(d, c);
    return p;
}
PlacedPart placeLigand(Document &d, const QString &id, const QList<int> &indices, const QJsonObject &t,
                       double twist) {
    PlacedPart placed;
    followingCharge(d, [&] { placed = bindLigand(d, id, indices, t, twist); });
    return placed;
}
static void refitCenter(Document &d, const QString &id) {
    auto c = center(d, id);
    const auto parts = d.parts;
    for (const auto &p : parts) {
        if (p.center() != id)
            continue;
        QList<int> idx;
        for (auto s : p.metadata["sites"].toArray()) {
            int i = s.toInt();
            if (i >= c.sites.size())
                fail(tr("占有サイトより少ない配位数には変更できません。"));
            idx << i;
        }
        const auto &oldIds = p.atoms;
        QJsonArray atoms, bonds;
        for (auto atomId : oldIds) {
            auto atom = d.atom(atomId).toJson();
            atom.remove("id");
            atoms.append(atom);
        }
        for (const auto &bond : d.bonds)
            if (oldIds.contains(bond.a) && oldIds.contains(bond.b))
                bonds.append(QJsonObject{
                    {"a", oldIds.indexOf(bond.a)}, {"b", oldIds.indexOf(bond.b)}, {"kind", bond.kind}});
        QJsonObject t{{"id", "my:temporary"},
                      {"category", "ligand"},
                      {"atoms", atoms},
                      {"bonds", bonds},
                      {"leaving", QJsonValue()}};
        QJsonArray connections;
        for (const auto &donor : p.connections)
            connections.append(oldIds.indexOf(donor));
        t["connections"] = connections;
        Document temp = d;
        for (auto &a : temp.atoms)
            a.fixed = false;
        temp.remove(oldIds);
        auto placed = bindLigand(temp, id, idx, t, 0);
        for (int i = 0; i < oldIds.size(); ++i) {
            Vec target = temp.atom(placed.atoms[i]).position;
            if (d.atom(oldIds[i]).fixed && (target - d.atom(oldIds[i]).position).norm() > 1e-7)
                fail(tr("固定原子を動かせません。"));
            d.atom(oldIds[i]).position = target;
        }
        c = center(d, id);
        for (int k = 0; k < idx.size(); ++k) {
            c.sites[idx[k]].donor = p.connections.value(k);
            c.sites[idx[k]].part = p.id;
        }
        updateCenter(d, c);
    }
}
void adjustCenter(Document &d, const QString &id, const QString &geometry, double distance, int oxidation) {
    auto c = center(d, id);
    auto sites = sitesOf(geometry, distance);
    if (distance <= 0)
        fail(tr("配位距離は正の値にしてください。"));
    c.geometry = geometry;
    c.sites = sites;
    c.oxidation = oxidation;
    followingCharge(d, [&] {
        updateCenter(d, c);
        refitCenter(d, id);
    });
}

void moveLigand(Document &d, const QString &partId, const QList<int> &sites, const QJsonObject &replacement,
                double twist) {
    for (auto &p : d.parts) {
        if (p.id != partId)
            continue;
        auto id = p.center();
        if (id.isEmpty())
            fail(tr("配位子を選択してください。"));
        QJsonObject t = replacement;
        if (t.isEmpty()) {
            auto c = center(d, id);
            if (sites.size() != p.connections.size())
                fail(tr("供与原子数と同じサイト数を指定してください。"));
            QSet<int> unique;
            for (int i : sites) {
                if (i < 0 || i >= c.sites.size() || unique.contains(i))
                    fail(tr("サイト番号が不正です。"));
                unique.insert(i);
                if (!c.sites[i].part.isEmpty() && c.sites[i].part != partId)
                    fail(tr("移動先サイトは占有されています。"));
            }
            for (auto &site : c.sites)
                if (site.part == partId)
                    site.donor = site.part = QString();
            QJsonArray indices;
            for (int i : sites)
                indices.append(i);
            p.metadata["sites"] = indices;
            updateCenter(d, c);
            refitCenter(d, id);
            return;
        }
        if (t.isEmpty())
            t = p.metadata["template_data"].isObject() ? p.metadata["template_data"].toObject()
                                                       : findTemplate(p.templateId);
        const auto atoms = p.atoms;
        followingCharge(d, [&] {
            d.remove(atoms);
            bindLigand(d, id, sites, t, twist);
        });
        return;
    }
    fail(tr("配位子を選択してください。"));
}
void setSiteDirection(Document &d, const QString &id, const QList<int> &indices, const Vec &direction,
                      double distance) {
    if (indices.isEmpty() || distance <= 0)
        fail(tr("サイト番号と正の距離を指定してください。"));
    Vec dir = unit(direction);
    auto c = center(d, id);
    for (int i : indices) {
        if (i < 0 || i >= c.sites.size())
            fail(tr("サイト番号が範囲外です。"));
        c.sites[i].direction = dir;
        c.sites[i].distance = distance;
    }
    updateCenter(d, c);
    refitCenter(d, id);
}
void swapLigands(Document &d, const QString &id, int first, int second) {
    auto c = center(d, id);
    if (first < 0 || second < 0 || first >= c.sites.size() || second >= c.sites.size() || first == second)
        fail(tr("異なる2サイトを指定してください。"));
    QString a = c.sites[first].part, b = c.sites[second].part;
    if (a.isEmpty() || b.isEmpty() || a == b)
        fail(tr("異なる配位子で占有された2サイトが必要です。"));
    for (auto &p : d.parts) {
        if (p.id != a && p.id != b)
            continue;
        if (p.metadata["sites"].toArray().size() != 1)
            fail(tr("サイト交換は2つの単座配位子で行ってください。"));
        p.metadata["sites"] = QJsonArray{p.id == a ? second : first};
    }
    for (int i : {first, second})
        c.sites[i].donor = c.sites[i].part = QString();
    updateCenter(d, c);
    refitCenter(d, id);
}
} // namespace mf
