// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/run_record.h"
#include "foundation/error.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "nwchem/execution_plan.h"
#include "nwchem/history.h"
#include "nwchem/run_diagnostics.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

namespace mf {
namespace {
constexpr qint64 recordLimit = 256 * 1024;
QDateTime recordTime(const QJsonObject &record, const char *key) {
    return QDateTime::fromString(record[QLatin1StringView(key)].toString(), Qt::ISODateWithMs);
}
} // namespace
QString jobFile(const QString &directory, const QString &name) {
    return QDir(directory).filePath(name);
}
QByteArray readJobFile(const QString &directory, const QString &name, qint64 limit, bool tail) {
    const auto path = jobFile(directory, name);
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile())
        fail(tr("通常のファイルを指定してください: %1").arg(name));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail(name + ": " + f.errorString());
    if (!tail && f.size() > limit)
        fail(tr("%1: 読込上限を超えています。").arg(name));
    const bool truncated = tail && f.size() > limit;
    if (truncated && !f.seek(f.size() - limit))
        fail(tr("%1: 読込位置を設定できません。").arg(name));
    auto data = f.read(limit + (tail ? 0 : 1));
    if (f.error() != QFile::NoError)
        fail(name + ": " + f.errorString());
    if (!tail && data.size() > limit)
        fail(tr("%1: 読込上限を超えています。").arg(name));
    if (truncated)
        data = tr("…末尾128 KiBを表示しています。\n").toUtf8() + data.mid(data.indexOf('\n') + 1);
    return data;
}

bool RunRecord::hasFormat() const {
    return data["format"] == format;
}
bool RunRecord::supported() const {
    return hasFormat() && data["version"].toDouble() == version;
}
RunState RunRecord::state() const {
    return runStateFromKey(data["status"].toString());
}
QString RunRecord::error() const {
    return data["error"].toString();
}
QDateTime RunRecord::created() const {
    return recordTime(data, "created_at");
}
QDateTime RunRecord::started() const {
    return recordTime(data, "started_at");
}
QDateTime RunRecord::finished() const {
    return recordTime(data, "finished_at");
}
std::optional<int> RunRecord::exitCode() const {
    if (!data.contains("exit_code"))
        return std::nullopt;
    return data["exit_code"].toInt();
}
RunSummary RunRecord::summary() const {
    const auto stored = data["summary"].toObject();
    RunSummary summary;
    summary.recorded = !stored.isEmpty();
    if (stored["energy"].isDouble())
        summary.energy = stored["energy"].toDouble();
    summary.energyMethod = stored["energy_method"].toString();
    summary.energySource = stored["energy_source"].toString();
    summary.normalTermination = stored["normal_termination"].toBool();
    return summary;
}
QVector<RunDiagnostic> RunRecord::diagnostics() const {
    QVector<RunDiagnostic> result;
    for (const auto &value : data["diagnostics"].toArray()) {
        const auto item = value.toObject();
        result.append({item["code"].toString(), item["setting"].toString(), item["source"].toString(),
                       item["evidence"].toString(), settingTargetFromKey(item["target"].toString()),
                       item["tab"].toInt(legacySettingTab(SettingTarget::Execution)), item});
    }
    return result;
}
std::optional<RunRemote> RunRecord::remote() const {
    const auto stored = data["remote"].toObject();
    if (stored.isEmpty())
        return std::nullopt;
    const auto host = stored["host"].toObject();
    return RunRemote{host["name"].toString(),    host["scheduler"].toString(), stored["job"].toString(),
                     stored["phase"].toString(), stored["state"].toString(),   stored["message"].toString(),
                     stored["detail"].toString()};
}

QJsonObject newRunRecord() {
    return {{"format", RunRecord::format}, {"version", RunRecord::version}, {"input", JobFile::input},
            {"stdout", JobFile::output},   {"stderr", JobFile::errors},     {"project", JobFile::project}};
}
QJsonObject readRunRecord(const QString &directory, RecordCheck check) {
    if (check == RecordCheck::Json)
        return parseJson(readFile(jobFile(directory, JobFile::record)));
    QByteArray bytes;
    if (check == RecordCheck::Complete)
        bytes = readJobFile(directory, JobFile::record, recordLimit);
    else {
        QFile file(jobFile(directory, JobFile::record));
        if (!file.open(QIODevice::ReadOnly))
            fail(QString(JobFile::record) + ": " + file.errorString());
        if (file.size() > recordLimit)
            fail(tr("%1: 読込上限を超えています。").arg(QString(JobFile::record)));
        bytes = file.read(recordLimit);
    }
    const RunRecord record(parseJson(bytes));
    const auto &m = record.json();
    if (!record.supported())
        fail(tr("未対応の実行記録です（MolForma NWChem version 1が必要です）。"));
    if (check != RecordCheck::Complete)
        return m;
    const auto state = record.state();
    if (state == RunState::Invalid)
        fail(tr("実行状態が不明です。"));
    if (state == RunState::Finished && (!m["exit_code"].isDouble() || m["exit_code"].toDouble() != 0))
        fail(tr("終了状態と終了コードが一致しません。"));
    const auto names = newRunRecord();
    for (const auto *key : {"input", "project", "stdout", "stderr"})
        if (m[QLatin1StringView(key)] != names[QLatin1StringView(key)])
            fail(tr("実行記録のファイル名が未対応です。"));
    return m;
}
void writeRunRecord(const QString &directory, const QJsonObject &record) {
    const auto path = jobFile(directory, JobFile::record);
    if (QFileInfo(path).isSymLink())
        fail(tr("実行記録がシンボリックリンクです。"));
    QSaveFile file(path);
    const auto bytes = QJsonDocument(record).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(tr("実行記録を保存できません: %1").arg(file.errorString()));
}
QString runRecordDigest(const QString &directory) {
    return QString::fromLatin1(
        QCryptographicHash::hash(readFile(jobFile(directory, JobFile::record)), QCryptographicHash::Sha256)
            .toHex());
}

void recordRunEnd(QJsonObject &record, const QString &directory, const RunEnd &end) {
    record["status"] = runStateKey(end.state);
    if (end.exitCode)
        record["exit_code"] = *end.exitCode;
    if (end.normalExit)
        record["exit_status"] = *end.normalExit ? "normal" : "crash";
    if (end.finished.isValid())
        record["finished_at"] = end.finished.toUTC().toString(Qt::ISODateWithMs);
    if (end.elapsedSeconds)
        record["elapsed_seconds"] = *end.elapsedSeconds;
    if (end.error)
        record["error"] = *end.error;
    if (end.summary)
        record["summary"] = *end.summary;
    else
        try { // The whole log, once: the history list then needs no log reading.
            record["summary"] = nwchemLogSummary(jobFile(directory, JobFile::output));
        } catch (const std::exception &) {
            record.remove("summary");
        }
    if (!end.diagnose && !end.wallTime && !end.needsLog)
        return;
    QString output, errors;
    try {
        output = historyText(directory, JobFile::output, true);
    } catch (const std::exception &) {
        if (end.needsLog)
            throw;
    }
    if (end.wallTime) {
        static const QRegularExpression times("Total times\\s+cpu:\\s*[0-9.]+s\\s+wall:\\s*([0-9.]+)s");
        if (const auto match = times.match(output); match.hasMatch())
            record["elapsed_seconds"] = qint64(match.captured(1).toDouble());
    }
    if (!end.diagnose)
        return;
    try {
        errors = historyText(directory, JobFile::errors, true);
    } catch (const std::exception &) {
    }
    auto diagnostics = diagnoseRun(end.state, output, errors, end.launchError);
    if (!end.launchSetting.isEmpty())
        for (qsizetype i = 0; i < diagnostics.size(); ++i)
            if (auto item = diagnostics[i].toObject(); item["setting"].toString() == "executable") {
                item["setting"] = end.launchSetting;
                diagnostics[i] = item;
            }
    record["diagnostics"] = diagnostics;
}

ExecutionPlan runRecordPlan(const QString &directory, const QJsonObject &record) {
    ExecutionPlan plan;
    plan.directory = directory;
    plan.program = record["program"].toString();
    for (const auto &argument : record["arguments"].toArray())
        plan.arguments << argument.toString();
    const auto overrides = record["environment_overrides"].toObject();
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
        plan.environment[it.key()] = it.value().toString();
    plan.timeoutSeconds = record["timeout_seconds"].toInt();
    plan.manifest = record;
    return plan;
}
} // namespace mf
