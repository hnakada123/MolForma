#include "nwchem_output.h"
#include "nwchem_advanced.h"
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
    return nwchemSettings(d)["start"].toString() + '_' + suffix + ".cube";
}
} // namespace
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
    const int n = a["cube_divisions"].toInt(40);
    for (int axis = 0; axis < 3; ++axis) {
        const auto key = QString("cube_") + "xyz"[axis];
        if (a.contains(key)) {
            const auto values = a[key].toArray();
            grid.minimum[axis] = values.at(0).toDouble();
            grid.maximum[axis] = values.at(1).toDouble();
            grid.divisions[axis] = values.at(2).toInt();
        } else {
            double lo = 0, hi = 0;
            if (!d.atoms.isEmpty())
                lo = hi = d.atoms.first().position[axis];
            for (const auto &atom : d.atoms) {
                lo = std::min(lo, atom.position[axis]);
                hi = std::max(hi, atom.position[axis]);
            }
            grid.minimum[axis] = lo - pad;
            grid.maximum[axis] = hi + pad;
            grid.divisions[axis] = n;
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
    return files;
}
void validateNwchemOutput(const Document &d, NwchemValidation &v) {
    const auto a = nwchemAdvanced(d), s = nwchemSettings(d);
    const bool cube = a["cube_output"].toBool(), molden = a["molden_output"].toBool();
    if ((!cube && !molden) || !v.errors.isEmpty())
        return;
    if (std::any_of(d.atoms.cbegin(), d.atoms.cend(), [](const auto &atom) { return atom.ghost; }))
        v.errors << "CUBE・MOLDENのGUI出力はゴースト中心に対応していません。";
    if (s["method"] != "hf" && s["method"] != "dft")
        v.warnings
            << "CUBE・MOLDENは参照HFの軌道・密度を出力します。MP2/CCSDの相関密度・自然軌道ではありません。";
    if (s["task"].toString().contains("frequencies"))
        v.warnings << "可視化ファイル出力前に、振動数計算後の基準構造でHF/DFTエネルギーを再計算します。";
    if (cube) {
        if (a["cube_type"] == "orbital") {
            if (a["cube_spin"] == "beta" && v.reference != "uhf")
                v.errors
                    << "Betaの分子軌道CUBEにはUHF/UKSを指定してください。制限計算ではAlphaを使用します。";
            if (a["cube_orbital"] == "index" && !a.contains("cube_index"))
                v.errors << "CUBEに出力する分子軌道番号を指定してください。";
            else if (orbital(d, v) < 1)
                v.errors << "選択したスピンにはHOMOがありません。LUMOまたは軌道番号を指定してください。";
            v.warnings << "CUBEの軌道番号は実行時の軌道総数以下である必要があります。最小基底等ではLUMOが存在"
                          "しない場合があります。";
        }
        const auto grid = nwchemCubeGrid(d);
        if (grid.points() > 2000000 || grid.points() < 8)
            v.errors << "CUBEの格子点数は200万点までです。分割数を小さくしてください。";
        for (int axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(grid.minimum[axis]) || !std::isfinite(grid.maximum[axis]) ||
                grid.minimum[axis] < -100000 || grid.maximum[axis] > 100000 ||
                grid.minimum[axis] >= grid.maximum[axis])
                v.errors << "CUBEの範囲が不正です。範囲・余白を確認してください。";
        }
        if (s["task"].toString().startsWith("optimize"))
            v.warnings
                << "CUBEの格子範囲は入力構造から決めます。最適化後の構造に十分な余白を指定してください。";
    }
    const auto vectors =
        nwchemPermanentFile(d, a["vectors_output"].toString(s["start"].toString() + ".movecs"));
    for (const auto &file : nwchemOutputFiles(d, v))
        if (file == vectors)
            v.errors << "CUBE・MOLDENと出力movecsには別のファイル名を指定してください。";
    v.errors.removeDuplicates();
}
QString nwchemOutputLines(const Document &d, const NwchemValidation &v) {
    const auto a = nwchemAdvanced(d), s = nwchemSettings(d);
    if (!a["cube_output"].toBool() && !a["molden_output"].toBool())
        return {};
    const auto theory = s["method"] == "dft" ? QString("dft") : QString("scf");
    // DPLOT resolves the same vector path repeatedly for orbital views. Explicit ./ or absolute
    // paths bypass that resolver, avoiding a repeated relative permanent_dir prefix (NWChem 7.3.1).
    auto vectors = nwchemPermanentFile(d, a["vectors_output"].toString(s["start"].toString() + ".movecs"));
    if (!QDir::isAbsolutePath(vectors))
        vectors.prepend("./");
    QString text = "\n# Visualization of the " + theory + " reference orbitals\n";
    if (s["task"].toString().contains("frequencies"))
        text += "task " + theory + " energy\n";
    if (a["molden_output"].toBool())
        text += "\nproperty\n  vectors \"" + vectors + "\"\n  moldenfile\n  molden_norm " +
                a["molden_norm"].toString("nwchem") + "\nend\ntask " + theory + " property\n";
    if (a["cube_output"].toBool()) {
        text += "\ndplot\n  vectors \"" + vectors + "\"\n  limitxyz units angstrom\n";
        const auto grid = nwchemCubeGrid(d);
        for (int axis = 0; axis < 3; ++axis)
            text += "    " + number(grid.minimum[axis]) + ' ' + number(grid.maximum[axis]) + ' ' +
                    QString::number(grid.divisions[axis]) + '\n';
        const auto type = a["cube_type"].toString("density");
        const QMap<QString, QString> spins{{"density", "total"},
                                           {"alpha_density", "alpha"},
                                           {"beta_density", "beta"},
                                           {"spin_density", "spindens"}};
        text += "  spin " + (type == "orbital" ? a["cube_spin"].toString("alpha") : spins.value(type)) + '\n';
        if (type == "orbital")
            text += "  orbitals view; 1; " + QString::number(orbital(d, v)) + '\n';
        text += "  gaussian\n  output " + cubeName(d, v) + "\nend\ntask dplot\n";
    }
    return text;
}
} // namespace mf
