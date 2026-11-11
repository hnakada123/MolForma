// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_restart.h"
#include "editing/coordinates.h"
#include "model/i18n.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_output.h"
#include "results/results.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace mf {
namespace {
// The folders of a job that NWChem wrote its permanent and scratch files into, the job folder first.
// Folders outside the job are not looked at: empty then.
QStringList jobFolders(const Document &d, const QString &job) {
    QStringList folders{QDir::cleanPath(job)};
    const auto advanced = nwchemAdvanced(d);
    for (const auto *key : {"permanent_dir", "scratch_dir"}) {
        if (!advanced.contains(key))
            continue;
        const auto value = advanced[key].toString();
        const auto clean = QDir::cleanPath(value);
        if (QDir::isAbsolutePath(value) || clean == ".." || clean.startsWith("../"))
            return {};
        const auto folder = QDir::cleanPath(QDir(job).filePath(value));
        if (!folders.contains(folder))
            folders << folder;
    }
    return folders;
}
QString permanentFolder(const Document &d, const QString &job) {
    return QDir::cleanPath(QDir(job).filePath(nwchemAdvanced(d)["permanent_dir"].toString(".")));
}
bool regularFile(const QString &path) {
    const QFileInfo info(path);
    return info.isFile() && !info.isSymLink();
}
// NWChem's scratch files (integrals, grids) and what MolForma asked it to write for viewing.
bool carried(const QString &file, const QString &prefix) {
    const auto suffix = file.mid(prefix.size() + 1);
    static const QStringList left{"cube", "cub", "molden", "normal", "xyz", "out", "err", "nw"};
    return !suffix.startsWith("gridpts.") && !suffix.startsWith("aoints.") &&
           !left.contains(QFileInfo(file).suffix().toLower());
}
struct LogEnd {
    int started = 0;
    bool ended = false;
    Document geometry; // The last structure printed; no atoms if none.
};
LogEnd readLog(const QString &job) {
    const auto path = QDir(job).filePath("output.out");
    if (!regularFile(path))
        fail(tr("再開元のログ（output.out）がありません。"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail("output.out: " + file.errorString());
    ResultData result;
    NwchemLog log(&result);
    QByteArray buffer(64 * 1024 + 1, Qt::Uninitialized);
    while (!file.atEnd()) {
        const qint64 size = file.readLine(buffer.data(), buffer.size());
        if (size < 0)
            break;
        log.addBytes(QByteArrayView(buffer.constData(), size));
        // Only the last structure is wanted, however long the optimization was.
        if (result.frames.size() > 1)
            result.frames.remove(0, result.frames.size() - 1);
    }
    if (file.error() != QFile::NoError)
        fail("output.out: " + file.errorString());
    LogEnd end{log.started, log.ended, {}};
    if (!result.frames.isEmpty())
        end.geometry = result.frames.last().molecule;
    return end;
}
// The saved project of a job at the last structure of its log. A log of another atom order, or one
// that moved fixed atoms, leaves the saved structure.
Document latestDocument(const QString &job, const LogEnd &log) {
    auto d = historyDocument(job);
    if (!log.geometry.atoms.isEmpty()) {
        auto moved = d;
        try {
            applyResultGeometry(moved, log.geometry);
            d = moved;
        } catch (const std::exception &) {
        }
    }
    return d;
}
} // namespace
QString restartDatabase(const QString &job) {
    Document d;
    try {
        d = historyDocument(job);
    } catch (const std::exception &) {
        return {};
    }
    if (jobFolders(d, job).isEmpty())
        return {};
    const auto path = QDir(permanentFolder(d, job)).filePath(nwchemSettings(d)["start"].toString() + ".db");
    return regularFile(path) ? path : QString();
}
QString restartLabel(const QString &mode) {
    return mode == "continue"  ? tr("続きから実行を準備")
           : mode == "hessian" ? tr("このHessianで遷移状態探索を準備")
                               : tr("Hessianを再利用して振動数を準備");
}
QVector<RestartChoice> restartChoices(const QString &job) {
    QString all; // A reason that rules out every way.
    Document d;
    const auto entry = readHistoryEntry(job);
    if (!entry.valid)
        all = tr("実行記録を読めません。");
    else if (runStateActive(entry.status) && !entry.orphaned)
        all = tr("計算が終わってから準備できます。");
    else if (entry.status == RunState::Prepared)
        all = tr("まだ実行していない計算です。");
    else
        try {
            d = historyDocument(job);
            if (jobFolders(d, job).isEmpty())
                all = tr("permanent_dir・scratch_dirをジョブの外に指定した計算は、MolFormaから再開できません"
                         "。");
            else if (restartDatabase(job).isEmpty())
                all = tr("NWChemのデータベース（%1.db）がジョブフォルダーにありません。")
                          .arg(nwchemSettings(d)["start"].toString());
        } catch (const std::exception &e) {
            all = QString::fromUtf8(e.what());
        }
    if (!all.isEmpty())
        return {{"continue", all}, {"hessian", all}, {"reuse", all}};
    const auto s = nwchemSettings(d);
    const NwchemCalculation calc(s);
    const bool normal = entry.status == RunState::Finished;
    RestartChoice next{"continue", {}};
    if (normal)
        next.reason = tr("正常に終わった計算です。条件を変えて計算する場合は「分子・条件を再利用」を使います"
                         "。");
    else if (calc.path() || s["bsse"].toBool())
        next.reason = tr("座標スキャン・IRC・NEB・BSSE補正の計算は再開できません。");
    const auto hessian = QDir(permanentFolder(d, job)).filePath(s["start"].toString() + ".hess");
    QString without;
    if (!normal)
        without = tr("振動数の計算が正常に終わったジョブで使えます。");
    else if (!calc.frequencies() || !regularFile(hessian))
        without =
            tr("振動数計算のHessian（%1.hess）がジョブフォルダーにありません。").arg(s["start"].toString());
    else if (s["bsse"].toBool())
        without = tr("座標スキャン・IRC・NEB・BSSE補正の計算は再開できません。");
    return {next, {"hessian", without}, {"reuse", without}};
}
Document restartDocument(const QString &job, const QString &mode) {
    for (const auto &choice : restartChoices(job))
        if (choice.mode == mode && !choice.available())
            fail(choice.reason);
    if (!QStringList{"continue", "hessian", "reuse"}.contains(mode))
        fail("Unknown restart mode: " + mode);
    const auto log = readLog(job);
    auto d = latestDocument(job, log);
    auto s = d.data["nwchem"].toObject();
    QJsonArray done;
    if (mode == "continue") {
        const auto tasks = nwchemTaskLines(historyText(job, "input.nw"));
        // The task under way when NWChem stopped is the first to run again.
        const int finished = log.ended ? int(tasks.size()) : std::max(0, log.started - 1);
        if (finished >= tasks.size())
            fail(tr("この計算に残っているタスクはありません。"));
        for (const auto &line : tasks.mid(0, finished))
            done << line;
    } else {
        // Another calculation at the structure of the Hessian: only what the Hessian is for.
        s["task"] = mode == "hessian" ? "saddle" : "frequencies";
        for (const auto *key :
             {"followup", "tddft", "prop_mulliken", "prop_esp", "prop_nmr", "prop_polarizability",
              "prop_raman", "prop_epr", "prop_ord", "prop_esp_grid", "prop_efield_grid"})
            s.remove(key);
        if (mode == "reuse")
            s.remove("numerical_frequencies");
    }
    d.data["nwchem"] = s;
    // NWChem goes on from the orbitals of the database, and writes those of visualizations again.
    for (const auto *key : {"vectors_guess", "vectors_input"})
        setNwchemAdvanced(d, key, QJsonValue::Undefined);
    if (mode != "continue")
        for (const auto *key : {"cube_output", "molden_output"})
            setNwchemAdvanced(d, key, QJsonValue::Undefined);
    setNwchemSetting(d, "restart",
                     QJsonObject{{"mode", mode},
                                 {"job", QDir::cleanPath(QFileInfo(job).absoluteFilePath())},
                                 {"done", done},
                                 {"geometry", nwchemRestartDigest(d, mode != "reuse")}});
    return d;
}
Document orbitalReuseDocument(const QString &job) {
    const auto entry = readHistoryEntry(job);
    if (!entry.valid || entry.status == RunState::Prepared ||
        (runStateActive(entry.status) && !entry.orphaned))
        fail(tr("計算が終わったジョブを選んでください。"));
    auto d = latestDocument(job, readLog(job));
    const auto s = nwchemSettings(d);
    const auto name = nwchemAdvanced(d)["vectors_output"].toString(s["start"].toString() + ".movecs");
    if (jobFolders(d, job).isEmpty() && !QDir::isAbsolutePath(nwchemPermanentFile(d, name)))
        fail(tr("permanent_dir・scratch_dirをジョブの外に指定した計算の分子軌道は引き継げません。"));
    const auto movecs = QDir::cleanPath(QDir(job).absoluteFilePath(nwchemPermanentFile(d, name)));
    if (!regularFile(movecs))
        fail(tr("分子軌道のファイルがジョブフォルダーにありません: %1").arg(QFileInfo(movecs).fileName()));
    setNwchemSetting(d, "restart", QJsonObject());
    setNwchemAdvanced(d, "vectors_guess", "file");
    setNwchemAdvanced(d, "vectors_input", movecs);
    // NWChem would otherwise write the new orbitals over the file it read.
    setNwchemAdvanced(d, "vectors_output", s["start"].toString() + ".movecs");
    return d;
}
QString restartSource(const Document &d) {
    const auto s = nwchemSettings(d);
    const auto job = nwchemRestart(s).job;
    if (job.isEmpty() || !regularFile(QDir(job).filePath("run.json")) || jobFolders(d, job).isEmpty())
        return {};
    return QDir(permanentFolder(d, job)).filePath(s["start"].toString() + ".db");
}
void copyRestartFiles(const Document &d, const QString &target) {
    const auto s = nwchemSettings(d);
    const auto restart = nwchemRestart(s);
    if (!restart.active())
        return;
    const auto prefix = s["start"].toString();
    const auto folders = jobFolders(d, restart.job);
    if (!regularFile(restartSource(d)))
        fail(tr("再開元のNWChemデータベースがありません: %1").arg(restart.job));
    QStringList files;
    for (const auto &folder : folders)
        for (const auto &name : QDir(folder).entryList({prefix + ".*"}, QDir::Files | QDir::Hidden))
            if (carried(name, prefix))
                files << QDir(folder).filePath(name);
    // Orbitals written under another name are part of the database, too.
    if (const auto advanced = nwchemAdvanced(d); advanced.contains("vectors_output"))
        if (const auto movecs = nwchemPermanentFile(d, advanced["vectors_output"].toString());
            !QDir::isAbsolutePath(movecs))
            files << QDir::cleanPath(QDir(restart.job).filePath(movecs));
    files.removeDuplicates();
    const QDir source(QDir::cleanPath(restart.job));
    for (const auto &file : std::as_const(files)) {
        if (!regularFile(file))
            continue;
        const auto relative = source.relativeFilePath(file);
        if (relative.startsWith(".."))
            continue;
        const auto copy = QDir(target).filePath(relative);
        if (!QDir().mkpath(QFileInfo(copy).absolutePath()) || !QFile::copy(file, copy))
            fail(tr("再開元のファイルを複写できません: %1").arg(relative));
    }
}
} // namespace mf
