#include "nwchem/history.h"
#include "model/i18n.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_execution.h"
#include "results/results.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <cmath>

namespace mf {
namespace {
constexpr qint64 textLimit = 128 * 1024, projectLimit = 8 * 1024 * 1024;
const QStringList states{"prepared", "starting",  "running",   "stopping",   "finished",
                         "failed",   "cancelled", "timed_out", "interrupted"};
QString resolve(QString path, const QString &base) {
    if (path == "~")
        path = QDir::homePath();
    else if (path.startsWith("~/"))
        path = QDir::home().filePath(path.mid(2));
    return QDir::cleanPath(QDir(base).absoluteFilePath(path));
}
QByteArray bytes(const QString &directory, const QString &name, qint64 limit, bool tail = false) {
    const auto path = QDir(directory).filePath(name);
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
QString digest(const QByteArray &b) {
    return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex());
}
QJsonObject manifest(const QString &directory) {
    auto m = parseJson(bytes(directory, "run.json", 256 * 1024));
    if (m["format"] != "molforma-nwchem-run" || m["version"].toDouble() != 1)
        fail(tr("未対応の実行記録です（MolForma NWChem version 1が必要です）。"));
    if (!states.contains(m["status"].toString()))
        fail(tr("実行状態が不明です。"));
    if (m["status"] == "finished" && (!m["exit_code"].isDouble() || m["exit_code"].toDouble() != 0))
        fail(tr("終了状態と終了コードが一致しません。"));
    for (auto pair : {qMakePair("input", "input.nw"), qMakePair("project", "project.kgui.json"),
                      qMakePair("stdout", "output.out"), qMakePair("stderr", "stderr.err")})
        if (m[pair.first] != pair.second)
            fail(tr("実行記録のファイル名が未対応です。"));
    return m;
}
bool pending(const QString &s) {
    return s == "starting" || s == "running" || s == "stopping";
}
// An energy method as shown: run.json keeps the mark of a counterpoise-corrected energy in Japanese
// (see nwchemLogSummary).
QString energyMethodText(const QString &method) {
    const QString bsse = "・BSSE補正"; // i18n-exempt (as stored in run.json)
    return method.endsWith(bsse) ? tr("%1・BSSE補正").arg(method.chopped(bsse.size())) : method;
}
// error: why the file does not match its recorded digest.
void checkDigest(const QByteArray &data, const QJsonObject &m, const QString &key, const QString &error) {
    if (!m.contains(key) && key == "project_sha256")
        return; // Compatibility with original v1 bundles.
    if (m[key].toString() != digest(data))
        fail(error);
}
QString inputMismatch() {
    return tr("入力が保存時のSHA-256と一致しません。");
}
QString projectMismatch() {
    return tr("プロジェクトが保存時のSHA-256と一致しません。");
}
} // namespace
QString historyRoot(const Document &d, const QString &base) {
    return resolve(executionSettings(d)["job_root"].toString(), base);
}
QString historyChoice(const QString &key, const QString &value) {
    for (const auto &c : nwchemChoices(key))
        if (c.value == value)
            return c.label;
    return value;
}
QString historyStatus(const QString &s, bool owned, bool orphaned) {
    if (pending(s) && !owned)
        return orphaned ? tr("中断・プロセスなし（記録: %1）").arg(s) : tr("稼働未確認（記録: %1）").arg(s);
    const QMap<QString, QString> names{
        {"prepared", tr("入力保存済み")},    {"starting", tr("起動中")},
        {"running", tr("実行中")},           {"stopping", tr("停止処理中")},
        {"finished", tr("終了（コード0）")}, {"failed", tr("実行失敗")},
        {"cancelled", tr("停止済み")},       {"timed_out", tr("時間制限", "state")},
        {"invalid", tr("記録を読めません")}, {"interrupted", tr("中断（記録を修復）")}};
    return names.value(s, tr("不明"));
}
HistoryEntry readHistoryEntry(const QString &directory) {
    HistoryEntry e;
    e.directory = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
    e.name = QFileInfo(e.directory).fileName();
    e.status = "invalid";
    try {
        e.manifest = manifest(e.directory);
        e.status = e.manifest["status"].toString();
        e.created = QDateTime::fromString(e.manifest["created_at"].toString(), Qt::ISODateWithMs);
        if (!e.created.isValid())
            e.warnings << tr("作成日時を読めません。");
        double seconds = e.manifest["elapsed_seconds"].toDouble(-1);
        if (std::isfinite(seconds) && seconds >= 0 && seconds < 1e12)
            e.seconds = qint64(seconds);
        e.valid = true;
        e.orphaned = pending(e.status) && executionProcessGone(e.manifest);
        auto calculation = e.manifest["calculation"].toObject();
        if (calculation.isEmpty()) {
            const auto project = parseJson(bytes(e.directory, "project.kgui.json", projectLimit));
            calculation = project["document"].toObject()["nwchem"].toObject();
            auto defaults = nwchemDefaults();
            for (auto i = calculation.begin(); i != calculation.end(); ++i)
                defaults[i.key()] = i.value();
            calculation = defaults;
        }
        const auto name = calculation["start"].toString();
        if (!name.isEmpty())
            e.name = name;
        e.method = historyChoice("method", calculation["method"].toString());
        if (calculation["method"] == "dft")
            e.method += " / " + nwchemFunctionalLabel(calculation);
        e.basis = nwchemBasisLabel(calculation);
        e.task = historyChoice("task", calculation["task"].toString());
        // Composition tells jobs with the same calculation name apart (older records: from the project).
        e.formula = e.manifest["formula"].toString();
        if (e.formula.isEmpty())
            e.formula =
                Document::fromProject(parseJson(bytes(e.directory, "project.kgui.json", projectLimit)))
                    .formula();
    } catch (const std::exception &error) {
        e.warnings << QString::fromUtf8(error.what());
    }
    try {
        e.memo = historyMemo(e.directory);
    } catch (const std::exception &error) {
        e.warnings << QString::fromUtf8(error.what());
    }
    if (const auto summary = e.manifest["summary"].toObject(); summary["energy"].isDouble()) {
        e.energy = summary["energy"].toDouble();
        e.energyMethod = summary["energy_method"].toString();
    } else if (QFileInfo::exists(QDir(e.directory).filePath("output.out"))) {
        // Older records and unfinished runs: the last energy in the end of the log.
        try {
            const auto text = historyText(e.directory, "output.out", true);
            static const QRegularExpression energy(
                R"((?:^|\n)\s*Total\s+(SCF|DFT|MP2|CCSD(?:\(T\))?)\s+energy\s*[:=]?\s*([-+0-9.eEdD]+)[ \t]*(?=\r?\n))",
                QRegularExpression::CaseInsensitiveOption);
            auto matches = energy.globalMatch(text);
            while (matches.hasNext()) {
                const auto match = matches.next();
                auto value = match.captured(2);
                value.replace('D', 'E', Qt::CaseInsensitive);
                bool ok;
                const double n = value.toDouble(&ok);
                if (ok && std::isfinite(n)) {
                    e.energy = n;
                    e.energyMethod = match.captured(1).toUpper();
                }
            }
        } catch (const std::exception &error) {
            e.warnings << QString::fromUtf8(error.what());
        }
    }
    return e;
}
QJsonObject nwchemLogSummary(const QString &path) {
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile())
        fail(tr("通常のファイルを指定してください: %1").arg(info.fileName()));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail(info.fileName() + ": " + f.errorString());
    NwchemEnergySummary energy;
    while (!f.atEnd()) {
        const auto line = f.readLine(64 * 1024);
        if (NwchemEnergySummary::relevant(line))
            energy.add(QString::fromLatin1(line).trimmed());
    }
    if (f.error() != QFile::NoError)
        fail(info.fileName() + ": " + f.errorString());
    QJsonObject summary{{"normal_termination", energy.normalTermination()}};
    if (const auto e = energy.energy(); !e.isEmpty()) {
        summary["energy"] = e["hartree"];
        // Stored in run.json and compared between jobs, so it stays the same in every language;
        // the history translates it where it shows it.
        summary["energy_method"] =
            e["method"].toString() + (e["source"] == "bsse_corrected" ? "・BSSE補正" : ""); // i18n-exempt
        summary["energy_source"] = e["source"];
    }
    return summary;
}
void repairHistoryEntry(const QString &directory) {
    auto m = manifest(directory);
    if (!pending(m["status"].toString()) || !executionProcessGone(m))
        fail(
            tr("実行中の可能性がある記録は修復できません（このPCで記録され、プロセスが終了したものだけ）。"));
    m["status"] = "interrupted";
    m["repaired_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    // Stored in Japanese whatever the language; the history shows it through translated().
    if (m["error"].toString().isEmpty())
        m["error"] = QT_TR_NOOP("記録の更新前にMolFormaが終了し、計算のプロセスもありませんでした。");
    try {
        m["summary"] = nwchemLogSummary(QDir(directory).filePath("output.out"));
    } catch (const std::exception &) {
        m.remove("summary");
    }
    const auto path = QDir(directory).filePath("run.json");
    if (QFileInfo(path).isSymLink())
        fail(tr("実行記録がシンボリックリンクです。"));
    QSaveFile file(path);
    const auto bytes = QJsonDocument(m).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(tr("実行記録を保存できません: %1").arg(file.errorString()));
}
QString historyMemo(const QString &directory) {
    if (!QFileInfo::exists(QDir(directory).filePath("memo.txt")))
        return {};
    return QString::fromUtf8(bytes(directory, "memo.txt", 4096)).trimmed();
}
void setHistoryMemo(const QString &directory, const QString &text) {
    if (text.size() > 1000)
        fail(tr("メモは1000文字以内で入力してください。"));
    const auto m = manifest(directory); // Only MolForma job folders.
    Q_UNUSED(m);
    const auto path = QDir(directory).filePath("memo.txt");
    if (QFileInfo(path).isSymLink())
        fail(tr("メモのファイルがシンボリックリンクです。"));
    if (text.trimmed().isEmpty()) {
        QFile::remove(path);
        return;
    }
    writeFile(path, text.trimmed().toUtf8() + '\n');
}
HistoryMolecule historyMolecule(const QString &directory) {
    const auto d = Document::fromProject(parseJson(bytes(directory, "project.kgui.json", projectLimit)));
    return {int(d.atoms.size()), d.job()["charge"].toInt(), d.job()["multiplicity"].toInt(1)};
}
QString historyIncomparable(const HistoryEntry &e, const HistoryEntry &reference) {
    if (e.formula.isEmpty() || e.formula != reference.formula)
        return tr("組成が基準と異なる");
    if (e.method != reference.method)
        return tr("手法が基準と異なる");
    if (e.basis != reference.basis)
        return tr("基底が基準と異なる");
    if (e.energyMethod != reference.energyMethod)
        return tr("エネルギーの種類が基準と異なる");
    return {};
}
int historyJobCount(const QString &root, int limit) {
    if (!QFileInfo(root).isDir() || !QFileInfo(root).isReadable())
        return -1;
    QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    int count = 0, examined = 0;
    while (it.hasNext() && count < limit && examined++ < 20000)
        count += QFileInfo::exists(QDir(it.next()).filePath("run.json"));
    return count;
}
QString historyText(const QString &directory, const QString &name, bool tail) {
    if (!QStringList{"input.nw", "output.out", "stderr.err", "run.json"}.contains(name))
        fail(tr("履歴で表示できないファイルです。"));
    return QString::fromUtf8(bytes(directory, name, tail ? textLimit : projectLimit, tail));
}
QString historyIntegrity(const HistoryEntry &e) {
    try {
        const auto m = manifest(e.directory);
        checkDigest(bytes(e.directory, "input.nw", projectLimit), m, "input_sha256", inputMismatch());
        if (!m.contains("project_sha256"))
            return tr("入力SHA-256一致。旧記録のためプロジェクトの照合情報なし。");
        checkDigest(bytes(e.directory, "project.kgui.json", projectLimit), m, "project_sha256",
                    projectMismatch());
        return tr("入力・プロジェクトのSHA-256一致。");
    } catch (const std::exception &error) {
        return QString::fromUtf8(error.what());
    }
}
Document historyDocument(const QString &directory) {
    const auto m = manifest(directory);
    const auto input = bytes(directory, "input.nw", projectLimit);
    const auto project = bytes(directory, "project.kgui.json", projectLimit);
    checkDigest(input, m, "input_sha256", inputMismatch());
    checkDigest(project, m, "project_sha256", projectMismatch());
    auto d = Document::fromProject(parseJson(project));
    const auto reference = m["reference_directory"].toString();
    if (!QDir::isAbsolutePath(reference))
        fail(tr("再利用に必要な参照ファイルの基準が不明です。"));
    setExecutionSetting(d, "reference_directory", reference);
    setExecutionSetting(d, "job_root", QFileInfo(directory).absolutePath());
    const auto base = m["project_directory"].toString();
    const auto s = executionSettings(d);
    for (const auto &key : {"executable", "launcher"}) {
        const auto value = s[key].toString();
        if (!QDir::isAbsolutePath(value) && (value.contains('/') || value.startsWith('~'))) {
            if (QDir::isAbsolutePath(base))
                setExecutionSetting(d, key, resolve(value, base));
            else {
                // Original v1 runs stored resolved commands but not the source project's directory.
                const bool mpi = s["mode"] == "mpi";
                if (!mpi && QString(key) == "launcher")
                    continue;
                const auto resolved = mpi && QString(key) == "executable"
                                          ? m["arguments"].toArray().at(2).toString()
                                          : m["program"].toString();
                if (!QDir::isAbsolutePath(resolved))
                    fail(tr("旧記録の相対実行ファイルパスを復元できません。"));
                setExecutionSetting(d, key, resolved);
            }
        }
    }
    return d;
}
void trashHistoryEntry(const QString &directory, const QString &root, const QString &active) {
    const QFileInfo info(directory);
    if (info.isSymLink() || !info.isDir() ||
        info.canonicalFilePath() == QFileInfo(active).canonicalFilePath() ||
        QFileInfo(info.absolutePath()).canonicalFilePath() != QFileInfo(root).canonicalFilePath())
        fail(tr("このジョブフォルダーはごみ箱へ移動できません。"));
    const auto m = manifest(directory);
    if (pending(m["status"].toString()) && !executionProcessGone(m))
        fail(tr("稼働未確認・実行中のジョブは移動できません。"));
    if (!QFile::moveToTrash(directory))
        fail(tr("ごみ箱へ移動できません。保存先を確認してください。"));
}
QString historyCsv(const QVector<HistoryEntry> &entries) {
    // The header in the interface language; the values as recorded (the state as its key), but for
    // the BSSE mark of an energy method.
    QString text = tr("作成日時UTC,計算名,手法,基底,目的,記録状態,経過秒,計算のエネルギーHartree,"
                      "エネルギー手法,フォルダー,組成,メモ") +
                   "\r\n";
    for (const auto &e : entries) {
        QStringList values{e.created.toUTC().toString(Qt::ISODateWithMs),
                           e.name,
                           e.method,
                           e.basis,
                           e.task,
                           e.status,
                           e.seconds < 0 ? "" : QString::number(e.seconds),
                           e.energy ? QString::number(*e.energy, 'g', 16) : "",
                           energyMethodText(e.energyMethod),
                           e.directory,
                           e.formula,
                           e.memo};
        for (int i = 0; i < values.size(); ++i) {
            auto &v = values[i];
            // Text columns must remain text when a spreadsheet opens the exported CSV.
            if (i != 6 && i != 7 && QRegularExpression("^[\\s]*[=+@-]").match(v).hasMatch())
                v.prepend('\'');
            v.replace('"', "\"\"");
            v = '"' + v + '"';
        }
        text += values.join(',') + "\r\n";
    }
    return text;
}
HistoryScan::HistoryScan(QObject *parent) : QObject(parent) {
    timer.setInterval(0);
    connect(&timer, &QTimer::timeout, this, &HistoryScan::step);
}
void HistoryScan::start(const QString &root) {
    cancel();
    entries.clear();
    message.clear();
    examined = 0;
    if (!QFileInfo(root).isDir() || !QFileInfo(root).isReadable()) {
        message = tr("ジョブ保存先がないか、読めません: %1").arg(root);
        emit finished();
        return;
    }
    iterator = std::make_unique<QDirIterator>(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    timer.start();
}
void HistoryScan::cancel() {
    timer.stop();
    iterator.reset();
}
void HistoryScan::step() {
    if (!iterator)
        return;
    if (entries.size() >= 2000 || examined >= 20000) {
        message = tr("走査上限（2000ジョブ / 20000フォルダー）に達しました。保存先を分割してください。");
        cancel();
        emit finished();
        return;
    }
    if (!iterator->hasNext()) {
        cancel();
        emit finished();
        return;
    }
    const auto directory = iterator->next();
    ++examined;
    if (QFileInfo::exists(QDir(directory).filePath("run.json")))
        entries << readHistoryEntry(directory);
}
} // namespace mf
