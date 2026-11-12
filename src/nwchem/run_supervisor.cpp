// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/run_supervisor.h"
#include "foundation/files.h"
#include "nwchem/execution_detail.h"
#include "nwchem/nwchem_run.h"
#include "nwchem/remote_run.h"
#include "nwchem/remote_supervisor.h"
#include "nwchem/run_process.h"
#include "nwchem/run_record.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#ifdef Q_OS_UNIX
#include <csignal>
#endif

namespace mf {
using namespace execution_detail;
namespace {
#ifdef Q_OS_UNIX
volatile std::sig_atomic_t stopSignalled = 0;
void stopSignal(int) {
    stopSignalled = 1;
}
#endif
} // namespace
QString runSupervisorProgram() {
    const auto image = qEnvironmentVariable("APPIMAGE");
    return QFileInfo(image).isFile() ? image : QCoreApplication::applicationFilePath();
}
int runJobMain(int argc, char **argv) {
    if (argc != 3 || QString::fromLocal8Bit(argv[1]) != "--run-job")
        return -1;
    QCoreApplication app(argc, argv);
    const auto directory = QDir::cleanPath(QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath());
    ExecutionPlan plan;
    try {
        // Only the job MolForma has just prepared and marked as starting, with the input it saved.
        const auto manifest = readRunRecord(directory, RecordCheck::Json);
        const RunRecord record(manifest);
        // A run on another computer: this process copies it there, follows it and fetches it back.
        if (record.hasFormat() && remoteRun(manifest))
            return remoteJobMain(directory, manifest);
        const auto input = readFile(jobFile(directory, JobFile::input));
        if (!record.hasFormat() || record.state() != RunState::Starting ||
            manifest["input_sha256"].toString() !=
                QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex()))
            return 2;
        plan = runRecordPlan(directory, manifest);
        plan.manifest["supervisor"] = executionProcessIdentity(QCoreApplication::applicationPid());
    } catch (const std::exception &) {
        return 2;
    }
#ifdef Q_OS_UNIX
    for (const int signal : {SIGTERM, SIGINT, SIGHUP})
        std::signal(signal, stopSignal);
#endif
    NwchemRun run;
    QTimer requests;
    requests.setInterval(250);
    QObject::connect(&requests, &QTimer::timeout, &run, [&] {
        bool asked = QFileInfo::exists(stopRequest(directory));
#ifdef Q_OS_UNIX
        asked |= stopSignalled != 0;
#endif
        if (asked)
            run.stop();
    });
    QObject::connect(&run, &NwchemRun::changed, &app, [&] {
        if (!run.active())
            QTimer::singleShot(0, &app, &QCoreApplication::quit);
    });
    run.startPrepared(plan);
    requests.start();
    const int code = run.active() ? app.exec() : 0;
    QFile::remove(stopRequest(directory));
    return code;
}
} // namespace mf
