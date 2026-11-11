// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_output.h"
#include "model/geometry.h"
#include "model/i18n.h"
#include "nwchem/input_builder.h"
#include "nwchem/nwchem_advanced.h"
#include <QDir>
#include <QLocale>
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
QString number(double value) {
    return QString::number(value, 'g', QLocale::FloatingPointShortest);
}
int orbital(const Document &d, const NwchemValidation &v) {
    const auto a = nwchemAdvanced(d);
    if (a["cube_orbital"] == "index")
        return a["cube_index"].toInt();
    const int homo = a["cube_spin"] == "beta" ? v.beta : v.alpha;
    return homo + (a["cube_orbital"] == "lumo" ? 1 : 0);
}
QString cubeName(const Document &d, const NwchemValidation &v) {
    const auto a = nwchemAdvanced(d);
    if (a.contains("cube_filename"))
        return a["cube_filename"].toString();
    QString suffix = a["cube_type"].toString("density");
    if (suffix == "orbital")
        suffix = a["cube_spin"].toString("alpha") + "_mo" + QString::number(orbital(d, v));
    else if (nwchemCubeExcited(suffix))
        suffix = suffix.section('_', 0, 0) + "_root" + QString::number(nwchemCubeRoot(d));
    return nwchemSettings(d)["start"].toString() + '_' + suffix + ".cube";
}
// A file NWChem writes into permanent_dir, as DPLOT reads it (see the vectors in nwchemOutputLines).
QString dplotFile(const Document &d, const QString &name) {
    auto path = nwchemPermanentFile(d, name);
    if (!QDir::isAbsolutePath(path))
        path.prepend("./");
    return path;
}
} // namespace
bool nwchemCubeExcited(const QString &type) {
    return type == "transition_density" || type == "excited_density" || type == "difference_density";
}
bool nwchemCubeGradient(const Document &d) {
    const auto a = nwchemAdvanced(d);
    return a["cube_output"].toBool() && nwchemSettings(d)["tddft"].toBool() &&
           (a["cube_type"] == "excited_density" || a["cube_type"] == "difference_density");
}
int nwchemCubeRoot(const Document &d) {
    return nwchemAdvanced(d)["cube_root"].toInt(1);
}
qint64 NwchemCubeGrid::points() const {
    return (qint64(divisions[0]) + 1) * (qint64(divisions[1]) + 1) * (qint64(divisions[2]) + 1);
}
QString nwchemPermanentFile(const Document &d, const QString &name) {
    if (QDir::isAbsolutePath(name) || name.startsWith("./") || name.startsWith("../"))
        return QDir::cleanPath(name);
    return QDir::cleanPath(QDir(nwchemAdvanced(d)["permanent_dir"].toString(".")).filePath(name));
}
NwchemCubeGrid nwchemCubeGrid(const Document &d) {
    NwchemCubeGrid grid;
    const auto a = nwchemAdvanced(d);
    const double pad = a["cube_padding"].toDouble(3);
    // Axes without their own range share one spacing: cube_divisions along the longest side.
    Vec lo = Vec::Zero(), hi = Vec::Zero();
    if (!d.atoms.isEmpty())
        lo = hi = d.atoms.first().position;
    for (const auto &atom : d.atoms) {
        lo = lo.cwiseMin(atom.position);
        hi = hi.cwiseMax(atom.position);
    }
    const auto uniform = uniformGrid(lo.array() - pad, hi.array() + pad, a["cube_divisions"].toInt(40));
    for (int axis = 0; axis < 3; ++axis) {
        const auto key = QString("cube_") + "xyz"[axis];
        if (a.contains(key)) {
            const auto values = a[key].toArray();
            grid.minimum[axis] = values.at(0).toDouble();
            grid.maximum[axis] = values.at(1).toDouble();
            grid.divisions[axis] = values.at(2).toInt();
        } else {
            grid.minimum[axis] = uniform.origin[axis];
            grid.maximum[axis] = uniform.origin[axis] + uniform.divisions[axis] * uniform.spacing;
            grid.divisions[axis] = uniform.divisions[axis];
        }
    }
    return grid;
}
QStringList nwchemOutputFiles(const Document &d, const NwchemValidation &v) {
    const auto a = nwchemAdvanced(d);
    QStringList files;
    if (a["molden_output"].toBool())
        files << nwchemPermanentFile(d, nwchemSettings(d)["start"].toString() + ".molden");
    if (a["cube_output"].toBool())
        files << nwchemPermanentFile(d, cubeName(d, v));
    for (const auto &kind : {QString("esp"), QString("efield")})
        if (nwchemSettings(d)["prop_" + kind + "_grid"].toBool())
            files << nwchemSettings(d)["start"].toString() + "_" + kind + "_grid.cube";
    return files;
}
void validateNwchemOutput(const Document &d, NwchemValidation &v) {
    const auto a = nwchemAdvanced(d), s = nwchemSettings(d);
    const bool cube = a["cube_output"].toBool(), molden = a["molden_output"].toBool();
    if ((!cube && !molden) || !v.errors.isEmpty())
        return;
    if (std::any_of(d.atoms.cbegin(), d.atoms.cend(), [](const auto &atom) { return atom.ghost; }))
        v.error("output.ghosts", "advanced:cube_output",
                tr("CUBE・MOLDENのGUI出力はゴースト中心に対応していません。"));
    const NwchemCalculation calculation(s);
    if (calculation.method != NwchemMethod::Hf && !calculation.dft())
        v.warning(
            "output.reference", "advanced:cube_output",
            tr("CUBE・MOLDENは参照HFの軌道・密度を出力します。MP2/CCSDの相関密度・自然軌道ではありません。"));
    if (calculation.frequencies())
        v.warning("output.frequencies", "advanced:cube_output",
                  tr("可視化ファイル出力前に、振動数計算後の基準構造でHF/DFTエネルギーを再計算します。"));
    if (cube) {
        if (nwchemCubeExcited(a["cube_type"].toString())) {
            const int roots = s["tddft_roots"].toInt();
            if (!s["tddft"].toBool())
                v.error(
                    "cube.tddft", "advanced:cube_type",
                    tr("遷移密度・励起状態の密度・差密度のCUBEには、「2 計算条件」の励起状態（TDDFT）が必要"
                       "です。"));
            else if (nwchemCubeRoot(d) > roots)
                v.error("cube.root", "advanced:cube_root",
                        tr("励起状態の番号は、求める励起状態の数（%1）以下にしてください。").arg(roots));
            if (nwchemCubeGradient(d)) {
                if (v.reference == "rhf" && s["tddft_triplets"].toBool())
                    v.error(
                        "cube.triplets", "tddft_triplets",
                        tr("三重項も求めるTDDFTでは、励起状態の密度・差密度のCUBEを出力できません。三重項を"
                           "外すか、遷移密度を選んでください。"));
                v.warning(
                    "cube.gradient", "advanced:cube_type",
                    tr("励起状態の密度・差密度は、TDDFTの勾配計算（task tddft gradient）で求めます。一点"
                       "エネルギーの計算より時間がかかります。"));
            } else if (v.reference == "rhf" && s["tddft_triplets"].toBool())
                v.warning("cube.singlet", "advanced:cube_root",
                          tr("遷移密度は一重項の励起状態のものです。番号は一重項の中で数えます。"));
        }
        if (a["cube_type"] == "orbital") {
            if (a["cube_spin"] == "beta" && v.reference != "uhf")
                v.error(
                    "cube.spin", "advanced:cube_spin",
                    tr("Betaの分子軌道CUBEにはUHF/UKSを指定してください。制限計算ではAlphaを使用します。"));
            if (a["cube_orbital"] == "index" && !a.contains("cube_index"))
                v.error("cube.index", "advanced:cube_index",
                        tr("CUBEに出力する分子軌道番号を指定してください。"));
            else if (orbital(d, v) < 1)
                v.error("cube.homo", "advanced:cube_orbital",
                        tr("選択したスピンにはHOMOがありません。LUMOまたは軌道番号を指定してください。"));
            v.warning("cube.orbitals", "advanced:cube_index",
                      tr("CUBEの軌道番号は実行時の軌道総数以下である必要があります。最小基底等ではLUMOが存在"
                         "しない場合があります。"));
        }
        const auto grid = nwchemCubeGrid(d);
        if (grid.points() > 2000000 || grid.points() < 8)
            v.error("cube.points", "advanced:cube_divisions",
                    tr("CUBEの格子点数は200万点までです。分割数を小さくしてください。"));
        for (int axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(grid.minimum[axis]) || !std::isfinite(grid.maximum[axis]) ||
                grid.minimum[axis] < -100000 || grid.maximum[axis] > 100000 ||
                grid.minimum[axis] >= grid.maximum[axis])
                v.error("cube.range", "advanced:cube_padding",
                        tr("CUBEの範囲が不正です。範囲・余白を確認してください。"));
        }
        if (calculation.driver())
            v.warning(
                "cube.padding", "advanced:cube_padding",
                tr("CUBEの格子範囲は入力構造から決めます。最適化後の構造に十分な余白を指定してください。"));
    }
    const auto vectors =
        nwchemPermanentFile(d, a["vectors_output"].toString(s["start"].toString() + ".movecs"));
    for (const auto &file : nwchemOutputFiles(d, v))
        if (file == vectors)
            v.error("output.same_path", "advanced:vectors_output",
                    tr("CUBE・MOLDENと出力movecsには別のファイル名を指定してください。"));
    v.errors.removeDuplicates();
}
NwchemGeneratedInput nwchemOutputLines(const Document &d, const NwchemValidation &v) {
    const auto a = nwchemAdvanced(d), s = nwchemSettings(d);
    if (!a["cube_output"].toBool() && !a["molden_output"].toBool())
        return {};
    const NwchemCalculation calculation(s);
    const auto theory = calculation.dft() ? QString("dft") : QString("scf");
    // DPLOT resolves the same vector path repeatedly for orbital views. Explicit ./ or absolute
    // paths bypass that resolver, avoiding a repeated relative permanent_dir prefix (NWChem 7.3.1).
    auto vectors = nwchemPermanentFile(d, a["vectors_output"].toString(s["start"].toString() + ".movecs"));
    if (!QDir::isAbsolutePath(vectors))
        vectors.prepend("./");
    NwchemInputBuilder out;
    out.append("\n# Visualization of the " + theory + " reference orbitals\n");
    if (calculation.frequencies())
        out.advancedBlock("task", "task " + theory + " energy\n", {"molden_output", "cube_output"},
                          "visualization");
    const bool properties = s["prop_mulliken"].toBool() || s["prop_nmr"].toBool() ||
                            s["prop_polarizability"].toBool() || s["prop_epr"].toBool() ||
                            s["prop_esp_grid"].toBool() || s["prop_efield_grid"].toBool();
    if (a["molden_output"].toBool()) {
        if (properties)
            out.advancedBlock("unset", "\nunset prop:*\n", {"molden_output"}, "visualization");
        NwchemInputBuilder body;
        body.sourced(QString(properties ? "property\n" : "\nproperty\n") + "  vectors \"" + vectors +
                         "\"\n  moldenfile\n",
                     {"molden_output", "vectors_output"}, true);
        body.sourced("  molden_norm " + a["molden_norm"].toString("nwchem") + "\n",
                     {"molden_output", "molden_norm"}, true);
        body.sourced("end\n", {"molden_output"}, true);
        out.block("property", body.result, {}, "visualization");
        out.advancedBlock("task", "task " + theory + " property\n", {"molden_output"}, "visualization");
    }
    if (a["cube_output"].toBool()) {
        NwchemInputBuilder body;
        // A density matrix takes the place of the orbitals.
        if (nwchemCubeGradient(d))
            body.sourced("\ndplot\n", {"cube_output"}, true);
        else
            body.sourced("\ndplot\n  vectors \"" + vectors + "\"\n", {"cube_output", "vectors_output"}, true);
        body.sourced("  limitxyz units angstrom\n", {"cube_output"}, true);
        const auto grid = nwchemCubeGrid(d);
        for (int axis = 0; axis < 3; ++axis) {
            const auto key = QString("cube_%1").arg(QString("xyz")[axis]);
            QStringList keys{"cube_output"};
            if (a.contains(key))
                keys << key;
            else
                keys << "cube_padding" << "cube_divisions";
            body.sourced("    " + number(grid.minimum[axis]) + ' ' + number(grid.maximum[axis]) + ' ' +
                             QString::number(grid.divisions[axis]) + '\n',
                         keys, true);
        }
        const auto type = a["cube_type"].toString("density");
        const auto start = s["start"].toString();
        // An excited state: the transition density from TDDFT's CI vectors (of the singlets of a closed
        // shell), or the density matrices a TDDFT gradient wrote (start.dmat, start.diffdmat).
        if (type == "transition_density")
            body.sourced("  civecs \"" +
                             dplotFile(d, start + (v.reference == "rhf" ? ".civecs_singlet" : ".civecs")) +
                             "\"\n  root " + QString::number(nwchemCubeRoot(d)) + '\n',
                         {"cube_output", "cube_type", "cube_root"}, true);
        else if (type == "excited_density" || type == "difference_density")
            body.sourced("  densmat \"" +
                             dplotFile(d, start + (type == "excited_density" ? ".dmat" : ".diffdmat")) +
                             "\"\n",
                         {"cube_output", "cube_type", "cube_root"}, true);
        const QMap<QString, QString> spins{
            {"density", "total"},           {"alpha_density", "alpha"},      {"beta_density", "beta"},
            {"spin_density", "spindens"},   {"transition_density", "total"}, {"excited_density", "total"},
            {"difference_density", "total"}};
        body.sourced("  spin " + (type == "orbital" ? a["cube_spin"].toString("alpha") : spins.value(type)) +
                         '\n',
                     {"cube_output", "cube_type", "cube_spin"}, true);
        if (type == "orbital")
            body.sourced("  orbitals view; 1; " + QString::number(orbital(d, v)) + '\n',
                         {"cube_output", "cube_orbital", "cube_index"}, true);
        body.sourced("  gaussian\n  output " + cubeName(d, v) + "\nend\n", {"cube_output", "cube_filename"},
                     true);
        out.block("dplot", body.result, {}, "visualization");
        out.advancedBlock("task", "task dplot\n", {"cube_output"}, "visualization");
    }
    return out.result;
}
} // namespace mf
