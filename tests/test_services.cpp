// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/chemistry_job.h"
#include "foundation/files.h"
#include "io/document_storage.h"
#include "io/pdb_download.h"
#include "io/pubchem.h"
#include "model/settings.h"
#include "nwchem/conformer_set.h"
#include "nwchem/nwchem_execution.h"
#include "nwchem/run_session.h"
#include <QCoreApplication>
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
