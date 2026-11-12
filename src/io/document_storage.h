// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <QLockFile>
#include <QObject>
#include <QSettings>
#include <QTimer>
#include <memory>
#include <optional>
#include <vector>

namespace mf {
// The project file (.molforma.json) of a document. Saving checks the document first (Document::validate)
// and replaces the file in one step; loading fails for a file that is no version-1 project.
void saveProject(const Document &document, const QString &path);
Document loadProject(const QString &path);
struct RecoveryCopy {
    Document document;
    QString file, origin, time;
    std::unique_ptr<QLockFile> lock; // Held while the caller presents the recovery choice.
    void discard() const;
};
// Project persistence, recent files and recovery copies; presentation stays with the caller.
class DocumentStorage : public QObject {
    Q_OBJECT
  public:
    explicit DocumentStorage(QSettings &settings, QObject *parent = nullptr);
    ~DocumentStorage() override;
    QString path() const {
        return projectPath;
    }
    void setPath(const QString &path) {
        projectPath = path;
    }
    void save(const Document &document, const QString &path);
    QStringList recentFiles() const;
    void remember(const QString &path);
    static QString autosaveDirectory();
    void startAutosave();
    void autosave(const Document &document, bool dirty);
    void removeAutosave();
    void close();
    std::vector<RecoveryCopy> recoverable() const;
  signals:
    void autosaveRequested();

  private:
    QSettings &settings;
    QString projectPath, autosavePath;
    QTimer timer;
    std::unique_ptr<QLockFile> lock;
    std::optional<Document> saved;
};
} // namespace mf
