#include "editing/stereochemistry.h"
#include "editing/library.h"
#include "editing/peptide.h"
#include "model/i18n.h"
#include <algorithm>
#include <memory>
#ifdef MOLFORMA_HAS_RDKIT
#include <GraphMol/Atom.h>
#include <GraphMol/Bond.h>
#if __has_include(<GraphMol/CIPLabeler/CIPLabeler.h>)
#include <GraphMol/CIPLabeler/CIPLabeler.h>
#else
#include <CIPLabeler/CIPLabeler.h> // Where RDKit 2023.09 is installed by Ubuntu 24.04.
#endif
#include <GraphMol/Conformer.h>
#include <GraphMol/MolOps.h>
#include <GraphMol/QueryAtom.h>
#include <GraphMol/QueryBond.h>
#include <GraphMol/QueryOps.h>
#include <GraphMol/RWMol.h>
#include <GraphMol/Substruct/SubstructMatch.h>
#endif
namespace mf {
bool hasStereochemistryBackend() {
#ifdef MOLFORMA_HAS_RDKIT
    return true;
#else
    return false;
#endif
}
#ifdef MOLFORMA_HAS_RDKIT
static std::unique_ptr<RDKit::RWMol> rdMol(const Document &d) {
    auto mol = std::make_unique<RDKit::RWMol>();
    QMap<QString, unsigned int> indices;
    for (const auto &a : d.atoms) {
        if (a.ghost)
            fail(tr("ゴースト原子を含む構造の立体判定には対応していません。"));
        auto *atom = new RDKit::Atom(element(a.element).number);
        atom->setFormalCharge(a.charge);
        atom->setNumRadicalElectrons(a.radical);
        atom->setNoImplicit(true);
        if (a.isotope)
            atom->setIsotope(a.isotope);
        indices[a.id] = mol->addAtom(atom, true, true);
    }
    for (const auto &b : d.bonds) {
        auto type = b.kind == "double"     ? RDKit::Bond::DOUBLE
                    : b.kind == "triple"   ? RDKit::Bond::TRIPLE
                    : b.kind == "aromatic" ? RDKit::Bond::AROMATIC
                    : b.kind == "dative"   ? RDKit::Bond::DATIVE
                                           : RDKit::Bond::SINGLE;
        mol->addBond(indices[b.a], indices[b.b], type);
        if (type == RDKit::Bond::AROMATIC) {
            mol->getAtomWithIdx(indices[b.a])->setIsAromatic(true);
            mol->getAtomWithIdx(indices[b.b])->setIsAromatic(true);
            mol->getBondBetweenAtoms(indices[b.a], indices[b.b])->setIsAromatic(true);
        }
    }
    auto *conf = new RDKit::Conformer(d.atoms.size());
    conf->set3D(true);
    for (int i = 0; i < d.atoms.size(); ++i) {
        auto p = d.atoms[i].position;
        conf->setAtomPos(i, RDGeom::Point3D(p.x(), p.y(), p.z()));
    }
    mol->addConformer(conf, true);
    mol->updatePropertyCache(false);
    return mol;
}
#endif
void recordStereo(Document &d) {
#ifdef MOLFORMA_HAS_RDKIT
    auto mol = rdMol(d);
    RDKit::MolOps::sanitizeMol(*mol);
    RDKit::MolOps::assignStereochemistryFrom3D(*mol);
    RDKit::CIPLabeler::assignCIPLabels(*mol, 1000000);
    for (unsigned int i = 0; i < mol->getNumAtoms(); ++i) {
        std::string label;
        auto *a = mol->getAtomWithIdx(i);
        d.atoms[i].stereo = a->getPropIfPresent("_CIPCode", label) ? QString::fromStdString(label) : "";
        auto meta = d.atoms[i].extra["metadata"].toObject();
        meta["chiral_tag"] = int(a->getChiralTag());
        d.atoms[i].extra["metadata"] = meta;
    }
    for (auto &b : d.bonds) {
        auto *bond = mol->getBondBetweenAtoms(d.index(b.a), d.index(b.b));
        auto stereo = bond->getStereo();
        b.stereo = stereo == RDKit::Bond::STEREOE || stereo == RDKit::Bond::STEREOTRANS ? "E"
                   : stereo == RDKit::Bond::STEREOZ || stereo == RDKit::Bond::STEREOCIS ? "Z"
                                                                                        : "";
    }
#else
    Q_UNUSED(d);
    fail(tr("立体判定にはRDKit C++ライブラリを有効にしてビルドしてください。"));
#endif
}
#ifdef MOLFORMA_HAS_RDKIT
// The formal charge a structure read without charges (XYZ) implies from its hydrogens and bonds: a
// carboxylate, phenolate or thiolate O/S with one bond and no hydrogen (−1), and an ammonium or
// iminium N with four bonds (+1). Aromatic rings are left to the caller (histidine).
static void impliedCharge(Document &d, const QString &id) {
    auto &a = d.atom(id);
    if (a.charge || a.radical)
        return;
    double order = 0;
    int hydrogens = 0;
    for (const auto &b : d.bonds) {
        if ((b.a != id && b.b != id) || b.kind == "dative")
            continue;
        if (b.kind == "aromatic")
            return;
        order += b.order();
        hydrogens += d.atom(b.a == id ? b.b : b.a).element == "H";
    }
    if ((a.element == "O" || a.element == "S") && order == 1 && !hydrogens)
        a.charge = -1;
    else if (a.element == "N" && order == 4)
        a.charge = 1;
}
// A cap at the free end of a recognized chain: the acetyl C=O–CH3 on the first residue's N, or the
// N-methylamide N(H)–CH3 on the last residue's C. Returns its atoms named as when built.
static QMap<QString, QString> recognizedCap(const Document &d, const QString &end, bool ace,
                                            const QSet<QString> &taken) {
    const auto hydrogensOf = [&](const QString &id) {
        Ids out;
        for (const auto &h : d.neighbors(id, false))
            if (d.atom(h).element == "H")
                out << h;
        return out;
    };
    for (const auto &first : d.neighbors(end, false)) {
        if (taken.contains(first) || d.atom(first).element != (ace ? "C" : "N"))
            continue;
        QString oxygen, methyl;
        int others = 0;
        for (const auto &other : d.neighbors(first, false)) {
            if (other == end || d.atom(other).element == "H")
                continue;
            if (ace && d.atom(other).element == "O" && d.bond(first, other)->kind == "double")
                oxygen = other;
            else if (d.atom(other).element == "C" && hydrogensOf(other).size() == 3)
                methyl = other;
            else
                ++others;
        }
        if (methyl.isEmpty() || others || (ace && oxygen.isEmpty()) ||
            (!ace && hydrogensOf(first).size() != 1))
            continue;
        QMap<QString, QString> names{{ace ? "C" : "N", first}, {"CH3", methyl}};
        int index = 0;
        if (ace)
            names["O"] = oxygen;
        else
            names["H" + QString::number(++index)] = hydrogensOf(first).first();
        for (const auto &h : hydrogensOf(methyl))
            names["H" + QString::number(++index)] = h;
        return names;
    }
    return {};
}
#endif
static void assignResidues(Document &d) {
#ifdef MOLFORMA_HAS_RDKIT
    auto mol = rdMol(d);
    QVector<QJsonObject> amino;
    for (auto v : templates()) {
        auto t = v.toObject();
        if (t["category"] == "amino")
            amino << t;
    }
    std::stable_sort(amino.begin(), amino.end(), [](auto a, auto b) {
        return a["atoms"].toArray().size() > b["atoms"].toArray().size();
    });
    QSet<QString> assigned;
    QVector<Residue> residues;
    for (auto t : amino) {
        Document pattern;
        instantiate(pattern, t);
        Ids remove;
        for (const auto &a : pattern.atoms)
            if (a.element == "H" || a.name == "OXT")
                remove << a.id;
        pattern.remove(remove);
        auto query = rdMol(pattern);
        for (unsigned int i = 0; i < query->getNumAtoms(); ++i) {
            RDKit::QueryAtom atom(query->getAtomWithIdx(i)->getAtomicNum());
            query->replaceAtom(i, &atom);
        }
        // Elements and connections only: bond orders read from XYZ (an imidazolium ring, a
        // carboxylate) need not be the template's.
        for (unsigned int i = 0; i < query->getNumBonds(); ++i) {
            RDKit::QueryBond bond;
            bond.setBeginAtomIdx(query->getBondWithIdx(i)->getBeginAtomIdx());
            bond.setEndAtomIdx(query->getBondWithIdx(i)->getEndAtomIdx());
            bond.setQuery(RDKit::makeBondNullQuery());
            query->replaceBond(i, &bond);
        }
        RDKit::SubstructMatchParameters params;
        params.useChirality = false;
        params.maxMatches = 10000;
        for (const auto &match : RDKit::SubstructMatch(*mol, *query, params)) {
            QMap<QString, QString> names;
            QSet<QString> members;
            bool overlap = false;
            for (auto pair : match) {
                auto id = d.atoms[pair.second].id;
                if (assigned.contains(id))
                    overlap = true;
                names[pattern.atoms[pair.first].name] = id;
                members.insert(id);
            }
            if (overlap)
                continue;
            QString n = names.value("N"), c = names.value("C");
            bool boundary = true;
            for (auto id : members)
                for (auto other : d.neighbors(id, false))
                    if (!members.contains(other) && d.atom(other).element != "H" && id != n && id != c)
                        boundary = false;
            if (!boundary)
                continue;
            for (auto id : members) {
                int index = 1;
                QString parentName;
                for (auto it = names.begin(); it != names.end(); ++it)
                    if (it.value() == id)
                        parentName = it.key();
                for (auto h : d.neighbors(id, false))
                    if (d.atom(h).element == "H")
                        names["H" + parentName + QString::number(index++)] = h;
            }
            for (auto other : d.neighbors(c, false))
                if (!members.contains(other) && d.atom(other).element == "O" &&
                    d.bond(c, other)->kind == "single") {
                    names["OXT"] = other;
                    int index = 1;
                    for (auto h : d.neighbors(other, false))
                        if (d.atom(h).element == "H")
                            names["HOXT" + QString::number(index++)] = h;
                }
            for (auto it = names.begin(); it != names.end(); ++it) {
                assigned.insert(it.value());
                d.atom(it.value()).name = it.key();
            }
            // Charges and side-chain states from the hydrogens present: the heavy atoms alone match
            // every state of a residue.
            for (auto it = names.begin(); it != names.end(); ++it)
                if (d.atom(it.value()).element != "H" && it.key() != "ND1" && it.key() != "NE2")
                    impliedCharge(d, it.value());
            const auto code = t["code"].toString();
            const auto charged = [&](std::initializer_list<const char *> atoms, int sign) {
                return std::any_of(atoms.begin(), atoms.end(), [&](const char *name) {
                    return names.contains(name) && d.atom(names.value(name)).charge * sign > 0;
                });
            };
            const auto protonated = [&](const char *name) {
                return names.contains(name) &&
                       std::any_of(names.begin(), names.end(), [&](const QString &id) {
                           return d.atom(id).element == "H" && d.bond(id, names.value(name));
                       });
            };
            QString state = t["state"].toString();
            if (code == "ASP" || code == "GLU")
                state = charged({"OD1", "OD2", "OE1", "OE2"}, -1) ? "default" : "neutral";
            else if (code == "LYS" || code == "ARG")
                state = charged({"NZ", "NE", "NH1", "NH2"}, 1) ? "default" : "neutral";
            else if (code == "CYS" || code == "TYR")
                state = charged({"SG", "OH"}, -1) ? "anion" : "default";
            else if (code == "HIS") {
                state = protonated("ND1") && protonated("NE2") ? "HIP" : protonated("ND1") ? "HID" : "HIE";
                if (state == "HIP")
                    d.atom(names.value("ND1")).charge = 1;
            }
            Residue residue;
            residue.code = code;
            residue.atoms = names;
            residue.state = state;
            residue.nTerminal = d.atom(n).charge > 0 ? "charged" : "neutral";
            residue.cTerminal =
                names.contains("OXT") && d.atom(names.value("OXT")).charge < 0 ? "charged" : "neutral";
            residues << residue;
        }
    }
    if (residues.isEmpty())
        fail(tr("標準アミノ酸の結合構造を認識できません。結合次数を確認してください。"));
    QMap<QString, int> byN, byC;
    for (int i = 0; i < residues.size(); ++i) {
        byN[residues[i].atoms.value("N")] = i;
        byC[residues[i].atoms.value("C")] = i;
    }
    QMap<int, int> next, previous;
    for (auto b : d.bonds) {
        QString c = b.a, n = b.b;
        if (byN.contains(c) && byC.contains(n))
            std::swap(c, n);
        if (byC.contains(c) && byN.contains(n)) {
            next[byC[c]] = byN[n];
            previous[byN[n]] = byC[c];
        }
    }
    QSet<int> seen;
    QMap<QString, Ids> chains;
    QVector<Residue> ordered;
    for (int pass = 0; pass < 2; ++pass)
        for (int start = 0; start < residues.size(); ++start) {
            if (seen.contains(start) || (pass == 0 && previous.contains(start)))
                continue;
            QString chain = newId();
            Ids seq;
            int i = start, num = 1;
            while (!seen.contains(i)) {
                seen.insert(i);
                auto r = residues[i];
                r.chain = chain;
                r.number = num++;
                if (previous.contains(i))
                    r.nTerminal = "internal";
                if (next.contains(i))
                    r.cTerminal = "internal";
                seq << r.id;
                ordered << r;
                if (!next.contains(i))
                    break;
                i = next[i];
            }
            chains[chain] = seq;
        }
    // ACE and NME caps at the ends of each chain, numbered before the first and after the last residue.
    QSet<QString> taken;
    for (const auto &r : std::as_const(ordered))
        for (const auto &id : r.atoms)
            taken.insert(id);
    const auto row = [&](const QString &rid) {
        for (int i = 0; i < ordered.size(); ++i)
            if (ordered[i].id == rid)
                return i;
        return -1;
    };
    for (auto it = chains.begin(); it != chains.end(); ++it) {
        auto seq = it.value();
        for (bool ace : {true, false}) {
            const int at = row(ace ? seq.first() : seq.last());
            const auto names = recognizedCap(d, ordered[at].atoms.value(ace ? "N" : "C"), ace, taken);
            if (names.isEmpty())
                continue;
            for (auto n = names.begin(); n != names.end(); ++n) {
                taken.insert(n.value());
                d.atom(n.value()).name = n.key();
            }
            Residue cap;
            cap.code = ace ? "ACE" : "NME";
            cap.atoms = names;
            cap.chain = it.key();
            cap.number = ordered[at].number + (ace ? -1 : 1);
            cap.chirality = "";
            cap.nTerminal = cap.cTerminal = "internal";
            (ace ? ordered[at].nTerminal : ordered[at].cTerminal) = ace ? "ACE" : "NME";
            ordered << cap;
            if (ace)
                seq.prepend(cap.id);
            else
                seq.append(cap.id);
        }
        it.value() = seq;
    }
    d.residues = ordered;
    d.chains = chains;
#else
    Q_UNUSED(d);
    fail(tr("グラフからの残基認識にはRDKit C++ライブラリが必要です。"));
#endif
}
void recognizeResidues(Document &d) {
    followingCharge(d, [&] { assignResidues(d); });
}
} // namespace mf
