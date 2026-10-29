// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/operations.h"
#include "model/i18n.h"
#include "model/topology.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
namespace mf {
void deleteAtoms(Document &d, const Ids &chosen, AtomDeletionOptions options) {
    const Topology topology(d);
    const auto chosenAtoms = d.indices(chosen);
    QVector<int> atoms = chosenAtoms;
    QVector<bool> deleted(d.atoms.size());
    for (int i : atoms)
        deleted[i] = true;
    if (options.includeAttachedHydrogens)
        for (int i : chosenAtoms)
            for (int n : topology.neighbors(i))
                if (d.atoms[n].element == "H" && !deleted[n]) {
                    const auto partners = topology.neighbors(n);
                    if (std::all_of(partners.begin(), partners.end(), [&](int p) { return deleted[p]; })) {
                        deleted[n] = true;
                        atoms << n;
                    }
                }
    Ids ids, cut;
    QVector<bool> capped(d.atoms.size());
    for (int i : atoms) {
        ids << d.atoms[i].id;
        if (options.capCutBonds && d.atoms[i].element != "H")
            for (int n : topology.neighbors(i))
                if (!deleted[n] && !capped[n]) {
                    capped[n] = true;
                    cut << d.atoms[n].id;
                }
    }
    followingCharge(d, [&] {
        d.remove(ids);
        // Deleting a hydrogen must not add it back. An empty selection would cap the whole molecule.
        if (options.capCutBonds && !cut.isEmpty())
            completeHydrogens(d, cut);
    });
}
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
template <typename Bonds>
static double valenceUsed(const Document &d, const QString &id, const Bonds &bonds,
                          const QSet<QString> &doubles) {
    double used = doubles.contains(id) ? 1 : 0;
    for (int k : bonds)
        used += d.bonds[k].kind == "aromatic" ? 1 : d.bonds[k].order();
    return used;
}
// The direction that matches tetrahedral/trigonal/linear bond angles (from the highest bond order)
// while avoiding the directions of the bonded atoms.
static Vec freeDirection(const QVector<Vec> &directions, double maxOrder) {
    if (directions.isEmpty())
        return Vec(1, 1, 1).normalized();
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
Vec freeDirection(const Document &d, const QString &id) {
    const Vec origin = d.atom(id).position;
    QVector<Vec> directions;
    for (const auto &n : d.neighbors(id, false))
        directions << unit(d.atom(n).position - origin);
    double maxOrder = 1;
    for (const auto &b : d.bonds)
        if (b.a == id || b.b == id)
            maxOrder = std::max(maxOrder, b.order());
    return freeDirection(directions, maxOrder);
}
void changeBond(Document &d, const QString &a, const QString &b, const QString &kind, bool hydrogen) {
    d.setBond(a, b, kind);
    if (hydrogen)
        completeHydrogens(d, {a, b});
}
void completeHydrogens(Document &d, const Ids &selection) {
    Ids ids = selection.isEmpty() ? d.ids() : selection;
    const auto doubles = aromaticDoubleBonds(d);
    // The atom indices, the bonds of each atom, and the residues and part of each atom: kept current
    // as hydrogens are added at the end, and rebuilt after one is removed.
    QHash<QString, int> index;
    QVector<QVector<int>> bondsOf;
    QHash<QString, QVector<int>> residuesOf;
    QHash<QString, int> partOf;
    const auto rebuild = [&] {
        index.clear();
        for (int i = 0; i < d.atoms.size(); ++i)
            if (!index.contains(d.atoms[i].id))
                index.insert(d.atoms[i].id, i);
        bondsOf = QVector<QVector<int>>(d.atoms.size());
        for (int k = 0; k < d.bonds.size(); ++k) {
            const int a = index.value(d.bonds[k].a, -1), b = index.value(d.bonds[k].b, -1);
            if (a >= 0 && b >= 0) {
                bondsOf[a] << k;
                bondsOf[b] << k;
            }
        }
        residuesOf.clear();
        for (int r = 0; r < d.residues.size(); ++r)
            for (const auto &atom : std::as_const(d.residues[r].atoms))
                if (auto &list = residuesOf[atom]; list.isEmpty() || list.last() != r)
                    list << r;
        partOf.clear();
        for (int p = d.parts.size() - 1; p >= 0; --p) // The first part of an atom wins.
            for (const auto &atom : std::as_const(d.parts[p].atoms))
                partOf.insert(atom, p);
    };
    const auto other = [&](int k, const QString &id) {
        return index.value(d.bonds[k].a == id ? d.bonds[k].b : d.bonds[k].a, -1);
    };
    rebuild();
    for (const auto &id : ids) {
        if (!index.contains(id))
            continue;
        Atom a = d.atoms[index[id]];
        if (a.ghost || a.element == "H")
            continue;
        const double used = valenceUsed(d, id, bondsOf[index[id]], doubles);
        int expected = valence(a, used);
        if (expected < 0)
            continue;
        int difference = int(std::floor(expected - used + .1));
        if (difference < 0) {
            Ids removable;
            for (int k : bondsOf[index[id]]) {
                const int n = other(k, id);
                if (d.bonds[k].kind == "dative" || n < 0)
                    continue;
                const auto &h = d.atoms[n];
                if (h.element == "H" && !h.fixed && !h.ghost && !h.isotope && h.mass == 0 && h.charge == 0 &&
                    h.radical == 0 && bondsOf[n].size() == 1)
                    removable << h.id;
            }
            while (difference < 0 && !removable.isEmpty()) {
                d.remove({removable.takeLast()});
                ++difference;
                rebuild();
            }
        }
        if (difference < 0)
            fail(tr("原子価を超えています: %1%2").arg(a.element).arg(index[id] + 1));
        for (int i = 0; i < difference; ++i) {
            const int heavy = index[id];
            const Vec origin = d.atoms[heavy].position;
            QVector<Vec> directions;
            double maxOrder = 1;
            for (int k : bondsOf[heavy]) {
                if (const int n = other(k, id); n >= 0 && d.bonds[k].kind != "dative")
                    directions << unit(d.atoms[n].position - origin);
                maxOrder = std::max(maxOrder, d.bonds[k].order());
            }
            Atom h;
            h.element = "H";
            h.automaticH = true;
            h.position = origin + freeDirection(directions, maxOrder) * (element(a.element).radius + .31);
            const int hydrogen = d.atoms.size();
            for (int r : residuesOf.value(id)) {
                auto &residue = d.residues[r];
                QString name = "H" + a.name + QString::number(i + 1);
                int k = 1;
                while (residue.atoms.contains(name))
                    name = "H" + a.name + QString::number(++k);
                h.name = name;
                residue.atoms[name] = h.id;
            }
            if (const int p = partOf.value(id, -1); p >= 0)
                d.parts[p].atoms << h.id;
            d.atoms << h;
            index.insert(h.id, hydrogen);
            // A new atom has no bond yet, so this is what setBond would add.
            d.bonds.append({id, h.id, "single", {}, "explicit"});
            const int bond = d.bonds.size() - 1;
            bondsOf.append(QVector<int>{bond});
            bondsOf[heavy] << bond;
        }
    }
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
                fail(tr("結合している水素を選択してください。"));
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
    return a.id;
}
QString insertAtom(Document &d, QString a, QString b, const QString &symbol, bool hydrogen) {
    element(symbol);
    const Topology topology(d);
    const int ia = topology.index(a), ib = topology.index(b);
    if (!d.bond(a, b) || ia < 0 || ib < 0)
        fail(tr("結合の両端を選択してください。"));
    auto sideA = topology.component(ia, ia, ib), sideB = topology.component(ib, ia, ib);
    const bool ring = sideB.contains(ia);
    const auto movable = [&](const QVector<int> &side) {
        return !ring && std::none_of(side.begin(), side.end(), [&](int i) { return d.atoms[i].fixed; });
    };
    // The smaller side moves out to make room.
    if (movable(sideA) && (!movable(sideB) || sideA.size() < sideB.size())) {
        std::swap(a, b);
        std::swap(sideA, sideB);
    }
    const Vec pa = d.atom(a).position, pb = d.atom(b).position, u = unit(pb - pa);
    // The new atom bends away from the other neighbors of both ends.
    Vec away = Vec::Zero();
    for (const auto &end : {a, b})
        for (const auto &n : d.neighbors(end, false))
            if (n != a && n != b)
                away -= unit(d.atom(n).position - d.atom(end).position);
    Vec w = away - u * away.dot(u);
    if (w.norm() < 1e-6)
        w = u.cross(std::abs(u.z()) < .9 ? Vec::UnitZ() : Vec::UnitX());
    w.normalize();
    const double ra = element(symbol).radius + element(d.atom(a).element).radius,
                 rb = element(symbol).radius + element(d.atom(b).element).radius;
    Vec x;
    if (movable(sideB)) {
        // Bond lengths from the covalent radii and a tetrahedral angle at the new atom.
        const double tilt = (180 - 109.47) / 2 * M_PI / 180;
        x = pa + ra * (std::cos(tilt) * u + std::sin(tilt) * w);
        const Vec shift = x + rb * (std::cos(tilt) * u - std::sin(tilt) * w) - pb;
        for (int i : sideB)
            d.atoms[i].position += shift;
    } else {
        // A ring bond, or fixed atoms on both sides: nothing else moves, and the new atom sits off
        // the bond line (on it the force field cannot open the 180° angle).
        const double half = (pb - pa).norm() / 2, r = (ra + rb) / 2;
        x = (pa + pb) / 2 + w * std::max(.5, std::sqrt(std::max(0., r * r - half * half)));
    }
    d.setBond(a, b, "none");
    Atom atom;
    atom.element = symbol;
    atom.position = x;
    d.atoms << atom;
    d.setBond(a, atom.id, "single");
    d.setBond(atom.id, b, "single");
    if (hydrogen)
        completeHydrogens(d, {a, b, atom.id});
    return atom.id;
}
void replaceElement(Document &d, const Ids &ids, const QString &symbol, bool hydrogen) {
    element(symbol);
    if (ids.isEmpty())
        fail(tr("原子を選択してください。"));
    for (int i : d.indices(ids)) {
        auto &a = d.atoms[i];
        a.element = symbol;
        a.isotope = 0;
        a.mass = 0;
        a.stereo.clear();
    }
    if (hydrogen)
        completeHydrogens(d, ids);
}
PlacedPart attachPart(Document &d, const QString &anchor, const QJsonObject &t, double twist) {
    QString leaving = anchor;
    if (d.atom(anchor).element == "H") {
        auto ns = d.neighbors(anchor);
        if (ns.size() != 1)
            fail(tr("置換する末端水素を選択してください。"));
    } else {
        Ids hs;
        for (const auto &n : d.neighbors(anchor))
            if (d.atom(n).element == "H")
                hs << n;
        if (hs.size() != 1)
            fail(tr("置換する水素を1個指定してください。"));
        leaving = hs.first();
    }
    return replacePart(d, {leaving}, t, twist);
}
PlacedPart replacePart(Document &d, Ids ids, const QJsonObject &t, double twist) {
    if (ids.isEmpty())
        fail(tr("置換する原子または部品を選択してください。"));
    if (ids.size() == 1 && d.atom(ids.first()).element != "H")
        for (auto id : d.neighbors(ids.first()))
            if (d.atom(id).element == "H")
                ids << id;
    QList<Bond> boundary;
    for (const auto &bond : d.bonds)
        if (ids.contains(bond.a) != ids.contains(bond.b))
            boundary << bond;
    if (boundary.size() != 1)
        fail(tr("部品置換は外部への結合が1本の範囲に対応します。残基・配位子は専用操作を使ってください。"));
    const auto bond = boundary.first();
    const auto parent = ids.contains(bond.a) ? bond.b : bond.a;
    const auto leaving = ids.contains(bond.a) ? bond.a : bond.b;
    auto connections = t["connections"].toArray();
    if (connections.size() != 1)
        fail(tr("置換基には接続点が1個必要です。"));
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
    const QSet<QString> replaced(ids.begin(), ids.end());
    Ids order;
    for (const auto &id : previousOrder)
        if (!replaced.contains(id))
            order << id;
        else if (!replacements.isEmpty())
            order << replacements.takeFirst();
    order += replacements;
    QVector<Atom> ordered;
    for (int i : d.indices(order))
        ordered << d.atoms[i];
    d.atoms = ordered;
    if (omit.isEmpty())
        d.atom(p.mapping[donor]).radical = std::max(0, d.atom(p.mapping[donor]).radical - int(bond.order()));
    return p;
}
Ids duplicate(Document &d, const Ids &ids, const Vec &offset) {
    if (ids.isEmpty())
        fail(tr("複製する原子を選択してください。"));
    if (offset.norm() < 1e-6)
        fail(tr("複製の間隔は0以外にしてください。"));
    // The copies, with the residues, parts, chains and coordination centers they hold entirely, take new
    // IDs in every reference (renameIds).
    Document copy;
    copy.data = {};
    QHash<QString, QString> renamed;
    const auto atoms = d.indices(ids);
    for (int i = 0; i < ids.size(); ++i) {
        const auto &id = ids[i];
        auto a = d.atoms[atoms[i]];
        renamed[id] = newId();
        a.position += offset;
        a.fixed = false;
        copy.atoms << a;
    }
    const auto copied = [&](const Ids &members) {
        return !members.isEmpty() && std::all_of(members.begin(), members.end(),
                                                 [&](const QString &id) { return renamed.contains(id); });
    };
    for (const auto &b : d.bonds)
        if (renamed.contains(b.a) && renamed.contains(b.b))
            copy.bonds << b;
    for (auto p : d.parts)
        if (copied(p.atoms)) {
            renamed[p.id] = newId();
            if (!p.center().isEmpty() && !renamed.contains(p.center())) {
                p.metadata.remove("coordination_center");
                p.metadata.remove("sites");
            }
            copy.parts << p;
        }
    for (const auto &r : d.residues)
        if (copied(r.atoms.values())) {
            renamed[r.id] = newId();
            if (!renamed.contains(r.chain))
                renamed[r.chain] = newId();
            copy.residues << r;
        }
    for (auto it = d.chains.begin(); it != d.chains.end(); ++it)
        if (renamed.contains(it.key())) {
            auto sequence = it.value();
            sequence.removeIf([&](const QString &id) { return !renamed.contains(id); });
            copy.chains[it.key()] = sequence;
        }
    for (auto c : d.centers)
        if (renamed.contains(c.atom)) {
            for (auto &site : c.sites)
                if (!renamed.contains(site.donor))
                    site.donor = site.part = QString();
            copy.centers << c;
        }
    copy.renameIds(renamed);
    Ids added = copy.ids();
    d.atoms += copy.atoms;
    d.bonds += copy.bonds;
    d.parts += copy.parts;
    d.residues += copy.residues;
    d.chains.insert(copy.chains);
    d.centers += copy.centers;
    return added;
}
void mirror(Document &d, const Ids &ids, int axis, const Vec &origin) {
    if (ids.isEmpty() || axis < 0 || axis > 2)
        fail(tr("鏡映する分子を選択してください。"));
    const auto atoms = d.indices(ids);
    {
        const Topology topology(d);
        QVector<bool> selected(d.atoms.size());
        for (int i : atoms)
            selected[i] = true;
        for (int i : atoms)
            for (int n : topology.neighbors(i))
                if (!selected[n])
                    fail(tr("分子全体を選択して鏡映してください。"));
    }
    Mat r = Mat::Identity();
    r(axis, axis) = -1;
    transform(d, ids, r, Vec::Zero(), origin);
    for (int i : atoms) {
        auto &a = d.atoms[i];
        if (a.stereo == "R")
            a.stereo = "S";
        else if (a.stereo == "S")
            a.stereo = "R";
    }
}
void invertCenter(Document &d, const QString &id) {
    auto ns = d.neighbors(id, false);
    if (ns.size() != 4)
        fail(tr("4配位の不斉中心を選択してください。"));
    auto a = d.bondSide(id, ns[0]), b = d.bondSide(id, ns[1]);
    for (auto x : a)
        if (b.contains(x))
            fail(tr("環で接続した置換基は反転できません。"));
    Vec o = d.atom(id).position, u = d.atom(ns[0]).position - o, v = d.atom(ns[1]).position - o;
    transform(d, a, alignVectors(u, v), Vec::Zero(), o);
    transform(d, b, alignVectors(v, u), Vec::Zero(), o);
    auto &c = d.atom(id);
    c.stereo = c.stereo == "R" ? "S" : c.stereo == "S" ? "R" : "";
}
void switchEZ(Document &d, const Ids &ids) {
    if (ids.size() != 2 || !d.bond(ids[0], ids[1]) || d.bond(ids[0], ids[1])->kind != "double")
        fail(tr("二重結合の両端を選択してください。"));
    Vec a = d.atom(ids[0]).position, b = d.atom(ids[1]).position;
    transform(d, d.bondSide(ids[0], ids[1]), rotation(b - a, 180), Vec::Zero(), b);
    for (auto &bond : d.bonds)
        if ((bond.a == ids[0] && bond.b == ids[1]) || (bond.b == ids[0] && bond.a == ids[1]))
            bond.stereo = bond.stereo == "E" ? "Z" : bond.stereo == "Z" ? "E" : "";
}
void waterToCharges(Document &d, const Ids &ids) {
    if (ids.size() != 3)
        fail(tr("水分子のOとH2個を選択してください。"));
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
        fail(tr("独立したH2O分子を選択してください。"));
    // O first, as in TIP3P, whatever order the atoms were selected in.
    Ids ordered{oid};
    for (const auto &id : ids)
        if (id != oid)
            ordered.append(id);
    for (const auto &id : ordered) {
        PointCharge q;
        q.position = d.atom(id).position;
        q.charge = d.atom(id).element == "O" ? -.834 : .417;
        q.source = "TIP3P";
        d.pointCharges << q;
    }
    d.remove(ids);
}
Ids insertFragment(Document &d, const Document &fragment) {
    if (fragment.atoms.isEmpty())
        fail(tr("追加する断片に原子がありません。"));
    auto errors = fragment.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
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
    QSet<QString> existing;
    for (const auto &a : d.atoms)
        existing.insert(a.id);
    followingCharge(d, [&] {
        for (auto a : fragment.atoms) {
            if (existing.contains(a.id))
                fail(tr("断片の原子IDが重複しています。"));
            a.position += offset;
            d.atoms << a;
            existing.insert(a.id);
            added << a.id;
        }
        d.bonds += fragment.bonds;
        d.parts += fragment.parts;
        d.residues += fragment.residues;
        d.centers += fragment.centers;
        d.chains.insert(fragment.chains);
    });
    return added;
}
QString addPointCharge(Document &d, const Vec &position, double charge) {
    PointCharge q;
    q.position = position;
    q.charge = charge;
    d.pointCharges << q;
    return q.id;
}
void setPointCharge(Document &d, const QString &id, const Vec &position, double charge) {
    for (auto &q : d.pointCharges)
        if (q.id == id) {
            q.position = position;
            q.charge = charge;
        }
}
void removePointCharge(Document &d, const QString &id) {
    d.pointCharges.removeIf([&](const PointCharge &q) { return q.id == id; });
}
QString structureReport(const Document &d) {
    QStringList lines = d.validate();
    int electrons = -d.charge;
    for (const auto &a : d.atoms)
        if (!a.ghost)
            electrons += element(a.element).number;
    int mult = d.multiplicity;
    lines << tr("%1 原子 / %2 結合 / %3\n形式電荷合計: %4 / 設定電荷: %5 / 多重度: %6")
                 .arg(d.atoms.size())
                 .arg(d.bonds.size())
                 .arg(d.formula())
                 .arg(d.formalCharge())
                 .arg(d.charge)
                 .arg(mult);
    if (electrons < 0 || mult < 1 || mult > electrons + 1 || (electrons + mult) % 2 != 1)
        lines << tr("電子数と多重度の組合せを確認してください。");
    if (!d.hasCenters() && d.formalCharge() != d.charge)
        lines << tr("形式電荷の合計と設定電荷が異なります。");
    const auto doubles = aromaticDoubleBonds(d);
    const Topology topology(d);
    for (int i = 0; i < d.atoms.size(); ++i) {
        const auto &a = d.atoms[i];
        const double used = valenceUsed(d, a.id, topology.bonds(i), doubles);
        int expected = valence(a, used);
        if (!a.ghost && expected >= 0 && std::abs(used - expected) > .1)
            lines << tr("%1%2: 結合次数合計 %3（目安 %4）").arg(a.element).arg(i + 1).arg(used).arg(expected);
    }
    // Coordination centers: free sites, and the total charge their oxidation numbers suggest.
    for (const auto &c : d.centers) {
        const auto &id = c.atom;
        if (!d.contains(id))
            continue;
        const auto &sites = c.sites;
        const auto open = std::count_if(sites.begin(), sites.end(),
                                        [](const CoordinationSite &s) { return s.donor.isEmpty(); });
        if (open)
            lines << tr("%1%2: 配位サイト %3 のうち %n "
                        "が空いています（配位子を置くか、配位形状を変えてください）。",
                        nullptr, int(open))
                         .arg(d.atom(id).element)
                         .arg(d.index(id) + 1)
                         .arg(sites.size());
    }
    if (d.hasCenters() && d.expectedCharge() != d.charge)
        lines << tr("配位中心の酸化数（補助）と配位子の形式電荷から見込まれる全電荷は %1 ですが、"
                    "設定電荷は %2 です。")
                     .arg(d.expectedCharge())
                     .arg(d.charge);
    // Besides the summary line, every line is a finding.
    if (lines.size() == 1)
        lines << tr("問題は見つかりませんでした。");
    return lines.join('\n');
}
} // namespace mf
