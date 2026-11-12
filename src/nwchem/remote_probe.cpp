// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/remote_probe.h"
#include "foundation/i18n.h"
#include "nwchem/execution_detail.h"
#include "nwchem/remote_shell.h"
#include <QTimer>

namespace mf {
using namespace execution_detail;
RemoteProbe::RemoteProbe(QObject *parent) : QObject(parent) {
    connect(&process, &QProcess::finished, this, &RemoteProbe::done);
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && !cancelling)
            emit finished(false, tr("ssh を起動できません: %1").arg(process.errorString()));
    });
}
RemoteProbe::~RemoteProbe() {
    cancel();
}
void RemoteProbe::start(const RemoteHost &host, const QString &prelude, const QString &program,
                        const QString &mpi) {
    cancel();
    probed = host;
    executable = host.executable.isEmpty() ? program : host.executable;
    launcher = host.launcher.isEmpty() ? mpi : host.launcher;
    const auto ssh = remoteProgram("ssh");
    if (ssh.isEmpty()) {
        QTimer::singleShot(0, this, [this] { emit finished(false, tr("このPCに ssh が見つかりません。")); });
        return;
    }
    nonce = newNonce();
    QString script = scriptStart(nonce);
    // As the job script prepares NWChem: the lines of Preferences, then those of the destination.
    if (!prelude.isEmpty())
        script += prelude + '\n';
    if (!host.prelude.trimmed().isEmpty())
        script += host.prelude + '\n';
    script += "printf 'host %s\\n' \"$(uname -n)\"\n"
              "for c in sbatch squeue scancel sacct qsub qstat qdel setsid; do\n"
              "  if command -v \"$c\" >/dev/null 2>&1; then printf 'has %s\\n' \"$c\"; fi\n"
              "done\n";
    script += "printf 'nwchem %s\\n' \"$(command -v " + quote(executable) + " 2>/dev/null)\"\n";
    if (!launcher.isEmpty())
        script += "printf 'launcher %s\\n' \"$(command -v " + quote(launcher) + " 2>/dev/null)\"\n";
    script +=
        "root=" + remoteRoot(host.directory) +
        "\nif mkdir -p -- \"$root\" 2>/dev/null && cd -- \"$root\" 2>/dev/null; then printf 'root %s\\n' "
        "\"$(pwd -P)\"; fi\n" +
        scriptEnd(nonce);
    process.setProgram(ssh);
    process.setArguments(sshArguments(host));
    process.start();
    process.write(script.toUtf8());
    process.closeWriteChannel();
    // A connection that hangs ends after a while (a later probe has a time of its own).
    QTimer::singleShot(90000, this, [this, probe = ++probes] {
        if (probe == probes && active())
            process.kill();
    });
}
void RemoteProbe::cancel() {
    if (!active())
        return;
    cancelling = true;
    process.kill();
    process.waitForFinished(3000);
    cancelling = false;
}
void RemoteProbe::done() {
    if (cancelling)
        return;
    const auto out = process.readAllStandardOutput();
    const auto err = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    const auto text = answer(out, nonce);
    if (!text) {
        QStringList report{tr("接続できません。")};
        const auto lines = err.split('\n', Qt::SkipEmptyParts);
        if (!lines.isEmpty())
            report << lines.mid(std::max<qsizetype>(0, lines.size() - 3)).join('\n');
        if (err.contains("Host key verification failed") || err.contains("No ED25519 host key") ||
            err.contains("host key"))
            report << tr("ホスト鍵が登録されていません。端末で一度「ssh "
                         "%1」を実行し、表示された鍵を確かめて登録してください。")
                          .arg(probed.host);
        else if (err.contains("Permission denied"))
            report << tr("認証できません。鍵を ssh-agent に登録するか、~/.ssh/config "
                         "で鍵を指定してください（パスワード"
                         "の入力には対応していません）。");
        emit finished(false, report.join('\n'));
        return;
    }
    QString host, root, nwchem, mpi;
    QStringList has;
    for (const auto &raw : QString::fromUtf8(*text).split('\n', Qt::SkipEmptyParts)) {
        const auto word = raw.section(' ', 0, 0), rest = raw.section(' ', 1).trimmed();
        if (word == "host")
            host = rest;
        else if (word == "root")
            root = rest;
        else if (word == "has")
            has << rest;
        else if (word == "nwchem")
            nwchem = rest;
        else if (word == "launcher")
            mpi = rest;
    }
    bool ok = true;
    QStringList report{tr("接続できました（%1）。").arg(host.isEmpty() ? probed.host : host)};
    if (root.isEmpty()) {
        ok = false;
        report << tr("作業フォルダー %1 を作れません。").arg(probed.directory);
    } else if (!remotePath().match(root).hasMatch()) {
        ok = false;
        report << tr("作業フォルダーのパスに使えない文字があります: %1").arg(root);
    } else
        report << tr("作業フォルダー: %1").arg(root);
    const QStringList needed = probed.scheduler == "slurm" ? QStringList{"sbatch", "squeue", "scancel"}
                               : probed.scheduler == "pbs" ? QStringList{"qsub", "qstat", "qdel"}
                                                           : QStringList{};
    QStringList missing;
    for (const auto &command : needed)
        if (!has.contains(command))
            missing << command;
    if (!missing.isEmpty()) {
        ok = false;
        report << tr("見つからないコマンド: %1").arg(missing.join(", "));
    } else if (!needed.isEmpty())
        report << tr("スケジューラー: %1").arg(needed.join(", "));
    if (probed.scheduler == "shell" && !has.contains("setsid"))
        report << tr("setsid がありません。停止はジョブのスクリプトとその子プロセスにだけ送ります。");
    report << (nwchem.isEmpty()
                   ? tr("確認: NWChem「%1」がログインしたときのPATHにありません（計算ノードでだけ使える場合"
                        "は問題ありません）。")
                         .arg(executable)
                   : tr("NWChem: %1").arg(nwchem));
    if (!launcher.isEmpty())
        report << (mpi.isEmpty()
                       ? tr("確認: MPIランチャー「%1」がログインしたときのPATHにありません。").arg(launcher)
                       : tr("MPIランチャー: %1").arg(mpi));
    emit finished(ok, report.join('\n'));
}
} // namespace mf
