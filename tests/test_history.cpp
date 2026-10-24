#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "ui/execution_panel.h"
#include "ui/history_panel.h"
#include "ui/main_window.h"
#include "ui/results_panel.h"
#include <QtTest>
#include <QtWidgets>
using namespace mf;

class HistoryTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    static Document water(const QString &root) {
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        d.atoms[0].fixed = true;
        d.envelope["future"] = QJsonObject{{"preserve", 12}};
        setExecutionSetting(d, "job_root", root);
        setExecutionSetting(d, "mode", "direct");
        setExecutionSetting(d, "executable", QCoreApplication::applicationDirPath() + "/fake_nwchem");
        return d;
    }
    static void manifest(ExecutionPlan &p, const QString &state, int seconds = 0) {
        p.manifest["status"] = state;
        p.manifest["elapsed_seconds"] = seconds;
        if (state == "finished")
            p.manifest["exit_code"] = 0;
        writeFile(p.directory + "/run.json", QJsonDocument(p.manifest).toJson());
    }
    template <typename T> static T *widget(MainWindow &w, const char *name) {
        auto *p = w.findChild<T *>(name);
        Q_ASSERT(p);
        return p;
    }
    static void showHistory(MainWindow &w) {
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentIndex(7);
        w.show();
    }
  private slots:
    void initTestCase() {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settings.path()); // MolForma uses NativeFormat.
    }
    void init() {
        QSettings("MolForma", "MolForma").clear();
    }
    void snapshotsLegacyAndMovedDirectories() {
        QTemporaryDir tmp;
        auto d = water(tmp.filePath("jobs"));
        auto p = saveExecutionBundle(d, tmp.path());
        auto entry = readHistoryEntry(p.directory);
        QVERIFY(entry.valid);
        QCOMPARE(entry.status, "prepared");
        QCOMPARE(entry.method, "Hartree–Fock");
        QCOMPARE(entry.basis, "sto-3g");
        QVERIFY(historyIntegrity(entry).contains("入力・プロジェクトのSHA-256一致"));
        const auto before = readFile(p.directory + "/project.kgui.json");
        auto next = historyDocument(p.directory);
        QCOMPARE(next.atoms[0].id, d.atoms[0].id);
        QVERIFY(next.atoms[0].fixed);
        QCOMPARE(next.envelope["future"], d.envelope["future"]);
        QCOMPARE(nwchemSettings(next), nwchemSettings(d));
        QCOMPARE(executionSettings(next)["reference_directory"].toString(), tmp.path());
        QCOMPARE(readFile(p.directory + "/project.kgui.json"), before);
        p.manifest.remove("calculation");
        p.manifest.remove("project_sha256");
        p.manifest.remove("project_directory");
        manifest(p, "finished", 9);
        entry = readHistoryEntry(p.directory);
        QVERIFY(entry.valid);
        QCOMPARE(entry.seconds, 9);
        QCOMPARE(entry.basis, "sto-3g");
        QVERIFY(historyIntegrity(entry).contains("旧記録"));
        const auto moved = tmp.filePath("moved job");
        QVERIFY(QDir().rename(p.directory, moved));
        next = historyDocument(moved);
        QCOMPARE(executionSettings(next)["job_root"].toString(), tmp.path());
        QCOMPARE(readHistoryEntry(moved).directory, moved); // Never follow the original manifest directory.
        QCOMPARE(readFile(moved + "/project.kgui.json"), before);
    }
    void hashesBoundsMissingAndMalformed() {
        QTemporaryDir tmp;
        auto p = saveExecutionBundle(water(tmp.path()), tmp.path());
        const auto input = readFile(p.directory + "/input.nw");
        writeFile(p.directory + "/input.nw", input + "# manually changed\n");
        QVERIFY(historyIntegrity(readHistoryEntry(p.directory)).contains("一致しません"));
        QVERIFY_EXCEPTION_THROWN(historyDocument(p.directory), std::runtime_error);
        writeFile(p.directory + "/input.nw", input);
        const auto project = readFile(p.directory + "/project.kgui.json");
        writeFile(p.directory + "/project.kgui.json", project + " ");
        QVERIFY_EXCEPTION_THROWN(historyDocument(p.directory), std::runtime_error);
        writeFile(p.directory + "/project.kgui.json", project);
        p.manifest["input"] = "../outside.nw";
        manifest(p, "prepared");
        QVERIFY(!readHistoryEntry(p.directory).valid);
        QVERIFY_EXCEPTION_THROWN(historyDocument(p.directory), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(historyText(p.directory, "../input.nw"), std::runtime_error);
        writeFile(p.directory + "/run.json", "{");
        QCOMPARE(readHistoryEntry(p.directory).status, "invalid");
        writeFile(p.directory + "/run.json", QByteArray(256 * 1024 + 1, ' '));
        QVERIFY(!readHistoryEntry(p.directory).valid);
        QVERIFY(!readHistoryEntry(tmp.filePath("missing")).valid);
        writeFile(p.directory + "/input.nw", QByteArray(8 * 1024 * 1024 + 1, 'a'));
        QVERIFY_EXCEPTION_THROWN(historyText(p.directory, "input.nw"), std::runtime_error);
#ifdef Q_OS_UNIX
        QVERIFY(QFile::remove(p.directory + "/input.nw"));
        QVERIFY(QFile::link(p.directory + "/project.kgui.json", p.directory + "/input.nw"));
        QVERIFY_EXCEPTION_THROWN(historyText(p.directory, "input.nw"), std::runtime_error);
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
        QVERIFY_EXCEPTION_THROWN(historyDocument(p.directory), std::runtime_error);
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
        QVERIFY(historyStatus("running").contains("稼働未確認"));
        QCOMPARE(historyStatus("running", true), "実行中");
        QVERIFY(!historyStatus("finished").contains("成功"));
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
    void runSummaryAndProcessRecord() {
        QTemporaryDir tmp;
        auto d = water(tmp.filePath("jobs"));
        setExecutionSetting(d, "environment",
                            QJsonArray{QJsonObject{
                                {"enabled", true}, {"name", "MOLFORMA_FAKE_MODE"}, {"value", "frequency"}}});
        MainWindow w;
        w.loadDocument(d);
        auto *execution = widget<ExecutionPanel>(w, "executionTab");
        execution->startCalculation();
        const auto directory = execution->currentRun().plan().directory;
        QTRY_VERIFY(!execution->running());
        const auto m = parseJson(readFile(directory + "/run.json"));
        QCOMPARE(m["host"].toString(), QSysInfo::machineHostName());
        QVERIFY(m["pid"].toInteger() > 0);
        QVERIFY(!executionProcessGone(QJsonObject{})); // Nothing recorded: not known to have ended.
        // The frequency task's reference energy, not the displaced structure's at the end of the log.
        QCOMPARE(m["summary"].toObject()["energy_source"].toString(), "frequency_reference");
        const auto e = readHistoryEntry(directory);
        QCOMPARE(*e.energy, -76.1);
        QCOMPARE(e.energyMethod, "DFT");
    }
    void orphanedRecordsAndRepair() {
#ifdef Q_OS_LINUX
        QTemporaryDir tmp;
        auto d = water(tmp.filePath("jobs"));
        auto p = saveExecutionBundle(d, tmp.path());
        writeFile(p.directory + "/output.out", " NWChem Input Module\n Total SCF energy = -74.9\n");
        // Recorded as running by another MolForma whose process (here: this test) is alive.
        const auto self = executionProcessIdentity(QCoreApplication::applicationPid());
        for (auto it = self.begin(); it != self.end(); ++it)
            p.manifest[it.key()] = it.value();
        manifest(p, "running");
        auto e = readHistoryEntry(p.directory);
        QVERIFY(!e.orphaned);
        QVERIFY(historyStatus(e.status, false, e.orphaned).contains("稼働未確認"));
        QVERIFY_THROWS_EXCEPTION(std::exception, repairHistoryEntry(p.directory));
        // Another computer: unknown. The same PID started at another time: a different process.
        p.manifest["host"] = "elsewhere.invalid";
        p.manifest["pid_start"] = "1";
        manifest(p, "running");
        QVERIFY(!readHistoryEntry(p.directory).orphaned);
        p.manifest["host"] = self["host"];
        manifest(p, "running");
        QVERIFY(readHistoryEntry(p.directory).orphaned);
        // A process that has ended.
        QProcess done;
        done.setWorkingDirectory(tmp.path());
        done.start(QCoreApplication::applicationDirPath() + "/fake_nwchem", {});
        QVERIFY(done.waitForStarted(10000));
        p.manifest["pid"] = done.processId();
        p.manifest.remove("pid_start");
        QVERIFY(done.waitForFinished(10000));
        manifest(p, "starting");
        e = readHistoryEntry(p.directory);
        QVERIFY(e.orphaned);
        QVERIFY(historyStatus(e.status, false, e.orphaned).contains("中断"));
        QVERIFY(!e.energy || *e.energy == -74.9);
        // Repair from the history tab: the record says "interrupted", with the log's energy.
        MainWindow w;
        w.loadDocument(d);
        showHistory(w);
        auto *panel = widget<HistoryPanel>(w, "historyTab");
        panel->selectDirectory(p.directory);
        QTRY_VERIFY(!panel->scanning());
        QVERIFY(widget<QPushButton>(w, "historyTrash")->isEnabled());
        QVERIFY(widget<QPushButton>(w, "historyRepair")->isEnabled());
        widget<QPushButton>(w, "historyRepair")->click();
        QTRY_VERIFY(!panel->scanning());
        e = readHistoryEntry(p.directory);
        QCOMPARE(e.status, "interrupted");
        QVERIFY(!e.orphaned);
        QCOMPARE(*e.energy, -74.9);
        QCOMPARE(parseJson(readFile(p.directory + "/run.json"))["summary"].toObject()["energy"].toDouble(),
                 -74.9);
        QVERIFY(!widget<QPushButton>(w, "historyRepair")->isEnabled());
        QVERIFY(historyStatus("interrupted").contains("中断"));
#endif
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
                            [](const auto &e) { return e.status == "invalid"; }));
        scan.start(tmp.filePath("missing"));
        QCOMPARE(done.count(), 2);
        QVERIFY(scan.entries.isEmpty());
        QVERIFY(!scan.message.isEmpty());
        scan.start(tmp.path());
        scan.start(tmp.filePath("bad"));
        QTRY_COMPARE(done.count(), 3);
        QVERIFY(scan.entries.isEmpty()); // No stale rows from the superseded root.
    }
    void tableFiltersSortingSelectionAndReuse() {
        QTemporaryDir tmp;
        auto d = water(tmp.filePath("jobs"));
        auto first = saveExecutionBundle(d, tmp.path());
        manifest(first, "finished", 9);
        writeFile(first.directory + "/output.out",
                  "NWChem\n Total SCF energy = -75\n Total times  cpu: 1s\n");
        auto s = nwchemSettings(d);
        s["start"] = "pbe_water";
        s["method"] = "dft";
        s["functional"] = "pbe";
        d.data["nwchem"] = s;
        auto second = saveExecutionBundle(d, tmp.path());
        manifest(second, "running", 100);
        MainWindow w;
        w.loadDocument(d);
        const auto before = w.document().project();
        const int undo = w.history()->count();
        showHistory(w);
        auto *panel = widget<HistoryPanel>(w, "historyTab");
        auto *table = widget<QTableWidget>(w, "historyTable");
        QTRY_VERIFY(!panel->scanning());
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(widget<QTabWidget>(w, "workspaceTabs")->tabText(7), "履歴");
        panel->selectDirectory(first.directory);
        QTRY_VERIFY(!panel->scanning());
        QCOMPARE(panel->selectedDirectory(), first.directory);
        // ΔE against the selected job, in the chosen unit.
        writeFile(second.directory + "/output.out", "NWChem\n Total DFT energy = -75.01\n");
        widget<QPushButton>(w, "historyRefresh")->click();
        QTRY_VERIFY(!panel->scanning());
        panel->selectDirectory(first.directory);
        QTRY_VERIFY(!panel->scanning());
        widget<QPushButton>(w, "historyReference")->click();
        QStringList deltas;
        for (int row = 0; row < table->rowCount(); ++row)
            deltas << table->item(row, 7)->text();
        QVERIFY2(deltas.contains("0.000（基準）") && deltas.contains("-6.275"),
                 qPrintable(deltas.join(", ")));
        auto *unit = widget<QComboBox>(w, "historyEnergyUnit");
        unit->setCurrentIndex(unit->findData("hartree"));
        deltas.clear();
        for (int row = 0; row < table->rowCount(); ++row)
            deltas << table->item(row, 7)->text();
        QVERIFY2(deltas.contains("-0.01000000"), qPrintable(deltas.join(", ")));
        // Composition beside the name, and an editable memo kept in the job folder.
        QCOMPARE(table->item(0, 8)->text(), QString("H2O"));
        for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 0)->data(Qt::UserRole).toString() == first.directory)
                table->item(row, 9)->setText("基準構造");
        QCOMPARE(readFile(first.directory + "/memo.txt").trimmed(), QByteArray("基準構造"));
        QCOMPARE(readHistoryEntry(first.directory).memo, QString("基準構造"));
        QVERIFY(historyCsv({readHistoryEntry(first.directory)}).contains("\"H2O\",\"基準構造\""));
        QVERIFY(widget<QPlainTextEdit>(w, "historyInput")->toPlainText().contains("task scf energy"));
        QVERIFY(widget<QPlainTextEdit>(w, "historySummary")->toPlainText().contains("SHA-256一致"));
        table->sortItems(6, Qt::AscendingOrder);
        QCOMPARE(table->item(0, 6)->text(), "9");
        table->sortItems(6, Qt::DescendingOrder);
        QCOMPARE(table->item(0, 6)->text(), "100");
        QCOMPARE(panel->selectedDirectory(), first.directory);
        auto *filter = widget<QComboBox>(w, "historyFilter");
        filter->setCurrentIndex(filter->findData("unconfirmed"));
        QCOMPARE(table->rowCount(), 1);
        QVERIFY(panel->selectedDirectory().isEmpty());
        QVERIFY(!widget<QPushButton>(w, "historyReuse")->isEnabled());
        table->selectRow(0);
        QCOMPARE(panel->selectedDirectory(), second.directory);
        QVERIFY(!widget<QPushButton>(w, "historyStop")->isEnabled());
        QVERIFY(!widget<QPushButton>(w, "historyTrash")->isEnabled());
        filter->setCurrentIndex(0);
        widget<QLineEdit>(w, "historySearch")->setText("pbe");
        QCOMPARE(table->rowCount(), 1);
        const auto csvPath = tmp.filePath("history.csv");
        panel->exportCsv(csvPath);
        QVERIFY(readFile(csvPath).contains("pbe_water"));
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), undo);
        widget<QLineEdit>(w, "historySearch")->clear();
        panel->selectDirectory(first.directory);
        QTRY_VERIFY(!panel->scanning());
        const auto savedInput = readFile(first.directory + "/input.nw");
        panel->reuseSelected();
        QCOMPARE(nwchemSettings(w.document())["method"].toString(), "hf");
        QVERIFY(!w.history()->isClean());
        QVERIFY(w.document().atoms[0].fixed);
        QCOMPARE(w.document().envelope["future"], before["future"]);
        QCOMPARE(widget<QTabWidget>(w, "workspaceTabs")->currentIndex(), 2);
        QCOMPARE(readFile(first.directory + "/input.nw"), savedInput);
        QVERIFY(!widget<ExecutionPanel>(w, "executionTab")->running());
        // Ctrl+S must ask for a new path, never overwrite the saved snapshot.
        const auto savePath = tmp.filePath("reused.kgui.json");
        bool prompted = false;
        QTimer::singleShot(50, &w, [&] {
            for (auto *top : QApplication::topLevelWidgets())
                if (auto *dialog = qobject_cast<QFileDialog *>(top)) {
                    prompted = true;
                    dialog->setDirectory(tmp.path());
                    auto *name = dialog->findChild<QLineEdit *>("fileNameEdit");
                    QVERIFY(name);
                    name->setText(QFileInfo(savePath).fileName());
                    QMetaObject::invokeMethod(dialog, "accept");
                }
        });
        QTest::keyClick(&w, Qt::Key_S, Qt::ControlModifier);
        QVERIFY(prompted);
        QVERIFY(QFileInfo::exists(savePath));
        QCOMPARE(readFile(first.directory + "/input.nw"), savedInput);
    }
    void runIntegrationAndResultNavigation() {
        QTemporaryDir tmp;
        auto d = water(tmp.filePath("jobs"));
        setExecutionSetting(
            d, "environment",
            QJsonArray{QJsonObject{{"enabled", true}, {"name", "MOLFORMA_FAKE_MODE"}, {"value", "slow"}}});
        MainWindow w;
        w.loadDocument(d);
        showHistory(w);
        auto *history = widget<HistoryPanel>(w, "historyTab");
        auto *execution = widget<ExecutionPanel>(w, "executionTab");
        execution->startCalculation();
        const auto directory = execution->currentRun().plan().directory;
        history->selectDirectory(directory);
        QTRY_VERIFY(!history->scanning());
        QTRY_VERIFY(widget<QPushButton>(w, "historyStop")->isEnabled());
        QVERIFY(!widget<QPushButton>(w, "historyTrash")->isEnabled());
        widget<QPushButton>(w, "historyStop")->click();
        QTRY_VERIFY(!execution->running());
        QTRY_VERIFY(widget<QPushButton>(w, "historyTrash")->isEnabled());
        QVERIFY(widget<QPlainTextEdit>(w, "historySummary")->toPlainText().contains("停止済み"));
        writeFile(directory + "/result.xyz", "3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        history->refresh();
        QTRY_VERIFY(!history->scanning());
        const auto before = w.document().project();
        widget<QPushButton>(w, "historyResults")->click();
        auto *viewer = widget<ResultsPanel>(w, "resultsTab");
        QCOMPARE(widget<QTabWidget>(w, "workspaceTabs")->currentIndex(), 6);
        QTRY_VERIFY(!viewer->loading());
        QCOMPARE(w.document().project(), before);
        // Directory navigation prefers output.out. Open the available XYZ via the same viewer.
        viewer->openFile(directory + "/result.xyz");
        QTRY_VERIFY(!viewer->loading());
        QCOMPARE(viewer->result().frames.size(), 1);
        widget<QPushButton>(w, "executionHistory")->click();
        QTRY_VERIFY(!history->scanning());
        QCOMPARE(history->selectedDirectory(), directory);
    }
    void reuseCancellationCandidatesAndTrashGuards() {
        QTemporaryDir tmp;
        auto d = water(tmp.path());
        auto p = saveExecutionBundle(d, tmp.path());
        MainWindow w;
        w.loadDocument(d);
        showHistory(w);
        auto *panel = widget<HistoryPanel>(w, "historyTab");
        panel->selectDirectory(p.directory);
        QTRY_VERIFY(!panel->scanning());
        w.edit("changed", [](Document &n) { n.atoms[1].position.x() += .1; });
        const auto changed = w.document().project();
        QTimer::singleShot(10, &w, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            box->button(QMessageBox::Cancel)->click();
        });
        panel->reuseSelected();
        QCOMPARE(w.document().project(), changed);
        w.edit("candidate", [](Document &n) { n.atoms[1].position.y() += .1; }, true);
        QVERIFY_EXCEPTION_THROWN(panel->reuseSelected(), std::runtime_error);
        QVERIFY(w.hasCandidate());
        w.cancelCandidate();
        QVERIFY_EXCEPTION_THROWN(trashHistoryEntry(p.directory, tmp.path(), p.directory), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(trashHistoryEntry(p.directory, p.directory, {}), std::runtime_error);
        manifest(p, "running");
        QVERIFY_EXCEPTION_THROWN(trashHistoryEntry(p.directory, tmp.path(), {}), std::runtime_error);
        manifest(p, "future");
        QVERIFY_EXCEPTION_THROWN(trashHistoryEntry(p.directory, tmp.path(), {}), std::runtime_error);
        QVERIFY(QFileInfo::exists(p.directory + "/input.nw"));
    }
    void screenshot() {
        const auto artifacts = qEnvironmentVariable("MOLFORMA_HISTORY_ARTIFACTS");
        if (artifacts.isEmpty())
            return;
        QVERIFY(QDir().mkpath(artifacts));
        QTemporaryDir tmp;
        auto d = water(tmp.path());
        auto p = saveExecutionBundle(d, tmp.path());
        manifest(p, "finished", 12);
        writeFile(p.directory + "/output.out",
                  "NWChem\n Total SCF energy = -74.945628203255\n Total times  cpu: 1s\n");
        auto s = nwchemSettings(d);
        s["start"] = "water_pbe";
        s["method"] = "dft";
        s["functional"] = "pbe";
        d.data["nwchem"] = s;
        auto other = saveExecutionBundle(d, tmp.path());
        manifest(other, "timed_out", 120);
        MainWindow w;
        w.loadDocument(d);
        w.resize(1440, 1000);
        showHistory(w);
        auto *panel = widget<HistoryPanel>(w, "historyTab");
        panel->selectDirectory(p.directory);
        QTRY_VERIFY(!panel->scanning());
        QTest::qWait(100);
        QVERIFY(w.grab().save(artifacts + "/history.png"));
    }
    void restartAndLogFollow() {
        QTemporaryDir tmp;
        auto p = saveExecutionBundle(water(tmp.path()), tmp.path());
        writeFile(p.directory + "/output.out", "original\n");
        {
            MainWindow w;
            auto *panel = widget<HistoryPanel>(w, "historyTab");
            panel->setRoot(tmp.path());
            showHistory(w);
            QTRY_VERIFY(!panel->scanning());
        }
        MainWindow w;
        showHistory(w);
        auto *panel = widget<HistoryPanel>(w, "historyTab");
        QTRY_VERIFY(!panel->scanning());
        QCOMPARE(widget<QLineEdit>(w, "historyRoot")->text(), tmp.path());
        QCOMPARE(widget<QTableWidget>(w, "historyTable")->rowCount(), 1);
        panel->selectDirectory(p.directory);
        QTRY_VERIFY(!panel->scanning());
        auto *log = widget<QPlainTextEdit>(w, "historyOutput");
        QCOMPARE(log->toPlainText(), "original\n");
        widget<QCheckBox>(w, "historyFollow")->setChecked(false);
        writeFile(p.directory + "/output.out", "later\n");
        QTest::qWait(1100);
        QCOMPARE(log->toPlainText(), "original\n");
        widget<QCheckBox>(w, "historyFollow")->setChecked(true);
        QTRY_COMPARE(log->toPlainText(), "later\n");
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
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--result-worker") {
        QCoreApplication app(argc, argv);
        return resultWorker(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    }
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    HistoryTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_history.moc"
