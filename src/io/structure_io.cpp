// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/structure_io.h"
#include "editing/chemistry.h"
#include "model/i18n.h"
#include "model/structure_file.h"
#include "nwchem/nwchem_import.h"
#include <QFileInfo>
#include <QRegularExpression>

namespace mf {
QStringList xyzFrames(const QString &text) {
    QStringList frames;
    const auto lines = text.split('\n');
    int i = 0;
    while (i < lines.size()) {
        if (lines[i].trimmed().isEmpty()) {
            ++i;
            continue;
        }
        bool ok;
        const int count = lines[i].trimmed().toInt(&ok);
        if (!ok || count < 0 || count > 100000 || i + count + 2 > lines.size())
            fail(tr("XYZの%1行目の原子数が不正です。").arg(i + 1));
        frames << lines.mid(i, count + 2).join('\n') + '\n';
        i += count + 2;
        if (frames.size() > 100000)
            fail(tr("XYZのフレームが多すぎます。"));
    }
    if (frames.isEmpty())
        fail(tr("XYZに構造がありません。"));
    return frames;
}
Document gaussianStructure(const QString &text) {
    auto lines = text.split('\n');
    for (auto &line : lines)
        line = line.section('!', 0, 0).trimmed(); // Gaussian comments.
    int i = 0;
    while (i < lines.size() && (lines[i].isEmpty() || lines[i].startsWith('%')))
        ++i;
    if (i >= lines.size() || !lines[i].startsWith('#'))
        fail(tr("Gaussian入力のルート行（#で始まる行）がありません。"));
    for (int blank = 0; i < lines.size() && blank < 2; ++i)
        blank += lines[i].isEmpty(); // Route section, then the title section.
    if (i >= lines.size())
        fail(tr("Gaussian入力の電荷・多重度の行がありません。"));
    static const QRegularExpression separators("[\\s,]+");
    const auto header = lines[i++].split(separators, Qt::SkipEmptyParts);
    bool chargeOk, multiplicityOk;
    const int charge = header.value(0).toInt(&chargeOk),
              multiplicity = header.value(1).toInt(&multiplicityOk);
    if (!chargeOk || !multiplicityOk || multiplicity < 1)
        fail(tr("Gaussian入力の電荷・多重度の行が不正です。"));
    QString xyz;
    int count = 0;
    for (; i < lines.size() && !lines[i].isEmpty(); ++i) {
        auto row = lines[i].split(separators, Qt::SkipEmptyParts);
        // "C", "C(Fragment=1)", "C-CA-0.1" (ONIOM) or the atomic number; an optional freeze code.
        auto tag = row.value(0).section('(', 0, 0).section('-', 0, 0);
        bool numeric;
        const int z = tag.toInt(&numeric);
        if (numeric)
            for (const auto &e : elements())
                if (e.number == z)
                    tag = e.symbol;
        // An integer freeze code (0, -1, -2, -3) precedes the coordinates when a fourth number follows;
        // ONIOM layers and link atoms come after the coordinates.
        bool code, fourth;
        row.value(1).toInt(&code);
        row.value(4).toDouble(&fourth);
        if (row.size() >= 5 && code && fourth)
            row.removeAt(1);
        bool x, y, w;
        row.value(1).toDouble(&x);
        row.value(2).toDouble(&y);
        row.value(3).toDouble(&w);
        if (row.size() < 4 || !x || !y || !w)
            fail(tr(
                "Z-"
                "matrix形式・変数を使うGaussian入力は取り込めません。Cartesian座標の入力を使ってください。"));
        xyz += QString("%1 %2 %3 %4\n").arg(tag, row[1], row[2], row[3]);
        ++count;
    }
    if (!count)
        fail(tr("Gaussian入力に原子がありません。"));
    auto d = xyzStructure(QString("%1\nGaussian input\n").arg(count) + xyz);
    d.charge = charge;
    d.multiplicity = multiplicity;
    return d;
}
Document importStructure(const QString &path, int model, const QString &altloc, int xyzFrame) {
    const auto ext = structureSuffix(path);
    if (ext == "json") {
        if (compressedFile(path))
            fail(tr("圧縮したプロジェクトは開けません。展開してから開いてください。"));
        return Document::load(path);
    }
    if (ext == "xyz") {
        const auto frames = xyzFrames(QString::fromUtf8(readStructureFile(path)));
        if (frames.size() > 1 && (xyzFrame < 0 || xyzFrame >= frames.size()))
            fail(tr("%1フレームのXYZです。読み込むフレーム（0〜%2）を指定してください。")
                     .arg(frames.size())
                     .arg(frames.size() - 1));
        return xyzStructure(frames.value(std::max(0, xyzFrame)));
    }
    if (ext == "nw" || ext == "nwinp")
        return importNwchemInput(QString::fromUtf8(readStructureFile(path))).document;
    if (ext == "gjf" || ext == "com" || ext == "gau")
        return gaussianStructure(QString::fromUtf8(readStructureFile(path)));
    return importBabelStructure(path, model, altloc);
}
void exportStructure(const Document &document, const QString &path) {
    const auto ext = QFileInfo(path).suffix().toLower();
    if (ext == "json")
        document.save(path);
    else if (ext == "xyz")
        writeFile(path, document.xyz().toUtf8());
    else
        exportBabelStructure(document, path);
}
} // namespace mf
