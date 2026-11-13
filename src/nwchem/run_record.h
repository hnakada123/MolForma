// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/setting_target.h"
#include "nwchem/run_state.h"
#include <QDateTime>
#include <QJsonObject>
#include <QVector>
#include <optional>

namespace mf {
struct ExecutionPlan;

// The job folder of a calculation and its record run.json (the "manifest") have their one owner here:
// the names of the files, reading and writing the record, what is recorded when a run ends, the plan a
// record was saved from and what a record says. Other code names no file of a job folder and no key of
// the record that this header offers.
namespace JobFile {
inline constexpr const char *record = "run.json";                // The record of the job and of its run.
inline constexpr const char *project = "project.molforma.json";  // The project the job was saved from.
inline constexpr const char *input = "input.nw";                 // The NWChem input.
inline constexpr const char *output = "output.out";              // NWChem's standard output: the log.
inline constexpr const char *errors = "stderr.err";              // NWChem's standard error.
inline constexpr const char *script = "job.sh";                  // Starts a job that runs elsewhere.
inline constexpr const char *exitCode = "exit_code";             // NWChem's exit code, left by job.sh.
inline constexpr const char *stopRequest = "stop.request";       // Asks the supervisor to stop the run.
inline constexpr const char *memo = "memo.txt";                  // The user's note on the job.
inline constexpr const char *supervisorLock = "supervisor.lock"; // Held by a remote run's supervisor.
} // namespace JobFile
// The path of a file of a job folder.
QString jobFile(const QString &directory, const QString &name);
// The bytes of a file of a job folder: a regular file (no symbolic link) of at most `limit` bytes. With
// `tail`, the last `limit` bytes of a longer file, from a whole line on and under a note that says so.
QByteArray readJobFile(const QString &directory, const QString &name, qint64 limit, bool tail = false);

// What a run's log or its end said went wrong (run.json "diagnostics", see diagnoseRun): a stable code,
// the setting it points at with its semantic owner, where it was found and the line itself.
// Descriptions come from runDiagnosisText(code), never from the record.
struct RunDiagnostic {
    QString code, setting, source, evidence;
    std::optional<SettingTarget> target; // None in records written before semantic targets.
    // The number such records stored instead ("tab", see legacySettingTab).
    int legacyTab = legacySettingTab(SettingTarget::Execution);
    QJsonObject stored; // As recorded, for clients that pass its fields on (MCP).
    // The owner of the setting, whichever way the record names it.
    SettingTarget owner() const {
        return target ? *target : settingTargetFromLegacyTab(legacyTab);
    }
};
// The summary of the whole log, read once when the run ended (run.json "summary", see
// nwchemLogSummary). Not recorded for older records and for runs that have not ended.
struct RunSummary {
    bool recorded = false;
    std::optional<double> energy;       // In Hartree; none when the log has no energy.
    QString energyMethod, energySource; // As stored: see historyEnergyMethod; final|frequency_reference|…
    bool normalTermination = false;     // Whether NWChem printed its normal end.
};
// The part of a record about a run on another computer (run.json "remote", see remote_run.h).
struct RunRemote {
    QString destination, scheduler; // The destination as it was when the job was saved.
    QString job, phase, state;      // The job there, how far its supervisor has come, how the job stands.
    QString message, detail;        // While the destination cannot be reached: why (shown via translated()).
};
// A typed view of a record. The stored form stays the QJsonObject (ExecutionPlan::manifest,
// HistoryEntry::manifest); nothing here changes its keys.
class RunRecord {
  public:
    static constexpr const char *format = "molforma-nwchem-run";
    static constexpr int version = 1;
    RunRecord() = default;
    explicit RunRecord(QJsonObject record) : data(std::move(record)) {}
    const QJsonObject &json() const {
        return data;
    }
    // A record of MolForma's runs, of any version (what the supervisor of a run asks), and of the
    // version this code reads (what the history asks).
    bool hasFormat() const;
    bool supported() const;
    RunState state() const; // Invalid for a record without a known state.
    // Why the run ended as it did: kept in Japanese whatever the language, shown through translated().
    QString error() const;
    // Invalid when the record has no such time.
    QDateTime created() const;
    QDateTime started() const;
    QDateTime finished() const;
    std::optional<int> exitCode() const; // None when no exit code is recorded.
    RunSummary summary() const;
    QVector<RunDiagnostic> diagnostics() const;
    std::optional<RunRemote> remote() const; // None for a run on this computer.

  private:
    QJsonObject data;
};

// A record with what every record says: its format and version, and the files of its job folder under
// their names. The one who saves a job adds the rest.
QJsonObject newRunRecord();
// How much a reader asks of the record it reads.
enum class RecordCheck {
    // A JSON object, whatever its size: the processes that start, follow and supervise a run read the
    // record of the job they were given (a record with a pasted basis can be larger than the history reads).
    Json,
    // At most 256 KiB, of MolForma's format and version 1.
    Supported,
    // As the history takes a job: besides, a regular file (no symbolic link), a known state, exit code 0
    // for a finished run and the files of the job under their names.
    Complete
};
// Reads run.json of a job folder. Throws when it cannot be read or is not what `check` asks; a caller
// that tolerates a missing or broken record catches that.
QJsonObject readRunRecord(const QString &directory, RecordCheck check);
// Replaces run.json of a job folder in one step, so that a reader never sees half a record. Refuses a
// record that is a symbolic link, and throws when the record cannot be saved.
void writeRunRecord(const QString &directory, const QJsonObject &record);
// The SHA-256 of run.json as it is in the job folder, so that a job can be told from a changed one.
QString runRecordDigest(const QString &directory);

// How a run ended, for recordRunEnd. What its caller does not give is not written: each of them knows
// a different part (a process that ended, a job on another computer, a log brought from elsewhere, a
// record whose processes are gone).
struct RunEnd {
    RunState state = RunState::Interrupted;
    std::optional<int> exitCode = {};          // "exit_code".
    std::optional<bool> normalExit = {};       // "exit_status": "normal", or "crash".
    QDateTime finished = {};                   // "finished_at", in UTC.
    std::optional<qint64> elapsedSeconds = {}; // "elapsed_seconds".
    std::optional<QString> error = {};         // "error": in Japanese whatever the language.
    // The summary of the log when the caller has read it already (nwchemLogSummary). Otherwise it is
    // read here, and a log that cannot be read leaves the record without "summary".
    std::optional<QJsonObject> summary = {};
    bool diagnose = true;       // "diagnostics", from the ends of the output and of the errors.
    QString launchError = {};   // Why the program did not start, for a failed run.
    QString launchSetting = {}; // The setting such a failure points at when not the executable ("remote").
    bool needsLog = false;      // Throws when the end of the output cannot be read.
};
// The one place that records the end of a run in its record: the state, the exit code and status, the
// time, the elapsed seconds, the error, the summary of the log and the diagnostics. Reads output.out and
// stderr.err of the job folder; the caller saves the record (writeRunRecord).
void recordRunEnd(QJsonObject &record, const QString &directory, const RunEnd &end);

// The plan a record was saved from, for running or following the job of `directory` again: the program,
// its arguments, the environment overrides, the time limit and the record itself. The input stays unread.
ExecutionPlan runRecordPlan(const QString &directory, const QJsonObject &record);
} // namespace mf
