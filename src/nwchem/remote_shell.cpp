// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/remote_shell.h"
#include "nwchem/execution_detail.h"
#include <QElapsedTimer>
#include <QProcess>
#include <QRandomGenerator>

namespace mf::execution_detail {
// The folder of the jobs as the shell of that computer reads it: "~" is its home.
QString remoteRoot(const QString &directory) {
    if (directory == "~")
        return "\"$HOME\"";
    if (directory.startsWith("~/"))
        return "\"$HOME\"/" + quote(directory.mid(2));
    return quote(directory);
}
QString newNonce() {
    return QString("MOLFORMA-%1").arg(QRandomGenerator::global()->generate64(), 16, 16, QChar('0'));
}
// What a script printed between its markers: anything a login profile printed before is no part of it.
std::optional<QByteArray> answer(const QByteArray &out, const QString &nonce) {
    const auto begin = (nonce + "-BEGIN\n").toLatin1(), end = ("\n" + nonce + "-END").toLatin1();
    const auto from = out.indexOf(begin);
    const auto to = out.lastIndexOf(end);
    if (from < 0 || to < from + begin.size() - 1)
        return std::nullopt;
    return out.mid(from + begin.size(), to - from - begin.size());
}
QString scriptStart(const QString &nonce) {
    return "printf '%s\\n' " + quote(nonce + "-BEGIN") + "\n";
}
QString scriptEnd(const QString &nonce) {
    return "printf '\\n%s\\n' " + quote(nonce + "-END") + "\n";
}
Reply execute(const QString &program, const QStringList &arguments, const QByteArray &input, qint64 timeout,
              const std::function<bool()> &cancelled) {
    Reply r;
    QProcess p;
    p.setProgram(program);
    p.setArguments(arguments);
    p.start();
    if (!p.waitForStarted(15000)) {
        r.failure = p.errorString();
        return r;
    }
    p.write(input);
    p.closeWriteChannel();
    QElapsedTimer clock;
    clock.start();
    while (p.state() != QProcess::NotRunning && !p.waitForFinished(250)) {
        r.out += p.readAllStandardOutput();
        r.err += p.readAllStandardError();
        const bool stop = cancelled && cancelled();
        if (stop || (timeout > 0 && clock.elapsed() > timeout)) {
            p.kill();
            p.waitForFinished(5000);
            r.cancelled = stop;
            r.failure = stop ? QString("cancelled") : QString("no answer in %1 s").arg(timeout / 1000);
            return r;
        }
    }
    r.out += p.readAllStandardOutput();
    r.err += p.readAllStandardError();
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    if (p.exitStatus() != QProcess::NormalExit)
        r.failure = p.errorString();
    return r;
}
QStringList sshOptions() {
    // Never a password prompt or a question about an unknown host key: what ssh cannot do by itself
    // fails, and the user sets it up once in a terminal (ssh-agent, known_hosts).
    return {"-o", "BatchMode=yes",          "-o", "ConnectTimeout=20",
            "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4"};
}
QStringList sshArguments(const RemoteHost &host) {
    auto arguments = sshOptions();
    if (host.port)
        arguments << "-p" << QString::number(host.port);
    // A login shell, which sets PATH and module as a terminal there would; the script comes on stdin, so
    // no shell of that computer reads it twice.
    return arguments << "--" << host.host << "bash -l -s";
}
const QRegularExpression &remotePath() {
    static const QRegularExpression pattern("^/[A-Za-z0-9._/+-]*$");
    return pattern;
}
} // namespace mf::execution_detail
