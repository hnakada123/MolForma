// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/remote_host.h"
#include <QByteArray>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

// Internal to src/nwchem: how the supervisor of a remote run and the probe of a destination talk to
// that computer through the ssh and scp of this one.
namespace mf::execution_detail {
// The folder of the jobs as the shell of that computer reads it: "~" is its home.
QString remoteRoot(const QString &directory);
// The markers around what a script prints (scriptStart, scriptEnd) and what stands between them
// (answer): anything a login profile printed before is no part of it.
QString newNonce();
std::optional<QByteArray> answer(const QByteArray &out, const QString &nonce);
QString scriptStart(const QString &nonce);
QString scriptEnd(const QString &nonce);
struct Reply {
    int code = -1;
    QByteArray out, err;
    QString failure; // The program did not start, took too long or was cancelled.
    bool cancelled = false;
    // What ssh or the other side said: its last lines.
    QString detail() const {
        if (!failure.isEmpty())
            return failure;
        auto lines = QString::fromLocal8Bit(err).split('\n', Qt::SkipEmptyParts);
        for (auto &line : lines)
            line = line.trimmed();
        lines.removeAll(QString());
        if (lines.size() > 3)
            lines = lines.mid(lines.size() - 3);
        const auto text = lines.join(" / ");
        return text.isEmpty() ? QString("exit code %1").arg(code) : text.left(600);
    }
};
// Runs a program with its input and waits for its end, a time limit (milliseconds; 0: none) or
// cancelled().
Reply execute(const QString &program, const QStringList &arguments, const QByteArray &input, qint64 timeout,
              const std::function<bool()> &cancelled);
QStringList sshOptions();
// The arguments of ssh for a script that comes on its standard input.
QStringList sshArguments(const RemoteHost &host);
// The connection to a destination, through the ssh and scp of this computer.
struct Connection {
    RemoteHost host;
    QString ssh = remoteProgram("ssh"), scp = remoteProgram("scp");
    Reply run(const QString &script, qint64 timeout = 180000) const {
        return execute(ssh, sshArguments(host), script.toUtf8(), timeout, {});
    }
    Reply copy(const QStringList &sources, const QString &target,
               const std::function<bool()> &cancelled) const {
        auto arguments = sshOptions();
        // Without -q, which would hide why ssh failed; scp shows no progress without a terminal.
        arguments << "-r" << "-p";
        if (host.port)
            arguments << "-P" << QString::number(host.port);
        return execute(scp, arguments << "--" << sources << target, {}, 0, cancelled);
    }
    QString remote(const QString &path) const {
        return host.host + ':' + path;
    }
};
// An absolute path there that may stand in a command as it is.
const QRegularExpression &remotePath();
} // namespace mf::execution_detail
