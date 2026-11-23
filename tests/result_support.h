// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "foundation/files.h"
#include "nwchem/nwchem.h"
#include "results/energy_profile.h"
#include "results/results.h"
#include <QtTest>

// Result files and what is read from them, for the tests of the readers (test_result_readers) and of the
// result viewer (test_results, test_molden).

// The folder of a test executable's files.
struct ResultFiles {
    QTemporaryDir files;
    QString save(const QString &name, const QString &contents) {
        auto path = files.filePath(name);
        mf::writeFile(path, contents.toUtf8());
        return path;
    }
};
// The structure table of an NWChem log: water.
inline QString geometry() {
    return " Output coordinates in angstroms (scale by 1.8897 to convert to a.u.)\n\n"
           " No. Tag Charge X Y Z\n ---- ---- ------ ----- ----- -----\n"
           " 1 O 8.0 0.0 0.0 0.0\n 2 H 1.0 0.758 0.0 0.504\n 3 H 1.0 -0.758 0.0 0.504\n\n";
}
// A TMS geometry and synthetic shielding table, for reference import through the real log reader.
inline QString tmsReferenceLog() {
    const auto molecule = mf::Document::fromXyz(
        "17\nTMS\nC 0.92531677 0.05533749 -0.03222677\nSi 0.30075812 -0.34658816 -1.752366\n"
        "C 0.92528425 -2.03725363 -2.26439158\nC -1.57289768 -0.34658436 -1.75237273\n"
        "C 0.92531781 0.94219121 -2.96049998\nH 0.57181985 -0.68590833 0.69053451\n"
        "H 0.57182607 1.04005856 0.28728428\nH 2.0190206 0.06066049 -0.00933892\n"
        "H 0.57181823 -2.80631846 -1.57134191\nH 0.57180903 -2.29254462 -3.26769632\n"
        "H 2.01900673 -2.0597148 -2.27120506\nH -1.95959186 0.6328136 -1.45575891\n"
        "H -1.95959519 -0.57940372 -2.74886227\nH -1.95960845 -1.09315494 -1.0525\n"
        "H 0.57182748 1.93869499 -2.67997696\nH 2.01901061 0.95928865 -2.97657491\n"
        "H 0.57180645 0.7264572 -3.97304639\n");
    QString log = "NWChem\n Output coordinates in angstroms (scale by 1.8897 to convert to a.u.)\n\n"
                  " No. Tag Charge X Y Z\n ---- ---- ------ ----- ----- -----\n";
    for (int i = 0; i < molecule.atoms.size(); ++i) {
        const auto &a = molecule.atoms[i];
        log += QString(" %1 %2 %3 %4 %5 %6\n")
                   .arg(i + 1)
                   .arg(a.element)
                   .arg(a.element == "H"   ? 1
                        : a.element == "C" ? 6
                                           : 14)
                   .arg(a.position.x(), 0, 'f', 8)
                   .arg(a.position.y(), 0, 'f', 8)
                   .arg(a.position.z(), 0, 'f', 8);
    }
    log += "\nChemical Shielding Tensors (GIAO, in ppm)\n";
    int hydrogen = 0, carbon = 0;
    for (int i = 0; i < molecule.atoms.size(); ++i) {
        const auto element = molecule.atoms[i].element;
        const double sigma = element == "H"   ? 32 + .01 * hydrogen++
                             : element == "C" ? 180 + .1 * carbon++
                                              : 450;
        log += QString("Atom: %1 %2\nisotropic = %3\nanisotropy = 1\n")
                   .arg(i + 1)
                   .arg(element)
                   .arg(sigma, 0, 'f', 4);
    }
    return log;
}
inline QString cubeText(bool multiple = false) {
    QString s = "CUBE regression\nlinear field\n";
    s += multiple ? "-1 1 2 3\n" : "1 1 2 3\n";
    s += "2 1 0 0\n2 0.2 1 0\n2 0 0.3 1\n6 4 1 2 3\n";
    if (multiple)
        s += "2 5 6\n";
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) {
                s += QString::number(x - .5) + " ";
                if (multiple)
                    s += QString::number(y - .5) + " ";
            }
    return s + "\n";
}
// A MOLDEN file of H2 with two orbitals and two vibrations.
inline QString moldenText() {
    return "[Molden Format]\n[Atoms] AU\nH 1 1 0 0 0\nH 2 1 0 0 1.4\n"
           "[GTO]\n1 0\ns 1 1.0\n1.0 1.0\n"
           "[MO]\nSym= A1\nEne= -0.5D+00\nSpin= Alpha\nOccup= 2.0\n1 0.5\n2 0.5\n"
           "Sym= B1\nEne= 0.25\nSpin= Beta\nOccup= 0\n1 0.5\n2 -0.5\n"
           "[FREQ]\n-100\n1500\n[FR-COORD]\nH 0 0 0\nH 0 0 1.4\n"
           "[FR-NORM-COORD]\nvibration 1\n0 0 .5\n0 0 -.5\nvibration 2\n0 0 .1\n0 0 -.1\n"
           "[INT]\n10\n20\n";
}
// A MOLDEN file of one atom with an sp shell: three orbitals of two spins.
inline QString sample() {
    return "[Molden Format]\n[Atoms] AU\nH 1 1 0 0 0\n[GTO]\n1 0\nsp 1 0\n1 1 1\n"
           "[MO]\nSym= a\nEne= -0.5\nSpin= Alpha\nOccup= 1\n1 1\n"
           "Sym= b\nEne= 0.2\nSpin= Alpha\nOccup= 0\n2 1\n"
           "Sym= c\nEne= -0.4\nSpin= Beta\nOccup= 1\n3 1\n";
}
#ifdef FIXTURE_DIR // Of the executables that read the fixtures (CMakeLists.txt).
// The energy record of a fixture log, named after it.
inline QJsonObject logRecord(const QString &name) {
    const auto r = mf::readResult(QString(FIXTURE_DIR) + "/nwchem/" + name + ".out");
    auto record = mf::energyRecord(r, r.frames.last().molecule, mf::nwchemDefaults());
    record["id"] = name;
    record["name"] = name;
    return record;
}
#endif
// An energy profile of one stage for each record.
inline QJsonObject profileOf(const QJsonArray &records, const QString &metric, double temperature) {
    QJsonArray stages;
    for (const auto &record : records)
        stages.append(QJsonObject{
            {"name", record.toObject()["name"]},
            {"terms", QJsonArray{QJsonObject{{"record", record.toObject()["id"]}, {"coefficient", 1}}}}});
    return {{"format", "molforma-energy-profile"},
            {"version", 1},
            {"metric", metric},
            {"unit", "kcal"},
            {"temperature", temperature},
            {"standard_state", "1 atm"},
            {"reference", 0},
            {"records", records},
            {"stages", stages}};
}
