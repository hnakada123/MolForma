// SPDX-License-Identifier: GPL-2.0-or-later
// A stand-in for ssh and scp (by the name it is started under) for the tests of remote runs: the
// "remote computer" is this one, with the folder MOLFORMA_FAKE_REMOTE_HOME as its home. No network,
// keys or sshd. Unix only (the commands run in /bin/sh and bash).
//  ssh [options] [--] host command…  runs the command in that home; "bash -l -s" (a login shell reading
//                                   its script from stdin) runs without the profiles of this computer,
//                                   after a banner on stdout, as a login profile might print.
//  scp [options] [--] source… target copies with cp -R -p; host:path names a path of that home.
// MOLFORMA_FAKE_SSH_DOWN: while that file exists, every connection is refused (exit code 255).
// MOLFORMA_FAKE_SSH_LOG: each call is appended to that file, one line of its arguments.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>
#include <vector>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const bool scp = QFileInfo(app.arguments().first()).baseName() == "scp";
    const auto home = qEnvironmentVariable("MOLFORMA_FAKE_REMOTE_HOME");
    if (home.isEmpty() || !QDir(home).exists()) {
        std::fprintf(stderr, "fake ssh: MOLFORMA_FAKE_REMOTE_HOME is not set\n");
        return 255;
    }
    if (const auto log = qEnvironmentVariable("MOLFORMA_FAKE_SSH_LOG"); !log.isEmpty()) {
        QFile file(log);
        if (file.open(QIODevice::Append))
            file.write((QStringList(scp ? "scp" : "ssh") + app.arguments().mid(1)).join(' ').toUtf8() + '\n');
    }
    if (const auto down = qEnvironmentVariable("MOLFORMA_FAKE_SSH_DOWN");
        !down.isEmpty() && QFileInfo::exists(down)) {
        std::fprintf(stderr, "ssh: connect to host fake port 22: Connection refused\n");
        return 255;
    }
    // Options and their values, then the host (ssh) or the paths (scp).
    const QStringList valued{"-o", "-p", "-P", "-i", "-l", "-F", "-J", "-c", "-S"};
    QStringList rest;
    const auto args = app.arguments().mid(1);
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--") {
            rest << args.mid(i + 1);
            break;
        }
        if (valued.contains(args[i]))
            ++i;
        else if (!args[i].startsWith('-'))
            rest << args[i];
    }
#ifdef Q_OS_UNIX
    if (!scp) {
        if (rest.size() < 2) {
            std::fprintf(stderr, "usage: ssh host command\n");
            return 255;
        }
        const auto command = rest.mid(1).join(' ');
        if (::chdir(QFile::encodeName(home).constData()) != 0)
            return 255;
        qputenv("HOME", home.toUtf8());
        if (command == "bash -l -s") {
            std::printf("Welcome to the fake cluster.\n");
            std::fflush(stdout);
            ::execl("/bin/bash", "bash", "--noprofile", "--norc", "-s", static_cast<char *>(nullptr));
        } else
            ::execl("/bin/sh", "sh", "-c", command.toLocal8Bit().constData(), static_cast<char *>(nullptr));
        return 255;
    }
    if (rest.size() < 2)
        return 1;
    // host:path is a path of the fake home; anything else is a path of this computer.
    QList<QByteArray> paths;
    for (const auto &path : rest) {
        const auto colon = path.indexOf(':');
        QString local = path;
        if (colon > 0 && !path.left(colon).contains('/')) {
            local = path.mid(colon + 1);
            if (local == "~" || local.isEmpty())
                local = home;
            else if (local.startsWith("~/"))
                local = home + local.mid(1);
            else if (!local.startsWith('/'))
                local = home + '/' + local;
        }
        paths << QFile::encodeName(local);
    }
    std::vector<char *> cp{const_cast<char *>("cp"), const_cast<char *>("-R"), const_cast<char *>("-p")};
    for (auto &path : paths)
        cp.push_back(path.data());
    cp.push_back(nullptr);
    ::execvp("cp", cp.data());
    return 1;
#else
    return 255;
#endif
}
