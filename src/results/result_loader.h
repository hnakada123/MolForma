// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <memory>

namespace mf {
struct ResultLoadRequest {
    QString path;
    double isovalue = .03;
    int dataset = 0, orbital = -1;
    QString normalization = "auto";
    int divisions = 40;
    // Read all steps of the freq-animate mode identified by path, rediscovering them on each load.
    bool animation = false;
    // Read the file again even if this session has read it (an explicit reload).
    bool reread = false;
};
struct ResultFileGroup {
    QStringList files; // Names relative to the directory, in step order for an animation.
    QString animationName;
    int mode = -1; // -1 for an ordinary file.
};
QVector<ResultFileGroup> resultFileGroups(const QString &directory);
// The same file selection and cancellable reader for GUI and headless sessions.
QString preferredResultFile(const QString &directory);
QString resultSourceStamp(const QString &path);
class ResultLoader : public QObject {
    Q_OBJECT
  public:
    explicit ResultLoader(QObject *parent = nullptr);
    ~ResultLoader() override;
    void start(const ResultLoadRequest &request);
    bool cancel();
    bool active() const {
        return process != nullptr;
    }
    QString pendingPath() const {
        return source;
    }
  signals:
    void loaded(mf::ResultData data, QString stamp);
    void failed(QString message);

  private:
    QProcess *process = nullptr;
    // A worker started ahead of the next request, after the first one: starting a process is most of
    // the time that another orbital or isovalue of a file read before takes.
    QProcess *ready = nullptr;
    QProcess *worker(const QString &request, const QString &response);
    std::unique_ptr<QTemporaryDir> scratch;
    // The readings kept for this loader's session (readResult), removed with it.
    std::unique_ptr<QTemporaryDir> cache;
    QString source;
    void release();
};
} // namespace mf
