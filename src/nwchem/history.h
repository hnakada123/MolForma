// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include "nwchem/run_state.h"
#include <QDateTime>
#include <QDirIterator>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <memory>
#include <optional>

namespace mf {
// The most jobs a history holds, and the most folders a scan looks into for them.
inline constexpr int historyJobLimit = 2000, historyFolderLimit = 20000;
struct HistoryEntry {
    QString directory, name, method, basis, task, formula, memo;
    // The level of theory in short, as lists show it: the method (the functional of a DFT method, "HF"
    // for Hartree–Fock) and the basis, "B3LYP / 6-31G*". Empty when the record gives neither.
    QString level;
    RunState status = RunState::Invalid;
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
// The state in words for details: "終了（コード0）", or whether an active record still runs.
QString historyStatus(RunState status, bool owned = false, bool orphaned = false);
// An energy method as shown: run.json keeps the mark of a counterpoise-corrected energy in Japanese
// (see nwchemLogSummary).
QString historyEnergyMethod(const QString &method);
// Energy of a whole NWChem log, read once when a run ends (run.json "summary"): that of the last
// task with one, the reference structure's for a frequency task, the corrected one for BSSE.
// {"energy", "energy_method", "energy_source": final|frequency_reference|bsse_corrected,
// "normal_termination"}; no "energy" if the log has none. wallSeconds gets the wall time NWChem printed
// at its end, when the log has it: the time of a run that MolForma did not see.
QJsonObject nwchemLogSummary(const QString &path, std::optional<double> *wallSeconds = nullptr);
// Marks an orphaned record (see HistoryEntry::orphaned) "interrupted", with its log summary.
void repairHistoryEntry(const QString &directory);
// A calculation that ran outside MolForma (another computer, a terminal) becomes a job of the history.
// `log` is NWChem's output, or a job folder. A job that MolForma saved ("保存のみ") and that now holds
// its output.out is completed where it is when it lies in root, and copied there first when it does
// not; any other log gets a new job folder in root with the input beside it (same name, .nw) and the
// files named after them. Returns the job folder.
QString importExternalRun(const QString &log, const QString &root);
// Whether a job saved without running holds an output that importExternalRun would register.
bool externalRunWaiting(const HistoryEntry &entry);
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
// The molecule a job was saved with (its project.molforma.json): the atoms, ghosts included, the charge
// and the multiplicity.
struct HistoryMolecule {
    int atoms = 0, charge = 0, multiplicity = 1;
};
HistoryMolecule historyMolecule(const QString &directory);
// The job folders (with run.json) directly in root, counted up to limit; -1 if root cannot be read.
int historyJobCount(const QString &root, int limit = historyJobLimit);

// The history as a list shows it (history_list.cpp): the state of each job in a word, the jobs that
// match a search and a state in an order, and their energies against a reference job. The history tab
// shows what these give; they hold no widget.

// A record that says its job runs, while this session does not run it (it does not own the job): the
// process has ended (orphaned), or whether it runs is not known. Any other record is as recorded.
enum class HistoryPresence { Recorded, Orphaned, Unknown };
HistoryPresence historyPresence(const HistoryEntry &entry, bool owned);
// The state in a word: the name of the recorded state, or "プロセスなし" ("未接続" for a job on another
// computer) and "稼働未確認" for the records above.
QString historyStateName(const HistoryEntry &entry, bool owned);
// The state a list is narrowed by: "active", "finished", "prepared" or "stopped" (also failed jobs and
// orphaned records).
QString historyStateGroup(const HistoryEntry &entry, bool owned);
// Whether a job can be the reference of ΔE: one that has ended, with an energy.
bool historyReference(const HistoryEntry &entry);
// What keeps the energy of a job from being compared with that of a reference job as ΔE: the first of
// another composition (or an unknown one), method, basis or kind of energy.
enum class HistoryDifference { None, Formula, Method, Basis, EnergyKind };
HistoryDifference historyDifference(const HistoryEntry &entry, const HistoryEntry &reference);
// That difference in words ("組成が基準と異なる", …); empty when they compare.
QString historyIncomparable(const HistoryEntry &entry, const HistoryEntry &reference);
// Why a job has no ΔE against the reference: it has not ended, it has no energy, or historyIncomparable.
// Empty when it has one.
QString historyComparisonNote(const HistoryEntry &entry, const HistoryEntry &reference);
enum class HistorySort { State, Name, Level, Task, Created, Duration, Energy, Delta, Memo };
struct HistoryQuery {
    // Kept are the jobs whose name, method, basis, task, composition, memo, folder or state has this
    // text (in any case), and of those the jobs of this state group; empty for all.
    QString search, state;
    HistorySort sort = HistorySort::Created;
    bool ascending = false;
    QString reference; // The job folder ΔE refers to; empty (or not a reference) for no comparison.
    QString active;    // The job folder this session runs: its record is owned.
};
struct HistoryRow {
    int entry = 0;               // Its place among the entries.
    bool owned = false;          // The run of this session.
    std::optional<double> delta; // ΔE / Hartree against the reference.
    QString note;                // Why there is no ΔE, when there is a reference.
};
struct HistoryList {
    // The jobs kept, in the order asked for: those with a value of the sorted column first, the newest
    // first among equals.
    QVector<HistoryRow> rows;
    // How many jobs match the search, by state group and, under "", in all.
    QMap<QString, int> counts;
    double largest = 0; // The largest |ΔE| of the rows.
};
HistoryList historyList(const QVector<HistoryEntry> &entries, const HistoryQuery &query);

// Incremental, bounded directory scan, a few milliseconds per event. Entries of ended jobs whose files
// have not changed since an earlier scan are not read again.
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
    // By folder: the stamp of its files and its entry, of this scan and of earlier ones.
    QHash<QString, std::pair<QString, HistoryEntry>> read, earlier;
    void step();
};
} // namespace mf
