// SPDX-License-Identifier: GPL-2.0-or-later
// A stand-in for the xtb program: reads input.xyz and xcontrol as xtb would, records its arguments and
// writes xtbopt.xyz with every atom that is not fixed moved 5 % away from the origin, the molecule turned
// and shifted as a whole (which MolForma fits back). MOLFORMA_FAKE_XTB: "fail" (an error, no structure),
// "slow" (runs for a minute), "unconverged" (the step limit, exit code 1), "fixed" (moves a fixed atom).
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <cmath>
#include <cstdio>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments().mid(1);
    const auto mode = qEnvironmentVariable("MOLFORMA_FAKE_XTB");
    {
        QFile capture("captured.json");
        if (capture.open(QIODevice::WriteOnly))
            capture.write(QJsonDocument(QJsonObject{{"arguments", QJsonArray::fromStringList(args)},
                                                    {"stack", qEnvironmentVariable("OMP_STACKSIZE")}})
                              .toJson());
    }
    // Copies for the test to look at (the folder is removed afterwards).
    if (const auto keep = qEnvironmentVariable("MOLFORMA_FAKE_XTB_KEEP"); !keep.isEmpty()) {
        QFile::remove(keep + "/captured.json");
        QFile::copy("captured.json", keep + "/captured.json");
        QFile::remove(keep + "/xcontrol");
        QFile::copy("xcontrol", keep + "/xcontrol");
    }
    std::printf("      -----------------------------------------------------------\n"
                "     |                   =====================                   |\n"
                "     |                           x T B                           |\n");
    if (mode == "fail") {
        std::printf("[ERROR] Program stopped due to fatal error\n-1- type_molecule: Unknown element\n");
        return 1;
    }
    if (mode == "slow")
        QThread::sleep(60);
    QSet<int> fixed;
    QFile control("xcontrol");
    if (control.open(QIODevice::ReadOnly)) {
        static const QRegularExpression atoms(R"(atoms:\s*(\S+))");
        if (const auto m = atoms.match(QString::fromUtf8(control.readAll())); m.hasMatch())
            for (const auto &part : m.captured(1).split(',')) {
                const auto range = part.split('-');
                for (int n = range[0].toInt(); n <= range.value(1, range[0]).toInt(); ++n)
                    fixed.insert(n);
            }
    }
    QFile input(args.value(0));
    if (!input.open(QIODevice::ReadOnly))
        return 2;
    const auto lines = QString::fromUtf8(input.readAll()).split('\n');
    const int count = lines[0].trimmed().toInt();
    QString out =
        QString::number(count) + "\n energy: -5.070544440612 gnorm: 0.000425187551 xtb: 6.7.1 (fake)\n";
    const double turn = 0.3, c = std::cos(turn), s = std::sin(turn);
    for (int i = 0; i < count; ++i) {
        const auto row = lines[i + 2].split(' ', Qt::SkipEmptyParts);
        double x = row[1].toDouble(), y = row[2].toDouble(), z = row[3].toDouble();
        if (!fixed.contains(i + 1)) {
            x *= 1.05;
            y *= 1.05;
            z *= 1.05;
        } else if (mode == "fixed")
            x += 0.1;
        if (fixed.isEmpty()) { // Turned about z and shifted, as a whole.
            const double rx = c * x - s * y, ry = s * x + c * y;
            x = rx + 1;
            y = ry - 2;
            z += 0.5;
        }
        out += QString("%1 %2 %3 %4\n")
                   .arg(row[0].left(1).toUpper() + row[0].mid(1).toLower())
                   .arg(x, 20, 'f', 14)
                   .arg(y, 20, 'f', 14)
                   .arg(z, 20, 'f', 14);
    }
    QFile result("xtbopt.xyz");
    if (!result.open(QIODevice::WriteOnly))
        return 3;
    result.write(out.toUtf8());
    if (mode == "unconverged") {
        std::printf("   *** FAILED TO CONVERGE GEOMETRY OPTIMIZATION IN 20 ITERATIONS ***\n");
        return 1;
    }
    std::printf("   *** GEOMETRY OPTIMIZATION CONVERGED AFTER 5 ITERATIONS ***\n"
                "          | TOTAL ENERGY               -5.070544440612 Eh   |\n");
    return 0;
}
