// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/execution_settings.h"
#include "foundation/error.h"
#include "foundation/i18n.h"
#include "nwchem/execution_detail.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_restart.h"
#include "nwchem/remote_host.h"
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <cmath>

namespace mf {
using namespace execution_detail;
QString absolutePath(QString path, const QString &base) {
    if (path == "~")
        path = QDir::homePath();
    else if (path.startsWith("~/"))
        path = QDir::home().filePath(path.mid(2));
    return QDir::cleanPath(QDir(base).absoluteFilePath(path));
}
bool networkPath(const QString &path) {
    const auto p = path.trimmed();
    return p.startsWith("//") || p.startsWith("\\\\");
}
QString untrustedNetworkPath(const Document &d, const ExecutionEnvironment &environment) {
    const auto s = executionSettings(d);
    const auto &preferred = environment.preferences;
    for (const auto *key : {"executable", "launcher", "job_root", "reference_directory"})
        if (const auto value = s[key].toString(); networkPath(value) && value != preferred[key].toString())
            return value;
    for (const auto &row : s["environment"].toArray())
        if (const auto value = row.toObject()["value"].toString();
            networkPath(value) && !preferred["environment"].toArray().contains(row))
            return value;
    const auto advanced = nwchemAdvanced(d);
    for (const auto *key : {"vectors_input", "vectors_output", "scratch_dir", "permanent_dir"})
        if (const auto value = advanced[key].toString(); networkPath(value))
            return value;
    if (const auto job = nwchemRestart(nwchemSettings(d)).job;
        networkPath(job) && !networkPath(preferred["job_root"].toString()))
        return job;
    return {};
}
namespace execution_detail {
const QStringList managedEnvironment{"OMP_NUM_THREADS",      "OMP_STACKSIZE",    "OMP_DYNAMIC",
                                     "OPENBLAS_NUM_THREADS", "GOTO_NUM_THREADS", "MKL_NUM_THREADS",
                                     "MKL_DYNAMIC",          "BLIS_NUM_THREADS", "VECLIB_MAXIMUM_THREADS"};
bool writableDirectory(const QString &path, bool mayCreate) {
    QFileInfo info(path);
    if (mayCreate)
        while (!info.exists() && info.absoluteFilePath() != info.absolutePath())
            info.setFile(info.absolutePath());
    return info.isDir() && info.isWritable();
}
QString executionRoot(const QJsonObject &s, const QString &base) {
    return absolutePath(s["job_root"].toString(), base);
}
} // namespace execution_detail
namespace {
QString locateProgram(const QString &name, const QString &base, const QProcessEnvironment &environment) {
    if (QDir::isAbsolutePath(name) || name.contains('/') || name.contains(QDir::separator()) ||
        name.startsWith("~"))
        return absolutePath(name, base);
    const auto paths = environment.value("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
    return paths.isEmpty() ? QString() : QStandardPaths::findExecutable(name, paths);
}
} // namespace
QJsonObject executionDefaults() {
    return {{"executable", "nwchem"},
            {"mode", "mpi"},
            {"launcher", "mpirun"},
            {"mpi_ranks", 2},
            {"omp_threads", 1},
            {"blas_threads", 1},
            {"omp_stacksize", ""},
            {"job_root", QDir::home().filePath("MolForma/jobs")},
            {"reference_directory", ""},
            {"timeout_seconds", 0},
            {"environment", QJsonArray{}},
            {"job_script", "none"},
            {"job_queue", ""},
            {"remote", ""}};
}
const QVector<ExecutionRange> &executionRanges() {
    static const QVector<ExecutionRange> ranges{{"mpi_ranks", 1, 4096},
                                                {"omp_threads", 1, 1024},
                                                {"blas_threads", 1, 1024},
                                                {"timeout_seconds", 0, 604800}};
    return ranges;
}
ExecutionRange executionRange(const QString &key) {
    for (const auto &range : executionRanges())
        if (range.key == key)
            return range;
    fail("Unknown execution setting: " + key);
}
QStringList executionPreferenceKeys() {
    return {"executable",   "mode",          "launcher", "mpi_ranks",       "omp_threads",
            "blas_threads", "omp_stacksize", "job_root", "timeout_seconds", "environment",
            "job_script",   "job_queue",     "remote"};
}
QJsonObject executionSettings(const Document &d) {
    auto result = executionDefaults();
    const auto stored = d.data["nwchem_execution"].toObject();
    for (auto it = stored.begin(); it != stored.end(); ++it)
        result[it.key()] = it.value();
    return result;
}
void setExecutionSetting(Document &d, const QString &key, const QJsonValue &value) {
    auto s = d.data.value("nwchem_execution").toObject();
    if (value.isUndefined())
        s.remove(key);
    else
        s[key] = value;
    if (!s.isEmpty() || d.data.contains("nwchem_execution"))
        d.data["nwchem_execution"] = s;
}
ExecutionValidation validateExecution(const Document &d, const QString &base, bool local,
                                      const ExecutionEnvironment &environment) {
    ExecutionValidation r;
    const auto s = executionSettings(d);
    const auto calculation = validateNwchem(d);
    static_cast<ValidationReport &>(r) = calculation;
    if (d.data.contains("nwchem_execution") && !d.data["nwchem_execution"].isObject())
        r.error("execution.object", "nwchem_execution",
                tr("NWChem実行設定はオブジェクトである必要があります。"), {}, SettingTarget::Execution);
    const auto defaults = executionDefaults();
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const auto v = s[it.key()];
        if (v.type() != it.value().type())
            r.error("execution.type", it.key(), tr("実行設定の型が不正です: %1").arg(it.key()), {},
                    SettingTarget::Execution);
        else if (v.isString() && (controls(v.toString()) || v.toString().size() > 4096))
            r.error("execution.string", it.key(),
                    tr("実行設定に制御文字または長すぎる文字列があります: %1").arg(it.key()), {},
                    SettingTarget::Execution);
    }
    auto integer = [&](const QString &key, int lo, int hi) {
        const double n = s[key].toDouble(-1);
        if (!s[key].isDouble() || !std::isfinite(n) || std::floor(n) != n || n < lo || n > hi)
            r.error("execution.integer", key,
                    tr("%1 は %2〜%3 の整数で指定してください。").arg(key).arg(lo).arg(hi), {},
                    SettingTarget::Execution);
    };
    const bool mpi = s["mode"] == "mpi";
    if (!mpi && s["mode"] != "direct")
        r.error("execution.mode", "mode", tr("起動方式はMPIまたは直接実行を選んでください。"), {},
                SettingTarget::Execution);
    for (const auto &range : executionRanges())
        integer(range.key, range.minimum, range.maximum);
    for (const auto &key : {"executable", "job_root"})
        if (s[key].toString().trimmed().isEmpty())
            r.error("execution.required", key, tr("%1 を指定してください。").arg(key), {},
                    SettingTarget::Execution);
    if (mpi && s["launcher"].toString().trimmed().isEmpty())
        r.error("execution.launcher", "launcher", tr("MPIランチャーを指定してください。"), {},
                SettingTarget::Execution);
    if (!QStringList{"none", "shell", "slurm", "pbs"}.contains(s["job_script"].toString()))
        r.error("execution.job_script", "job_script",
                tr("ジョブスクリプトは、作らない・シェル・Slurm・PBSから選んでください。"), {},
                SettingTarget::Execution);
    if (!QRegularExpression("^[A-Za-z0-9_.@:+-]{0,64}$").match(s["job_queue"].toString()).hasMatch())
        r.error("execution.job_queue", "job_queue",
                tr("キュー・パーティション名は英数字と _ . @ : + - の64文字以内で指定してください。"), {},
                SettingTarget::Execution);
    const auto stack = s["omp_stacksize"].toString();
    if (!stack.isEmpty() && !QRegularExpression("^[1-9][0-9]{0,8}[BKMGbkmg]?$").match(stack).hasMatch())
        r.error("execution.stack", "omp_stacksize",
                tr("OpenMPスタックは64M・1G等の正の整数と単位B/K/M/Gで指定してください。"), {},
                SettingTarget::Execution);
    r.environment = {{"OMP_NUM_THREADS", QString::number(s["omp_threads"].toInt())},
                     {"OMP_DYNAMIC", "FALSE"},
                     {"MKL_DYNAMIC", "FALSE"}};
    for (const auto &key : {"OPENBLAS_NUM_THREADS", "GOTO_NUM_THREADS", "MKL_NUM_THREADS", "BLIS_NUM_THREADS",
                            "VECLIB_MAXIMUM_THREADS"})
        r.environment[key] = QString::number(s["blas_threads"].toInt());
    if (!stack.isEmpty())
        r.environment["OMP_STACKSIZE"] = stack.toUpper();
    QSet<QString> seen;
    for (const auto &entry : s["environment"].toArray()) {
        const auto row = entry.toObject();
        if (!entry.isObject() || !row["enabled"].isBool()) {
            r.error("execution.environment_enabled", "environment",
                    tr("環境変数の行に有効・無効の指定がありません。"), {}, SettingTarget::Execution);
            continue;
        }
        if (!row["enabled"].toBool())
            continue;
        const auto name = row["name"].toString(), value = row["value"].toString();
        if (!row["name"].isString() ||
            !QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$").match(name).hasMatch() ||
            !row["value"].isString() || controls(value) || value.size() > 8192) {
            r.error("execution.environment_value", "environment",
                    tr("有効な環境変数の名前・値を確認してください。"), {}, SettingTarget::Execution);
            continue;
        }
        if (seen.contains(name) || managedEnvironment.contains(name)) {
            r.error("execution.environment_duplicate", "environment",
                    tr("環境変数が重複、または専用の資源設定と重複しています: %1").arg(name), {},
                    SettingTarget::Execution);
            continue;
        }
        seen.insert(name);
        r.environment[name] = value;
    }
    if (r.environment.contains("NWCHEM_BASIS_LIBRARY") && !r.environment["NWCHEM_BASIS_LIBRARY"].isEmpty())
        r.environment["NWCHEM_BASIS_LIBRARY"] =
            absolutePath(r.environment["NWCHEM_BASIS_LIBRARY"], base) + '/';
    auto variables = QProcessEnvironment::systemEnvironment();
    for (auto it = r.environment.begin(); it != r.environment.end(); ++it)
        variables.insert(it.key(), it.value());
    r.referenceDirectory = s["reference_directory"].toString().isEmpty()
                               ? absolutePath(".", base)
                               : absolutePath(s["reference_directory"].toString(), base);
    // A remote run starts NWChem on its destination: the programs are named for that computer.
    const auto remoteName = s["remote"].toString();
    const auto remote = environment.remote(remoteName);
    if (!remoteName.isEmpty()) {
        if (!remote)
            r.error("execution.remote", "remote",
                    tr("実行先「%1」はこのPCの環境設定に登録されていません。「3 "
                       "実行設定」で実行先を選び直してくだ"
                       "さい。")
                        .arg(remoteName),
                    {}, SettingTarget::Execution);
        else
            r.remote = remoteHostSummary(*remote);
        if (nwchemAdvanced(d)["vectors_guess"].toString() == "file")
            r.error("execution.remote_vectors", "remote",
                    tr("リモート実行では、ファイルから読む初期軌道（入力movecs）を使えません。前のジョブから"
                       "続けると"
                       "きは「履歴」の「続きから実行を準備」を使ってください。"),
                    {}, SettingTarget::Execution);
    } else {
        r.executable = locateProgram(s["executable"].toString(), base, variables);
        r.launcher = locateProgram(s["launcher"].toString(), base, variables);
    }
    if (const auto network = untrustedNetworkPath(d, environment); !network.isEmpty()) {
        r.error(
            "execution.network", "job_root",
            tr("ネットワーク上の場所 %1 は、「編集 → 環境設定」で指定した場合だけ使えます。").arg(network),
            {}, SettingTarget::Execution);
        local = false; // Not even looked at.
    }
    if (local) {
        const auto check = [&](const QString &key, const QString &label, bool ok, const QString &found,
                               const QString &error) {
            r.checks << ExecutionCheck{key, label, ok ? found : error, ok ? QString() : error, ok};
            if (!ok)
                r.error("execution.local", key, error, {}, SettingTarget::Execution);
        };
        const auto program = [&](const QString &key, const QString &path, const QString &label,
                                 const QString &error) {
            const QFileInfo f(path);
            check(key, label, !path.isEmpty() && f.isFile() && f.isExecutable(), path, error);
        };
        if (!remoteName.isEmpty()) {
            check("remote", tr("リモート実行先"), remote.has_value(), r.remote,
                  tr("実行先「%1」が環境設定にありません。").arg(remoteName));
            const auto ssh = remoteProgram("ssh"), scp = remoteProgram("scp");
            check("remote", tr("ssh・scp"), !ssh.isEmpty() && !scp.isEmpty(), ssh + ", " + scp,
                  tr("このPCに ssh と scp が見つかりません（OpenSSHのクライアントが必要です）。"));
        } else {
            program("executable", r.executable, tr("NWChem実行ファイル"),
                    tr("NWChem実行ファイルが見つからないか、実行権限がありません。"));
            if (mpi)
                program("launcher", r.launcher, tr("MPIランチャー"),
                        tr("MPIランチャーが見つからないか、実行権限がありません。"));
        }
        check("reference_directory", tr("参照ファイルの基準"), QFileInfo(r.referenceDirectory).isDir(),
              r.referenceDirectory, tr("参照ファイルの基準ディレクトリがありません。"));
        const auto root = executionRoot(s, base);
        check("job_root", tr("ジョブ保存先"), writableDirectory(root, true), root,
              tr("ジョブ保存先にディレクトリを作成できません。"));
        if (nwchemRestart(nwchemSettings(d)).active()) {
            const auto database = restartSource(d);
            const QFileInfo info(database);
            check("restart", tr("再開元のデータベース"),
                  !database.isEmpty() && info.isFile() && !info.isSymLink() && info.isReadable(), database,
                  tr("再開元のジョブフォルダーにNWChemのデータベース（計算名.db）がありません。計算名とper"
                     "manent_dirを再開元のままにするか、「2 計算条件」で再開を解除してください。"));
        }
        const auto basis = r.environment.value("NWCHEM_BASIS_LIBRARY");
        if (!basis.isEmpty() && remoteName.isEmpty())
            check("environment", "NWCHEM_BASIS_LIBRARY", QFileInfo(basis).isDir(), basis,
                  tr("NWCHEM_BASIS_LIBRARYのディレクトリがありません。"));
        // The input's own errors stay in the list; the check only counts them.
        r.checks << ExecutionCheck{{},
                                   tr("NWChem入力"),
                                   calculation.errors.isEmpty() ? tr("生成できます")
                                                                : tr("生成できません（エラー %n 件）",
                                                                     nullptr, int(calculation.errors.size())),
                                   {},
                                   calculation.errors.isEmpty()};
    }
    const int ranks = mpi ? s["mpi_ranks"].toInt() : 1;
    if (mpi && ranks == 1)
        r.warning("execution.mpi_pr", "mpi_ranks",
                  tr("MPI-PR版NWChemは補助プロセスを必要とします。1計算プロセスでもMPI起動数2が必要です。"),
                  {}, SettingTarget::Execution);
    if (!mpi)
        r.warning("execution.direct", "mode",
                  tr("直接実行は対応する逐次版等に使用してください。MPI版はMPIランチャーを選んでください。"),
                  {}, SettingTarget::Execution);
    if (remoteName.isEmpty() &&
        ranks * std::max(s["omp_threads"].toInt(), s["blas_threads"].toInt()) > QThread::idealThreadCount())
        r.warning("execution.cpus", "mpi_ranks", tr("指定した並列度が、このPCの論理CPU数を超えています。"),
                  {}, SettingTarget::Execution);
    if (s["omp_threads"].toInt() > 1 && s["blas_threads"].toInt() > 1)
        r.warning("execution.threads", "omp_threads",
                  tr("OpenMPとBLASの多重並列により、想定より多くのスレッドを使う場合があります。"), {},
                  SettingTarget::Execution);
    for (auto it = s.begin(); it != s.end(); ++it)
        if (!defaults.contains(it.key()))
            r.warning("execution.unknown", it.key(), tr("未対応の実行設定を保持しています: %1").arg(it.key()),
                      {}, SettingTarget::Execution);
    r.errors.removeDuplicates();
    return r;
}
} // namespace mf
