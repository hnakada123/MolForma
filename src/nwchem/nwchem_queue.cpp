// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_queue.h"
#include "editing/coordinates.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "nwchem/history.h"
#include "nwchem/run_record.h"
#include "results/result_path.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>

namespace mf {
QStringList queueChainStructures() {
    return {"final", "highest", "irc_forward", "irc_backward"};
}
QString queueChainStructureName(const QString &structure) {
    return structure == "final"          ? tr("最終構造")
           : structure == "highest"      ? tr("経路のエネルギー最高点")
           : structure == "irc_forward"  ? tr("IRCの前進側の端")
           : structure == "irc_backward" ? tr("IRCの後退側の端")
                                         : structure;
}
Document chainedDocument(const Document &document, const QString &job, const QString &structure) {
    const auto log = jobFile(job, JobFile::output);
    if (!QFileInfo(log).isFile())
        fail(tr("引継ぎ元のジョブに出力（output.out）がありません: %1").arg(job));
    const auto result = readResult(log);
    int frame = -1;
    if (structure == "final")
        frame = int(result.frames.size()) - 1;
    else if (structure == "highest") {
        // The highest point with a structure of the paths in the log. The ends of an NEB are fixed, never
        // its highest point; a scan or an IRC (with its transition state) may be highest at an end.
        double highest = -INFINITY;
        for (const auto kind : {ResultPathKind::Scan, ResultPathKind::Neb, ResultPathKind::Irc}) {
            const auto path = resultPath(result, kind);
            const int point =
                path.highest(kind == ResultPathKind::Neb ? PathEnds::Excluded : PathEnds::Included, true);
            if (point >= 0 && path.points[point].energy > highest) {
                highest = path.points[point].energy;
                frame = path.points[point].frame;
            }
        }
        if (frame < 0)
            fail(tr("引継ぎ元のジョブに経路（座標スキャン・IRC・NEB）の点がありません: %1").arg(job));
    } else if (structure == "irc_forward" || structure == "irc_backward") {
        const auto direction = structure.mid(4);
        int last = 0;
        for (const auto &value : result.properties.value("irc").toObject()["points"].toArray()) {
            const auto point = value.toObject();
            if (point["direction"] == direction && point["point"].toInt() >= last &&
                point["frame"].toInt(-1) >= 0) {
                last = point["point"].toInt();
                frame = point["frame"].toInt();
            }
        }
        if (frame < 0)
            fail(tr("引継ぎ元のジョブにIRCの%1の点がありません: %2")
                     .arg(direction == "forward" ? tr("前進方向") : tr("後退方向"), job));
    } else
        fail(tr("引き継ぐ構造の指定が不正です: %1").arg(structure));
    if (frame < 0 || frame >= result.frames.size())
        fail(tr("引継ぎ元のジョブに構造がありません: %1").arg(job));
    auto next = document;
    applyResultGeometry(next, result.frames[frame].molecule);
    return next;
}
void NwchemQueue::setStore(const QString &path) {
    store = path;
}
void NwchemQueue::save() const {
    if (store.isEmpty())
        return;
    QJsonArray rows;
    for (const auto &item : items) {
        const bool current = item.id == currentId && runStateActive(item.state);
        if (item.state != RunState::Prepared && !current)
            continue;
        QJsonObject row{{"id", item.id},
                        {"name", item.name},
                        {"base", item.base},
                        {"directory", item.plan.directory},
                        {"running", current}};
        // A waiting job is the record that was registered; a running one is rewritten as it goes.
        if (!current)
            row["record"] = item.record;
        // A chained entry waiting for its structure has no job yet: its document is kept here.
        if (item.chain.active() && item.plan.directory.isEmpty()) {
            const int source = find(item.chain.from);
            row["chain"] =
                QJsonObject{{"from", item.chain.from},
                            {"structure", item.chain.structure},
                            {"directory", source >= 0 ? items[source].plan.directory : item.chain.directory}};
            row["document"] = item.document.project();
        }
        rows.append(row);
    }
    if (rows.isEmpty()) {
        QFile::remove(store);
        return;
    }
    QDir().mkpath(QFileInfo(store).absolutePath());
    QSaveFile file(store);
    const auto bytes = QJsonDocument(QJsonObject{{"format", "molforma-queue"},
                                                 {"version", 1},
                                                 {"stopOnFailure", stopOnError},
                                                 {"entries", rows}})
                           .toJson();
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size())
        file.commit();
}
int NwchemQueue::restore() {
    if (store.isEmpty() || !items.isEmpty() || run.active() || !QFileInfo(store).isFile())
        return 0;
    QJsonObject saved;
    try {
        saved = parseJson(readFile(store));
    } catch (const std::exception &) {
        return 0;
    }
    if (saved["format"] != "molforma-queue" || saved["version"] != 1)
        return 0;
    stopOnError = saved["stopOnFailure"].toBool(true);
    isPaused = true;
    for (const auto &value : saved["entries"].toArray()) {
        const auto row = value.toObject();
        QueuedCalculation item;
        item.id = row["id"].toString();
        item.name = row["name"].toString();
        item.base = row["base"].toString();
        const auto directory = row["directory"].toString();
        if (item.id.isEmpty() || items.size() >= 1000)
            continue;
        if (row.contains("chain")) {
            const auto chain = row["chain"].toObject();
            item.chain = {chain["from"].toString(), chain["structure"].toString(),
                          chain["directory"].toString()};
            try {
                item.document = Document::fromProject(row["document"].toObject());
            } catch (const std::exception &) {
                continue;
            }
            if (!item.chain.active() || !queueChainStructures().contains(item.chain.structure))
                continue;
            items.push_back(std::move(item));
            continue;
        }
        if (directory.isEmpty())
            continue;
        try {
            if (row["running"].toBool()) {
                // Still running under its supervisor: followed. Ended meanwhile: the history has it.
                if (!currentId.isEmpty() || !run.attach(directory))
                    continue;
                item.plan = run.plan();
                item.state = run.state();
                currentId = item.id;
            } else {
                // What the job folder's record says, so that a restored entry is the job that was registered.
                item.record = row["record"].toString();
                if (item.record.isEmpty() || runRecordDigest(directory) != item.record)
                    continue;
                item.document = historyDocument(directory);
                const auto manifest = readRunRecord(directory, RecordCheck::Json);
                if (RunRecord(manifest).state() != RunState::Prepared)
                    continue;
                item.plan = runRecordPlan(QDir::cleanPath(directory), manifest);
                item.plan.input = historyText(directory, JobFile::input);
            }
        } catch (const std::exception &) {
            continue;
        }
        items.push_back(std::move(item));
    }
    save();
    if (!items.isEmpty())
        emit changed();
    return int(items.size());
}
NwchemQueue::NwchemQueue(NwchemRun &runner, QObject *parent) : QObject(parent), run(runner) {
    connect(this, &NwchemQueue::changed, this, &NwchemQueue::save);
    connect(&run, &NwchemRun::changed, this, [this] {
        if (!currentId.isEmpty()) {
            auto &item = items[index(currentId)];
            const bool changedState = item.state != run.state();
            item.state = run.state();
            item.plan = run.plan();
            item.error = run.message();
            if (!run.active()) {
                if (stopOnError && (item.state != RunState::Finished ||
                                    !RunRecord(item.plan.manifest).diagnostics().isEmpty()))
                    isPaused = true;
                currentId.clear();
            }
            if (changedState)
                emit changed();
        }
        if (!run.active())
            schedule();
    });
}
int NwchemQueue::find(const QString &id) const {
    for (int i = 0; i < items.size(); ++i)
        if (items[i].id == id)
            return i;
    return -1;
}
int NwchemQueue::index(const QString &id) const {
    if (const int i = find(id); i >= 0)
        return i;
    fail(tr("キューの計算が見つかりません。"));
}
void NwchemQueue::checkOrder() const {
    for (int i = 0; i < items.size(); ++i)
        if (items[i].chain.active() && items[i].state == RunState::Prepared)
            if (const int source = find(items[i].chain.from); source > i)
                fail(tr("構造を引き継ぐ計算は、引継ぎ元の計算より後に置いてください。"));
}
int NwchemQueue::pending() const {
    return int(std::count_if(items.begin(), items.end(),
                             [](const auto &item) { return item.state == RunState::Prepared; }));
}
QString NwchemQueue::enqueue(const Document &d, const QString &base, std::function<void()> check,
                             const QueueChain &chain, const ExecutionEnvironment &environment) {
    if (closed || items.size() >= 1000)
        fail(tr("キューにはこのセッションで1000件まで登録できます。"));
    if (check)
        check();
    QueuedCalculation item;
    item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.document = d;
    item.base = QFileInfo(base).absoluteFilePath();
    item.name = nwchemSettings(d)["start"].toString();
    if (chain.active()) {
        const auto &source = items[index(chain.from)];
        if (source.state != RunState::Prepared && !runStateActive(source.state) &&
            source.state != RunState::Finished)
            fail(tr("引継ぎ元の計算（%1）は正常に終わっていません。").arg(source.name));
        if (!queueChainStructures().contains(chain.structure))
            fail(tr("引き継ぐ構造の指定が不正です: %1").arg(chain.structure));
        const NwchemCalculation from(nwchemSettings(source.document));
        if (chain.structure == "highest" && !from.path())
            fail(tr("経路のエネルギー最高点は、座標スキャン・IRC・NEBの計算から引き継げます。"));
        if (chain.structure.startsWith("irc_") && from.task != NwchemTask::Irc)
            fail(tr("IRCの端の構造は、IRCの計算から引き継げます。"));
        bool same = source.document.atoms.size() == d.atoms.size();
        for (int i = 0; same && i < d.atoms.size(); ++i)
            same = source.document.atoms[i].element == d.atoms[i].element &&
                   source.document.atoms[i].ghost == d.atoms[i].ghost;
        if (!same)
            fail(tr("引継ぎ元の計算（%1）と原子の数・順番が異なります。").arg(source.name));
        // Checked now, saved when the structure is there.
        executionPlan(d, item.base, {}, true, environment);
        item.chain = chain;
        item.chain.directory = source.plan.directory;
    } else {
        item.plan = saveExecutionBundle(d, item.base, true, environment);
        item.record = runRecordDigest(item.plan.directory);
    }
    item.checkAccess = std::move(check);
    const auto id = item.id;
    items.push_back(std::move(item));
    emit changed();
    schedule();
    return id;
}
QStringList NwchemQueue::enqueueChain(const Document &current, const QString &base, const QString &from,
                                      const QString &structure, const QStringList &steps, bool endFrequencies,
                                      const std::function<std::function<void()>(const Document &)> &access) {
    // Each step: the document, the step its structure comes from (-1: the entry `from`) and which one.
    struct Step {
        Document document;
        int source;
        QString structure;
    };
    QVector<Step> plan;
    const auto step = [&](const QString &task, const QString &suffix, int source, const QString &which) {
        auto d = current;
        deriveNwchemCalculation(d, task, suffix);
        if (task == "irc")
            setNwchemSetting(d, "irc_direction", "both");
        plan.append({d, source, which});
    };
    for (const auto &key : steps)
        if (!QStringList{"current", "saddle", "irc", "ends"}.contains(key))
            fail(tr("連鎖の段階の指定が不正です: %1").arg(key));
    if (steps.contains("current"))
        plan.append({current, -1, structure});
    else {
        int last = -1;
        QString which = structure;
        if (steps.contains("saddle")) {
            step("saddle_frequencies", "_ts", last, which);
            last = int(plan.size()) - 1;
            which = "final";
        }
        if (steps.contains("irc")) {
            step("irc", "_irc", last, which);
            last = int(plan.size()) - 1;
        }
        if (steps.contains("ends")) {
            if (!steps.contains("irc"))
                fail(tr("IRCの両端の最適化には、IRCも登録してください。"));
            const auto task = endFrequencies ? "optimize_frequencies" : "optimize";
            step(task, "_fwd", last, "irc_forward");
            step(task, "_bwd", last, "irc_backward");
        }
    }
    if (plan.isEmpty())
        fail(tr("登録する計算を選んでください。"));
    if (closed || items.size() + plan.size() > 1000)
        fail(tr("キューにはこのセッションで1000件まで登録できます。"));
    index(from);
    // One state of the Preferences for all the steps.
    const auto environment = executionEnvironment();
    for (const auto &item : plan) // Every step is checked before any is registered.
        executionPlan(item.document, QFileInfo(base).absoluteFilePath(), {}, true, environment);
    QStringList ids;
    for (const auto &item : plan)
        ids << enqueue(item.document, base, access ? access(item.document) : std::function<void()>(),
                       QueueChain{item.source < 0 ? from : ids[item.source], item.structure, {}},
                       environment);
    return ids;
}
void NwchemQueue::resume() {
    if (closed)
        fail(tr("終了処理中のキューは開始できません。"));
    isPaused = false;
    emit changed();
    schedule();
}
void NwchemQueue::pause() {
    isPaused = true;
    emit changed();
}
void NwchemQueue::setStopOnFailure(bool stop) {
    stopOnError = stop;
    emit changed();
}
void NwchemQueue::cancel(const QString &id) {
    auto &item = items[index(id)];
    if (item.state != RunState::Prepared)
        fail(tr("待機中の計算だけをキューから取り消せます。"));
    item.state = RunState::Cancelled;
    // The calculations that would take their structure from it, and theirs in turn.
    for (bool more = true; more;) {
        more = false;
        for (auto &other : items)
            if (const int source = find(other.chain.from); other.state == RunState::Prepared &&
                                                           other.chain.active() && source >= 0 &&
                                                           items[source].state == RunState::Cancelled) {
                other.state = RunState::Cancelled;
                other.error = tr("引継ぎ元の計算が取り消されました。");
                more = true;
            }
    }
    emit changed(); // The prepared bundle remains reusable in the history.
}
void NwchemQueue::move(const QString &id, int position) {
    const int from = index(id);
    if (position < 0 || position >= items.size() || items[from].state != RunState::Prepared ||
        items[position].state != RunState::Prepared)
        fail(tr("待機中の計算どうしで順番を変更してください。"));
    items.move(from, position);
    try {
        checkOrder();
    } catch (const std::exception &) {
        items.move(position, from);
        throw;
    }
    emit changed();
}
void NwchemQueue::shutdown() {
    closed = true;
    pause();
}
void NwchemQueue::schedule() {
    if (closed || isPaused || run.active() || scheduled || !pending())
        return;
    scheduled = true;
    QTimer::singleShot(0, this, [this] {
        scheduled = false;
        advance();
    });
}
void NwchemQueue::advance() {
    if (closed || isPaused || run.active())
        return;
    for (auto &item : items) {
        if (item.state != RunState::Prepared)
            continue;
        try {
            if (item.checkAccess)
                item.checkAccess();
            // The Preferences as they are now that the entry's turn has come.
            const auto environment = executionEnvironment();
            if (item.chain.active() && item.plan.directory.isEmpty()) {
                // The structure of the earlier job, now that it has ended, and then the bundle.
                const int source = find(item.chain.from);
                auto directory = source >= 0 ? items[source].plan.directory : item.chain.directory;
                auto state = source >= 0 ? items[source].state : RunState::Idle;
                if (source < 0 && !directory.isEmpty())
                    state = RunRecord(readRunRecord(directory, RecordCheck::Json)).state();
                if (state != RunState::Finished || directory.isEmpty())
                    fail(tr("引継ぎ元の計算が正常に終わっていないため開始できません。"));
                item.document = chainedDocument(item.document, directory, item.chain.structure);
                item.chain.directory = directory;
                item.plan = saveExecutionBundle(item.document, item.base, true, environment);
                item.record = runRecordDigest(item.plan.directory);
                emit bundleSaved(item.plan.directory);
            }
            if (QFileInfo(item.plan.directory).isSymLink())
                fail(tr("登録したジョブが変更されたため開始できません: %1").arg(item.plan.directory));
            for (const auto *file : {JobFile::output, JobFile::errors}) {
                const QFileInfo output(QDir(item.plan.directory).filePath(file));
                if (output.exists() || output.isSymLink())
                    fail(tr("登録したジョブが変更されたため開始できません: %1").arg(item.plan.directory));
            }
            historyDocument(item.plan.directory); // Throws on digest/format/reference failures.
            const auto manifest = readRunRecord(item.plan.directory, RecordCheck::Complete);
            if (manifest != item.plan.manifest)
                fail(tr("登録したジョブが変更されたため開始できません: %1").arg(item.plan.directory));
            // Revalidate programs and output collisions after waiting, without changing the snapshot.
            const auto checked =
                executionPlan(item.document, item.base, item.plan.directory, true, environment);
            if (checked.program != item.plan.program || checked.arguments != item.plan.arguments ||
                checked.environment != item.plan.environment || checked.input != item.plan.input)
                fail(tr("登録後に実行条件が変わりました。キューへ登録し直してください。"));
            currentId = item.id;
            run.startPrepared(item.plan);
        } catch (const std::exception &e) {
            currentId.clear();
            item.state = RunState::Failed;
            item.error = QString::fromUtf8(e.what());
            if (stopOnError)
                isPaused = true;
            emit changed();
            schedule();
        }
        return;
    }
}
QJsonObject NwchemQueue::json() const {
    QJsonArray rows;
    for (const auto &item : items) {
        QJsonObject row{{"id", item.id},
                        {"name", item.name},
                        {"directory", item.plan.directory},
                        {"state", item.state == RunState::Prepared ? "queued" : runStateKey(item.state)},
                        {"error", item.error}};
        if (item.chain.active())
            row["chain"] = QJsonObject{{"from", item.chain.from}, {"structure", item.chain.structure}};
        rows.append(row);
    }
    return {{"paused", isPaused}, {"stopOnFailure", stopOnError}, {"pending", pending()}, {"entries", rows}};
}
} // namespace mf
