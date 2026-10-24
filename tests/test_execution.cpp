#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "ui/execution_panel.h"
#include "ui/main_window.h"
#include <QtTest>
#include <QtWidgets>
using namespace mf;

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
        helper = files.filePath("native engine");
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/fake_nwchem", helper));
        QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    void defaultsAndCompatibility() {
        auto d = water();
        d.envelope["execution"] = QJsonObject{{"kagami_exe", "keep"}, {"future", 12}};
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
        const auto xyz = files.filePath("preferred.xyz"), project = files.filePath("own.kgui.json");
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
        QCOMPARE(Document::fromProject(readObject(first.directory + "/project.kgui.json")).toJson(),
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
        QVERIFY_EXCEPTION_THROWN(executionPlan(d, files.path(), p.directory), std::runtime_error);
        QCOMPARE(readFile(files.filePath("seed orbitals.movecs")), QByteArray("seed"));
        for (const auto &bad : {"../outside.movecs", "input.nw", "run.json", "stderr.err"}) {
            setNwchemAdvanced(d, "vectors_output", bad);
            QVERIFY_EXCEPTION_THROWN(executionPlan(d, files.path(), p.directory), std::runtime_error);
        }
        setNwchemAdvanced(d, "vectors_output", "new.movecs");
        setNwchemAdvanced(d, "vectors_input", "absent.movecs");
        QVERIFY_EXCEPTION_THROWN(executionPlan(d, files.path(), p.directory), std::runtime_error);
        setNwchemAdvanced(d, "vectors_input", "seed orbitals.movecs");
        setNwchemAdvanced(d, "permanent_dir", files.path());
        writeFile(files.filePath("molforma.db"), "previous");
        QVERIFY_EXCEPTION_THROWN(executionPlan(d, files.path(), p.directory), std::runtime_error);
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
        QVERIFY_EXCEPTION_THROWN(run.start(d, files.path()), std::runtime_error);
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 5000);
        QCOMPARE(run.state(), "finished");
        QCOMPARE(QString::fromUtf8(readFile(directory + "/input.nw")), input);
        const auto captured = readObject(directory + "/captured.json");
        QCOMPARE(captured["arguments"].toArray(), QJsonArray{"input.nw"});
        QCOMPARE(captured["cwd"].toString(), directory);
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
        QCOMPARE(run.state(), "failed");
        QCOMPARE(run.plan().manifest["exit_code"].toInt(), 7);
        const auto first = run.plan().directory;
#ifdef Q_OS_UNIX
        const auto broken = files.filePath("broken executable");
        writeFile(broken, "#!/nonexistent/molforma-interpreter\n");
        QVERIFY(QFile::setPermissions(broken, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        setExecutionSetting(d, "executable", broken);
        run.start(d, files.path());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 5000);
        QCOMPARE(run.state(), "failed");
        QVERIFY(!run.message().isEmpty());
#endif
        run.start(configured(), files.path());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 5000);
        QCOMPARE(run.state(), "finished");
        QVERIFY(run.plan().directory != first);
    }
    void stopTimeoutAndChildProcesses() {
        NwchemRun run;
        run.start(configured("slow"), files.path());
        run.stop(); // Includes the Starting state.
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 5000);
        QCOMPARE(run.state(), "cancelled");
        auto d = configured("slow");
        setExecutionSetting(d, "timeout_seconds", 1);
        run.start(d, files.path());
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 6000);
        QCOMPARE(run.state(), "timed_out");
        QCOMPARE(readObject(run.plan().directory + "/run.json")["status"].toString(), "timed_out");
#ifdef Q_OS_UNIX
        run.start(configured("tree"), files.path());
        const auto path = run.plan().directory;
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(path + "/child.pid"), 5000);
        const auto child = readFile(path + "/child.pid").trimmed();
        run.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 6000);
        QCOMPARE(run.state(), "cancelled");
#ifdef Q_OS_LINUX
        const QString proc = "/proc/" + QString::fromLatin1(child) + "/stat";
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(proc) || readFile(proc).contains(") Z"), 3000);
#endif
#endif
        run.start(configured("slow"), files.path());
        run.shutdown();
        QVERIFY(!run.active());
        QCOMPARE(run.state(), "cancelled");
    }
    void nativeTabUndoEnvironmentCandidatesAndRun() {
        MainWindow w;
        auto d = configured("large");
        w.loadDocument(d);
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        QCOMPARE(tabs->count(), 8);
        QCOMPARE(tabs->tabText(4), "5 実行設定");
        tabs->setCurrentIndex(4);
        QCOMPARE(w.document().toJson(), d.toJson());
        QVERIFY(w.findChild<QPlainTextEdit *>("executionCommand")->isReadOnly());
        w.findChild<QSpinBox *>("execution_memory")->setValue(256);
        QCOMPARE(nwchemSettings(w.document())["memory_mb"].toInt(), 256);
        button(w, "executionUndo")->click();
        QCOMPARE(w.document().toJson(), d.toJson());
        input(w, "omp_stacksize", "bad");
        QVERIFY(!button(w, "executionStart")->isEnabled());
        input(w, "omp_stacksize", "64M");
        QVERIFY(button(w, "executionStart")->isEnabled());
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
        QVERIFY(!button(w, "executionStart")->isEnabled());
        QVERIFY(!button(w, "executionPrepare")->isEnabled());
        w.cancelCandidate();
        button(w, "executionStart")->click();
        auto *run = w.findChild<NwchemRun *>("nwchemRun");
        QVERIFY(run);
        QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 5000);
        QCOMPARE(run->state(), "finished");
        auto *log = w.findChild<QPlainTextEdit *>("executionLog");
        QVERIFY(log->toPlainText().contains("log tail marker"));
        QVERIFY(log->toPlainText().size() < 132000);
        QVERIFY(button(w, "executionStart")->isEnabled());
        QVERIFY(!button(w, "executionStop")->isEnabled());
        button(w, "executionPrepare")->click();
        QVERIFY(w.findChild<QLabel *>("executionState")->text().contains("入力一式"));
    }
    void saveFocusedFieldsAndTeardown() {
        const auto path = files.filePath("settings.kgui.json");
        configured().save(path);
        MainWindow w;
        w.loadPath(path);
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(4);
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
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(4);
        w.resize(1380, 1000);
        w.show();
        auto *panel = w.findChild<ExecutionPanel *>("executionTab");
        panel->startCalculation();
        auto *run = w.findChild<NwchemRun *>("nwchemRun");
        QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 90000);
        QCOMPARE(run->state(), "finished");
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
        panel->startCalculation();
        const auto cancelledDirectory = run->plan().directory;
        const auto output = cancelledDirectory + "/output.out";
        QTRY_VERIFY_WITH_TIMEOUT(
            !run->active() || (QFileInfo::exists(output) && readFile(output).contains("NWChem DFT Module")),
            30000);
        QVERIFY(run->active());
        button(w, "executionStop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 10000);
        QCOMPARE(run->state(), "cancelled");
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
