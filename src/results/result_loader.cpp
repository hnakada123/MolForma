// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/result_loader.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>

namespace mf {
namespace {
const QRegularExpression animationStep(R"(^(.*)\.m-(\d+)\.s-(\d+)\.xyz$)");
QStringList sourceFiles(const ResultLoadRequest &request) {
    // A combination is made of its CUBE files; what it writes is read once it exists.
    if (!request.combine.isEmpty()) {
        QStringList paths;
        for (const auto &term : request.combine)
            paths << QFileInfo(term.path).absoluteFilePath();
        return paths;
    }
    const QFileInfo source(request.path);
    if (!request.animation)
        return {source.absoluteFilePath()};
    const auto match = animationStep.match(source.fileName());
    if (match.hasMatch())
        for (const auto &group : resultFileGroups(resultFiles(source.absolutePath())))
            if (group.mode == match.captured(2).toInt() && group.animationName == match.captured(1)) {
                QStringList paths;
                for (const auto &name : group.files)
                    paths << QDir(source.absolutePath()).filePath(name);
                return paths;
            }
    fail(tr("振動アニメーションのファイルが見つかりません。"));
}
QString sourceStamp(const QStringList &paths) {
    QJsonArray stamps;
    for (const auto &path : paths) {
        const auto stamp = resultSourceStamp(path);
        if (stamp.isEmpty())
            return {};
        stamps.append(stamp);
    }
    return QString::fromUtf8(QJsonDocument(stamps).toJson(QJsonDocument::Compact));
}
} // namespace
ResultLoader::ResultLoader(QObject *parent) : QObject(parent) {}
ResultLoader::~ResultLoader() {
    cancel();
    if (ready) {
        ready->kill();
        ready->waitForFinished(2000);
    }
}
QProcess *ResultLoader::worker(const QString &request, const QString &response) {
    const auto start = [](QProcess *p, const QStringList &arguments) {
        p->setProgram(QCoreApplication::applicationFilePath());
        p->setArguments(arguments);
        p->setStandardOutputFile(QProcess::nullDevice());
        p->setStandardErrorFile(QProcess::nullDevice());
        p->start();
    };
    QProcess *p = std::exchange(ready, nullptr);
    if (p && p->state() != QProcess::NotRunning) { // Writes wait while it is still starting.
        p->write(QJsonDocument(QJsonObject{{"request", request}, {"response", response}})
                     .toJson(QJsonDocument::Compact) +
                 '\n');
        p->closeWriteChannel();
    } else {
        if (p) // It ended while waiting.
            p->deleteLater();
        p = new QProcess(this);
        start(p, {"--result-worker", request, response});
    }
    // The next one starts now, while this one reads.
    ready = new QProcess(this);
    start(ready, {"--result-worker-ready"});
    return p;
}
void ResultLoader::release() {
    auto *p = std::exchange(process, nullptr);
    if (p) {
        p->disconnect(this);
        p->deleteLater();
    }
    scratch.reset();
    source.clear();
}
bool ResultLoader::cancel() {
    if (!process)
        return false;
    process->disconnect(this);
    process->kill();
    process->waitForFinished(2000);
    release();
    return true;
}
void ResultLoader::start(const ResultLoadRequest &q) {
    cancel();
    auto work = std::make_unique<QTemporaryDir>();
    if (!work->isValid())
        fail(tr("一時フォルダーを作成できません。"));
    const auto paths = sourceFiles(q);
    const bool combined = !q.combine.isEmpty();
    const auto path = combined ? QFileInfo(q.path).absoluteFilePath() : paths.first();
    // Keep the single-file stamp compatible with the orbital cache.
    const auto stamp = q.animation || combined ? sourceStamp(paths) : resultSourceStamp(path);
    QJsonArray terms;
    for (const auto &term : q.combine)
        terms.append(QJsonObject{{"path", QFileInfo(term.path).absoluteFilePath()},
                                 {"factor", term.factor},
                                 {"dataset", term.dataset}});
    const auto request = work->filePath("request.json"), response = work->filePath("result.bin");
    if (!cache || q.reread)
        cache = std::make_unique<QTemporaryDir>();
    writeFile(
        request,
        QJsonDocument(
            QJsonObject{{"path", path},
                        {"isovalue", q.isovalue},
                        {"dataset", q.dataset},
                        {"orbital", q.orbital},
                        {"normalization", q.normalization},
                        {"orbital_divisions", q.divisions},
                        {"animation_files", q.animation ? QJsonArray::fromStringList(paths) : QJsonArray{}},
                        {"combine", terms},
                        {"cache", cache->isValid() && !q.animation ? cache->path() : QString()}})
            .toJson());
    scratch = std::move(work);
    source = path;
    auto *p = process = worker(request, response);
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (process != p || e != QProcess::FailedToStart)
            return;
        const auto message = p->errorString();
        release();
        emit failed(message);
    });
    connect(p, &QProcess::finished, this,
            [this, p, q, paths, path, stamp, response, combined](int code, QProcess::ExitStatus status) {
                if (process != p)
                    return;
                ResultData data;
                QString error;
                try {
                    if (code != 0 || status != QProcess::NormalExit)
                        fail(QFileInfo::exists(response + ".error")
                                 ? QString::fromUtf8(readFile(response + ".error"))
                                 : tr("結果の読込プロセスが失敗しました。"));
                    if (stamp.isEmpty() ||
                        (q.animation || combined ? sourceFiles(q) != paths || sourceStamp(paths) != stamp
                                                 : resultSourceStamp(path) != stamp))
                        fail(tr("読込中にファイルが更新されました。再読込してください。"));
                    data = loadResultData(response);
                } catch (const std::exception &e) {
                    error = QString::fromUtf8(e.what());
                }
                // Release before notifying: a receiver may immediately request another orbital/file.
                release();
                if (error.isEmpty())
                    emit loaded(std::move(data), combined ? resultSourceStamp(path) : stamp);
                else
                    emit failed(error);
            });
}
} // namespace mf
