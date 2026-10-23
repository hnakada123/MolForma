#include "results.h"
#include "coordinates.h"
#include "molden_orbitals.h"
#include <Eigen/Geometry>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
constexpr qint64 maxBytes = 128 * 1024 * 1024;
constexpr int maxAtoms = 2000, maxFrames = 2000, maxPoints = 2000000, maxTriangles = 100000;
QStringList words(const QString &line) {
    static const QRegularExpression whitespace("\\s+");
    return line.split(whitespace, Qt::SkipEmptyParts);
}
double number(QString text) {
    text.replace('D', 'E', Qt::CaseInsensitive);
    bool ok;
    const double n = text.toDouble(&ok);
    if (!ok || !std::isfinite(n))
        fail("結果ファイルの数値が不正です: " + text.left(80));
    return n;
}
int integer(const QString &text, int lo, int hi) {
    const double n = number(text);
    if (n < lo || n > hi || n != std::floor(n))
        fail("結果ファイルの整数・個数が範囲外です: " + text.left(80));
    return int(n);
}
Vec point(const QStringList &w, int start, double scale = 1) {
    if (w.size() < start + 3)
        fail("結果ファイルの座標が不足しています。");
    const Vec p(number(w[start]), number(w[start + 1]), number(w[start + 2]));
    if (p.cwiseAbs().maxCoeff() > 1e6)
        fail("結果ファイルの座標が範囲外です。");
    return p * scale;
}
QString symbol(int z) {
    for (const auto &e : elements())
        if (e.number == z)
            return e.symbol;
    fail("このビューアで扱えない原子番号です: " + QString::number(z));
}
Atom atom(QString tag, const Vec &p) {
    Atom a;
    if (tag.startsWith('@')) {
        a.ghost = true;
        tag.remove(0, 1);
    }
    if (tag.startsWith("bq", Qt::CaseInsensitive)) {
        a.ghost = true;
        tag = tag.mid(2);
    }
    const auto match = QRegularExpression("^([A-Za-z]{1,2})[0-9_]*$").match(tag);
    if (!match.hasMatch())
        fail("未対応の原子タグです: " + tag.left(80));
    tag = match.captured(1).toLower();
    tag[0] = tag[0].toUpper();
    a.element = element(tag).symbol;
    a.position = p;
    return a;
}
void addFrame(ResultData &r, Document d, const QString &label) {
    if (d.atoms.isEmpty() || d.atoms.size() > maxAtoms || r.frames.size() >= maxFrames)
        fail("構造数または原子数が対応範囲外です（2000構造・2000原子まで）。");
    qsizetype total = d.atoms.size();
    for (const auto &frame : r.frames)
        total += frame.molecule.atoms.size();
    if (total > 200000)
        fail("構造列が大きすぎます（合計20万原子まで）。");
    d.inferBonds();
    r.frames.append({std::move(d), label});
}
QByteArray boundedRead(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail(f.errorString());
    if (f.size() > maxBytes)
        fail("結果ファイルは128 MiBまで読み込めます。ログを分割するか格子を小さくしてください。");
    auto data = f.read(maxBytes + 1);
    if (data.size() > maxBytes)
        fail("読込中に結果ファイルが128 MiBを超えました。");
    return data;
}
void xyz(ResultData &r, const QStringList &lines) {
    int i = 0;
    while (i < lines.size()) {
        if (lines[i].trimmed().isEmpty()) {
            ++i;
            continue;
        }
        const int count = integer(lines[i++].trimmed(), 1, maxAtoms);
        if (i + count >= lines.size())
            fail("XYZの構造が途中で切れています。");
        const QString title = lines[i++];
        Document d;
        for (int n = 0; n < count; ++n) {
            const auto row = words(lines[i++]);
            if (row.size() != 4)
                fail("結果XYZは元素・X・Y・Zの4列で指定してください。");
            d.atoms << atom(row[0], point(row, 1));
        }
        addFrame(r, d, title);
    }
}
void nwchem(ResultData &r, const QStringList &lines) {
    r.status = "正常終了マーカーなし（実行中・中断・不完全なログの可能性）";
    bool fatal = false, ended = false, recognized = false;
    static const QRegularExpression energy(
        R"(\bTotal\s+(SCF|DFT|MP2|CCSD(?:\(T\))?)\s+energy\s*[:=]?\s*([-+0-9.eEdD]+))",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression error(
        "failed to converge|convergence failure|not converged|MPI_ABORT|An error occurred|"
        "error termination|task.*failed|failed.*task|Calculation failed",
        QRegularExpression::CaseInsensitiveOption);
    for (int i = 0; i < lines.size(); ++i) {
        const auto &line = lines[i];
        recognized |= line.contains("NWChem");
        if (error.match(line).hasMatch()) {
            fatal = true;
            if (r.warnings.size() < 30)
                r.warnings << QString("行 %1: %2").arg(i + 1).arg(line.trimmed().left(250));
        }
        ended |= line.contains("Total times  cpu:");
        if (line.contains("Optimization converged"))
            r.warnings << "最適化の収束メッセージを検出しました。";
        auto match = energy.match(line);
        if (match.hasMatch()) {
            if (r.energies.size() >= 20000)
                fail("エネルギーの記録数が2万件を超えています。");
            r.energies << ResultEnergy{match.captured(1).toUpper(), number(match.captured(2)), i + 1};
        }
        if (line.contains("Output coordinates in ")) {
            const double scale = line.contains("angstroms") ? 1 : line.contains("a.u.") ? BohrToAngstrom : 0;
            if (!scale) {
                r.warnings << "未対応の座標単位を読み飛ばしました。";
                continue;
            }
            Document d;
            int j = i + 1;
            while (j < lines.size() && j < i + 10 && !lines[j].contains("----"))
                ++j;
            ++j;
            while (j < lines.size()) {
                const auto row = words(lines[j]);
                if (row.size() != 6 || !QRegularExpression("^\\d+$").match(row[0]).hasMatch())
                    break;
                if (integer(row[0], 1, maxAtoms) != d.atoms.size() + 1)
                    break;
                try {
                    d.atoms << atom(row[1], point(row, 3, scale));
                } catch (const std::exception &e) {
                    r.warnings << QString::fromUtf8(e.what());
                    d.atoms.clear();
                    break;
                }
                ++j;
            }
            // Only accept a terminated table, never a partial last row at EOF.
            if (!d.atoms.isEmpty() && j < lines.size() && lines[j].trimmed().isEmpty() &&
                (r.frames.isEmpty() || r.frames.first().molecule.atoms.size() == d.atoms.size()))
                addFrame(r, d, QString("ログ 行 %1").arg(i + 1));
            else
                r.warnings << QString("行 %1の不完全な座標表を読み飛ばしました。").arg(i + 1);
        }
        if (line.contains("(Projected Frequencies expressed")) {
            r.modes.clear();
            if (!r.frames.isEmpty())
                r.vibrationGeometry = r.frames.last().molecule;
        }
        if (line.trimmed().startsWith("P.Frequency") && !r.vibrationGeometry.atoms.isEmpty()) {
            const auto frequencies = words(line).mid(1);
            QVector<ResultMode> modes;
            for (const auto &f : frequencies)
                modes.append({number(f), std::numeric_limits<double>::quiet_NaN(),
                              QVector<Vec>(r.vibrationGeometry.atoms.size(), Vec::Zero())});
            int j = i + 1, rowCount = 0;
            while (j < lines.size() && lines[j].trimmed().isEmpty())
                ++j;
            for (; j < lines.size() && rowCount < 3 * r.vibrationGeometry.atoms.size(); ++j) {
                auto row = words(lines[j]);
                if (row.size() != frequencies.size() + 1 || row[0] != QString::number(rowCount + 1))
                    break;
                for (int k = 0; k < modes.size(); ++k)
                    modes[k].displacement[rowCount / 3][rowCount % 3] = number(row[k + 1]);
                ++rowCount;
            }
            if (rowCount == 3 * r.vibrationGeometry.atoms.size())
                r.modes += modes;
            else
                r.warnings << "未完の振動モード表を読み飛ばしました。";
        }
        if (line.contains("Projected Infra Red Intensities")) {
            for (int j = i + 1; j < lines.size(); ++j) {
                auto row = words(lines[j]);
                if (row.size() == 7 && row[2] == "||" &&
                    QRegularExpression("^\\d+$").match(row[0]).hasMatch()) {
                    const int n = integer(row[0], 1, 6000) - 1;
                    if (n < r.modes.size())
                        r.modes[n].intensity = number(row[5]);
                } else if (j > i + 3)
                    break;
            }
        }
    }
    if (!recognized)
        fail("NWChemの出力ログを認識できません。");
    if (fatal)
        r.status = "エラー・未収束メッセージあり（ログを確認）";
    else if (ended)
        r.status = "正常終了マーカーあり（収束・全taskの成否はログを確認）";
    r.warnings.removeDuplicates();
}
void mesh(ResultData &r, const QVector<double> &values, const Vec &origin, const Mat &axes);
void molden(ResultData &r, const QStringList &lines, int requestedOrbital) {
    QMap<QString, QStringList> blocks;
    QMap<QString, QString> headers;
    QString current;
    const QRegularExpression section("^\\s*\\[([^]]+)\\](.*)$");
    for (const auto &line : lines) {
        auto match = section.match(line);
        if (match.hasMatch()) {
            current = match.captured(1).trimmed().toUpper();
            if (blocks.contains(current))
                fail("MOLDENのセクションが重複しています: " + current);
            blocks[current] = {};
            headers[current] = match.captured(2).trimmed().toUpper();
        } else if (!current.isEmpty() && !line.trimmed().isEmpty())
            blocks[current] << line;
    }
    if (!blocks.contains("MOLDEN FORMAT"))
        fail("MOLDENヘッダーがありません。");
    if (blocks.contains("ATOMS")) {
        const auto units = headers["ATOMS"];
        if (units != "ANGS" && units != "AU" && units != "(ANGS)" && units != "(AU)")
            fail("MOLDEN [Atoms] の単位はAngsまたはAUを指定してください。");
        const double scale = units.contains("ANGS") ? 1 : BohrToAngstrom;
        Document d;
        for (const auto &line : blocks["ATOMS"]) {
            auto row = words(line);
            if (row.size() != 6 || integer(row[1], 1, maxAtoms) != d.atoms.size() + 1)
                fail("MOLDEN原子表の形式・番号が不正です。");
            d.atoms << atom(symbol(integer(row[2], 1, elements().last().number)), point(row, 3, scale));
        }
        addFrame(r, d, "MOLDEN原子座標");
        r.orbitalFrame = 0;
    }
    if (blocks.contains("GEOMETRIES")) {
        if (headers["GEOMETRIES"] != "XYZ")
            fail("MOLDENの構造列はXYZ形式に対応します。");
        // XYZ titles may be blank; use original lines to preserve them.
        QStringList geometry;
        bool active = false;
        for (const auto &line : lines) {
            if (line.trimmed().startsWith('[')) {
                if (active)
                    break;
                active = line.contains("[GEOMETRIES]", Qt::CaseInsensitive);
            } else if (active)
                geometry << line;
        }
        xyz(r, geometry);
    }
    ResultOrbital orbital;
    QVector<QVector<double>> coefficients;
    QVector<double> mo;
    QSet<int> seenCoefficients;
    qint64 totalCoefficients = 0;
    QSet<QString> fields;
    auto finishOrbital = [&] {
        if (fields.isEmpty())
            return;
        if (!fields.contains("ENE") || !fields.contains("OCCUP") || !fields.contains("SPIN"))
            fail("MOLDENの軌道情報が不足しています。");
        if (orbital.occupation < 0 || orbital.occupation > 2 ||
            (orbital.spin != "Alpha" && orbital.spin != "Beta"))
            fail("MOLDENの軌道占有数・スピンが不正です。");
        if (r.orbitals.size() >= 20000)
            fail("MOLDENの軌道は2万件まで読み込めます。");
        r.orbitals << orbital;
        totalCoefficients += mo.size();
        if (totalCoefficients > 4000000)
            fail("MOLDENのMO係数は400万個までです。");
        coefficients << mo;
        mo.clear();
        seenCoefficients.clear();
        orbital = {};
        fields.clear();
    };
    for (const auto &line : blocks["MO"]) {
        const auto pair = line.split('=');
        if (pair.size() != 2) {
            const auto row = words(line);
            if (row.size() != 2 || fields.isEmpty())
                fail("MOLDENのMO係数の行が不正です。");
            const int ao = integer(row[0], 1, 4000) - 1;
            if (seenCoefficients.contains(ao))
                fail("MOLDENのMO係数番号が重複しています。");
            seenCoefficients.insert(ao);
            if (mo.size() <= ao)
                mo.resize(ao + 1);
            mo[ao] = number(row[1]);
            continue;
        }
        const QString key = pair[0].trimmed().toUpper(), value = pair[1].trimmed();
        if (key == "SYM" || fields.contains(key))
            finishOrbital();
        fields.insert(key);
        if (key == "SYM")
            orbital.symmetry = value;
        else if (key == "ENE")
            orbital.energy = number(value);
        else if (key == "OCCUP")
            orbital.occupation = number(value);
        else if (key == "SPIN")
            orbital.spin = value.left(1).toUpper() + value.mid(1).toLower();
    }
    finishOrbital();
    if (!r.orbitals.isEmpty()) {
        r.orbitalBasis = blocks.contains("GTO") && r.orbitalFrame >= 0;
        try {
            if (!r.orbitalBasis)
                fail("軌道の形状表示には[Atoms]・[GTO]・[MO]が必要です。");
            const auto &atoms = r.frames[r.orbitalFrame].molecule;
            const auto wavefunction = moldenWavefunction(blocks, atoms, coefficients, r.normalizationRequest);
            int selected = requestedOrbital;
            if (selected < 0) {
                selected = frontierOrbital(r.orbitals, "Alpha");
                if (selected < 0)
                    selected = frontierOrbital(r.orbitals, "Beta");
                if (selected < 0)
                    selected = 0;
            }
            Vec lo = atoms.atoms.first().position, hi = lo;
            for (const auto &atom : atoms.atoms) {
                lo = lo.cwiseMin(atom.position);
                hi = hi.cwiseMax(atom.position);
            }
            lo.array() -= 3.;
            hi.array() += 3.;
            const Vec step = (hi - lo) / r.orbitalDivisions;
            r.grid.fill(r.orbitalDivisions + 1);
            const auto values = moldenOrbitalGrid(wavefunction, selected, lo, step, r.grid);
            r.minimum = *std::min_element(values.cbegin(), values.cend());
            r.maximum = *std::max_element(values.cbegin(), values.cend());
            mesh(r, values, lo, step.asDiagonal());
            r.selectedOrbital = selected;
            r.orbitalNormalization = wavefunction.normalization;
            if (r.surface.isEmpty())
                r.warnings << "指定した等値の面がありません。「軌道」の等値を小さくしてください。";
            r.warnings
                << "MOLDENの分子軌道を表示しています。赤が正、青が負です。全体の位相の符号は任意です。";
        } catch (const std::exception &e) {
            r.surface.clear();
            r.grid = {};
            if (requestedOrbital >= 0)
                throw;
            r.orbitalError = QString::fromUtf8(e.what());
            r.warnings << "軌道形状を表示できません: " + r.orbitalError;
        }
    }
    if (blocks.contains("FR-COORD")) {
        Document d;
        for (const auto &line : blocks["FR-COORD"]) {
            auto row = words(line);
            if (row.size() != 4)
                fail("MOLDENの振動基準座標が不正です。");
            d.atoms << atom(row[0], point(row, 1, BohrToAngstrom));
        }
        addFrame(r, d, "MOLDEN振動基準座標");
        r.vibrationGeometry = r.frames.last().molecule;
    }
    for (const auto &line : blocks["FREQ"]) {
        if (r.modes.size() >= 6000)
            fail("振動モード数が大きすぎます。");
        r.modes.append({number(line.trimmed()), std::numeric_limits<double>::quiet_NaN(), {}});
    }
    const auto norm = blocks["FR-NORM-COORD"];
    QSet<int> seen;
    for (int i = 0; i < norm.size();) {
        auto row = words(norm[i++]);
        if (row.size() != 2 || row[0].toLower() != "vibration" || r.vibrationGeometry.atoms.isEmpty())
            fail("MOLDEN振動モードのヘッダー・基準座標が不正です。");
        const int mode = integer(row[1], 1, r.modes.size()) - 1;
        if (seen.contains(mode))
            fail("振動モードの番号が重複しています。");
        seen.insert(mode);
        for (int a = 0; a < r.vibrationGeometry.atoms.size(); ++a) {
            if (i >= norm.size())
                fail("MOLDEN振動モードが途中で切れています。");
            r.modes[mode].displacement << point(words(norm[i++]), 0, BohrToAngstrom);
        }
    }
    const auto intensities = blocks["INT"];
    if (!intensities.isEmpty() && intensities.size() != r.modes.size())
        fail("MOLDENの振動数とIR強度の件数が一致しません。");
    for (int i = 0; i < intensities.size(); ++i)
        r.modes[i].intensity = number(words(intensities[i]).first());
    bool energy = false;
    for (const auto &line : blocks["GEOCONV"]) {
        if (line.trimmed().toLower() == "energy") {
            energy = true;
            continue;
        }
        if (line.trimmed().front().isLetter()) {
            energy = false;
            continue;
        }
        if (energy)
            r.energies << ResultEnergy{"GEOCONV", number(line.trimmed()), 0};
    }
}
void mesh(ResultData &r, const QVector<double> &values, const Vec &origin, const Mat &axes) {
    static const int corners[8][3]{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                   {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    static const int tetra[6][4]{{0, 1, 2, 6}, {0, 2, 3, 6}, {0, 3, 7, 6},
                                 {0, 7, 4, 6}, {0, 4, 5, 6}, {0, 5, 1, 6}};
    auto add = [&](const Vec &a, const Vec &b, const Vec &c, bool positive) {
        if ((b - a).cross(c - a).squaredNorm() < 1e-24)
            return;
        if (r.surface.size() >= maxTriangles)
            fail("等値面が10万三角形を超えました。等値を大きくするか格子を小さくしてください。");
        r.surface.append({a, b, c, positive});
    };
    for (int x = 0; x < r.grid[0] - 1; ++x)
        for (int y = 0; y < r.grid[1] - 1; ++y)
            for (int z = 0; z < r.grid[2] - 1; ++z) {
                std::array<Vec, 8> p;
                std::array<double, 8> v;
                for (int i = 0; i < 8; ++i) {
                    const int a = x + corners[i][0], b = y + corners[i][1], c = z + corners[i][2];
                    p[i] = origin + axes * Vec(a, b, c);
                    v[i] = values[(a * r.grid[1] + b) * r.grid[2] + c];
                }
                for (bool positive : {true, false}) {
                    const double level = positive ? r.isovalue : -r.isovalue;
                    const auto bounds = std::minmax_element(v.begin(), v.end());
                    if (level < *bounds.first || level > *bounds.second)
                        continue;
                    for (const auto &t : tetra) {
                        QVector<int> high, low;
                        for (int i : t)
                            (v[i] >= level ? high : low).append(i);
                        auto cut = [&](int a, int b) -> Vec {
                            return p[a] + (p[b] - p[a]) * ((level - v[a]) / (v[b] - v[a]));
                        };
                        if (high.size() == 1 || low.size() == 1) {
                            const auto &one = high.size() == 1 ? high : low;
                            const auto &three = high.size() == 1 ? low : high;
                            add(cut(one[0], three[0]), cut(one[0], three[1]), cut(one[0], three[2]),
                                positive);
                        } else if (high.size() == 2) {
                            const Vec a = cut(high[0], low[0]), b = cut(high[0], low[1]),
                                      c = cut(high[1], low[0]), d = cut(high[1], low[1]);
                            add(a, b, c, positive);
                            add(b, d, c, positive);
                        }
                    }
                }
            }
}
void cube(ResultData &r, QString text) {
    QTextStream stream(&text, QIODevice::ReadOnly);
    stream.readLine();
    stream.readLine();
    auto row = words(stream.readLine());
    if (row.size() < 4 || row.size() > 5)
        fail("CUBEヘッダーが不正です。");
    const int count = integer(row[0], -maxAtoms, maxAtoms);
    if (count == 0)
        fail("原子を含まないCUBEは未対応です。");
    int channels = row.size() == 5 ? integer(row[4], 1, 256) : 1;
    if (count < 0 && channels != 1)
        fail("CUBEの軌道数指定が不正です。");
    const Vec origin = point(row, 1, BohrToAngstrom);
    Mat axes;
    qint64 size = 1;
    for (int i = 0; i < 3; ++i) {
        row = words(stream.readLine());
        if (row.size() != 4)
            fail("CUBEの格子軸が不正です。");
        // NWChem Gaussian CUBE stores coordinates in Bohr. Reject ambiguous signed counts.
        r.grid[i] = integer(row[0], 2, maxPoints);
        size *= r.grid[i];
        if (size > maxPoints)
            fail("CUBE格子は200万点まで読み込めます。");
        axes.col(i) = point(row, 1, BohrToAngstrom);
    }
    if (std::abs(axes.determinant()) < 1e-15)
        fail("CUBE格子の軸が独立ではありません。");
    Document d;
    for (int i = 0; i < std::abs(count); ++i) {
        row = words(stream.readLine());
        if (row.size() != 5)
            fail("CUBE原子表が不正です。");
        d.atoms << atom(symbol(integer(row[0], 1, elements().last().number)), point(row, 2, BohrToAngstrom));
        number(row[1]); // Nuclear charge may differ from Z when an ECP is used.
    }
    addFrame(r, d, "CUBE原子座標");
    auto token = [&] {
        QString value;
        stream >> value;
        if (value.isEmpty())
            fail("CUBEデータが途中で切れています。");
        return value;
    };
    if (count < 0) {
        channels = integer(token(), 1, 256);
        for (int i = 0; i < channels; ++i)
            r.datasets << integer(token(), 1, 1000000);
        if (QSet<int>(r.datasets.begin(), r.datasets.end()).size() != channels)
            fail("CUBEデータ番号が重複しています。");
    } else
        for (int i = 0; i < channels; ++i)
            r.datasets << i + 1;
    if (size * channels > 4000000)
        fail("CUBE全データは400万値まで読み込めます。");
    if (r.dataset < 0 || r.dataset >= channels)
        fail("CUBEのデータ番号が範囲外です。");
    QVector<double> values;
    values.reserve(size);
    for (qint64 i = 0; i < size; ++i)
        for (int channel = 0; channel < channels; ++channel) {
            double value = number(token());
            if (channel == r.dataset)
                values << value;
        }
    if (!stream.readAll().trimmed().isEmpty())
        fail("CUBE格子数とデータ数が一致しません。");
    auto bounds = std::minmax_element(values.begin(), values.end());
    r.minimum = *bounds.first;
    r.maximum = *bounds.second;
    mesh(r, values, origin, axes);
    if (r.surface.isEmpty())
        r.warnings << "指定した等値の面がありません。等値とデータ範囲を確認してください。";
    r.warnings << "CUBEの値はファイルの単位を使用します。赤が正、青が負です。密度と軌道の区別はタイトル・生成"
                  "条件を確認してください。";
}
} // namespace
ResultData readResult(const QString &path, double iso, int dataset, int orbital, const QString &normalization,
                      int orbitalDivisions) {
    if (!std::isfinite(iso) || iso <= 0 || iso > 1e6)
        fail("等値は0より大きい有限の値で指定してください。");
    const auto bytes = boundedRead(path);
    const auto text = QString::fromUtf8(bytes);
    ResultData r;
    r.source = QFileInfo(path).absoluteFilePath();
    r.isovalue = iso;
    r.dataset = dataset;
    if (orbitalDivisions < 16 || orbitalDivisions > 100)
        fail("MOLDENの格子分割数は16〜100を指定してください。");
    r.orbitalDivisions = orbitalDivisions;
    r.normalizationRequest = normalization;
    r.log = QString::fromUtf8(bytes.right(128 * 1024));
    if (bytes.size() > 128 * 1024)
        r.log.prepend("…末尾128 KiBを表示しています。\n");
    auto lines = text.split('\n');
    for (auto &line : lines)
        if (line.endsWith('\r'))
            line.chop(1);
    const auto suffix = QFileInfo(path).suffix().toLower();
    r.status = "ファイルを読み込みました（計算の成否は判定しません）";
    if (suffix == "cube" || suffix == "cub") {
        r.format = "CUBE";
        cube(r, text);
    } else if (text.trimmed().startsWith("[Molden Format]", Qt::CaseInsensitive)) {
        r.format = "MOLDEN";
        molden(r, lines, orbital);
    } else if (suffix == "xyz") {
        r.format = "XYZ";
        xyz(r, lines);
    } else {
        r.format = "NWChem";
        nwchem(r, lines);
    }
    if (r.format != "NWChem" && r.frames.isEmpty())
        fail("表示可能な原子座標がありません。");
    return r;
}
Document resultModeGeometry(const ResultData &r, int mode, double phase, double amplitude) {
    if (mode < 0 || mode >= r.modes.size() || !std::isfinite(phase) || !std::isfinite(amplitude) ||
        amplitude < 0 || amplitude > 2)
        fail("振動モード・表示振幅が不正です。");
    const auto &v = r.modes[mode].displacement;
    auto d = r.vibrationGeometry;
    if (v.size() != d.atoms.size() || v.isEmpty())
        fail("この振動モードには変位データがありません。");
    double scale = 0;
    for (const auto &p : v)
        scale = std::max(scale, p.norm());
    if (scale > 0)
        for (int i = 0; i < d.atoms.size(); ++i)
            d.atoms[i].position += v[i] * (std::sin(phase) * amplitude / scale);
    return d;
}
void saveResultData(const QString &path, const ResultData &r) {
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s.setVersion(QDataStream::Qt_6_4);
    s << QString("MolFormaResult2") << r.source << r.format << r.status << r.log << r.warnings;
    s << r.orbitalBasis << qint32(r.selectedOrbital) << qint32(r.orbitalFrame) << qint32(r.orbitalDivisions)
      << r.orbitalError << r.orbitalNormalization << r.normalizationRequest;
    auto doc = [&](const Document &d) { s << QJsonDocument(d.toJson()).toJson(QJsonDocument::Compact); };
    auto vec = [&](const Vec &v) { s << v.x() << v.y() << v.z(); };
    s << qint32(r.frames.size());
    for (const auto &f : r.frames) {
        doc(f.molecule);
        s << f.label;
    }
    s << qint32(r.energies.size());
    for (const auto &e : r.energies)
        s << e.method << e.hartree << qint32(e.line);
    s << qint32(r.orbitals.size());
    for (const auto &o : r.orbitals)
        s << o.symmetry << o.spin << o.energy << o.occupation;
    doc(r.vibrationGeometry);
    s << qint32(r.modes.size());
    for (const auto &m : r.modes) {
        s << m.frequency << m.intensity << qint32(m.displacement.size());
        for (const auto &v : m.displacement)
            vec(v);
    }
    s << qint32(r.surface.size());
    for (const auto &t : r.surface) {
        vec(t.a);
        vec(t.b);
        vec(t.c);
        s << t.positive;
    }
    s << r.datasets << qint32(r.dataset) << r.isovalue << r.minimum << r.maximum;
    for (int n : r.grid)
        s << qint32(n);
    if (s.status() != QDataStream::Ok)
        fail("結果の転送データを保存できません。");
    writeFile(path, bytes);
}
ResultData loadResultData(const QString &path) {
    auto bytes = boundedRead(path);
    QDataStream s(bytes);
    s.setVersion(QDataStream::Qt_6_4);
    QString magic;
    s >> magic;
    if (magic != "MolFormaResult2")
        fail("結果の転送形式が不正です。");
    ResultData r;
    s >> r.source >> r.format >> r.status >> r.log >> r.warnings;
    qint32 orbital, orbitalFrame, divisions;
    s >> r.orbitalBasis >> orbital >> orbitalFrame >> divisions >> r.orbitalError >> r.orbitalNormalization >>
        r.normalizationRequest;
    r.selectedOrbital = orbital;
    r.orbitalFrame = orbitalFrame;
    r.orbitalDivisions = divisions;
    auto count = [&](int max) {
        qint32 n;
        s >> n;
        if (s.status() != QDataStream::Ok || n < 0 || n > max)
            fail("結果の転送件数が不正です。");
        return n;
    };
    auto doc = [&] {
        QByteArray b;
        s >> b;
        return Document::fromJson(parseJson(b));
    };
    auto vec = [&] {
        double x, y, z;
        s >> x >> y >> z;
        return Vec(x, y, z);
    };
    for (int n = count(maxFrames); n--;) {
        ResultFrame f;
        f.molecule = doc();
        s >> f.label;
        r.frames << f;
    }
    for (int n = count(20000); n--;) {
        ResultEnergy e;
        qint32 line;
        s >> e.method >> e.hartree >> line;
        e.line = line;
        r.energies << e;
    }
    for (int n = count(20000); n--;) {
        ResultOrbital o;
        s >> o.symmetry >> o.spin >> o.energy >> o.occupation;
        r.orbitals << o;
    }
    r.vibrationGeometry = doc();
    for (int n = count(6000); n--;) {
        ResultMode m;
        s >> m.frequency >> m.intensity;
        for (int k = count(maxAtoms); k--;)
            m.displacement << vec();
        r.modes << m;
    }
    for (int n = count(maxTriangles); n--;) {
        SurfaceTriangle t;
        t.a = vec();
        t.b = vec();
        t.c = vec();
        s >> t.positive;
        r.surface << t;
    }
    qint32 dataset;
    s >> r.datasets >> dataset >> r.isovalue >> r.minimum >> r.maximum;
    r.dataset = dataset;
    for (int &n : r.grid) {
        qint32 value;
        s >> value;
        n = value;
    }
    if (s.status() != QDataStream::Ok || !s.atEnd())
        fail("結果の転送データが不完全です。");
    return r;
}
int resultWorker(const QString &request, const QString &response) {
    try {
        const auto q = parseJson(boundedRead(request));
        saveResultData(response,
                       readResult(q["path"].toString(), q["isovalue"].toDouble(.03), q["dataset"].toInt(),
                                  q["orbital"].toInt(-1), q["normalization"].toString("auto"),
                                  q["orbital_divisions"].toInt(40)));
        return 0;
    } catch (const std::exception &e) {
        try {
            writeFile(response + ".error", QByteArray(e.what()).left(4096));
        } catch (...) {
        }
        return 1;
    }
}
QStringList resultFiles(const QString &directory) {
    auto files = QDir(directory).entryList(
        {"*.out", "*.log", "*.xyz", "*.cube", "*.cub", "*.molden", "*.mld", "*.molde"},
        QDir::Files | QDir::Readable, QDir::Name);
    // Generated files can live in permanent_dir; do not recursively scan it or trust the saved cwd.
    QFile record(QDir(directory).filePath("run.json"));
    if (record.open(QIODevice::ReadOnly) && record.size() <= 256 * 1024) {
        const auto m = QJsonDocument::fromJson(record.read(256 * 1024)).object();
        if (m["format"] == "molforma-nwchem-run" && m["version"].toDouble() == 1) {
            QSet<QString> known;
            for (const auto &file : files)
                known.insert(QFileInfo(QDir(directory).filePath(file)).canonicalFilePath());
            const auto outputs = m["visualization_files"].toArray();
            for (int i = 0; i < std::min(2, int(outputs.size())); ++i) {
                const auto name = outputs[i].toString();
                const auto clean = QDir::cleanPath(name);
                if (clean == ".." || clean.startsWith("../"))
                    continue;
                const QFileInfo info(QDir(directory).filePath(name));
                if (!QStringList{"cube", "cub", "molden"}.contains(info.suffix().toLower()) ||
                    !info.isFile() || !info.isReadable() || known.contains(info.canonicalFilePath()))
                    continue;
                known.insert(info.canonicalFilePath());
                files << name;
            }
        }
    }
    return files;
}
} // namespace mf
