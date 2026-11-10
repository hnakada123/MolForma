// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/i18n.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/run_diagnostics.h"
#include "results/results.h"
#include "ui/execution_panel.h"
#include "ui/main_window.h"
#include <QtTest>
#include <QtWidgets>
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
    QString helper;
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
                           settings.path()); // MolForma uses NativeFormat.
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
        QCOMPARE(w.executionPanel()->calculationQueue().pending(), 2);
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
        own.save(project);
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
        QStringList labels;
        for (const auto &c : report.checks) {
            labels << c.label;
            QVERIFY2(c.ok, qPrintable(c.label + ": " + c.detail));
        }
        QCOMPARE(labels,
                 (QStringList{"NWChem実行ファイル", "参照ファイルの基準", "ジョブ保存先", "NWChem入力"}));
        QCOMPARE(report.checks[0].detail, helper);
        auto broken = configured();
        setExecutionSetting(broken, "executable", files.filePath("missing"));
        const auto failed = validateExecution(broken, files.path(), true);
        QVERIFY(!failed.checks[0].ok);
        QVERIFY(failed.errors.contains(failed.checks[0].error));
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
        QCOMPARE(progress.steps.size(), 2);
        QCOMPARE(progress.steps.last().gmax, 0.0004);
        QCOMPARE(progress.thresholds["gmax"].toDouble(), 0.00045);
        QCOMPARE(progress.thresholds["xrms"].toDouble(), 0.0012);
        QCOMPARE(progress.stepTask, 0);
        QCOMPARE(progress.convergedTask, 0);
        QCOMPARE(progress.energy, -76.026587203);
        progress.add(" NWChem Input Module");
        progress.add(" Total times  cpu: 1.0s");
        QCOMPARE(progress.started, 2);
        QVERIFY(progress.ended);
        const auto input =
            QString("driver\n  maxiter 80\nend\ntask dft optimize\ntask dft frequencies numerical\n"
                    "task tddft energy\ntask dplot\n");
        QCOMPARE(NwchemLog::tasks(input),
                 (QStringList{"構造最適化", "振動数", "励起状態（TDDFT）", "CUBE出力"}));
        QCOMPARE(NwchemLog::driverSteps(input), 80);
        QCOMPARE(NwchemLog::driverSteps("task scf energy\n"), 0);
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
        QVERIFY(panel->isVisibleTo(&w));
        QVERIFY(w.findChild<QWidget *>("executionRunCard")->isVisibleTo(&w));
        w.findChild<QToolButton *>("runPanelClose")->click();
        QVERIFY(!panel->isVisibleTo(&w));
        tabs->setCurrentIndex(0);
        pill->click(); // From any tab.
        QVERIFY(panel->isVisibleTo(&w));
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
        configured().save(path);
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
