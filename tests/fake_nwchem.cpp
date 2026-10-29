// SPDX-License-Identifier: GPL-2.0-or-later
// Deterministic native process fixture: no NWChem, MPI, shell or network required.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <cstdio>
#ifdef Q_OS_UNIX
#include <csignal>
#endif
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments().mid(1);
    const auto mode = qEnvironmentVariable("MOLFORMA_FAKE_MODE");
    QJsonObject capture{{"cwd", QDir::currentPath()},
                        {"arguments", QJsonArray::fromStringList(args)},
                        {"omp", qEnvironmentVariable("OMP_NUM_THREADS")},
                        {"blas", qEnvironmentVariable("OPENBLAS_NUM_THREADS")},
                        {"literal", qEnvironmentVariable("MOLFORMA_LITERAL")}};
    QFile data("captured.json");
    if (!data.open(QIODevice::WriteOnly))
        return 10;
    data.write(QJsonDocument(capture).toJson());
    data.close();
    std::printf("fake NWChem started\n");
    std::fflush(stdout);
    std::fprintf(stderr, "fixture stderr\n");
    std::fflush(stderr);
    if (mode == "frequency") {
        // Optimization, then numerical frequencies: energies of displaced structures come last.
        std::printf(" NWChem Input Module\n Total DFT energy = -76.1\n NWChem Input Module\n"
                    " NWChem Nuclear Hessian and Frequency Analysis\n Total DFT energy = -76.1\n"
                    " Total DFT energy = -76.09\n NWChem Input Module\n Total times  cpu: 1.0s\n");
    }
    if (mode == "optimize") {
        // A DRIVER optimization under way: its thresholds and the first steps, then it keeps running.
        std::printf("                NWChem Input Module\n"
                    "  maximum gradient threshold         (gmax) =   0.000450\n"
                    "  rms gradient threshold             (grms) =   0.000300\n"
                    "  maximum cartesian step threshold   (xmax) =   0.001800\n"
                    "  rms cartesian step threshold       (xrms) =   0.001200\n"
                    "@ Step       Energy      Delta E   Gmax     Grms     Xrms     Xmax   Walltime\n"
                    "@    0     -76.02658720  0.0D+00  0.03036  0.02303  0.00000  0.00000      0.4\n"
                    "         Total DFT energy =      -76.026587203\n"
                    "@    1     -76.03010450 -3.5D-03  0.00040  0.00020  0.00500  0.00900      0.9\n");
        std::fflush(stdout);
    }
    if (mode == "large") {
        const QByteArray output(200 * 1024, 'x');
        std::fwrite(output.constData(), 1, output.size(), stdout);
        std::printf("\nlog tail marker\n");
    }
    QProcess child;
    if (mode == "tree") {
#ifdef Q_OS_UNIX
        std::signal(SIGTERM, SIG_IGN);
#endif
        if (!args.contains("--child")) {
            child.start(app.applicationFilePath(), {"--child"});
            if (!child.waitForStarted())
                return 12;
            QFile pid("child.pid");
            if (pid.open(QIODevice::WriteOnly))
                pid.write(QByteArray::number(child.processId()));
        }
    }
    const int delay = mode == "slow" || mode == "tree" || mode == "optimize" ? 60000 : 150;
    QTimer::singleShot(delay, &app, [&] {
        std::printf("fake NWChem finished\n");
        std::fflush(stdout);
        app.exit(mode == "fail" ? 7 : 0);
    });
    return app.exec();
}
