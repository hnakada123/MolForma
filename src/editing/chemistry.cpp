#include "editing/chemistry.h"
#include "editing/peptide.h"
#include "editing/stereochemistry.h"
#include <QFileInfo>
#include <QRegularExpression>
#include <memory>
#include <openbabel/atom.h>
#include <openbabel/bond.h>
#include <openbabel/builder.h>
#include <openbabel/elements.h>
#include <openbabel/forcefield.h>
#include <openbabel/mol.h>
#include <openbabel/obconversion.h>
#include <openbabel/residue.h>
#include <openbabel/stereo/stereo.h>
#include <sstream>
namespace mf {
using namespace OpenBabel;
static OBMol toOB(const Document &d) {
    OBMol mol;
    mol.BeginModify();
    QMap<QString, int> indices;
    for (const auto &a : d.atoms) {
        if (a.ghost)
            fail("ゴースト原子はこの形式・力場では扱えません。プロジェクトまたはXYZを使用してください。");
        auto *o = mol.NewAtom();
        o->SetAtomicNum(element(a.element).number);
        o->SetFormalCharge(a.charge);
        o->SetIsotope(a.isotope);
        o->SetVector(a.position.x(), a.position.y(), a.position.z());
        if (a.radical)
            o->SetSpinMultiplicity(a.radical + 1);
        indices[a.id] = o->GetIdx();
    }
    for (const auto &b : d.bonds) {
        if (b.kind == "dative")
            fail("配位結合を含む分子はプロジェクトまたはXYZで保存してください。力場整形の対象外です。");
        int order = b.kind == "aromatic" ? 5 : int(b.order());
        mol.AddBond(indices[b.a], indices[b.b], order);
        if (b.kind == "aromatic") {
            mol.GetBond(mol.NumBonds() - 1)->SetAromatic();
            mol.GetAtom(indices[b.a])->SetAromatic();
            mol.GetAtom(indices[b.b])->SetAromatic();
        }
    }
    mol.EndModify();
    QMap<QString, char> chainNames;
    for (auto v : d.data["residues"].toArray()) {
        auto r = v.toObject();
        QString chain = r["chain"].toString();
        if (!chainNames.contains(chain))
            chainNames[chain] = char('A' + chainNames.size() % 26);
        auto *residue = mol.NewResidue();
        residue->SetName(r["code"].toString().toStdString());
        residue->SetNum(r["number"].toInt());
        residue->SetChain(chainNames[chain]);
        auto names = r["atoms"].toObject();
        for (auto it = names.begin(); it != names.end(); ++it) {
            int index = indices.value(it.value().toString());
            if (!index)
                continue;
            auto *atom = mol.GetAtom(index);
            residue->AddAtom(atom);
            residue->SetAtomID(atom, it.key().toStdString());
        }
    }
    if (!d.data["residues"].toArray().isEmpty())
        mol.SetChainsPerceived();
    mol.SetDimension(3);
    mol.SetTotalCharge(d.job()["charge"].toInt(d.formalCharge()));
    mol.SetTotalSpinMultiplicity(d.job()["multiplicity"].toInt(1));
    return mol;
}
static Document fromOB(OBMol &mol, bool readResidues = false) {
    Document d;
    QMap<unsigned int, QString> ids;
    QMap<OBResidue *, QJsonObject> residues;
    QMap<QString, QString> chains;
    for (unsigned int i = 1; i <= mol.NumAtoms(); ++i) {
        auto *o = mol.GetAtom(i);
        if (o->GetAtomicNum() == 0)
            fail("ダミー原子または未認識の元素があります。");
        Atom a;
        a.element = OBElements::GetSymbol(o->GetAtomicNum());
        element(a.element);
        a.position = Vec(o->GetX(), o->GetY(), o->GetZ());
        a.charge = o->GetFormalCharge();
        a.isotope = o->GetIsotope();
        a.radical = std::max(0, int(o->GetSpinMultiplicity()) - 1);
        ids[i] = a.id;
        if (auto *r = readResidues ? o->GetResidue() : nullptr) {
            a.name = QString::fromStdString(r->GetAtomID(o)).trimmed();
            QString chain = QString(QChar(r->GetChain()));
            if (!chains.contains(chain))
                chains[chain] = newId();
            if (!residues.contains(r)) {
                residues[r] = {{"id", newId()},
                               {"code", QString::fromStdString(r->GetName()).trimmed()},
                               {"atoms", QJsonObject()},
                               {"chain", chains[chain]},
                               {"number", r->GetNum()},
                               {"insertion_code", ""},
                               {"chirality", "L"},
                               {"state", "default"},
                               {"n_terminal", "neutral"},
                               {"c_terminal", "neutral"},
                               {"matched", false},
                               {"modified", false},
                               {"metadata", QJsonObject{{"pdb_chain", chain}}}};
            }
            auto rr = residues[r];
            auto names = rr["atoms"].toObject();
            if (names.contains(a.name))
                fail("PDBに原子名の重複があります。MODELとALTLOCを選び直してください。");
            names[a.name] = a.id;
            rr["atoms"] = names;
            residues[r] = rr;
        }
        d.atoms << a;
    }
    for (unsigned int i = 0; i < mol.NumBonds(); ++i) {
        auto *b = mol.GetBond(i);
        QString kind = b->IsAromatic()          ? "aromatic"
                       : b->GetBondOrder() == 3 ? "triple"
                       : b->GetBondOrder() == 2 ? "double"
                                                : "single";
        d.bonds.append({ids[b->GetBeginAtomIdx()], ids[b->GetEndAtomIdx()], kind, {}, "import"});
    }
    // Iterate residues in atom order; pointers are only lookup keys, never ordering keys.
    QSet<QString> seen;
    QJsonObject chainMap;
    for (const auto &a : d.atoms)
        for (const auto &r : residues) {
            bool member = false;
            for (auto id : r["atoms"].toObject())
                if (id == a.id)
                    member = true;
            if (!member || seen.contains(r["id"].toString()))
                continue;
            seen.insert(r["id"].toString());
            updateResidue(d, r);
            auto seq = chainMap[r["chain"].toString()].toArray();
            seq.append(r["id"]);
            chainMap[r["chain"].toString()] = seq;
        }
    d.data["chains"] = chainMap;
    auto rr = d.data["residues"].toArray();
    for (int i = 0; i < rr.size(); ++i) {
        auto r = rr[i].toObject();
        auto names = r["atoms"].toObject();
        for (auto side : {"N", "C"})
            if (names.contains(side)) {
                QString own = names[side].toString();
                for (auto n : d.neighbors(own)) {
                    QString other = residueForAtom(d, n);
                    if (!other.isEmpty() && other != r["id"].toString()) {
                        if (QString(side) == "N" && d.atom(n).name == "C")
                            r["n_terminal"] = "internal";
                        if (QString(side) == "C" && d.atom(n).name == "N")
                            r["c_terminal"] = "internal";
                    }
                }
            }
        rr[i] = r;
    }
    d.data["residues"] = rr;
    d.setJobValue("charge", mol.GetTotalCharge());
    d.setJobValue("multiplicity", int(mol.GetTotalSpinMultiplicity()));
    auto meta = d.data["metadata"].toObject();
    meta["source_dimension"] = mol.GetDimension();
    d.data["metadata"] = meta;
    auto errors = d.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    return d;
}
QStringList pdbModels(const QString &text) {
    QStringList models;
    for (const auto &line : text.split('\n'))
        if (line.startsWith("MODEL "))
            models << line.mid(10, 4).trimmed();
    if (models.isEmpty())
        models << "1";
    models.removeDuplicates();
    return models;
}
QString preparePdb(const QString &text, int model, const QString &altloc) {
    auto lines = text.split('\n');
    bool hasModels = text.contains(QRegularExpression("(^|\n)MODEL "));
    int current = hasModels ? 0 : model;
    QStringList chosen;
    QMap<QString, QMap<QString, double>> occupancies;
    for (const auto &l : lines) {
        if (l.startsWith("MODEL "))
            current = l.mid(10, 4).trimmed().toInt();
        if (l.startsWith("ENDMDL"))
            current = 0;
        if (current != model)
            continue;
        chosen << l;
        if (l.startsWith("ATOM  ") || l.startsWith("HETATM")) {
            QString residue = l.mid(21, 6), alt = l.mid(16, 1);
            if (!alt.trimmed().isEmpty())
                occupancies[residue][alt] += l.mid(54, 6).toDouble();
        }
    }
    QMap<QString, QString> preferred;
    for (auto it = occupancies.begin(); it != occupancies.end(); ++it) {
        double best = -1;
        for (auto a = it.value().begin(); a != it.value().end(); ++a)
            if (a.value() > best) {
                best = a.value();
                preferred[it.key()] = a.key();
            }
    }
    QString out;
    for (auto l : chosen) {
        if (l.startsWith("MODEL") || l.startsWith("ENDMDL"))
            continue;
        if (l.startsWith("ATOM  ") || l.startsWith("HETATM")) {
            QString alt = l.mid(16, 1), want = altloc.isEmpty() ? preferred[l.mid(21, 6)] : altloc;
            if (!alt.trimmed().isEmpty() && alt != want)
                continue;
            if (l.size() > 16)
                l[16] = ' ';
        }
        out += l + '\n';
    }
    return out;
}
Document xyzStructure(const QString &text) {
    Document d = Document::fromXyz(text);
    // Bonds come from distances. Let Open Babel assign double/triple/aromatic orders from the
    // geometry so hydrogen completion does not saturate rings. Metal and ghost bonds stay single.
    if (d.atoms.isEmpty() || d.atoms.size() > 20000)
        return d;
    OBMol mol;
    mol.BeginModify();
    QHash<QString, int> indices;
    for (const auto &a : d.atoms) {
        if (a.ghost)
            continue;
        auto *o = mol.NewAtom();
        o->SetAtomicNum(element(a.element).number);
        o->SetVector(a.position.x(), a.position.y(), a.position.z());
        indices[a.id] = o->GetIdx();
    }
    QVector<int> perceived;
    for (int i = 0; i < d.bonds.size(); ++i) {
        const int x = indices.value(d.bonds[i].a), y = indices.value(d.bonds[i].b);
        if (!x || !y || mol.GetAtom(x)->IsMetal() || mol.GetAtom(y)->IsMetal())
            continue;
        mol.AddBond(x, y, 1);
        perceived << i;
    }
    mol.EndModify();
    mol.SetDimension(3);
    mol.PerceiveBondOrders();
    for (int k = 0; k < perceived.size(); ++k) {
        auto *b = mol.GetBond(k);
        d.bonds[perceived[k]].kind = b->IsAromatic()          ? "aromatic"
                                     : b->GetBondOrder() == 3 ? "triple"
                                     : b->GetBondOrder() == 2 ? "double"
                                                              : "single";
    }
    return d;
}
Document importStructure(const QString &path, int model, const QString &altloc) {
    QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "json")
        return Document::load(path);
    auto bytes = readFile(path);
    if (ext == "xyz")
        return xyzStructure(QString::fromUtf8(bytes));
    if (ext == "ent")
        ext = "pdb";
    if (ext == "sd")
        ext = "sdf";
    if (ext == "mdl")
        ext = "mol";
    if (ext == "pdb")
        bytes = preparePdb(QString::fromUtf8(bytes), model, altloc).toUtf8();
    OBConversion conversion;
    if (!conversion.SetInFormat(ext.toLatin1().constData()))
        fail("対応していない形式です: " + ext);
    OBMol mol;
    if (!conversion.ReadString(&mol, bytes.toStdString()) || !mol.NumAtoms())
        fail("構造ファイルを読み込めません。");
    Document d = fromOB(mol, ext == "pdb");
    auto meta = d.data["metadata"].toObject();
    meta["source_file"] = path;
    meta["source_format"] = ext;
    if (ext == "pdb") {
        meta["pdb_model"] = model;
        meta["pdb_altloc"] = altloc;
    }
    d.data["metadata"] = meta;
    return d;
}
void exportStructure(const Document &d, const QString &path) {
    QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "json") {
        d.save(path);
        return;
    }
    if (ext == "xyz") {
        writeFile(path, d.xyz().toUtf8());
        return;
    }
    auto mol = toOB(d);
    OpenBabel::StereoFrom3D(&mol);
    OBConversion conversion;
    if (!conversion.SetOutFormat(ext.toLatin1().constData()))
        fail("対応していない出力形式です。");
    auto result = conversion.WriteString(&mol);
    if (result.empty())
        fail("構造を書き出せません。");
    writeFile(path, QByteArray::fromStdString(result));
}
Document fromSmiles(const QString &smiles) {
    if (smiles.trimmed().isEmpty() || smiles.contains('\n'))
        fail("SMILESを1行で入力してください。");
    OBConversion conv;
    conv.SetInFormat("smi");
    OBMol mol;
    if (!conv.ReadString(&mol, smiles.toStdString()) || !mol.NumAtoms())
        fail("SMILESを解析できません。");
    mol.AddHydrogens();
    OBBuilder builder;
    if (!builder.Build(mol))
        fail("3D座標を生成できません。");
    Document d = fromOB(mol);
    return refine(d);
}
Document refine(const Document &d, bool regenerate, const Ids &moving) {
    if (d.atoms.isEmpty())
        fail("分子が空です。");
    for (const auto &a : d.atoms)
        if (a.ghost)
            fail("金属・ゴースト原子を含む分子は力場整形の対象外です。");
    auto mol = toOB(d);
    // Heavy main-group elements (Br, I, Se, Te, ...) are left to the force-field setup below.
    for (unsigned int i = 1; i <= mol.NumAtoms(); ++i)
        if (mol.GetAtom(i)->IsMetal())
            fail("金属・ゴースト原子を含む分子は力場整形の対象外です。");
    OpenBabel::StereoFrom3D(&mol);
    if (regenerate) {
        for (const auto &a : d.atoms)
            if (a.fixed)
                fail("固定原子があるため3D再生成できません。");
        OBBuilder builder;
        if (!builder.Build(mol))
            fail("3D座標の再生成に失敗しました。");
    }
    OBFFConstraints constraints;
    for (int i = 0; i < d.atoms.size(); ++i)
        if (d.atoms[i].fixed || (!moving.isEmpty() && !moving.contains(d.atoms[i].id)))
            constraints.AddAtomConstraint(i + 1);
    std::unique_ptr<OBForceField> ff;
    QString used;
    for (auto name : {"MMFF94", "UFF"}) {
        auto *prototype = OBForceField::FindForceField(name);
        if (!prototype)
            continue;
        ff.reset(prototype->MakeNewInstance());
        ff->SetLogLevel(OBFF_LOGLVL_NONE);
        if (ff->Setup(mol, constraints)) {
            used = name;
            break;
        }
        ff.reset();
    }
    if (!ff)
        fail("この構造に使える力場パラメータがありません。結合次数・形式電荷を確認してください。");
    ff->ConjugateGradients(600, 1e-7);
    ff->GetCoordinates(mol);
    Document out = d;
    for (int i = 0; i < out.atoms.size(); ++i) {
        auto *a = mol.GetAtom(i + 1);
        Vec p(a->GetX(), a->GetY(), a->GetZ());
        if (!p.allFinite())
            fail("整形で非有限座標が発生しました。");
        if (!d.atoms[i].fixed && (moving.isEmpty() || moving.contains(d.atoms[i].id)))
            out.atoms[i].position = p;
    }
    auto meta = out.data["metadata"].toObject();
    meta["molforma_force_field"] = used;
    meta["molforma_energy"] = ff->Energy();
    meta["accept_2d"] = true;
    out.data["metadata"] = meta;
    if (hasStereochemistryBackend()) {
        auto labeled = out;
        recordStereo(labeled);
        for (int i = 0; i < d.atoms.size(); ++i)
            if (!d.atoms[i].stereo.isEmpty() && labeled.atoms[i].stereo != d.atoms[i].stereo)
                fail("整形結果が記録済みの立体配置と異なります。元の構造を保持しました。");
        for (int i = 0; i < d.bonds.size(); ++i)
            if (!d.bonds[i].stereo.isEmpty() && labeled.bonds[i].stereo != d.bonds[i].stereo)
                fail("整形結果が記録済みのE/Z配置と異なります。元の構造を保持しました。");
    }
    return out;
}
} // namespace mf
