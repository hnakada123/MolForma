// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/chemistry.h"
#include "editing/peptide.h"
#include "editing/stereochemistry.h"
#include "model/geometry.h"
#include "model/i18n.h"
#include "model/topology.h"
#include <QFileInfo>
#include <QRegularExpression>
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
            fail(tr("ゴースト原子はこの形式・力場では扱えません。プロジェクトまたはXYZを使用してください。"));
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
            fail(tr("配位結合を含む分子はプロジェクトまたはXYZで保存してください。力場整形の対象外です。"));
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
    for (const auto &r : d.residues) {
        if (!chainNames.contains(r.chain))
            chainNames[r.chain] = char('A' + chainNames.size() % 26);
        auto *residue = mol.NewResidue();
        residue->SetName(r.code.toStdString());
        residue->SetNum(r.number);
        residue->SetChain(chainNames[r.chain]);
        for (auto it = r.atoms.begin(); it != r.atoms.end(); ++it) {
            int index = indices.value(it.value());
            if (!index)
                continue;
            auto *atom = mol.GetAtom(index);
            residue->AddAtom(atom);
            residue->SetAtomID(atom, it.key().toStdString());
        }
    }
    if (!d.residues.isEmpty())
        mol.SetChainsPerceived();
    mol.SetDimension(3);
    mol.SetTotalCharge(d.charge);
    mol.SetTotalSpinMultiplicity(d.multiplicity);
    return mol;
}
static Document fromOB(OBMol &mol, bool readResidues = false) {
    Document d;
    QMap<unsigned int, QString> ids;
    QMap<OBResidue *, Residue> residues;
    QVector<OBResidue *> residueOf; // By atom index.
    QMap<QString, QString> chains;
    for (unsigned int i = 1; i <= mol.NumAtoms(); ++i) {
        auto *o = mol.GetAtom(i);
        if (o->GetAtomicNum() == 0)
            fail(tr("ダミー原子または未認識の元素があります。"));
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
                Residue residue;
                residue.code = QString::fromStdString(r->GetName()).trimmed();
                residue.chain = chains[chain];
                residue.number = r->GetNum();
                residue.matched = false;
                residue.metadata = {{"pdb_chain", chain}};
                residues[r] = residue;
            }
            auto &names = residues[r].atoms;
            if (names.contains(a.name))
                fail(tr("PDBに原子名の重複があります。MODELとALTLOCを選び直してください。"));
            names[a.name] = a.id;
        }
        residueOf << (readResidues ? o->GetResidue() : nullptr);
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
    QSet<OBResidue *> seen;
    for (auto *key : residueOf)
        if (key && residues.contains(key) && !seen.contains(key)) {
            seen.insert(key);
            const auto &r = residues[key];
            updateResidue(d, r);
            d.chains[r.chain] << r.id;
        }
    const Topology topology(d);
    const auto residueIds = atomResidues(d);
    for (auto &r : d.residues)
        for (auto side : {"N", "C"})
            if (const int atom = topology.index(r.atoms.value(side)); atom >= 0)
                for (int n : topology.neighbors(atom)) {
                    QString other = residueIds.value(d.atoms[n].id);
                    if (!other.isEmpty() && other != r.id) {
                        if (QString(side) == "N" && d.atoms[n].name == "C")
                            r.nTerminal = "internal";
                        if (QString(side) == "C" && d.atoms[n].name == "N")
                            r.cTerminal = "internal";
                    }
                }
    d.charge = mol.GetTotalCharge();
    d.multiplicity = int(mol.GetTotalSpinMultiplicity());
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
Document importBabelStructure(const QString &path, int model, const QString &altloc) {
    QString ext = QFileInfo(path).suffix().toLower();
    auto bytes = readFile(path);
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
        fail(tr("対応していない形式です: %1").arg(ext));
    OBMol mol;
    if (!conversion.ReadString(&mol, bytes.toStdString()) || !mol.NumAtoms())
        fail(tr("構造ファイルを読み込めません。"));
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
void exportBabelStructure(const Document &d, const QString &path) {
    const auto ext = QFileInfo(path).suffix().toLower();
    auto mol = toOB(d);
    OpenBabel::StereoFrom3D(&mol);
    OBConversion conversion;
    if (!conversion.SetOutFormat(ext.toLatin1().constData()))
        fail(tr("対応していない出力形式です。"));
    auto result = conversion.WriteString(&mol);
    if (result.empty())
        fail(tr("構造を書き出せません。"));
    writeFile(path, QByteArray::fromStdString(result));
}
Document fromSmiles(const QString &smiles) {
    if (smiles.trimmed().isEmpty() || smiles.contains('\n'))
        fail(tr("SMILESを1行で入力してください。"));
    OBConversion conv;
    conv.SetInFormat("smi");
    OBMol mol;
    if (!conv.ReadString(&mol, smiles.toStdString()) || !mol.NumAtoms())
        fail(tr("SMILESを解析できません。"));
    mol.AddHydrogens();
    OBBuilder builder;
    if (!builder.Build(mol))
        fail(tr("3D座標を生成できません。"));
    Document d = fromOB(mol);
    return refine(d);
}
Document refine(const Document &d, bool regenerate, const Ids &moving) {
    if (d.atoms.isEmpty())
        fail(tr("分子が空です。"));
    for (const auto &a : d.atoms)
        if (a.ghost)
            fail(tr("金属・ゴースト原子を含む分子は力場整形の対象外です。"));
    for (const auto &b : d.bonds)
        if (b.kind == "dative")
            fail(tr("配位結合を含む分子（金属錯体など）は力場整形の対象外です。"));
    auto mol = toOB(d);
    // Heavy main-group elements (Br, I, Se, Te, ...) are left to the force-field setup below.
    for (unsigned int i = 1; i <= mol.NumAtoms(); ++i)
        if (mol.GetAtom(i)->IsMetal())
            fail(tr("金属・ゴースト原子を含む分子は力場整形の対象外です。"));
    OpenBabel::StereoFrom3D(&mol);
    if (regenerate) {
        for (const auto &a : d.atoms)
            if (a.fixed)
                fail(tr("固定原子があるため3D再生成できません。"));
        OBBuilder builder;
        if (!builder.Build(mol))
            fail(tr("3D座標の再生成に失敗しました。"));
    }
    OBFFConstraints constraints;
    for (int i = 0; i < d.atoms.size(); ++i)
        if (d.atoms[i].fixed || (!moving.isEmpty() && !moving.contains(d.atoms[i].id)))
            constraints.AddAtomConstraint(i + 1);
    // Use the registered force fields directly, as Open Babel's obminimize does: in Open Babel 3.2 a
    // UFF copy made by MakeNewInstance() frees its buffers twice and aborts. Refinement runs once per
    // worker process (and sequentially in tests), so the shared instances are not used concurrently.
    OBForceField *ff = nullptr;
    QString used;
    for (auto name : {"MMFF94", "UFF"}) {
        auto *candidate = OBForceField::FindForceField(name);
        if (!candidate)
            continue;
        candidate->SetLogLevel(OBFF_LOGLVL_NONE);
        if (candidate->Setup(mol, constraints)) {
            ff = candidate;
            used = name;
            break;
        }
    }
    if (!ff)
        fail(tr("この構造に使える力場パラメータがありません。結合次数・形式電荷を確認してください。"));
    ff->ConjugateGradients(600, 1e-7);
    ff->GetCoordinates(mol);
    Document out = d;
    for (int i = 0; i < out.atoms.size(); ++i) {
        auto *a = mol.GetAtom(i + 1);
        Vec p(a->GetX(), a->GetY(), a->GetZ());
        if (!p.allFinite())
            fail(tr("整形で非有限座標が発生しました。"));
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
                fail(tr("整形結果が記録済みの立体配置と異なります。元の構造を保持しました。"));
        for (int i = 0; i < d.bonds.size(); ++i)
            if (!d.bonds[i].stereo.isEmpty() && labeled.bonds[i].stereo != d.bonds[i].stereo)
                fail(tr("整形結果が記録済みのE/Z配置と異なります。元の構造を保持しました。"));
    }
    return out;
}
QVector<Conformer> searchConformers(const Document &d, int count) {
    if (d.atoms.isEmpty())
        fail(tr("分子が空です。"));
    if (count < 1 || count > 100)
        fail(tr("配座の数は1〜100で指定してください。"));
    for (const auto &a : d.atoms) {
        if (a.ghost)
            fail(tr("金属・ゴースト原子を含む分子は配座探索の対象外です。"));
        if (a.fixed)
            fail(tr("固定原子があるため配座探索できません。固定を解除してください。"));
    }
    auto mol = toOB(d);
    for (unsigned int i = 1; i <= mol.NumAtoms(); ++i)
        if (mol.GetAtom(i)->IsMetal())
            fail(tr("金属・ゴースト原子を含む分子は配座探索の対象外です。"));
    OBForceField *ff = nullptr;
    for (auto name : {"MMFF94", "UFF"})
        if (auto *candidate = OBForceField::FindForceField(name)) {
            candidate->SetLogLevel(OBFF_LOGLVL_NONE);
            if (candidate->Setup(mol)) {
                ff = candidate;
                break;
            }
        }
    if (!ff)
        fail(tr("この構造に使える力場パラメータがありません。結合次数・形式電荷を確認してください。"));
    const double toKcal = QString::fromStdString(ff->GetUnit()).contains("kJ") ? 1 / 4.184 : 1.;
    struct Found {
        QVector<Vec> positions;
        double energy;
    };
    QVector<Found> found;
    // Each random rotamer is minimized by the force field; record every one, not only the best.
    const auto record = [&] {
        ff->ConjugateGradients(500, 1e-6);
        ff->GetCoordinates(mol);
        Found f{{}, ff->Energy(false) * toKcal};
        for (unsigned int i = 1; i <= mol.NumAtoms(); ++i) {
            auto *a = mol.GetAtom(i);
            f.positions << Vec(a->GetX(), a->GetY(), a->GetZ());
        }
        if (std::isfinite(f.energy))
            found << f;
    };
    record(); // The starting structure, also for molecules without rotatable bonds.
    ff->RandomRotorSearchInitialize(std::max(30, count * 4), 200);
    while (ff->RandomRotorSearchNextConformer(200))
        record();
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.energy < b.energy; });
    QVector<Conformer> result;
    for (const auto &f : found) {
        Document next = d;
        for (int i = 0; i < next.atoms.size(); ++i)
            next.atoms[i].position = f.positions[i];
        // Distinct: heavy atoms differ by more than 0.25 Å RMSD after superposition.
        const bool duplicate = std::any_of(result.begin(), result.end(), [&](const Conformer &c) {
            return std::abs(c.energy - f.energy) < .05 || superpose(next, c.document).heavyRmsd < .25;
        });
        if (duplicate)
            continue;
        result << Conformer{next, f.energy};
        if (result.size() >= count)
            break;
    }
    if (hasStereochemistryBackend())
        for (const auto &c : result) {
            auto labeled = c.document;
            recordStereo(labeled);
            for (int i = 0; i < d.atoms.size(); ++i)
                if (!d.atoms[i].stereo.isEmpty() && labeled.atoms[i].stereo != d.atoms[i].stereo)
                    fail(tr("配座探索の結果が記録済みの立体配置と異なります。"));
        }
    return result;
}
} // namespace mf
#ifdef MOLFORMA_OPENBABEL_DATADIR
// Last in the file: the macros of windows.h rename Open Babel's methods such as AddAtom.
#include <QDir>
#define NOMINMAX
#include <windows.h>
namespace mf {
// Open Babel built with MSVC finds its force fields and fragments only through BABEL_DATADIR: the data
// folder beside the installed executable, else the one of the Open Babel it was built with.
[[maybe_unused]] static const bool openBabelData = [] {
    if (!qEnvironmentVariableIsEmpty("BABEL_DATADIR"))
        return false;
    wchar_t path[MAX_PATH];
    const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const QDir beside = QFileInfo(QString::fromWCharArray(path, int(length))).dir();
    const auto data =
        beside.exists("data/UFF.prm") ? beside.filePath("data") : QString(MOLFORMA_OPENBABEL_DATADIR);
    return qputenv("BABEL_DATADIR", QDir::toNativeSeparators(data).toLocal8Bit());
}();
} // namespace mf
#endif
