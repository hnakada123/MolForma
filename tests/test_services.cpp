// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/chemistry_job.h"
#include "io/document_storage.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QThread>
#include <QtTest>

using namespace mf;
class ServicesTest : public QObject {
    Q_OBJECT
    QTemporaryDir home;
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
