// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/remote_run.h"
#include "foundation/i18n.h"
#include "nwchem/remote_host.h"
#include <QDateTime>

namespace mf {
namespace {
QString stateName(const QString &state) {
    if (state == "queued")
        return tr("待機中", "remote");
    if (state == "running")
        return tr("実行中");
    if (state == "held")
        return tr("保留中", "remote");
    if (state == "ending")
        return tr("終了処理中", "remote");
    if (state == "ended")
        return tr("終了", "state");
    return tr("状態不明", "remote");
}
} // namespace
bool remoteRun(const QJsonObject &manifest) {
    return manifest["remote"].isObject() && !manifest["remote"].toObject().isEmpty();
}
QString remoteRunText(const QJsonObject &manifest) {
    if (!remoteRun(manifest))
        return {};
    const auto r = manifest["remote"].toObject();
    const auto host = RemoteHost::fromJson(r["host"].toObject());
    QStringList parts{tr("実行先 %1（%2）").arg(host.name, host.host)};
    const auto job = r["job"].toString();
    const auto phase = r["phase"].toString();
    if (phase.isEmpty() || phase == "prepared" || phase == "upload")
        parts << tr("ジョブを複写中");
    else if (phase == "submit")
        parts << tr("投入中");
    else {
        const auto what = host.scheduler == "shell" ? tr("プロセス %1").arg(job) : tr("ジョブ %1").arg(job);
        if (phase == "retrieve")
            parts << tr("%1 · 結果を回収中").arg(what);
        else if (phase == "done")
            parts << what;
        else {
            const auto raw = r["scheduler_state"].toString().section(' ', 0, 0);
            const auto state = stateName(r["state"].toString());
            parts << (raw.isEmpty() || host.scheduler != "slurm" ? tr("%1 · %2").arg(what, state)
                                                                 : tr("%1 · %2（%3）").arg(what, state, raw));
        }
    }
    if (const auto message = r["message"].toString(); !message.isEmpty()) {
        const auto since = QDateTime::fromString(r["failing_since"].toString(), Qt::ISODateWithMs);
        parts
            << (since.isValid()
                    ? tr("%1（%2から）").arg(translated(message), since.toLocalTime().toString("MM/dd HH:mm"))
                    : translated(message));
        if (const auto detail = r["detail"].toString(); !detail.isEmpty())
            parts << detail;
    } else if (const auto detail = r["detail"].toString(); !detail.isEmpty() && phase == "done")
        parts << detail;
    return parts.join(" · ");
}
} // namespace mf
