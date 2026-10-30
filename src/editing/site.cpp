// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/site.h"
#include "editing/operations.h"
#include "editing/peptide.h"
#include "model/i18n.h"
#include "model/topology.h"

namespace mf {
namespace {
bool metal(const QString &symbol) {
    static const QSet<QString> nonmetals{"H",  "He", "B",  "C",  "N",  "O",  "F",  "Ne", "Si", "P",  "S",
                                         "Cl", "Ar", "As", "Se", "Br", "Kr", "Te", "I",  "Xe", "At", "Rn"};
    return !nonmetals.contains(symbol);
}
} // namespace
Ids siteAtoms(const Document &d, const Ids &center, const SiteOptions &options) {
    const auto centerAtoms = d.indices(center);
    if (centerAtoms.isEmpty())
        fail(tr("中心にする原子（配位子や残基）を選択してください。"));
    const Topology topology(d);
    const int n = d.atoms.size();
    // The units that come whole: residues, and the molecules of the atoms outside residues.
    QVector<int> unit(n, -1);
    QVector<QVector<int>> units;
    const auto residues = atomResidues(d);
    QHash<QString, int> residueUnit;
    for (int i = 0; i < n; ++i)
        if (const auto residue = residues.value(d.atoms[i].id); !residue.isEmpty()) {
            auto it = residueUnit.find(residue);
            if (it == residueUnit.end()) {
                it = residueUnit.insert(residue, units.size());
                units.emplaceBack();
            }
            unit[i] = *it;
            units[*it] << i;
        }
    for (int i = 0; i < n; ++i)
        if (unit[i] < 0) {
            unit[i] = units.size();
            QVector<int> molecule{i};
            for (int k = 0; k < molecule.size(); ++k)
                for (int m : topology.neighbors(molecule[k]))
                    if (unit[m] < 0) {
                        unit[m] = unit[i];
                        molecule << m;
                    }
            units << molecule;
        }
    const auto water = [&](int u) {
        int oxygens = 0;
        for (int i : units[u])
            if (d.atoms[i].element == "O")
                ++oxygens;
            else if (d.atoms[i].element != "H")
                return false;
        return oxygens == 1 && units[u].size() <= 3;
    };
    QVector<bool> chosen(n), taken(units.size());
    for (int c : centerAtoms)
        chosen[c] = true;
    const double limit = options.radius * options.radius;
    for (int i = 0; i < n; ++i) {
        if (chosen[i] || (!options.waters && water(unit[i])))
            continue;
        const auto &position = d.atoms[i].position;
        if (std::none_of(centerAtoms.begin(), centerAtoms.end(),
                         [&](int c) { return (position - d.atoms[c].position).squaredNorm() <= limit; }))
            continue;
        chosen[i] = true;
        if (options.wholeResidues)
            taken[unit[i]] = true;
    }
    if (options.wholeResidues) {
        for (int c : centerAtoms)
            taken[unit[c]] = true;
        for (int i = 0; i < n; ++i)
            chosen[i] = chosen[i] || taken[unit[i]];
    }
    // A hydrogen goes with its atom: none is left without it, nor left behind by it.
    for (int i = 0; i < n; ++i)
        if (d.atoms[i].element == "H")
            for (int m : topology.neighbors(i))
                if (d.atoms[m].element != "H") {
                    chosen[i] = chosen[m];
                    break;
                }
    Ids out;
    for (int i = 0; i < n; ++i)
        if (chosen[i])
            out << d.atoms[i].id;
    return out;
}
Ids cutOutSite(Document &d, const Ids &kept, bool fixCuts) {
    if (kept.isEmpty())
        fail(tr("切り出す原子を選択してください。"));
    const Topology topology(d);
    QVector<bool> keep(d.atoms.size());
    for (int i : d.indices(kept))
        keep[i] = true;
    struct Link {
        QString atom;
        Vec position;
    };
    QVector<Link> links;
    for (const auto &bond : d.bonds) {
        const int a = topology.index(bond.a), b = topology.index(bond.b);
        if (a < 0 || b < 0 || keep[a] == keep[b])
            continue;
        const auto &inside = d.atoms[keep[a] ? a : b], &outside = d.atoms[keep[a] ? b : a];
        if (bond.kind == "dative" || inside.element == "H" || inside.ghost || metal(inside.element) ||
            metal(outside.element))
            continue;
        const Vec along = (outside.position - inside.position).normalized();
        links.append(
            {inside.id, inside.position + along * (element(inside.element).radius + element("H").radius)});
    }
    Ids removed, added;
    for (int i = 0; i < d.atoms.size(); ++i)
        if (!keep[i])
            removed << d.atoms[i].id;
    followingCharge(d, [&] {
        d.remove(removed);
        for (const auto &link : links) {
            Atom hydrogen;
            hydrogen.element = "H";
            hydrogen.position = link.position;
            Bond bond;
            bond.a = link.atom;
            bond.b = hydrogen.id;
            d.atoms << hydrogen;
            d.bonds << bond;
            added << hydrogen.id;
        }
        if (fixCuts) {
            const Topology cut(d);
            for (const auto &link : links)
                cut.atom(d, link.atom).fixed = true;
        }
    });
    return added;
}
} // namespace mf
