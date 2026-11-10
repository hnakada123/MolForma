// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_queue.h"
#include "model/i18n.h"
#include "nwchem/history.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>

namespace mf {
namespace {
// What a job folder's record says, so that a restored entry is the job that was registered.
QString recordDigest(const QString &directory) {
    return QString::fromLatin1(
        QCryptographicHash::hash(readFile(QDir(directory).filePath("run.json")), QCryptographicHash::Sha256)
            .toHex());
}
} // namespace
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
        if (item.id.isEmpty() || directory.isEmpty() || items.size() >= 1000)
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
                item.record = row["record"].toString();
                if (item.record.isEmpty() || recordDigest(directory) != item.record)
                    continue;
                item.document = historyDocument(directory);
                const auto manifest = parseJson(readFile(QDir(directory).filePath("run.json")));
                if (runStateFromKey(manifest["status"].toString()) != RunState::Prepared)
                    continue;
                item.plan.directory = QDir::cleanPath(directory);
                item.plan.manifest = manifest;
                item.plan.program = manifest["program"].toString();
                for (const auto &argument : manifest["arguments"].toArray())
                    item.plan.arguments << argument.toString();
                const auto overrides = manifest["environment_overrides"].toObject();
                for (auto it = overrides.begin(); it != overrides.end(); ++it)
                    item.plan.environment[it.key()] = it.value().toString();
                item.plan.timeoutSeconds = manifest["timeout_seconds"].toInt();
                item.plan.input = historyText(directory, "input.nw");
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
                                    !item.plan.manifest["diagnostics"].toArray().isEmpty()))
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
int NwchemQueue::index(const QString &id) const {
    for (int i = 0; i < items.size(); ++i)
        if (items[i].id == id)
            return i;
    fail(tr("キューの計算が見つかりません。"));
}
int NwchemQueue::pending() const {
    return int(std::count_if(items.begin(), items.end(),
                             [](const auto &item) { return item.state == RunState::Prepared; }));
}
QString NwchemQueue::enqueue(const Document &d, const QString &base, std::function<void()> check) {
    if (closed || items.size() >= 1000)
        fail(tr("キューにはこのセッションで1000件まで登録できます。"));
    if (check)
        check();
    QueuedCalculation item;
    item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.document = d;
    item.base = QFileInfo(base).absoluteFilePath();
    item.name = nwchemSettings(d)["start"].toString();
    item.plan = saveExecutionBundle(d, item.base, true);
    item.record = recordDigest(item.plan.directory);
    item.checkAccess = std::move(check);
    const auto id = item.id;
    items.push_back(std::move(item));
    emit changed();
    schedule();
    return id;
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
    emit changed(); // The prepared bundle remains reusable in the history.
}
void NwchemQueue::move(const QString &id, int position) {
    const int from = index(id);
    if (position < 0 || position >= items.size() || items[from].state != RunState::Prepared ||
        items[position].state != RunState::Prepared)
        fail(tr("待機中の計算どうしで順番を変更してください。"));
    items.move(from, position);
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
            if (QFileInfo(item.plan.directory).isSymLink())
                fail(tr("登録したジョブが変更されたため開始できません: %1").arg(item.plan.directory));
            for (const auto *file : {"output.out", "stderr.err"}) {
                const QFileInfo output(QDir(item.plan.directory).filePath(file));
                if (output.exists() || output.isSymLink())
                    fail(tr("登録したジョブが変更されたため開始できません: %1").arg(item.plan.directory));
            }
            historyDocument(item.plan.directory); // Throws on digest/format/reference failures.
            const auto manifest = parseJson(historyText(item.plan.directory, "run.json", false).toUtf8());
            if (manifest != item.plan.manifest)
                fail(tr("登録したジョブが変更されたため開始できません: %1").arg(item.plan.directory));
            // Revalidate programs and output collisions after waiting, without changing the snapshot.
            const auto checked = executionPlan(item.document, item.base, item.plan.directory, true);
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
    for (const auto &item : items)
        rows.append(
            QJsonObject{{"id", item.id},
                        {"name", item.name},
                        {"directory", item.plan.directory},
                        {"state", item.state == RunState::Prepared ? "queued" : runStateKey(item.state)},
                        {"error", item.error}});
    return {{"paused", isPaused}, {"stopOnFailure", stopOnError}, {"pending", pending()}, {"entries", rows}};
}
} // namespace mf
