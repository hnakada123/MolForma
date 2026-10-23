#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_execution.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>

namespace mf {
namespace {
constexpr qint64 textLimit = 128 * 1024, projectLimit = 8 * 1024 * 1024;
const QStringList states{"prepared", "starting", "running",   "stopping",
                         "finished", "failed",   "cancelled", "timed_out"};
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
        fail("通常のファイルを指定してください: " + name);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail(name + ": " + f.errorString());
    if (!tail && f.size() > limit)
        fail(name + ": 読込上限を超えています。");
    const bool truncated = tail && f.size() > limit;
    if (truncated && !f.seek(f.size() - limit))
        fail(name + ": 読込位置を設定できません。");
    auto data = f.read(limit + (tail ? 0 : 1));
    if (f.error() != QFile::NoError)
        fail(name + ": " + f.errorString());
    if (!tail && data.size() > limit)
        fail(name + ": 読込上限を超えています。");
    if (truncated)
        data = "…末尾128 KiBを表示しています。\n" + data.mid(data.indexOf('\n') + 1);
    return data;
}
QString digest(const QByteArray &b) {
    return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex());
}
QJsonObject manifest(const QString &directory) {
    auto m = parseJson(bytes(directory, "run.json", 256 * 1024));
    if (m["format"] != "molforma-nwchem-run" || m["version"].toDouble() != 1)
        fail("未対応の実行記録です（MolForma NWChem version 1が必要です）。");
    if (!states.contains(m["status"].toString()))
        fail("実行状態が不明です。");
    if (m["status"] == "finished" && (!m["exit_code"].isDouble() || m["exit_code"].toDouble() != 0))
        fail("終了状態と終了コードが一致しません。");
    for (auto pair : {qMakePair("input", "input.nw"), qMakePair("project", "project.kgui.json"),
                      qMakePair("stdout", "output.out"), qMakePair("stderr", "stderr.err")})
        if (m[pair.first] != pair.second)
            fail("実行記録のファイル名が未対応です。");
    return m;
}
bool pending(const QString &s) {
    return s == "starting" || s == "running" || s == "stopping";
}
void checkDigest(const QByteArray &data, const QJsonObject &m, const QString &key, const QString &name) {
    if (!m.contains(key) && key == "project_sha256")
        return; // Compatibility with original v1 bundles.
    if (m[key].toString() != digest(data))
        fail(name + "が保存時のSHA-256と一致しません。");
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
QString historyStatus(const QString &s, bool owned) {
    if (pending(s) && !owned)
        return "稼働未確認（記録: " + s + "）";
    const QMap<QString, QString> names{
        {"prepared", "入力保存済み"}, {"starting", "起動中"},          {"running", "実行中"},
        {"stopping", "停止処理中"},   {"finished", "終了（コード0）"}, {"failed", "実行失敗"},
        {"cancelled", "停止済み"},    {"timed_out", "時間制限"},       {"invalid", "記録を読めません"}};
    return names.value(s, "不明");
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
            e.warnings << "作成日時を読めません。";
        double seconds = e.manifest["elapsed_seconds"].toDouble(-1);
        if (std::isfinite(seconds) && seconds >= 0 && seconds < 1e12)
            e.seconds = qint64(seconds);
        e.valid = true;
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
            e.method += " / " + calculation["functional"].toString().toUpper();
        e.basis = calculation["basis"].toString();
        e.task = historyChoice("task", calculation["task"].toString());
    } catch (const std::exception &error) {
        e.warnings << QString::fromUtf8(error.what());
    }
    if (QFileInfo::exists(QDir(e.directory).filePath("output.out"))) {
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
QString historyText(const QString &directory, const QString &name, bool tail) {
    if (!QStringList{"input.nw", "output.out", "stderr.err", "run.json"}.contains(name))
        fail("履歴で表示できないファイルです。");
    return QString::fromUtf8(bytes(directory, name, tail ? textLimit : projectLimit, tail));
}
QString historyIntegrity(const HistoryEntry &e) {
    try {
        const auto m = manifest(e.directory);
        checkDigest(bytes(e.directory, "input.nw", projectLimit), m, "input_sha256", "入力");
        if (!m.contains("project_sha256"))
            return "入力SHA-256一致。旧記録のためプロジェクトの照合情報なし。";
        checkDigest(bytes(e.directory, "project.kgui.json", projectLimit), m, "project_sha256",
                    "プロジェクト");
        return "入力・プロジェクトのSHA-256一致。";
    } catch (const std::exception &error) {
        return QString::fromUtf8(error.what());
    }
}
Document historyDocument(const QString &directory) {
    const auto m = manifest(directory);
    const auto input = bytes(directory, "input.nw", projectLimit);
    const auto project = bytes(directory, "project.kgui.json", projectLimit);
    checkDigest(input, m, "input_sha256", "入力");
    checkDigest(project, m, "project_sha256", "プロジェクト");
    auto d = Document::fromProject(parseJson(project));
    const auto reference = m["reference_directory"].toString();
    if (!QDir::isAbsolutePath(reference))
        fail("再利用に必要な参照ファイルの基準が不明です。");
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
                    fail("旧記録の相対実行ファイルパスを復元できません。");
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
        fail("このジョブフォルダーはごみ箱へ移動できません。");
    const auto m = manifest(directory);
    if (pending(m["status"].toString()))
        fail("稼働未確認・実行中のジョブは移動できません。");
    if (!QFile::moveToTrash(directory))
        fail("ごみ箱へ移動できません。保存先を確認してください。");
}
QString historyCsv(const QVector<HistoryEntry> &entries) {
    QString text = "作成日時UTC,計算名,手法,基底,目的,記録状態,経過秒,最終検出エネルギーHartree,"
                   "エネルギー手法,フォルダー\r\n";
    for (const auto &e : entries) {
        QStringList values{e.created.toUTC().toString(Qt::ISODateWithMs),
                           e.name,
                           e.method,
                           e.basis,
                           e.task,
                           e.status,
                           e.seconds < 0 ? "" : QString::number(e.seconds),
                           e.energy ? QString::number(*e.energy, 'g', 16) : "",
                           e.energyMethod,
                           e.directory};
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
        message = "ジョブ保存先がないか、読めません: " + root;
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
        message = "走査上限（2000ジョブ / 20000フォルダー）に達しました。保存先を分割してください。";
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
