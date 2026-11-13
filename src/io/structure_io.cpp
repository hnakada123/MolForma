// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/structure_io.h"
#include "editing/chemistry.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "foundation/structure_file.h"
#include "io/babel_io.h"
#include "io/document_storage.h"
#include "io/mmcif.h"
#include "nwchem/nwchem_import.h"
#include "results/results.h"
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
    Document d;
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
        const Vec position(row.value(1).toDouble(&x), row.value(2).toDouble(&y), row.value(3).toDouble(&w));
        if (row.size() < 4 || !x || !y || !w)
            fail(tr(
                "Z-"
                "matrix形式・変数を使うGaussian入力は取り込めません。Cartesian座標の入力を使ってください。"));
        if (!position.allFinite())
            fail(tr("Gaussian入力の座標は有限の数値にしてください: %1").arg(lines[i]));
        Atom a;
        a.element = tag;
        if (tag == "D" || tag == "T") { // The isotopes of hydrogen, as in XYZ files.
            a.isotope = tag == "D" ? 2 : 3;
            a.element = "H";
        }
        element(a.element); // An element MolForma does not know fails here.
        a.position = position;
        d.atoms << a;
    }
    if (d.atoms.isEmpty())
        fail(tr("Gaussian入力に原子がありません。"));
    perceiveBonds(d); // By distance, with their orders from the geometry, as for XYZ.
    d.charge = charge;
    d.multiplicity = multiplicity;
    return d;
}
StructureImport importStructureWithNotes(const QString &path, const MacromoleculeSelection &selection,
                                         int xyzFrame, const CrystalSelection &crystal) {
    const auto ext = structureSuffix(path);
    if (ext == "json") {
        if (compressedFile(path))
            fail(tr("圧縮したプロジェクトは開けません。展開してから開いてください。"));
        return {loadProject(path), {}};
    }
    if (ext == "xyz") {
        const auto frames = xyzFrames(QString::fromUtf8(readStructureFile(path)));
        if (frames.size() > 1 && (xyzFrame < 0 || xyzFrame >= frames.size()))
            fail(tr("%1フレームのXYZです。読み込むフレーム（0〜%2）を指定してください。")
                     .arg(frames.size())
                     .arg(frames.size() - 1));
        return {xyzStructure(frames.value(std::max(0, xyzFrame))), {}};
    }
    if (ext == "nw" || ext == "nwinp") {
        const auto input = importNwchemInput(QString::fromUtf8(readStructureFile(path)));
        return {input.document, input.notes};
    }
    if (ext == "gjf" || ext == "com" || ext == "gau")
        return {gaussianStructure(QString::fromUtf8(readStructureFile(path))), {}};
    if (ext == "cif" || ext == "mmcif") {
        // A crystal structure of a small molecule, rather than an entry of the PDB.
        if (const auto bytes = readStructureFile(path); crystalCif(bytes)) {
            auto d = crystalStructure(readCrystal(bytes), crystal);
            auto meta = d.data["metadata"].toObject();
            meta["source_file"] = path;
            d.data["metadata"] = meta;
            return {d, {}};
        }
    }
    return {importBabelStructure(path, selection), {}};
}
Document importStructure(const QString &path, const MacromoleculeSelection &selection, int xyzFrame,
                         const CrystalSelection &crystal) {
    return importStructureWithNotes(path, selection, xyzFrame, crystal).document;
}
bool nebEndFileIsLog(const QString &path) {
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > MaximumNebEndFileBytes)
        fail(
            tr("%1 MiB以下の通常のファイルを指定してください。").arg(MaximumNebEndFileBytes / (1024 * 1024)));
    const auto suffix = info.suffix().toLower();
    return suffix == "out" || suffix == "log" || suffix == "nwo";
}
Document nebEndFromLog(const ResultData &log) {
    if (log.frames.isEmpty())
        fail(tr("このファイルに構造がありません。"));
    return log.frames.last().molecule;
}
Document nebEndFromFile(const QString &path) {
    if (nebEndFileIsLog(path))
        return nebEndFromLog(readResult(path));
    if (QFileInfo(path).suffix().toLower() == "xyz") {
        const int frames = int(xyzFrames(QString::fromUtf8(readFile(path))).size());
        return importStructure(path, {}, frames > 1 ? frames - 1 : -1);
    }
    return importStructure(path);
}
void exportStructure(const Document &document, const QString &path) {
    const auto ext = QFileInfo(path).suffix().toLower();
    if (ext == "json")
        saveProject(document, path);
    else if (ext == "xyz")
        writeFile(path, document.xyz().toUtf8());
    else
        exportBabelStructure(document, path);
}
} // namespace mf
