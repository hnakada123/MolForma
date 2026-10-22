#include "stereochemistry.h"
#include "library.h"
#include "peptide.h"
#include <memory>
#ifdef MOLFORMA_HAS_RDKIT
#include <GraphMol/Atom.h>
#include <GraphMol/Bond.h>
#include <GraphMol/CIPLabeler/CIPLabeler.h>
#include <GraphMol/Conformer.h>
#include <GraphMol/MolOps.h>
#include <GraphMol/QueryAtom.h>
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
            fail("ゴースト原子を含む構造の立体判定には対応していません。");
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
    fail("立体判定にはRDKit C++ライブラリを有効にしてビルドしてください。");
#endif
}
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
            residues.append(QJsonObject{
                {"id", newId()},
                {"code", t["code"]},
                {"atoms", names},
                {"chain", ""},
                {"number", 0},
                {"insertion_code", ""},
                {"chirality", "L"},
                {"state", t["state"]},
                {"n_terminal", d.atom(n).charge > 0 ? "charged" : "neutral"},
                {"c_terminal",
                 names.contains("OXT") && d.atom(names["OXT"].toString()).charge < 0 ? "charged" : "neutral"},
                {"matched", true},
                {"modified", false},
                {"metadata", QJsonObject()}});
        }
    }
    if (residues.isEmpty())
        fail("標準アミノ酸の結合構造を認識できません。結合次数を確認してください。");
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
    d.data["residues"] = ordered;
    d.data["chains"] = chains;
    d.invalidateAssignments();
#else
    Q_UNUSED(d);
    fail("グラフからの残基認識にはRDKit C++ライブラリが必要です。");
#endif
}
} // namespace mf
