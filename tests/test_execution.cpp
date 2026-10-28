#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
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
        w.findChild<QToolButton *>("runButton")->click();
        auto *run = w.findChild<NwchemRun *>("nwchemRun");
        QVERIFY(run);
        QTRY_VERIFY_WITH_TIMEOUT(!run->active(), 5000);
        QCOMPARE(run->state(), "finished");
        // The end is announced, and the job folder loads into the result viewer (tab unchanged).
        QVERIFY(w.findChild<QCheckBox *>("executionOpenResults")->isChecked());
        auto *runStatus = w.findChild<QToolButton *>("runStatusButton");
        QTRY_VERIFY(runStatus->isVisibleTo(&w) && runStatus->text().contains("最後の計算: 終了"));
        // The message at the left does not repeat it (it would add the summary of the log only).
        QVERIFY(!w.statusBar()->currentMessage().contains("計算が終了しました"));
        // The run panel is over the tabs, whatever the tab: it opens on a run and from the status bar.
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
        NwchemProgress progress;
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
        QCOMPARE(NwchemProgress::tasks(input),
                 (QStringList{"構造最適化", "振動数", "励起状態（TDDFT）", "CUBE出力"}));
        QCOMPARE(NwchemProgress::driverSteps(input), 80);
        QCOMPARE(NwchemProgress::driverSteps("task scf energy\n"), 0);
    }
    void runningProgressAndHeader() {
        MainWindow w;
        auto d = configured("optimize");
        auto s = nwchemSettings(d);
        s["task"] = "optimize_frequencies";
        d.data["nwchem"] = s;
        w.loadDocument(d);
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
        // with its progress opens over the tabs.
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
        const QRect area(tabs->mapTo(&w, QPoint()), tabs->size());
        QVERIFY(area.contains(QRect(panel->mapTo(&w, QPoint()), panel->size())));
        // Over "1 分子" it takes the place of the pages at the right: the view stays in sight.
        const QRect view(w.canvas()->mapTo(&w, QPoint()), w.canvas()->size());
        QVERIFY(!QRect(panel->mapTo(&w, QPoint()), panel->size()).intersects(view));
        tabs->setCurrentIndex(2);
        QVERIFY(panel->width() >= 420);
        // Over "3 実行設定" it starts below the bar, so what to do when a run ends stays in sight,
        // also as a long calculation wraps the bar.
        auto *toolbar = w.findChild<QWidget *>("executionToolbar");
        const auto below = [&] {
            const QRect bar(toolbar->mapTo(&w, QPoint()), toolbar->size());
            const QRect over(panel->mapTo(&w, QPoint()), panel->size());
            return !over.intersects(bar) && over.top() == bar.bottom() + 1 && over.bottom() == area.bottom();
        };
        QTRY_VERIFY(below());
        QVERIFY(w.findChild<QCheckBox *>("executionOpenResults")->isVisibleTo(&w));
        const int barHeight = toolbar->height();
        auto *context = w.findChild<QLabel *>("executionContext");
        const auto calculation = context->text();
        context->setText(calculation.repeated(12));
        QTRY_VERIFY(toolbar->height() > barHeight);
        QTRY_VERIFY(below());
        context->setText(calculation);
        QTRY_COMPARE(toolbar->height(), barHeight);
        QTRY_VERIFY(below());
        tabs->setCurrentIndex(0);
        QTRY_COMPARE(panel->mapTo(&w, QPoint()).y(), tabs->mapTo(&w, QPoint()).y());
        // The tasks of the input, the current one with its last step, and the four criteria.
        QTRY_VERIFY(w.findChild<QLabel *>("executionStage0") &&
                    w.findChild<QLabel *>("executionStage0")->text().contains("ステップ 1 / 最大 100"));
        QVERIFY(w.findChild<QLabel *>("executionStage0")->text().contains("構造最適化"));
        QVERIFY(w.findChild<QLabel *>("executionStage1")->text().contains("振動数"));
        QVERIFY(w.findChild<QLabel *>("executionStage1")->text().contains("待機"));
        QVERIFY(w.findChild<QWidget *>("executionCriteria")->isVisibleTo(&w));
        QVERIFY(
            w.findChild<QLabel *>("executionCriterion0")->text().contains("達成")); // Gmax 0.0004 ≤ 0.00045
        QVERIFY(w.findChild<QLabel *>("executionCriterion2")->text().contains("未達")); // Xmax 0.009 > 0.0018
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
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(2);
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
        QTimer::singleShot(0, &w, [] { // The stop button asks first.
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->button(QMessageBox::Yes)->click();
        });
        w.findChild<QToolButton *>("headerStop")->click();
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
