// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/spectrum.h"
#include "model/i18n.h"
#include "results/result_summary.h"
#include <QSet>
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
constexpr double FreeElectronG = 2.00231930436256;
Mat tensor(const QJsonArray &values) {
    if (values.size() != 9)
        fail(tr("ESRには完全なg・Aテンソルが必要です。"));
    Mat result;
    for (int i = 0; i < 9; ++i) {
        if (!values[i].isDouble() || !std::isfinite(values[i].toDouble()))
            fail(tr("ESRには完全なg・Aテンソルが必要です。"));
        result(i / 3, i % 3) = values[i].toDouble();
    }
    return result;
}
double required(const QJsonObject &p, const QString &key) {
    if (!p[key].isDouble() || !std::isfinite(p[key].toDouble()))
        fail(tr("スペクトルの %1 に有限の数値を指定してください。").arg(key));
    return p[key].toDouble();
}
} // namespace
QString eprIsotope(const QString &element) {
    static const QMap<QString, QString> isotopes{{"H", "1H"},    {"C", "13C"},  {"N", "14N"},   {"O", "17O"},
                                                 {"F", "19F"},   {"P", "31P"},  {"Si", "29Si"}, {"S", "33S"},
                                                 {"Cl", "35Cl"}, {"Br", "79Br"}};
    return isotopes.value(element);
}
QJsonArray eprNuclei(const ResultData &result) {
    const Mat g =
        tensor(result.properties["epr_gshift_ppt"].toArray()) * .001 + Mat::Identity() * FreeElectronG;
    QJsonArray rows;
    for (const auto &value : result.properties["epr_hyperfine"].toArray()) {
        auto row = value.toObject();
        const Mat a = tensor(row["tensor_mhz"].toArray());
        row["isotope"] = eprIsotope(row["element"].toString());
        row["isotropic_mhz"] = (a * g / FreeElectronG).trace() / 3;
        rows.append(row);
    }
    return rows;
}
namespace {
double spectrumWidth(const QJsonObject &p) {
    const double width = required(p, "width");
    if (width <= 0 || width > 1e5)
        fail(tr("スペクトルの半値全幅は0より大きく100000以下にしてください。"));
    return width;
}
// The peaks, sticks and axes of a result for the kind of out.parameters.
void collectPeaks(const ResultData &result, SpectrumData &out) {
    const auto p = out.parameters;
    if (p["kind"] == "nmr") {
        if (result.properties.contains("spin_multiplicity") && result.properties["spin_multiplicity"] != 1)
            fail(tr("分裂なしNMRは一重項の反磁性分子を対象にしています。"));
        const auto nucleus = p["nucleus"].toString();
        if (nucleus != "1H" && nucleus != "13C")
            fail(tr("NMRの核種は1Hまたは13Cを指定してください。"));
        const auto element = nucleus == "1H" ? QString("H") : QString("C");
        const double reference = required(p, "reference_ppm");
        const auto weights = p["weights"].toObject();
        out.xUnit = "ppm";
        out.yUnit = tr("核の重み（任意単位）");
        out.reversed = true;
        QSet<int> atoms;
        for (const auto &value : result.properties["shielding"].toArray()) {
            const auto shielding = value.toObject();
            if (shielding["element"] != element)
                continue;
            const int atom = shielding["atom"].toInt();
            if (atom <= 0 || atoms.contains(atom))
                fail(tr("NMRの原子番号が不正または重複しています。"));
            atoms.insert(atom);
            const double sigma = required(shielding, "isotropic");
            const auto key = QString::number(atom);
            const double weight = weights.contains(key) ? required(weights, key) : 1;
            if (weight < 0 || weight > 1e6)
                fail(tr("ピークの重みは0以上1000000以下にしてください。"));
            out.peaks.append(QJsonObject{{"atom", atom},
                                         {"nucleus", nucleus},
                                         {"shielding_ppm", sigma},
                                         {"x", reference - sigma},
                                         {"weight", weight}});
            out.sticks.append({reference - sigma, weight});
        }
    } else if (p["kind"] == "raman") {
        out.xUnit = "cm⁻¹";
        out.yUnit = "Å⁴/amu";
        out.reversed = true;
        for (const auto &value : result.properties["raman"].toArray()) {
            const auto r = value.toObject();
            const auto frequency = required(r, "frequency"), activity = required(r, "activity");
            if (frequency <= 0 || activity < 0)
                continue;
            QJsonObject peak{{"x", frequency}, {"weight", activity}, {"raman_index", r["raman_index"]}};
            if (r.contains("mode"))
                peak["mode"] = r["mode"];
            out.peaks.append(peak);
            out.sticks.append({frequency, activity});
        }
    } else if (p["kind"] == "ord") {
        out.xUnit = "nm";
        out.yUnit = "βiso / a.u. (GIAO)";
        for (const auto &value : result.properties["ord"].toArray()) {
            auto row = value.toObject();
            if (row["unit"] != "au" || row["gauge"] != "giao")
                fail(tr("ORDの単位またはゲージに対応していません。"));
            row["x"] = required(row, "wavelength_nm");
            row["weight"] = required(row, "isotropic");
            out.peaks.append(row);
            out.sticks.append({row["x"].toDouble(), row["weight"].toDouble()});
        }
    } else if (p["kind"] == "ecd") {
        out.xUnit = "eV";
        out.yUnit = "R / 10⁻⁴⁰ esu² cm²";
        for (const auto &value : result.properties["ecd"].toArray()) {
            const auto row = value.toObject();
            if (row["unit"] != "1e-40 esu^2 cm^2" || row["gauge"] != "length")
                fail(tr("ECDの単位またはゲージに対応していません。"));
            const auto ev = required(row, "ev"), strength = required(row, "strength");
            if (ev <= 0)
                fail(tr("励起エネルギーは正の値が必要です。"));
            auto peak = row;
            peak["x"] = ev;
            peak["weight"] = strength;
            out.peaks.append(peak);
            out.sticks.append({ev, strength});
        }
    } else if (p["kind"] == "epr") {
        if (result.properties["spin_multiplicity"] != 2)
            fail(tr("溶液ESRは二重項（S＝1/2）の計算結果に対応します。"));
        const Mat g =
            tensor(result.properties["epr_gshift_ppt"].toArray()) * .001 + Mat::Identity() * FreeElectronG;
        const double iso = g.trace() / 3;
        if (!(iso > 0 && iso < 10))
            fail(tr("ESRの等方g値が範囲外です。"));
        const double microwave = required(p, "microwave_ghz") * 1000;
        if (!(microwave >= 100 && microwave <= 1000000))
            fail(tr("ESRのマイクロ波周波数は0.1〜1000 GHzにしてください。"));
        if (!p["nuclei"].isArray())
            fail(tr("ESRで含める核のリストを指定してください。"));
        const auto all = eprNuclei(result);
        QMap<int, QJsonObject> byAtom;
        for (const auto &value : all)
            byAtom[value.toObject()["atom"].toInt()] = value.toObject();
        QSet<int> used;
        QVector<QPointF> splittings{{0, 1}};
        double maximum = 0;
        for (const auto &value : p["nuclei"].toArray()) {
            const auto nucleus = value.toObject();
            const int atom = nucleus["atom"].toInt();
            const auto isotope = nucleus["isotope"].toString();
            if (!byAtom.contains(atom) || used.contains(atom) || isotope.isEmpty() ||
                byAtom[atom]["isotope"] != isotope)
                fail(tr("ESRの核種または原子番号が計算結果と一致しません。"));
            used.insert(atom);
            const int twiceSpin = isotope == "14N"                                       ? 2
                                  : isotope == "17O"                                     ? 5
                                  : QStringList{"33S", "35Cl", "79Br"}.contains(isotope) ? 3
                                                                                         : 1;
            if (splittings.size() * (twiceSpin + 1) > 4096)
                fail(tr("スペクトルは4096本までのピークに対応します。"));
            const double a = required(byAtom[atom], "isotropic_mhz");
            maximum += std::abs(a) * twiceSpin / 2;
            QVector<QPointF> next;
            for (const auto &s : splittings)
                for (int m = 0; m <= twiceSpin; ++m)
                    next.append({s.x() + a * (m - twiceSpin / 2.), s.y() / (twiceSpin + 1)});
            splittings = next;
        }
        if (maximum > microwave * .1)
            fail(tr("超微細結合が大きすぎるため高磁場近似のESRを表示できません。"));
        out.parameters["g_isotropic"] = iso;
        out.xUnit = "mT";
        out.yUnit = p["derivative"].toBool() ? tr("一次微分（任意単位）") : tr("吸収（任意単位）");
        for (const auto &s : splittings)
            out.sticks.append({(microwave - s.x()) / (iso * 13.99624555), s.y()});
        std::sort(out.sticks.begin(), out.sticks.end(),
                  [](const auto &a, const auto &b) { return a.x() < b.x(); });
        QVector<QPointF> merged;
        for (const auto &s : out.sticks) {
            if (!merged.isEmpty() && std::abs(merged.last().x() - s.x()) < 1e-7)
                merged.last().ry() += s.y();
            else
                merged.append(s);
        }
        out.sticks = merged;
        for (const auto &s : out.sticks)
            out.peaks.append(QJsonObject{{"x", s.x()}, {"weight", s.y()}});
    } else if (p["kind"] == "ir") {
        // As the IR chart of the result viewer: the real vibrations with an intensity.
        out.xUnit = "cm⁻¹";
        out.yUnit = "km/mol";
        out.reversed = true;
        const auto kinds = resultModeKinds(result);
        for (int i = 0; i < result.modes.size(); ++i) {
            const auto &m = result.modes[i];
            if (kinds[i] == ResultModeKind::Rigid || m.frequency <= 0 || !std::isfinite(m.intensity))
                continue;
            out.peaks.append(QJsonObject{{"x", m.frequency}, {"weight", m.intensity}, {"mode", i}});
            out.sticks.append({m.frequency, m.intensity});
        }
    } else if (p["kind"] == "uv") {
        // As the absorption chart of the result viewer: the states with an oscillator strength.
        out.xUnit = "eV";
        out.yUnit = tr("振動子強度");
        for (int i = 0; i < result.excitations.size(); ++i) {
            const auto &x = result.excitations[i];
            if (!(x.ev > 0) || !(x.oscillator > 0))
                continue;
            out.peaks.append(
                QJsonObject{{"x", x.ev}, {"weight", x.oscillator}, {"root", x.root}, {"excitation", i}});
            out.sticks.append({x.ev, x.oscillator});
        }
    } else {
        fail(tr("対応していないスペクトルの種類です。"));
    }
}
// The display range of the sticks and their broadened curve.
void broaden(SpectrumData &out) {
    const auto p = out.parameters;
    const double width = spectrumWidth(p);
    if (out.sticks.isEmpty())
        fail(tr("指定した種類のスペクトルに必要な計算結果がありません。"));
    double low = out.sticks.first().x(), high = low;
    for (const auto &point : out.sticks) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
            fail(tr("スペクトルに有限でない計算値があります。"));
        low = std::min(low, point.x());
        high = std::max(high, point.x());
    }
    const double margin = std::max(width * 8, (high - low) * .05);
    low = p.contains("minimum") ? required(p, "minimum") : low - margin;
    high = p.contains("maximum") ? required(p, "maximum") : high + margin;
    if (!(low < high) || high - low > 1e9)
        fail(tr("スペクトルの表示範囲が不正です。"));
    out.parameters["minimum"] = low;
    out.parameters["maximum"] = high;
    if (p["kind"] == "ord") {
        out.curve = out.sticks;
        std::sort(out.curve.begin(), out.curve.end(),
                  [](const auto &a, const auto &b) { return a.x() < b.x(); });
        for (int i = 1; i < out.curve.size(); ++i)
            if (std::abs(out.curve[i].x() - out.curve[i - 1].x()) < 1e-5)
                fail(tr("ORDに同じ波長の結果が重複しています。別の計算を分けて読み込んでください。"));
        return;
    }
    // Include each peak position as well as the regular grid so narrow peaks are never missed (the
    // many sticks of a conformer set are drawn over the grid alone).
    QVector<double> xs;
    for (int i = 0; i <= 4096; ++i)
        xs.append(low + (high - low) * i / 4096);
    if (out.sticks.size() <= 4096)
        for (const auto &point : out.sticks)
            for (double offset : {0., -width / (2 * std::sqrt(3.)), width / (2 * std::sqrt(3.))})
                if (point.x() + offset >= low && point.x() + offset <= high)
                    xs.append(point.x() + offset);
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    // A Lorentzian of the given full width at half maximum, its derivative for ESR, or the Gaussian
    // of the absorption chart; the height at a peak is its weight.
    const double gamma = width / 2;
    const bool derivative = p["kind"] == "epr" && p["derivative"].toBool(), gaussian = p["kind"] == "uv";
    for (double x : xs) {
        double y = 0;
        for (const auto &point : out.sticks) {
            const double dx = (x - point.x()) / gamma;
            y += derivative ? -2 * point.y() * dx / (gamma * std::pow(1 + dx * dx, 2))
                 : gaussian ? point.y() * std::exp(-std::log(2.) * dx * dx)
                            : point.y() / (1 + dx * dx);
        }
        out.curve.append({x, y});
    }
}
} // namespace
SpectrumData calculateSpectrum(const ResultData &result, const QJsonObject &p) {
    SpectrumData out;
    out.parameters = p;
    spectrumWidth(p);
    collectPeaks(result, out);
    if (out.sticks.size() > 4096)
        fail(tr("スペクトルは4096本までのピークに対応します。"));
    broaden(out);
    return out;
}
QJsonObject spectrumContributions(const ResultData &result) {
    QJsonObject spectra;
    for (const auto *kind : {"ir", "raman", "uv", "ecd"}) {
        SpectrumData s;
        s.parameters = {{"kind", kind}};
        try {
            collectPeaks(result, s);
        } catch (const std::exception &) {
            continue; // Data MolForma does not draw, such as another ECD gauge.
        }
        if (!s.peaks.isEmpty())
            spectra[kind] = s.peaks;
    }
    if (!result.properties.contains("spin_multiplicity") || result.properties["spin_multiplicity"] == 1) {
        QJsonArray shielding;
        for (const auto &value : result.properties["shielding"].toArray()) {
            const auto row = value.toObject();
            if (row["isotropic"].isDouble())
                shielding.append(QJsonObject{
                    {"atom", row["atom"]}, {"element", row["element"]}, {"isotropic", row["isotropic"]}});
        }
        if (!shielding.isEmpty())
            spectra["shielding"] = shielding;
    }
    return spectra;
}
SpectrumData averagedSpectrum(const QJsonArray &members, const QJsonObject &p) {
    const auto kind = p["kind"].toString();
    if (!QStringList{"ir", "raman", "uv", "ecd", "nmr"}.contains(kind))
        fail(tr("配座平均はIR・通常ラマン・UV–Vis・ECD・NMRに対応します。"));
    const QString key = kind == "nmr" ? "shielding" : kind;
    QJsonArray included, omitted;
    double total = 0;
    for (const auto &value : members) {
        const auto member = value.toObject();
        const double weight = member["weight"].toDouble();
        if (!(weight > 0) || !std::isfinite(weight))
            continue; // A duplicate of another conformer.
        if (member["spectra"].toObject()[key].toArray().isEmpty()) {
            omitted.append(member["name"]);
            continue;
        }
        included.append(member);
        total += weight;
    }
    if (included.isEmpty())
        fail(tr("この種類のスペクトルのデータを持つ配座がありません。"));
    SpectrumData out;
    QJsonArray conformers;
    if (kind != "nmr") {
        out.parameters = p;
        spectrumWidth(p);
        collectPeaks(ResultData(), out); // The axes of the kind.
    }
    QMap<int, QJsonObject> shielding; // The mean of each atom.
    for (const auto &value : included) {
        const auto member = value.toObject();
        const double fraction = member["weight"].toDouble() / total;
        conformers.append(QJsonObject{{"name", member["name"]}, {"weight", fraction}});
        const auto rows = member["spectra"].toObject()[key].toArray();
        QSet<int> atoms;
        for (const auto &row : rows) {
            auto peak = row.toObject();
            if (kind == "nmr") {
                const int atom = peak["atom"].toInt();
                if (conformers.size() == 1)
                    shielding[atom] = {{"atom", atom}, {"element", peak["element"]}, {"isotropic", 0.}};
                if (!shielding.contains(atom) || shielding[atom]["element"] != peak["element"] ||
                    atoms.contains(atom))
                    fail(tr("配座の間でNMR遮蔽定数の原子が一致しません。"));
                atoms.insert(atom);
                shielding[atom]["isotropic"] =
                    shielding[atom]["isotropic"].toDouble() + fraction * required(peak, "isotropic");
                continue;
            }
            const double x = required(peak, "x"), weight = fraction * required(peak, "weight");
            peak["weight"] = weight;
            peak["conformer"] = member["name"];
            out.peaks.append(peak);
            out.sticks.append({x, weight});
        }
        if (kind == "nmr" && atoms.size() != shielding.size())
            fail(tr("配座の間でNMR遮蔽定数の原子が一致しません。"));
    }
    if (kind == "nmr") {
        // Fast exchange: one line per atom at the population-weighted mean shielding.
        ResultData ensemble;
        QJsonArray rows;
        for (const auto &row : shielding)
            rows.append(row);
        ensemble.properties["shielding"] = rows;
        out = calculateSpectrum(ensemble, p);
    } else {
        if (out.sticks.size() > 32768)
            fail(tr("配座平均スペクトルは32768本までのピークに対応します。"));
        broaden(out);
    }
    out.parameters["conformers"] = conformers;
    out.parameters["omitted"] = omitted;
    return out;
}
QByteArray spectrumCsv(const SpectrumData &s) {
    // The sticks of an averaged spectrum name their conformer in a last column.
    const bool conformers = std::any_of(s.peaks.begin(), s.peaks.end(), [](const auto &peak) {
        return peak.toObject().contains("conformer");
    });
    QByteArray text = conformers ? "type,atom,mode,x,weight,conformer\n" : "type,atom,mode,x,weight\n";
    for (const auto &value : s.peaks) {
        const auto p = value.toObject();
        text += "peak," + (p.contains("atom") ? QByteArray::number(p["atom"].toInt()) : QByteArray()) + ',' +
                (p.contains("mode") ? QByteArray::number(p["mode"].toInt()) : QByteArray()) + ',' +
                QByteArray::number(p["x"].toDouble(), 'g', 16) + ',' +
                QByteArray::number(p["weight"].toDouble(), 'g', 16);
        if (conformers)
            text += ",\"" + p["conformer"].toString().replace('"', "\"\"").toUtf8() + '"';
        text += '\n';
    }
    for (const auto &point : s.curve)
        text += "curve,,," + QByteArray::number(point.x(), 'g', 16) + ',' +
                QByteArray::number(point.y(), 'g', 16) + (conformers ? ",\n" : "\n");
    return text;
}
} // namespace mf
