// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/conformer_set.h"
#include "editing/coordinates.h"
#include "foundation/i18n.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "results/spectrum.h"
#include <QDir>
#include <QTimer>
#include <cmath>

namespace mf {
namespace {
QJsonObject optionJson(const ConformerOptions &s) {
    return {{"metric", s.metric},
            {"temperature", s.temperature},
            {"rmsd", s.rmsd},
            {"frequencies", s.frequencies},
            {"frequency_scale", s.thermal.scale},
            {"entropy_model", s.thermal.model},
            {"entropy_cutoff", s.thermal.cutoff}};
}
ConformerOptions readOptions(const QJsonObject &p) {
    ConformerOptions s;
    s.metric = p["metric"].toString("E");
    s.temperature = p["temperature"].toDouble(298.15);
    s.rmsd = p["rmsd"].toDouble(.1);
    s.frequencies = p["frequencies"].toBool();
    s.thermal = thermalOptions(p);
    if (!QStringList{"E", "G"}.contains(s.metric) || !std::isfinite(s.temperature) || s.temperature < .01 ||
        s.temperature > 10000 || !std::isfinite(s.rmsd) || s.rmsd < .001 || s.rmsd > 2)
        fail(tr("配座セットの比較条件が不正です。"));
    return s;
}
} // namespace
ConformerSet::ConformerSet(QObject *parent) : QObject(parent), loader(this) {
    connect(&loader, &ResultLoader::loaded, this, [this](const ResultData &r, const QString &stamp) {
        if (reading < 0 || reading >= items.size())
            return;
        auto item = items[reading].toObject();
        try {
            if (r.properties["optimization"] != "converged")
                fail(tr("最適化の収束を確認できません。"));
            auto result = energyRecord(r, pendingOriginal, nwchemFinalSettings(pendingOriginal));
            result["source_stamp"] = stamp;
            item["result"] = result;
            // What the log holds for the averaged spectra of the set.
            item["spectra"] = spectrumContributions(r);
            item.remove("error");
        } catch (const std::exception &e) {
            item["error"] = QString::fromUtf8(e.what());
        }
        items[reading] = item;
        scheduleNext();
        emit changed();
    });
    connect(&loader, &ResultLoader::failed, this, [this](const QString &error) {
        if (reading >= 0 && reading < items.size()) {
            auto item = items[reading].toObject();
            item["error"] = error;
            items[reading] = item;
        }
        scheduleNext();
        emit changed();
    });
}
void ConformerSet::setConformers(const Document &base, const QVector<Conformer> &conformers) {
    QJsonArray rows;
    for (int i = 0; i < conformers.size(); ++i)
        rows.append(QJsonObject{{"name", tr("配座 %1").arg(i + 1)},
                                {"selected", true},
                                {"geometry", conformers[i].document.xyz()},
                                {"force_field", conformers[i].forceField},
                                {"force_field_kcal", conformers[i].energy}});
    setSpecification({{"format", "molforma-conformer-set"},
                      {"version", 1},
                      {"original", base.project()},
                      {"entries", rows},
                      {"metric", "E"},
                      {"temperature", 298.15},
                      {"rmsd", .1}});
}
QJsonObject ConformerSet::specification() const {
    auto out = extra;
    const auto o = optionJson(settings);
    for (auto it = o.begin(); it != o.end(); ++it)
        out[it.key()] = it.value();
    out["format"] = "molforma-conformer-set";
    out["version"] = 1;
    out["original"] = base.project();
    out["entries"] = items;
    return out;
}
void ConformerSet::setSpecification(const QJsonObject &p) {
    if (p["format"] != "molforma-conformer-set" || p["version"] != 1 || p["entries"].toArray().isEmpty() ||
        p["entries"].toArray().size() > 100)
        fail(tr("配座セットの形式が不正です（1〜100配座）。"));
    const auto original = Document::fromProject(p["original"].toObject());
    for (const auto &value : p["entries"].toArray()) {
        auto candidate = original;
        applyResultGeometry(candidate, Document::fromXyz(value.toObject()["geometry"].toString()));
    }
    const auto options = readOptions(p);
    cancel();
    base = original;
    items = p["entries"].toArray();
    settings = options;
    extra = p;
    emit changed();
}
Document ConformerSet::geometry(int index) const {
    if (index < 0 || index >= items.size())
        fail(tr("配座を選んでください。"));
    auto d = base;
    applyResultGeometry(d, Document::fromXyz(items[index].toObject()["geometry"].toString()));
    return d;
}
void ConformerSet::queueSelected(const Enqueue &enqueue) {
    QVector<Document> selected;
    QVector<int> indices;
    for (int i = 0; i < items.size(); ++i)
        if (items[i].toObject()["selected"].toBool()) {
            selected.append(geometry(i));
            indices.append(i);
        }
    if (selected.isEmpty())
        fail(tr("配座を選んでください。"));
    const auto jobs = enqueue(selected, settings.frequencies);
    if (jobs.isEmpty())
        return; // The interactive caller cancelled.
    if (jobs.size() != indices.size())
        fail(tr("配座とジョブの件数が一致しません。"));
    cancel();
    for (int i = 0; i < jobs.size(); ++i) {
        auto entry = items[indices[i]].toObject();
        entry["job"] = jobs[i];
        entry.remove("result");
        entry.remove("spectra");
        entry.remove("error");
        items[indices[i]] = entry;
    }
    emit changed();
}
void ConformerSet::refreshResults(CheckPath check) {
    if (loading())
        return;
    checkPath = std::move(check);
    pending.clear();
    for (int i = 0; i < items.size(); ++i)
        if (!items[i].toObject()["job"].toString().isEmpty())
            pending.append(i);
    readNext();
}
void ConformerSet::readNext() {
    if (pending.isEmpty()) {
        reading = -1;
        emit changed();
        return;
    }
    reading = pending.takeFirst();
    auto item = items[reading].toObject();
    item.remove("result");
    item.remove("spectra");
    item.remove("error");
    items[reading] = item;
    const auto dir = item["job"].toString();
    try {
        if (checkPath)
            checkPath(dir);
        for (const auto *file : {"run.json", "project.molforma.json", "input.nw", "output.out"})
            if (checkPath)
                checkPath(QDir(dir).filePath(file));
        const auto job = readHistoryEntry(dir);
        item["state"] = historyStatus(job.status);
        items[reading] = item;
        if (job.status != RunState::Finished) {
            scheduleNext();
            return;
        }
        pendingOriginal = historyDocument(dir);
        loader.start({.path = QDir(dir).filePath("output.out")});
    } catch (const std::exception &e) {
        item["error"] = QString::fromUtf8(e.what());
        items[reading] = item;
        scheduleNext();
    }
}
void ConformerSet::cancel() {
    ++generation;
    loader.cancel();
    pending.clear();
    reading = -1;
}
void ConformerSet::scheduleNext() {
    const auto serial = generation;
    QTimer::singleShot(0, this, [this, serial] {
        if (serial == generation)
            readNext();
    });
}
void ConformerSet::checkIndex(int index) const {
    if (index < 0 || index >= items.size())
        fail(tr("配座を選んでください。"));
}
void ConformerSet::setOptions(const ConformerOptions &options) {
    settings = readOptions(optionJson(options));
}
void ConformerSet::setSelected(int index, bool selected) {
    checkIndex(index);
    auto row = items[index].toObject();
    row["selected"] = selected;
    items[index] = row;
    emit changed();
}
void ConformerSet::setName(int index, const QString &name) {
    checkIndex(index);
    auto row = items[index].toObject();
    row["name"] = name;
    items[index] = row;
    emit changed();
}
QByteArray ConformerSet::selectedXyz() const {
    QByteArray xyz;
    for (int i = 0; i < items.size(); ++i)
        if (items[i].toObject()["selected"].toBool())
            xyz += geometry(i).xyz().toUtf8();
    return xyz;
}
ConformerAnalysis ConformerSet::analyze() const {
    QJsonArray results;
    QVector<int> indices;
    for (int i = 0; i < items.size(); ++i) {
        const auto entry = items[i].toObject();
        if (!entry["selected"].toBool())
            continue;
        const auto result = entry["result"].toObject();
        if (result.isEmpty())
            return {};
        results.append(result);
        indices.append(i);
    }
    ConformerAnalysis out;
    if (results.isEmpty())
        return out;
    const auto rows = conformerPopulations(results, settings.metric, settings.temperature, settings.rmsd,
                                           optionJson(settings));
    for (int k = 0; k < rows.size(); ++k) {
        auto row = rows[k].toObject();
        const int i = indices[k];
        row["entry"] = i;
        out.rows.append(row);
        const auto entry = items[i].toObject();
        out.spectra.append(QJsonObject{
            {"name", entry["name"]}, {"weight", row["population"]}, {"spectra", entry["spectra"]}});
    }
    return out;
}
} // namespace mf
