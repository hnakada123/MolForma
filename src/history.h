#pragma once
#include "document.h"
#include <QDateTime>
#include <QDirIterator>
#include <QObject>
#include <QTimer>
#include <memory>
#include <optional>

namespace mf {
struct HistoryEntry {
    QString directory, name, method, basis, task, status;
    QDateTime created;
    QJsonObject manifest;
    QStringList warnings;
    QString energyMethod;
    std::optional<double> energy;
    qint64 seconds = -1;
    bool valid = false;
};
QString historyRoot(const Document &document, const QString &baseDirectory);
HistoryEntry readHistoryEntry(const QString &directory);
QString historyStatus(const QString &status, bool owned = false);
QString historyChoice(const QString &key, const QString &value);
// Reads only fixed bundle filenames, rejects symlinks and bounds all reads.
QString historyText(const QString &directory, const QString &name, bool tail = false);
QString historyIntegrity(const HistoryEntry &entry);
Document historyDocument(const QString &directory);
void trashHistoryEntry(const QString &directory, const QString &root, const QString &activeDirectory);
QString historyCsv(const QVector<HistoryEntry> &entries);

// Incremental, bounded directory scan. Each event reads at most one bundle.
class HistoryScan : public QObject {
    Q_OBJECT
  public:
    explicit HistoryScan(QObject *parent = nullptr);
    void start(const QString &root);
    void cancel();
    bool active() const {
        return timer.isActive();
    }
    QVector<HistoryEntry> entries;
    QString message;
  signals:
    void finished();

  private:
    QTimer timer;
    std::unique_ptr<QDirIterator> iterator;
    int examined = 0;
    void step();
};
} // namespace mf
