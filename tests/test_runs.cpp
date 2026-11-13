// SPDX-License-Identifier: GPL-2.0-or-later
#include "foundation/files.h"
#include "model/settings.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_execution.h"
#include "nwchem/nwchem_plan.h"
#include "nwchem/nwchem_queue.h"
#include "nwchem/nwchem_restart.h"
#include "results/results.h"
#include "run_support.h"
#include <QtTest>
#include <algorithm>
using namespace mf;

// Runs without a window: the launch settings and their checks, plans and saved job bundles, the
// processes of a run (of the stand-in NWChem), the queue, restarts from a job's files, and the history
// read from job folders.
class RunsTest : public QObject, RunFixture {
    Q_OBJECT
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
  private slots:
    void initTestCase() {
        QVERIFY(files.isValid());
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settings.path()); // MolForma uses NativeFormat.
        qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
        qputenv("XDG_DATA_HOME", settings.filePath("data").toUtf8()); // The stored queue of sessions.
        copyEngine();
    }
    // Every test starts without saved preferences.
    void init() {
        userPreferences().clear();
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
    void snapshotsLegacyAndMovedDirectories() {
        QTemporaryDir tmp;
        auto d = water(tmp.filePath("jobs"));
        auto p = saveExecutionBundle(d, tmp.path());
        auto entry = readHistoryEntry(p.directory);
        QVERIFY(entry.valid);
        QCOMPARE(entry.status, RunState::Prepared);
        QCOMPARE(entry.method, "Hartree–Fock");
        QCOMPARE(entry.basis, "STO-3G");
        QCOMPARE(entry.name, QString("molforma"));
        QVERIFY(historyIntegrity(entry).contains("入力・プロジェクトのSHA-256一致"));
        // Under the default calculation name, the job is listed by the user's title; a chosen name wins.
        {
            auto titled = d;
            auto s = nwchemSettings(titled);
            s["title"] = "water HF";
            titled.data["nwchem"] = s;
            QCOMPARE(readHistoryEntry(saveExecutionBundle(titled, tmp.path()).directory).name,
                     QString("water HF"));
            s["start"] = "water_hf";
            titled.data["nwchem"] = s;
            QCOMPARE(readHistoryEntry(saveExecutionBundle(titled, tmp.path()).directory).name,
                     QString("water_hf"));
        }
        const auto molecule = historyMolecule(p.directory);
        QCOMPARE(molecule.atoms, 3);
        QCOMPARE(molecule.charge, 0);
        QCOMPARE(molecule.multiplicity, 1);
        QCOMPARE(historyJobCount(tmp.filePath("jobs")), 3);
        QCOMPARE(historyJobCount(tmp.filePath("missing")), -1);
        const auto before = readFile(p.directory + "/project.molforma.json");
        auto next = historyDocument(p.directory);
        QCOMPARE(next.atoms[0].id, d.atoms[0].id);
        QVERIFY(next.atoms[0].fixed);
        QCOMPARE(next.envelope["future"], d.envelope["future"]);
        QCOMPARE(nwchemSettings(next), nwchemSettings(d));
        QCOMPARE(executionSettings(next)["reference_directory"].toString(), tmp.path());
        QCOMPARE(readFile(p.directory + "/project.molforma.json"), before);
        p.manifest.remove("calculation");
        p.manifest.remove("project_sha256");
        p.manifest.remove("project_directory");
        manifest(p, "finished", 9);
        entry = readHistoryEntry(p.directory);
        QVERIFY(entry.valid);
        QCOMPARE(entry.seconds, 9);
        QCOMPARE(entry.basis, "STO-3G");
        QVERIFY(historyIntegrity(entry).contains("旧記録"));
        const auto moved = tmp.filePath("moved job");
        QVERIFY(QDir().rename(p.directory, moved));
        next = historyDocument(moved);
        QCOMPARE(executionSettings(next)["job_root"].toString(), tmp.path());
        QCOMPARE(readHistoryEntry(moved).directory, moved); // Never follow the original manifest directory.
        QCOMPARE(readFile(moved + "/project.molforma.json"), before);
    }
    void hashesBoundsMissingAndMalformed() {
        QTemporaryDir tmp;
        auto p = saveExecutionBundle(water(tmp.path()), tmp.path());
        const auto input = readFile(p.directory + "/input.nw");
        writeFile(p.directory + "/input.nw", input + "# manually changed\n");
        QVERIFY(historyIntegrity(readHistoryEntry(p.directory)).contains("一致しません"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, historyDocument(p.directory));
        writeFile(p.directory + "/input.nw", input);
        const auto project = readFile(p.directory + "/project.molforma.json");
        writeFile(p.directory + "/project.molforma.json", project + " ");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, historyDocument(p.directory));
        writeFile(p.directory + "/project.molforma.json", project);
        p.manifest["input"] = "../outside.nw";
        manifest(p, "prepared");
        QVERIFY(!readHistoryEntry(p.directory).valid);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, historyDocument(p.directory));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, historyText(p.directory, "../input.nw"));
        writeFile(p.directory + "/run.json", "{");
        QCOMPARE(readHistoryEntry(p.directory).status, RunState::Invalid);
        writeFile(p.directory + "/run.json", QByteArray(256 * 1024 + 1, ' '));
        QVERIFY(!readHistoryEntry(p.directory).valid);
        QVERIFY(!readHistoryEntry(tmp.filePath("missing")).valid);
        writeFile(p.directory + "/input.nw", QByteArray(8 * 1024 * 1024 + 1, 'a'));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, historyText(p.directory, "input.nw"));
#ifdef Q_OS_UNIX
        QVERIFY(QFile::remove(p.directory + "/input.nw"));
        QVERIFY(QFile::link(p.directory + "/project.molforma.json", p.directory + "/input.nw"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, historyText(p.directory, "input.nw"));
#endif
    }
    void relativePathsAndExternalReferences() {
        QTemporaryDir tmp;
        auto d = water(tmp.filePath("jobs"));
        setExecutionSetting(d, "reference_directory", "orbitals");
        setExecutionSetting(d, "executable", "bin/nwchem");
        setNwchemAdvanced(d, "vectors_guess", "file");
        setNwchemAdvanced(d, "vectors_input", "guess.movecs");
        setNwchemAdvanced(d, "vectors_output", "new.movecs");
        auto p = saveExecutionBundle(d, tmp.path());
        auto next = historyDocument(p.directory);
        QCOMPARE(executionSettings(next)["executable"].toString(), tmp.filePath("bin/nwchem"));
        QCOMPARE(executionSettings(next)["reference_directory"].toString(), tmp.filePath("orbitals"));
        const auto preview = executionPreviewPlan(next, tmp.filePath("elsewhere"));
        QVERIFY(preview.input.contains(tmp.filePath("orbitals/guess.movecs")));
        p.manifest.remove("project_directory");
        p.manifest["program"] = "bin/nwchem";
        manifest(p, "prepared");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, historyDocument(p.directory));
        p.manifest["program"] = tmp.filePath("bin/nwchem");
        manifest(p, "finished");
        next = historyDocument(p.directory);
        QCOMPARE(executionSettings(next)["executable"].toString(), tmp.filePath("bin/nwchem"));
    }
    void logEnergiesAndCsv() {
        QTemporaryDir tmp;
        auto p = saveExecutionBundle(water(tmp.path()), tmp.path());
        writeFile(
            p.directory + "/output.out",
            QByteArray(150 * 1024, 'x') +
                "\n Total SCF energy = -7.5D+01\n Total MP2 energy -75.1\n Total CCSD(T) energy: -75.2\n"
                " Total DFT energy = -999"); // Partial final line must not become an energy.
        auto e = readHistoryEntry(p.directory);
        QVERIFY(e.energy.has_value());
        QCOMPARE(*e.energy, -75.2);
        QCOMPARE(e.energyMethod, "CCSD(T)");
        auto log = historyText(p.directory, "output.out", true);
        QVERIFY(log.size() < 132000);
        QVERIFY(log.contains("末尾128"));
        e.name = "=cmd,\"name\"\nline";
        e.seconds = 12;
        auto csv = historyCsv({e});
        QVERIFY(csv.contains("\"'=cmd,\"\"name\"\"\nline\""));
        QVERIFY(csv.contains("\"-75.2\""));
        QVERIFY(historyStatus(RunState::Running).contains("稼働未確認"));
        QCOMPARE(historyStatus(RunState::Running, true), "実行中");
        QVERIFY(!historyStatus(RunState::Finished).contains("成功"));
        // run.json records the states by their keys.
        for (auto state : {RunState::Prepared, RunState::Starting, RunState::Running, RunState::Stopping,
                           RunState::Finished, RunState::Failed, RunState::Cancelled, RunState::TimedOut,
                           RunState::Interrupted})
            QCOMPARE(runStateFromKey(runStateKey(state)), state);
        QCOMPARE(runStateKey(RunState::TimedOut), "timed_out");
        QCOMPARE(runStateFromKey("idle"), RunState::Invalid);
        QCOMPARE(runStateFromKey("done"), RunState::Invalid);
    }
    void comparableEnergies() {
        HistoryEntry a;
        a.formula = "H2O";
        a.method = "Hartree–Fock";
        a.basis = "STO-3G";
        a.energyMethod = "SCF";
        auto b = a;
        QVERIFY(historyIncomparable(a, b).isEmpty());
        b.formula = "H4O2";
        QVERIFY(historyIncomparable(a, b).contains("組成"));
        b = a;
        b.method = "DFT / B3LYP";
        QVERIFY(historyIncomparable(a, b).contains("手法"));
        b = a;
        b.basis = "6-31G*";
        QVERIFY(historyIncomparable(a, b).contains("基底"));
        b = a;
        b.energyMethod = "SCF・BSSE補正"; // A counterpoise-corrected energy is another quantity.
        QVERIFY(historyIncomparable(a, b).contains("エネルギーの種類"));
        b = a;
        a.formula.clear();
        b.formula.clear();
        QVERIFY(historyIncomparable(a, b).contains("組成")); // Unknown compositions never compare.
    }
    void logSummaryOfTasks() {
        QTemporaryDir tmp;
        const auto log = tmp.filePath("output.out");
        // The last task's last energy; correlated methods print theirs after the SCF.
        writeFile(log, " NWChem Input Module\n Total SCF energy = -75.0\n NWChem Input Module\n"
                       " Total SCF energy = -75.5\n Total MP2 energy = -75.7\n NWChem Input Module\n");
        auto s = nwchemLogSummary(log);
        QCOMPARE(s["energy"].toDouble(), -75.7);
        QCOMPARE(s["energy_method"].toString(), "MP2");
        QCOMPARE(s["energy_source"].toString(), "final");
        QVERIFY(!s["normal_termination"].toBool());
        // Counterpoise: the corrected energy, not the last monomer's.
        writeFile(log, " NWChem Input Module\n Total SCF energy = -149.9277\n Total SCF energy = -74.9644\n"
                       "  Supermolecular energy =    -149.9277\n       Corrected energy =    -149.9276\n"
                       " Total times  cpu: 1.0s\n");
        s = nwchemLogSummary(log);
        QCOMPARE(s["energy"].toDouble(), -149.9276);
        QCOMPARE(s["energy_source"].toString(), "bsse_corrected");
        QVERIFY(s["normal_termination"].toBool());
        writeFile(log, "no energies\n");
        QVERIFY(!nwchemLogSummary(log).contains("energy"));
    }
    void scanningCancellationAndInvalidRows() {
        QTemporaryDir tmp;
        const auto p = saveExecutionBundle(water(tmp.path()), tmp.path());
        QVERIFY(QDir().mkpath(tmp.filePath("bad")));
        writeFile(tmp.filePath("bad/run.json"), "not JSON");
        QVERIFY(QDir().mkpath(tmp.filePath("unrelated/nested")));
        writeFile(tmp.filePath("unrelated/nested/run.json"), QJsonDocument(p.manifest).toJson());
#ifdef Q_OS_UNIX
        QVERIFY(QFile::link(p.directory, tmp.filePath("symlink")));
#endif
        HistoryScan scan;
        QSignalSpy done(&scan, &HistoryScan::finished);
        scan.start(tmp.path());
        scan.cancel();
        QTest::qWait(30);
        QCOMPARE(done.count(), 0);
        scan.start(tmp.path());
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(scan.entries.size(), 2);
        QVERIFY(std::any_of(scan.entries.cbegin(), scan.entries.cend(),
                            [](const auto &e) { return e.status == RunState::Invalid; }));
        scan.start(tmp.filePath("missing"));
        QCOMPARE(done.count(), 2);
        QVERIFY(scan.entries.isEmpty());
        QVERIFY(!scan.message.isEmpty());
        scan.start(tmp.path());
        scan.start(tmp.filePath("bad"));
        QTRY_COMPARE(done.count(), 3);
        QVERIFY(scan.entries.isEmpty()); // No stale rows from the superseded root.
    }
    void rescansReadOnlyChangedJobs() {
        // A scan keeps the readings of ended jobs whose files are unchanged; a note or a rewritten
        // record is read again.
        QTemporaryDir tmp;
        auto p = saveExecutionBundle(water(tmp.path()), tmp.path());
        manifest(p, "finished", 3);
        HistoryScan scan;
        QSignalSpy done(&scan, &HistoryScan::finished);
        scan.start(tmp.path());
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(scan.entries.size(), 1);
        QCOMPARE(scan.entries[0].seconds, 3);
        scan.start(tmp.path());
        QTRY_COMPARE(done.count(), 2);
        QCOMPARE(scan.entries[0].seconds, 3);
        QTest::qWait(5);
        setHistoryMemo(p.directory, "second look");
        scan.start(tmp.path());
        QTRY_COMPARE(done.count(), 3);
        QCOMPARE(scan.entries[0].memo, QString("second look"));
        QTest::qWait(5);
        manifest(p, "finished", 7);
        scan.start(tmp.path());
        QTRY_COMPARE(done.count(), 4);
        QCOMPARE(scan.entries[0].seconds, 7);
        QCOMPARE(scan.entries[0].memo, QString("second look"));
    }
    void trashCompletedBundle() {
#ifdef Q_OS_LINUX
        QTemporaryDir tmp;
        const auto saved = qgetenv("XDG_DATA_HOME");
        struct Restore {
            QByteArray value;
            ~Restore() {
                if (value.isNull())
                    qunsetenv("XDG_DATA_HOME");
                else
                    qputenv("XDG_DATA_HOME", value);
            }
        } restore{saved};
        const auto data = tmp.filePath("data");
        QVERIFY(QDir().mkpath(data));
        qputenv("XDG_DATA_HOME", data.toUtf8());
        QCOMPARE(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation), data);
        const auto p = saveExecutionBundle(water(tmp.filePath("jobs")), tmp.path());
        trashHistoryEntry(p.directory, tmp.filePath("jobs"), {});
        QVERIFY(!QFileInfo::exists(p.directory));
        const auto files = QDir(data + "/Trash/files").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(files.size(), 1);
        QCOMPARE(readFile(data + "/Trash/files/" + files.first() + "/input.nw"), p.input.toUtf8());
#endif
    }
};
int main(int argc, char **argv) {
    if (const int code = resultWorkerMain(argc, argv); code >= 0)
        return code;
    QCoreApplication app(argc, argv);
    RunsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_runs.moc"
