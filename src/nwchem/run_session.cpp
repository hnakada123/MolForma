// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/run_session.h"
#include "editing/coordinates.h"
#include "model/settings.h"
#include "nwchem/nwchem.h"
#include <QDir>
#include <QSettings>

namespace mf {
namespace {
// The job a session left to its supervisor, for the next session to follow.
const auto RunningKey = "execution/running";
QString rememberedRun() {
    return QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
        .value(RunningKey)
        .toString();
}
void rememberRun(const QString &directory) {
    QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma").setValue(RunningKey, directory);
}
} // namespace
RunSession::RunSession(QObject *parent) : QObject(parent), runner(this), pending(runner, this) {
    runner.setObjectName("nwchemRun");
    connect(&pending, &NwchemQueue::bundleSaved, this, &RunSession::bundleChanged);
    connect(&runner, &NwchemRun::changed, this, [this] {
        if (runner.state() == previous)
            return;
        previous = runner.state();
        // The next session looks for a run this one may leave to its supervisor.
        if (runner.supervised())
            rememberRun(runner.active() ? runner.plan().directory : QString());
        emit bundleChanged(runner.plan().directory);
    });
}
void RunSession::supervise(const QString &program) {
    runner.setSupervisor(program);
}
bool RunSession::keepQueue(const QString &folder) {
    if (folder.isEmpty() || !QDir().mkpath(folder))
        return false;
    // A second session on the same account keeps its queue to itself.
    queueLock = std::make_unique<QLockFile>(QDir(folder).filePath("queue.lock"));
    queueLock->setStaleLockTime(0);
    if (!queueLock->tryLock(0)) {
        queueLock.reset();
        return false;
    }
    pending.setStore(QDir(folder).filePath("queue.json"));
    return true;
}
RunSession::Restored RunSession::restore(bool withQueue,
                                         const std::function<void(const QString &)> &checkPath) {
    Restored restored;
    if (withQueue && queueLock)
        pending.restore();
    restored.waiting = pending.pending();
    // The run of the last session, if the queue has not taken it up already.
    if (const auto last = rememberedRun(); !last.isEmpty() && !runner.active()) {
        bool allowed = true;
        try {
            if (checkPath)
                checkPath(last);
        } catch (const std::exception &) {
            allowed = false; // Not this session's to look at: it stays for one that may.
        }
        if (allowed && !runner.attach(last)) {
            rememberRun({});
            restored.ended = last;
        }
    }
    restored.following = runner.active();
    return restored;
}
void RunSession::shutdown(bool keepRunning) {
    pending.shutdown();
    if (keepRunning && runner.supervised())
        runner.release();
    else
        runner.shutdown();
}
ExecutionPlan RunSession::saveBundle(const Document &document, const QString &base) {
    bundle = saveExecutionBundle(document, base);
    emit bundleChanged(bundle.directory);
    return bundle;
}
std::function<void()> RunSession::turn(const Check &check, const Document &document, const QString &base) {
    if (!check)
        return {};
    return [check, document, base] { check(document, base); };
}
QString RunSession::enqueue(const Document &document, const QString &base, const Check &check,
                            const QueueChain &chain) {
    const auto id = pending.enqueue(document, base, turn(check, document, base), chain);
    // A chained entry saves its bundle when its turn comes (NwchemQueue::bundleSaved).
    if (!chain.active())
        emit bundleChanged(pending.entries().last().plan.directory);
    return id;
}
QStringList RunSession::enqueueChain(const Document &current, const QString &base, const QString &from,
                                     const QString &structure, const QStringList &steps, bool endFrequencies,
                                     const Check &check) {
    return pending.enqueueChain(current, base, from, structure, steps, endFrequencies,
                                [check, base](const Document &next) { return turn(check, next, base); });
}
QStringList RunSession::enqueueGeometries(const Document &current, const QString &base,
                                          const QVector<Document> &geometries, bool frequencies,
                                          const Check &check) {
    if (geometries.isEmpty() || geometries.size() + pending.entries().size() > 1000)
        fail(tr("キューにはこのセッションで1000件まで登録できます。"));
    QVector<Document> documents;
    for (int i = 0; i < geometries.size(); ++i) {
        auto d = current;
        applyResultGeometry(d, geometries[i]);
        setNwchemSetting(d, "task", frequencies ? "optimize_frequencies" : "optimize");
        setNwchemSetting(d, "start", nwchemSettings(d)["start"].toString() + QString("_conf%1").arg(i + 1));
        executionPlan(d, base, {}, true); // Validate the entire batch before writing any bundle.
        documents.append(d);
    }
    QStringList directories;
    for (const auto &d : documents) {
        pending.enqueue(d, base, turn(check, d, base));
        directories << pending.entries().last().plan.directory;
        emit bundleChanged(directories.last());
    }
    return directories;
}
} // namespace mf
