#pragma once
#include "model/document.h"
#include <QDateTime>
#include <QDirIterator>
#include <QObject>
#include <QTimer>
#include <memory>
#include <optional>

namespace mf {
struct HistoryEntry {
    QString directory, name, method, basis, task, status, formula, memo;
    QDateTime created;
    QJsonObject manifest;
    QStringList warnings;
    QString energyMethod;
    std::optional<double> energy;
    qint64 seconds = -1;
    bool valid = false;
    bool orphaned = false; // Recorded as starting/running/stopping, but its process has ended.
};
QString historyRoot(const Document &document, const QString &baseDirectory);
HistoryEntry readHistoryEntry(const QString &directory);
QString historyStatus(const QString &status, bool owned = false, bool orphaned = false);
// Energy of a whole NWChem log, read once when a run ends (run.json "summary"): that of the last
// task with one, the reference structure's for a frequency task, the corrected one for BSSE.
// {"energy", "energy_method", "energy_source": final|frequency_reference|bsse_corrected,
// "normal_termination"}; no "energy" if the log has none.
QJsonObject nwchemLogSummary(const QString &path);
// Marks an orphaned record (see HistoryEntry::orphaned) "interrupted", with its log summary.
void repairHistoryEntry(const QString &directory);
QString historyChoice(const QString &key, const QString &value);
// Reads only fixed bundle filenames, rejects symlinks and bounds all reads.
QString historyText(const QString &directory, const QString &name, bool tail = false);
QString historyIntegrity(const HistoryEntry &entry);
Document historyDocument(const QString &directory);
void trashHistoryEntry(const QString &directory, const QString &root, const QString &activeDirectory);
QString historyCsv(const QVector<HistoryEntry> &entries);
// The user's note on a job: memo.txt in the job folder, outside the files the bundle hashes.
QString historyMemo(const QString &directory);
void setHistoryMemo(const QString &directory, const QString &text);

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
