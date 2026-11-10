// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/energy_profile.h"
#include "model/geometry.h"
#include "model/i18n.h"
#include "model/units.h"
#include "results/result_summary.h"
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>

namespace mf {
namespace {
double number(const QJsonObject &object, const QString &key) {
    if (!object[key].isDouble() || !std::isfinite(object[key].toDouble()))
        fail(tr("エネルギー比較の %1 が不足または不正です。").arg(key));
    return object[key].toDouble();
}
QJsonObject level(const QJsonObject &s) {
    QJsonObject out;
    for (const auto *key :
         {"method", "basis", "basis_overrides", "basis_functions", "functional", "xc_custom", "dispersion",
          "relativistic", "reference", "ri_j", "grid", "solvent", "solvent_name", "dielectric", "bsse"})
        out[key] = s[key];
    return out;
}
// The entropy of a harmonic oscillator in units of R, for x = hcν/kT.
double harmonicEntropy(double x) {
    return x / std::expm1(x) - std::log(-std::expm1(-x));
}
} // namespace
ThermalOptions thermalOptions(const QJsonObject &p) {
    ThermalOptions o;
    if (p.contains("frequency_scale"))
        o.scale = number(p, "frequency_scale");
    if (p.contains("entropy_cutoff"))
        o.cutoff = number(p, "entropy_cutoff");
    for (const auto *key : {"entropy_model", "standard_state_correction"})
        if (p.contains(key) && !p[key].isString())
            fail(tr("エネルギー比較の %1 が不足または不正です。").arg(key));
    o.model = p["entropy_model"].toString("rrho");
    const auto state = p["standard_state_correction"].toString("none");
    if (!(o.scale >= .5 && o.scale <= 1.5))
        fail(tr("振動数のスケール因子は0.5〜1.5にしてください。"));
    if (!QStringList{"rrho", "grimme", "truhlar"}.contains(o.model) || !(o.cutoff >= 10 && o.cutoff <= 500))
        fail(tr("エントロピーのモデルはrrho・grimme・truhlar、低振動数のしきい値は10〜500 cm⁻¹にしてくだ"
                "さい。"));
    if (state != "none" && state != "1M")
        fail(tr("標準状態の補正はnone（1 atm）または1M（1 mol/L）を指定してください。"));
    o.molar = state == "1M";
    return o;
}
VibrationalTerms vibrationalTerms(const QVector<double> &wavenumbers, double temperature,
                                  const ThermalOptions &o) {
    VibrationalTerms terms;
    for (const double wavenumber : wavenumbers) {
        const double scaled = o.scale * wavenumber;
        if (!(scaled > 0) || !std::isfinite(scaled) || !(temperature > 0))
            fail(tr("熱補正の振動数または温度が不正です。"));
        const double theta = WavenumberToKelvin * scaled, x = theta / temperature;
        terms.zpe += GasConstantKcal * theta / 2;
        terms.thermal += GasConstantKcal * theta / std::expm1(x);
        double entropy = harmonicEntropy(
            o.model == "truhlar" ? std::max(x, WavenumberToKelvin * o.cutoff / temperature) : x);
        if (o.model == "grimme") {
            // Grimme, Chem. Eur. J. 2012, 18, 9955: a free rotor of the same frequency, whose moment of
            // inertia μ = h/(8π²cν) is limited by B_av = 1e-44 kg m²: S = R[1/2 + ln √(8π³μ′kT/h²)].
            const double pi = std::numbers::pi, average = 1e-44;
            const double moment = PlanckConstant / (8 * pi * pi * LightSpeedCm * scaled),
                         limited = moment * average / (moment + average);
            const double rotor = .5 + std::log(std::sqrt(8 * pi * pi * pi * limited * BoltzmannConstant *
                                                         temperature / (PlanckConstant * PlanckConstant)));
            const double weight = 1 / (1 + std::pow(o.cutoff / scaled, 4));
            entropy = weight * entropy + (1 - weight) * rotor;
        }
        terms.entropy += 1000 * GasConstantKcal * entropy;
    }
    return terms;
}
double standardStateShift(double temperature) {
    // R′ in L atm mol⁻¹ K⁻¹: an ideal gas at 1 atm takes R′T litres per mole.
    const double liters = 1000 * BoltzmannConstant * AvogadroConstant / AtmosphereToPascal;
    return GasConstantKcal * temperature * std::log(liters * temperature);
}
double boltzmannFactor(double energy, double temperature) {
    return std::exp(-energy / (GasConstantKcal * temperature));
}
double eyringRate(double barrier, double temperature) {
    return BoltzmannConstant * temperature / PlanckConstant * boltzmannFactor(barrier, temperature);
}
QJsonObject energyRecord(const ResultData &r, const Document &original, const QJsonObject &settings) {
    const auto final = r.properties["final_energy"].toObject();
    const double energy = number(final, "hartree");
    QJsonObject composition;
    for (const auto &a : original.atoms) {
        if (a.ghost)
            fail(tr("ゴースト中心を含む計算は反応エネルギー比較に登録できません。"));
        composition[a.element] = composition[a.element].toInt() + 1;
    }
    const auto geometry = (final["source"] == "frequency_reference" || r.properties.contains("raman")) &&
                                  !r.vibrationGeometry.atoms.isEmpty()
                              ? r.vibrationGeometry
                          : r.frames.isEmpty() ? original
                                               : r.frames.last().molecule;
    // What NWChem's thermochemistry sums over (vib_wrtfreq.F): its scale factor (vib:scalefreq) times
    // every projected frequency, if above 0.1 cm⁻¹. That leaves out imaginary modes and the
    // translations and rotations, which are left out here by their kind too.
    const auto thermochemistry = r.properties["thermochemistry"].toArray();
    const double scale = thermochemistry.at(0).toObject()["nwchem_frequency_scale"].toDouble(1);
    const auto kinds = resultModeKinds(r);
    QJsonArray vibrations;
    for (int i = 0; i < r.modes.size(); ++i)
        if (kinds[i] == ResultModeKind::Vibration && scale * r.modes[i].frequency > .1)
            vibrations.append(scale * r.modes[i].frequency);
    QJsonObject record{
        {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"name", settings["start"]},
        {"source", r.source},
        {"energy_hartree", energy},
        {"energy_source", final},
        {"settings", settings},
        {"composition", composition},
        {"charge", original.charge},
        {"multiplicity", original.multiplicity},
        {"geometry", geometry.xyz()},
        {"normal", r.outcome == ResultOutcome::Normal},
        {"thermochemistry", r.properties["thermochemistry"]},
        {"imaginary", r.modes.isEmpty() ? QJsonValue() : QJsonValue(resultModeCounts(kinds).significant)}};
    if (!r.modes.isEmpty() && !thermochemistry.isEmpty())
        record["vibrations"] = vibrations;
    return record;
}
QJsonObject evaluateEnergyProfile(const QJsonObject &p) {
    if (p["format"] != "molforma-energy-profile" || p["version"] != 1)
        fail(tr("エネルギー比較ファイルの形式が不正です。"));
    const auto metric = p["metric"].toString(), unit = p["unit"].toString();
    if (!QStringList{"E", "ZPE", "H", "G"}.contains(metric) ||
        !QStringList{"hartree", "kcal", "kj"}.contains(unit))
        fail(tr("エネルギー比較の種類または単位が不正です。"));
    const double temperature = number(p, "temperature");
    if (temperature <= 0 || temperature > 10000 || p["standard_state"].toString().trimmed().isEmpty())
        fail(tr("温度と標準状態を指定してください。"));
    const auto options = thermalOptions(p);
    // The entropy model changes G only; the scale factor every thermal correction.
    const bool vibrational = options.scale != 1 || (metric == "G" && options.vibrational());
    const double shift = metric == "G" && options.molar ? standardStateShift(temperature) : 0;
    QMap<QString, QJsonObject> records;
    const auto input = p["records"].toArray(), stages = p["stages"].toArray();
    if (input.size() > 200 || stages.isEmpty() || stages.size() > 200)
        fail(tr("比較は200件の結果・200段階までです。少なくとも1段階を登録してください。"));
    for (const auto &value : input) {
        const auto r = value.toObject();
        const auto id = r["id"].toString();
        if (id.isEmpty() || records.contains(id))
            fail(tr("エネルギー比較の結果IDが重複または不足しています。"));
        records[id] = r;
    }
    const double scale = unit == "kcal" ? HartreeToKcal : unit == "kj" ? HartreeToKj : 1;
    QJsonArray evaluated;
    QStringList warnings;
    QJsonObject firstComposition, firstLevel;
    double firstCharge = 0;
    bool comparable = true;
    for (int stageIndex = 0; stageIndex < stages.size(); ++stageIndex) {
        const auto stage = stages[stageIndex].toObject();
        const auto terms = stage["terms"].toArray();
        if (terms.isEmpty() || terms.size() > 200)
            fail(tr("反応段階には1〜200個の成分を登録してください。"));
        double total = 0, charge = 0;
        QJsonObject composition;
        QJsonArray sources;
        for (const auto &value : terms) {
            const auto term = value.toObject();
            const auto id = term["record"].toString();
            if (!records.contains(id))
                fail(tr("反応段階が未登録の計算結果を参照しています。"));
            const auto r = records[id];
            if (!r["normal"].toBool())
                fail(tr("正常終了していない計算はエネルギー比較に使えません: %1").arg(r["name"].toString()));
            const double coefficient = number(term, "coefficient");
            if (!(coefficient > 0 && coefficient <= 1000))
                fail(tr("化学量論係数は0より大きく1000以下にしてください。"));
            double correction = 0;
            QString thermalId;
            QJsonObject applied;
            if (metric != "E") {
                thermalId = term["thermal"].toString(id);
                if (!records.contains(thermalId))
                    fail(tr("熱補正の計算結果がありません。"));
                const auto t = records[thermalId];
                if (!t["normal"].toBool() || t["composition"] != r["composition"] ||
                    t["charge"] != r["charge"] || t["multiplicity"] != r["multiplicity"])
                    fail(tr("熱補正と電子エネルギーの組成・電荷・終了状態が一致しません。"));
                QJsonObject thermal;
                for (const auto &item : t["thermochemistry"].toArray())
                    if (std::abs(item.toObject()["temperature"].toDouble() - temperature) < .01) {
                        thermal = item.toObject();
                        break;
                    }
                if (thermal.isEmpty())
                    fail(tr("指定した温度の熱補正がありません: %1").arg(t["name"].toString()));
                double zpe = metric == "ZPE" ? number(thermal, "zpe_kcal") : 0,
                       enthalpy = metric == "ZPE" ? 0 : number(thermal, "enthalpy_kcal"),
                       entropy = metric == "G" ? number(thermal, "entropy_cal") : 0;
                if (vibrational) {
                    // NWChem's totals with their vibrational parts replaced: the difference of the
                    // model at the scaled wavenumbers from the harmonic oscillators NWChem summed.
                    if (!t["vibrations"].isArray())
                        fail(tr("%1: "
                                "振動数の記録がない結果には、スケール因子・エントロピーの補正を適用できません"
                                "。振動数計算のジョブを追加し直してください。")
                                 .arg(t["name"].toString()));
                    QVector<double> wavenumbers;
                    for (const auto &wavenumber : t["vibrations"].toArray())
                        wavenumbers << wavenumber.toDouble();
                    const auto printed = vibrationalTerms(wavenumbers, temperature),
                               model = vibrationalTerms(wavenumbers, temperature, options);
                    zpe += model.zpe - printed.zpe;
                    enthalpy += model.zpe + model.thermal - (printed.zpe + printed.thermal);
                    entropy += model.entropy - printed.entropy;
                }
                // 1 atm → 1 mol/L lowers the translational entropy by R·ln(R′T).
                entropy -= 1000 * shift / temperature;
                correction = metric == "ZPE" ? zpe : enthalpy;
                applied[metric == "ZPE" ? "zpe_kcal" : "enthalpy_kcal"] = correction;
                if (metric == "G") {
                    correction -= temperature * entropy / 1000;
                    applied["entropy_cal"] = entropy;
                }
                if (thermalId != id)
                    warnings << tr("%1: 別ジョブの熱補正を使用（%2）。")
                                    .arg(r["name"].toString(), t["source"].toString());
                if (t["imaginary"].toInt() > 0)
                    warnings << tr("%1: 有意な虚振動を含む熱補正です。").arg(t["name"].toString());
            }
            const auto currentLevel = level(r["settings"].toObject());
            if (firstLevel.isEmpty())
                firstLevel = currentLevel;
            else if (firstLevel != currentLevel) {
                warnings << tr("電子エネルギーの計算条件が揃っていません。");
                comparable = false;
            }
            const double e = number(r, "energy_hartree");
            total += coefficient * (e + correction / HartreeToKcal);
            charge += coefficient * number(r, "charge");
            const auto atoms = r["composition"].toObject();
            if (atoms.isEmpty())
                fail(tr("計算結果の組成がありません。"));
            for (auto it = atoms.begin(); it != atoms.end(); ++it)
                composition[it.key()] =
                    composition[it.key()].toDouble() + coefficient * number(atoms, it.key());
            applied["record"] = id;
            applied["thermal"] = thermalId;
            applied["coefficient"] = coefficient;
            applied["electronic_hartree"] = e;
            applied["correction_kcal"] = correction;
            sources.append(applied);
        }
        if (!stageIndex) {
            firstComposition = composition;
            firstCharge = charge;
        } else if (composition != firstComposition || std::abs(charge - firstCharge) > 1e-8) {
            warnings << tr("反応段階の組成または電荷の収支が一致しません。");
            comparable = false;
        }
        if (!std::isfinite(total))
            fail(tr("エネルギー比較に有限でない値があります。"));
        evaluated.append(QJsonObject{{"name", stage["name"]},
                                     {"hartree", total},
                                     {"composition", composition},
                                     {"charge", charge},
                                     {"sources", sources}});
    }
    const int reference = p["reference"].toInt(-1);
    if (reference < 0 || reference >= evaluated.size())
        fail(tr("比較の基準段階を選んでください。"));
    const double base = evaluated[reference].toObject()["hartree"].toDouble();
    for (int i = 0; i < evaluated.size(); ++i) {
        auto row = evaluated[i].toObject();
        row["relative"] = (row["hartree"].toDouble() - base) * scale;
        if (metric == "G") {
            // Against the reference stage; a rate constant only for a barrier.
            const double delta = (row["hartree"].toDouble() - base) * HartreeToKcal,
                         factor = boltzmannFactor(delta, temperature);
            if (std::isfinite(factor)) {
                row["boltzmann_factor"] = factor;
                if (delta > 0)
                    row["rate_constant"] = eyringRate(delta, temperature);
            }
        }
        evaluated[i] = row;
    }
    warnings.removeDuplicates();
    return {{"stages", evaluated},
            {"metric", metric},
            {"unit", unit},
            {"temperature", temperature},
            {"standard_state", p["standard_state"]},
            {"frequency_scale", options.scale},
            {"entropy_model", options.model},
            {"entropy_cutoff", options.cutoff},
            {"standard_state_correction", options.molar ? "1M" : "none"},
            {"standard_state_shift_kcal", shift},
            {"comparable", comparable},
            {"warnings", QJsonArray::fromStringList(warnings)}};
}
QJsonArray conformerPopulations(const QJsonArray &records, const QString &metric, double temperature,
                                double tolerance, const QJsonObject &thermal) {
    if (records.isEmpty() || records.size() > 100 || !std::isfinite(tolerance) || tolerance < .001 ||
        tolerance > 2)
        fail(tr("配座比較は1〜100件、RMSD許容値は0.001〜2 Åにしてください。"));
    QJsonArray stages;
    QVector<Document> geometries;
    const QJsonValue multiplicity = records[0].toObject()["multiplicity"];
    for (const auto &value : records) {
        const auto r = value.toObject();
        if (r["multiplicity"] != multiplicity ||
            (metric == "G" && (!r["imaginary"].isDouble() || r["imaginary"].toInt() != 0)))
            fail(tr("配座分布には多重度が揃い、有意な虚振動のない結果を使ってください。"));
        stages.append(
            QJsonObject{{"name", r["name"]},
                        {"terms", QJsonArray{QJsonObject{{"record", r["id"]}, {"coefficient", 1}}}}});
        geometries.append(Document::fromXyz(r["geometry"].toString()));
        if (geometries.last().atoms.size() * records.size() * records.size() > 2000000)
            fail(tr("RMSD比較が大きすぎます。比較する配座を減らしてください。"));
    }
    QJsonObject profile{{"format", "molforma-energy-profile"},
                        {"version", 1},
                        {"records", records},
                        {"stages", stages},
                        {"metric", metric},
                        {"unit", "kcal"},
                        {"temperature", temperature},
                        {"standard_state", "as calculated"},
                        {"reference", 0}};
    // The standard state shifts every conformer alike, so it is left out.
    for (const auto *key : {"frequency_scale", "entropy_model", "entropy_cutoff"})
        if (thermal.contains(key))
            profile[key] = thermal[key];
    const auto evaluated = evaluateEnergyProfile(profile);
    if (!evaluated["comparable"].toBool())
        fail(tr("組成・電荷・計算条件の異なる配座から分布を計算できません。"));
    const auto energies = evaluated["stages"].toArray();
    QVector<int> order(records.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return energies[a].toObject()["hartree"].toDouble() < energies[b].toObject()["hartree"].toDouble();
    });
    QVector<int> representatives, group(records.size());
    for (int index : order) {
        int matched = -1;
        for (int k = 0; k < representatives.size(); ++k)
            if (superpose(geometries[index], geometries[representatives[k]]).rmsd <= tolerance) {
                matched = k;
                break;
            }
        if (matched < 0) {
            matched = representatives.size();
            representatives.append(index);
        }
        group[index] = matched;
    }
    const double lowest = energies[order[0]].toObject()["hartree"].toDouble();
    QVector<double> weights(records.size());
    double total = 0;
    for (int index : representatives) {
        const double delta = (energies[index].toObject()["hartree"].toDouble() - lowest) * HartreeToKcal;
        weights[index] = boltzmannFactor(delta, temperature);
        total += weights[index];
    }
    QJsonArray rows;
    for (int i = 0; i < records.size(); ++i)
        rows.append(QJsonObject{
            {"index", i},
            {"group", group[i] + 1},
            {"representative", representatives[group[i]]},
            {"population", weights[i] / total},
            {"relative_kcal", (energies[i].toObject()["hartree"].toDouble() - lowest) * HartreeToKcal}});
    return rows;
}
} // namespace mf
