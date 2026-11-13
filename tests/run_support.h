// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "foundation/files.h"
#include "nwchem/nwchem_execution.h"
#include "test_support.h"
#include <QtTest>

// Calculations run by the stand-in NWChem (fake_nwchem) and their job folders, for the tests of runs
// and of the history (test_runs) and of the screens that show them (test_execution, test_history).

inline void environment(mf::Document &d, const QMap<QString, QString> &values) {
    QJsonArray rows;
    for (auto it = values.begin(); it != values.end(); ++it)
        rows.append(QJsonObject{{"enabled", true}, {"name", it.key()}, {"value", it.value()}});
    mf::setExecutionSetting(d, "environment", rows);
}
inline QJsonObject readObject(const QString &path) {
    return QJsonDocument::fromJson(mf::readFile(path)).object();
}
// The folders of a test executable, and water set up to run in them.
struct RunFixture {
    QTemporaryDir files, settings;
    QString helper; // The stand-in NWChem, copied to a path with a space.
    void copyEngine() {
        helper = files.filePath("native engine" EXE);
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/fake_nwchem" EXE, helper));
        QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    mf::Document configured(const QString &mode = {}) const {
        auto d = water();
        mf::setExecutionSetting(d, "mode", "direct");
        mf::setExecutionSetting(d, "executable", helper);
        mf::setExecutionSetting(d, "job_root", files.filePath("jobs with spaces"));
        if (!mode.isEmpty())
            environment(d, {{"MOLFORMA_FAKE_MODE", mode}});
        return d;
    }
};
// Water for a job of the history: its jobs go to root, and the project holds what a job must keep.
inline mf::Document water(const QString &root) {
    auto d = mf::Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
    d.atoms[0].fixed = true;
    d.envelope["future"] = QJsonObject{{"preserve", 12}};
    mf::setExecutionSetting(d, "job_root", root);
    mf::setExecutionSetting(d, "mode", "direct");
    mf::setExecutionSetting(d, "executable", QCoreApplication::applicationDirPath() + "/fake_nwchem" EXE);
    return d;
}
// Writes the record of a saved job as a run in that state leaves it.
inline void manifest(mf::ExecutionPlan &p, const QString &state, int seconds = 0) {
    p.manifest["status"] = state;
    p.manifest["elapsed_seconds"] = seconds;
    if (state == "finished")
        p.manifest["exit_code"] = 0;
    mf::writeFile(p.directory + "/run.json", QJsonDocument(p.manifest).toJson());
}
