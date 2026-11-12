// SPDX-License-Identifier: GPL-2.0-or-later
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "io/document_storage.h"
#include "mcp/policy.h"
#include "mcp/tools.h"
#include "model/settings.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_plan.h"
#include "nwchem/nwchem_queue.h"
#include "nwchem/nwchem_restart.h"
#include "nwchem/remote_execution.h"
#include "nwchem/run_diagnostics.h"
#include "nwchem/run_record.h"
#include "results/results.h"
#include "ui/execution_panel.h"
#include "ui/main_window.h"
#include "ui/remote_hosts_dialog.h"
#include "ui/setting_navigation.h"
#include <QtTest>
#include <QtWidgets>
#ifdef Q_OS_UNIX
#include <csignal>
#endif
using namespace mf;
// Executables carry .exe on Windows.
#ifdef Q_OS_WIN
#define EXE ".exe"
#else
#define EXE ""
#endif

class ExecutionTest : public QObject {
    Q_OBJECT
    QTemporaryDir files, settings;
    QString helper, remoteHome;
    static Document water() {
        return Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
    }
    Document configured(const QString &mode = {}) const {
        auto d = water();
        setExecutionSetting(d, "mode", "direct");
        setExecutionSetting(d, "executable", helper);
        setExecutionSetting(d, "job_root", files.filePath("jobs with spaces"));
        if (!mode.isEmpty())
            environment(d, {{"MOLFORMA_FAKE_MODE", mode}});
        return d;
    }
    static void environment(Document &d, const QMap<QString, QString> &values) {
        QJsonArray rows;
        for (auto it = values.begin(); it != values.end(); ++it)
            rows.append(QJsonObject{{"enabled", true}, {"name", it.key()}, {"value", it.value()}});
        setExecutionSetting(d, "environment", rows);
    }
    static QJsonObject readObject(const QString &path) {
        return QJsonDocument::fromJson(readFile(path)).object();
    }
    static QPushButton *button(MainWindow &w, const char *name) {
        return w.findChild<QPushButton *>(name);
    }
    static void input(MainWindow &w, const char *key, const QString &value) {
        auto *line = w.findChild<QLineEdit *>(QString("execution_") + key);
        QVERIFY(line);
        line->setText(value);
        line->setModified(true);
        QMetaObject::invokeMethod(line, "editingFinished");
    }
  private slots:
    void initTestCase() {
        QVERIFY(files.isValid());
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settings.path());                          // MolForma uses NativeFormat.
        qputenv("XDG_DATA_HOME", settings.filePath("data").toUtf8()); // The stored queue of sessions.
        // The supervisors this test starts read the same Preferences (remote destinations).
        qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
        helper = files.filePath("native engine" EXE);
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/fake_nwchem" EXE, helper));
        QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    void runDiagnostics() {
        const auto scf = diagnoseRun(RunState::Failed, "SCF failed to converge in 20 iterations\n", {});
        QCOMPARE(scf.size(), 1);
        QCOMPARE(scf[0].toObject()["code"].toString(), "scf_convergence");
        QCOMPARE(scf[0].toObject()["setting"].toString(), "scf_maxiter");
        QCOMPARE(scf[0].toObject()["source"].toString(), "stdout");
        QCOMPARE(scf[0].toObject()["target"].toString(), "calculation");
        QVERIFY(!scf[0].toObject().contains("tab"));
        // The tab of a diagnostic as a record holds it.
        const auto tab = [](const QJsonObject &diagnostic) {
            const RunRecord record(QJsonObject{{"diagnostics", QJsonArray{diagnostic}}});
            return workspaceTab(record.diagnostics().first().owner());
        };
        QCOMPARE(tab(scf[0].toObject()), 1);
        QCOMPARE(tab(QJsonObject{{"tab", 1}}), 1); // Older saved records.
        QCOMPARE(tab(QJsonObject{{"target", "execution"}, {"tab", 0}}), 2);
        QCOMPARE(tab(QJsonObject{{"tab", 0}}), 0);
        QCOMPARE(tab(QJsonObject{{"tab", 9}}), 2); // Any other number, or none, was the execution's.
        QCOMPARE(tab(QJsonObject{}), 2);
        // The legacy numbers have one definition beside the targets; today's tabs are the UI's.
        for (const auto target :
             {SettingTarget::Molecule, SettingTarget::Calculation, SettingTarget::Execution}) {
            QVERIFY(settingTargetFromLegacyTab(legacySettingTab(target)) == target);
            const RunRecord record(
                QJsonObject{{"diagnostics", QJsonArray{QJsonObject{{"tab", legacySettingTab(target)}}}}});
            QVERIFY(!record.diagnostics().first().target && record.diagnostics().first().owner() == target);
        }
        QCOMPARE(legacySettingTab(SettingTarget::Molecule), 0);
        QCOMPARE(legacySettingTab(SettingTarget::Calculation), 1);
        QCOMPARE(legacySettingTab(SettingTarget::Execution), 2);
        QVERIFY(settingTargetFromLegacyTab(-1) == SettingTarget::Execution);
        QCOMPARE(diagnoseRun(RunState::Finished, "maxiter 20\nSCF converged\n", {}).size(), 0);
        const auto memory = diagnoseRun(RunState::Finished, {}, "MA_alloc_get: failed\n");
        QCOMPARE(memory[0].toObject()["code"].toString(), "memory");
        QCOMPARE(diagnoseRun(RunState::Failed, {}, {})[0].toObject()["code"].toString(), "process");
        QCOMPARE(diagnoseRun(RunState::TimedOut, {}, {})[0].toObject()["code"].toString(), "time_limit");
        QCOMPARE(diagnoseRun(RunState::Failed, {}, {}, "not executable")[0].toObject()["code"].toString(),
                 "launch");
        QCOMPARE(diagnoseRun(RunState::Failed, "maximum number of optimization steps exceeded", {})[0]
                     .toObject()["code"]
                     .toString(),
                 "optimization_limit");
    }
    void conformerBatchSnapshots() {
        MainWindow w;
        auto d = configured();
        w.loadDocument(d);
        w.trustCurrentExecution();
        const auto before = w.document().project();
        auto other = d;
        other.atoms[1].position.x() += .2;
        const auto jobs = w.executionPanel()->enqueueGeometries({d, other}, true);
        QCOMPARE(jobs.size(), 2);
        QCOMPARE(w.document().project(), before);
        const auto first = historyDocument(jobs[0]), second = historyDocument(jobs[1]);
        QCOMPARE(nwchemSettings(first)["task"].toString(), "optimize_frequencies");
        QCOMPARE(second.ids(), d.ids());
        QVERIFY((second.atoms[1].position - other.atoms[1].position).norm() < 1e-9);
        QVERIFY(!w.executionPanel()->running());
        auto invalid = other;
        invalid.atoms[1].element = "He";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 w.executionPanel()->enqueueGeometries({d, invalid}, false));
        QCOMPARE(w.runSession().queue().pending(), 2);
    }
    void queueSnapshotsAndOrder() {
        NwchemRun run;
        NwchemQueue queue(run);
        auto first = configured();
        setNwchemSetting(first, "title", "snapshot one");
        const auto id1 = queue.enqueue(first, files.path());
        setNwchemSetting(first, "title", "edited later");
        const auto id2 = queue.enqueue(first, files.path());
        const auto path1 = queue.entries()[0].plan.directory;
        const auto path2 = queue.entries()[1].plan.directory;
        const auto recorded = readHistoryEntry(path1);
        QVERIFY2(recorded.valid, qPrintable(recorded.warnings.join('\n')));
        QCOMPARE(recorded.status, RunState::Prepared);
        QCOMPARE(readObject(path1 + "/run.json"), queue.entries()[0].plan.manifest);
        QVERIFY(readFile(path1 + "/input.nw").contains("snapshot one"));
        QVERIFY(!readFile(path1 + "/input.nw").contains("edited later"));
        QCOMPARE(queue.pending(), 2);
        QVERIFY(queue.paused());
        QVERIFY(!run.active());
        queue.move(id2, 0);
        QCOMPARE(queue.entries()[0].id, id2);
        QStringList started;
        connect(&run, &NwchemRun::changed, &queue, [&] {
            if (run.state() == RunState::Starting)
                started << run.plan().directory;
        });
        queue.resume();
        QTRY_VERIFY_WITH_TIMEOUT(queue.entries()[0].state != RunState::Prepared, 5000);
        QVERIFY2(queue.entries()[0].state != RunState::Failed, qPrintable(queue.entries()[0].error));
        QTRY_COMPARE_WITH_TIMEOUT(queue.entries()[1].state, RunState::Finished, 5000);
        QCOMPARE(started, (QStringList{path2, path1}));
        QCOMPARE(queue.entries()[1].id, id1);
        QCOMPARE(queue.pending(), 0);
        QCOMPARE(readObject(path1 + "/run.json")["status"].toString(), "finished");
    }
    void queueChainsStructures() {
        NwchemRun run;
        NwchemQueue queue(run);
        const auto first = queue.enqueue(configured("geometry"), files.path());
        auto next = configured();
        setNwchemSetting(next, "title", "chained");
        const auto second = queue.enqueue(next, files.path(), {}, QueueChain{first, "final", {}});
        // No bundle before the structure is there, and never ahead of its source.
        QVERIFY(queue.entries()[1].plan.directory.isEmpty());
        QCOMPARE(queue.json()["entries"].toArray()[1].toObject()["chain"].toObject()["from"].toString(),
                 first);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, queue.move(second, 0));
        QCOMPARE(queue.entries()[1].id, second);
        // The highest point needs a path; other atoms do not match.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 queue.enqueue(next, files.path(), {}, QueueChain{first, "highest", {}}));
        auto other = configured();
        other.atoms[1].element = "F";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 queue.enqueue(other, files.path(), {}, QueueChain{first, "final", {}}));
        QCOMPARE(queue.entries().size(), 2);
        QStringList saved;
        connect(&queue, &NwchemQueue::bundleSaved, this, [&](const QString &job) { saved << job; });
        queue.resume();
        QTRY_COMPARE_WITH_TIMEOUT(queue.entries()[1].state, RunState::Finished, 10000);
        const auto job = queue.entries()[1].plan.directory;
        QCOMPARE(saved, QStringList{job});
        const auto taken = historyDocument(job);
        QCOMPARE(taken.ids(), next.ids());
        QVERIFY((taken.atoms[1].position - Vec(0.8, 0, 0.55)).norm() < 1e-9);
        QVERIFY(readFile(job + "/input.nw").contains("chained"));
        QVERIFY(readFile(job + "/input.nw").contains("  H 0.8 0 0.55\n"));
        // A source that failed: what goes on from it does not start.
        queue.setStopOnFailure(false);
        const auto failing = queue.enqueue(configured("fail"), files.path());
        queue.enqueue(next, files.path(), {}, QueueChain{failing, "final", {}});
        QTRY_COMPARE_WITH_TIMEOUT(queue.entries()[3].state, RunState::Failed, 10000);
        QVERIFY(queue.entries()[3].error.contains("正常に終わっていない"));
        QVERIFY(queue.entries()[3].plan.directory.isEmpty());
        // Cancelling a source cancels the chain after it.
        queue.pause();
        const auto source = queue.enqueue(configured(), files.path());
        const auto chain =
            queue.enqueueChain(configured(), files.path(), source, "final", {"saddle", "irc", "ends"});
        QCOMPARE(chain.size(), 4);
        const auto entry = [&](const QString &id) {
            for (const auto &e : queue.entries())
                if (e.id == id)
                    return e;
            return QueuedCalculation{};
        };
        QCOMPARE(nwchemSettings(entry(chain[0]).document)["task"].toString(), "saddle_frequencies");
        QCOMPARE(entry(chain[0]).name, "molforma_ts");
        QCOMPARE(entry(chain[1]).chain.from, chain[0]);
        QCOMPARE(nwchemSettings(entry(chain[1]).document)["task"].toString(), "irc");
        QCOMPARE(entry(chain[2]).chain.structure, "irc_forward");
        QCOMPARE(entry(chain[3]).chain.structure, "irc_backward");
        QCOMPARE(entry(chain[3]).chain.from, chain[1]);
        QCOMPARE(nwchemSettings(entry(chain[3]).document)["task"].toString(), "optimize_frequencies");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 queue.enqueueChain(configured(), files.path(), source, "final", {"ends"}));
        queue.cancel(source);
        for (const auto &id : chain)
            QCOMPARE(entry(id).state, RunState::Cancelled);
        QCOMPARE(queue.pending(), 0);
    }
    void chainDialog() {
        MainWindow w;
        w.loadDocument(configured());
        w.trustCurrentExecution();
        auto *panel = w.executionPanel();
        w.runSession().queue().pause();
        panel->enqueueCalculation();
        panel->showChainDialog();
        auto *dialog = w.findChild<QDialog *>("calculationChainDialog");
        QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QComboBox *>("calculationChainSource")->count(), 1);
        // A single optimization is no path: its final structure is offered first.
        QCOMPARE(dialog->findChild<QComboBox *>("calculationChainStructure")->currentData().toString(),
                 "final");
        dialog->findChild<QCheckBox *>("calculationChainEnds")->setChecked(false);
        dialog->findChild<QPushButton *>("calculationChainAdd")->click();
        const auto &entries = w.runSession().queue().entries();
        QCOMPARE(entries.size(), 3);
        QCOMPARE(entries[1].chain.from, entries[0].id);
        QCOMPARE(entries[2].chain.from, entries[1].id);
        QCOMPARE(nwchemSettings(entries[2].document)["task"].toString(), "irc");
        auto *table = w.findChild<QTableWidget *>("calculationQueueTable");
        QVERIFY(table);
        QVERIFY(table->item(2, 2)->text().contains("最終構造"));
        QVERIFY(table->item(2, 3)->text().contains("終了後に保存"));
    }
    void queueChainSurvivesTheSession() {
        const auto store = files.filePath("chain queue/queue.json");
        QString first, second;
        {
            NwchemRun run;
            NwchemQueue queue(run);
            queue.setStore(store);
            first = queue.enqueue(configured("geometry"), files.path());
            second = queue.enqueue(configured(), files.path(), {}, QueueChain{first, "final", {}});
            const auto kept = readObject(store)["entries"].toArray();
            QCOMPARE(kept.size(), 2);
            QCOMPARE(kept[1].toObject()["chain"].toObject()["from"].toString(), first);
            queue.shutdown();
        }
        NwchemRun run;
        NwchemQueue queue(run);
        queue.setStore(store);
        QCOMPARE(queue.restore(), 2);
        QCOMPARE(queue.entries()[1].chain.from, first);
        QCOMPARE(queue.entries()[1].document.atoms.size(), 3);
        queue.resume();
        QTRY_COMPARE_WITH_TIMEOUT(queue.entries()[1].state, RunState::Finished, 10000);
        QVERIFY(
            (historyDocument(queue.entries()[1].plan.directory).atoms[0].position - Vec(0, 0, 0.1)).norm() <
            1e-9);
    }
    void queueFailurePauseAndCancellation() {
        NwchemRun run;
        NwchemQueue queue(run);
        queue.enqueue(configured("fail"), files.path());
        const auto next = queue.enqueue(configured(), files.path());
        queue.resume();
        QTRY_COMPARE_WITH_TIMEOUT(queue.entries()[0].state, RunState::Failed, 5000);
        QVERIFY(queue.paused());
        QCOMPARE(queue.pending(), 1);
        QVERIFY(!QFileInfo::exists(queue.entries()[1].plan.directory + "/captured.json"));
        queue.cancel(next);
        QCOMPARE(queue.pending(), 0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, queue.cancel(next));
        queue.setStopOnFailure(false);
        queue.enqueue(configured("fail"), files.path());
        queue.enqueue(configured(), files.path());
        queue.resume();
        QTRY_COMPARE_WITH_TIMEOUT(queue.entries().last().state, RunState::Finished, 5000);
        QCOMPARE(queue.entries()[2].state, RunState::Failed);
    }
    void queueRejectsChangedFilesAndAccess() {
        NwchemRun run;
        NwchemQueue queue(run);
        queue.enqueue(configured(), files.path());
        const auto directory = queue.entries()[0].plan.directory;
        writeFile(directory + "/input.nw", "changed\n");
        queue.resume();
        QTRY_COMPARE(queue.entries()[0].state, RunState::Failed);
        QVERIFY(!run.active());
        QVERIFY(!QFileInfo::exists(directory + "/captured.json"));
        QVERIFY(!queue.entries()[0].error.isEmpty());
        bool allowed = true;
        queue.enqueue(configured(), files.path(), [&] {
            if (!allowed)
                throw std::runtime_error("access changed");
        });
        allowed = false;
        queue.resume();
        QTRY_COMPARE(queue.entries()[1].state, RunState::Failed);
        QCOMPARE(queue.entries()[1].error, QString("access changed"));
        queue.enqueue(configured(), files.path());
        writeFile(queue.entries()[2].plan.directory + "/output.out", "existing output\n");
        queue.resume();
        QTRY_COMPARE(queue.entries()[2].state, RunState::Failed);
        QCOMPARE(readFile(queue.entries()[2].plan.directory + "/output.out"),
                 QByteArray("existing output\n"));
    }
    void queuePauseAndShutdown() {
        NwchemRun run;
        NwchemQueue queue(run);
        queue.enqueue(configured("slow"), files.path());
        queue.enqueue(configured(), files.path());
        queue.resume();
        QTRY_VERIFY(run.active());
        queue.pause();
        QVERIFY(run.active());
        queue.shutdown();
        run.shutdown();
        QVERIFY(!run.active());
        QCOMPARE(queue.pending(), 1);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, queue.resume());
        QVERIFY(!QFileInfo::exists(queue.entries()[1].plan.directory + "/captured.json"));
    }
    void defaultsAndCompatibility() {
        auto d = water();
        d.envelope["execution"] = QJsonObject{{"legacy_exe", "keep"}, {"future", 12}};
        d.envelope["result_reference"] = QJsonObject{{"job_dir", "old-result"}};
        const auto before = d.project();
        QCOMPARE(executionSettings(d)["mpi_ranks"].toInt(), 2);
        QVERIFY(validateExecution(d, files.path(), false).errors.isEmpty());
        QCOMPARE(d.project(), before);
        setExecutionSetting(d, "future", QJsonObject{{"keep", 9}});
        setExecutionSetting(d, "omp_threads", 3);
        const auto persisted = executionSettings(d);
        applyNwchemPreset(d, nwchemPresets().first().settings);
        QCOMPARE(executionSettings(d), persisted);
        const auto restored = Document::fromProject(d.project());
        QCOMPARE(executionSettings(restored), persisted);
        QCOMPARE(restored.envelope["execution"], before["execution"]);
        QCOMPARE(restored.envelope["result_reference"], before["result_reference"]);
    }
    void savedPreferences() {
        // Preferences fill new documents and structure files; projects keep their own settings.
        const QJsonObject chosen{
            {"executable", helper},
            {"mode", "direct"},
            {"mpi_ranks", 6},
            {"job_root", files.filePath("preferred-jobs")},
            {"reference_directory", "/not/a/preference"},
            {"environment",
             QJsonArray{QJsonObject{{"enabled", true}, {"name", "MOLFORMA_X"}, {"value", "1"}}}}};
        saveExecutionPreferences(chosen);
        QCOMPARE(executionPreferences()["mpi_ranks"].toInt(), 6);
        QCOMPARE(executionPreferences()["launcher"].toString(), QString("mpirun"));
        QVERIFY(!executionPreferences().contains("reference_directory"));
        Document fresh;
        applyExecutionPreferences(fresh);
        QCOMPARE(executionSettings(fresh)["executable"].toString(), helper);
        QVERIFY(!fresh.data["nwchem_execution"].toObject().contains("launcher")); // Defaults stay implicit.
        const auto xyz = files.filePath("preferred.xyz"), project = files.filePath("own.molforma.json");
        writeFile(xyz, "3\nwater\nO 0 0 0\nH .76 0 .5\nH -.76 0 .5\n");
        auto own = water();
        setExecutionSetting(own, "mpi_ranks", 3);
        saveProject(own, project);
        {
            MainWindow w;
            QCOMPARE(executionSettings(w.document())["mpi_ranks"].toInt(), 6);
            w.loadPath(xyz);
            QCOMPARE(executionSettings(w.document())["executable"].toString(), helper);
            w.loadPath(project);
            QCOMPARE(executionSettings(w.document())["mpi_ranks"].toInt(), 3);
        }
        saveExecutionPreferences({});
        // "編集 → 環境設定" shows the controls of "3 実行設定", with the same ranges and new rows.
        {
            MainWindow w;
            bool checked = false;
            QTimer::singleShot(0, &w, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog && dialog->objectName() == "preferencesDialog");
                QVERIFY(!dialog->findChild<QLineEdit *>("preferences_reference_directory"));
                auto *ranks = dialog->findChild<QSpinBox *>("preferences_mpi_ranks");
                QCOMPARE(ranks->maximum(), executionRange("mpi_ranks").maximum);
                QCOMPARE(dialog->findChild<QSpinBox *>("preferences_timeout_seconds")->maximum(),
                         executionRange("timeout_seconds").maximum);
                ranks->setValue(5);
                dialog->findChild<QPushButton *>("preferencesAddEnvironment")->click();
                auto *table = dialog->findChild<QTableWidget *>("preferencesEnvironment");
                QCOMPARE(table->rowCount(), 1);
                QCOMPARE(table->item(0, 0)->checkState(), Qt::Unchecked);
                table->item(0, 1)->setText("MOLFORMA_Y");
                table->item(0, 0)->setCheckState(Qt::Checked);
                // A row left empty is not kept.
                dialog->findChild<QPushButton *>("preferencesAddEnvironment")->click();
                QTRY_COMPARE(table->rowCount(), 2);
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
                checked = true;
            });
            w.findChild<QAction *>("preferencesAction")->trigger();
            QVERIFY(checked);
            QCOMPARE(executionPreferences()["mpi_ranks"].toInt(), 5);
            const auto rows = executionPreferences()["environment"].toArray();
            QCOMPARE(rows.size(), 1);
            QCOMPARE(rows[0].toObject()["name"].toString(), QString("MOLFORMA_Y"));
            QVERIFY(rows[0].toObject()["enabled"].toBool());
        }
        saveExecutionPreferences({});
        QCOMPARE(executionPreferences(), [] {
            QJsonObject defaults;
            for (const auto &key : executionPreferenceKeys())
                defaults[key] = executionDefaults()[key];
            return defaults;
        }());
    }
    void validationAndEnvironment() {
        auto d = configured();
        QVERIFY(validateExecution(d, files.path()).errors.isEmpty());
        setExecutionSetting(d, "mpi_ranks", 1.5);
        QVERIFY(!validateExecution(d, files.path()).errors.isEmpty());
        setExecutionSetting(d, "mpi_ranks", 1);
        setExecutionSetting(d, "mode", "mpi");
        setExecutionSetting(d, "launcher", helper);
        QVERIFY(validateExecution(d, files.path()).warnings.join('\n').contains("補助プロセス"));
        setExecutionSetting(d, "omp_stacksize", "64M");
        QCOMPARE(validateExecution(d, files.path()).environment.value("OMP_STACKSIZE"), "64M");
        setExecutionSetting(d, "omp_stacksize", "64M\nBAD=1");
        QVERIFY(!validateExecution(d, files.path()).errors.isEmpty());
        setExecutionSetting(d, "omp_stacksize", "");
        for (const auto &name : {"OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "BAD-NAME"}) {
            environment(d, {{name, "2"}});
            QVERIFY(!validateExecution(d, files.path()).errors.isEmpty());
        }
        environment(d, {{"OK_NAME", "literal value"}, {"NWCHEM_BASIS_LIBRARY", "."}});
        auto r = validateExecution(d, files.path());
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.environment.value("NWCHEM_BASIS_LIBRARY"), files.path() + '/');
        auto rows = executionSettings(d)["environment"].toArray();
        rows.append(rows.first());
        setExecutionSetting(d, "environment", rows);
        QVERIFY(validateExecution(d, files.path()).errors.join('\n').contains("重複"));
        rows[rows.size() - 1] = QJsonObject{{"enabled", false}, {"name", "BAD-NAME"}, {"value", "retained"}};
        setExecutionSetting(d, "environment", rows);
        QVERIFY(validateExecution(d, files.path()).errors.isEmpty());
        setExecutionSetting(d, "executable", files.filePath("missing"));
        QVERIFY(!validateExecution(d, files.path()).errors.isEmpty());
        QVERIFY(validateExecution(d, files.path(), false).errors.isEmpty());
        d.data["nwchem_execution"] = "malformed";
        QVERIFY(validateExecution(d, files.path(), false).errors.join('\n').contains("オブジェクト"));
        // A network location (UNC) that the document names and Preferences do not is not looked at:
        // on Windows that would offer the user's credentials to the server.
        QVERIFY(networkPath("\\\\server\\share") && networkPath("//server/share") &&
                !networkPath("/srv/jobs"));
        for (const auto *key : {"executable", "job_root", "reference_directory"}) {
            auto network = configured();
            setExecutionSetting(network, key, "//server/share/molforma");
            QCOMPARE(untrustedNetworkPath(network), QString("//server/share/molforma"));
            QVERIFY2(
                validateExecution(network, files.path()).errors.join('\n').contains("ネットワーク上の場所"),
                key);
        }
        QVERIFY(untrustedNetworkPath(configured()).isEmpty());
    }
    void plansAndIsolatedBundles() {
        auto d = configured();
        setExecutionSetting(d, "mode", "mpi");
        setExecutionSetting(d, "launcher", helper);
        setExecutionSetting(d, "mpi_ranks", 4);
        setExecutionSetting(d, "omp_threads", 2);
        environment(d, {{"MOLFORMA_LITERAL", "a ' ; $(touch SHOULD_NOT_EXIST)"}});
        const auto before = d.project();
        auto p = executionPlan(d, files.path(), files.filePath("preview"));
        QCOMPARE(p.program, helper);
        QCOMPARE(p.arguments, QStringList({"-np", "4", helper, "input.nw"}));
        QVERIFY(executionCommand(p).contains("'\\''"));
        QCOMPARE(p.environment.value("OMP_NUM_THREADS"), "2");
        QCOMPARE(p.environment.value("OPENBLAS_NUM_THREADS"), "1");
        QVERIFY(!p.manifest["environment_overrides"].toObject().contains("HOME"));
        setExecutionSetting(d, "executable", files.filePath("not installed"));
        const auto first = saveExecutionBundle(d, files.path());
        const auto second = saveExecutionBundle(d, files.path());
        QVERIFY(first.directory != second.directory);
        QCOMPARE(QString::fromUtf8(readFile(first.directory + "/input.nw")), nwchemInput(d));
        QCOMPARE(readObject(first.directory + "/run.json")["status"].toString(), "prepared");
        QCOMPARE(Document::fromProject(readObject(first.directory + "/project.molforma.json")).toJson(),
                 d.toJson());
        QVERIFY(!QFileInfo::exists(first.directory + "/output.out"));
        setExecutionSetting(d, "executable", helper);
        QCOMPARE(d.project(), before);
    }
    void advancedPathsAndCollisions() {
        auto d = configured();
        writeFile(files.filePath("seed orbitals.movecs"), "seed");
        setNwchemAdvanced(d, "vectors_guess", "file");
        setNwchemAdvanced(d, "vectors_input", "seed orbitals.movecs");
        setNwchemAdvanced(d, "vectors_output", "orbitals/new.movecs");
        setNwchemAdvanced(d, "scratch_dir", "scratch");
        setNwchemAdvanced(d, "permanent_dir", "permanent");
        const auto before = d.toJson();
        const auto p = saveExecutionBundle(d, files.path(), true);
        QVERIFY(p.input.contains("input \"" + files.filePath("seed orbitals.movecs") + "\""));
        QVERIFY(p.input.contains("output \"orbitals/new.movecs\""));
        QVERIFY(QFileInfo(p.directory + "/scratch").isDir());
        QVERIFY(QFileInfo(p.directory + "/permanent").isDir());
        QVERIFY(QFileInfo(p.directory + "/permanent/orbitals").isDir());
        QCOMPARE(d.toJson(), before);
        setNwchemAdvanced(d, "vectors_output", files.filePath("seed orbitals.movecs"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, executionPlan(d, files.path(), p.directory));
        QCOMPARE(readFile(files.filePath("seed orbitals.movecs")), QByteArray("seed"));
        for (const auto &bad : {"../outside.movecs", "input.nw", "run.json", "stderr.err"}) {
            setNwchemAdvanced(d, "vectors_output", bad);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, executionPlan(d, files.path(), p.directory));
        }
        setNwchemAdvanced(d, "vectors_output", "new.movecs");
        setNwchemAdvanced(d, "vectors_input", "absent.movecs");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, executionPlan(d, files.path(), p.directory));
        setNwchemAdvanced(d, "vectors_input", "seed orbitals.movecs");
        setNwchemAdvanced(d, "permanent_dir", files.path());
        writeFile(files.filePath("molforma.db"), "previous");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, executionPlan(d, files.path(), p.directory));
        QCOMPARE(readFile(files.filePath("molforma.db")), QByteArray("previous"));
    }
    void processSnapshotAndLiteralArguments() {
        auto d = configured();
        environment(d, {{"MOLFORMA_LITERAL", "a ' ; $(touch SHOULD_NOT_EXIST)"}});
        NwchemRun run;
        run.start(d, files.path());
        const auto directory = run.plan().directory;
        const auto input = nwchemInput(d);
        d.atoms[1].position.x() += 5;
        setExecutionSetting(d, "omp_threads", 9);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, run.start(d, files.path()));
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 5000);
        QCOMPARE(run.state(), RunState::Finished);
        QCOMPARE(QString::fromUtf8(readFile(directory + "/input.nw")), input);
        const auto captured = readObject(directory + "/captured.json");
        QCOMPARE(captured["arguments"].toArray(), QJsonArray{"input.nw"});
        // The temporary folder may be reached through a symbolic link (/var -> /private/var on macOS).
        QCOMPARE(QFileInfo(captured["cwd"].toString()).canonicalFilePath(),
                 QFileInfo(directory).canonicalFilePath());
        QCOMPARE(captured["omp"].toString(), "1");
        QCOMPARE(captured["literal"].toString(), "a ' ; $(touch SHOULD_NOT_EXIST)");
        QVERIFY(!QFileInfo::exists(directory + "/SHOULD_NOT_EXIST"));
        QVERIFY(readFile(directory + "/output.out").contains("finished"));
        QVERIFY(readFile(directory + "/stderr.err").contains("fixture stderr"));
        const auto manifest = readObject(directory + "/run.json");
        QCOMPARE(manifest["status"].toString(), "finished");
        QCOMPARE(manifest["exit_code"].toInt(-1), 0);
    }
    void failedLaunchExitAndRestart() {
        NwchemRun run;
        auto d = configured("fail");
        run.start(d, files.path());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 5000);
        QCOMPARE(run.state(), RunState::Failed);
        QCOMPARE(run.plan().manifest["exit_code"].toInt(), 7);
        const auto first = run.plan().directory;
#ifdef Q_OS_UNIX
        const auto broken = files.filePath("broken executable");
        writeFile(broken, "#!/nonexistent/molforma-interpreter\n");
        QVERIFY(QFile::setPermissions(broken, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        setExecutionSetting(d, "executable", broken);
        run.start(d, files.path());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 5000);
        QCOMPARE(run.state(), RunState::Failed);
        QVERIFY(!run.message().isEmpty());
#endif
        run.start(configured(), files.path());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 5000);
        QCOMPARE(run.state(), RunState::Finished);
        QVERIFY(run.plan().directory != first);
    }
    void stopTimeoutAndChildProcesses() {
        NwchemRun run;
        run.start(configured("slow"), files.path());
        run.stop(); // Includes the Starting state.
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 5000);
        QCOMPARE(run.state(), RunState::Cancelled);
        auto d = configured("slow");
        setExecutionSetting(d, "timeout_seconds", 1);
        run.start(d, files.path());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 6000);
        QCOMPARE(run.state(), RunState::TimedOut);
        QCOMPARE(readObject(run.plan().directory + "/run.json")["status"].toString(), "timed_out");
#ifdef Q_OS_UNIX
        run.start(configured("tree"), files.path());
        const auto path = run.plan().directory;
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(path + "/child.pid"), 5000);
        const auto child = readFile(path + "/child.pid").trimmed();
        run.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 6000);
        QCOMPARE(run.state(), RunState::Cancelled);
#ifdef Q_OS_LINUX
        const QString proc = "/proc/" + QString::fromLatin1(child) + "/stat";
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(proc) || readFile(proc).contains(") Z"), 3000);
#endif
#endif
        run.start(configured("slow"), files.path());
        run.shutdown();
        QVERIFY(!run.active());
        QCOMPARE(run.state(), RunState::Cancelled);
    }
    void nativeTabUndoEnvironmentCandidatesAndRun() {
        MainWindow w;
        auto d = configured("large");
        w.loadDocument(d);
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        QCOMPARE(tabs->count(), 6);
        QCOMPARE(tabs->tabText(2), "3 実行設定");
        tabs->setCurrentIndex(2);
        QCOMPARE(w.document().toJson(), d.toJson());
        QVERIFY(w.findChild<QPlainTextEdit *>("executionCommand")->isReadOnly());
        w.findChild<QSpinBox *>("execution_memory")->setValue(256);
        QCOMPARE(nwchemSettings(w.document())["memory_mb"].toInt(), 256);
        w.history()->undo(); // Undo is the header's.
        QCOMPARE(w.document().toJson(), d.toJson());
        // Under the field only where a name was found in PATH; a path is in the checks at the right.
        auto *found = w.findChild<QLabel *>("executionFound_executable");
        QVERIFY(found->isHidden());
        if (!QStandardPaths::findExecutable("sh").isEmpty()) {
            input(w, "executable", "sh");
            QVERIFY(!found->isHidden());
            QVERIFY(found->text().startsWith("✓ PATHから: "));
            w.history()->undo();
            QVERIFY(found->isHidden());
        }
        // Runs start from the header (also F5); the checks of this tab say whether they can.
        auto *checks = w.findChild<QLabel *>("executionCheckStatus");
        input(w, "omp_stacksize", "bad");
        QVERIFY(checks->text().startsWith("実行できません"));
        QVERIFY(!w.findChild<QLabel *>("executionStartHint")->isVisibleTo(&w));
        input(w, "omp_stacksize", "64M");
        QVERIFY(checks->text().startsWith("実行できます"));
        button(w, "executionAddEnvironment")->click();
        auto *table = w.findChild<QTableWidget *>("executionEnvironment");
        QCOMPARE(table->rowCount(), 2);
        table->item(1, 1)->setText("CUSTOM_VALUE");
        table->item(1, 2)->setText("a b");
        table->item(1, 0)->setCheckState(Qt::Checked);
        QTRY_COMPARE(
            executionSettings(w.document())["environment"].toArray()[1].toObject()["value"].toString(),
            "a b");
        w.edit("candidate", [](Document &next) { next.atoms[1].position.x() += 0.2; }, true);
        QVERIFY(checks->text().contains("完了待ち"));
        QVERIFY(!button(w, "executionPrepare")->isEnabled());
        w.cancelCandidate();
        QVERIFY(!w.findChild<QPushButton *>("executionStart")); // Only the header's button runs.
        // The executable came with the loaded document, not from Preferences: the first run asks.
        QCOMPARE(w.untrustedExecution(), QStringList{"executable"});
        bool asked = false;
        QTimer::singleShot(0, &w, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box && box->property("mcpProtected").toBool());
            QCOMPARE(box->textFormat(), Qt::PlainText);
            QVERIFY(box->informativeText().contains(helper));
            asked = true;
            box->button(QMessageBox::Yes)->click();
        });
        w.findChild<QToolButton *>("runButton")->click();
        QVERIFY(asked);
        auto *run = w.findChild<NwchemRun *>("nwchemRun");
        QVERIFY(run);
        QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 5000);
        QCOMPARE(run->state(), RunState::Finished);
        // The end is announced, and the job folder loads into the result viewer (tab unchanged).
        QVERIFY(w.findChild<QCheckBox *>("executionOpenResults")->isChecked());
        auto *runStatus = w.findChild<QToolButton *>("runStatusButton");
        QTRY_VERIFY(runStatus->isVisibleTo(&w) && runStatus->text().contains("最後の計算: 終了"));
        // The message at the left does not repeat it (it would add the summary of the log only).
        QVERIFY(!w.statusBar()->currentMessage().contains("計算が終了しました"));
        // The run panel is below the tabs, whatever the tab: it opens on a run and from the status bar.
        auto *panel = w.findChild<QWidget *>("runPanel");
        QVERIFY(panel->isVisibleTo(&w));
        QCOMPARE(w.findChild<QLabel *>("runPanelTitle")->text(),
                 w.findChild<QAction *>("runPanelAction")->text());
        w.findChild<QToolButton *>("runPanelClose")->click();
        QVERIFY(!panel->isVisibleTo(&w));
        tabs->setCurrentIndex(0);
        runStatus->click();
        QVERIFY(panel->isVisibleTo(&w));
        QCOMPARE(tabs->currentIndex(), 0);
        tabs->setCurrentIndex(2);
        // (This test executable is not a result worker, so only the request is checked.)
        QTRY_COMPARE(w.findChild<QComboBox *>("resultsFiles")->currentText(), QString("output.out"));
        QCOMPARE(tabs->currentIndex(), 2);
        auto *log = w.findChild<QPlainTextEdit *>("executionLog");
        QVERIFY(log->toPlainText().contains("log tail marker"));
        QVERIFY(log->toPlainText().size() < 132000);
        QVERIFY(checks->text().startsWith("実行できます"));
        QVERIFY(!w.findChild<QToolButton *>("headerStop")->isVisibleTo(&w));
        button(w, "executionPrepare")->click();
        QVERIFY(w.findChild<QLabel *>("executionState")->text().contains("入力一式"));
    }
    void untrustedSettingsAskBeforeRunning() {
        // What a loaded document names instead of Preferences runs only once the user confirmed it.
        MainWindow w;
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(2);
        w.loadDocument(configured());
        QCOMPARE(w.untrustedExecution(), QStringList{"executable"});
        auto *panel = w.findChild<ExecutionPanel *>("executionTab");
        auto *run = w.findChild<NwchemRun *>("nwchemRun");
        QString shown;
        const auto answer = [&](QMessageBox::StandardButton button) {
            QTimer::singleShot(0, &w, [&, button] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(box);
                shown = box->text();
                box->button(button)->click();
            });
        };
        answer(QMessageBox::No);
        panel->startCalculation();
        QVERIFY(shown.contains("任意のプログラムを起動できます"));
        QVERIFY(!run->active());
        QVERIFY(run->plan().directory.isEmpty());
        // Editing another setting, undo and redo, or loading again does not make it trusted.
        w.edit("environment", [](Document &d) { environment(d, {{"MOLFORMA_UNUSED", "1"}}); });
        w.history()->undo();
        w.history()->redo();
        w.loadDocument(w.document());
        QCOMPARE(w.untrustedExecution(), QStringList{"executable"});
        answer(QMessageBox::Yes);
        panel->startCalculation();
        QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 5000);
        QCOMPARE(run->state(), RunState::Finished);
        QVERIFY(w.untrustedExecution().isEmpty());
        // Values of Preferences and values typed in the window are trusted.
        auto other = configured();
        setExecutionSetting(other, "executable", "nwchem");
        w.loadDocument(other);
        QVERIFY(w.untrustedExecution().isEmpty());
        input(w, "executable", QCoreApplication::applicationFilePath());
        QCOMPARE(executionSettings(w.document())["executable"].toString(),
                 QCoreApplication::applicationFilePath());
        QVERIFY(w.untrustedExecution().isEmpty());
        // A project's other program asks again.
        setExecutionSetting(other, "executable", files.filePath("other engine" EXE));
        w.loadDocument(other);
        QCOMPARE(w.untrustedExecution(), QStringList{"executable"});
    }
    void checksAndProgressParsing() {
        // The checks before a run, one per program, folder and the input.
        const auto report = validateExecution(configured(), files.path(), true);
        QStringList labels, keys;
        for (const auto &c : report.checks) {
            labels << c.label;
            keys << c.key;
            QVERIFY2(c.ok, qPrintable(c.label + ": " + c.detail));
        }
        QCOMPARE(labels,
                 (QStringList{"NWChem実行ファイル", "参照ファイルの基準", "ジョブ保存先", "NWChem入力"}));
        // Each check names its setting; that of the input has none.
        QCOMPARE(keys, (QStringList{"executable", "reference_directory", "job_root", ""}));
        QCOMPARE(report.checks[0].detail, helper);
        auto broken = configured();
        setExecutionSetting(broken, "executable", files.filePath("missing"));
        const auto failed = validateExecution(broken, files.path(), true);
        QVERIFY(!failed.checks[0].ok);
        QVERIFY(failed.errors.contains(failed.checks[0].error));
        // With MPI the launcher has a check of its own, found by its key whatever the language.
        setExecutionSetting(broken, "mode", "mpi");
        setExecutionSetting(broken, "launcher", files.filePath("missing launcher"));
        const auto parallel = validateExecution(broken, files.path(), true);
        const auto check = [&](const QString &key) {
            const auto found = std::find_if(parallel.checks.begin(), parallel.checks.end(),
                                            [&](const ExecutionCheck &c) { return c.key == key; });
            return found == parallel.checks.end() ? ExecutionCheck() : *found;
        };
        QCOMPARE(check("launcher").label, QString("MPIランチャー"));
        QCOMPARE(check("launcher").error, QString("MPIランチャーが見つからないか、実行権限がありません。"));
        QVERIFY(!check("launcher").ok && !check("executable").ok);
        QCOMPARE(check("executable").label, QString("NWChem実行ファイル"));
        // What a running log tells: tasks begun, DRIVER steps against thresholds, the energy.
        NwchemLog progress;
        for (const auto &line :
             QStringList{" NWChem Input Module", " NWChem Input Module",
                         "  maximum gradient threshold         (gmax) =   0.000450",
                         "  rms cartesian step threshold       (xrms) =   0.001200",
                         "@    0     -76.02658720  0.0D+00  0.03036  0.02303  0.00000  0.00000  0.4",
                         "         Total DFT energy =      -76.026587203",
                         "@    1     -76.03010450 -3.5D-03  0.00040  0.00020  0.00500  0.00900  0.9",
                         "@    1     -76.03010450 -3.5D-03  0.00040  0.00020  0.00500  0.00900  0.9",
                         "      Optimization converged"})
            progress.add(line);
        QCOMPARE(progress.started, 1); // A repeated banner starts no task.
        QCOMPARE(progress.optimization().steps.size(), 2);
        QCOMPARE(progress.optimization().steps.last().gmax, 0.0004);
        QCOMPARE(progress.optimization().thresholds["gmax"].toDouble(), 0.00045);
        QCOMPARE(progress.optimization().thresholds["xrms"].toDouble(), 0.0012);
        QCOMPARE(progress.optimization().task, 0);
        QCOMPARE((progress.optimization().converged ? progress.optimization().task : -1), 0);
        QCOMPARE(progress.energy, -76.026587203);
        progress.add(" NWChem Input Module");
        progress.add(" Total times  cpu: 1.0s");
        QCOMPARE(progress.started, 2);
        QVERIFY(progress.ended);
        const auto input =
            QString("driver\n  maxiter 80\nend\ntask dft optimize\ntask dft frequencies numerical\n"
                    "task tddft energy\ntask dplot\n");
        QCOMPARE(nwchemInputStages(input),
                 (QStringList{"構造最適化", "振動数", "励起状態（TDDFT）", "CUBE出力"}));
        QCOMPARE(nwchemInputDriverSteps(input), 80);
        QCOMPARE(nwchemInputDriverSteps("task scf energy\n"), 0);
        // Every operation in its words: those of the choices of "task" where a choice is one task, short
        // names for the paths, and an operation without a name as the input writes it.
        QCOMPARE(nwchemInputStages("task scf\ntask scf energy\ntask dft gradient\ntask mp2 saddle\n"
                                   "task dft freq\ntask dft mepgs\ntask scf neb\ntask dft property\n"
                                   "task esp\ntask tddft optimize\ntask dft raman\n"),
                 (QStringList{"一点エネルギー", "一点エネルギー", "勾配", "遷移状態探索", "振動数", "IRC",
                              "NEB", "物性", "ESP電荷", "励起状態の構造最適化（TDDFT）", "raman"}));
        const auto choice = [](const QString &value) {
            for (const auto &c : nwchemChoices("task"))
                if (c.value == value)
                    return c.label;
            return QString();
        };
        QCOMPARE(nwchemInputStages("task dft optimize\n"), QStringList{choice("optimize")});
        QVERIFY(nwchemInputStages("task scf neb\n") != QStringList{choice("neb")});
        // Only lines as the generator writes them: not indented, in lower case.
        QCOMPARE(nwchemInputStages("  task dft optimize\nTASK dft energy\ntitle \"task\"\n"), QStringList());
        QCOMPARE(nwchemInputDriverSteps("  driver\n  maxiter 5\nend\n"), 0);
    }
    void runningProgressAndHeader() {
        MainWindow w;
        auto d = configured("optimize");
        auto s = nwchemSettings(d);
        s["task"] = "optimize_frequencies";
        d.data["nwchem"] = s;
        w.loadDocument(d);
        w.trustCurrentExecution(); // As if the user confirmed the test engine before the first run.
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        tabs->setCurrentIndex(2);
        w.resize(1380, 900);
        w.show();
        auto *panel = w.findChild<QWidget *>("runPanel");
        QVERIFY(!panel->isVisibleTo(&w));
        // A message hides the summary of the molecule, also as the run status appears beside it.
        auto *summary = w.findChild<QLabel *>("documentSummary");
        w.statusBar()->showMessage("message", 60000);
        QVERIFY(summary->isHidden());
        w.findChild<QToolButton *>("runButton")->click();
        auto *run = w.findChild<NwchemRun *>("nwchemRun");
        QVERIFY(run->active());
        QTest::qWait(50); // The status bar lays out its widgets again.
        QVERIFY(summary->isHidden());
        w.statusBar()->clearMessage();
        QVERIFY(!summary->isHidden());
        // The header shows the run and offers to stop it instead of starting another; the run panel
        // with its progress opens below the tabs.
        auto *pill = w.findChild<QToolButton *>("runStatePill");
        QVERIFY(pill->isVisibleTo(&w));
        QVERIFY(pill->text().startsWith("実行中"));
        QVERIFY(!w.findChild<QToolButton *>("runButton")->isVisibleTo(&w));
        QVERIFY(w.findChild<QToolButton *>("headerStop")->isVisibleTo(&w));
        QVERIFY(w.findChild<QToolButton *>("headerStop")->isEnabled());
        QVERIFY(panel->isVisibleTo(&w));
        QVERIFY(w.findChild<QWidget *>("executionRunCard")->isVisibleTo(&w));
        // The menu follows too: stopping is offered, and "実行状況とログ" is checked with the panel.
        auto *stopAction = w.findChild<QAction *>("stopCalculationAction");
        auto *panelAction = w.findChild<QAction *>("runPanelAction");
        QVERIFY(stopAction->isEnabled());
        QVERIFY(panelAction->isChecked());
        w.findChild<QToolButton *>("runPanelClose")->click();
        QVERIFY(!panel->isVisibleTo(&w));
        QVERIFY(!panelAction->isChecked());
        tabs->setCurrentIndex(0);
        pill->click(); // From any tab.
        QVERIFY(panel->isVisibleTo(&w));
        QVERIFY(panelAction->isChecked());
        // The tasks of the input, the current one with its last step, and the four criteria.
        QTRY_VERIFY(w.findChild<QLabel *>("executionStage0") &&
                    w.findChild<QLabel *>("executionStage0")->text().contains("ステップ 1 / 最大 100"));
        QVERIFY(w.findChild<QLabel *>("executionStage0")->text().contains("構造最適化"));
        QVERIFY(w.findChild<QLabel *>("executionStage1")->text().contains("振動数"));
        QVERIFY(w.findChild<QLabel *>("executionStage1")->text().contains("待機"));
        QVERIFY(w.findChild<QWidget *>("executionCriteria")->isVisibleTo(&w));
        QVERIFY(
            w.findChild<QLabel *>("executionCriterion0")->text().contains("達成")); // Gmax 0.0004 ≤ 0.00045
        QVERIFY(w.findChild<QLabel *>("executionCriterion3")->text().contains("未達")); // Xmax 0.009 > 0.0018
        // The run panel stops it too, asking first like the header's "停止".
        auto *stop = w.findChild<QPushButton *>("executionStopRun");
        QVERIFY(stop->isVisibleTo(&w) && stop->isEnabled());
        QTimer::singleShot(0, &w, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->button(QMessageBox::Yes)->click();
        });
        stop->click();
        QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 10000);
        QVERIFY(!stop->isVisibleTo(&w));
        QVERIFY(!pill->isVisibleTo(&w));
        QVERIFY(!w.findChild<QToolButton *>("headerStop")->isVisibleTo(&w));
        QVERIFY(!stopAction->isEnabled());
        QVERIFY(w.findChild<QToolButton *>("runButton")->isVisibleTo(&w));
        QVERIFY(w.findChild<QLabel *>("executionStage0")->text().contains("未完了"));
        QVERIFY(w.findChild<QLabel *>("executionStatePill")->text().contains("停止"));
    }
    void runPanelKeepsTheTabsInSight() {
        MainWindow w;
        w.loadDocument(configured());
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        auto *split = w.findChild<QSplitter *>("workspaceSplit");
        auto *panel = w.findChild<QWidget *>("runPanel");
        auto *maximize = w.findChild<QToolButton *>("runPanelMaximize");
        w.resize(1380, 900);
        w.show();
        w.executionPanel()->showRunPanel();
        const auto rect = [&](QWidget *widget) { return QRect(widget->mapTo(&w, QPoint()), widget->size()); };
        // Below every tab, at the width of the window: the tab gives up the height, so nothing is
        // covered, and the page keeps the width the window gives it.
        for (int i = 0; i < tabs->count(); ++i) {
            tabs->setCurrentIndex(i);
            QTRY_VERIFY(tabs->currentWidget()->isVisible() && panel->isVisible());
            QVERIFY(!rect(tabs).intersects(rect(panel)));
            QCOMPARE(rect(panel).left(), rect(tabs).left());
            QCOMPARE(panel->width(), tabs->width());
            QCOMPARE(tabs->currentWidget()->width(), w.width());
            QVERIFY(panel->height() >= 200 && tabs->height() >= 400);
        }
        // The whole run with its four criteria fits the panel as it opens.
        QVERIFY(panel->height() >= 290);
        // A dragged height stays as the window changes, and is remembered.
        const int opened = panel->height();
        auto *handle = split->handle(1);
        QTRY_VERIFY(handle->isVisible() && handle->y() == tabs->height());
        const QPointF from = handle->rect().center(), to = from - QPointF(0, 40);
        QMouseEvent press(QEvent::MouseButtonPress, from, handle->mapToGlobal(from), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &press);
        QMouseEvent move(QEvent::MouseMove, to, handle->mapToGlobal(to), Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, to, handle->mapToGlobal(to), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &release);
        QCOMPARE(panel->height(), opened + 40);
        QCOMPARE(w.preference("execution/run_panel_height").toInt(), opened + 40);
        w.resize(1380, 1000);
        QTRY_VERIFY(tabs->height() > 500);
        QCOMPARE(panel->height(), opened + 40);
        w.resize(1380, 900);
        QTRY_COMPARE(panel->height(), opened + 40);
        // It can fill the place of the tabs; asking for a tab gives them their place back.
        tabs->setCurrentIndex(1);
        maximize->click();
        QVERIFY(!tabs->isVisible());
        QTRY_COMPARE(panel->height(), split->height());
        QVERIFY(maximize->toolTip().contains("元の高さ"));
        w.findChild<QToolButton *>("workflowStep_2")->click(); // The tab shown already.
        QVERIFY(tabs->isVisible() && panel->isVisible());
        QCOMPARE(tabs->currentIndex(), 1);
        maximize->click();
        QVERIFY(!tabs->isVisible());
        tabs->setCurrentIndex(0);
        QVERIFY(tabs->isVisible() && panel->isVisible());
        QVERIFY(!maximize->isChecked());
        // Too low a window for both: the panel takes the place of the tabs until one is asked for.
        w.resize(900, 620);
        QTRY_VERIFY(!tabs->isVisible());
        QVERIFY(!maximize->isVisible());
        QTRY_COMPARE(panel->height(), split->height());
        w.resize(1380, 900);
        QTRY_VERIFY(tabs->isVisible());
        QVERIFY(maximize->isVisible());
        w.resize(900, 620);
        QTRY_VERIFY(!tabs->isVisible());
        w.findChild<QToolButton *>("workflowStep_3")->click();
        QVERIFY(tabs->isVisible());
        QVERIFY(!panel->isVisible());
        QCOMPARE(tabs->currentIndex(), 2);
        w.setPreference("execution/run_panel_height", 0);
    }
    void criteriaFitInEachLanguage() {
        for (const bool on : {false, true}) {
            setEnglish(on);
            MainWindow w;
            auto d = configured("optimize");
            auto s = nwchemSettings(d);
            s["task"] = "optimize_frequencies";
            d.data["nwchem"] = s;
            w.loadDocument(d);
            w.trustCurrentExecution(); // As if the user confirmed the test engine before the first run.
            w.resize(1380, 900);
            w.show();
            w.findChild<QToolButton *>("runButton")->click();
            auto *last = w.findChild<QLabel *>("executionCriterion3");
            QTRY_VERIFY(last->isVisible() && last->text().contains("0.00900"));
            QTest::qWait(50); // The tiles are laid out for their text.
            QCOMPARE(last->text().contains("Target"), on);
            auto *details = w.findChild<QScrollArea *>("executionRunDetails");
            auto *folder = w.findChild<QPushButton *>("executionOpenFolder");
            auto *view = details->viewport();
            QVERIFY(last->mapTo(view, QPoint(last->width(), 0)).x() <= view->width());
            QVERIFY(folder->mapTo(view, QPoint(folder->width(), 0)).x() <= view->width());
            auto *run = w.findChild<NwchemRun *>("nwchemRun");
            run->stop();
            QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 10000);
        }
        setEnglish(false);
    }
    void runPanelFollowsTheRun() {
        const auto finish = [](MainWindow &w) {
            w.findChild<QToolButton *>("runButton")->click();
            auto *run = w.findChild<NwchemRun *>("nwchemRun");
            QVERIFY(run->active());
            QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 5000);
        };
        MainWindow w;
        w.loadDocument(configured());
        w.trustCurrentExecution(); // As if the user confirmed the test engine before the first run.
        auto *loadResults = w.findChild<QCheckBox *>("executionOpenResults");
        loadResults->setChecked(false); // This test executable is not a result worker.
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        auto *panel = w.findChild<QWidget *>("runPanel");
        auto *toResults = button(w, "executionResults");
        auto *runStatus = w.findChild<QToolButton *>("runStatusButton");
        w.resize(1380, 900);
        w.show();
        QVERIFY(!toResults->property("primary").toBool());
        // A run that ended well stays in sight until its results are looked at, then the panel
        // closes, and "結果ビューアへ" is the next step.
        finish(w);
        QCOMPARE(w.findChild<NwchemRun *>("nwchemRun")->state(), RunState::Finished);
        QVERIFY(panel->isVisibleTo(&w));
        QVERIFY(toResults->property("primary").toBool());
        tabs->setCurrentIndex(4);
        QVERIFY(!panel->isVisibleTo(&w));
        // Opened again it stays, also on the way back to the viewer.
        runStatus->click();
        QVERIFY(panel->isVisibleTo(&w));
        tabs->setCurrentIndex(0);
        tabs->setCurrentIndex(4);
        QVERIFY(panel->isVisibleTo(&w));
        runStatus->click(); // The same button closes it.
        QVERIFY(!panel->isVisibleTo(&w));
        // Started from the viewer it stays until asked for the results of this run.
        finish(w);
        QVERIFY(panel->isVisibleTo(&w));
        QCOMPARE(tabs->currentIndex(), 4);
        writeFile(QDir(w.executionPanel()->savedDirectory()).filePath("output.out"), "NWChem\n");
        w.executionPanel()->refresh();
        toResults->click();
        QVERIFY(!panel->isVisibleTo(&w));
        // Closed while it ran, it stays closed.
        tabs->setCurrentIndex(0);
        w.findChild<QToolButton *>("runButton")->click();
        w.findChild<QToolButton *>("runPanelClose")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!w.findChild<NwchemRun *>("nwchemRun")->active(), 5000);
        QVERIFY(!panel->isVisibleTo(&w));
        // A failure shows why, also where the panel was closed, and stays open in the viewer.
        MainWindow failing;
        failing.loadDocument(configured("fail"));
        failing.trustCurrentExecution();
        auto *failingPanel = failing.findChild<QWidget *>("runPanel");
        auto *failingTabs = failing.findChild<QTabWidget *>("workspaceTabs");
        auto *channel = failing.findChild<QComboBox *>("executionLogChannel");
        failing.resize(1380, 900);
        failing.show();
        failing.findChild<QToolButton *>("runButton")->click();
        failing.findChild<QToolButton *>("runPanelClose")->click();
        QVERIFY(!failingPanel->isVisibleTo(&failing));
        auto *run = failing.findChild<NwchemRun *>("nwchemRun");
        QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 5000);
        QCOMPARE(run->state(), RunState::Failed);
        QVERIFY(failingPanel->isVisibleTo(&failing));
        QCOMPARE(channel->currentData().toString(), "stderr.err");
        QTRY_VERIFY(
            failing.findChild<QPlainTextEdit *>("executionLog")->toPlainText().contains("fixture stderr"));
        QVERIFY(!failing.findChild<QPushButton *>("executionResults")->property("primary").toBool());
        failingTabs->setCurrentIndex(4);
        QVERIFY(failingPanel->isVisibleTo(&failing));
        // The next run starts on its output again.
        failing.findChild<QToolButton *>("runButton")->click();
        QCOMPARE(channel->currentData().toString(), "output.out");
        QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 5000);
        loadResults->setChecked(true);
    }
    void saveFocusedFieldsAndTeardown() {
        const auto path = files.filePath("settings.molforma.json");
        saveProject(configured(), path);
        MainWindow w;
        w.loadPath(path);
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(2);
        w.show();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        auto *line = w.findChild<QLineEdit *>("execution_omp_stacksize");
        line->setFocus();
        QTest::keyClicks(line, "32M");
        QTest::keyClick(line, Qt::Key_S, Qt::ControlModifier);
        QTRY_COMPARE(executionSettings(Document::fromProject(readObject(path)))["omp_stacksize"].toString(),
                     "32M");
        button(w, "executionAddEnvironment")->click();
        auto *table = w.findChild<QTableWidget *>("executionEnvironment");
        table->item(0, 1)->setText("CUSTOM");
        QCoreApplication::processEvents();
        table->setCurrentCell(0, 2);
        table->editItem(table->item(0, 2));
        auto *editor = qobject_cast<QLineEdit *>(QApplication::focusWidget());
        QVERIFY(editor);
        QTest::keyClicks(editor, "saved via shortcut");
        QTest::keyClick(editor, Qt::Key_S, Qt::ControlModifier);
        QTRY_COMPARE(executionSettings(Document::fromProject(readObject(path)))["environment"]
                         .toArray()[0]
                         .toObject()["value"]
                         .toString(),
                     "saved via shortcut");
        QVERIFY(w.history()->isClean());
        line->setFocus();
        QTest::keyClicks(line, "pending");
    }
    // The built application is the supervisor of calculations that go on without their window.
    static QString supervisor() {
        return QCoreApplication::applicationDirPath() + "/molforma" EXE;
    }
    void supervisedRunEndsAndRecords() {
        NwchemRun run;
        run.setSupervisor(supervisor());
        QVERIFY(run.supervised());
        auto d = configured();
        environment(d, {{"MOLFORMA_LITERAL", "kept ' as is"}});
        run.start(d, files.path());
        QVERIFY(run.active());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 15000);
        QCOMPARE(run.state(), RunState::Finished);
        const auto directory = run.plan().directory;
        const auto record = readObject(directory + "/run.json");
        QCOMPARE(record["status"].toString(), "finished");
        QCOMPARE(record["exit_code"].toInt(), 0);
        QVERIFY(record["supervisor"].toObject()["pid"].toInteger() > 0);
        QVERIFY(record["summary"].isObject());
        QCOMPARE(run.plan().manifest, record);
        QVERIFY(readFile(directory + "/output.out").contains("fake NWChem finished"));
        QCOMPARE(readObject(directory + "/captured.json")["literal"].toString(), "kept ' as is");
        QVERIFY(!QFileInfo::exists(directory + "/stop.request"));
        QCOMPARE(readHistoryEntry(directory).status, RunState::Finished);
        // A failing program and the time limit are the supervisor's to record, too.
        run.start(configured("fail"), files.path());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 15000);
        QCOMPARE(run.state(), RunState::Failed);
        QCOMPARE(run.plan().manifest["exit_code"].toInt(), 7);
        QVERIFY(!run.plan().manifest["diagnostics"].toArray().isEmpty());
        // MCP clients still get the legacy number of a diagnostic's owner beside its target.
        const auto reported = calculationJson(run, {})["diagnostics"].toArray();
        QCOMPARE(reported.size(), run.plan().manifest["diagnostics"].toArray().size());
        for (const auto &value : reported) {
            const auto target = settingTargetFromKey(value.toObject()["target"].toString());
            QVERIFY(target.has_value());
            QCOMPARE(value.toObject()["tab"].toInt(-1), legacySettingTab(*target));
            QVERIFY(!value.toObject()["message"].toString().isEmpty());
        }
        auto limited = configured("slow");
        setExecutionSetting(limited, "timeout_seconds", 1);
        run.start(limited, files.path());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 15000);
        QCOMPARE(run.state(), RunState::TimedOut);
        // A supervisor that cannot be started fails the run where it stands.
        NwchemRun broken;
        broken.setSupervisor(files.filePath("no such program"));
        broken.start(configured(), files.path());
        QCOMPARE(broken.state(), RunState::Failed);
        QCOMPARE(readObject(broken.plan().directory + "/run.json")["status"].toString(), "failed");
        // The supervisor runs only the job that was just prepared for it.
        QProcess again;
        again.start(supervisor(), {"--run-job", directory});
        QVERIFY(again.waitForFinished(15000));
        QCOMPARE(again.exitCode(), 2);
        QCOMPARE(readObject(directory + "/run.json"), record);
    }
    void supervisedRunOutlivesItsWatcher() {
        QString directory;
        {
            NwchemRun run;
            run.setSupervisor(supervisor());
            run.start(configured("slow"), files.path());
            QTRY_COMPARE_WITH_TIMEOUT(run.state(), RunState::Running, 15000);
            directory = run.plan().directory;
            run.release(); // The window closes; the calculation goes on.
            QVERIFY(!run.active());
        }
        QTest::qWait(400);
        auto entry = readHistoryEntry(directory);
        QCOMPARE(entry.status, RunState::Running);
        QVERIFY(!entry.orphaned);
        QVERIFY(runSupervised(entry.manifest));
        // Another session finds it running, follows it and stops it.
        NwchemRun again;
        QVERIFY(again.attach(directory));
        QCOMPARE(again.state(), RunState::Running);
        QCOMPARE(again.plan().directory, directory);
        QVERIFY(again.elapsedSeconds() >= 0);
        again.stop();
        QCOMPARE(again.state(), RunState::Stopping);
        QTRY_VERIFY_WITH_TIMEOUT(!again.active(), 15000);
        QCOMPARE(again.state(), RunState::Cancelled);
        QCOMPARE(readObject(directory + "/run.json")["status"].toString(), "cancelled");
        QVERIFY(!QFileInfo::exists(directory + "/stop.request"));
        QVERIFY(!again.attach(directory)); // Nothing left to follow.
        // Anyone may ask the supervisor of a running job to stop it.
        NwchemRun third;
        third.setSupervisor(supervisor());
        third.start(configured("slow"), files.path());
        QTRY_COMPARE_WITH_TIMEOUT(third.state(), RunState::Running, 15000);
        QVERIFY(requestRunStop(third.plan().directory));
        QTRY_VERIFY_WITH_TIMEOUT(!third.active(), 15000);
        QCOMPARE(third.state(), RunState::Cancelled);
        QVERIFY(!requestRunStop(third.plan().directory));
#ifdef Q_OS_UNIX
        // A supervisor that is killed leaves NWChem running without a keeper: the watcher still sees
        // it, ends it itself, and the record becomes "interrupted".
        NwchemRun orphan;
        orphan.setSupervisor(supervisor());
        orphan.start(configured("slow"), files.path());
        QTRY_COMPARE_WITH_TIMEOUT(orphan.state(), RunState::Running, 15000);
        const auto job = orphan.plan().directory;
        const auto keeper = readObject(job + "/run.json")["supervisor"].toObject()["pid"].toInteger();
        QVERIFY(keeper > 0);
        QCOMPARE(::kill(pid_t(keeper), SIGKILL), 0);
        QTest::qWait(600);
        QVERIFY(orphan.active());
        QVERIFY(!readHistoryEntry(job).orphaned); // NWChem itself is still there.
        orphan.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!orphan.active(), 15000);
        QCOMPARE(orphan.state(), RunState::Interrupted);
        QCOMPARE(readObject(job + "/run.json")["status"].toString(), "interrupted");
#endif
    }
    void jobScriptsForOtherComputers() {
        auto d = configured();
        setExecutionSetting(d, "mode", "mpi");
        setExecutionSetting(d, "launcher", "mpirun");
        setExecutionSetting(d, "executable", "nwchem");
        setExecutionSetting(d, "mpi_ranks", 8);
        setExecutionSetting(d, "omp_threads", 2);
        setExecutionSetting(d, "timeout_seconds", 3 * 3600 + 25 * 60 + 7);
        environment(d, {{"NWCHEM_BASIS_LIBRARY", "/opt/nwchem/libraries"}});
        setNwchemSetting(d, "start", "ethanol");
        // No script unless one is asked for.
        QVERIFY(executionJobScript(d).isEmpty());
        auto plain = saveExecutionBundle(d, files.path());
        QVERIFY(!QFileInfo::exists(plain.directory + "/job.sh"));
        saveExecutionJobPrelude("module load nwchem/7.3.1\nexport SCRATCH=/work/$USER");
        setExecutionSetting(d, "job_script", "slurm");
        setExecutionSetting(d, "job_queue", "short");
        const auto slurm = executionJobScript(d);
        QVERIFY2(slurm.startsWith("#!/bin/bash\n#SBATCH --job-name=ethanol\n#SBATCH --nodes=1\n"
                                  "#SBATCH --ntasks=8\n#SBATCH --cpus-per-task=2\n#SBATCH --time=03:25:07\n"
                                  "#SBATCH --partition=short\n"),
                 qPrintable(slurm));
        QVERIFY(slurm.contains("\nmodule load nwchem/7.3.1\nexport SCRATCH=/work/$USER\n"));
        QVERIFY(slurm.contains("\nexport OMP_NUM_THREADS='2'\n"));
        // A path of this computer is shown, not exported, on the other one.
        QVERIFY(slurm.contains("\n# export NWCHEM_BASIS_LIBRARY='/opt/nwchem/libraries/'\n"));
        QVERIFY(slurm.contains("\n'mpirun' -np 8 'nwchem' input.nw > output.out 2> stderr.err\n"));
        // NWChem's exit code stays beside its output.
        QVERIFY(slurm.endsWith("\nstatus=$?\necho \"$status\" > exit_code\nexit \"$status\"\n"));
        setExecutionSetting(d, "job_script", "pbs");
        const auto pbs = executionJobScript(d);
        QVERIFY2(pbs.contains("#PBS -N ethanol\n#PBS -l select=1:ncpus=16:mpiprocs=8:ompthreads=2\n"
                              "#PBS -l walltime=03:25:07\n#PBS -q short\n#PBS -j oe\n"),
                 qPrintable(pbs));
        QVERIFY(pbs.contains("PBS_O_WORKDIR"));
        // A plain script for a direct start, without limits or a queue.
        setExecutionSetting(d, "job_script", "shell");
        setExecutionSetting(d, "mode", "direct");
        setExecutionSetting(d, "timeout_seconds", 0);
        setExecutionSetting(d, "executable", "/opt/nw chem/bin/nwchem");
        const auto shell = executionJobScript(d);
        QVERIFY(!shell.contains("#SBATCH") && !shell.contains("#PBS"));
        QVERIFY(
            shell.contains("\n'/opt/nw chem/bin/nwchem' input.nw > output.out 2> stderr.err\nstatus=$?\n"));
        // "ジョブとして保存" takes the script along; a run of this computer needs none.
        const auto saved = saveExecutionBundle(d, files.path());
        QCOMPARE(QString::fromUtf8(readFile(saved.directory + "/job.sh")), shell);
        QVERIFY(QFileInfo(saved.directory + "/job.sh").isExecutable());
        QCOMPARE(readObject(saved.directory + "/run.json")["job_script"].toString(), "job.sh");
        historyDocument(saved.directory); // Still the bundle that was saved.
        setExecutionSetting(d, "executable", helper);
        environment(d, {});
        const auto run = saveExecutionBundle(d, files.path(), true);
        QVERIFY(!QFileInfo::exists(run.directory + "/job.sh"));
        // What a script could not carry safely is refused.
        setExecutionSetting(d, "job_queue", "short; rm -rf ~");
        QVERIFY(validateExecution(d, files.path(), false)
                    .errors.join('\n')
                    .contains(tr("キュー・パーティション名")));
        setExecutionSetting(d, "job_queue", "");
        setExecutionSetting(d, "job_script", "lsf");
        QVERIFY(!validateExecution(d, files.path(), false).errors.isEmpty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, saveExecutionJobPrelude(QString(4001, 'x')));
        saveExecutionJobPrelude({});
        // The form of "3 実行設定" edits both; the queue only where a scheduler takes one.
        MainWindow w;
        w.loadDocument(configured());
        auto *kind = w.findChild<QComboBox *>("execution_job_script");
        auto *queue = w.findChild<QLineEdit *>("execution_job_queue");
        QVERIFY(kind && queue);
        QCOMPARE(kind->currentData().toString(), "none");
        QVERIFY(!queue->isEnabled());
        kind->setCurrentIndex(kind->findData("slurm"));
        QCOMPARE(executionSettings(w.document())["job_script"].toString(), "slurm");
        QVERIFY(queue->isEnabled());
        input(w, "job_queue", "long");
        QCOMPARE(executionSettings(w.document())["job_queue"].toString(), "long");
        w.history()->undo();
        w.history()->undo();
        QCOMPARE(kind->currentData().toString(), "none");
    }
    // A remote destination of the tests: this computer through the stand-in ssh and scp, with a folder
    // of its own as the home there and a stand-in Slurm (sbatch, squeue, scancel, sacct) on PATH.
    static RemoteHost destination(const QString &name, const QString &scheduler) {
        RemoteHost h;
        h.name = name;
        h.host = "chemist@cluster.example.org";
        h.directory = "~/molforma-jobs";
        h.scheduler = scheduler;
        h.interval = 10;
        return h;
    }
    bool fakeRemote() {
#ifdef Q_OS_UNIX
        if (!remoteHome.isEmpty())
            return true;
        const auto bin = files.filePath("fake bin");
        remoteHome = files.filePath("remote-home"); // Paths there take no spaces.
        if (!QDir().mkpath(bin) || !QDir().mkpath(remoteHome))
            return false;
        const auto fake = QCoreApplication::applicationDirPath() + "/fake_ssh";
        if (!QFile::link(fake, bin + "/ssh") || !QFile::link(fake, bin + "/scp"))
            return false;
        const QMap<QString, QByteArray> scripts{
            // The job runs at once, in a session of its own; its ID is its PID.
            {"sbatch", "#!/bin/sh\n[ \"$1\" = \"--parsable\" ] && shift\n"
                       "setsid bash \"$1\" > /dev/null 2>&1 < /dev/null &\necho \"$!;fake\"\n"},
            {"squeue", "#!/bin/sh\nwhile [ $# -gt 0 ]; do [ \"$1\" = \"-j\" ] && id=$2; shift; done\n"
                       "if kill -0 \"$id\" 2>/dev/null; then echo RUNNING; fi\n"},
            {"scancel", "#!/bin/sh\ntouch \"$HOME/.cancelled-$1\"\nkill -TERM -- \"-$1\" 2>/dev/null\n"},
            {"sacct",
             "#!/bin/sh\nwhile [ $# -gt 0 ]; do [ \"$1\" = \"-j\" ] && id=$2; shift; done\n"
             "if [ -f \"$HOME/.cancelled-$id\" ]; then echo 'CANCELLED by 1000'; else echo COMPLETED; fi\n"}};
        for (auto it = scripts.begin(); it != scripts.end(); ++it) {
            const auto path = bin + '/' + it.key();
            writeFile(path, it.value());
            QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        qputenv("PATH", (bin + ':' + qEnvironmentVariable("PATH")).toLocal8Bit());
        qputenv("MOLFORMA_FAKE_REMOTE_HOME", remoteHome.toLocal8Bit());
        return true;
#else
        return false;
#endif
    }
    // A calculation for a destination. Its environment rows are not exported there: the mode of the
    // stand-in NWChem goes through the preparation lines of the destination instead.
    Document remoteDocument(const QString &name, const QString &mode = {}) const {
        auto hosts = remoteHosts();
        for (auto &h : hosts)
            if (h.name == name)
                h.prelude = mode.isEmpty() ? QString() : "export MOLFORMA_FAKE_MODE=" + mode;
        saveRemoteHosts(hosts);
        auto d = configured();
        setExecutionSetting(d, "remote", name);
        return d;
    }
    void remoteDestinationsAndTemplates() {
        fakeRemote(); // ssh and scp to be found here (Unix).
        // Destinations are checked before they are kept: nothing a shell or ssh would read as more.
        auto good = destination("cluster", "slurm");
        good.header = "#SBATCH --account=chem\n#SBATCH --mem={memory_mb}M";
        good.prelude = "module load nwchem/7.3.1";
        QVERIFY(remoteHostErrors(good).isEmpty());
        for (const auto &host : {QString("-oProxyCommand=evil"), QString("user@host;rm"), QString()}) {
            auto bad = good;
            bad.host = host;
            QVERIFY2(!remoteHostErrors(bad).isEmpty(), qPrintable(host));
        }
        for (const auto &folder : {QString("../outside"), QString("~/a b"), QString("~/$(id)"), QString()}) {
            auto bad = good;
            bad.directory = folder;
            QVERIFY2(!remoteHostErrors(bad).isEmpty(), qPrintable(folder));
        }
        auto bad = good;
        bad.header = "sbatch --account=chem";
        QVERIFY(!remoteHostErrors(bad).isEmpty());
        bad = good;
        bad.interval = 5;
        QVERIFY(!remoteHostErrors(bad).isEmpty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, saveRemoteHosts({good, good}));
        saveRemoteHosts({good, destination("workstation", "shell")});
        QCOMPARE(remoteHosts().size(), 2);
        QCOMPARE(remoteHosts()[0], good);
        QVERIFY(findRemoteHost("workstation").has_value());
        QVERIFY(!findRemoteHost("nowhere").has_value());

        // The templates of Slurm and PBS headers: the defaults write what job scripts always had.
        QCOMPARE(jobHeaderTemplate("slurm"), defaultJobHeader("slurm"));
        QVERIFY(!jobHeaderErrors("#PBS -l mem={nothing}").isEmpty());
        QVERIFY(!jobHeaderErrors("module load x").isEmpty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, saveJobHeaderTemplate("slurm", "#SBATCH {what}"));
        saveJobHeaderTemplate("slurm", "#SBATCH -J {name}\n#SBATCH -n {ranks}\n#SBATCH -t {time}\n"
                                       "#SBATCH --mem-per-cpu={memory_mb}M\n");
        auto d = configured();
        setExecutionSetting(d, "job_script", "slurm");
        setExecutionSetting(d, "mode", "mpi");
        setExecutionSetting(d, "mpi_ranks", 4);
        setNwchemSetting(d, "memory_mb", 1500);
        auto script = executionJobScript(d);
        QVERIFY2(script.startsWith(
                     "#!/bin/bash\n#SBATCH -J molforma\n#SBATCH -n 4\n#SBATCH --mem-per-cpu=1500M\n"),
                 qPrintable(script)); // No time limit: its line is left out.
        saveJobHeaderTemplate("slurm", defaultJobHeader("slurm"));
        QCOMPARE(QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
                     .contains("execution/job_header_slurm"),
                 false);

        // A run there: its scheduler, the destination's header lines, preparation and programs.
        setExecutionSetting(d, "remote", "cluster");
        auto remote = *findRemoteHost("cluster");
        remote.executable = "/opt/nwchem/bin/nwchem";
        script = executionJobScript(d, &remote);
        QVERIFY2(script.contains("#SBATCH --partition") == false &&
                     script.contains("\n#SBATCH --account=chem\n") &&
                     script.contains("\n#SBATCH --mem=1500M\n") &&
                     script.contains("\nmodule load nwchem/7.3.1\n") &&
                     script.contains("'mpirun' -np 4 '/opt/nwchem/bin/nwchem' input.nw"),
                 qPrintable(script));
        // No NWChem is looked for on this computer; the destination and ssh are.
        auto report = validateExecution(d, files.path());
        QStringList labels, keys;
        for (const auto &check : report.checks) {
            labels << check.label;
            keys << check.key;
        }
        QVERIFY2(labels.contains(tr("リモート実行先")) && !labels.contains(tr("NWChem実行ファイル")),
                 qPrintable(labels.join(", ")));
        QVERIFY2(keys.contains("remote") && !keys.contains("executable") && !keys.contains("launcher"),
                 qPrintable(keys.join(", ")));
        QVERIFY(report.remote.contains("chemist@cluster.example.org"));
        setExecutionSetting(d, "remote", "nowhere");
        QVERIFY(validateExecution(d, files.path(), false).errors.join('\n').contains("nowhere"));
        setExecutionSetting(d, "remote", "cluster");
        setNwchemAdvanced(d, "vectors_guess", "file");
        setNwchemAdvanced(d, "vectors_input", "start.movecs");
        QVERIFY(
            validateExecution(d, files.path(), false).errors.join('\n').contains(tr("入力movecs").left(2)));
        // The computer that runs it is chosen like the program: MCP cannot change it.
        QVERIFY(isProtectedExecutionKey("remote"));
        QCOMPARE(protectedExecution(d)["remote"].toString(), "cluster");
        // A remote run needs a supervisor of its own.
        if (!remoteProgram("ssh").isEmpty() && !remoteProgram("scp").isEmpty()) {
            NwchemRun unsupervised;
            unsupervised.start(remoteDocument("cluster"), files.path());
            QCOMPARE(unsupervised.state(), RunState::Failed);
        }
        // "3 実行設定" chooses it among the destinations of Preferences.
        MainWindow w;
        w.loadDocument(configured());
        auto *choice = w.findChild<QComboBox *>("execution_remote");
        QVERIFY(choice);
        QCOMPARE(choice->count(), 3);
        QVERIFY(choice->property("mcpExecutionSetting").toBool());
        choice->setCurrentIndex(choice->findData("workstation"));
        QCOMPARE(executionSettings(w.document())["remote"].toString(), "workstation");
        QVERIFY(w.findChild<QLabel *>("executionRemoteNote")->isVisibleTo(w.executionPanel()));
        QVERIFY(w.findChild<QPlainTextEdit *>("executionCommand")->toPlainText().contains("nohup ./job.sh"));
        // The destinations are edited in a dialog of their own, which MCP cannot operate.
        {
            RemoteHostsDialog dialog("nwchem", "mpirun");
            QVERIFY(dialog.property("mcpProtected").toBool());
            auto *list = dialog.findChild<QListWidget *>("remoteHostsList");
            QCOMPARE(list->count(), 2);
            dialog.findChild<QPushButton *>("remoteHostsAdd")->click();
            QCOMPARE(list->count(), 3);
            auto *host = dialog.findChild<QLineEdit *>("remoteHostHost");
            auto *problems = dialog.findChild<QLabel *>("remoteHostProblems");
            QTest::keyClicks(host, "a b");
            QVERIFY(!problems->text().isEmpty());
            host->clear();
            QTest::keyClicks(host, "me@hpc.example.org");
            QVERIFY(problems->text().isEmpty());
            auto *scheduler = dialog.findChild<QComboBox *>("remoteHostScheduler");
            scheduler->setCurrentIndex(scheduler->findData("pbs"));
            dialog.accept();
            QCOMPARE(remoteHosts().size(), 3);
            QCOMPARE(remoteHosts()[2].host, "me@hpc.example.org");
            QCOMPARE(remoteHosts()[2].scheduler, "pbs");
        }
        saveRemoteHosts({});
    }
    void remoteRunsOverSsh() {
        if (!fakeRemote())
            QSKIP("The stand-in ssh runs its commands with /bin/sh.");
        auto shell = destination("workstation", "shell");
        shell.executable = helper; // The stand-in NWChem, where the "remote" job runs.
        auto slurm = destination("cluster", "slurm");
        slurm.executable = helper;
        saveRemoteHosts({shell, slurm});
        // In the background on the computer it logs in to.
        NwchemRun run;
        run.setSupervisor(supervisor());
        auto d = remoteDocument("workstation");
        environment(d, {{"MOLFORMA_LITERAL", "not exported there"}});
        run.start(d, files.path());
        QVERIFY(run.active());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 60000);
        QVERIFY2(run.state() == RunState::Finished, QJsonDocument(run.plan().manifest).toJson().constData());
        auto job = run.plan().directory;
        auto record = readObject(job + "/run.json");
        QCOMPARE(record["status"].toString(), "finished");
        QCOMPARE(record["exit_code"].toInt(), 0);
        const auto remote = record["remote"].toObject();
        QCOMPARE(remote["phase"].toString(), "done");
        QCOMPARE(remote["host"].toObject()["name"].toString(), "workstation");
        QVERIFY(remote["job"].toString().toInt() > 0);
        const auto there = remote["directory"].toString();
        QCOMPARE(there, QDir(remoteHome).canonicalPath() + "/molforma-jobs/" + QFileInfo(job).fileName());
        // Only the job went there; the record and the project stayed.
        QVERIFY(QFileInfo::exists(there + "/input.nw") && QFileInfo::exists(there + "/job.sh"));
        QVERIFY(!QFileInfo::exists(there + "/run.json") &&
                !QFileInfo::exists(there + "/project.molforma.json"));
        // The folder came back: the log, what NWChem wrote and its exit code.
        QVERIFY(readFile(job + "/output.out").contains("fake NWChem finished"));
        QCOMPARE(readObject(job + "/captured.json")["cwd"].toString(), there);
        QCOMPARE(readObject(job + "/captured.json")["literal"].toString(), QString());
        QCOMPARE(readFile(job + "/exit_code").trimmed(), QByteArray("0"));
        QVERIFY(!QFileInfo::exists(job + "/.retrieved") && !QFileInfo::exists(job + "/supervisor.lock"));
        QCOMPARE(readHistoryEntry(job).status, RunState::Finished);
        QVERIFY(remoteRunText(record).contains("workstation"));
        historyDocument(job); // The bundle is still the one that was saved.

        // A Slurm job, submitted with sbatch and followed with squeue.
        run.start(remoteDocument("cluster"), files.path());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 60000);
        QCOMPARE(run.state(), RunState::Finished);
        record = readObject(run.plan().directory + "/run.json");
        QVERIFY(
            QRegularExpression("^[0-9]+$").match(record["remote"].toObject()["job"].toString()).hasMatch());
        QVERIFY(readFile(run.plan().directory + "/job.sh").contains("#SBATCH --job-name=molforma"));
        // A failing NWChem fails the run, with its exit code.
        run.start(remoteDocument("cluster", "fail"), files.path());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 60000);
        QCOMPARE(run.state(), RunState::Failed);
        QCOMPARE(run.plan().manifest["exit_code"].toInt(), 7);

        // Stopped while it runs: scancel, and the folder still comes back.
        run.start(remoteDocument("cluster", "slow"), files.path());
        QTRY_VERIFY_WITH_TIMEOUT(run.plan().manifest["remote"].toObject()["phase"].toString() == "submitted",
                                 30000);
        QTRY_VERIFY_WITH_TIMEOUT(
            readFile(run.plan().directory + "/output.out").contains("fake NWChem started"), 30000);
        run.stop();
        QCOMPARE(run.state(), RunState::Stopping);
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 60000);
        QCOMPARE(run.state(), RunState::Cancelled);
        QVERIFY(QFileInfo::exists(run.plan().directory + "/captured.json"));
        QVERIFY(!QFileInfo::exists(run.plan().directory + "/stop.request"));

        // A folder that cannot be made there fails the run before anything is submitted.
        auto hosts = remoteHosts();
        hosts[0].directory = "/proc/molforma-jobs";
        saveRemoteHosts(hosts);
        run.start(remoteDocument("workstation"), files.path());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 30000);
        QCOMPARE(run.state(), RunState::Failed);
        QVERIFY(!run.plan().manifest["remote"].toObject().contains("job"));
        QCOMPARE(run.plan().manifest["diagnostics"].toArray()[0].toObject()["code"].toString(), "launch");
        QVERIFY(remoteRunText(run.plan().manifest).contains("mkdir"));
        saveRemoteHosts({});
    }
    void remoteRunsOutliveTheirSupervisor() {
#ifndef Q_OS_UNIX
        QSKIP("The stand-in ssh runs its commands with /bin/sh.");
#else
        if (!fakeRemote())
            QSKIP("No stand-in ssh.");
        auto shell = destination("workstation", "shell");
        shell.executable = helper;
        saveRemoteHosts({shell});
        QString job;
        {
            NwchemRun run;
            run.setSupervisor(supervisor());
            run.start(remoteDocument("workstation", "slow"), files.path());
            QTRY_VERIFY_WITH_TIMEOUT(
                run.plan().manifest["remote"].toObject()["phase"].toString() == "submitted", 30000);
            job = run.plan().directory;
            run.release();
        }
        // The supervisor ends (the computer restarts): the job goes on there, and the record waits.
        const auto keeper = readObject(job + "/run.json")["supervisor"].toObject()["pid"].toInteger();
        QVERIFY(keeper > 0);
        QCOMPARE(::kill(pid_t(keeper), SIGKILL), 0);
        QTRY_VERIFY_WITH_TIMEOUT(readHistoryEntry(job).orphaned, 10000);
        QCOMPARE(readHistoryEntry(job).status, RunState::Running);
        // Connections fail for a while: the look is retried, and the record says why it waits.
        QTemporaryDir state;
        const auto down = state.filePath("down");
        writeFile(down, {});
        qputenv("MOLFORMA_FAKE_SSH_DOWN", down.toLocal8Bit());
        // Another session follows it again with a new supervisor.
        NwchemRun again;
        again.setSupervisor(supervisor());
        QVERIFY(again.attach(job));
        QVERIFY(again.active());
        QTRY_VERIFY_WITH_TIMEOUT(
            !readObject(job + "/run.json")["remote"].toObject()["message"].toString().isEmpty(), 30000);
        QVERIFY(remoteRunText(readObject(job + "/run.json")).contains("Connection refused"));
        QFile::remove(down);
        QTRY_VERIFY_WITH_TIMEOUT(
            readObject(job + "/run.json")["remote"].toObject()["message"].toString().isEmpty(), 30000);
        QVERIFY(again.active());
        again.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!again.active(), 60000);
        QCOMPARE(again.state(), RunState::Cancelled);
        QCOMPARE(readHistoryEntry(job).status, RunState::Cancelled);
        QVERIFY(!resumeRemoteRun(job, supervisor())); // Nothing left to follow.
        qunsetenv("MOLFORMA_FAKE_SSH_DOWN");
        saveRemoteHosts({});
#endif
    }
    void queueSurvivesTheSession() {
        const auto store = files.filePath("kept queue/queue.json");
        QStringList jobs;
        {
            NwchemRun run;
            run.setSupervisor(supervisor());
            NwchemQueue queue(run);
            queue.setStore(store);
            queue.enqueue(configured("slow"), files.path());
            queue.enqueue(configured(), files.path());
            queue.enqueue(configured(), files.path());
            queue.resume();
            QTRY_COMPARE_WITH_TIMEOUT(run.state(), RunState::Running, 15000);
            for (const auto &entry : queue.entries())
                jobs << entry.plan.directory;
            const auto kept = readObject(store)["entries"].toArray();
            QCOMPARE(kept.size(), 3);
            QVERIFY(kept[0].toObject()["running"].toBool());
            // The session ends: the calculation goes on, the rest waits for the next session.
            queue.shutdown();
            run.release();
        }
        QCOMPARE(readObject(store)["entries"].toArray().size(), 3);
        NwchemRun run;
        run.setSupervisor(supervisor());
        NwchemQueue queue(run);
        queue.setStore(store);
        QCOMPARE(queue.restore(), 3);
        QVERIFY(queue.paused());
        QVERIFY(run.active());
        QCOMPARE(run.plan().directory, jobs[0]);
        QCOMPARE(queue.pending(), 2);
        QCOMPARE(queue.entries()[1].plan.directory, jobs[1]);
        QCOMPARE(queue.restore(), 0); // Taken up once.
        run.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 15000);
        QCOMPARE(queue.entries()[0].state, RunState::Cancelled);
        QTest::qWait(100);
        QVERIFY(!run.active()); // Paused: nothing starts by itself.
        queue.setStopOnFailure(false);
        queue.resume();
        QTRY_VERIFY_WITH_TIMEOUT(queue.pending() == 0 && !run.active(), 20000);
        QCOMPARE(queue.entries()[1].state, RunState::Finished);
        QCOMPARE(queue.entries()[2].state, RunState::Finished);
        QCOMPARE(readHistoryEntry(jobs[2]).status, RunState::Finished);
        QVERIFY(!QFileInfo::exists(store)); // Nothing left to take up.
        // A job that changed since it was registered is not started in another session.
        {
            NwchemRun other;
            NwchemQueue changed(other);
            changed.setStore(store);
            const auto id = changed.enqueue(configured(), files.path());
            QVERIFY(QFileInfo::exists(store));
            const auto job = changed.entries().first().plan.directory;
            auto record = readObject(job + "/run.json");
            record["program"] = "/bin/echo";
            writeFile(job + "/run.json", QJsonDocument(record).toJson());
            changed.shutdown();
        }
        NwchemRun last;
        NwchemQueue rejected(last);
        rejected.setStore(store);
        QCOMPARE(rejected.restore(), 0);
        QVERIFY(!QFileInfo::exists(store));
    }
    void closingLeavesTheRunToItsSupervisor() {
        QString job;
        {
            MainWindow w;
            w.superviseRuns(supervisor());
            QVERIFY(w.runSession().run().supervised());
            w.loadDocument(configured("slow"));
            w.trustCurrentExecution();
            w.show();
            w.executionPanel()->startCalculation();
            QTRY_COMPARE_WITH_TIMEOUT(w.runSession().run().state(), RunState::Running, 15000);
            job = w.runSession().run().plan().directory;
            QCOMPARE(w.preference("execution/running").toString(), job);
            // Cancel keeps the window; "計算を続けて閉じる" leaves the calculation running.
            const auto answer = [&](const char *button) {
                QTimer::singleShot(50, &w, [button] {
                    auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                    if (!box || box->objectName() != "closeWhileRunning")
                        return;
                    if (*button)
                        box->findChild<QAbstractButton *>(button)->click();
                    else
                        box->button(QMessageBox::Cancel)->click();
                });
            };
            answer("");
            QVERIFY(!w.close());
            QVERIFY(w.executionPanel()->running());
            answer("closeKeepRunning");
            QVERIFY(w.close());
        }
        QTest::qWait(300);
        QCOMPARE(readHistoryEntry(job).status, RunState::Running);
        // The next session follows it again, and stops it with the window on request.
        MainWindow w;
        w.superviseRuns(supervisor());
        const auto note = w.restoreRuns();
        QVERIFY2(note.contains(QFileInfo(job).fileName()), qPrintable(note));
        QVERIFY(w.executionPanel()->running());
        QCOMPARE(w.runSession().run().plan().directory, job);
        w.show();
        QTimer::singleShot(50, &w, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->findChild<QAbstractButton *>("closeStopRunning")->click();
        });
        QVERIFY(w.close());
        QCOMPARE(readHistoryEntry(job).status, RunState::Cancelled);
        // Over MCP a job outside its folders (--mcp-allow-dir) is not looked at: it stays for a session
        // that may, as in the headless server.
        w.setPreference("execution/running", job);
        {
            QTemporaryDir allowed;
            McpPolicy policy;
            policy.allowDirectory(allowed.path());
            MainWindow confined;
            confined.setAccessPolicy(&policy);
            confined.superviseRuns(supervisor());
            QVERIFY(confined.restoreRuns(false).isEmpty());
            QVERIFY(!confined.executionPanel()->running());
            QCOMPARE(confined.preference("execution/running").toString(), job);
            confined.setAccessPolicy(nullptr);
        }
        // A run that ended while no session followed it is told once.
        MainWindow third;
        third.superviseRuns(supervisor());
        QVERIFY(third.restoreRuns().contains(runStateName(RunState::Cancelled)));
        QVERIFY(!third.executionPanel()->running());
        QVERIFY(third.preference("execution/running").toString().isEmpty());
    }
    // A log as NWChem writes it: each task starts with the input module, and a structure is a table.
    static QString logTask(const QString &body) {
        return "                NWChem Input Module\n         Total SCF energy =    -74.960000000000\n" +
               body;
    }
    static QString logGeometry(double oz) {
        return QString("   Output coordinates in angstroms (scale by  1.889725989 to convert to a.u.)\n\n"
                       "  No.       Tag          Charge          X              Y              Z\n"
                       " ---- ---------------- ---------- -------------- -------------- --------------\n"
                       "    1 O                    8.0000     0.00000000     0.00000000    %1\n"
                       "    2 H                    1.0000     0.75800000     0.00000000     0.50400000\n"
                       "    3 H                    1.0000    -0.75800000     0.00000000     0.50400000\n\n")
            .arg(oz, 14, 'f', 8);
    }
    // A job that ran and left NWChem's files: the fake engine ends it, the test writes what it left.
    QString leftJob(Document d, const QString &mode, const QString &log, const QStringList &suffixes) {
        environment(d, mode.isEmpty() ? QMap<QString, QString>{}
                                      : QMap<QString, QString>{{"MOLFORMA_FAKE_MODE", mode}});
        NwchemRun run;
        run.start(d, files.path());
        if (!QTest::qWaitFor([&] { return !run.active(); }, 5000))
            return {};
        const auto job = run.plan().directory;
        writeFile(job + "/output.out", log.toUtf8());
        for (const auto &suffix : suffixes)
            writeFile(job + "/molforma." + suffix, ("data " + suffix).toUtf8());
        return job;
    }
    void restartContinuesAnInterruptedJob() {
        auto d = configured();
        setNwchemSetting(d, "task", "optimize_frequencies");
        // The optimization was under way when NWChem ended; the structure had moved.
        const auto job = leftJob(d, "fail", logTask(logGeometry(0) + logGeometry(0.125)),
                                 {"db", "movecs", "drv.hess", "gridpts.0", "aoints.0", "molden"});
        QVERIFY(!job.isEmpty());
        QCOMPARE(readHistoryEntry(job).status, RunState::Failed);
        const auto choices = restartChoices(job);
        QCOMPARE(choices.size(), 3);
        QVERIFY(choices[0].available());
        QVERIFY(!choices[1].available());
        QVERIFY(!choices[2].available());
        QCOMPARE(restartDatabase(job), job + "/molforma.db");
        const auto next = restartDocument(job, "continue");
        QCOMPARE(next.atoms[0].position.z(), 0.125);
        const auto restart = nwchemRestart(nwchemSettings(next));
        QCOMPARE(restart.mode, "continue");
        QCOMPARE(restart.job, job);
        QVERIFY(restart.done.isEmpty());
        const auto check = validateNwchem(next);
        QVERIFY2(check.errors.isEmpty(), qPrintable(check.errors.join('\n')));
        QVERIFY(check.warnings.join('\n').contains(QFileInfo(job).fileName()));
        const auto generated = generateNwchemInput(next);
        const auto input = generated.text;
        QVERIFY(input.contains("\nrestart molforma\n"));
        QVERIFY(!input.contains("\nstart "));
        QVERIFY(!input.contains("geometry"));
        QVERIFY(input.contains("\nbasis"));
        QCOMPARE(nwchemTaskLines(input), QStringList({"task scf optimize", "task scf frequencies"}));
        // The outline still names the lines of its blocks.
        const auto lines = input.split('\n');
        for (const auto &block : generated.outline.blocks)
            QVERIFY2(lines.value(block.line).trimmed().startsWith(block.keyword),
                     qPrintable(block.keyword + ": " + lines.value(block.line)));
        // The database goes into the new job; integrals, grids and viewing files stay behind.
        const auto plan = saveExecutionBundle(next, files.path(), true);
        for (const auto *file : {"molforma.db", "molforma.movecs", "molforma.drv.hess"})
            QCOMPARE(readFile(plan.directory + '/' + file), readFile(job + '/' + file));
        for (const auto *file : {"molforma.gridpts.0", "molforma.aoints.0", "molforma.molden"})
            QVERIFY(!QFileInfo::exists(plan.directory + '/' + file));
        QVERIFY(QString::fromUtf8(readFile(plan.directory + "/input.nw")).contains("restart molforma"));
        QCOMPARE(readObject(plan.directory + "/run.json")["calculation"]
                     .toObject()["restart"]
                     .toObject()["job"]
                     .toString(),
                 job);
        // A restart waits in the queue like any job: the files it brought do not stop its start.
        {
            auto queued = next;
            environment(queued, {});
            NwchemRun run;
            NwchemQueue queue(run);
            queue.enqueue(queued, files.path());
            queue.resume();
            QTRY_VERIFY_WITH_TIMEOUT(queue.pending() == 0 && !run.active(), 10000);
            QVERIFY2(queue.entries().first().state == RunState::Finished,
                     qPrintable(queue.entries().first().error));
            const auto ran = queue.entries().first().plan.directory;
            QCOMPARE(readObject(ran + "/captured.json")["arguments"].toArray().last().toString(), "input.nw");
            QVERIFY(QFileInfo::exists(ran + "/molforma.db"));
        }
        // What the database fixes cannot change; releasing the restart makes it an ordinary job.
        auto moved = next;
        moved.atoms[1].position.x() += .1;
        QVERIFY(validateNwchem(moved).errors.join('\n').contains(tr("再開元と異なります")));
        setNwchemSetting(moved, "restart", QJsonObject());
        QVERIFY(validateNwchem(moved).errors.isEmpty());
        QVERIFY(nwchemInput(moved).contains("\nstart molforma\n"));
        QVERIFY(nwchemInput(moved).contains("geometry"));
        auto renamed = next;
        setNwchemSetting(renamed, "start", "other");
        QVERIFY(validateExecution(renamed, files.path()).errors.join('\n').contains(tr("再開元")));
        auto scan = next;
        setNwchemSetting(scan, "task", "scan");
        QVERIFY(validateNwchem(scan).errors.join('\n').contains(tr("再開できません")));
        auto vectors = next;
        setNwchemAdvanced(vectors, "vectors_guess", "file");
        setNwchemAdvanced(vectors, "vectors_input", "a.movecs");
        setNwchemAdvanced(vectors, "vectors_output", "b.movecs");
        QVERIFY(validateNwchem(vectors).errors.join('\n').contains(tr("入力movecsは指定できません")));
    }
    void restartSkipsFinishedTasks() {
        auto d = configured();
        setNwchemSetting(d, "task", "optimize_frequencies");
        setNwchemSetting(d, "prop_mulliken", true);
        // The optimization ended; the frequencies were under way.
        const auto job =
            leftJob(d, "fail", logTask(logGeometry(0) + logGeometry(0.05)) + logTask(logGeometry(0.05)),
                    {"db", "movecs", "fdrst"});
        QVERIFY(!job.isEmpty());
        const auto next = restartDocument(job, "continue");
        QCOMPARE(nwchemRestart(nwchemSettings(next)).done, QStringList{"task scf optimize"});
        const auto generated = generateNwchemInput(next);
        QCOMPARE(nwchemTaskLines(generated.text), QStringList({"task scf frequencies", "task scf property"}));
        QVERIFY(generated.text.contains("\ndriver\n"));
        const auto lines = generated.text.split('\n');
        for (const auto &block : generated.outline.blocks)
            QVERIFY2(lines.value(block.line).trimmed().startsWith(block.keyword),
                     qPrintable(block.keyword + ": " + lines.value(block.line)));
        QVERIFY(!generated.text.contains("\n\n\n\n"));
        // Tasks that no longer start the calculation cannot be skipped.
        auto other = next;
        setNwchemSetting(other, "task", "frequencies");
        QVERIFY(validateNwchem(other).errors.join('\n').contains(tr("再開元で終わったタスク")));
        auto more = next; // A task added after those that ended is run.
        setNwchemSetting(more, "prop_esp", true);
        QVERIFY(validateNwchem(more).errors.isEmpty());
        QCOMPARE(nwchemTaskLines(nwchemInput(more)),
                 QStringList({"task scf frequencies", "task scf property", "task esp"}));
        // A job whose every task ended has nothing to go on with.
        const auto whole = leftJob(d, "fail",
                                   logTask(logGeometry(0)) + logTask({}) + logTask({}) +
                                       " Total times  cpu:        0.1s     wall:        0.1s\n",
                                   {"db"});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, restartDocument(whole, "continue"));
    }
    // Tasks the user wrote in the additional input are tasks of the job like the generated ones: the log
    // counts them, and a continued job still runs every generated task that did not end.
    void restartCountsTasksOfAdditionalInput() {
        auto d = configured();
        setNwchemSetting(d, "task", "optimize_frequencies");
        setNwchemSetting(d, "extra_input", "set scf:maxiter 60\n  TASK scf energy");
        // The user's task and the optimization ended; the frequencies were under way.
        auto job = leftJob(
            d, "fail", logTask({}) + logTask(logGeometry(0) + logGeometry(0.05)) + logTask(logGeometry(0.05)),
            {"db", "movecs", "fdrst"});
        QVERIFY(!job.isEmpty());
        auto next = restartDocument(job, "continue");
        QCOMPARE(nwchemRestart(nwchemSettings(next)).done,
                 QStringList({"TASK scf energy", "task scf optimize"}));
        auto check = validateNwchem(next);
        QVERIFY2(check.errors.isEmpty(), qPrintable(check.errors.join('\n')));
        // The user's text stays as it is and runs again; the frequencies are not left out with it.
        QCOMPARE(nwchemTaskLines(nwchemInput(next)),
                 QStringList({"TASK scf energy", "task scf frequencies"}));
        // With an initial guess from fragments, whose tasks follow those of the additional input and are
        // not done again.
        auto dimer = configured();
        const auto two = Document::fromXyz("6\ndimer\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n"
                                           "O 0 0 3\nH 0.758 0 3.504\nH -0.758 0 3.504\n");
        dimer.atoms = two.atoms;
        dimer.bonds = two.bonds;
        const auto ids = dimer.ids();
        QJsonArray fragments;
        for (const auto &atoms : {ids.mid(0, 3), ids.mid(3)})
            fragments << QJsonObject{{"atoms", idsToJson(atoms)}, {"charge", 0}, {"multiplicity", 1}};
        setNwchemSetting(dimer, "fragment_guess", fragments);
        setNwchemSetting(dimer, "task", "optimize_frequencies");
        setNwchemSetting(dimer, "extra_input", "task scf energy");
        QCOMPARE(nwchemTaskLines(nwchemInput(dimer)),
                 QStringList({"task scf energy", "task scf energy", "task scf energy", "task scf optimize",
                              "task scf frequencies"}));
        // The user's task, both fragments and the optimization ended.
        job = leftJob(dimer, "fail", logTask({}) + logTask({}) + logTask({}) + logTask({}) + logTask({}),
                      {"db", "movecs", "fdrst"});
        QVERIFY(!job.isEmpty());
        next = restartDocument(job, "continue");
        QCOMPARE(nwchemRestart(nwchemSettings(next)).done,
                 QStringList({"task scf energy", "task scf optimize"}));
        check = validateNwchem(next);
        QVERIFY2(check.errors.isEmpty(), qPrintable(check.errors.join('\n')));
        QCOMPARE(nwchemTaskLines(nwchemInput(next)),
                 QStringList({"task scf energy", "task scf frequencies"}));
    }
    void restartFromAHessian() {
        auto d = configured();
        setNwchemSetting(d, "task", "optimize_frequencies");
        setNwchemSetting(d, "prop_mulliken", true);
        d.atoms[1].isotope = 2;
        const auto log = logTask(logGeometry(0) + logGeometry(0.03)) + logTask(logGeometry(0.03)) +
                         " Total times  cpu:        0.1s     wall:        0.1s\n";
        const auto job = leftJob(d, {}, log, {"db", "movecs", "hess", "drv.hess"});
        QVERIFY(!job.isEmpty());
        QCOMPARE(readHistoryEntry(job).status, RunState::Finished);
        const auto choices = restartChoices(job);
        QVERIFY(!choices[0].available());
        QVERIFY(choices[1].available());
        QVERIFY(choices[2].available());
        // A transition-state search with the Hessian of the frequencies, not the optimizer's own.
        const auto saddle = restartDocument(job, "hessian");
        QVERIFY2(validateNwchem(saddle).errors.isEmpty(),
                 qPrintable(validateNwchem(saddle).errors.join('\n')));
        const auto search = nwchemInput(saddle);
        QVERIFY(search.contains("\nrestart molforma\n"));
        QVERIFY(search.contains("\ndriver\n  maxiter 100\n  default\n  clear\n  inhess 2\n"));
        QCOMPARE(nwchemTaskLines(search), QStringList{"task scf saddle"});
        QVERIFY(!search.contains("geometry"));
        auto followed = saddle;
        setNwchemSetting(followed, "task", "saddle_frequencies");
        QCOMPARE(nwchemTaskLines(nwchemInput(followed)),
                 QStringList({"task scf saddle", "task scf frequencies"}));
        auto wrong = saddle;
        setNwchemSetting(wrong, "task", "optimize");
        QVERIFY(validateNwchem(wrong).errors.join('\n').contains(tr("遷移状態探索")));
        // Frequencies and thermochemistry again: other temperatures and isotopes, no new Hessian.
        auto reuse = restartDocument(job, "reuse");
        setNwchemAdvanced(reuse, "freq_temperatures", QJsonArray{298.15, 350.});
        reuse.atoms[2].isotope = 2;
        reuse.atoms[1].isotope = 0;
        reuse.atoms[1].mass = 3.5;
        QVERIFY2(validateNwchem(reuse).errors.isEmpty(), qPrintable(validateNwchem(reuse).errors.join('\n')));
        const auto again = nwchemInput(reuse);
        QVERIFY2(again.contains("\nfreq\n  reuse\n  mass 2 3.5\n  mass 3 2.014101778\n"), qPrintable(again));
        QVERIFY(again.contains("temp 2 298.15 350"));
        QCOMPARE(nwchemTaskLines(again), QStringList{"task scf frequencies"});
        reuse.atoms[0].position.z() += .2;
        QVERIFY(validateNwchem(reuse).errors.join('\n').contains(tr("同位体・質量だけ")));
        const auto plan = saveExecutionBundle(saddle, files.path(), true);
        QCOMPARE(readFile(plan.directory + "/molforma.hess"), QByteArray("data hess"));
    }
    void orbitalsOfAJobStartAnother() {
        const auto job = leftJob(configured(), {}, logTask(logGeometry(0.2)), {"db", "movecs"});
        QVERIFY(!job.isEmpty());
        const auto next = orbitalReuseDocument(job);
        const auto a = nwchemAdvanced(next);
        QCOMPARE(a["vectors_guess"].toString(), "file");
        QCOMPARE(a["vectors_input"].toString(), job + "/molforma.movecs");
        QCOMPARE(next.atoms[0].position.z(), 0.2);
        QVERIFY(!nwchemRestart(nwchemSettings(next)).active());
        QVERIFY2(validateNwchem(next).errors.isEmpty(), qPrintable(validateNwchem(next).errors.join('\n')));
        const auto plan = saveExecutionBundle(next, files.path(), true);
        QVERIFY(
            plan.input.contains("vectors input \"" + job + "/molforma.movecs\" output \"molforma.movecs\""));
        QFile::remove(job + "/molforma.movecs");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, orbitalReuseDocument(job));
    }
    void realEngineArtifacts() {
        const auto root = qEnvironmentVariable("MOLFORMA_EXECUTION_ARTIFACTS");
        if (root.isEmpty())
            return;
        QVERIFY(QDir().mkpath(root));
        auto d = water();
        setExecutionSetting(d, "executable",
                            qEnvironmentVariable("MOLFORMA_EXECUTION_ENGINE", "/usr/bin/nwchem"));
        setExecutionSetting(d, "job_root", root);
        auto s = nwchemSettings(d);
        s["memory_mb"] = 256;
        d.data["nwchem"] = s;
        MainWindow w;
        w.loadDocument(d);
        w.trustCurrentExecution(); // As if the user confirmed the engine before the first run.
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(2);
        w.resize(1380, 1000);
        w.show();
        auto *panel = w.findChild<ExecutionPanel *>("executionTab");
        panel->startCalculation();
        auto *run = w.findChild<NwchemRun *>("nwchemRun");
        QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 90000);
        QCOMPARE(run->state(), RunState::Finished);
        QVERIFY(readFile(run->plan().directory + "/output.out").contains("Total SCF energy"));
        QCOMPARE(w.document().toJson(), d.toJson());
        writeFile(root + "/latest-directory.txt", run->plan().directory.toUtf8());
        QCoreApplication::processEvents();
        QVERIFY(w.grab().save(root + "/execution.png"));
        // Exercise cancellation against the real MPI launcher after NWChem has started computing.
        auto longer = d;
        auto calculation = nwchemSettings(longer);
        calculation["start"] = "cancel_check";
        calculation["method"] = "dft";
        calculation["basis"] = "cc-pvtz";
        calculation["task"] = "optimize_frequencies";
        longer.data["nwchem"] = calculation;
        w.loadDocument(longer);
        w.trustCurrentExecution();
        panel->startCalculation();
        const auto cancelledDirectory = run->plan().directory;
        const auto output = cancelledDirectory + "/output.out";
        QTRY_VERIFY_WITH_TIMEOUT(
            !run->active() || (QFileInfo::exists(output) && readFile(output).contains("NWChem DFT Module")),
            30000);
        QVERIFY(run->active());
        QTimer::singleShot(0, &w, [] { // The stop button asks first.
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->button(QMessageBox::Yes)->click();
        });
        w.findChild<QToolButton *>("headerStop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 10000);
        QCOMPARE(run->state(), RunState::Cancelled);
#ifdef Q_OS_LINUX
        auto childrenGone = [&] {
            for (const auto &pid : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                bool numeric;
                pid.toInt(&numeric);
                if (numeric && QFileInfo("/proc/" + pid + "/cwd").symLinkTarget() == cancelledDirectory)
                    return false;
            }
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(childrenGone(), 10000);
#endif
    }
};
QTEST_MAIN(ExecutionTest)
#include "test_execution.moc"
