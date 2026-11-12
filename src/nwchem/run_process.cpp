// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/run_process.h"
#include "nwchem/execution_detail.h"
#include "nwchem/run_record.h"
#include <QFile>
#include <QHash>
#include <QSysInfo>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace mf {
namespace {
#ifdef Q_OS_WIN
// Defined last in this file, away from the macros of windows.h.
bool windowsProcessRunning(qint64 pid);
void windowsTerminateTree(qint64 pid);
#endif
QString bootId() {
    QFile f("/proc/sys/kernel/random/boot_id");
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.read(64)).trimmed() : QString();
}
// Start time of a process in clock ticks after boot (field 22 of /proc/<pid>/stat), or empty.
QString processStart(qint64 pid) {
    QFile f(QString("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const auto stat = QString::fromLatin1(f.read(4096));
    return stat.mid(stat.lastIndexOf(')') + 2).split(' ').value(19);
}
} // namespace
namespace execution_detail {
// The file that asks the supervisor of a job to stop it.
QString stopRequest(const QString &directory) {
    return jobFile(directory, JobFile::stopRequest);
}
// Ends the process group of a run (Unix), or the whole tree of its first process (Windows).
void signalGroup(qint64 group, bool force) {
    if (group <= 0)
        return;
#ifdef Q_OS_UNIX
    ::kill(-pid_t(group), force ? SIGKILL : SIGTERM);
#elif defined(Q_OS_WIN)
    // No process groups and no signal a console program would heed: the forced stop ends the whole
    // tree (cmd, wsl.exe, mpiexec and NWChem), not only the program MolForma started.
    if (force)
        windowsTerminateTree(group);
#else
    Q_UNUSED(force);
#endif
}
} // namespace execution_detail
using namespace execution_detail;
bool runSupervised(const QJsonObject &manifest) {
    const auto supervisor = manifest["supervisor"].toObject();
    return supervisor["pid"].toInteger() > 0 &&
           supervisor["host"].toString() == QSysInfo::machineHostName() && !executionProcessGone(supervisor);
}
bool executionRunGone(const QJsonObject &manifest) {
    const auto supervisor = manifest["supervisor"].toObject();
    if (supervisor.isEmpty())
        return executionProcessGone(manifest);
    return executionProcessGone(supervisor) && (!manifest.contains("pid") || executionProcessGone(manifest));
}
bool requestRunStop(const QString &directory) {
    QJsonObject manifest;
    try {
        manifest = readRunRecord(directory, RecordCheck::Json);
    } catch (const std::exception &) {
        return false;
    }
    if (!runStateActive(RunRecord(manifest).state()) || !runSupervised(manifest))
        return false;
    QFile request(stopRequest(directory));
    return request.open(QIODevice::WriteOnly);
}
QJsonObject executionProcessIdentity(qint64 pid) {
    QJsonObject identity{{"host", QSysInfo::machineHostName()}, {"pid", pid}};
    if (const auto boot = bootId(); !boot.isEmpty())
        identity["boot_id"] = boot;
    if (const auto start = processStart(pid); !start.isEmpty())
        identity["pid_start"] = start;
    return identity;
}
bool executionProcessGone(const QJsonObject &m) {
#ifdef Q_OS_UNIX
    if (m["host"].toString().isEmpty() || m["host"].toString() != QSysInfo::machineHostName())
        return false;
    const auto boot = bootId();
    if (!boot.isEmpty() && !m["boot_id"].toString().isEmpty() && m["boot_id"].toString() != boot)
        return true;
    const qint64 pid = m["pid"].toInteger();
    if (pid <= 0)
        return false;
    if (::kill(pid_t(pid), 0) == -1 && errno == ESRCH)
        return true;
    const auto start = processStart(pid);
    return !start.isEmpty() && !m["pid_start"].toString().isEmpty() && start != m["pid_start"].toString();
#elif defined(Q_OS_WIN)
    if (m["host"].toString().isEmpty() || m["host"].toString() != QSysInfo::machineHostName())
        return false;
    const qint64 pid = m["pid"].toInteger();
    return pid > 0 && !windowsProcessRunning(pid);
#else
    Q_UNUSED(m);
    return false;
#endif
}
} // namespace mf
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>

#include <tlhelp32.h> // After windows.h.
namespace mf {
namespace {
bool windowsProcessRunning(qint64 pid) {
    const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!handle)
        return GetLastError() == ERROR_ACCESS_DENIED; // Exists, but belongs to someone else.
    DWORD code = 0;
    const bool running = GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    CloseHandle(handle);
    return running;
}
// Creation time of a process, or 0 when it cannot be opened.
quint64 windowsProcessCreation(DWORD pid) {
    const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle)
        return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool ok = GetProcessTimes(handle, &created, &exited, &kernel, &user);
    CloseHandle(handle);
    return ok ? (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
}
void windowsTerminateTree(qint64 pid) {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    QMultiHash<DWORD, DWORD> children;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (bool more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        if (entry.th32ProcessID != entry.th32ParentProcessID)
            children.insert(entry.th32ParentProcessID, entry.th32ProcessID);
    CloseHandle(snapshot);
    // A parent id may have been recycled: only processes started after the run belong to its tree.
    const auto started = windowsProcessCreation(DWORD(pid));
    QList<DWORD> tree{DWORD(pid)};
    for (qsizetype i = 0; i < tree.size(); ++i)
        for (const auto child : children.values(tree[i]))
            if (!tree.contains(child) && windowsProcessCreation(child) >= started)
                tree << child;
    // Parents first, so that none of them starts a replacement for an ended child.
    for (const auto id : tree)
        if (const auto handle = OpenProcess(PROCESS_TERMINATE, FALSE, id)) {
            TerminateProcess(handle, 1);
            CloseHandle(handle);
        }
}
} // namespace
} // namespace mf
#endif
