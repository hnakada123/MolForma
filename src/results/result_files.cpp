// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/result_files.h"
#include "model/document.h"
#include "model/i18n.h"
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
}
QStringList resultFiles(const QString &directory) {
    return QDir(directory).entryList(
        {"*.out", "*.log", "*.xyz", "*.cube", "*.cub", "*.molden", "*.mld", "*.molde", "*.normal"},
        QDir::Files | QDir::Readable, QDir::Name);
}
QVector<ResultFileGroup> resultFileGroups(const QStringList &names) {
    QVector<ResultFileGroup> groups;
    QMap<QPair<QString, int>, QStringList> animations;
    for (const auto &name : names) {
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
QString preferredResultFile(const QString &directory, const QStringList &files) {
    const QDir dir(directory);
    if (!dir.exists())
        fail(tr("フォルダーが見つかりません。"));
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
} // namespace mf
