#include "editing/coordinates.h"
#include "ui/execution_panel.h"
#include "ui/main_window.h"
#include "ui/results_panel.h"
#include <Eigen/LU>
#include <QtTest>
#include <QtWidgets>
#include <numbers>
using namespace mf;

class ResultsTest : public QObject {
    Q_OBJECT
    QTemporaryDir files, settings;
    QString save(const QString &name, const QString &contents) {
        auto path = files.filePath(name);
        writeFile(path, contents.toUtf8());
        return path;
    }
    static QString geometry() {
        return " Output coordinates in angstroms (scale by 1.8897 to convert to a.u.)\n\n"
               " No. Tag Charge X Y Z\n ---- ---- ------ ----- ----- -----\n"
               " 1 O 8.0 0.0 0.0 0.0\n 2 H 1.0 0.758 0.0 0.504\n 3 H 1.0 -0.758 0.0 0.504\n\n";
    }
    static QString cubeText(bool multiple = false) {
        QString s = "CUBE regression\nlinear field\n";
        s += multiple ? "-1 1 2 3\n" : "1 1 2 3\n";
        s += "2 1 0 0\n2 0.2 1 0\n2 0 0.3 1\n6 4 1 2 3\n";
        if (multiple)
            s += "2 5 6\n";
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 2; ++y)
                for (int z = 0; z < 2; ++z) {
                    s += QString::number(x - .5) + " ";
                    if (multiple)
                        s += QString::number(y - .5) + " ";
                }
        return s + "\n";
    }
    static QString moldenText() {
        return "[Molden Format]\n[Atoms] AU\nH 1 1 0 0 0\nH 2 1 0 0 1.4\n"
               "[GTO]\n1 0\ns 1 1.0\n1.0 1.0\n"
               "[MO]\nSym= A1\nEne= -0.5D+00\nSpin= Alpha\nOccup= 2.0\n1 0.5\n2 0.5\n"
               "Sym= B1\nEne= 0.25\nSpin= Beta\nOccup= 0\n1 0.5\n2 -0.5\n"
               "[FREQ]\n-100\n1500\n[FR-COORD]\nH 0 0 0\nH 0 0 1.4\n"
               "[FR-NORM-COORD]\nvibration 1\n0 0 .5\n0 0 -.5\nvibration 2\n0 0 .1\n0 0 -.1\n"
               "[INT]\n10\n20\n";
    }
    template <typename T> static T *widget(MainWindow &w, const char *name) {
        auto *p = w.findChild<T *>(name);
        Q_ASSERT(p);
        return p;
    }
  private slots:
    void initTestCase() {
        QVERIFY(files.isValid());
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    }
    void logGeometryEnergyAndFailure() {
        auto source = "NWChem\n" + geometry() +
                      " Total SCF energy = -7.4945628203255D+01\n"
                      " Total MP2 energy  -75.0\n Total CCSD(T) energy: -75.1\n"
                      " Optimization converged\n Total times  cpu: 1.0s wall: 1.0s\n";
        auto r = readResult(save("normal.out", source));
        QCOMPARE(r.frames.size(), 1);
        QCOMPARE(r.frames[0].molecule.atoms.size(), 3);
        QCOMPARE(r.frames[0].molecule.bonds.size(), 2);
        QCOMPARE(r.energies.size(), 3);
        QVERIFY(std::abs(r.energies[0].hartree + 74.945628203255) < 1e-12);
        QCOMPARE(r.energies[2].method, "CCSD(T)");
        QVERIFY(r.status.contains("正常終了マーカーあり"));
        r = readResult(save("failed.out", source + "An error occurred\nMPI_ABORT\n"));
        QVERIFY(r.status.contains("エラー"));
        // NWChem 7.3 errquit block (here: an unknown basis name), without any generic error words.
        const QString dashes(" " + QString(72, '-') + "\n");
        r = readResult(save("errquit.out",
                            "NWChem\n" + geometry() + dashes +
                                " bas_tag_lib: failed opening basis file                   0\n" + dashes +
                                dashes + "  current input line : \n" + "    11: task scf energy\n" + dashes +
                                dashes + " There is an error in the specified basis set\n" + dashes));
        QCOMPARE(r.status, QString("NWChemエラー: bas_tag_lib: failed opening basis file"));
        r = readResult(save("optfail.out", "NWChem\n" + geometry() + dashes +
                                               " scf optimize failed                                    0\n" +
                                               dashes + dashes + "  current input line : \n" +
                                               "    17: task scf optimize\n" + dashes));
        QCOMPARE(r.status, QString("NWChemエラー: scf optimize failed"));
        r = readResult(save("partial.out", "NWChem\n" + geometry() + geometry().chopped(35)));
        QCOMPARE(r.frames.size(), 1);
        QVERIFY(r.status.contains("マーカーなし"));
        QVERIFY(!r.warnings.isEmpty());
        auto au = geometry();
        au.replace("in angstroms", "in a.u.");
        r = readResult(save("bohr.log", "NWChem\n" + au));
        QVERIFY(std::abs(r.frames[0].molecule.atoms[1].position.x() - .758 * BohrToAngstrom) < 1e-12);
        QVERIFY_EXCEPTION_THROWN(readResult(save("not.log", "hello")), std::runtime_error);
    }
    void projectedModes() {
        QString s = "NWChem\n" + geometry() +
                    " (Projected Frequencies expressed in cm-1)\n\n P.Frequency -100 1600\n\n";
        for (int i = 0; i < 9; ++i)
            s += QString(" %1 %2 %3\n").arg(i + 1).arg(i == 0 ? 1 : 0).arg(i == 4 ? .5 : 0);
        s += "\n Normal Eigenvalue || Projected Infra Red Intensities\n Mode [cm**-1] || units units units "
             "units\n ---- ---- || ---- ---- ---- ----\n"
             " 1 -100 || 0.1 0.2 5.5 1.0\n 2 1600 || 0.2 0.3 8.5 1.2\n ----\n";
        const auto r = readResult(save("modes.out", s));
        QCOMPARE(r.modes.size(), 2);
        QCOMPARE(r.modes[1].intensity, 8.5);
        const auto moved = resultModeGeometry(r, 0, std::numbers::pi / 2, .3);
        QVERIFY(std::abs(moved.atoms[0].position.x() - .3) < 1e-12);
        QCOMPARE(r.vibrationGeometry.atoms[0].position.x(), 0.0);
    }
    void xyzAndMolden() {
        auto r = readResult(save("path.xyz", "2\n\nH 0 0 0\nH 0 0 1\n2\nsecond\nH 0 0 0\nH 0 0 2\n"));
        QCOMPARE(r.frames.size(), 2);
        QCOMPARE(r.frames[1].label, "second");
        QVERIFY_EXCEPTION_THROWN(readResult(save("truncated.xyz", "2\ntitle\nH 0 0 0\n")),
                                 std::runtime_error);
        r = readResult(save("orbitals.molden", moldenText()));
        QCOMPARE(r.orbitals.size(), 2);
        QCOMPARE(r.orbitals[0].energy, -.5);
        QCOMPARE(r.orbitals[1].spin, "Beta");
        QCOMPARE(r.modes.size(), 2);
        QCOMPARE(r.modes[1].intensity, 20.0);
        QVERIFY(std::abs(r.frames[0].molecule.atoms[1].position.z() - 1.4 * BohrToAngstrom) < 1e-12);
        auto bad = moldenText();
        bad.replace("[Atoms] AU", "[Atoms] unknown");
        QVERIFY_EXCEPTION_THROWN(readResult(save("bad.molden", bad)), std::runtime_error);
        bad = moldenText();
        bad.replace("Occup= 2.0", "Occup= nan");
        QVERIFY_EXCEPTION_THROWN(readResult(save("nan.molden", bad)), std::runtime_error);
        bad = moldenText();
        bad.replace("vibration 2\n0 0 .1\n0 0 -.1", "vibration 2\n0 0 .1");
        QVERIFY_EXCEPTION_THROWN(readResult(save("partial.molden", bad)), std::runtime_error);
    }
    void affineCubeAndDatasets() {
        const auto path = save("affine.cube", cubeText());
        auto r = readResult(path, .25);
        QCOMPARE(r.frames[0].molecule.atoms[0].element, "C");
        QVERIFY(!r.surface.isEmpty());
        Mat axes;
        axes << 1, .2, 0, 0, 1, .3, 0, 0, 1;
        axes *= BohrToAngstrom;
        const Vec origin = Vec(1, 2, 3) * BohrToAngstrom;
        for (const auto &t : r.surface)
            for (const Vec &p : {t.a, t.b, t.c}) {
                Vec grid = axes.inverse() * (p - origin);
                QVERIFY(std::abs(grid.x() - (t.positive ? .75 : .25)) < 1e-10);
            }
        r = readResult(save("orbitals.cube", cubeText(true)), .25, 1);
        QCOMPARE(r.datasets, QVector<int>({5, 6}));
        for (const auto &t : r.surface)
            for (const Vec &p : {t.a, t.b, t.c}) {
                Vec grid = axes.inverse() * (p - origin);
                QVERIFY(std::abs(grid.y() - (t.positive ? .75 : .25)) < 1e-10);
            }
        auto nval = cubeText(true);
        nval.replace("-1 1 2 3", "1 1 2 3 2");
        nval.replace("2 5 6\n", "");
        QCOMPARE(readResult(save("channels.cube", nval), .25, 1).datasets, QVector<int>({1, 2}));
        QVERIFY(readResult(path, 10).surface.isEmpty());
        for (const auto &bad :
             {cubeText() + "1", cubeText().chopped(8), QString(cubeText()).replace("2 1 0 0", "-2 1 0 0"),
              QString(cubeText()).replace("2 1 0 0", "2000000 1 0 0"),
              QString(cubeText()).replace("0.5", "nan")})
            QVERIFY_EXCEPTION_THROWN(readResult(save("invalid.cube", bad)), std::runtime_error);
    }
    void heavyElementResults() {
        for (const auto &symbol : {"Fr", "U", "Nh", "Og"}) {
            const int z = element(symbol).number;
            auto cube = cubeText();
            cube.replace("6 4 1 2 3", QString("%1 %1 1 2 3").arg(z));
            const auto c = readResult(save("heavy.cube", cube), .25);
            QCOMPARE(c.frames[0].molecule.atoms[0].element, QString(symbol));
            const auto molden = QString("[Molden Format]\n[Atoms] Angs\n%1 1 %2 0 0 0\n").arg(symbol).arg(z);
            const auto m = readResult(save("heavy.molden", molden));
            QCOMPARE(m.frames[0].molecule.atoms[0].element, QString(symbol));
            const auto log = QString("NWChem\n Output coordinates in angstroms\n\n"
                                     " No. Tag Charge X Y Z\n ---- ---- ------ ----- ----- -----\n"
                                     " 1 %1 %2.0 0.0 0.0 0.0\n\n")
                                 .arg(symbol)
                                 .arg(z);
            const auto l = readResult(save("heavy.out", log));
            QCOMPARE(l.frames[0].molecule.atoms[0].element, QString(symbol));
        }
        auto invalid = cubeText();
        invalid.replace("6 4 1 2 3", "119 119 1 2 3");
        QVERIFY_EXCEPTION_THROWN(readResult(save("unknown.cube", invalid)), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(
            readResult(save("unknown.molden", "[Molden Format]\n[Atoms] Angs\nUue 1 119 0 0 0\n")),
            std::runtime_error);
    }
    void transferAndBounds() {
        auto r = readResult(save("transfer.cube", cubeText()), .25);
        auto path = files.filePath("result.bin");
        saveResultData(path, r);
        auto loaded = loadResultData(path);
        QCOMPARE(loaded.surface.size(), r.surface.size());
        QCOMPARE(loaded.frames[0].molecule.toJson(), r.frames[0].molecule.toJson());
        QVERIFY((loaded.surface[0].a - r.surface[0].a).norm() < 1e-15);
        writeFile(path, readFile(path).chopped(4));
        QVERIFY_EXCEPTION_THROWN(loadResultData(path), std::runtime_error);
        QFile huge(files.filePath("huge.xyz"));
        QVERIFY(huge.open(QIODevice::WriteOnly));
        QVERIFY(huge.resize(128 * 1024 * 1024 + 1));
        huge.close();
        QVERIFY_EXCEPTION_THROWN(readResult(huge.fileName()), std::runtime_error);
    }
    void independentViewerAndCancellation() {
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        auto *tabs = widget<QTabWidget>(w, "workspaceTabs");
        QCOMPARE(tabs->count(), 8);
        QCOMPARE(tabs->tabText(6), "結果ビューア");
        tabs->setCurrentIndex(6);
        const auto before = w.document().project();
        const int history = w.history()->count();
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        const auto a = save("viewer.xyz", "2\nframe 1\nH 0 0 0\nH 0 0 1\n2\nframe 2\nH 0 0 0\nH 0 0 2\n");
        panel->openFile(a);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(panel->result().frames.size(), 2);
        const auto output = files.filePath("saved.xyz");
        panel->saveGeometry(output);
        QCOMPARE(Document::fromXyz(QString::fromUtf8(readFile(output))).atoms[1].position.z(), 2.0);
        widget<QSpinBox>(w, "resultsFrame")->setValue(1);
        panel->saveGeometry(output);
        QCOMPARE(Document::fromXyz(QString::fromUtf8(readFile(output))).atoms[1].position.z(), 1.0);
        panel->openFile(a);
        panel->cancelLoad();
        QVERIFY(!panel->loading());
        QCOMPARE(panel->result().source, a);
        panel->openFile(save("first.cube", cubeText()));
        panel->openFile(a);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        QCOMPARE(panel->result().format, "XYZ");
        panel->openFile(save("broken.cube", "bad"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QCOMPARE(panel->result().source, a);
        panel->openFile(save("modes.molden", moldenText()));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 3, 10000);
        widget<QTableWidget>(w, "resultsModes")->setCurrentCell(1, 0);
        widget<QPushButton>(w, "resultsPlayMode")->click();
        QTest::qWait(100);
        panel->saveGeometry(output);
        QVERIFY(std::abs(Document::fromXyz(QString::fromUtf8(readFile(output))).atoms[1].position.z() -
                         1.4 * BohrToAngstrom) < 1e-10);
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), history);
        // Destruction cancels an in-flight reader and stops animation.
        panel->openFile(a);
    }
    void fileSelectionLoadsAndCancels() {
        QTemporaryDir job;
        QVERIFY(job.isValid());
        const auto output = job.filePath("output.out"), xyz = job.filePath("a.xyz"),
                   cube = job.filePath("z.cube");
        writeFile(output, ("NWChem\n" + geometry()).toUtf8());
        writeFile(xyz, "1\nhydrogen\nH 0 0 0\n");
        writeFile(cube, cubeText().toUtf8());
        writeFile(job.filePath("broken.xyz"), "invalid XYZ\n");
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        w.show();
        auto *chooser = widget<QComboBox>(w, "resultsFiles");
        auto *view = widget<MoleculeView>(w, "resultsView");
        const auto before = w.document().project();
        const int history = w.history()->count();
        QVERIFY(!w.findChild<QPushButton *>("resultsOpenSelected"));
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        panel->openDirectory(job.path());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QCOMPARE(chooser->currentText(), "output.out"); // Prefer the log over the first list item.
        QCOMPARE(panel->result().source, output);
        QVERIFY(!panel->loading());
        chooser->setCurrentText("a.xyz");
        QVERIFY(panel->loading());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        QCOMPARE(panel->result().source, xyz);
        QCOMPARE(panel->result().frames[0].molecule.atoms.size(), 1);
        const auto atomId = panel->result().frames[0].molecule.atoms[0].id;
        QVERIFY(view->rect().contains(view->atomScreenPosition(atomId).toPoint()));
        // Keyboard selection also opens the newly chosen file without an extra action.
        chooser->setFocus();
        QTest::keyClick(chooser, Qt::Key_End);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 3, 10000);
        QCOMPARE(chooser->currentText(), "z.cube");
        QCOMPARE(panel->result().source, cube);
        QVERIFY(!panel->result().surface.isEmpty());
        chooser->setCurrentText("a.xyz");
        QVERIFY(panel->loading());
        chooser->setCurrentText("output.out");
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 4, 10000);
        QCOMPARE(panel->result().source, output);
        QVERIFY(panel->result().surface.isEmpty());
        QVERIFY(!panel->loading()); // Refreshing the list must not start another reader.
        QCOMPARE(failed.count(), 0);
        chooser->setCurrentText("broken.xyz");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QCOMPARE(panel->result().source, output);
        QCOMPARE(done.count(), 4);
        chooser->setCurrentText("a.xyz");
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 5, 10000);
        QCOMPARE(panel->result().source, xyz);
        chooser->setCurrentText("output.out");
        QVERIFY(panel->loading());
        widget<QLineEdit>(w, "resultsIsovalue")->setText("nan");
        chooser->setCurrentText("z.cube");
        QCOMPARE(failed.count(), 2);
        QVERIFY(!panel->loading()); // Even a rejected selection cancels the older request.
        QTest::qWait(100);
        QCOMPARE(done.count(), 5);
        QCOMPARE(panel->result().source, xyz);
        chooser->setCurrentText("output.out");
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 6, 10000);
        QCOMPARE(panel->result().source, output);
        const auto other = job.filePath("result.dat");
        writeFile(other, ("NWChem\n" + geometry()).toUtf8());
        panel->openFile(other);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 7, 10000);
        QCOMPARE(chooser->currentText(), "result.dat");
        QCOMPARE(panel->result().source, other);
        QVERIFY(!panel->loading());
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), history);
    }
    void cameraSlidersPreserveGeometry() {
        MainWindow w;
        auto editor = Document::fromXyz("2\neditor\nH 0 0 0\nH 0 0 .74\n");
        editor.atoms[0].fixed = true;
        w.loadDocument(editor);
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        w.show();
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(save("camera.xyz", "3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        auto *view = widget<MoleculeView>(w, "resultsView");
        const auto molecule = panel->result().frames[0].molecule;
        const auto before = w.document().project();
        const int history = w.history()->count();
        const auto editorPosition = w.canvas()->atomScreenPosition(w.document().atoms[0].id);
        const auto output = files.filePath("camera-saved.xyz");
        panel->saveGeometry(output);
        const auto saved = readFile(output);
        MoleculeView expected;
        expected.resize(view->size());
        expected.setDocument(molecule);
        expected.fit();
        int angles[] = {-25, 25, 0};
        const int rotated[] = {60, -45, 90};
        const char *names[] = {"resultsYaw", "resultsPitch", "resultsRoll"};
        expected.setAngles(angles[0], angles[1], angles[2]);
        for (const auto &atom : molecule.atoms)
            QCOMPARE(view->atomScreenPosition(atom.id), expected.atomScreenPosition(atom.id));
        for (int i = 0; i < 3; ++i) {
            auto *slider = widget<QSlider>(w, names[i]);
            QVERIFY(slider->isVisible());
            QCOMPARE(slider->orientation(), Qt::Horizontal);
            QCOMPARE(slider->minimum(), -180);
            QCOMPARE(slider->maximum(), 180);
            QCOMPARE(slider->value(), angles[i]);
            const auto position = view->atomScreenPosition(molecule.atoms[1].id);
            slider->setValue(rotated[i]);
            angles[i] = rotated[i];
            expected.setAngles(angles[0], angles[1], angles[2]);
            QVERIFY(QLineF(position, view->atomScreenPosition(molecule.atoms[1].id)).length() > 1);
            for (const auto &atom : molecule.atoms)
                QCOMPARE(view->atomScreenPosition(atom.id), expected.atomScreenPosition(atom.id));
            panel->saveGeometry(output);
            QCOMPARE(readFile(output), saved);
        }
        QCOMPARE(panel->result().frames[0].molecule.toJson(), molecule.toJson());
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), history);
        QCOMPARE(w.canvas()->atomScreenPosition(w.document().atoms[0].id), editorPosition);
    }
    void changedSourceRejected() {
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        const auto path = save("changing.xyz", "1\ninitial\nH 0 0 0\n");
        panel->openFile(path);
        writeFile(path, "1\nmodified before worker completes\nH 1 0 0\n");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QCOMPARE(done.count(), 0);
        QVERIFY(panel->result().source.isEmpty());
    }
    void executionNavigationAndCubeControls() {
        MainWindow w;
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 .504\nH -.758 0 .504\n");
        setExecutionSetting(d, "job_root", files.filePath("viewer-jobs"));
        w.loadDocument(d);
        auto *execution = widget<ExecutionPanel>(w, "executionTab");
        execution->saveBundle();
        const auto directory = widget<QLabel>(w, "executionLocation")->text();
        QVERIFY(!directory.isEmpty());
        const auto output = QDir(directory).filePath("output.out");
        writeFile(output, ("NWChem\n" + geometry() + "Total SCF energy = -74.9\n").toUtf8());
        execution->refresh();
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        QVERIFY(widget<QPushButton>(w, "executionResults")->isEnabled());
        widget<QPushButton>(w, "executionResults")->click();
        QCOMPARE(widget<QTabWidget>(w, "workspaceTabs")->currentIndex(), 6);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QCOMPARE(panel->result().source, output);
        panel->openFile(save("control.cube", cubeText(true)));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        widget<QLineEdit>(w, "resultsIsovalue")->setText("0.25");
        widget<QComboBox>(w, "resultsDataset")->setCurrentIndex(1);
        widget<QPushButton>(w, "resultsApplyIso")->click();
        QVERIFY(!widget<QPushButton>(w, "resultsSaveXyz")->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 3, 10000);
        QCOMPARE(panel->result().isovalue, .25);
        QCOMPARE(panel->result().dataset, 1);
        widget<QLineEdit>(w, "resultsIsovalue")->setText("nan");
        widget<QPushButton>(w, "resultsApplyIso")->click();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(panel->result().isovalue, .25);
        panel->openFile(output); // An invalid CUBE setting must not block logs.
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 4, 10000);
        QCOMPARE(w.document().project(), d.project());
        QVERIFY(w.history()->isClean());
    }
    void realEngineArtifacts() {
        const auto root = qEnvironmentVariable("MOLFORMA_RESULTS_REAL");
        if (root.isEmpty())
            return;
        auto logData = readResult(QDir(root).filePath("output.out"));
        QVERIFY(logData.frames.size() > 2);
        QCOMPARE(logData.modes.size(), 9);
        QVERIFY(logData.status.contains("エラー"));
        QVERIFY(std::abs(logData.modes[6].frequency - 2169.67) < .01);
        QVERIFY(std::abs(logData.modes[6].intensity - 7.247) < .001);
        auto electronic = readResult(QDir(root).filePath("export.out"));
        QVERIFY(electronic.status.contains("正常終了マーカーあり"));
        const auto methods = readResult(QDir(root).filePath("methods2.out"));
        QVERIFY(methods.status.contains("正常終了マーカーあり"));
        QMap<QString, double> energies;
        for (const auto &e : methods.energies)
            energies[e.method] = e.hartree;
        QVERIFY(std::abs(energies.value("MP2") + 76.106091880695) < 1e-9);
        QVERIFY(std::abs(energies.value("CCSD") + 76.112438787689) < 1e-9);
        QVERIFY(std::abs(energies.value("CCSD(T)") + 76.113292997246) < 1e-9);
        QVERIFY(std::abs(energies.value("DFT") + 76.377870344163) < 1e-9);
        auto mo = readResult(QDir(root).filePath("export.molden"));
        QCOMPARE(mo.orbitals.size(), 7);
        QCOMPARE(mo.frames[0].molecule.atoms.size(), 3);
        auto cube = readResult(QDir(root).filePath("homo.cube"));
        QVERIFY(!cube.surface.isEmpty());
        auto trajectory = readResult(QDir(root).filePath("water_vib.freq.m-007.xyz"));
        QVERIFY(trajectory.frames.size() > 1);
        MainWindow w;
        w.resize(1380, 900);
        w.show();
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentIndex(6);
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(QDir(root).filePath("homo.cube"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QCoreApplication::processEvents();
        QVERIFY(w.grab().save(QDir(root).filePath("viewer-cube.png")));
        panel->openFile(QDir(root).filePath("output.out"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        widget<QTabWidget>(w, "resultsDetails")->setCurrentIndex(3);
        widget<QTableWidget>(w, "resultsModes")->setCurrentCell(6, 0);
        QCoreApplication::processEvents();
        QVERIFY(w.grab().save(QDir(root).filePath("viewer-vibration.png")));
    }
};
int main(int argc, char **argv) {
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--result-worker") {
        QCoreApplication app(argc, argv);
        return resultWorker(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    }
    QApplication app(argc, argv);
    ResultsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_results.moc"
