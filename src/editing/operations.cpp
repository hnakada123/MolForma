// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/operations.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
namespace mf {
static int valence(const Atom &a, double used) {
    if (a.element == "H")
        return std::max(0, (a.charge ? 0 : 1) - a.radical); // H+ / H- (hydride) have no bond
    if (a.element == "C" || a.element == "Si")
        return 4 - std::abs(a.charge) - a.radical;
    // Onium cations gain one bond; each negative charge removes one (oxide O2- has none).
    if (a.element == "N")
        return std::max(0, (a.charge > 0 ? 4 : 3 + a.charge) - a.radical);
    if (a.element == "O")
        return std::max(0, (a.charge > 0 ? 3 : 2 + a.charge) - a.radical);
    if (QStringList{"F", "Cl", "Br", "I"}.contains(a.element))
        return a.charge < 0 ? 0 : 1;
    if (a.element == "B")
        return a.charge < 0 ? 4 : 3;
    if (a.element == "P")
        return used > 3.1 ? 5 : 3;
    if (a.element == "S" && a.charge)
        return std::max(0, 2 + a.charge - a.radical); // thiolate S-, sulfide S2-, sulfonium S+
    if (a.element == "S")
        return used > 4.1 ? 6 : used > 2.1 ? 4 : 2;
    return -1;
}
// Aromatic bonds count as single bonds plus at most one Kekulé double bond per atom. Atoms
// with a free valence are paired along aromatic bonds; when no perfect pairing exists, the
// assignment leaves heteroatoms unpaired before carbon (pyrrole-type N takes an H instead).
// Returns the ids of atoms that received a double bond.
static QSet<QString> aromaticDoubleBonds(const Document &d) {
    const int n = d.atoms.size();
    QHash<QString, int> index;
    for (int i = 0; i < n; ++i)
        index[d.atoms[i].id] = i;
    QVector<double> sigma(n);
    QVector<QVector<int>> aromatic(n);
    for (const auto &b : d.bonds) {
        const int x = index.value(b.a, -1), y = index.value(b.b, -1);
        if (x < 0 || y < 0)
            continue;
        const bool ring = b.kind == "aromatic";
        sigma[x] += ring ? 1 : b.order();
        sigma[y] += ring ? 1 : b.order();
        if (ring) {
            aromatic[x] << y;
            aromatic[y] << x;
        }
    }
    QVector<bool> wants(n), seen(n);
    for (int i = 0; i < n; ++i)
        if (!aromatic[i].isEmpty() && !d.atoms[i].ghost) {
            const int v = valence(d.atoms[i], sigma[i]);
            wants[i] = v >= 0 && v - sigma[i] > .9;
        }
    QSet<QString> paired;
    QVector<int> partner(n, -1), best;
    for (int start = 0; start < n; ++start) {
        if (!wants[start] || seen[start])
            continue;
        // Breadth-first order keeps ring neighbors close, so the first descent usually pairs all.
        QVector<int> order{start};
        seen[start] = true;
        for (int k = 0; k < order.size(); ++k)
            for (int j : aromatic[order[k]])
                if (wants[j] && !seen[j]) {
                    seen[j] = true;
                    order << j;
                }
        int penalty = 0, bestPenalty = std::numeric_limits<int>::max();
        long steps = 0;
        std::function<void(int)> search = [&](int k) {
            if (penalty >= bestPenalty || ++steps > 200000)
                return;
            if (k == order.size()) {
                bestPenalty = penalty;
                best = partner;
                return;
            }
            const int i = order[k];
            if (partner[i] >= 0)
                return search(k + 1);
            for (int j : aromatic[i])
                if (wants[j] && partner[j] < 0) {
                    partner[i] = j;
                    partner[j] = i;
                    search(k + 1);
                    partner[i] = partner[j] = -1;
                }
            const int cost = d.atoms[i].element == "C" ? 3 : 1;
            penalty += cost;
            search(k + 1);
            penalty -= cost;
        };
        search(0);
        for (int i : order)
            if (best[i] >= 0)
                paired.insert(d.atoms[i].id);
    }
    return paired;
}
// Bond-order sum for valence checks, with aromatic bonds in their Kekulé form.
static double valenceUsed(const Document &d, const QString &id, const QSet<QString> &doubles) {
    double used = doubles.contains(id) ? 1 : 0;
    for (const auto &b : d.bonds)
        if (b.a == id || b.b == id)
            used += b.kind == "aromatic" ? 1 : b.order();
    return used;
}
Vec freeDirection(const Document &d, const QString &id) {
    const Vec origin = d.atom(id).position;
    QVector<Vec> directions;
    for (const auto &n : d.neighbors(id, false))
        directions << unit(d.atom(n).position - origin);
    if (directions.isEmpty())
        return Vec(1, 1, 1).normalized();
    // Match tetrahedral/trigonal/linear bond angles while avoiding existing atoms.
    double maxOrder = 1;
    for (const auto &b : d.bonds)
        if (b.a == id || b.b == id)
            maxOrder = std::max(maxOrder, b.order());
    double target = maxOrder >= 3 ? -1 : maxOrder >= 1.5 ? -.5 : -1. / 3;
    Vec best = Vec::UnitZ();
    double bestScore = 1e100;
    for (int i = 0; i < 320; ++i) {
        double z = 1 - 2 * (i + .5) / 320., phi = i * 2.3999632297;
        Vec v(std::sqrt(1 - z * z) * std::cos(phi), std::sqrt(1 - z * z) * std::sin(phi), z);
        double score = 0;
        for (const auto &n : directions)
            score += std::pow(v.dot(n) - target, 2);
        if (score < bestScore) {
            bestScore = score;
            best = v;
        }
    }
    return best;
}
void completeHydrogens(Document &d, const Ids &selection) {
    Ids ids = selection.isEmpty() ? d.ids() : selection;
    const auto doubles = aromaticDoubleBonds(d);
    for (const auto &id : ids) {
        if (!d.contains(id))
            continue;
        Atom a = d.atom(id);
        if (a.ghost || a.element == "H")
            continue;
        const double used = valenceUsed(d, id, doubles);
        int expected = valence(a, used);
        if (expected < 0)
            continue;
        int difference = int(std::floor(expected - used + .1));
        if (difference < 0) {
            Ids removable;
            for (const auto &n : d.neighbors(id, false)) {
                const auto &h = d.atom(n);
                if (h.element == "H" && !h.fixed && !h.ghost && !h.isotope && h.mass == 0 && h.charge == 0 &&
                    h.radical == 0 && d.neighbors(n).size() == 1)
                    removable << n;
            }
            while (difference < 0 && !removable.isEmpty()) {
                d.remove({removable.takeLast()});
                ++difference;
            }
        }
        if (difference < 0)
            fail("原子価を超えています: " + a.element + QString::number(d.index(id) + 1));
        for (int i = 0; i < difference; ++i) {
            Atom h;
            h.element = "H";
            h.automaticH = true;
            h.position = d.atom(id).position + freeDirection(d, id) * (element(a.element).radius + .31);
            d.atoms << h;
            d.setBond(id, h.id, "single");
            auto residues = d.data["residues"].toArray();
            for (int j = 0; j < residues.size(); ++j) {
                auto r = residues[j].toObject(), names = r["atoms"].toObject();
                bool found = false;
                for (auto v : names)
                    if (v.toString() == id)
                        found = true;
                if (found) {
                    QString name = "H" + a.name + QString::number(i + 1);
                    int k = 1;
                    while (names.contains(name))
                        name = "H" + a.name + QString::number(++k);
                    d.atom(h.id).name = name;
                    names[name] = h.id;
                    r["atoms"] = names;
                    residues[j] = r;
                }
            }
            d.data["residues"] = residues;
            auto parts = d.data["parts"].toArray();
            for (int j = 0; j < parts.size(); ++j) {
                auto p = parts[j].toObject();
                auto list = idsFromJson(p["atoms"]);
                if (list.contains(id)) {
                    list << h.id;
                    p["atoms"] = idsToJson(list);
                    parts[j] = p;
                    break;
                }
            }
            d.data["parts"] = parts;
        }
    }
    d.invalidateAssignments();
}
QString addAtom(Document &d, const QString &symbol, const Vec &position, const QString &anchor,
                const QString &kind, bool hydrogen) {
    element(symbol);
    QString parent = anchor;
    Vec p = position;
    if (!parent.isEmpty()) {
        if (d.atom(parent).element == "H") {
            auto n = d.neighbors(parent);
            if (n.size() != 1)
                fail("結合している水素を選択してください。");
            p = d.atom(parent).position;
            QString h = parent;
            parent = n.first();
            Vec delta = unit(p - d.atom(parent).position);
            d.remove({h});
            p = d.atom(parent).position +
                delta * (element(symbol).radius + element(d.atom(parent).element).radius);
        } else {
            auto ns = d.neighbors(parent);
            QString h;
            for (const auto &n : ns)
                if (d.atom(n).element == "H" && !d.atom(n).fixed && !d.atom(n).isotope) {
                    h = n;
                    break;
                }
            Vec direction =
                h.isEmpty() ? freeDirection(d, parent) : unit(d.atom(h).position - d.atom(parent).position);
            if (!h.isEmpty())
                d.remove({h});
            p = d.atom(parent).position +
                direction * (element(symbol).radius + element(d.atom(parent).element).radius);
        }
    }
    Atom a;
    a.element = symbol;
    a.position = d.atoms.isEmpty() ? Vec::Zero() : p;
    d.atoms << a;
    if (!parent.isEmpty())
        d.setBond(parent, a.id, kind);
    if (hydrogen)
        completeHydrogens(d, parent.isEmpty() ? Ids{a.id} : Ids{parent, a.id});
    d.invalidateAssignments();
    return a.id;
}
void replaceElement(Document &d, const Ids &ids, const QString &symbol, bool hydrogen) {
    element(symbol);
    if (ids.isEmpty())
        fail("原子を選択してください。");
    for (const auto &id : ids) {
        auto &a = d.atom(id);
        a.element = symbol;
        a.isotope = 0;
        a.mass = 0;
        a.stereo.clear();
    }
    if (hydrogen)
        completeHydrogens(d, ids);
    d.invalidateAssignments();
}
PlacedPart attachPart(Document &d, const QString &anchor, const QJsonObject &t, double twist) {
    QString leaving = anchor;
    if (d.atom(anchor).element == "H") {
        auto ns = d.neighbors(anchor);
        if (ns.size() != 1)
            fail("置換する末端水素を選択してください。");
    } else {
        Ids hs;
        for (const auto &n : d.neighbors(anchor))
            if (d.atom(n).element == "H")
                hs << n;
        if (hs.size() != 1)
            fail("置換する水素を1個指定してください。");
        leaving = hs.first();
    }
    return replacePart(d, {leaving}, t, twist);
}
PlacedPart replacePart(Document &d, Ids ids, const QJsonObject &t, double twist) {
    if (ids.isEmpty())
        fail("置換する原子または部品を選択してください。");
    if (ids.size() == 1 && d.atom(ids.first()).element != "H")
        for (auto id : d.neighbors(ids.first()))
            if (d.atom(id).element == "H")
                ids << id;
    QList<Bond> boundary;
    for (const auto &bond : d.bonds)
        if (ids.contains(bond.a) != ids.contains(bond.b))
            boundary << bond;
    if (boundary.size() != 1)
        fail("部品置換は外部への結合が1本の範囲に対応します。残基・配位子は専用操作を使ってください。");
    const auto bond = boundary.first();
    const auto parent = ids.contains(bond.a) ? bond.b : bond.a;
    const auto leaving = ids.contains(bond.a) ? bond.a : bond.b;
    auto connections = t["connections"].toArray();
    if (connections.size() != 1)
        fail("置換基には接続点が1個必要です。");
    int donor = connections.first().toInt();
    QVector<Vec> points;
    for (auto a : t["atoms"].toArray())
        points << vectorFromJson(a.toObject()["position"]);
    Vec direction = unit(d.atom(leaving).position - d.atom(parent).position), source = Vec::UnitX();
    QSet<int> omit;
    if (t["leaving"].isDouble()) {
        int l = t["leaving"].toInt();
        source = points[donor] - points[l];
        omit.insert(l);
    } else {
        Vec center = Vec::Zero();
        for (auto p : points)
            center += p;
        center /= points.size();
        if ((center - points[donor]).norm() > 1e-8)
            source = center - points[donor];
    }
    QString symbol = t["atoms"].toArray()[donor].toObject()["element"].toString();
    Vec target = d.atom(parent).position +
                 direction * (element(symbol).radius + element(d.atom(parent).element).radius);
    Mat r = rotation(direction, twist) * alignVectors(source, direction);
    Vec start = points[donor];
    for (auto &p : points)
        p = target + r * (p - start);
    // Sample rotations about the attachment bond; only the new part moves.
    auto best = points;
    double bestScore = std::numeric_limits<double>::max();
    for (int angle = 0; angle < 360; angle += 30) {
        auto candidate = points;
        double score = 0;
        const auto turn = rotation(direction, angle);
        for (int i = 0; i < candidate.size(); ++i) {
            candidate[i] = target + turn * (points[i] - target);
            if (omit.contains(i))
                continue;
            double radius = element(t["atoms"].toArray()[i].toObject()["element"].toString()).radius;
            for (const auto &old : d.atoms) {
                if (ids.contains(old.id) || old.id == parent)
                    continue;
                double cutoff = .65 * (radius + element(old.element).radius);
                score += std::pow(std::max(0., cutoff - (candidate[i] - old.position).norm()), 2);
            }
        }
        if (score < bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    const auto previousOrder = d.ids();
    d.remove(ids);
    auto p = instantiate(d, t, best, omit);
    d.setBond(parent, p.mapping[donor], bond.kind);
    Ids replacements = p.atoms;
    replacements.removeAll(p.mapping[donor]);
    replacements.prepend(p.mapping[donor]);
    QVector<Atom> ordered;
    for (auto id : previousOrder) {
        if (ids.contains(id)) {
            if (!replacements.isEmpty())
                ordered << d.atom(replacements.takeFirst());
        } else
            ordered << d.atom(id);
    }
    for (auto id : replacements)
        ordered << d.atom(id);
    d.atoms = ordered;
    if (omit.isEmpty())
        d.atom(p.mapping[donor]).radical = std::max(0, d.atom(p.mapping[donor]).radical - int(bond.order()));
    return p;
}
Ids duplicate(Document &d, const Ids &ids, const Vec &offset) {
    if (ids.isEmpty())
        fail("複製する原子を選択してください。");
    if (offset.norm() < 1e-6)
        fail("複製の間隔は0以外にしてください。");
    auto old = d;
    QMap<QString, QString> mapping;
    Ids added;
    for (auto id : ids) {
        auto a = old.atom(id);
        a.id = newId();
        a.position += offset;
        a.fixed = false;
        mapping[id] = a.id;
        added << a.id;
        d.atoms << a;
    }
    QMap<QString, QJsonArray> selectedRows;
    for (auto key : {"parts", "residues"})
        for (auto v : old.data[key].toArray()) {
            auto o = v.toObject();
            Ids members;
            if (QString(key) == "parts")
                members = idsFromJson(o["atoms"]);
            else
                for (auto id : o["atoms"].toObject())
                    members << id.toString();
            bool complete = !members.isEmpty();
            for (auto id : members)
                complete &= mapping.contains(id);
            if (!complete)
                continue;
            mapping[o["id"].toString()] = newId();
            if (QString(key) == "residues" && !mapping.contains(o["chain"].toString()))
                mapping[o["chain"].toString()] = newId();
            selectedRows[key].append(o);
        }
    std::function<QJsonValue(QJsonValue)> remap = [&](QJsonValue v) -> QJsonValue {
        if (v.isString())
            return mapping.contains(v.toString()) ? QJsonValue(mapping[v.toString()]) : v;
        if (v.isArray()) {
            QJsonArray out;
            for (auto a : v.toArray())
                out.append(remap(a));
            return out;
        }
        if (v.isObject()) {
            auto out = v.toObject();
            for (auto it = out.begin(); it != out.end(); ++it)
                it.value() = remap(it.value());
            return out;
        }
        return v;
    };
    for (auto b : old.bonds)
        if (mapping.contains(b.a) && mapping.contains(b.b)) {
            b.a = mapping[b.a];
            b.b = mapping[b.b];
            d.bonds << b;
        }
    for (auto key : {"parts", "residues"}) {
        auto out = d.data[key].toArray();
        for (auto v : selectedRows[key]) {
            auto o = remap(v).toObject();
            if (QString(key) == "parts") {
                auto meta = o["metadata"].toObject();
                auto original = v.toObject()["metadata"].toObject()["coordination_center"].toString();
                if (!original.isEmpty() && !mapping.contains(original)) {
                    meta.remove("coordination_center");
                    meta.remove("sites");
                }
                o["metadata"] = meta;
            }
            out.append(o);
        }
        d.data[key] = out;
    }
    auto chains = d.data["chains"].toObject();
    auto oldChains = old.data["chains"].toObject();
    for (auto it = oldChains.begin(); it != oldChains.end(); ++it)
        if (mapping.contains(it.key())) {
            QJsonArray seq;
            for (auto id : idsFromJson(it.value()))
                if (mapping.contains(id))
                    seq.append(mapping[id]);
            chains[mapping[it.key()]] = seq;
        }
    d.data["chains"] = chains;
    auto centers = d.data["centers"].toArray();
    for (auto v : old.data["centers"].toArray()) {
        auto c = v.toObject();
        if (!mapping.contains(c["atom"].toString()))
            continue;
        QJsonArray sites;
        for (auto x : c["sites"].toArray()) {
            auto site = x.toObject();
            if (!mapping.contains(site["donor"].toString())) {
                site.remove("donor");
                site.remove("part");
            }
            sites.append(remap(site));
        }
        c = remap(c).toObject();
        c["sites"] = sites;
        centers.append(c);
    }
    d.data["centers"] = centers;
    d.invalidateAssignments();
    return added;
}
void mirror(Document &d, const Ids &ids, int axis, const Vec &origin) {
    if (ids.isEmpty() || axis < 0 || axis > 2)
        fail("鏡映する分子を選択してください。");
    QSet<QString> selected(ids.begin(), ids.end());
    for (const auto &id : ids)
        for (const auto &n : d.neighbors(id))
            if (!selected.contains(n))
                fail("分子全体を選択して鏡映してください。");
    Mat r = Mat::Identity();
    r(axis, axis) = -1;
    transform(d, ids, r, Vec::Zero(), origin);
    for (const auto &id : ids) {
        auto &a = d.atom(id);
        if (a.stereo == "R")
            a.stereo = "S";
        else if (a.stereo == "S")
            a.stereo = "R";
    }
}
void invertCenter(Document &d, const QString &id) {
    auto ns = d.neighbors(id, false);
    if (ns.size() != 4)
        fail("4配位の不斉中心を選択してください。");
    auto a = d.bondSide(id, ns[0]), b = d.bondSide(id, ns[1]);
    for (auto x : a)
        if (b.contains(x))
            fail("環で接続した置換基は反転できません。");
    Vec o = d.atom(id).position, u = d.atom(ns[0]).position - o, v = d.atom(ns[1]).position - o;
    transform(d, a, alignVectors(u, v), Vec::Zero(), o);
    transform(d, b, alignVectors(v, u), Vec::Zero(), o);
    auto &c = d.atom(id);
    c.stereo = c.stereo == "R" ? "S" : c.stereo == "S" ? "R" : "";
}
void switchEZ(Document &d, const Ids &ids) {
    if (ids.size() != 2 || !d.bond(ids[0], ids[1]) || d.bond(ids[0], ids[1])->kind != "double")
        fail("二重結合の両端を選択してください。");
    Vec a = d.atom(ids[0]).position, b = d.atom(ids[1]).position;
    transform(d, d.bondSide(ids[0], ids[1]), rotation(b - a, 180), Vec::Zero(), b);
    for (auto &bond : d.bonds)
        if ((bond.a == ids[0] && bond.b == ids[1]) || (bond.b == ids[0] && bond.a == ids[1]))
            bond.stereo = bond.stereo == "E" ? "Z" : bond.stereo == "Z" ? "E" : "";
}
void assignFragments(Document &d, const QString &target, const QList<int> &members, const QList<int> &charges,
                     const QList<int> &mult, const QList<QPair<int, int>> &cuts) {
    if (members.size() != d.atoms.size() || members.isEmpty())
        fail("全原子分の断片番号が必要です。");
    int count = *std::max_element(members.begin(), members.end());
    if (count < 1 || count > members.size() || charges.size() != count || mult.size() != count)
        fail("断片数と電荷・多重度の個数を合わせてください。");
    QJsonObject mm, qq, ss;
    QSet<int> seen;
    for (int i = 0; i < members.size(); ++i) {
        if (members[i] < 1)
            fail("断片番号は1から連番にしてください。");
        seen.insert(members[i]);
        mm[d.atoms[i].id] = QString::number(members[i]);
    }
    if (seen.size() != count)
        fail("断片番号に欠番があります。");
    int total = 0;
    for (int i = 0; i < count; ++i) {
        if (mult[i] < 1)
            fail("多重度は1以上にしてください。");
        qq[QString::number(i + 1)] = charges[i];
        ss[QString::number(i + 1)] = mult[i];
        total += charges[i];
    }
    QJsonArray cc;
    for (auto pair : cuts) {
        if (pair.first < 1 || pair.second < 1 || pair.first > d.atoms.size() ||
            pair.second > d.atoms.size() || members[pair.first - 1] == members[pair.second - 1])
            fail("切断位置が不正です。");
        auto a = d.atoms[pair.first - 1].id, b = d.atoms[pair.second - 1].id;
        if (!d.bond(a, b))
            fail("切断位置に結合がありません。");
        cc.append(QJsonArray{a, b});
    }
    for (const auto &b : d.bonds)
        if (members[d.index(b.a)] != members[d.index(b.b)] && b.kind != "dative") {
            bool present = false;
            for (auto c : cc) {
                auto pair = c.toArray();
                if ((pair[0] == b.a && pair[1] == b.b) || (pair[1] == b.a && pair[0] == b.b))
                    present = true;
            }
            if (!present)
                fail("断片間の共有結合にはBDA:BAAの指定が必要です。");
        }
    if (target == "sapt0" && count != 2)
        fail("SAPT0は2断片にしてください。");
    if (target != "fmo" && !cuts.isEmpty())
        fail("共有結合の切断はFMOのみ対応しています。");
    if (target == "sapt0")
        for (int m : mult)
            if (m != 1)
                fail("SAPT0の断片は閉殻一重項にしてください。");
    auto assignments = d.data["assignments"].toObject();
    assignments[target] = QJsonObject{{"members", mm}, {"charges", qq},     {"multiplicities", ss},
                                      {"cuts", cc},    {"status", "valid"}, {"issues", QJsonArray()}};
    d.data["assignments"] = assignments;
    d.setJobValue("charge", total);
    if (target == "fmo")
        d.setJobValue("method", "fmo");
    else
        d.setJobValue("runtype", target);
}
void proposeFragments(Document &d, const QString &target, const QString &mode) {
    if (d.atoms.isEmpty())
        fail("分子が空です。");
    QList<QPair<int, int>> cuts;
    QSet<QString> blocked;
    auto pairKey = [](QString a, QString b) { return a < b ? a + b : b + a; };
    if (mode != "component") {
        int batch = mode == "residue2" ? 2 : 1;
        QMap<QString, QJsonObject> residues;
        QSet<QString> covered;
        for (auto v : d.data["residues"].toArray()) {
            auto r = v.toObject();
            residues[r["id"].toString()] = r;
            for (auto id : r["atoms"].toObject())
                covered.insert(id.toString());
        }
        if (covered.size() != d.atoms.size())
            fail("未登録の原子があります。残基を確認するか手動指定してください。");
        for (auto sequence : d.data["chains"].toObject()) {
            Ids chain;
            for (auto id : idsFromJson(sequence))
                if (residues[id]["code"] != "ACE" && residues[id]["code"] != "NME")
                    chain << id;
            for (int i = 0; i + 1 < chain.size(); ++i)
                if ((i + 1) % batch == 0) {
                    auto names = residues[chain[i]]["atoms"].toObject();
                    QString a = names["CA"].toString(), b = names["C"].toString();
                    if (!d.contains(a) || !d.contains(b) || !d.bond(a, b) || d.bond(a, b)->kind != "single")
                        fail("Cα–C(=O)の単結合が必要です。");
                    blocked.insert(pairKey(a, b));
                    cuts.append({d.index(a) + 1, d.index(b) + 1});
                }
        }
    }
    QMap<QString, int> groups;
    int n = 0;
    for (const auto &a : d.atoms)
        if (!groups.contains(a.id)) {
            ++n;
            Ids todo{a.id};
            while (!todo.isEmpty()) {
                auto id = todo.takeFirst();
                if (groups.contains(id))
                    continue;
                groups[id] = n;
                for (auto other : d.neighbors(id))
                    if (!blocked.contains(pairKey(id, other)))
                        todo << other;
            }
        }
    QList<int> members, charges(n, 0), mult(n, 1);
    for (const auto &a : d.atoms) {
        members << groups[a.id];
        if (!a.ghost)
            charges[groups[a.id] - 1] += a.charge;
    }
    for (auto pair : cuts)
        if (groups[d.atoms[pair.first - 1].id] == groups[d.atoms[pair.second - 1].id])
            fail("架橋または環が切断を迂回します。分割単位を変更してください。");
    assignFragments(d, target, members, charges, mult, cuts);
}
void mergeIonContacts(Document &d) {
    auto assignments = d.data["assignments"].toObject();
    auto assignment = assignments["fmo"].toObject();
    auto members = assignment["members"].toObject(), charges = assignment["charges"].toObject(),
         spins = assignment["multiplicities"].toObject();
    for (const auto &a : d.atoms)
        if (!members.contains(a.id))
            fail("先にFMOの断片割当を作成・確定してください。");
    static const QStringList metals =
        QString(
            "Li Be Na Mg Al K Ca Sc Ti V Cr Mn Fe Co Ni Cu Zn Ga Rb Sr Y Zr Nb Mo Tc Ru Rh Pd Ag Cd In Sn "
            "Cs Ba La Ce Pr Nd Pm Sm Eu Gd Tb Dy Ho Er Tm Yb Lu Hf Ta W Re Os Ir Pt Au Hg Tl Pb Bi Po")
            .split(' ');
    QStringList order;
    for (const auto &a : d.atoms)
        if (!order.contains(members[a.id].toString()))
            order << members[a.id].toString();
    bool merged = false;
    for (int i = 0; i < d.atoms.size(); ++i) {
        const auto &a = d.atoms[i];
        if (a.ghost || !metals.contains(a.element))
            continue;
        for (const auto &b : d.atoms) {
            auto left = members[a.id].toString(), right = members[b.id].toString();
            if (b.ghost || left == right)
                continue;
            double cutoff = 1.25 * (element(a.element).radius + element(b.element).radius) + .40;
            if ((a.position - b.position).norm() > cutoff)
                continue;
            bool cut = false;
            for (auto v : assignment["cuts"].toArray()) {
                const auto pair = idsFromJson(v);
                if (pair.contains(a.id) && pair.contains(b.id))
                    cut = true;
            }
            if (cut)
                continue;
            if (spins[left].toInt(1) != 1 || spins[right].toInt(1) != 1)
                fail("開殻断片の統合にはスピン結合の確認が必要です。手動で割り当ててください。");
            if (order.indexOf(left) > order.indexOf(right))
                std::swap(left, right);
            for (auto it = members.begin(); it != members.end(); ++it)
                if (it.value() == right)
                    it.value() = left;
            charges[left] = charges[left].toInt() + charges[right].toInt();
            charges.remove(right);
            spins.remove(right);
            merged = true;
        }
    }
    if (!merged)
        fail("統合対象となる金属と断片の近接はありません。");
    QJsonArray cuts;
    for (auto value : assignment["cuts"].toArray()) {
        auto pair = idsFromJson(value);
        if (pair.size() == 2 && members[pair[0]] != members[pair[1]])
            cuts.append(value);
    }
    assignment["members"] = members;
    assignment["charges"] = charges;
    assignment["multiplicities"] = spins;
    assignment["cuts"] = cuts;
    assignment["status"] = "candidate";
    assignment["issues"] =
        charges.size() == 1
            ? QJsonArray{"統合後は1断片です。FMO計算には使えないため、通常のHF/DFTとして設定してください。"}
            : QJsonArray{"金属との近接から断片を統合しました。電荷・多重度を確認してください。"};
    assignments["fmo"] = assignment;
    d.data["assignments"] = assignments;
}
void waterToCharges(Document &d, const Ids &ids) {
    if (ids.size() != 3)
        fail("水分子のOとH2個を選択してください。");
    int oxygen = 0, hydrogen = 0;
    QString oid;
    for (const auto &id : ids) {
        auto e = d.atom(id).element;
        if (e == "O") {
            ++oxygen;
            oid = id;
        }
        if (e == "H")
            ++hydrogen;
    }
    if (oxygen != 1 || hydrogen != 2 || d.component(oid).size() != 3)
        fail("独立したH2O分子を選択してください。");
    auto charges = d.data["point_charges"].toArray();
    for (const auto &id : ids) {
        const auto &a = d.atom(id);
        charges.append(QJsonObject{{"id", newId()},
                                   {"position", vectorToJson(a.position)},
                                   {"charge", a.element == "O" ? -.834 : .417},
                                   {"source", "TIP3P"},
                                   {"source_unit", "angstrom"}});
    }
    d.remove(ids);
    d.data["point_charges"] = charges;
}
void addReference(Document &d, const Ids &ids, const QString &kind, double value, double end, int steps) {
    if (ids.isEmpty() || (kind != "freeze" && (ids.size() < 2 || ids.size() > 4)))
        fail("凍結は1原子以上、拘束とスキャンは2〜4原子を選択してください。");
    auto job = d.emittedJob(), sections = job["sections"].toObject();
    auto refs = d.data["references"].toArray();
    QString section = kind == "scan" ? "scan" : "constraint";
    QString coordinate = ids.size() == 2 ? "bond" : ids.size() == 3 ? "angle" : "dihedral";
    QString key = kind == "freeze" ? "freeze" : kind == "scan" ? coordinate : "list";
    auto values = sections[section].toObject();
    Ids actual = ids;
    if (kind == "freeze")
        for (auto v : refs) {
            auto r = v.toObject();
            if (r["section"] == section && r["key"] == key)
                for (auto id : idsFromJson(r["ids"]))
                    if (!actual.contains(id))
                        actual.prepend(id);
        }
    QStringList numbers;
    for (auto id : actual) {
        if (!d.contains(id))
            fail("原子参照が不正です。");
        numbers << QString::number(d.index(id) + 1);
    }
    QString atomText = numbers.join(','), text = atomText;
    QJsonValue span;
    if (kind == "constraint") {
        text = values[key].toString();
        if (!text.isEmpty())
            text += "; ";
        int start = text.size() + coordinate.size() + 1;
        text += coordinate + "=" + atomText + " value=" + QString::number(value, 'g', 12) +
                " unit=" + (ids.size() == 2 ? "angstrom" : "degree");
        span = QJsonArray{start, start + atomText.size()};
        // Refresh existing spans against the current atom order before appending.
        QRegularExpression pattern("(?:bond|angle|dihedral)=([0-9,]+)");
        auto matches = pattern.globalMatch(text);
        QVector<QJsonArray> spans;
        while (matches.hasNext()) {
            auto match = matches.next();
            spans << QJsonArray{int(match.capturedStart(1)), int(match.capturedEnd(1))};
        }
        int i = 0;
        for (int j = 0; j < refs.size(); ++j) {
            auto r = refs[j].toObject();
            if (r["section"] == section && r["key"] == key) {
                r["text"] = text;
                if (i < spans.size())
                    r["span"] = spans[i++];
                refs[j] = r;
            }
        }
    } else {
        for (int i = refs.size() - 1; i >= 0; --i) {
            auto r = refs[i].toObject();
            if (r["section"] == section && (kind == "scan" || r["key"] == key))
                refs.removeAt(i);
        }
    }
    refs.append(QJsonObject{
        {"section", section}, {"key", key}, {"text", text}, {"ids", idsToJson(actual)}, {"span", span}});
    if (kind == "scan") {
        for (auto k : {"bond", "angle", "dihedral"})
            values.remove(k);
        values["start"] = QString::number(value, 'g', 12);
        values["stop"] = QString::number(end, 'g', 12);
        values["points"] = QString::number(steps + 1);
        values["unit"] = ids.size() == 2 ? "angstrom" : "degree";
    }
    values[key] = text;
    sections[section] = values;
    job["sections"] = sections;
    job["runtype"] = "optimize";
    d.data["job"] = job;
    d.data["references"] = refs;
}
Ids insertFragment(Document &d, const Document &fragment) {
    if (fragment.atoms.isEmpty())
        fail("追加する断片に原子がありません。");
    auto errors = fragment.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    const bool chargeFollows = d.job()["charge"].toInt() == d.formalCharge();
    Vec offset = Vec::Zero();
    if (!d.atoms.isEmpty()) {
        double right = -INFINITY, left = INFINITY;
        Vec center = Vec::Zero(), own = Vec::Zero();
        for (const auto &a : d.atoms) {
            right = std::max(right, a.position.x());
            center += a.position;
        }
        for (const auto &a : fragment.atoms) {
            left = std::min(left, a.position.x());
            own += a.position;
        }
        center /= d.atoms.size();
        own /= fragment.atoms.size();
        offset = Vec(right + 3 - left, center.y() - own.y(), center.z() - own.z());
    }
    Ids added;
    for (auto a : fragment.atoms) {
        if (d.contains(a.id))
            fail("断片の原子IDが重複しています。");
        a.position += offset;
        d.atoms << a;
        added << a.id;
    }
    d.bonds += fragment.bonds;
    for (auto key : {"parts", "residues", "centers"}) {
        auto list = d.data[key].toArray();
        for (const auto &v : fragment.data[key].toArray())
            list.append(v);
        d.data[key] = list;
    }
    auto chains = d.data["chains"].toObject();
    const auto more = fragment.data["chains"].toObject();
    for (auto it = more.begin(); it != more.end(); ++it)
        chains[it.key()] = it.value();
    d.data["chains"] = chains;
    if (chargeFollows)
        d.setJobValue("charge", d.formalCharge());
    d.invalidateAssignments();
    return added;
}
QString structureReport(const Document &d) {
    QStringList lines = d.validate();
    int electrons = -d.job()["charge"].toInt();
    for (const auto &a : d.atoms)
        if (!a.ghost)
            electrons += element(a.element).number;
    int mult = d.job()["multiplicity"].toInt(1);
    lines << QString("%1 原子 / %2 結合 / %3\n形式電荷合計: %4 / 設定電荷: %5 / 多重度: %6")
                 .arg(d.atoms.size())
                 .arg(d.bonds.size())
                 .arg(d.formula())
                 .arg(d.formalCharge())
                 .arg(d.job()["charge"].toInt())
                 .arg(mult);
    if (electrons < 0 || mult < 1 || mult > electrons + 1 || (electrons + mult) % 2 != 1)
        lines << "電子数と多重度の組合せを確認してください。";
    if (d.formalCharge() != d.job()["charge"].toInt())
        lines << "形式電荷の合計と設定電荷が異なります。";
    if (!d.data["invalid_settings"].toArray().isEmpty())
        lines << "削除された原子を参照する保留設定があります。";
    for (auto a : d.data["assignments"].toObject())
        for (auto issue : a.toObject()["issues"].toArray())
            lines << issue.toString();
    const auto doubles = aromaticDoubleBonds(d);
    for (const auto &a : d.atoms) {
        const double used = valenceUsed(d, a.id, doubles);
        int expected = valence(a, used);
        if (!a.ghost && expected >= 0 && std::abs(used - expected) > .1)
            lines << QString("%1%2: 結合次数合計 %3（目安 %4）")
                         .arg(a.element)
                         .arg(d.index(a.id) + 1)
                         .arg(used)
                         .arg(expected);
    }
    // Coordination centers: free sites, and the total charge their oxidation numbers suggest.
    int expected = 0;
    QSet<QString> centers;
    for (const auto &v : d.data["centers"].toArray()) {
        const auto c = v.toObject();
        const auto id = c["atom"].toString();
        if (!d.contains(id))
            continue;
        centers.insert(id);
        expected += c["oxidation"].toInt();
        const auto sites = c["sites"].toArray();
        const auto open = std::count_if(sites.begin(), sites.end(), [](const QJsonValue &s) {
            return s.toObject()["donor"].toString().isEmpty();
        });
        if (open)
            lines << QString("%1%2: 配位サイト %3 のうち %4 "
                             "が空いています（配位子を置くか、配位形状を変えてください）。")
                         .arg(d.atom(id).element)
                         .arg(d.index(id) + 1)
                         .arg(sites.size())
                         .arg(open);
    }
    if (!centers.isEmpty()) {
        for (const auto &a : d.atoms)
            if (!centers.contains(a.id))
                expected += a.charge;
        if (expected != d.job()["charge"].toInt())
            lines << QString("配位中心の酸化数（補助）と配位子の形式電荷から見込まれる全電荷は %1 ですが、"
                             "設定電荷は %2 です。")
                         .arg(expected)
                         .arg(d.job()["charge"].toInt());
    }
    // Besides the summary line, every line is a finding.
    if (lines.size() == 1)
        lines << "問題は見つかりませんでした。";
    return lines.join('\n');
}
} // namespace mf
