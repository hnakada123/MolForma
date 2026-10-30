// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/site.h"
#include "editing/operations.h"
#include "editing/pdb.h"
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
// The ions of the PDB's chemical component dictionary, by their names, with their charges.
const QHash<QString, int> &ions() {
    static const QHash<QString, int> charges{{"ZN", 2},  {"MG", 2},  {"CA", 2},  {"MN", 2},   {"MN3", 3},
                                             {"FE2", 2}, {"FE", 3},  {"CU", 2},  {"CU1", 1},  {"CO", 2},
                                             {"3CO", 3}, {"NI", 2},  {"CD", 2},  {"HG", 2},   {"SR", 2},
                                             {"BA", 2},  {"NA", 1},  {"K", 1},   {"LI", 1},   {"RB", 1},
                                             {"CS", 1},  {"CL", -1}, {"BR", -1}, {"IOD", -1}, {"F", -1}};
    return charges;
}
bool ion(const Residue &r) {
    return r.atoms.size() == 1 && ions().contains(r.code);
}
bool amino(const Residue &r) {
    return r.atoms.contains("N") && r.atoms.contains("CA") && r.atoms.contains("C");
}
// The charged groups of amino acids at pH 7, their charge spread over the atoms that share it.
QVector<std::pair<QString, double>> groupCharges(const QString &code) {
    if (code == "ASP")
        return {{"OD1", -.5}, {"OD2", -.5}};
    if (code == "GLU")
        return {{"OE1", -.5}, {"OE2", -.5}};
    if (code == "LYS")
        return {{"NZ", 1}};
    if (code == "ARG")
        return {{"NE", 1. / 3}, {"NH1", 1. / 3}, {"NH2", 1. / 3}};
    if (code == "HIP")
        return {{"ND1", .5}, {"NE2", .5}};
    if (code == "CYM")
        return {{"SG", -1}};
    if (code == "SO4")
        return {{"O1", -.5}, {"O2", -.5}, {"O3", -.5}, {"O4", -.5}};
    return {};
}
// An end of a chain: an atom bonded only to atoms of its own residue.
bool chainEnd(const Document &d, const Topology &topology, const Residue &r, const QString &atom) {
    const int i = topology.index(atom);
    if (i < 0)
        return false;
    for (int n : topology.neighbors(i))
        if (std::find(r.atoms.begin(), r.atoms.end(), d.atoms[n].id) == r.atoms.end())
            return false;
    return true;
}
// The point charges that the atoms not kept leave (SiteCutOptions::charges).
QVector<PointCharge> surroundings(const Document &d, const Topology &topology, const QVector<bool> &keep,
                                  double radius) {
    QVector<PointCharge> out;
    QSet<QString> registered;
    QVector<Vec> kept;
    for (int i = 0; i < d.atoms.size(); ++i)
        if (keep[i])
            kept << d.atoms[i].position;
    const auto add = [&](const QString &id, double charge) {
        const int i = topology.index(id);
        if (i < 0 || keep[i] || !charge ||
            std::none_of(
                kept.begin(), kept.end(),
                [&](const Vec &p) { return (p - d.atoms[i].position).squaredNorm() <= radius * radius; }))
            return;
        PointCharge q;
        q.position = d.atoms[i].position;
        q.charge = charge;
        q.source = "site";
        out << q;
    };
    for (const auto &r : d.residues) {
        for (const auto &id : r.atoms)
            registered.insert(id);
        if (ion(r)) {
            add(r.atoms.first(), ions()[r.code]);
        } else if (amino(r) || r.code == "SO4") {
            for (const auto &[name, charge] : groupCharges(r.code))
                add(r.atoms.value(name), charge);
            if (!amino(r))
                continue;
            if (chainEnd(d, topology, r, r.atoms["N"]))
                add(r.atoms["N"], 1);
            if (r.atoms.contains("OXT")) {
                add(r.atoms.value("O"), -.5);
                add(r.atoms["OXT"], -.5);
            }
        } else if (!waterResidue(r.code)) {
            for (const auto &id : r.atoms)
                add(id, topology.atom(d, id).charge);
        }
    }
    for (const auto &a : d.atoms)
        if (!registered.contains(a.id))
            add(a.id, a.charge);
    return out;
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
Ids cutOutSite(Document &d, const Ids &kept, const SiteCutOptions &options) {
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
    const auto charges =
        options.charges ? surroundings(d, topology, keep, options.chargeRadius) : QVector<PointCharge>();
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
        if (options.fixCuts) {
            const Topology cut(d);
            for (const auto &link : links)
                cut.atom(d, link.atom).fixed = true;
        }
        d.pointCharges += charges;
    });
    if (options.hydrogens)
        protonate(d);
    return added;
}
void protonate(Document &d) {
    followingCharge(d, [&] {
        const Topology topology(d);
        const auto bondOf = [&](const QString &a, const QString &b) -> Bond * {
            const int i = topology.index(a);
            if (i < 0 || b.isEmpty())
                return nullptr;
            for (int k : topology.bonds(i))
                if (d.bonds[k].a == b || d.bonds[k].b == b)
                    return &d.bonds[k];
            return nullptr;
        };
        const auto setKind = [](Bond *bond, const QString &kind) {
            if (bond && bond->kind != kind) {
                bond->kind = kind;
                bond->stereo.clear();
            }
        };
        const auto boundToMetal = [&](const QString &id) {
            const int i = topology.index(id);
            for (int n : i < 0 ? std::span<const int>() : topology.neighbors(i))
                if (metal(d.atoms[n].element))
                    return true;
            return false;
        };
        // Bonds to metals: dative bonds from the donor, which hydrogens do not count as a valence.
        for (auto &bond : d.bonds) {
            const bool a = metal(topology.atom(d, bond.a).element),
                       b = metal(topology.atom(d, bond.b).element);
            if (a == b || bond.kind == "dative")
                continue;
            if (a)
                std::swap(bond.a, bond.b);
            bond.kind = "dative";
            bond.stereo.clear();
        }
        for (auto &r : d.residues) {
            const auto atom = [&](const char *name) -> Atom * {
                const auto id = r.atoms.value(name);
                return id.isEmpty() ? nullptr : &topology.atom(d, id);
            };
            const auto charge = [&](const char *name, int value) {
                if (auto *a = atom(name))
                    a->charge = value;
            };
            // A carboxylate: the double bond to the oxygen the geometry gave it (else the first), the
            // charge or the hydrogen on the other.
            const auto carboxylate = [&](const char *c, const char *o1, const char *o2, bool charged) {
                auto *first = bondOf(r.atoms.value(c), r.atoms.value(o1)),
                     *second = bondOf(r.atoms.value(c), r.atoms.value(o2));
                if (!first || !second)
                    return;
                const bool swapped = second->kind == "double" && first->kind != "double";
                setKind(swapped ? second : first, "double");
                setKind(swapped ? first : second, "single");
                charge(swapped ? o2 : o1, 0);
                charge(swapped ? o1 : o2, charged ? -1 : 0);
            };
            if (ion(r)) {
                topology.atom(d, r.atoms.first()).charge = ions()[r.code];
                continue;
            }
            if (r.code == "SO4") { // Sulfate: two S=O, two S–O−.
                for (const auto *o : {"O1", "O2", "O3", "O4"}) {
                    const bool single = o == QString("O3") || o == QString("O4");
                    setKind(bondOf(r.atoms.value("S"), r.atoms.value(o)), single ? "single" : "double");
                    charge(o, single ? -1 : 0);
                }
                continue;
            }
            if (!amino(r))
                continue;
            charge("N", chainEnd(d, topology, r, r.atoms["N"]) ? 1 : 0);
            if (r.atoms.contains("OXT"))
                carboxylate("C", "O", "OXT", true);
            const auto &code = r.code;
            if (code == "ASP" || code == "ASH")
                carboxylate("CG", "OD1", "OD2", code == "ASP");
            else if (code == "GLU" || code == "GLH")
                carboxylate("CD", "OE1", "OE2", code == "GLU");
            else if (code == "LYS" || code == "LYN")
                charge("NZ", code == "LYS" ? 1 : 0);
            else if (code == "CYS" || code == "CYM")
                charge("SG", code == "CYM" || boundToMetal(r.atoms.value("SG")) ? -1 : 0);
            else if (code == "ARG") {
                // The charge on the nitrogen of the C=N bond, as the geometry gave it (else NH2).
                QString charged = "NH2";
                for (const auto *n : {"NE", "NH1", "NH2"})
                    if (auto *bond = bondOf(r.atoms.value("CZ"), r.atoms.value(n));
                        bond && bond->kind == "double")
                        charged = n;
                for (const auto *n : {"NE", "NH1", "NH2"}) {
                    setKind(bondOf(r.atoms.value("CZ"), r.atoms.value(n)),
                            charged == n ? "double" : "single");
                    charge(n, charged == n ? 1 : 0);
                }
            } else if (QStringList{"HIS", "HID", "HIE", "HIP"}.contains(code)) {
                // The tautomer as a Kekulé structure: the nitrogen with two single bonds takes the hydrogen.
                const bool nd1 = boundToMetal(r.atoms.value("ND1")), ne2 = boundToMetal(r.atoms.value("NE2"));
                QString state = code == "HIS" ? (ne2 && !nd1 ? "HID" : "HIE") : code;
                const auto ring = [&](const char *a, const char *b, const char *kind) {
                    setKind(bondOf(r.atoms.value(a), r.atoms.value(b)), kind);
                };
                ring("CG", "CD2", "double");
                ring("CG", "ND1", "single");
                ring("ND1", "CE1", state == "HID" ? "single" : "double");
                ring("CE1", "NE2", state == "HID" ? "double" : "single");
                ring("NE2", "CD2", "single");
                charge("ND1", state == "HIP" ? 1 : 0);
                charge("NE2", 0);
                r.state = state;
            }
        }
        completeHydrogens(d);
    });
}
} // namespace mf
