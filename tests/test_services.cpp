// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/chemistry_job.h"
#include "io/document_storage.h"
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
