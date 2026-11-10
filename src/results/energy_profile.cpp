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
} // namespace
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
    return {{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
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
            {"imaginary", r.modes.isEmpty() ? QJsonValue()
                                            : QJsonValue(resultModeCounts(resultModeKinds(r)).significant)}};
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
                correction = metric == "ZPE" ? number(thermal, "zpe_kcal") : number(thermal, "enthalpy_kcal");
                if (metric == "G")
                    correction -= temperature * number(thermal, "entropy_cal") / 1000;
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
            sources.append(QJsonObject{{"record", id},
                                       {"thermal", thermalId},
                                       {"coefficient", coefficient},
                                       {"electronic_hartree", e},
                                       {"correction_kcal", correction}});
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
        evaluated[i] = row;
    }
    warnings.removeDuplicates();
    return {{"stages", evaluated},
            {"metric", metric},
            {"unit", unit},
            {"temperature", temperature},
            {"standard_state", p["standard_state"]},
            {"comparable", comparable},
            {"warnings", QJsonArray::fromStringList(warnings)}};
}
QJsonArray conformerPopulations(const QJsonArray &records, const QString &metric, double temperature,
                                double tolerance) {
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
    const auto evaluated = evaluateEnergyProfile({{"format", "molforma-energy-profile"},
                                                  {"version", 1},
                                                  {"records", records},
                                                  {"stages", stages},
                                                  {"metric", metric},
                                                  {"unit", "kcal"},
                                                  {"temperature", temperature},
                                                  {"standard_state", "as calculated"},
                                                  {"reference", 0}});
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
        weights[index] = std::exp(-delta / (.00198720425864 * temperature));
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
