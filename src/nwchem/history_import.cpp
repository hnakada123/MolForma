// SPDX-License-Identifier: GPL-2.0-or-later
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_execution.h"
#include "nwchem/nwchem_import.h"
#include "nwchem/run_diagnostics.h"
#include "results/results.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>

namespace mf {
namespace {
constexpr qint64 inputLimit = 8 * 1024 * 1024;
constexpr int fileLimit = 2000;
bool regularFile(const QString &path) {
    const QFileInfo info(path);
    return info.isFile() && !info.isSymLink();
}
QString digest(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
void copyFile(const QString &from, const QString &to) {
    if (!QDir().mkpath(QFileInfo(to).absolutePath()) || !QFile::copy(from, to))
        fail(tr("ファイルを複写できません: %1").arg(QFileInfo(from).fileName()));
}
// The files and folders of a job folder, without following links.
void copyFolder(const QString &from, const QString &to, int &count) {
    for (const auto &info :
         QDir(from).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (info.isSymLink())
            continue;
        if (++count > fileLimit)
            fail(tr("ファイルが多すぎるフォルダーは取り込めません（%1個まで）。").arg(fileLimit));
        const auto target = QDir(to).filePath(info.fileName());
        if (info.isDir())
            copyFolder(info.absoluteFilePath(), target, count);
        else
            copyFile(info.absoluteFilePath(), target);
    }
}
void writeRecord(const QString &directory, const QJsonObject &manifest) {
    const auto path = QDir(directory).filePath("run.json");
    if (QFileInfo(path).isSymLink())
        fail(tr("実行記録がシンボリックリンクです。"));
    QSaveFile file(path);
    const auto bytes = QJsonDocument(manifest).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(tr("実行記録を保存できません: %1").arg(file.errorString()));
}
// What the log of a run says about its end, added to the record of a job: the state, the energy
// summary, the wall time NWChem printed and the known failures.
void recordLog(const QString &directory, QJsonObject &manifest) {
    const auto log = QDir(directory).filePath("output.out");
    const auto summary = nwchemLogSummary(log);
    const bool normal = summary["normal_termination"].toBool();
    const auto state = normal ? RunState::Finished : RunState::Interrupted;
    manifest["status"] = runStateKey(state);
    manifest["external"] = true;
    manifest["summary"] = summary;
    manifest["finished_at"] = QFileInfo(log).lastModified().toUTC().toString(Qt::ISODateWithMs);
    if (normal)
        manifest["exit_code"] = 0;
    else
        // Stored in Japanese whatever the language; the history shows it through translated().
        manifest["error"] = QT_TR_NOOP("外部で実行した計算のログに、正常終了の記録がありません。");
    const auto end = historyText(directory, "output.out", true);
    static const QRegularExpression times("Total times\\s+cpu:\\s*[0-9.]+s\\s+wall:\\s*([0-9.]+)s");
    if (const auto match = times.match(end); match.hasMatch())
        manifest["elapsed_seconds"] = qint64(match.captured(1).toDouble());
    if (!regularFile(QDir(directory).filePath("stderr.err")))
        writeFile(QDir(directory).filePath("stderr.err"), {});
    QString errors;
    try {
        errors = historyText(directory, "stderr.err", true);
    } catch (const std::exception &) {
    }
    manifest["diagnostics"] = diagnoseRun(state, end, errors);
}
// A job folder MolForma saved without running, which now holds the output of a run elsewhere.
QString completeSavedJob(const QString &directory) {
    const auto entry = readHistoryEntry(directory);
    if (!entry.valid)
        fail(entry.warnings.join('\n'));
    if (entry.status != RunState::Prepared)
        fail(tr("このジョブは実行済みです。結果を登録できるのは「保存のみ」のジョブです。"));
    if (!regularFile(QDir(directory).filePath("output.out")))
        fail(tr("ジョブフォルダーにNWChemの出力（output.out）がありません。実行先から複写してください。"));
    historyDocument(directory); // The input and project must still be those that were saved.
    auto manifest = entry.manifest;
    manifest["directory"] = directory;
    recordLog(directory, manifest);
    writeRecord(directory, manifest);
    return directory;
}
bool nwchemLog(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const auto head = file.read(64 * 1024);
    return head.contains("Northwest Computational Chemistry Package") || head.contains("NWChem Input Module");
}
// The first structure of a log, for a job without an input MolForma can read.
Document firstStructure(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(QFileInfo(path).fileName() + ": " + file.errorString());
    ResultData result;
    NwchemLog log(&result);
    QByteArray buffer(64 * 1024 + 1, Qt::Uninitialized);
    while (!file.atEnd() && result.frames.isEmpty()) {
        const qint64 size = file.readLine(buffer.data(), buffer.size());
        if (size < 0)
            break;
        log.addBytes(QByteArrayView(buffer.constData(), size));
    }
    if (result.frames.isEmpty())
        fail(tr("ログに構造がなく、NWChemの入力（同じ名前の .nw）も読めません。"));
    return result.frames.first().molecule;
}
QString importLog(const QString &path, const QString &root) {
    const QFileInfo log(path);
    if (!regularFile(path) || !nwchemLog(path))
        fail(tr("NWChemの出力ファイルを選んでください: %1").arg(log.fileName()));
    const QDir folder(log.absolutePath());
    // The input of the run: the file of the same name, or the only input in the folder.
    QString input = folder.filePath(log.completeBaseName() + ".nw");
    if (!regularFile(input)) {
        const auto inputs = folder.entryList({"*.nw"}, QDir::Files);
        input = inputs.size() == 1 ? folder.filePath(inputs.first()) : QString();
    }
    QByteArray inputBytes;
    Document d;
    bool read = false;
    if (!input.isEmpty() && regularFile(input) && QFileInfo(input).size() <= inputLimit) {
        inputBytes = readFile(input);
        try {
            d = importNwchemInput(QString::fromUtf8(inputBytes)).document;
            read = !d.atoms.isEmpty();
        } catch (const std::exception &) {
        }
    }
    if (!read) {
        d = firstStructure(path);
        auto name = log.completeBaseName();
        name.remove(QRegularExpression("[^A-Za-z0-9_-]"));
        if (!name.isEmpty() && name[0].isLetter())
            setNwchemSetting(d, "start", name.left(64));
    }
    if (inputBytes.isEmpty())
        inputBytes = "# The input of this calculation was not found beside its output.\n";
    applyExecutionPreferences(d);
    const auto errors = d.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    const auto start = nwchemSettings(d)["start"].toString();
    if (!QDir().mkpath(root))
        fail(tr("ジョブ保存先を作成できません: %1").arg(root));
    QTemporaryDir directory(
        QDir(root).filePath(start + '-' + log.lastModified().toString("yyyyMMdd-HHmmss") + "-XXXXXX"));
    if (!directory.isValid())
        fail(tr("ジョブフォルダーを作成できません: %1").arg(directory.errorString()));
    // What the run wrote under its names: orbitals, the database, files for viewing.
    QStringList prefixes{log.completeBaseName(), start};
    prefixes.removeDuplicates();
    int count = 0;
    for (const auto &info : folder.entryInfoList(QDir::Files | QDir::Hidden)) {
        const auto name = info.fileName();
        const bool named = std::any_of(prefixes.begin(), prefixes.end(), [&](const QString &prefix) {
            return name.startsWith(prefix + '.') || name.startsWith(prefix + '_');
        });
        if (!named || info.isSymLink() || info.absoluteFilePath() == log.absoluteFilePath() ||
            info.absoluteFilePath() == QFileInfo(input).absoluteFilePath() ||
            QStringList{"input.nw", "output.out", "stderr.err", "run.json", "project.molforma.json"}.contains(
                name))
            continue;
        if (++count > fileLimit)
            fail(tr("ファイルが多すぎるフォルダーは取り込めません（%1個まで）。").arg(fileLimit));
        copyFile(info.absoluteFilePath(), directory.filePath(name));
    }
    copyFile(log.absoluteFilePath(), directory.filePath("output.out"));
    writeFile(directory.filePath("input.nw"), inputBytes);
    const auto project = QJsonDocument(d.project()).toJson();
    writeFile(directory.filePath("project.molforma.json"), project);
    QJsonObject manifest{{"format", "molforma-nwchem-run"},
                         {"version", 1},
                         {"created_at", log.lastModified().toUTC().toString(Qt::ISODateWithMs)},
                         {"directory", directory.path()},
                         {"program", ""},
                         {"arguments", QJsonArray()},
                         {"environment_overrides", QJsonObject()},
                         {"reference_directory", folder.absolutePath()},
                         {"project_directory", folder.absolutePath()},
                         {"calculation", nwchemSettings(d)},
                         {"formula", d.formula()},
                         {"input", "input.nw"},
                         {"stdout", "output.out"},
                         {"stderr", "stderr.err"},
                         {"project", "project.molforma.json"},
                         {"timeout_seconds", 0},
                         {"input_sha256", digest(inputBytes)},
                         {"project_sha256", digest(project)},
                         {"imported_from", log.absoluteFilePath()}};
    recordLog(directory.path(), manifest);
    writeRecord(directory.path(), manifest);
    directory.setAutoRemove(false);
    return directory.path();
}
} // namespace
bool externalRunWaiting(const HistoryEntry &entry) {
    return entry.valid && entry.status == RunState::Prepared &&
           regularFile(QDir(entry.directory).filePath("output.out"));
}
QString importExternalRun(const QString &source, const QString &root) {
    const QFileInfo info(source);
    if (info.isSymLink() || !info.exists())
        fail(tr("取り込むNWChemの出力またはジョブフォルダーがありません: %1").arg(source));
    const auto folder = QDir::cleanPath(info.isDir() ? info.absoluteFilePath() : info.absolutePath());
    const auto home = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    // A job MolForma saved: the folder is the job, whatever file of it was chosen.
    if (regularFile(QDir(folder).filePath("run.json")) &&
        regularFile(QDir(folder).filePath("project.molforma.json"))) {
        if (QFileInfo(folder).absolutePath() == home)
            return completeSavedJob(folder);
        if (!QDir().mkpath(home))
            fail(tr("ジョブ保存先を作成できません: %1").arg(home));
        QTemporaryDir directory(QDir(home).filePath(QFileInfo(folder).fileName() + "-XXXXXX"));
        if (!directory.isValid())
            fail(tr("ジョブフォルダーを作成できません: %1").arg(directory.errorString()));
        int count = 0;
        copyFolder(folder, directory.path(), count);
        const auto job = completeSavedJob(directory.path());
        directory.setAutoRemove(false);
        return job;
    }
    if (info.isDir()) {
        // A plain folder: the one NWChem output in it.
        QStringList logs;
        for (const auto &name : QDir(folder).entryList({"*.out", "*.log"}, QDir::Files))
            if (nwchemLog(QDir(folder).filePath(name)))
                logs << name;
        if (logs.size() != 1)
            fail(logs.isEmpty()
                     ? tr("フォルダーにNWChemの出力（.out・.log）がありません。")
                     : tr("フォルダーにNWChemの出力が複数あります。取り込む出力ファイルを選んでください。"));
        return importLog(QDir(folder).filePath(logs.first()), home);
    }
    return importLog(info.absoluteFilePath(), home);
}
} // namespace mf
