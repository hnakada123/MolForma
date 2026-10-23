// SPDX-License-Identifier: Apache-2.0
// Derived from KAGAMI tools/kagami_gui (Copyright 2026 Hiroshi Nakada).
// MolForma: translated and adapted to native Qt6/C++ document and editing APIs.
#include "editing/coordination.h"
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
    return {"直線",   "三角平面", "四面体",         "平面四角形", "三方両錐",   "四角錐",
            "八面体", "五方両錐", "四方逆プリズム", "十二面体",   "三冠三角柱", "一冠四方逆プリズム"};
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
        fail("未知の配位形状です。");
    for (auto &v : out)
        v.normalize();
    return out;
}
QJsonObject center(const Document &d, const QString &id) {
    for (auto v : d.data["centers"].toArray())
        if (v.toObject()["atom"] == id)
            return v.toObject();
    fail("配位中心を選択してください。");
}
void updateCenter(Document &d, const QJsonObject &c) {
    auto rows = d.data["centers"].toArray();
    for (int i = 0; i < rows.size(); ++i)
        if (rows[i].toObject()["atom"] == c["atom"]) {
            rows[i] = c;
            d.data["centers"] = rows;
            return;
        }
    rows.append(c);
    d.data["centers"] = rows;
}
QString addCenter(Document &d, const QString &symbol, const QString &geometry, const Vec &position,
                  double distance, int oxidation) {
    if (distance <= 0)
        fail("配位距離は正の値にしてください。");
    element(symbol);
    Atom a;
    a.element = symbol;
    a.position = d.atoms.isEmpty() ? Vec::Zero() : position;
    d.atoms << a;
    QJsonArray sites;
    for (auto direction : siteVectors(geometry))
        sites.append(QJsonObject{{"direction", vectorToJson(direction)}, {"distance", distance}});
    updateCenter(d, {{"atom", a.id}, {"geometry", geometry}, {"sites", sites}, {"oxidation", oxidation}});
    d.invalidateAssignments();
    return a.id;
}
PlacedPart placeLigand(Document &d, const QString &id, const QList<int> &indices, const QJsonObject &t,
                       double twist) {
    auto c = center(d, id);
    auto sites = c["sites"].toArray(), connections = t["connections"].toArray();
    if (indices.size() != connections.size() || indices.isEmpty() || indices.size() > 2)
        fail("配位子の供与原子数に合う1〜2サイトを選択してください。");
    QSet<int> distinct;
    QVector<Vec> targets, points, anchors;
    Vec origin = d.atom(id).position;
    for (int i : indices) {
        if (i < 0 || i >= sites.size() || distinct.contains(i) ||
            !sites[i].toObject()["donor"].toString().isEmpty())
            fail("サイトが占有済み、重複、または範囲外です。");
        distinct.insert(i);
        auto s = sites[i].toObject();
        targets << origin + vectorFromJson(s["direction"]) * s["distance"].toDouble();
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
            fail("二座配位子には隣接するシスサイトを指定してください。");
        if (std::abs((anchors[1] - anchors[0]).norm() - (targets[1] - targets[0]).norm()) > .18)
            fail("供与原子間隔とサイト間隔が合いません。配位距離を調整してください。");
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
            fail("供与原子以外の原子が配位中心に近すぎます。サイト・距離・回転角を変更してください。");
    auto p = instantiate(d, t, points);
    auto parts = d.data["parts"].toArray();
    auto last = parts.last().toObject(), meta = last["metadata"].toObject();
    QJsonArray selected;
    for (int i : indices)
        selected.append(i);
    meta["coordination_center"] = id;
    meta["sites"] = selected;
    meta["twist"] = twist;
    last["metadata"] = meta;
    parts[parts.size() - 1] = last;
    d.data["parts"] = parts;
    for (int k = 0; k < indices.size(); ++k) {
        auto s = sites[indices[k]].toObject();
        QString donor = p.mapping[connections[k].toInt()];
        s["donor"] = donor;
        s["part"] = p.id;
        sites[indices[k]] = s;
        d.setBond(donor, id, "dative");
    }
    c["sites"] = sites;
    updateCenter(d, c);
    return p;
}
static void refitCenter(Document &d, const QString &id) {
    auto c = center(d, id);
    auto sites = c["sites"].toArray();
    auto parts = d.data["parts"].toArray();
    for (auto v : parts) {
        auto p = v.toObject(), meta = p["metadata"].toObject();
        if (meta["coordination_center"] != id)
            continue;
        QList<int> idx;
        for (auto s : meta["sites"].toArray()) {
            int i = s.toInt();
            if (i >= sites.size())
                fail("占有サイトより少ない配位数には変更できません。");
            idx << i;
        }
        auto oldIds = idsFromJson(p["atoms"]);
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
        for (auto donor : idsFromJson(p["connections"]))
            connections.append(oldIds.indexOf(donor));
        t["connections"] = connections;
        Document temp = d;
        for (auto &a : temp.atoms)
            a.fixed = false;
        temp.remove(oldIds);
        auto placed = placeLigand(temp, id, idx, t, 0);
        for (int i = 0; i < oldIds.size(); ++i) {
            Vec target = temp.atom(placed.atoms[i]).position;
            if (d.atom(oldIds[i]).fixed && (target - d.atom(oldIds[i]).position).norm() > 1e-7)
                fail("固定原子を動かせません。");
            d.atom(oldIds[i]).position = target;
        }
        c = center(d, id);
        sites = c["sites"].toArray();
        for (int k = 0; k < idx.size(); ++k) {
            auto s = sites[idx[k]].toObject();
            s["donor"] = p["connections"].toArray()[k];
            s["part"] = p["id"];
            sites[idx[k]] = s;
        }
        c["sites"] = sites;
        updateCenter(d, c);
    }
}
void adjustCenter(Document &d, const QString &id, const QString &geometry, double distance, int oxidation) {
    auto old = center(d, id);
    auto directions = siteVectors(geometry);
    QJsonArray sites;
    for (auto v : directions)
        sites.append(QJsonObject{{"direction", vectorToJson(v)}, {"distance", distance}});
    if (distance <= 0)
        fail("配位距離は正の値にしてください。");
    auto c = old;
    c["geometry"] = geometry;
    c["sites"] = sites;
    c["oxidation"] = oxidation;
    updateCenter(d, c);
    refitCenter(d, id);
}

void moveLigand(Document &d, const QString &partId, const QList<int> &sites, const QJsonObject &replacement,
                double twist) {
    for (auto v : d.data["parts"].toArray()) {
        auto p = v.toObject();
        if (p["id"] != partId)
            continue;
        auto meta = p["metadata"].toObject();
        auto id = meta["coordination_center"].toString();
        if (id.isEmpty())
            fail("配位子を選択してください。");
        QJsonObject t = replacement;
        if (t.isEmpty()) {
            auto c = center(d, id);
            auto current = c["sites"].toArray();
            if (sites.size() != p["connections"].toArray().size())
                fail("供与原子数と同じサイト数を指定してください。");
            QSet<int> unique;
            for (int i : sites) {
                if (i < 0 || i >= current.size() || unique.contains(i))
                    fail("サイト番号が不正です。");
                unique.insert(i);
                auto s = current[i].toObject();
                if (!s["part"].toString().isEmpty() && s["part"] != partId)
                    fail("移動先サイトは占有されています。");
            }
            for (int i = 0; i < current.size(); ++i) {
                auto s = current[i].toObject();
                if (s["part"] == partId) {
                    s.remove("donor");
                    s.remove("part");
                    current[i] = s;
                }
            }
            QJsonArray indices;
            for (int i : sites)
                indices.append(i);
            meta["sites"] = indices;
            p["metadata"] = meta;
            auto parts = d.data["parts"].toArray();
            for (int i = 0; i < parts.size(); ++i)
                if (parts[i].toObject()["id"] == partId)
                    parts[i] = p;
            d.data["parts"] = parts;
            c["sites"] = current;
            updateCenter(d, c);
            refitCenter(d, id);
            return;
        }
        if (t.isEmpty())
            t = meta["template_data"].isObject() ? meta["template_data"].toObject()
                                                 : findTemplate(p["template"].toString());
        d.remove(idsFromJson(p["atoms"]));
        placeLigand(d, id, sites, t, twist);
        return;
    }
    fail("配位子を選択してください。");
}
void setSiteDirection(Document &d, const QString &id, const QList<int> &indices, const Vec &direction,
                      double distance) {
    if (indices.isEmpty() || distance <= 0)
        fail("サイト番号と正の距離を指定してください。");
    Vec dir = unit(direction);
    auto c = center(d, id);
    auto sites = c["sites"].toArray();
    for (int i : indices) {
        if (i < 0 || i >= sites.size())
            fail("サイト番号が範囲外です。");
        auto s = sites[i].toObject();
        s["direction"] = vectorToJson(dir);
        s["distance"] = distance;
        sites[i] = s;
    }
    c["sites"] = sites;
    updateCenter(d, c);
    refitCenter(d, id);
}
void swapLigands(Document &d, const QString &id, int first, int second) {
    auto c = center(d, id);
    auto sites = c["sites"].toArray();
    if (first < 0 || second < 0 || first >= sites.size() || second >= sites.size() || first == second)
        fail("異なる2サイトを指定してください。");
    QString a = sites[first].toObject()["part"].toString(), b = sites[second].toObject()["part"].toString();
    if (a.isEmpty() || b.isEmpty() || a == b)
        fail("異なる配位子で占有された2サイトが必要です。");
    auto parts = d.data["parts"].toArray();
    for (int i = 0; i < parts.size(); ++i) {
        auto p = parts[i].toObject();
        if (p["id"] != a && p["id"] != b)
            continue;
        auto meta = p["metadata"].toObject();
        if (meta["sites"].toArray().size() != 1)
            fail("サイト交換は2つの単座配位子で行ってください。");
        meta["sites"] = QJsonArray{p["id"] == a ? second : first};
        p["metadata"] = meta;
        parts[i] = p;
    }
    for (int i : {first, second}) {
        auto s = sites[i].toObject();
        s.remove("donor");
        s.remove("part");
        sites[i] = s;
    }
    c["sites"] = sites;
    d.data["parts"] = parts;
    updateCenter(d, c);
    refitCenter(d, id);
}
} // namespace mf
