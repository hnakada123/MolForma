#include "editing/stereochemistry.h"
#include "editing/library.h"
#include "editing/peptide.h"
#include "model/i18n.h"
#include <algorithm>
#include <memory>
#ifdef MOLFORMA_HAS_RDKIT
#include <GraphMol/Atom.h>
#include <GraphMol/Bond.h>
#include <GraphMol/CIPLabeler/CIPLabeler.h>
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
static QJsonObject recognizedCap(const Document &d, const QString &end, bool ace,
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
        QJsonObject names{{ace ? "C" : "N", first}, {"CH3", methyl}};
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
void recognizeResidues(Document &d) {
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
    QJsonArray residues;
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
            QJsonObject names;
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
            QString n = names["N"].toString(), c = names["C"].toString();
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
                assigned.insert(it.value().toString());
                d.atom(it.value().toString()).name = it.key();
            }
            // Charges and side-chain states from the hydrogens present: the heavy atoms alone match
            // every state of a residue.
            for (auto it = names.begin(); it != names.end(); ++it)
                if (d.atom(it.value().toString()).element != "H" && it.key() != "ND1" && it.key() != "NE2")
                    impliedCharge(d, it.value().toString());
            const auto code = t["code"].toString();
            const auto charged = [&](std::initializer_list<const char *> atoms, int sign) {
                return std::any_of(atoms.begin(), atoms.end(), [&](const char *name) {
                    return names.contains(name) && d.atom(names[name].toString()).charge * sign > 0;
                });
            };
            const auto protonated = [&](const char *name) {
                return names.contains(name) &&
                       std::any_of(names.begin(), names.end(), [&](const QJsonValue &v) {
                           return d.atom(v.toString()).element == "H" &&
                                  d.bond(v.toString(), names[name].toString());
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
                    d.atom(names["ND1"].toString()).charge = 1;
            }
            residues.append(QJsonObject{
                {"id", newId()},
                {"code", t["code"]},
                {"atoms", names},
                {"chain", ""},
                {"number", 0},
                {"insertion_code", ""},
                {"chirality", "L"},
                {"state", state},
                {"n_terminal", d.atom(n).charge > 0 ? "charged" : "neutral"},
                {"c_terminal",
                 names.contains("OXT") && d.atom(names["OXT"].toString()).charge < 0 ? "charged" : "neutral"},
                {"matched", true},
                {"modified", false},
                {"metadata", QJsonObject()}});
        }
    }
    if (residues.isEmpty())
        fail(tr("標準アミノ酸の結合構造を認識できません。結合次数を確認してください。"));
    QMap<QString, int> byN, byC;
    for (int i = 0; i < residues.size(); ++i) {
        auto names = residues[i].toObject()["atoms"].toObject();
        byN[names["N"].toString()] = i;
        byC[names["C"].toString()] = i;
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
    QJsonObject chains;
    QJsonArray ordered;
    for (int pass = 0; pass < 2; ++pass)
        for (int start = 0; start < residues.size(); ++start) {
            if (seen.contains(start) || (pass == 0 && previous.contains(start)))
                continue;
            QString chain = newId();
            QJsonArray seq;
            int i = start, num = 1;
            while (!seen.contains(i)) {
                seen.insert(i);
                auto r = residues[i].toObject();
                r["chain"] = chain;
                r["number"] = num++;
                if (previous.contains(i))
                    r["n_terminal"] = "internal";
                if (next.contains(i))
                    r["c_terminal"] = "internal";
                seq.append(r["id"]);
                ordered.append(r);
                if (!next.contains(i))
                    break;
                i = next[i];
            }
            chains[chain] = seq;
        }
    // ACE and NME caps at the ends of each chain, numbered before the first and after the last residue.
    QSet<QString> taken;
    for (const auto &v : std::as_const(ordered))
        for (const auto &id : v.toObject()["atoms"].toObject())
            taken.insert(id.toString());
    const auto row = [&](const QString &rid) {
        for (int i = 0; i < ordered.size(); ++i)
            if (ordered[i].toObject()["id"] == rid)
                return i;
        return -1;
    };
    for (auto it = chains.begin(); it != chains.end(); ++it) {
        auto seq = it.value().toArray();
        for (bool ace : {true, false}) {
            const int at = row((ace ? seq.first() : seq.last()).toString());
            auto end = ordered[at].toObject();
            const auto names =
                recognizedCap(d, end["atoms"].toObject()[ace ? "N" : "C"].toString(), ace, taken);
            if (names.isEmpty())
                continue;
            for (auto n = names.begin(); n != names.end(); ++n) {
                taken.insert(n.value().toString());
                d.atom(n.value().toString()).name = n.key();
            }
            const QString cap = newId();
            ordered.append(QJsonObject{{"id", cap},
                                       {"code", ace ? "ACE" : "NME"},
                                       {"atoms", names},
                                       {"chain", it.key()},
                                       {"number", end["number"].toInt() + (ace ? -1 : 1)},
                                       {"insertion_code", ""},
                                       {"chirality", ""},
                                       {"state", "default"},
                                       {"n_terminal", "internal"},
                                       {"c_terminal", "internal"},
                                       {"matched", true},
                                       {"modified", false},
                                       {"metadata", QJsonObject()}});
            end[ace ? "n_terminal" : "c_terminal"] = ace ? "ACE" : "NME";
            ordered[at] = end;
            if (ace)
                seq.prepend(cap);
            else
                seq.append(cap);
        }
        it.value() = seq;
    }
    d.data["residues"] = ordered;
    d.data["chains"] = chains;
    d.invalidateAssignments();
#else
    Q_UNUSED(d);
    fail(tr("グラフからの残基認識にはRDKit C++ライブラリが必要です。"));
#endif
}
} // namespace mf
