// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/xtb.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "model/geometry.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

namespace mf {
QStringList xtbMethods() {
    return {"gfn2", "gfn1", "gfnff"};
}
QString xtbMethodName(const QString &method) {
    return method == "gfn1"    ? QString("GFN1-xTB")
           : method == "gfnff" ? QString("GFN-FF")
                               : QString("GFN2-xTB");
}
QStringList xtbLevels() {
    return {"crude", "loose", "normal", "tight"};
}
QString xtbLevelName(const QString &level) {
    return level == "crude"   ? tr("粗い（crude）")
           : level == "loose" ? tr("緩い（loose）")
           : level == "tight" ? tr("厳しい（tight）")
                              : tr("標準（normal）");
}
QStringList xtbSolvents() {
    return {"acetone", "acetonitrile", "benzene", "ch2cl2",   "chcl3", "dmf",     "dmso",
            "ether",   "ethylacetate", "hexane",  "methanol", "thf",   "toluene", "water"};
}
QString xtbExecutable(const QString &setting) {
    const auto name = setting.trimmed().isEmpty() ? QString("xtb") : setting.trimmed();
    const auto path = name.contains('/') || name.contains('\\') ? name : QStandardPaths::findExecutable(name);
    const QFileInfo info(path);
    return !path.isEmpty() && info.isFile() && info.isExecutable() ? info.absoluteFilePath() : QString();
}
namespace {
// The atoms xtb keeps in place: the fixed ones, and all but the moving ones when those are given.
QVector<bool> fixedAtoms(const Document &d, const XtbOptions &options) {
    const QSet<QString> moving(options.moving.begin(), options.moving.end());
    QVector<bool> fixed(d.atoms.size());
    for (int i = 0; i < d.atoms.size(); ++i)
        fixed[i] = d.atoms[i].fixed || (!moving.isEmpty() && !moving.contains(d.atoms[i].id));
    return fixed;
}
// "1-3,7,9-12" for 1-based atom numbers.
QString ranges(const QVector<int> &numbers) {
    QStringList parts;
    for (int i = 0; i < numbers.size();) {
        int j = i;
        while (j + 1 < numbers.size() && numbers[j + 1] == numbers[j] + 1)
            ++j;
        parts << (j > i ? QString("%1-%2").arg(numbers[i]).arg(numbers[j]) : QString::number(numbers[i]));
        i = j + 1;
    }
    return parts.join(',');
}
} // namespace
QStringList prepareXtb(const Document &d, const XtbOptions &options, const QString &folder) {
    if (d.atoms.isEmpty())
        fail(tr("分子が空です。"));
    if (!xtbMethods().contains(options.method) || !xtbLevels().contains(options.level) ||
        (!options.solvent.isEmpty() && !xtbSolvents().contains(options.solvent)))
        fail(tr("xTBの条件が不正です。"));
    QString xyz = QString::number(d.atoms.size()) + "\nMolForma\n";
    for (const auto &a : d.atoms) {
        if (a.ghost)
            fail(tr("ゴースト原子を含む構造はxTBで最適化できません。"));
        if (element(a.element).number > 86)
            fail(tr("xTBはRnまでの元素に対応します（%1）。").arg(a.element));
        xyz += QString("%1 %2 %3 %4\n")
                   .arg(a.element)
                   .arg(a.position.x(), 0, 'f', 10)
                   .arg(a.position.y(), 0, 'f', 10)
                   .arg(a.position.z(), 0, 'f', 10);
    }
    for (const auto &id : options.moving)
        d.atom(id); // Fails for an atom the document does not have.
    const auto fixed = fixedAtoms(d, options);
    QVector<int> numbers;
    for (int i = 0; i < fixed.size(); ++i)
        if (fixed[i])
            numbers << i + 1;
    if (numbers.size() == d.atoms.size())
        fail(tr("動かせる原子がありません（すべて固定、または選択した原子がすべて固定）。"));
    const QDir dir(folder);
    writeFile(dir.filePath("input.xyz"), xyz.toUtf8());
    QStringList arguments{"input.xyz", "--opt", options.level};
    if (options.method == "gfnff")
        arguments << "--gfnff";
    else
        arguments << "--gfn" << (options.method == "gfn1" ? "1" : "2");
    arguments << "--chrg" << QString::number(d.charge) << "--uhf" << QString::number(d.multiplicity - 1);
    if (!options.solvent.isEmpty())
        arguments << "--alpb" << options.solvent;
    if (!numbers.isEmpty()) {
        // Exact fixing: xtb leaves these atoms where they are.
        writeFile(dir.filePath("xcontrol"), ("$fix\n   atoms: " + ranges(numbers) + "\n$end\n").toUtf8());
        arguments << "--input" << "xcontrol";
    }
    return arguments;
}
XtbResult readXtb(const Document &d, const XtbOptions &options, const QString &folder,
                  const QString &output) {
    const QDir dir(folder);
    const auto log = QString::fromUtf8(readFile(dir.filePath(output)).right(1 << 20));
    XtbResult result;
    result.converged = log.contains("GEOMETRY OPTIMIZATION CONVERGED");
    const auto structure = dir.filePath("xtbopt.xyz");
    if (!QFileInfo(structure).isFile()) {
        // xtb's error lines ("[ERROR] …", "-1- …"), else the end of its output.
        QStringList errors;
        for (const auto &line : log.split('\n'))
            if (line.contains("[ERROR]") || line.contains(QRegularExpression("^\\s*-\\d+-\\s")) ||
                line.contains("#ERROR"))
                errors << line.trimmed();
        fail(tr("xTBの最適化に失敗しました。\n%1")
                 .arg(errors.isEmpty() ? log.trimmed().right(800) : errors.mid(0, 8).join('\n')));
    }
    const auto lines = QString::fromUtf8(readFile(structure)).split('\n');
    if (lines.size() < d.atoms.size() + 2 || lines[0].trimmed().toInt() != d.atoms.size())
        fail(tr("xTBの結果（xtbopt.xyz）の原子数が分子と一致しません。"));
    static const QRegularExpression energy(R"(energy:\s*(-?[0-9.]+(?:[eE][-+]?\d+)?))");
    if (const auto m = energy.match(lines[1]); m.hasMatch())
        result.energy = m.captured(1).toDouble();
    Document moved = d;
    for (int i = 0; i < d.atoms.size(); ++i) {
        const auto row = lines[i + 2].split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        bool x = false, y = false, z = false;
        const Vec p(row.value(1).toDouble(&x), row.value(2).toDouble(&y), row.value(3).toDouble(&z));
        if (row.size() < 4 || !x || !y || !z || !p.allFinite() ||
            row[0].compare(d.atoms[i].element, Qt::CaseInsensitive) != 0)
            fail(tr("xTBの結果（xtbopt.xyz）の%1行目を読めません。").arg(i + 3));
        moved.atoms[i].position = p;
    }
    const auto fixed = fixedAtoms(d, options);
    if (!fixed.contains(true) && d.atoms.size() >= 2) {
        // Free molecules: fitted back, so that only the shape changes.
        const auto fit = superpose(moved, d);
        for (auto &a : moved.atoms)
            a.position = fit.rotation * a.position + fit.shift;
    }
    double squares = 0;
    for (int i = 0; i < d.atoms.size(); ++i) {
        const double distance = (moved.atoms[i].position - d.atoms[i].position).norm();
        if (fixed[i]) {
            if (distance > 1e-3)
                fail(tr("固定した原子%1がxTBの結果で %2 Å 動いています。")
                         .arg(i + 1)
                         .arg(distance, 0, 'f', 4));
            moved.atoms[i].position = d.atoms[i].position;
            continue;
        }
        squares += distance * distance;
        result.largest = std::max(result.largest, distance);
    }
    result.rmsd = std::sqrt(squares / d.atoms.size());
    result.document = moved;
    return result;
}
} // namespace mf
