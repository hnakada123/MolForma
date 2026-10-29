// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/document_storage.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

namespace mf {
void RecoveryCopy::discard() const {
    QFile::remove(file);
}
DocumentStorage::DocumentStorage(QSettings &s, QObject *parent) : QObject(parent), settings(s) {
    timer.setInterval(60000);
    connect(&timer, &QTimer::timeout, this, &DocumentStorage::autosaveRequested);
}
DocumentStorage::~DocumentStorage() {
    close();
}
void DocumentStorage::save(const Document &document, const QString &path) {
    document.save(path);
    projectPath = path;
    remember(path);
    removeAutosave();
}
QStringList DocumentStorage::recentFiles() const {
    return settings.value("recentFiles").toStringList();
}
void DocumentStorage::remember(const QString &path) {
    const auto absolute = QFileInfo(path).absoluteFilePath();
    auto files = recentFiles();
    files.removeAll(absolute);
    files.prepend(absolute);
    while (files.size() > 10)
        files.removeLast();
    settings.setValue("recentFiles", files);
}
QString DocumentStorage::autosaveDirectory() {
#ifdef Q_OS_WIN
    if (const auto home = qEnvironmentVariable("XDG_DATA_HOME"); !home.isEmpty())
        return home + "/MolForma/autosave";
#endif
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/autosave";
}
void DocumentStorage::startAutosave() {
    if (lock)
        return;
    const QDir directory(autosaveDirectory());
    if (!directory.mkpath("."))
        return;
    const auto name = QString("session-%1-%2").arg(QCoreApplication::applicationPid()).arg(newId());
    auto sessionLock = std::make_unique<QLockFile>(directory.filePath(name + ".lock"));
    sessionLock->setStaleLockTime(0);
    if (!sessionLock->tryLock(0))
        return;
    lock = std::move(sessionLock);
    autosavePath = directory.filePath(name + ".molforma.json");
    timer.start();
}
void DocumentStorage::autosave(const Document &document, bool dirty) {
    if (autosavePath.isEmpty())
        return;
    if (!dirty) {
        removeAutosave();
        return;
    }
    // The copy last written; while unchanged it shares its data with the document and compares at once.
    if (saved && *saved == document && QFileInfo::exists(autosavePath))
        return;
    auto project = document.project();
    project["molforma_autosave"] =
        QJsonObject{{"path", projectPath}, {"time", QDateTime::currentDateTime().toString(Qt::ISODate)}};
    // writeFile uses QSaveFile: the previous copy survives a failed replacement.
    writeFile(autosavePath, QJsonDocument(project).toJson(QJsonDocument::Compact));
    saved = document;
}
void DocumentStorage::removeAutosave() {
    if (!autosavePath.isEmpty())
        QFile::remove(autosavePath);
    saved.reset();
}
void DocumentStorage::close() {
    timer.stop();
    removeAutosave();
    autosavePath.clear();
    lock.reset();
}
std::vector<RecoveryCopy> DocumentStorage::recoverable() const {
    std::vector<RecoveryCopy> copies;
    const QDir directory(autosaveDirectory());
    for (const auto &name : directory.entryList({"session-*.molforma.json"}, QDir::Files, QDir::Time)) {
        const auto file = directory.filePath(name);
        if (file == autosavePath)
            continue;
        auto guard = std::make_unique<QLockFile>(
            directory.filePath(name.chopped(QString(".molforma.json").size()) + ".lock"));
        guard->setStaleLockTime(0);
        if (!guard->tryLock(0))
            continue;
        try {
            auto project = parseJson(readFile(file));
            const auto metadata = project.take("molforma_autosave").toObject();
            copies.push_back({Document::fromProject(project), file, metadata["path"].toString(),
                              metadata["time"].toString(), std::move(guard)});
        } catch (const std::exception &) {
            QFile::remove(file);
        }
    }
    return copies;
}
} // namespace mf
