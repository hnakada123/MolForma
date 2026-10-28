#include "nwchem/nwchem_output.h"
#include "ui/calculation_panel.h"
#include "ui/execution_panel.h"
#include "ui/input_preview_panel.h"
#include "ui/main_window.h"
#include "ui/results_panel.h"
#include <QtTest>
#include <QtWidgets>
using namespace mf;
// Executables carry .exe on Windows.
#ifdef Q_OS_WIN
#define EXE ".exe"
#else
#define EXE ""
#endif

class OutputTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    static Document water() {
        return Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
    }
    static void base(Document &d, const QString &key, const QJsonValue &value) {
        auto s = d.data["nwchem"].toObject();
        s[key] = value;
        d.data["nwchem"] = s;
    }
    static void both(Document &d) {
        setNwchemAdvanced(d, "cube_output", true);
        setNwchemAdvanced(d, "molden_output", true);
    }
    template <typename T> static T *widget(MainWindow &w, const QString &name) {
        auto *p = w.findChild<T *>(name);
        Q_ASSERT(p);
        return p;
    }
    static void edit(MainWindow &w, const QString &key, const QString &value) {
        auto *line = widget<QLineEdit>(w, "advanced_" + key);
        line->setText(value);
        line->setModified(true);
        QMetaObject::invokeMethod(line, "editingFinished");
    }
    static QString cube() {
        return "density\nfixture\n1 0 0 0\n2 1 0 0\n2 0 1 0\n2 0 0 1\n1 1 0 0 0\n-1 -1 -1 -1 1 1 1 1\n";
    }
  private slots:
    void initTestCase() {
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settings.path()); // MolForma uses NativeFormat.
    }
    void defaultsAndTasks() {
        auto d = water();
        const auto before = d.project();
        QVERIFY(!nwchemInput(d).contains("dplot"));
        QVERIFY(!nwchemInput(d).contains("property"));
        QVERIFY(nwchemOutputFiles(d, validateNwchem(d)).isEmpty());
        QCOMPARE(d.project(), before);
        both(d);
        const auto input = nwchemInput(d);
        QVERIFY(input.contains("task scf energy\n\n# Visualization"));
        QVERIFY(input.contains("property\n  vectors \"./molforma.movecs\"\n  moldenfile\n  molden_norm "
                               "nwchem\nend\ntask scf property"));
        QVERIFY(input.contains("spin total\n  gaussian\n  output molforma_density.cube\nend\ntask dplot"));
        QVERIFY(input.indexOf("task scf energy") < input.indexOf("task scf property"));
        QVERIFY(input.indexOf("task scf property") < input.indexOf("task dplot"));
        // 40 intervals along the longest side (X: 7.516 Å), the same spacing on Y and Z.
        auto grid = nwchemCubeGrid(d);
        QCOMPARE(grid.divisions, (std::array<int, 3>{40, 32, 35}));
        QCOMPARE(grid.points(), 41LL * 33 * 36);
        QCOMPARE(grid.minimum[0], -3.758);
        for (int axis = 0; axis < 3; ++axis)
            QVERIFY(std::abs((grid.maximum[axis] - grid.minimum[axis]) / grid.divisions[axis] - 7.516 / 40) <
                    1e-12);
        QVERIFY(grid.minimum[2] < -3 && grid.maximum[2] > 3.504);
        QVERIFY(std::abs(grid.minimum[2] + grid.maximum[2] - .504) < 1e-12); // Centered on the atoms.
        base(d, "geometry_units", "bohr");
        QVERIFY(nwchemInput(d).contains("limitxyz units angstrom"));
        QCOMPARE(nwchemCubeGrid(d).minimum, grid.minimum);
        base(d, "method", "dft");
        QVERIFY(nwchemInput(d).contains("task dft property"));
        base(d, "method", "mp2");
        QVERIFY(nwchemInput(d).contains("task mp2 energy"));
        QVERIFY(nwchemInput(d).contains("task scf property"));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("参照HF"));
        base(d, "method", "hf");
        base(d, "task", "optimize_frequencies");
        base(d, "numerical_frequencies", true);
        const auto frequency = nwchemInput(d);
        QVERIFY(frequency.indexOf("task scf optimize") < frequency.indexOf("task scf frequencies numerical"));
        QVERIFY(frequency.indexOf("task scf frequencies numerical") <
                frequency.lastIndexOf("task scf energy"));
        QVERIFY(frequency.lastIndexOf("task scf energy") < frequency.indexOf("task scf property"));
    }
    void orbitalsAndSpin() {
        auto d = water();
        both(d);
        setNwchemAdvanced(d, "cube_type", "orbital");
        QVERIFY(nwchemInput(d).contains("spin alpha\n  orbitals view; 1; 5"));
        QVERIFY(nwchemInput(d).contains("output molforma_alpha_mo5.cube"));
        setNwchemAdvanced(d, "cube_orbital", "lumo");
        QVERIFY(nwchemInput(d).contains("orbitals view; 1; 6"));
        setNwchemAdvanced(d, "cube_spin", "beta");
        QVERIFY_EXCEPTION_THROWN(nwchemInput(d), std::runtime_error);
        base(d, "reference", "uhf");
        d.setJobValue("charge", 1);
        d.setJobValue("multiplicity", 2);
        setNwchemAdvanced(d, "cube_orbital", "homo");
        QVERIFY(nwchemInput(d).contains("spin beta\n  orbitals view; 1; 4"));
        setNwchemAdvanced(d, "cube_orbital", "index");
        QVERIFY_EXCEPTION_THROWN(nwchemInput(d), std::runtime_error);
        setNwchemAdvanced(d, "cube_index", 7);
        QVERIFY(nwchemInput(d).contains("orbitals view; 1; 7"));
        setNwchemAdvanced(d, "cube_type", "density");
        setNwchemAdvanced(d, "cube_index", "bad but inactive");
        QVERIFY(!nwchemInput(d).contains("orbitals view"));
        const QMap<QString, QString> types{{"density", "total"},
                                           {"alpha_density", "alpha"},
                                           {"beta_density", "beta"},
                                           {"spin_density", "spindens"}};
        for (auto it = types.begin(); it != types.end(); ++it) {
            setNwchemAdvanced(d, "cube_type", it.key());
            QVERIFY(nwchemInput(d).contains("spin " + it.value()));
        }
        auto h = Document::fromXyz("1\nH\nH 0 0 0\n");
        h.setJobValue("multiplicity", 2);
        base(h, "reference", "uhf");
        both(h);
        setNwchemAdvanced(h, "cube_type", "orbital");
        setNwchemAdvanced(h, "cube_spin", "beta");
        QVERIFY_EXCEPTION_THROWN(nwchemInput(h), std::runtime_error);
        setNwchemAdvanced(h, "cube_orbital", "lumo");
        QVERIFY(nwchemInput(h).contains("orbitals view; 1; 1"));
    }
    void validationAndPreservation() {
        auto d = water();
        both(d);
        d.envelope["future"] = "keep";
        for (const auto &bad : {QJsonValue(QJsonArray{-3, 3}), QJsonValue(QJsonArray{3, -3, 20}),
                                QJsonValue(QJsonArray{-3, 3, 1.5}), QJsonValue("NaN")}) {
            setNwchemAdvanced(d, "cube_x", bad);
            QVERIFY_EXCEPTION_THROWN(nwchemInput(d), std::runtime_error);
        }
        setNwchemAdvanced(d, "cube_x", QJsonArray{-3, 3, 30});
        setNwchemAdvanced(d, "cube_divisions", 1000);
        QVERIFY_EXCEPTION_THROWN(nwchemInput(d), std::runtime_error);
        setNwchemAdvanced(d, "cube_divisions", 20);
        QCOMPARE(nwchemCubeGrid(d).points(), 31LL * 17 * 19); // Y and Z keep the automatic spacing.
        for (const auto &name :
             {"../bad.cube", "input.nw", "bad name.cube", "bad;task.cube", "bad\n.cube", "/abs.cube"}) {
            setNwchemAdvanced(d, "cube_filename", name);
            QVERIFY_EXCEPTION_THROWN(nwchemInput(d), std::runtime_error);
        }
        setNwchemAdvanced(d, "cube_filename", "orbitals.CUBE");
        setNwchemAdvanced(d, "vectors_output", "orbitals.CUBE");
        QVERIFY_EXCEPTION_THROWN(nwchemInput(d), std::runtime_error);
        setNwchemAdvanced(d, "vectors_output", "orbitals.movecs");
        const auto saved = d.project();
        auto next = Document::fromProject(saved);
        QCOMPARE(nwchemAdvanced(next), nwchemAdvanced(d));
        QCOMPARE(next.envelope["future"].toString(), "keep");
        applyNwchemPreset(next, nwchemPresets().first().settings);
        QCOMPARE(nwchemAdvanced(next), nwchemAdvanced(d)); // Output choices are not numerical presets.
        setNwchemAdvanced(next, "cube_output", QJsonValue::Undefined);
        setNwchemAdvanced(next, "cube_filename", "invalid while disabled");
        QVERIFY(!nwchemInput(next).contains("dplot"));
        QVERIFY(nwchemInput(next).contains("moldenfile"));
        setNwchemAdvanced(next, "cube_output", true);
        QVERIFY_EXCEPTION_THROWN(nwchemInput(next), std::runtime_error);
        resetNwchemAdvanced(next, "visualization");
        QVERIFY(!nwchemInput(next).contains("property"));
        QCOMPARE(nwchemAdvanced(next)["vectors_output"].toString(), "orbitals.movecs");
    }
    void bundlePathsCollisionsAndViewer() {
        QTemporaryDir tmp;
        auto d = water();
        both(d);
        setExecutionSetting(d, "job_root", tmp.path());
        setExecutionSetting(d, "mode", "direct");
        setExecutionSetting(d, "executable", QCoreApplication::applicationDirPath() + "/fake_nwchem" EXE);
        setNwchemAdvanced(d, "permanent_dir", "permanent");
        setNwchemAdvanced(d, "vectors_output", "nested/custom orbitals.movecs");
        const auto plan = saveExecutionBundle(d, tmp.path(), true);
        QVERIFY(QFileInfo(plan.directory + "/permanent/nested").isDir());
        QVERIFY(plan.input.contains("dplot\n  vectors \"./permanent/nested/custom orbitals.movecs\""));
        QCOMPARE(plan.input, executionPreviewPlan(d, tmp.path()).input);
        QCOMPARE(plan.manifest["visualization_files"].toArray(),
                 QJsonArray({"permanent/molforma.molden", "permanent/molforma_density.cube"}));
        writeFile(plan.directory + "/output.out", "NWChem\n Total SCF energy = -75\n Total times  cpu: 1s\n");
        writeFile(plan.directory + "/permanent/molforma_density.cube", cube().toUtf8());
        QVERIFY(resultFiles(plan.directory).contains("permanent/molforma_density.cube"));
        QVERIFY_EXCEPTION_THROWN(executionPlan(d, tmp.path(), plan.directory), std::runtime_error);
        MainWindow w;
        w.loadDocument(d);
        const auto before = w.document().project();
        auto *viewer = widget<ResultsPanel>(w, "resultsTab");
        viewer->openDirectory(plan.directory);
        QTRY_VERIFY(!viewer->loading());
        auto *files = widget<QComboBox>(w, "resultsFiles");
        const int index = files->findText("permanent/molforma_density.cube");
        QVERIFY(index >= 0);
        files->setCurrentIndex(index);
        QTRY_VERIFY(!viewer->loading());
        QCOMPARE(viewer->result().format, "CUBE");
        QVERIFY(files->findText("output.out") >= 0);
        QCOMPARE(files->currentText(), "permanent/molforma_density.cube");
        QCOMPARE(w.document().project(), before);
        const auto moved = tmp.filePath("moved");
        QVERIFY(QDir().rename(plan.directory, moved));
        QVERIFY(resultFiles(moved).contains("permanent/molforma_density.cube"));
#ifndef Q_OS_WIN // There absolute paths start with a drive, and NWChem input takes no ":" in them.
        // An absolute permanent directory uses the same discovery path without recursively scanning it.
        setNwchemAdvanced(d, "permanent_dir", moved + "/permanent");
        const auto external = saveExecutionBundle(d, tmp.path());
        QVERIFY(resultFiles(external.directory).contains(moved + "/permanent/molforma_density.cube"));
#endif
        setNwchemAdvanced(d, "vectors_output", "./direct/custom.movecs");
        const auto direct = saveExecutionBundle(d, tmp.path());
        QVERIFY(QFileInfo(direct.directory + "/direct").isDir());
        QVERIFY(direct.input.contains("dplot\n  vectors \"./direct/custom.movecs\""));
    }
    void nativeControlsUndoAndPreview() {
        QTemporaryDir tmp;
        MainWindow w;
        w.loadDocument(water());
        auto *calculation = widget<CalculationPanel>(w, "calculationTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(calculation);
        w.show();
        // The files of a calculation are all that its section holds: open from the start.
        auto *cubeOutput = widget<QCheckBox>(w, "advanced_cube_output");
        auto *moldenOutput = widget<QCheckBox>(w, "advanced_molden_output");
        QVERIFY(cubeOutput->isVisibleTo(&w));
        QVERIFY(!widget<QWidget>(w, "calculationDetailRow_cube_type")->isVisibleTo(&w)); // Used with CUBE.
        auto *outputs = widget<QLabel>(w, "calculationOutputFiles");
        QCOMPARE(outputs->text(), QString("CUBE・MOLDENの出力はオフです。"));
        cubeOutput->click();
        moldenOutput->click();
        QVERIFY(widget<QWidget>(w, "calculationDetailRow_cube_type")->isVisibleTo(&w));
        QVERIFY(outputs->text().startsWith("計算後に出力するファイル:"));
        QVERIFY(outputs->text().contains(".molden"));
        auto *type = widget<QComboBox>(w, "advanced_cube_type");
        type->setCurrentIndex(type->findData("orbital"));
        auto *orb = widget<QComboBox>(w, "advanced_cube_orbital");
        orb->setCurrentIndex(orb->findData("index"));
        QVERIFY(widget<QLabel>(w, "calculationStatus")->text().contains("エラー"));
        edit(w, "cube_index", "5");
        edit(w, "cube_x", "-3 3 24");
        QCOMPARE(nwchemAdvanced(w.document())["cube_x"].toArray(), QJsonArray({-3, 3, 24}));
        QVERIFY(!widget<QLabel>(w, "calculationStatus")->text().contains("エラー"));
        const auto generated = nwchemInput(w.document());
        // The CUBE options write into the dplot block, as the chosen detail shows.
        calculation->showDetail("cube_x");
        const auto snippet = widget<QPlainTextEdit>(w, "calculationSnippet")->toPlainText();
        QVERIFY2(snippet.startsWith("dplot") && snippet.contains("-3 3 24"), qPrintable(snippet));
        auto *preview = widget<InputPreviewPanel>(w, "inputPreviewTab");
        preview->saveInput(tmp.filePath("output.nw"));
        QCOMPARE(QString::fromUtf8(readFile(tmp.filePath("output.nw"))), generated);
        const auto before = w.document().project();
        widget<QPushButton>(w, "calculationResetSection_orbitals")->click();
        QVERIFY(!nwchemInput(w.document()).contains("dplot"));
        QCOMPARE(w.history()->undoText(), QString("「軌道・出力ファイル」を既定に戻す"));
        w.history()->undo();
        QCOMPARE(w.document().project(), before);
        w.edit("candidate", [](Document &d) { d.atoms[1].position.x() += .1; }, true);
        QVERIFY(!cubeOutput->isEnabled());
        QVERIFY(!widget<QPushButton>(w, "calculationResetAll")->isEnabled());
        w.cancelCandidate();
    }
    void realEngineArtifacts() {
        const auto root = qEnvironmentVariable("MOLFORMA_OUTPUT_ARTIFACTS");
        if (root.isEmpty())
            return;
        QVERIFY(QDir().mkpath(root));
        auto runCase = [&](Document d, const QString &name) {
            base(d, "start", name);
            base(d, "memory_mb", 256);
            both(d);
            setNwchemAdvanced(d, "cube_divisions", 12);
            setExecutionSetting(d, "job_root", root);
            setExecutionSetting(d, "executable", "/usr/bin/nwchem");
            NwchemRun run;
            run.start(d, root);
            QTRY_VERIFY_WITH_TIMEOUT(!run.active(), 90000);
            QCOMPARE(run.state(), "finished");
            const auto folder = run.plan().directory;
            for (const auto &name : nwchemOutputFiles(d, validateNwchem(d))) {
                const auto path = QDir(folder).filePath(name);
                QVERIFY2(QFileInfo::exists(path), qPrintable(path));
                const auto result = readResult(path);
                QVERIFY(!result.frames.isEmpty());
                QCOMPARE(result.frames.first().molecule.atoms.size(), d.atoms.size());
                QVERIFY(resultFiles(folder).contains(name));
                if (result.format == "MOLDEN")
                    QVERIFY(!result.orbitals.isEmpty());
                if (result.format == "CUBE")
                    QVERIFY(!result.surface.isEmpty() || nwchemAdvanced(d)["cube_type"] == "spin_density");
            }
            d.save(root + '/' + name + ".molforma.json");
            writeFile(root + '/' + name + "-directory.txt", folder.toUtf8());
        };
        runCase(water(), "hf_density");
        if (QTest::currentTestFailed())
            return;
        auto dft = water();
        base(dft, "method", "dft");
        base(dft, "functional", "pbe");
        dft.setJobValue("charge", 1);
        dft.setJobValue("multiplicity", 2);
        setNwchemAdvanced(dft, "cube_type", "orbital");
        setNwchemAdvanced(dft, "cube_spin", "beta");
        setNwchemAdvanced(dft, "permanent_dir", "permanent");
        setNwchemAdvanced(dft, "vectors_output", "nested/custom orbitals.movecs");
        runCase(dft, "uks_beta");
        if (QTest::currentTestFailed())
            return;
        auto mp2 = water();
        base(mp2, "method", "mp2");
        setNwchemAdvanced(mp2, "molden_norm", "none");
        runCase(mp2, "mp2_reference");
        if (QTest::currentTestFailed())
            return;
        auto h2 = Document::fromXyz("2\nhydrogen\nH 0 0 0\nH 0 0 .78\n");
        base(h2, "task", "optimize_frequencies");
        base(h2, "numerical_frequencies", true);
        runCase(h2, "h2_frequencies");
        if (QTest::currentTestFailed())
            return;
        MainWindow w;
        both(dft);
        w.loadDocument(dft);
        w.resize(1440, 1000);
        widget<CalculationPanel>(w, "calculationTab")->showDetailGroup("visualization");
        w.show();
        QTest::qWait(100);
        QVERIFY(w.grab().save(root + "/output-settings.png"));
    }
};
int main(int argc, char **argv) {
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--result-worker") {
        QCoreApplication app(argc, argv);
        return resultWorker(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    }
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    OutputTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_output.moc"
