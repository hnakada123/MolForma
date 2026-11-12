// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/remote_host.h"
#include "foundation/i18n.h"
#include "nwchem/execution_detail.h"
#include "nwchem/job_script.h"
#include <QRegularExpression>
#include <QStandardPaths>

namespace mf {
using namespace execution_detail;
namespace {
const QStringList schedulers{"shell", "slurm", "pbs"};
} // namespace
QJsonObject RemoteHost::json() const {
    return {{"name", name},           {"host", host},           {"port", port},
            {"directory", directory}, {"scheduler", scheduler}, {"executable", executable},
            {"launcher", launcher},   {"header", header},       {"prelude", prelude},
            {"interval", interval}};
}
RemoteHost RemoteHost::fromJson(const QJsonObject &o) {
    RemoteHost h;
    h.name = o["name"].toString();
    h.host = o["host"].toString();
    h.port = o["port"].toInt();
    h.directory = o["directory"].toString(h.directory);
    h.scheduler = o["scheduler"].toString(h.scheduler);
    h.executable = o["executable"].toString();
    h.launcher = o["launcher"].toString();
    h.header = o["header"].toString();
    h.prelude = o["prelude"].toString();
    h.interval = o["interval"].toInt(h.interval);
    return h;
}
QStringList remoteSchedulers() {
    return schedulers;
}
QString remoteSchedulerName(const QString &scheduler) {
    if (scheduler == "slurm")
        return tr("Slurm（sbatch）");
    if (scheduler == "pbs")
        return tr("PBS（qsub）");
    return tr("シェル（バックグラウンド）");
}
QStringList remoteHostErrors(const RemoteHost &h) {
    QStringList errors;
    const auto name = h.name.trimmed();
    if (name.isEmpty() || name.size() > 64 || controls(h.name, false) || name != h.name)
        errors << tr("実行先の名前は、前後に空白のない64文字以内で指定してください。");
    if (!QRegularExpression("^[A-Za-z0-9_][A-Za-z0-9._@%+-]{0,254}$").match(h.host).hasMatch())
        errors << tr("接続先は「ユーザー名@ホスト名」か、~/.ssh/config "
                     "のホスト名で指定してください（英数字と . _ @ % + -）。");
    if (h.port < 0 || h.port > 65535)
        errors << tr("ポートは0（sshの設定どおり）か1〜65535で指定してください。");
    const auto parts = h.directory.split('/');
    if (h.directory.size() > 1024 ||
        !QRegularExpression("^(~|~/[A-Za-z0-9._/+-]*|/[A-Za-z0-9._/+-]*|[A-Za-z0-9._+][A-Za-z0-9._/+-]*)$")
             .match(h.directory)
             .hasMatch() ||
        parts.contains(".."))
        errors << tr(
            "実行先の作業フォルダーは、英数字と . _ / + - だけのパスで指定してください（~/ で始めるとホーム"
            "から、.. は使えません）。");
    if (!schedulers.contains(h.scheduler))
        errors << tr("投入方式は、シェル・Slurm・PBSから選んでください。");
    for (const auto &value : {h.executable, h.launcher})
        if (value.size() > 1024 || controls(value, false))
            errors << tr("実行先のNWChem・MPIランチャーに制御文字または長すぎる文字列があります。");
    if (!h.header.trimmed().isEmpty())
        errors << jobHeaderErrors(h.header);
    if (h.prelude.size() > 4000 || controls(h.prelude, true))
        errors << tr("準備の行は、制御文字を含めず4000文字以内で指定してください。");
    if (h.interval < 10 || h.interval > 3600)
        errors << tr("状態を確認する間隔は10〜3600秒で指定してください。");
    errors.removeDuplicates();
    return errors;
}
QString remoteHostSummary(const RemoteHost &h) {
    const auto where = h.port ? tr("%1（ポート %2）").arg(h.host).arg(h.port) : h.host;
    const auto kind = h.scheduler == "slurm" ? QString("Slurm")
                      : h.scheduler == "pbs" ? QString("PBS")
                                             : tr("シェル");
    return tr("%1:%2（%3）").arg(where, h.directory, kind);
}
QString remoteProgram(const QString &name) {
    return QStandardPaths::findExecutable(name);
}
} // namespace mf
