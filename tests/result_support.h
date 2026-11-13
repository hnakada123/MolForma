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
