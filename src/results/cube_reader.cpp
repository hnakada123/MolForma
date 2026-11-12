// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/i18n.h"
#include "model/units.h"
#include "results/result_fields.h"
#include "results/result_parsing.h"
#include "results/results.h"
#include <Eigen/LU>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mf {
using namespace result_detail;
namespace result_detail {
// Reads a CUBE into the result (atoms, datasets, grid) and returns the values of r.dataset.
GridValues cubeValues(ResultData &r, QString text) {
    QTextStream stream(&text, QIODevice::ReadOnly);
    stream.readLine();
    stream.readLine();
    auto row = words(stream.readLine());
    if (row.size() < 4 || row.size() > 5)
        fail(tr("CUBEヘッダーが不正です。"));
    const int count = integer(row[0], -maxAtoms, maxAtoms);
    if (count == 0)
        fail(tr("原子を含まないCUBEは未対応です。"));
    int channels = row.size() == 5 ? integer(row[4], 1, 256) : 1;
    if (count < 0 && channels != 1)
        fail(tr("CUBEの軌道数指定が不正です。"));
    const Vec origin = point(row, 1, BohrToAngstrom);
    Mat axes;
    qint64 size = 1;
    for (int i = 0; i < 3; ++i) {
        row = words(stream.readLine());
        if (row.size() != 4)
            fail(tr("CUBEの格子軸が不正です。"));
        // NWChem Gaussian CUBE stores coordinates in Bohr. Reject ambiguous signed counts.
        r.grid[i] = integer(row[0], 2, maxPoints);
        size *= r.grid[i];
        if (size > maxPoints)
            fail(tr("CUBE格子は200万点まで読み込めます。"));
        axes.col(i) = point(row, 1, BohrToAngstrom);
    }
    if (std::abs(axes.determinant()) < 1e-15)
        fail(tr("CUBE格子の軸が独立ではありません。"));
    Document d;
    for (int i = 0; i < std::abs(count); ++i) {
        row = words(stream.readLine());
        if (row.size() != 5)
            fail(tr("CUBE原子表が不正です。"));
        d.atoms << atom(symbol(integer(row[0], 1, elements().last().number)), point(row, 2, BohrToAngstrom));
        number(row[1]); // Nuclear charge may differ from Z when an ECP is used.
    }
    addFrame(r, d, tr("CUBE原子座標"));
    // The values follow as whitespace-separated words (up to millions): read in place.
    const QStringView all(text);
    qsizetype at = stream.pos();
    auto token = [&] {
        while (at < all.size() && all[at].isSpace())
            ++at;
        const qsizetype start = at;
        while (at < all.size() && !all[at].isSpace())
            ++at;
        if (at == start)
            fail(tr("CUBEデータが途中で切れています。"));
        return all.mid(start, at - start);
    };
    if (count < 0) {
        channels = integer(token(), 1, 256);
        for (int i = 0; i < channels; ++i)
            r.datasets << integer(token(), 1, 1000000);
        if (QSet<int>(r.datasets.begin(), r.datasets.end()).size() != channels)
            fail(tr("CUBEデータ番号が重複しています。"));
    } else
        for (int i = 0; i < channels; ++i)
            r.datasets << i + 1;
    if (size * channels > 4000000)
        fail(tr("CUBE全データは400万値まで読み込めます。"));
    if (r.dataset < 0 || r.dataset >= channels)
        fail(tr("CUBEのデータ番号が範囲外です。"));
    QVector<double> values;
    values.reserve(size);
    for (qint64 i = 0; i < size; ++i)
        for (int channel = 0; channel < channels; ++channel) {
            double value = number(token());
            if (channel == r.dataset)
                values << value;
        }
    if (!all.mid(at).trimmed().isEmpty())
        fail(tr("CUBE格子数とデータ数が一致しません。"));
    return {values, origin, axes};
}
// The surface of the CUBE's values at r.isovalue.
void cubeSurface(ResultData &r, const GridValues &g) {
    r.values = g.values;
    r.gridOrigin = g.origin;
    r.gridAxes = g.axes;
    auto bounds = std::minmax_element(g.values.begin(), g.values.end());
    r.minimum = *bounds.first;
    r.maximum = *bounds.second;
    mesh(r, g.values, g.origin, g.axes);
    if (r.surface.isEmpty())
        r.warnings << tr("指定した等値の面がありません。等値とデータ範囲を確認してください。");
    r.warnings << tr(
        "CUBEの値はファイルの単位を使用します。赤が正、青が負です。密度と軌道の区別はタイトル・生成"
        "条件を確認してください。");
}
} // namespace result_detail
void combineCubes(const QVector<CubeTerm> &terms, const QString &output) {
    if (terms.isEmpty() || terms.size() > 16)
        fail(tr("組み合わせるCUBEは1〜16個で指定してください。"));
    const QFileInfo target(output);
    const auto cube = [](const QFileInfo &file) {
        return file.suffix().compare("cube", Qt::CaseInsensitive) == 0 ||
               file.suffix().compare("cub", Qt::CaseInsensitive) == 0;
    };
    if (!cube(target))
        fail(tr("書き出すCUBEの拡張子は.cubeまたは.cubにしてください。"));
    ResultData first;
    GridValues grid;
    QVector<double> sum;
    QStringList parts;
    for (const auto &term : terms) {
        const QFileInfo source(term.path);
        if (!cube(source))
            fail(tr("CUBEファイル（.cube・.cub）を指定してください: %1").arg(source.fileName()));
        if (!std::isfinite(term.factor) || std::abs(term.factor) > 1e6)
            fail(tr("係数は絶対値が10⁶以下の数で指定してください。"));
        if (source.absoluteFilePath() == target.absoluteFilePath() ||
            (target.exists() && source.canonicalFilePath() == target.canonicalFilePath()))
            fail(tr("書き出すファイルには、組み合わせるCUBEとは別のファイルを指定してください。"));
        ResultData r;
        r.dataset = term.dataset;
        const auto values = cubeValues(r, QString::fromUtf8(boundedRead(term.path)));
        if (sum.isEmpty()) {
            first = r;
            grid = values;
            sum.fill(0, values.values.size());
        } else if (r.grid != first.grid || (values.origin - grid.origin).cwiseAbs().maxCoeff() > 1e-4 ||
                   (values.axes - grid.axes).cwiseAbs().maxCoeff() > 1e-5)
            fail(tr("CUBEの格子が %1 と一致しません: "
                    "%2。格子点数・原点・間隔が同じCUBEだけを組み合わせられます"
                    "（「詳細設定」の「軌道ファイル」でX・Y・Zの範囲と分割数を同じ値にして出力し直してくださ"
                    "い）。")
                     .arg(QFileInfo(terms.first().path).fileName(), source.fileName()));
        for (qsizetype i = 0; i < sum.size(); ++i)
            sum[i] += term.factor * values.values[i];
        parts << QString("%1%2 %3")
                     .arg(term.factor < 0 ? "" : "+")
                     .arg(QString::number(term.factor, 'g', 10), source.fileName());
    }
    // Gaussian CUBE: lengths in Bohr, the values of each (x, y) column six to a line.
    const auto &atoms = first.frames.first().molecule.atoms;
    QByteArray text = "MolForma combination of CUBE files\n" + parts.join(' ').left(4000).toUtf8() + '\n';
    text.reserve(text.size() + 400 + 80 * atoms.size() + sum.size() * 14 + sum.size() / 6 + 1000);
    const auto bohr = [](const Vec &v) { return v / BohrToAngstrom; };
    const Vec origin = bohr(grid.origin);
    text +=
        QString::asprintf("%5d %12.6f %12.6f %12.6f\n", int(atoms.size()), origin.x(), origin.y(), origin.z())
            .toLatin1();
    for (int axis = 0; axis < 3; ++axis) {
        const Vec step = bohr(grid.axes.col(axis));
        text +=
            QString::asprintf("%5d %12.6f %12.6f %12.6f\n", first.grid[axis], step.x(), step.y(), step.z())
                .toLatin1();
    }
    for (const auto &atom : atoms) {
        const int z = element(atom.element).number;
        const Vec at = bohr(atom.position);
        text += QString::asprintf("%5d %12.6f %12.6f %12.6f %12.6f\n", z, double(z), at.x(), at.y(), at.z())
                    .toLatin1();
    }
    const int column = first.grid[2];
    char number[32];
    for (qsizetype i = 0; i < sum.size(); ++i) {
        std::snprintf(number, sizeof number, " %13.6E", sum[i]);
        text += number;
        if ((i % column) % 6 == 5 || i % column == column - 1)
            text += '\n';
    }
    QSaveFile file(output);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit())
        fail(tr("CUBEを書き出せません: %1").arg(file.errorString()));
}

} // namespace mf
