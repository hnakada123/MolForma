// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/result_loader.h"
#include "model/i18n.h"
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
    const QFileInfo source(request.path);
    if (!request.animation)
        return {source.absoluteFilePath()};
    const auto match = animationStep.match(source.fileName());
    if (match.hasMatch())
        for (const auto &group : resultFileGroups(source.absolutePath()))
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
QVector<ResultFileGroup> resultFileGroups(const QString &directory) {
    QVector<ResultFileGroup> groups;
    QMap<QPair<QString, int>, QStringList> animations;
    for (const auto &name : resultFiles(directory)) {
        const auto match = animationStep.match(name);
        if (match.hasMatch())
            animations[{match.captured(1), match.captured(2).toInt()}] << name;
        else
            groups.append({{name}, {}, -1});
    }
    for (auto it = animations.begin(); it != animations.end(); ++it) {
        auto files = it.value();
        std::stable_sort(files.begin(), files.end(), [](const QString &a, const QString &b) {
            return animationStep.match(a).captured(3).toInt() < animationStep.match(b).captured(3).toInt();
        });
        groups.append({files, it.key().first, it.key().second});
    }
    return groups;
}
QString preferredResultFile(const QString &directory) {
    const QDir dir(directory);
    if (!dir.exists())
        fail(tr("フォルダーが見つかりません。"));
    const auto files = resultFiles(dir.absolutePath());
    if (files.isEmpty())
        fail(tr("対応する結果ファイルがありません。"));
    return dir.absoluteFilePath(files.contains("output.out") ? "output.out" : files.first());
}
QString resultSourceStamp(const QString &path) {
    const QFileInfo file(path);
    if (!file.isFile())
        return {};
    return QString::fromUtf8(
        QJsonDocument(
            QJsonArray{
                file.absoluteFilePath(), file.canonicalFilePath(), QString::number(file.size()),
                QString::number(file.lastModified().toMSecsSinceEpoch()),
                QString::number(file.fileTime(QFileDevice::FileMetadataChangeTime).toMSecsSinceEpoch())})
            .toJson(QJsonDocument::Compact));
}
ResultLoader::ResultLoader(QObject *parent) : QObject(parent) {}
ResultLoader::~ResultLoader() {
    cancel();
    if (ready) {
        ready->kill();
        ready->waitForFinished(2000);
    }
}
QProcess *ResultLoader::worker(const QString &request, const QString &response) {
    const auto start = [this](QProcess *p, const QStringList &arguments) {
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
    const auto path = paths.first();
    // Keep the single-file stamp compatible with the orbital cache.
    const auto stamp = q.animation ? sourceStamp(paths) : resultSourceStamp(path);
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
            [this, p, q, paths, path, stamp, response](int code, QProcess::ExitStatus status) {
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
                        (q.animation ? sourceFiles(q) != paths || sourceStamp(paths) != stamp
                                     : resultSourceStamp(path) != stamp))
                        fail(tr("読込中にファイルが更新されました。再読込してください。"));
                    data = loadResultData(response);
                } catch (const std::exception &e) {
                    error = QString::fromUtf8(e.what());
                }
                // Release before notifying: a receiver may immediately request another orbital/file.
                release();
                if (error.isEmpty())
                    emit loaded(std::move(data), stamp);
                else
                    emit failed(error);
            });
}
} // namespace mf
