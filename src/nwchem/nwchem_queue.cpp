// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_queue.h"
#include "model/i18n.h"
#include "nwchem/history.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>

namespace mf {
NwchemQueue::NwchemQueue(NwchemRun &runner, QObject *parent) : QObject(parent), run(runner) {
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
