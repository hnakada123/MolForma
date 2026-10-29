#include "results/result_loader.h"
#include "model/i18n.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

namespace mf {
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
    const auto path = QFileInfo(q.path).absoluteFilePath(), stamp = resultSourceStamp(path);
    const auto request = work->filePath("request.json"), response = work->filePath("result.bin");
    writeFile(request, QJsonDocument(QJsonObject{{"path", path},
                                                 {"isovalue", q.isovalue},
                                                 {"dataset", q.dataset},
                                                 {"orbital", q.orbital},
                                                 {"normalization", q.normalization},
                                                 {"orbital_divisions", q.divisions}})
                           .toJson());
    scratch = std::move(work);
    source = path;
    auto *p = process = new QProcess(this);
    p->setProgram(QCoreApplication::applicationFilePath());
    p->setArguments({"--result-worker", request, response});
    p->setStandardOutputFile(QProcess::nullDevice());
    p->setStandardErrorFile(QProcess::nullDevice());
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (process != p || e != QProcess::FailedToStart)
            return;
        const auto message = p->errorString();
        release();
        emit failed(message);
    });
    connect(p, &QProcess::finished, this,
            [this, p, path, stamp, response](int code, QProcess::ExitStatus status) {
                if (process != p)
                    return;
                ResultData data;
                QString error;
                try {
                    if (code != 0 || status != QProcess::NormalExit)
                        fail(QFileInfo::exists(response + ".error")
                                 ? QString::fromUtf8(readFile(response + ".error"))
                                 : tr("結果の読込プロセスが失敗しました。"));
                    if (stamp.isEmpty() || resultSourceStamp(path) != stamp)
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
    p->start();
}
} // namespace mf
