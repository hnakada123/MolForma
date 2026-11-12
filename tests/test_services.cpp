// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/chemistry_job.h"
#include "foundation/files.h"
#include "io/document_storage.h"
#include "io/pdb_download.h"
#include "io/pubchem.h"
#include "model/settings.h"
#include "nwchem/conformer_set.h"
#include "nwchem/history.h"
#include "nwchem/nwchem_execution.h"
#include "nwchem/run_diagnostics.h"
#include "nwchem/run_record.h"
#include "nwchem/run_session.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QThread>
#include <QtTest>

using namespace mf;
// Executables carry .exe on Windows.
#ifdef Q_OS_WIN
#define EXE ".exe"
#else
#define EXE ""
#endif
class ServicesTest : public QObject {
    Q_OBJECT
    QTemporaryDir home;
    // Runs the stand-in xtb (fake_xtb) on a document and returns the note of its result, or the error.
    struct XtbRun {
        bool ready = false;
        QString note, error;
        QJsonObject captured;
        QString control;
    };
    XtbRun runXtb(EditorSession &session, const XtbOptions &options, const QString &mode = {}) {
        QTemporaryDir keep;
        qputenv("MOLFORMA_FAKE_XTB_KEEP", keep.path().toUtf8());
        qputenv("MOLFORMA_FAKE_XTB", mode.toUtf8());
        ChemistryJob job(session);
        XtbRun run;
        connect(&job, &ChemistryJob::ready, this, [&](const ChemistryResult &result) {
            run.ready = true;
            run.note = result.note;
            job.propose(result);
        });
        connect(&job, &ChemistryJob::failed, this,
                [&](const ChemistryRequest &, const QString &message, bool) { run.error = message; });
        ChemistryRequest request{"xtb"};
        request.executable = xtbExecutable(QCoreApplication::applicationDirPath() + "/fake_xtb" EXE);
        request.xtb = options;
        job.start(request);
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!job.active(), 15000); }();
        run.captured = QJsonDocument::fromJson(readFile(keep.filePath("captured.json"))).object();
        if (QFileInfo::exists(keep.filePath("xcontrol")))
            run.control = QString::fromUtf8(readFile(keep.filePath("xcontrol")));
        qunsetenv("MOLFORMA_FAKE_XTB");
        return run;
    }

  private slots:
    void initTestCase() {
        QVERIFY(home.isValid());
        qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
        qputenv("XDG_DATA_HOME", home.filePath("data").toUtf8());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, home.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, home.path());
    }
    void conformerSetWithoutWidgets() {
        auto base = Document::fromXyz("3\nwater\nO 0 0 0\nH .8 0 .6\nH -.8 0 .6\n");
        auto moved = base;
        moved.atoms[1].position.x() += .3;
        ConformerSet set;
        set.setConformers(base, {{base, 0, "MMFF94"}, {moved, 1, "MMFF94"}});
        auto saved = set.specification();
        saved["future"] = QJsonObject{{"keep", true}};
        auto entries = saved["entries"].toArray();
        auto row = entries[1].toObject();
        row["future_entry"] = "keep";
        entries[1] = row;
        saved["entries"] = entries;
        set.setSpecification(saved);
        QCOMPARE(set.specification(), saved);
        // An invalid load must leave the entire current set intact.
        auto bad = saved;
        bad["rmsd"] = -1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, set.setSpecification(bad));
        QCOMPARE(set.specification(), saved);
        bad["rmsd"] = .1;
        bad["entries"] = QJsonArray{QJsonObject{{"geometry", "1\nwrong\nHe 0 0 0\n"}}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, set.setSpecification(bad));
        QCOMPARE(set.specification(), saved);
        set.setSelected(0, false);
        set.setName(1, "second");
        auto options = set.options();
        options.frequencies = true;
        set.setOptions(options);
        const auto beforeQueue = set.specification();
        set.queueSelected([](const QVector<Document> &, bool) { return QStringList{}; });
        QCOMPARE(set.specification(), beforeQueue);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, set.queueSelected([](const QVector<Document> &, bool) {
            return QStringList{"a", "b"};
        }));
        QCOMPARE(set.specification(), beforeQueue);
        QVector<Document> queued;
        bool frequencies = false;
        set.queueSelected([&](const QVector<Document> &docs, bool freq) {
            queued = docs;
            frequencies = freq;
            return QStringList{"second-job"};
        });
        QVERIFY(frequencies);
        QCOMPARE(queued.size(), 1);
        QVERIFY((queued[0].atoms[1].position - moved.atoms[1].position).norm() < 1e-9);
        QCOMPARE(queued[0].ids(), base.ids());
        QVERIFY(!set.entries()[0].toObject().contains("job"));
        QCOMPARE(set.entries()[1].toObject()["job"].toString(), "second-job");
        QCOMPARE(set.entries()[1].toObject()["future_entry"].toString(), "keep");
        QCOMPARE(set.selectedXyz(), set.geometry(1).xyz().toUtf8());
    }
    void conformerResultsAndCancellation() {
        const auto result = readResult(QString(FIXTURE_DIR) + "/nwchem/water_optfreq.out");
        auto base = result.frames.last().molecule;
        setExecutionSetting(base, "job_root", home.filePath("conformer-jobs"));
        auto job = saveExecutionBundle(base, home.path());
        const QDir dir(job.directory);
        writeFile(dir.filePath("output.out"), readFile(result.source));
        job.manifest["status"] = "finished";
        job.manifest["exit_code"] = 0;
        writeFile(dir.filePath("run.json"), QJsonDocument(job.manifest).toJson());
        ConformerSet set;
        set.setConformers(base, {{base, 0, "MMFF94"}});
        set.queueSelected([&](const QVector<Document> &, bool) { return QStringList{job.directory}; });
        QStringList checked;
        set.refreshResults([&](const QString &path) { checked << path; });
        QTRY_VERIFY_WITH_TIMEOUT(!set.loading(), 15000);
        QCOMPARE(checked.size(), 5);
        QVERIFY(checked.contains(dir.filePath("output.out")));
        QVERIFY2(!set.entries()[0].toObject()["result"].toObject().isEmpty(),
                 qPrintable(set.entries()[0].toObject()["error"].toString()));
        const auto analysis = set.analyze();
        QCOMPARE(analysis.rows.size(), 1);
        QCOMPARE(analysis.rows[0].toObject()["entry"].toInt(), 0);
        QCOMPARE(analysis.rows[0].toObject()["population"].toDouble(), 1.);
        QVERIFY(analysis.spectra[0].toObject()["spectra"].toObject().contains("ir"));
        // A refused path must invalidate previously cached results, without reading its files.
        set.refreshResults([](const QString &) { fail("denied"); });
        QTRY_VERIFY(!set.loading());
        QCOMPARE(set.entries()[0].toObject()["error"].toString(), "denied");
        QVERIFY(set.analyze().rows.isEmpty());
        // Neither a cancelled worker nor a queued next read may change a newly loaded set.
        set.refreshResults();
        QVERIFY(set.loading());
        set.cancel();
        set.setConformers(base, {{base, 7, "new"}});
        const auto fresh = set.specification();
        QCoreApplication::processEvents();
        QCOMPARE(set.specification(), fresh);
        set.queueSelected([](const QVector<Document> &, bool) { return QStringList{"missing"}; });
        set.refreshResults(); // Schedules a next read for a missing/unfinished job.
        set.setConformers(base, {{base, 8, "last"}});
        const auto last = set.specification();
        QCoreApplication::processEvents();
        QCOMPARE(set.specification(), last);
        QVERIFY(!set.loading());
    }
    // A job saved without running, for the tests of its record.
    ExecutionPlan savedJob(const QTemporaryDir &jobs) {
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
        setExecutionSetting(d, "mode", "direct");
        setExecutionSetting(d, "executable", QCoreApplication::applicationFilePath());
        setExecutionSetting(d, "job_root", jobs.filePath("jobs"));
        setExecutionSetting(d, "timeout_seconds", 90);
        setExecutionSetting(
            d, "environment",
            QJsonArray{QJsonObject{{"enabled", true}, {"name", "MOLFORMA_LITERAL"}, {"value", "kept"}}});
        return saveExecutionBundle(d, jobs.path());
    }
    // The record of a job has one reader and one writer: what is written is read back, each reader gets
    // what it asks for, and a record that is a link is neither replaced nor taken for a job.
    void runRecordRoundTripAndChecks() {
        QTemporaryDir jobs;
        const auto plan = savedJob(jobs);
        const auto job = plan.directory;
        for (const auto *name : {JobFile::record, JobFile::project, JobFile::input})
            QVERIFY2(QFileInfo(jobFile(job, name)).isFile(), name);
        QCOMPARE(jobFile(job, JobFile::record), job + "/run.json");
        for (const auto check : {RecordCheck::Json, RecordCheck::Supported, RecordCheck::Complete})
            QCOMPARE(readRunRecord(job, check), plan.manifest);
        // Every record names its format and the files of its folder.
        const auto fresh = newRunRecord();
        for (auto it = fresh.begin(); it != fresh.end(); ++it)
            QCOMPARE(plan.manifest[it.key()], it.value());
        QCOMPARE(fresh["format"].toString(), QString(RunRecord::format));
        QCOMPARE(fresh["stdout"].toString(), QString(JobFile::output));
        QVERIFY(RunRecord(plan.manifest).supported());
        QCOMPARE(RunRecord(plan.manifest).state(), RunState::Prepared);
        // A written record is the bytes of its JSON, with the keys this code does not know.
        auto record = plan.manifest;
        record["future"] = QJsonObject{{"keep", true}};
        writeRunRecord(job, record);
        const auto bytes = QJsonDocument(record).toJson();
        QCOMPARE(readFile(jobFile(job, JobFile::record)), bytes);
        QCOMPARE(readRunRecord(job, RecordCheck::Complete), record);
        QCOMPARE(runRecordDigest(job),
                 QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        QVERIFY(readHistoryEntry(job).valid);
        // Another version is a record only for the processes of its own run.
        auto other = record;
        other["version"] = 2;
        writeRunRecord(job, other);
        QCOMPARE(readRunRecord(job, RecordCheck::Json), other);
        QVERIFY(RunRecord(other).hasFormat() && !RunRecord(other).supported());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readRunRecord(job, RecordCheck::Supported));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readRunRecord(job, RecordCheck::Complete));
        // The history asks more: a finished run has ended with code 0.
        other = record;
        other["status"] = "finished";
        writeRunRecord(job, other);
        QCOMPARE(readRunRecord(job, RecordCheck::Supported), other);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readRunRecord(job, RecordCheck::Complete));
        other["stdout"] = "elsewhere.out";
        other["exit_code"] = 0;
        writeRunRecord(job, other);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readRunRecord(job, RecordCheck::Complete));
        // A record larger than the history reads still serves the run it belongs to.
        other = record;
        other["padding"] = QString(256 * 1024, 'x');
        writeRunRecord(job, other);
        QCOMPARE(readRunRecord(job, RecordCheck::Json), other);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readRunRecord(job, RecordCheck::Supported));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readRunRecord(job, RecordCheck::Complete));
        writeFile(jobFile(job, JobFile::record), "{");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readRunRecord(job, RecordCheck::Json));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runRecordDigest(jobs.filePath("missing")));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 readRunRecord(jobs.filePath("missing"), RecordCheck::Json));
#ifdef Q_OS_UNIX
        // A record that is a symbolic link: not written through, and no job of the history.
        const auto elsewhere = jobs.filePath("elsewhere.json");
        writeFile(elsewhere, bytes);
        QVERIFY(QFile::remove(jobFile(job, JobFile::record)));
        QVERIFY(QFile::link(elsewhere, jobFile(job, JobFile::record)));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, writeRunRecord(job, plan.manifest));
        QCOMPARE(readFile(elsewhere), bytes);
        QVERIFY(QFileInfo(jobFile(job, JobFile::record)).isSymLink());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readRunRecord(job, RecordCheck::Complete));
        QVERIFY(!readHistoryEntry(job).valid);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readJobFile(job, JobFile::record, 1024 * 1024));
#endif
    }
    // The end of a run is recorded in one place, with what its caller knows of it.
    void runEndIsRecordedOnce() {
        QTemporaryDir jobs;
        const auto plan = savedJob(jobs);
        const auto job = plan.directory;
        const auto ended = QDateTime::fromString("2026-01-02T03:04:05.678Z", Qt::ISODateWithMs);
        // A finished run: the state, how the process ended and when, and the summary of its whole log.
        writeFile(jobFile(job, JobFile::output), "                NWChem Input Module\n"
                                                 "         Total DFT energy =      -76.380000000000\n"
                                                 " Total times  cpu:       12.3s     wall:       45.6s\n");
        writeFile(jobFile(job, JobFile::errors), {});
        auto record = plan.manifest;
        recordRunEnd(record, job,
                     {.state = RunState::Finished,
                      .exitCode = 0,
                      .normalExit = true,
                      .finished = ended,
                      .elapsedSeconds = 12,
                      .error = QString()});
        auto expected = plan.manifest;
        expected["status"] = "finished";
        expected["exit_code"] = 0;
        expected["exit_status"] = "normal";
        expected["finished_at"] = "2026-01-02T03:04:05.678Z";
        expected["elapsed_seconds"] = 12;
        expected["error"] = "";
        expected["summary"] = QJsonObject{{"energy", -76.38},
                                          {"energy_method", "DFT"},
                                          {"energy_source", "final"},
                                          {"normal_termination", true}};
        expected["diagnostics"] = QJsonArray();
        QCOMPARE(record, expected);
        writeRunRecord(job, record);
        const auto entry = readHistoryEntry(job);
        QVERIFY2(entry.valid, qPrintable(entry.warnings.join('\n')));
        QCOMPARE(entry.status, RunState::Finished);
        QCOMPARE(*entry.energy, -76.38);
        QCOMPARE(entry.seconds, 12);
        // A failed run: its exit code, and what the ends of its output and errors say went wrong.
        writeFile(jobFile(job, JobFile::output), "  dft energy failed\n");
        writeFile(jobFile(job, JobFile::errors), "ma_alloc_get: failed\n");
        record = plan.manifest;
        recordRunEnd(record, job,
                     {.state = RunState::Failed,
                      .exitCode = 7,
                      .normalExit = false,
                      .finished = ended,
                      .elapsedSeconds = 3,
                      .error = QString("起動できません"),
                      .launchError = "no such program"});
        QCOMPARE(record["status"].toString(), "failed");
        QCOMPARE(record["exit_code"].toInt(), 7);
        QCOMPARE(record["exit_status"].toString(), "crash");
        QCOMPARE(record["error"].toString(), "起動できません");
        QCOMPARE(record["summary"].toObject(), (QJsonObject{{"normal_termination", false}}));
        auto diagnostics = RunRecord(record).diagnostics();
        QCOMPARE(diagnostics.size(), 3);
        QCOMPARE(diagnostics[0].code, "launch");
        QCOMPARE(diagnostics[0].setting, "executable");
        QCOMPARE(diagnostics[0].evidence, "no such program");
        QCOMPARE(diagnostics[1].code, "scf_convergence");
        QCOMPARE(diagnostics[1].source, "stdout");
        QCOMPARE(diagnostics[2].code, "memory");
        QCOMPARE(diagnostics[2].source, "stderr");
        QCOMPARE(record["diagnostics"].toArray(), diagnoseRun(RunState::Failed, "  dft energy failed\n",
                                                              "ma_alloc_get: failed\n", "no such program"));
        // A run that did not start on another computer points at its destination.
        recordRunEnd(record, job,
                     {.state = RunState::Failed, .launchError = "unreachable", .launchSetting = "remote"});
        QCOMPARE(RunRecord(record).diagnostics()[0].setting, "remote");
        QCOMPARE(RunRecord(record).diagnostics()[1].setting, "scf_maxiter");
        QCOMPARE(record["exit_code"].toInt(), 7); // What this caller did not give stays.
        // What no one saw end: the state and the summary, and nothing else (a repaired record).
        record = plan.manifest;
        recordRunEnd(record, job, {.state = RunState::Interrupted, .diagnose = false});
        expected = plan.manifest;
        expected["status"] = "interrupted";
        expected["summary"] = QJsonObject{{"normal_termination", false}};
        QCOMPARE(record, expected);
        // A log brought from elsewhere: its summary as read, and the wall time NWChem printed.
        writeFile(jobFile(job, JobFile::output), " Total times  cpu:       12.3s     wall:       45.6s\n");
        record = plan.manifest;
        const QJsonObject summary{{"normal_termination", true}, {"given", true}};
        recordRunEnd(record, job,
                     {.state = RunState::Finished, .summary = summary, .wallTime = true, .needsLog = true});
        QCOMPARE(record["summary"].toObject(), summary);
        QCOMPARE(record["elapsed_seconds"].toInt(), 45);
        QVERIFY(!record.contains("finished_at") && !record.contains("exit_code"));
        // Without a log there is no summary; only one who needs the log is refused.
        QVERIFY(QFile::remove(jobFile(job, JobFile::output)));
        record = expected;
        recordRunEnd(record, job, {.state = RunState::Cancelled});
        QVERIFY(!record.contains("summary"));
        QCOMPARE(record["status"].toString(), "cancelled");
        QCOMPARE(RunRecord(record).diagnostics().size(), 1); // The errors are still there.
        QCOMPARE(RunRecord(record).diagnostics()[0].code, "memory");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 recordRunEnd(record, job, {.state = RunState::Finished, .needsLog = true}));
    }
    // A record gives the plan it was saved from, and says what the panels and MCP show of it.
    void runRecordPlanAndTypedAccess() {
        QTemporaryDir jobs;
        const auto saved = savedJob(jobs);
        const auto plan =
            runRecordPlan(saved.directory, readRunRecord(saved.directory, RecordCheck::Complete));
        QCOMPARE(plan.directory, saved.directory);
        QCOMPARE(plan.program, saved.program);
        QCOMPARE(plan.arguments, saved.arguments);
        QCOMPARE(plan.arguments, QStringList{JobFile::input});
        QCOMPARE(plan.environment, saved.environment);
        QCOMPARE(plan.environment.value("MOLFORMA_LITERAL"), QString("kept"));
        QCOMPARE(plan.timeoutSeconds, 90);
        QCOMPARE(plan.manifest, saved.manifest);
        QVERIFY(plan.input.isEmpty()); // The input stays in its file.
        // A saved job: no run, nothing of one.
        const RunRecord prepared(saved.manifest);
        QVERIFY(prepared.created().isValid());
        QVERIFY(!prepared.started().isValid() && !prepared.finished().isValid());
        QVERIFY(!prepared.exitCode() && !prepared.summary().recorded && !prepared.summary().energy);
        QVERIFY(prepared.diagnostics().isEmpty() && !prepared.remote() && prepared.error().isEmpty());
        // A record with diagnostics (one of them from before semantic targets) and a remote part.
        auto json = saved.manifest;
        json["status"] = "failed";
        json["started_at"] = "2026-01-02T03:04:05.678Z";
        json["finished_at"] = "2026-01-02T03:05:00.000Z";
        json["exit_code"] = 7;
        json["error"] = "ジョブを投入できません。";
        json["summary"] = QJsonObject{{"energy", -1.5},
                                      {"energy_method", "DFT・BSSE補正"},
                                      {"energy_source", "bsse_corrected"},
                                      {"normal_termination", false}};
        json["diagnostics"] = QJsonArray{QJsonObject{{"code", "memory"},
                                                     {"setting", "memory_mb"},
                                                     {"target", "execution"},
                                                     {"source", "stderr"},
                                                     {"evidence", "out of memory"},
                                                     {"future", 1}},
                                         QJsonObject{{"code", "basis"}, {"setting", "basis"}, {"tab", 1}}};
        json["remote"] = QJsonObject{{"host", QJsonObject{{"name", "cluster"}, {"scheduler", "slurm"}}},
                                     {"job", "4711"},
                                     {"phase", "submitted"},
                                     {"state", "queued"},
                                     {"message", "実行先へ接続できません。"},
                                     {"detail", "Connection refused"}};
        const RunRecord record(json);
        QCOMPARE(record.json(), json);
        QCOMPARE(record.state(), RunState::Failed);
        QCOMPARE(record.error(), "ジョブを投入できません。");
        QCOMPARE(record.started(), QDateTime::fromString("2026-01-02T03:04:05.678Z", Qt::ISODateWithMs));
        QCOMPARE(record.started().toUTC().time(), QTime(3, 4, 5, 678));
        QCOMPARE(record.started().secsTo(record.finished()), 54);
        QCOMPARE(*record.exitCode(), 7);
        const auto summary = record.summary();
        QVERIFY(summary.recorded && !summary.normalTermination);
        QCOMPARE(*summary.energy, -1.5);
        QCOMPARE(summary.energyMethod, "DFT・BSSE補正");
        QCOMPARE(summary.energySource, "bsse_corrected");
        const auto diagnostics = record.diagnostics();
        QCOMPARE(diagnostics.size(), 2);
        QCOMPARE(diagnostics[0].code, "memory");
        QCOMPARE(diagnostics[0].setting, "memory_mb");
        QCOMPARE(diagnostics[0].source, "stderr");
        QCOMPARE(diagnostics[0].evidence, "out of memory");
        QVERIFY(diagnostics[0].target == SettingTarget::Execution);
        QCOMPARE(diagnostics[0].stored, json["diagnostics"].toArray()[0].toObject());
        QVERIFY(diagnostics[0].owner() == SettingTarget::Execution);
        QVERIFY(!diagnostics[1].target);
        QCOMPARE(diagnostics[1].legacyTab, 1);
        QVERIFY(diagnostics[1].owner() == SettingTarget::Calculation);
        QCOMPARE(runDiagnosisLines(json),
                 (QStringList{runDiagnosisText("memory"), "out of memory", runDiagnosisText("basis")}));
        const auto remote = record.remote();
        QVERIFY(remote.has_value());
        QCOMPARE(remote->destination, "cluster");
        QCOMPARE(remote->scheduler, "slurm");
        QCOMPARE(remote->job, "4711");
        QCOMPARE(remote->phase, "submitted");
        QCOMPARE(remote->state, "queued");
        QCOMPARE(remote->message, "実行先へ接続できません。");
        QCOMPARE(remote->detail, "Connection refused");
        // An empty remote part is a run on this computer; an exit code that is recorded is one, even 0.
        json["remote"] = QJsonObject();
        json["exit_code"] = 0;
        QVERIFY(!RunRecord(json).remote());
        QCOMPARE(*RunRecord(json).exitCode(), 0);
    }
    // The calculations of a session, what it leaves for the next one and the checks of who asked, with
    // no window: the queue starts paused, so nothing runs here.
    void runSessionWithoutWidgets() {
        QTemporaryDir jobs;
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
        setExecutionSetting(d, "mode", "direct");
        setExecutionSetting(d, "executable", QCoreApplication::applicationFilePath());
        setExecutionSetting(d, "job_root", jobs.filePath("jobs"));
        RunSession session;
        QStringList changed;
        connect(&session, &RunSession::bundleChanged, this,
                [&](const QString &directory) { changed << directory; });
        const auto plan = session.saveBundle(d, jobs.path());
        QVERIFY(QFileInfo::exists(plan.directory + "/input.nw"));
        QCOMPARE(session.saved().directory, plan.directory);
        QCOMPARE(changed, QStringList{plan.directory});
        // A queued calculation keeps the check of the one who asked for it: now, and when its turn comes.
        int checks = 0;
        const RunSession::Check check = [&](const Document &, const QString &) { ++checks; };
        session.enqueue(d, jobs.path(), check);
        QCOMPARE(checks, 1);
        QCOMPARE(session.queue().pending(), 1);
        QCOMPARE(changed.size(), 2);
        QCOMPARE(changed.last(), session.queue().entries().last().plan.directory);
        QVERIFY(session.queue().entries().last().checkAccess);
        // One calculation per structure, under the name of the calculation.
        auto moved = d;
        moved.atoms[1].position.x() += .2;
        const auto folders = session.enqueueGeometries(d, jobs.path(), {d, moved}, true, check);
        QCOMPARE(folders.size(), 2);
        QCOMPARE(checks, 3);
        QCOMPARE(changed.mid(2), folders);
        {
            const auto &entries = session.queue().entries();
            QCOMPARE(entries.size(), 3);
            QCOMPARE(entries[1].name, QString("molforma_conf1"));
            QCOMPARE(entries[2].name, QString("molforma_conf2"));
            QCOMPARE(nwchemSettings(entries[2].document)["task"].toString(), QString("optimize_frequencies"));
            QVERIFY((entries[2].document.atoms[1].position - moved.atoms[1].position).norm() < 1e-9);
        }
        // Other atoms in one structure refuse the whole batch; so does an empty one.
        auto other = moved;
        other.atoms[1].element = "He";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 session.enqueueGeometries(d, jobs.path(), {d, other}, false));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, session.enqueueGeometries(d, jobs.path(), {}, false));
        QCOMPARE(session.queue().pending(), 3);
        // A chained calculation saves its bundle only when its turn comes.
        const auto chained = session.enqueueChain(d, jobs.path(), session.queue().entries().first().id,
                                                  "final", {"current"}, true, check);
        QCOMPARE(chained.size(), 1);
        QCOMPARE(changed.size(), 4);
        QVERIFY(session.queue().entries().last().plan.directory.isEmpty());
        QVERIFY(session.queue().entries().last().checkAccess);
        // What the last session left running: a job this one may not look at stays for one that may.
        QSettings preferences(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma");
        preferences.setValue("execution/running", plan.directory);
        auto restored = session.restore(false, [](const QString &) { fail("Outside the allowed folders."); });
        QVERIFY(!restored.following);
        QVERIFY(restored.ended.isEmpty());
        QCOMPARE(restored.waiting, 4);
        QCOMPARE(preferences.value("execution/running").toString(), plan.directory);
        // One that no longer runs is told once and forgotten.
        QString asked;
        restored = session.restore(false, [&](const QString &path) { asked = path; });
        QCOMPARE(asked, plan.directory);
        QCOMPARE(restored.ended, plan.directory);
        QVERIFY(!restored.following);
        QVERIFY(preferences.value("execution/running").toString().isEmpty());
        QVERIFY(session.restore(false).ended.isEmpty());
        // One session at a time keeps the stored queue.
        QVERIFY(session.keepQueue(jobs.filePath("data")));
        QVERIFY(QFileInfo::exists(jobs.filePath("data/queue.lock")));
        RunSession second;
        QVERIFY(!second.keepQueue(jobs.filePath("data")));
        session.shutdown();
        QVERIFY(session.queue().paused());
    }
    // The one factory of the preferences: the store that tests redirect (QSettings::setPath), with the
    // format, scope, organization and application of the files users have.
    void userPreferencesFollowTheRedirectedPath() {
        const QString key = "services/factory_probe";
        userPreferences().setValue(key, 42);
        QSettings plain(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma");
        QCOMPARE(plain.value(key).toInt(), 42);
        QVERIFY2(QDir::cleanPath(plain.fileName()).startsWith(QDir::cleanPath(home.path()) + '/'),
                 qPrintable(plain.fileName()));
        {
            const auto store = userPreferences();
            QCOMPARE(store.fileName(), plain.fileName());
            QCOMPARE(store.format(), SettingsFormat);
            QCOMPARE(store.scope(), QSettings::UserScope);
            QCOMPARE(store.organizationName(), QString("MolForma"));
            QCOMPARE(store.applicationName(), QString("MolForma"));
        }
        // On disk too, once written; and what another store writes is read through the factory.
        plain.sync();
        QVERIFY(QFileInfo::exists(plain.fileName()));
        plain.setValue(key, 7);
        QCOMPARE(userPreferences().value(key).toInt(), 7);
        userPreferences().remove(key);
        QVERIFY(!plain.contains(key));
    }
    // The names of the keys are those of the preference files users have: none is renamed.
    void preferenceKeysStayAsStored() {
        const QVector<std::pair<QString, QString>> keys{
            {Preference::windowGeometry, "geometry"},
            {Preference::directory, "directory"},
            {Preference::recentFiles, "recentFiles"},
            {Preference::startupWater, "startupWater"},
            {Preference::language, "ui/language"},
            {Preference::xtbExecutable, "xtb/executable"},
            {Preference::keepSelection, "keepSelection"},
            {Preference::keepTool, "keepTool"},
            {Preference::customParts, "parts"},
            {Preference::moleculePalette, "molecule/palette"},
            {Preference::moleculeSplit, "molecule/split"},
            {Preference::moleculeLayout, "molecule/layout"},
            {Preference::sequenceCollapsed, "molecule/sequenceCollapsed"},
            {Preference::coordinateAttributes, "coordinates/attributes"},
            {Preference::symmetryTolerance, "symmetry/tolerance"},
            {Preference::solvationSolvent, "solvation/solvent"},
            {Preference::solvationCount, "solvation/count"},
            {Preference::viewRenderer, "view/renderer"},
            {Preference::viewLabels, "view/labels"},
            {Preference::viewStyle, "view/style"},
            {Preference::viewHydrogenBonds, "view/hydrogen_bonds"},
            {Preference::nwchemPresets, "nwchem/presets"},
            {Preference::inputPreviewPaths, "inputPreview/showPaths"},
            {Preference::resultLabels, "results/labels"},
            {Preference::historyRoot, "history/root"},
            {Preference::historyFilesOpen, "history/files_open"},
            {Preference::imageScale, "export/image_scale"},
            {Preference::imageBackground, "export/image_background"},
            {Preference::animationFormat, "export/animation_format"},
            {Preference::animationScale, "export/animation_scale"},
            {Preference::animationBackground, "export/animation_background"},
            {Preference::openResults, "execution/open_results"},
            {Preference::runPanelHeight, "execution/run_panel_height"},
            {Preference::executionDefaults, "execution/defaults"},
            {Preference::remoteHosts, "execution/remotes"},
            {Preference::jobPrelude, "execution/job_prelude"},
            {Preference::jobHeader("slurm"), "execution/job_header_slurm"},
            {Preference::jobHeader("pbs"), "execution/job_header_pbs"},
            {Preference::runningJob, "execution/running"}};
        QSet<QString> seen;
        for (const auto &[constant, stored] : keys) {
            QCOMPARE(constant, stored);
            QVERIFY2(!seen.contains(stored), qPrintable(stored));
            seen.insert(stored);
        }
    }
    // The checks and generators of a run take the machine-wide environment from the snapshot they are
    // given and read nothing from the Preferences: what only the snapshot holds shows in their results,
    // what only the Preferences hold does not.
    void executionUsesTheEnvironmentItIsGiven() {
        QTemporaryDir jobs;
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
        setExecutionSetting(d, "mode", "direct");
        setExecutionSetting(d, "executable", QCoreApplication::applicationFilePath());
        setExecutionSetting(d, "job_root", jobs.filePath("jobs"));
        const auto hasError = [](const ExecutionValidation &report, const QString &code) {
            return std::any_of(report.diagnostics.begin(), report.diagnostics.end(), [&](const auto &item) {
                return item.code == code && item.severity == DiagnosticSeverity::Error;
            });
        };
        // In the Preferences only: a destination, preparation lines and a header.
        const auto forget = qScopeGuard([] {
            saveRemoteHosts({});
            saveExecutionJobPrelude({});
            saveJobHeaderTemplate("slurm", {});
            saveExecutionPreferences({});
        });
        RemoteHost saved;
        saved.name = "saved";
        saved.host = "user@saved.example.org";
        saveRemoteHosts({saved});
        saveExecutionJobPrelude("module load saved-only");
        saveJobHeaderTemplate("slurm", "#SBATCH --comment=saved-only\n");
        // In a hand-built snapshot only: another destination, other lines and another header.
        RemoteHost cluster;
        cluster.name = "given";
        cluster.host = "user@given.example.org";
        cluster.scheduler = "slurm";
        ExecutionEnvironment given;
        given.remotes = {cluster};
        given.jobPrelude = "module load given-only";
        given.jobHeaders["slurm"] = "#SBATCH --comment=given-only\n";

        // The loader is the snapshot of the Preferences.
        const auto loaded = executionEnvironment();
        QCOMPARE(loaded.remotes, QVector<RemoteHost>{saved});
        QCOMPARE(loaded.jobPrelude, QString("module load saved-only"));
        QCOMPARE(loaded.jobHeader("slurm"), QString("#SBATCH --comment=saved-only\n"));
        QCOMPARE(loaded.jobHeader("pbs"), defaultJobHeader("pbs"));
        QCOMPARE(loaded.preferences, executionPreferences());
        QVERIFY(loaded.remote("saved") && !loaded.remote("given") && !loaded.remote({}));
        // A snapshot without a header, or with lines that are no header, has the default one.
        QCOMPARE(ExecutionEnvironment{}.jobHeader("slurm"), defaultJobHeader("slurm"));
        ExecutionEnvironment broken;
        broken.jobHeaders["slurm"] = "rm -rf ~";
        QCOMPARE(broken.jobHeader("slurm"), defaultJobHeader("slurm"));

        // validateExecution: the destinations are those of the snapshot.
        setExecutionSetting(d, "remote", "given");
        auto report = validateExecution(d, jobs.path(), false, given);
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join('\n')));
        QCOMPARE(report.remote, remoteHostSummary(cluster));
        QVERIFY(hasError(validateExecution(d, jobs.path(), false), "execution.remote"));
        setExecutionSetting(d, "remote", "saved");
        QVERIFY(hasError(validateExecution(d, jobs.path(), false, given), "execution.remote"));
        report = validateExecution(d, jobs.path(), false);
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join('\n')));
        QCOMPARE(report.remote, remoteHostSummary(saved));

        // executionJobScript: the header and the preparation lines are those of the snapshot.
        setExecutionSetting(d, "remote", "");
        setExecutionSetting(d, "job_script", "slurm");
        auto script = executionJobScript(d, nullptr, given);
        QVERIFY2(script.startsWith("#!/bin/bash\n#SBATCH --comment=given-only\n"), qPrintable(script));
        QVERIFY2(script.contains("\nmodule load given-only\n"), qPrintable(script));
        QVERIFY2(!script.contains("saved-only"), qPrintable(script));
        script = executionJobScript(d);
        QVERIFY2(script.startsWith("#!/bin/bash\n#SBATCH --comment=saved-only\n"), qPrintable(script));
        QVERIFY2(script.contains("\nmodule load saved-only\n"), qPrintable(script));
        QVERIFY2(!script.contains("given-only"), qPrintable(script));
        script = executionJobScript(d, nullptr, ExecutionEnvironment{});
        QVERIFY2(script.startsWith("#!/bin/bash\n#SBATCH --job-name=molforma\n"), qPrintable(script));
        QVERIFY2(!script.contains("module load") && !script.contains("-only"), qPrintable(script));
        // With a destination, its own lines follow those of the snapshot.
        cluster.prelude = "module load at-destination";
        script = executionJobScript(d, &cluster, given);
        QVERIFY2(script.contains("\nmodule load given-only\nmodule load at-destination\n"),
                 qPrintable(script));
        cluster.prelude.clear();

        // executionPlan and its preview: the record takes the destination from the snapshot.
        setExecutionSetting(d, "remote", "given");
        const auto preview = executionPreviewPlan(d, jobs.path(), false, given);
        QCOMPARE(RemoteHost::fromJson(preview.manifest["remote"].toObject()["host"].toObject()), cluster);
        QCOMPARE(executionPlan(d, jobs.path(), jobs.filePath("job"), false, given).manifest.value("remote"),
                 preview.manifest.value("remote"));
        QVERIFY_THROWS_EXCEPTION(ValidationFailure, executionPreviewPlan(d, jobs.path()));
        QVERIFY_THROWS_EXCEPTION(ValidationFailure, executionPlan(d, jobs.path(), jobs.filePath("job")));
        QCOMPARE(remoteJobPreview(d, given).section('\n', 1), executionJobScript(d, &cluster, given));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, remoteJobPreview(d, executionEnvironment()));

        // saveExecutionBundle: the saved record and job.sh come from the same snapshot.
        const auto bundle = saveExecutionBundle(d, jobs.path(), false, given);
        const auto recorded = RunRecord(readRunRecord(bundle.directory, RecordCheck::Json)).remote();
        QVERIFY(recorded.has_value());
        QCOMPARE(recorded->destination, QString("given"));
        const auto savedScript = QString::fromUtf8(readFile(jobFile(bundle.directory, JobFile::script)));
        QCOMPARE(savedScript, executionJobScript(d, nullptr, given));
        QVERIFY2(savedScript.contains("given-only") && !savedScript.contains("saved-only"),
                 qPrintable(savedScript));
        QVERIFY_THROWS_EXCEPTION(ValidationFailure, saveExecutionBundle(d, jobs.path()));

        // untrustedNetworkPath: a network location is trusted where the snapshot's preferences name it.
        auto network = d;
        setExecutionSetting(network, "remote", "");
        setExecutionSetting(network, "job_root", "//server/share/jobs");
        ExecutionEnvironment trusting;
        trusting.preferences = QJsonObject{{"job_root", "//server/share/jobs"}};
        QCOMPARE(untrustedNetworkPath(network, given), QString("//server/share/jobs"));
        QVERIFY(untrustedNetworkPath(network, trusting).isEmpty());
        QVERIFY(hasError(validateExecution(network, jobs.path(), false, given), "execution.network"));
        QVERIFY(!hasError(validateExecution(network, jobs.path(), false, trusting), "execution.network"));
        // Saved in the Preferences, it is trusted by the loader's snapshot and still not by another.
        auto preferred = executionPreferences();
        preferred["job_root"] = "//server/share/jobs";
        saveExecutionPreferences(preferred);
        QVERIFY(untrustedNetworkPath(network).isEmpty());
        QCOMPARE(untrustedNetworkPath(network, given), QString("//server/share/jobs"));
    }
    void downloadsWithoutWidgets() {
        QTemporaryDir source, cache;
        const auto from = source.filePath("source.sdf"), to = cache.filePath("kept.sdf");
        writeFile(from, "downloaded");
        writeFile(to, "previous");
        FileDownload cancelled(QUrl::fromLocalFile(from), to, "test", "missing");
        QSignalSpy cancelledDone(&cancelled, &Download::finished);
        cancelled.start();
        cancelled.cancel();
        QTRY_COMPARE(cancelledDone.count(), 1);
        QVERIFY(cancelled.cancelled());
        QCOMPARE(readFile(to), QByteArray("previous"));
        FileDownload missing(QUrl::fromLocalFile(source.filePath("missing")), to, "test", "missing");
        QSignalSpy missingDone(&missing, &Download::finished);
        missing.start();
        QTRY_COMPARE(missingDone.count(), 1);
        QVERIFY(missing.notFound());
        QCOMPARE(missingDone[0][0].toString(), QString("missing"));
        QCOMPARE(readFile(to), QByteArray("previous"));
        FileDownload success(QUrl::fromLocalFile(from), to, "test", "missing");
        QSignalSpy successDone(&success, &Download::finished);
        success.start();
        QTRY_COMPARE(successDone.count(), 1);
        QVERIFY(successDone[0][0].toString().isEmpty());
        QCOMPARE(readFile(to), QByteArray("downloaded"));
        QCOMPARE(QDir(cache.path()).entryList(QDir::Files), QStringList{"kept.sdf"});
    }
    void pubchemWithoutWidgets() {
        QTemporaryDir source, cache;
        const auto url =
            QUrl::fromLocalFile(source.filePath("X.sdf")).toString().replace("X.sdf", "%1_%2_%3.sdf");
        writeFile(source.filePath("cid_2244_3d.sdf"), "3D record");
        writeFile(source.filePath("name_benzene_2d.sdf"), "2D record");
        PubchemDownload conformer(*pubchemQuery("cid:2244"), cache.path(), url);
        QSignalSpy done(&conformer, &Download::finished);
        conformer.start();
        QTRY_COMPARE(done.count(), 1);
        QVERIFY(done[0][0].toString().isEmpty());
        QVERIFY(conformer.record().threeD);
        QCOMPARE(readFile(conformer.path()), QByteArray("3D record"));
        PubchemDownload fallback(*pubchemQuery("benzene"), cache.path(), url);
        QSignalSpy fallbackDone(&fallback, &Download::finished);
        fallback.start();
        QTRY_COMPARE(fallbackDone.count(), 1);
        QVERIFY(fallbackDone[0][0].toString().isEmpty());
        QVERIFY(!fallback.record().threeD);
        QCOMPARE(readFile(fallback.path()), QByteArray("2D record"));
        PubchemDownload cancelled(*pubchemQuery("benzene"), cache.filePath("cancelled"), url);
        QSignalSpy cancelledDone(&cancelled, &Download::finished);
        cancelled.start();
        cancelled.cancel();
        QTRY_COMPARE(cancelledDone.count(), 1);
        QVERIFY(cancelled.cancelled());
        QVERIFY(cancelled.path().isEmpty());
        QVERIFY(QDir(cache.filePath("cancelled")).entryList(QDir::Files).isEmpty());
    }
    void chemistryCandidateKeepsSnapshotAndProvenance() {
        EditorSession session;
        auto d = Document::fromXyz("1\ncarbon\nC 0 0 0\n");
        d.envelope["future"] = QJsonObject{{"value", 42}};
        session.reset(d);
        ChemistryJob job(session);
        int ready = 0;
        connect(&job, &ChemistryJob::ready, this, [&](const ChemistryResult &result) {
            QVERIFY(job.active());
            QVERIFY(job.propose(result));
            ++ready;
        });
        job.start({"refine", "", {}, 10, true});
        QTRY_VERIFY(!job.active());
        QCOMPARE(ready, 1);
        QVERIFY(session.candidateRemote());
        QCOMPARE(session.document().project(), d.project());
        QCOMPARE(session.candidate()->envelope, d.envelope);
        QCOMPARE(session.candidate()->atoms[0].position.x(), 1.0);
        session.confirm();
        session.navigate("undo");
        QCOMPARE(session.document().project(), d.project());
    }
    void chemistryRejectsStaleResultsAndCancels() {
        EditorSession session;
        session.reset(Document::fromXyz("1\ncarbon\nC 0 0 0\n"));
        ChemistryJob job(session);
        int ready = 0, failed = 0;
        connect(&job, &ChemistryJob::ready, this, [&](const ChemistryResult &) { ++ready; });
        connect(&job, &ChemistryJob::failed, this,
                [&](const ChemistryRequest &, const QString &, bool) { ++failed; });
        job.start({"refine"});
        session.edit("intervening edit", [](Document &d) { d.atoms[0].position.y() = 2; });
        QTRY_VERIFY(!job.active());
        QCOMPARE(failed, 1);
        QCOMPARE(ready, 0);
        QVERIFY(!session.candidate());
        QCOMPARE(session.document().atoms[0].position.y(), 2.0);
        job.start({"refine", "hold"});
        QVERIFY(job.cancel());
        QVERIFY(!job.active());
        QVERIFY(!job.cancel());
        QCOMPARE(ready, 0);
        QCOMPARE(failed, 1);
    }
    void xtbKeepsFixedAtomsAndTakesTheConditions() {
        const auto program = QCoreApplication::applicationDirPath() + "/fake_xtb" EXE;
        QCOMPARE(xtbExecutable(program), QFileInfo(program).absoluteFilePath());
        QVERIFY(xtbExecutable(home.filePath("missing-xtb")).isEmpty());
        EditorSession session;
        // An aqua zinc ion: the metal that MMFF94/UFF refuse, fixed in place.
        auto d = Document::fromXyz("4\nzinc aqua\nZn 0 0 0\nO 2 0 0\nH 2.5 0.8 0\nH 2.5 -0.8 0\n");
        d.charge = 2;
        d.atoms[0].fixed = true;
        session.reset(d);
        XtbOptions options;
        options.solvent = "water";
        const auto run = runXtb(session, options);
        QVERIFY2(run.ready, qPrintable(run.error));
        QVERIFY(session.candidate());
        const auto &c = *session.candidate();
        QCOMPARE(c.atoms[0].position, d.atoms[0].position);
        QVERIFY((c.atoms[1].position - Vec(2.1, 0, 0)).norm() < 1e-9);
        QCOMPARE(c.ids(), d.ids());
        QVERIFY(run.note.contains("GFN2-xTB") && run.note.contains("-5.07054444") &&
                run.note.contains("収束"));
        const auto arguments = run.captured["arguments"].toVariant().toStringList();
        QCOMPARE(arguments, (QStringList{"input.xyz", "--opt", "normal", "--gfn", "2", "--chrg", "2", "--uhf",
                                         "0", "--alpb", "water", "--input", "xcontrol"}));
        QCOMPARE(run.control, QString("$fix\n   atoms: 1\n$end\n"));
        if (!qEnvironmentVariableIsSet("OMP_STACKSIZE"))
            QCOMPARE(run.captured["stack"].toString(), "1G");
        session.confirm();
        session.navigate("undo");
        QCOMPARE(session.document().project(), d.project());
    }
    void xtbFitsFreeMoleculesAndReportsTrouble() {
        EditorSession session;
        const auto water = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
        session.reset(water);
        // A molecule without fixed atoms is fitted back: only its shape changes.
        XtbOptions options;
        options.method = "gfnff";
        options.level = "tight";
        auto run = runXtb(session, options);
        QVERIFY2(run.ready, qPrintable(run.error));
        const auto &c = *session.candidate();
        Vec before = Vec::Zero(), after = Vec::Zero();
        for (int i = 0; i < 3; ++i) {
            before += water.atoms[i].position / 3;
            after += c.atoms[i].position / 3;
        }
        QVERIFY((before - after).norm() < 1e-9);
        QVERIFY(std::abs((c.atoms[1].position - c.atoms[2].position).norm() - 1.05 * 1.516) < 1e-9);
        QVERIFY(run.captured["arguments"].toVariant().toStringList().contains("--gfnff"));
        QVERIFY(run.control.isEmpty());
        session.cancel();
        // The selection moves; the other atoms are fixed in the xcontrol file.
        options = {};
        options.moving = {water.atoms[1].id, water.atoms[2].id};
        run = runXtb(session, options);
        QCOMPARE(run.control, QString("$fix\n   atoms: 1\n$end\n"));
        QCOMPARE(session.candidate()->atoms[0].position, water.atoms[0].position);
        session.cancel();
        // The step limit: the structure with a note; errors and fixed atoms that moved: no candidate.
        run = runXtb(session, {}, "unconverged");
        QVERIFY(run.ready);
        QVERIFY(run.note.contains("未収束") && run.note.contains("終了コード 1"));
        session.cancel();
        run = runXtb(session, {}, "fail");
        QVERIFY(!run.ready);
        QVERIFY(run.error.contains("Unknown element"));
        QVERIFY(!session.candidate());
        options = {};
        options.moving = {water.atoms[1].id};
        run = runXtb(session, options, "fixed");
        QVERIFY(run.error.contains("固定した原子1"));
        // What xtb cannot take, and no program.
        auto ghost = water;
        ghost.atoms[0].ghost = true;
        session.reset(ghost);
        ChemistryJob job(session);
        ChemistryRequest request{"xtb"};
        request.executable = xtbExecutable(QCoreApplication::applicationDirPath() + "/fake_xtb" EXE);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, job.start(request));
        session.reset(water);
        request.xtb.moving = {water.atoms[0].id};
        request.xtb.method = "pm6";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, job.start(request));
        request.xtb = {};
        request.executable.clear();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, job.start(request));
        // Cancelled while xtb runs.
        qputenv("MOLFORMA_FAKE_XTB", "slow");
        request.executable = xtbExecutable(QCoreApplication::applicationDirPath() + "/fake_xtb" EXE);
        job.start(request);
        QVERIFY(job.active());
        QVERIFY(job.cancel());
        QVERIFY(!session.candidate());
        qunsetenv("MOLFORMA_FAKE_XTB");
    }
    void sessionRevisionFollowsEveryChange() {
        // Every change of the document or its candidate moves the revision on, also one back to the
        // same contents; an edit that changes nothing does not.
        EditorSession session;
        session.reset(Document::fromXyz("1\ncarbon\nC 0 0 0\n"));
        auto revision = session.revision();
        const auto moved = [&] {
            const bool changed = session.revision() != revision;
            revision = session.revision();
            return changed;
        };
        QVERIFY(!session.edit("nothing", [](Document &) {}));
        QVERIFY(!moved());
        session.edit("move", [](Document &d) { d.atoms[0].position.y() = 1; });
        QVERIFY(moved());
        session.navigate("undo");
        QVERIFY(moved());
        session.edit("candidate", [](Document &d) { d.atoms[0].position.y() = 2; }, true);
        QVERIFY(moved());
        session.cancel();
        QVERIFY(moved());
        session.edit("candidate", [](Document &d) { d.atoms[0].position.y() = 2; }, true);
        QVERIFY(moved());
        session.confirm();
        QVERIFY(moved());
    }
    void chemistryStartFailureAndFixedAtoms() {
        EditorSession session;
        auto d = Document::fromXyz("1\ncarbon\nC 0 0 0\n");
        d.atoms[0].fixed = true;
        session.reset(d);
        ChemistryJob job(session, nullptr, home.filePath("missing-worker"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, job.start({"smiles", "C"}));
        QVERIFY(!job.active());
        int failed = 0;
        connect(&job, &ChemistryJob::failed, this,
                [&](const ChemistryRequest &, const QString &, bool starting) {
                    QVERIFY(starting);
                    ++failed;
                });
        job.start({"refine"});
        QTRY_COMPARE(failed, 1);
        QVERIFY(!job.active());
        QCOMPARE(session.document().project(), d.project());
    }
    void storageLocksAndRoundTrips() {
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, "services", "test");
        DocumentStorage storage(settings), other(settings);
        auto d = Document::fromXyz("1\ncarbon\nC 0 0 0\n");
        d.envelope["future"] = QJsonObject{{"value", 42}};
        const auto path = home.filePath("project.molforma.json");
        storage.save(d, path);
        QCOMPARE(Document::load(path).project(), d.project());
        QCOMPARE(storage.path(), path);
        QCOMPARE(storage.recentFiles(), QStringList{path});
        storage.startAutosave();
        storage.startAutosave(); // Does not abandon an existing lock/copy.
        storage.autosave(d, true);
        QVERIFY(other.recoverable().empty()); // Active sessions never offer each other's copies.
        const QDir directory(DocumentStorage::autosaveDirectory());
        const auto files = directory.entryList({"session-*.molforma.json"});
        QCOMPARE(files.size(), 1);
        const auto crashed = directory.filePath("session-crashed.molforma.json");
        QVERIFY(QFile::copy(directory.filePath(files[0]), crashed));
        {
            const auto recovery = other.recoverable();
            QCOMPARE(recovery.size(), size_t(1));
            QCOMPARE(recovery[0].document.project(), d.project());
            QCOMPARE(recovery[0].origin, path);
            QVERIFY(storage.recoverable().empty()); // The choice holds the crash copy's lock too.
        }
        auto recovery = other.recoverable();
        QCOMPARE(recovery.size(), size_t(1));
        recovery[0].discard();
        storage.autosave(d, false);
        QVERIFY(directory.entryList({"session-*.molforma.json"}).isEmpty());
    }
};

int main(int argc, char **argv) {
    if (const int code = resultWorkerMain(argc, argv); code >= 0)
        return code;
    QCoreApplication app(argc, argv);
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--worker") {
        // Deterministic stand-in for chemistry: echo one displaced atom, or wait for cancellation.
        const auto request = parseJson(readFile(QString::fromLocal8Bit(argv[2])));
        if (request["smiles"] == "hold")
            QThread::sleep(30);
        auto d = Document::fromJson(request["document"].toObject());
        d.atoms[0].position.x() += 1;
        writeFile(QString::fromLocal8Bit(argv[3]),
                  QJsonDocument(QJsonObject{{"document", d.toJson()}}).toJson());
        return 0;
    }
    ServicesTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_services.moc"
